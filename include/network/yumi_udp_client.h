/*
 * yumi_udp_client.h - Yumi UDP client: raw channel-based transport with decoupled send/recv worker pools, reliable/unreliable modes, retransmit, and BBR→TDS CC.
 * Copyright (C) 2026 DevNullIsaac
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * yumi_udp_client.h — Yumi UDP Client: raw transport with channels
 *
 * Architecture:
 *   - Decoupled send/recv worker thread pools for pipelined crypto.
 *   - Send workers: each owns an MPSC ring, drains, encrypts, sends.
 *   - Recv workers: worker 0 uses epoll (socket+timer+ICE), extras
 *     call recvfrom directly; decrypt, ACK, reorder, deliver.
 *   - Shared state (reliable table, CC) protected by state_lock spinlock.
 *   - Per-channel recv ordering protected by channel_locks[ch] spinlocks.
 *   - Dual transmission modes per channel: Reliable and Non-Reliable.
 *   - BBR probe → TDS cruise congestion control via bbr.h.
 *   - All non-trivial state is heap-allocated.
 *   - POSIX <pthread.h> worker threads.
 *   - Lockless concurrency via atomics where possible; spinlocks for
 *     shared mutable state between send and recv paths.
 */

#ifndef YUMI_UDP_CLIENT_H
#define YUMI_UDP_CLIENT_H

#include "network/net.h"
#include "network/bbr.h"

#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>

/* Forward-declare libjuice agent (avoids pulling in juice.h everywhere) */
typedef struct juice_agent juice_agent_t;

/* ── Reliable reorder buffer slot (worker-owned, per-channel) ──────────── */

#define YUMI_REORDER_WINDOW  32
#define YUMI_REORDER_MASK    (YUMI_REORDER_WINDOW - 1)

typedef struct {
    bool     occupied;
    uint32_t seq;
    uint32_t len;
    uint8_t  data[YUMI_UDP_MAX_PAYLOAD];
} yumi_reorder_slot_t;

/* ── Retransmit min-heap (worker-owned, lazy deletion, growable) ────────── */

typedef struct {
    uint64_t expiry;
    uint32_t table_idx;
    uint32_t seq;
    uint8_t  flags;
    uint8_t  channel;
} yumi_retx_entry_t;

typedef struct {
    yumi_retx_entry_t *entries;
    int count;
    int capacity;
} yumi_retx_heap_t;

/* ── Receive callback ──────────────────────────────────────────────────── */

typedef void (*yumi_recv_cb_t)(void *user, const void *data,
                                uint32_t len, bool reliable, uint32_t seq);

/* ── ICE state callback ────────────────────────────────────────────────── */

typedef enum {
    YUMI_ICE_DISCONNECTED = 0,
    YUMI_ICE_GATHERING,
    YUMI_ICE_CONNECTING,
    YUMI_ICE_CONNECTED,
    YUMI_ICE_COMPLETED,
    YUMI_ICE_FAILED,
} yumi_ice_state_t;

typedef void (*yumi_ice_state_cb_t)(void *user, yumi_ice_state_t state);
typedef void (*yumi_ice_candidate_cb_t)(void *user, const char *sdp);
typedef void (*yumi_ice_gathering_done_cb_t)(void *user);

/* ── TURN server entry ─────────────────────────────────────────────────── */

typedef struct {
    const char *host;
    const char *username;
    const char *password;
    uint16_t    port;
} yumi_turn_server_t;

/* ── Client configuration ──────────────────────────────────────────────── */

typedef struct {
    /* Raw-mode peer address (ignored when ICE is enabled) */
    struct sockaddr_in6 peer_addr;
    uint16_t            local_port;          /* 0 = ephemeral              */
    uint64_t            reliable_timeout_us; /* 0 = default (200 ms)       */
    uint32_t            max_retransmits;     /* 0 = default (5)            */
    yumi_recv_cb_t      recv_cb;
    void               *recv_user;

    /* ── ICE / STUN / TURN (optional — NULL stun_server = raw mode) ──── */
    const char                  *stun_server;      /* STUN host, e.g. "stun.l.google.com" */
    uint16_t                     stun_port;         /* 0 = default (3478)   */
    yumi_turn_server_t          *turn_servers;      /* array, may be NULL   */
    int                          turn_servers_count;
    yumi_ice_state_cb_t          ice_state_cb;
    yumi_ice_candidate_cb_t      ice_candidate_cb;
    yumi_ice_gathering_done_cb_t ice_gathering_done_cb;
    void                        *ice_user;

    /* ── Datagram-level encryption (optional) ────────────────────────── */
    /**
     * When set, ALL outgoing datagrams (transport header + payload) are
     * encrypted before sendto() and ALL incoming datagrams are decrypted
     * before header parsing.  Flags, seq, channel, payload_len are NEVER
     * exposed on the wire.
     *
     * pkt_encrypt(ctx, plaintext, pt_len, ciphertext_out, &ct_len):
     *   plaintext = [hdr(10)][payload].  ct_out has room for
     *   pt_len + pkt_crypto_overhead.
     * pkt_decrypt(ctx, ciphertext, ct_len, plaintext_out, &pt_len):
     *   Returns 0 on success (tag verified), -1 on failure (drop).
     * pkt_crypto_overhead: bytes added per packet (nonce + tag).
     *   Deducted from max_payload so encrypted datagrams fit in MTU.
     */
    int (*pkt_encrypt)(void *ctx, const uint8_t *pt, uint32_t pt_len,
                       uint8_t *ct, uint32_t *ct_len);
    int (*pkt_decrypt)(void *ctx, const uint8_t *ct, uint32_t ct_len,
                       uint8_t *pt, uint32_t *pt_len);
    void    *pkt_crypto_ctx;
    uint32_t pkt_crypto_overhead;

    /* Worker-thread callback — invoked for YUMI_WORK_USER_CB items */
    void    (*user_work_cb)(void *ctx, const void *data, uint32_t len);
    void    *user_work_ctx;

    /* ── Worker thread pool (optional — defaults: 1 send, 1 recv) ──── */
    uint32_t num_send_workers;     /* 0 = 1 (default)                  */
    uint32_t num_recv_workers;     /* 0 = 1 (default)                  */
} yumi_udp_client_config_t;

/* ── Reliable retransmit entry (worker-owned, no sharing) ──────────────── */

typedef struct {
    uint32_t seq;
    uint64_t send_time_us;
    uint64_t timeout_us;
    uint32_t retransmit_count;
    uint32_t max_retransmits;
    uint32_t payload_len;
    uint8_t  flags;
    uint8_t  channel;
    bool     active;
    yumi_packet_t cc_pkt;                    /* BBR delivery state snapshot */
    struct sockaddr_in6 dest;                /* target address for retransmit */
    uint8_t  payload[YUMI_UDP_MAX_PAYLOAD];
} yumi_reliable_entry_t;

#define YUMI_RELIABLE_INIT_SLOTS 4096

/* ── Worker context types (defined in yumi_udp_client.c) ───────────────── */
struct yumi_send_worker;
struct yumi_recv_worker;

/* ── Client structure (heap-allocate via calloc) ───────────────────────── */

typedef struct {
    /* File descriptors */
    int fd;
    int epoll_fd;
    int timer_fd;
    int recv_event_fd;           /* eventfd to wake recv worker 0       */

    /* Peer address */
    struct sockaddr_in6 peer_addr;

    /* ── Worker pools (send/recv thread separation) ────────────────── */
    uint32_t              num_send_workers;
    uint32_t              num_recv_workers;
    struct yumi_send_worker *send_wk;  /* heap [num_send_workers]       */
    struct yumi_recv_worker *recv_wk;  /* heap [num_recv_workers]       */

    /* Shared state protection — spinlocks.
     * state_lock protects: reliable_table, retx_heap, cc.
     * channel_locks[ch] protects: recv_*_next, recv_*_started,
     * reorder_buf for channel ch (multi-recv-worker ordering). */
    _Alignas(64) atomic_int  state_lock;
    _Alignas(64) atomic_int *channel_locks; /* [YUMI_UDP_MAX_CHANNELS] */

    /* Cached CC readings — updated under state_lock, read locklessly.
     * Avoids acquiring state_lock on every send-worker pacing check. */
    _Alignas(64) atomic_int            cc_mode_cache;  /* yumi_mode_t   */
    _Alignas(64) atomic_uint_fast64_t  cc_rate_cache;  /* send rate B/s  */

    /* Reliable retransmit table (state_lock) */
    yumi_reliable_entry_t *reliable_table;
    yumi_retx_heap_t      *retx_heap;
    uint32_t               reliable_capacity;
    uint64_t               reliable_timeout_us;
    uint32_t               max_retransmits;

    /* MTU — discovered from NIC at create time */
    uint32_t link_mtu;               /* raw link MTU (e.g. 1500)       */
    uint32_t max_payload;            /* max Yumi payload for this path */

    /* Per-channel, per-mode sequence counters (atomics, no lock) */
    atomic_uint *rel_seq;            /* [YUMI_UDP_MAX_CHANNELS]         */
    atomic_uint *unrel_seq;          /* [YUMI_UDP_MAX_CHANNELS]         */
    atomic_uint probe_seq;           /* CC probe seq                    */
    atomic_int  probe_inflight;      /* O(1) probe count for CC         */

    /* Receive-side ordering — per-channel (channel_locks) */
    uint32_t *recv_rel_next;         /* reliable: next expected seq     */
    bool     *recv_rel_started;
    uint32_t *recv_unrel_next;       /* unreliable: dedup high-water    */
    bool     *recv_unrel_started;

    /* Reliable reorder buffer (channel_locks) */
    yumi_reorder_slot_t *reorder_buf; /* [MAX_CHANNELS * REORDER_WINDOW] */

    /* Receive callback */
    yumi_recv_cb_t recv_cb;
    void          *recv_user;

    /* Congestion control — BBR probe → TDS cruise (state_lock) */
    yumi_cc_t cc;

    /* Stats (lockless atomics, no mutex) */
    _Alignas(64) atomic_uint_fast64_t stat_tx_packets;
    _Alignas(64) atomic_uint_fast64_t stat_tx_bytes;
    _Alignas(64) atomic_uint_fast64_t stat_rx_packets;
    _Alignas(64) atomic_uint_fast64_t stat_rx_bytes;
    _Alignas(64) atomic_uint_fast64_t stat_retransmits;
#ifdef YUMI_DEBUG
    _Alignas(64) atomic_uint_fast64_t stat_reliable_full;
#endif

    /* Shutdown flag */
    atomic_bool running;

    /* ── ICE transport (optional — NULL when raw mode) ─────────────── */
    juice_agent_t              *ice_agent;
    int                         ice_recv_fd;   /* pipe write-end for cb_recv  */
    int                         ice_pipe_rd;   /* pipe read-end in epoll      */
    uint8_t                     ice_pipe_hdr[4];
    uint8_t                     ice_pipe_hdr_pos;
    uint32_t                    ice_pipe_msg_pos;
    atomic_int                  ice_state;
    yumi_ice_state_cb_t         ice_state_cb;
    yumi_ice_candidate_cb_t     ice_candidate_cb;
    yumi_ice_gathering_done_cb_t ice_gathering_done_cb;
    void                       *ice_user;

    /* Datagram-level crypto hooks (socket-boundary encrypt/decrypt) */
    int (*pkt_encrypt)(void *ctx, const uint8_t *pt, uint32_t pt_len,
                       uint8_t *ct, uint32_t *ct_len);
    int (*pkt_decrypt)(void *ctx, const uint8_t *ct, uint32_t ct_len,
                       uint8_t *pt, uint32_t *pt_len);
    void    *pkt_crypto_ctx;
    uint32_t pkt_crypto_overhead;

    /* Worker-thread user callback (dispatched from send worker ring) */
    void    (*user_work_cb)(void *ctx, const void *data, uint32_t len);
    void    *user_work_ctx;
} yumi_udp_client_t;

/* ── API ───────────────────────────────────────────────────────────────── */

int  yumi_udp_client_create(yumi_udp_client_t *client,
                             const yumi_udp_client_config_t *cfg);
void yumi_udp_client_destroy(yumi_udp_client_t *client);

/* Send on default channel (0) */
int  yumi_udp_client_send(yumi_udp_client_t *client,
                           const void *data, uint32_t len);
int  yumi_udp_client_send_reliable(yumi_udp_client_t *client,
                                    const void *data, uint32_t len);

/* Send on a specific channel (0-255) */
int  yumi_udp_client_send_channel(yumi_udp_client_t *client,
                                   uint8_t channel,
                                   const void *data, uint32_t len);
int  yumi_udp_client_send_reliable_channel(yumi_udp_client_t *client,
                                            uint8_t channel,
                                            const void *data, uint32_t len);

/* Send to a specific destination address (multi-peer) */
int  yumi_udp_client_send_to(yumi_udp_client_t *client,
                              const struct sockaddr_in6 *dest,
                              const void *data, uint32_t len);
int  yumi_udp_client_send_reliable_to(yumi_udp_client_t *client,
                                       const struct sockaddr_in6 *dest,
                                       const void *data, uint32_t len);
int  yumi_udp_client_send_channel_to(yumi_udp_client_t *client,
                                      uint8_t channel,
                                      const struct sockaddr_in6 *dest,
                                      const void *data, uint32_t len);
int  yumi_udp_client_send_reliable_channel_to(yumi_udp_client_t *client,
                                               uint8_t channel,
                                               const struct sockaddr_in6 *dest,
                                               const void *data, uint32_t len);

/* MTU queries */
uint32_t yumi_udp_client_get_mtu(const yumi_udp_client_t *client);
uint32_t yumi_udp_client_get_max_payload(const yumi_udp_client_t *client);

/* Post a user-callback work item to the worker thread's ring.
 * The callback set in config.user_work_cb will be invoked on the
 * worker thread with (user_work_ctx, data, len). */
int  yumi_udp_client_post_user(yumi_udp_client_t *client,
                               const void *data, uint32_t len);

/* Stats (lockless reads) */
uint64_t yumi_udp_client_stat_tx_packets(const yumi_udp_client_t *client);
uint64_t yumi_udp_client_stat_tx_bytes(const yumi_udp_client_t *client);
uint64_t yumi_udp_client_stat_rx_packets(const yumi_udp_client_t *client);
uint64_t yumi_udp_client_stat_rx_bytes(const yumi_udp_client_t *client);

/* ── ICE API (only valid when stun_server was set in config) ───────── */

/* Get the local SDP description (call after create, before setting remote) */
int  yumi_udp_client_ice_get_local_sdp(yumi_udp_client_t *client,
                                        char *buf, size_t size);

/* Set the remote peer's SDP description (triggers connectivity checks) */
int  yumi_udp_client_ice_set_remote_sdp(yumi_udp_client_t *client,
                                         const char *sdp);

/* Add a remote ICE candidate (trickle ICE) */
int  yumi_udp_client_ice_add_remote_candidate(yumi_udp_client_t *client,
                                               const char *sdp);

/* Signal that remote gathering is complete */
int  yumi_udp_client_ice_set_remote_gathering_done(yumi_udp_client_t *client);

/* Query current ICE state */
yumi_ice_state_t yumi_udp_client_ice_get_state(const yumi_udp_client_t *client);

/* Query whether ICE mode is active */
bool yumi_udp_client_ice_enabled(const yumi_udp_client_t *client);

#endif /* YUMI_UDP_CLIENT_H */
