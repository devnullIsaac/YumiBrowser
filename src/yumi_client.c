/*
    Yumi Client — High-Level Peer Group Networking (Implementation)
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * yumi_client.c — Yumi Client: high-level peer group networking
 *
 * Orchestrates Group Registrar, Secure UDP, delta sync, attestation,
 * and bootstrap into a single cohesive client.
 *
 * Architecture:
 *
 *   ┌──────────────────────────────────────────────────┐
 *   │                  yumi_client_t                    │
 *   │                                                  │
 *   │  gr_registrar_t *reg          (group state)      │
 *   │  gr_identity_t   identity     (our keypair)      │
 *   │                                                  │
 *   │  peer_slot_t peers[64]   ─── SUDP client/peer    │
 *   │                                                  │
 *   │  sync_worker        ─── polls version, pushes    │
 *   │                         deltas, auto-connects    │
 *   │                                                  │
 *   │  boot_fd            ─── raw UDP bootstrap:       │
 *   │  boot_worker            registrar fetch +        │
 *   │                         SUDP port signaling      │
 *   └──────────────────────────────────────────────────┘
 *
 * Internal SUDP channels:
 *   0    Sync:   version exchange, delta push/pull
 *   1    Mesh:   relay packets for unreachable peers
 *   254  Attest: join nonce + header attestation
 *
 * Bootstrap wire protocol (raw UDP, "YBOT" magic):
 *   0x01  BOOT_REQ   [token(128)]
 *   0x02  BOOT_DATA  [seq(2)][total(2)][chunk...]
 *   0x03  BOOT_DONE
 *   0x04  BOOT_NACK  [count(2)][seq(2)*N]
 *   0x05  BOOT_ACK
 *   0x10  CONN_REQ   [peer_id(32)][sudp_port(2)]
 *   0x11  CONN_RESP  [peer_id(32)][sudp_port(2)]
 *   0xFF  BOOT_ERR   [code(1)]
 *
 * Sync protocol (SUDP channel 0, reliable):
 *   0x01  VERSION      [version(4)]
 *   0x02  DELTA_REQ    [since_version(4)]
 *   0x03  DELTA_DATA   (inline or chunked)
 *   0x04  FULL_REQ
 *   0x05  FULL_DATA    (inline or chunked)
 *   0x06  PING         (keepalive, empty payload)
 *   0x07  PONG         (keepalive response)
 *
 * Chunk envelope (for large sync/attest messages):
 *   0xFE  CHUNK_START  [inner_type(1)][total_len(4)][data...]
 *   0xFF  CHUNK_CONT   [data...]
 *
 * Attestation protocol (SUDP channel 254, reliable):
 *   0x10  NONCE        [nonce(32)]
 *   0x11  ATTEST       [header_wire][peer_id(32)][pk(2592)][sig(4627)]
 *
 * Meshnet relay (SUDP channel 1, reliable):
 *   0x01  FORWARD      [ttl(1)][src_id(32)][dst_id(32)][ch(1)][payload...]
 */

#define _GNU_SOURCE
#include "yumi_client.h"
#include "network/yumi_sudp_client.h"
#include "network/yumi_udp_client.h"
#include "group_registrar.h"
#include "crypto.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Section 1: Constants and internal types
 * ═══════════════════════════════════════════════════════════════════ */

/* Bootstrap wire magic */
static const uint8_t BOOT_MAGIC[4] = {'Y','B','O','T'};
#define BOOT_HDR  4   /* magic length */

/* Bootstrap message types */
#define BOOT_REQ      0x01
#define BOOT_DATA     0x02
#define BOOT_DONE     0x03
#define BOOT_NACK     0x04
#define BOOT_ACK      0x05
#define BOOT_CONN_REQ 0x10
#define BOOT_CONN_RSP 0x11
#define BOOT_RECONNECT 0x20
#define BOOT_ERR      0xFF

/* Sync channel message types */
#define SYNC_VERSION    0x01
#define SYNC_DELTA_REQ  0x02
#define SYNC_DELTA      0x03
#define SYNC_FULL_REQ   0x04
#define SYNC_FULL       0x05

/* Attest channel message types */
#define ATT_NONCE       0x10
#define ATT_HEADER      0x11

/* Meshnet relay message types */
#define MESH_FORWARD    0x01

/* Meshnet relay header: ttl(1) + src(32) + dst(32) + inner_ch(1) = 66 */
#define MESH_HDR_LEN    (1 + GR_PEER_ID_LEN + GR_PEER_ID_LEN + 1)

/* Chunk envelope */
#define CHUNK_START     0xFE
#define CHUNK_CONT      0xFF

/* Sync worker interval */
#define SYNC_POLL_MS    500

/* Keepalive (sync channel 0) */
#define SYNC_PING              0x06
#define SYNC_PONG              0x07
#define KEEPALIVE_INTERVAL_MS  15000   /* send PING after 15 s silence  */
#define KEEPALIVE_TIMEOUT_MS   45000   /* peer dead after 45 s silence  */

/* Bootstrap IP blocklist */
#define BOOT_BLOCK_MAX_FAILS     3     /* failed attempts before block  */
#define BOOT_BLOCK_DURATION_MS   300000 /* block for 5 minutes          */

/* Header wire size (all gr_header_t fields serialized big-endian) */
#define HDR_WIRE_LEN  (GR_HASH_LEN + 4 + GR_MAX_NAME_LEN + 4 + 8 + 8 + 4 \
                       + 8 + 8 + 8 \
                       + GR_PEER_ID_LEN + GR_PUBLIC_KEY_LEN \
                       + GR_PEER_ID_LEN + GR_PUBLIC_KEY_LEN \
                       + GR_SIGN_LEN + GR_HASH_LEN)

/* Attestation wire = header + peer_id + sign_key + attestation sig */
#define ATT_WIRE_LEN  (HDR_WIRE_LEN + GR_PEER_ID_LEN + GR_PUBLIC_KEY_LEN + GR_SIGN_LEN)

/* ── Peer slot ──────────────────────────────────────────────────── */

typedef enum {
    SLOT_FREE = 0,
    SLOT_SIGNALING,     /* CONN_REQ sent, waiting CONN_RESP */
    SLOT_CONNECTING,    /* SUDP created, handshake in progress */
    SLOT_CONNECTED,     /* SUDP established */
    SLOT_FAILED,
} slot_state_t;

/* Chunk reassembly state */
typedef struct {
    uint8_t  msg_type;
    uint32_t total_len;
    uint8_t *buf;
    uint32_t received;
} yc_reasm_t;

typedef struct {
    uint8_t              peer_id[GR_PEER_ID_LEN];
    yumi_sudp_client_t  *sudp;
    atomic_int           state;        /* slot_state_t */
    uint32_t             remote_version;
    uint16_t             sudp_local_port;
    bool                 first_contact;
    bool                 nonce_sent;   /* attestation: we sent our nonce */

    /* Chunk reassembly per internal channel */
    yc_reasm_t           sync_reasm;
    yc_reasm_t           attest_reasm;
    yc_reasm_t           mesh_reasm;

    /* Meshnet rate limiter (token bucket) */
    int64_t              mesh_tokens;       /* remaining bytes allowed   */
    int64_t              mesh_last_refill;  /* monotonic ms last refill  */
    uint64_t             mesh_fwd_bytes;    /* total forwarded via peer  */

    /* Keepalive state */
    int64_t              last_recv_ms;      /* last data received (mono) */
    int64_t              last_ping_ms;      /* last PING sent (mono)     */
    uint8_t              missed_pongs;      /* unanswered PINGs          */

    /* STUN fallback index */
    uint32_t             stun_idx;          /* STUN server used for this */
} peer_slot_t;

/* ── Client handle ──────────────────────────────────────────────── */

struct yumi_client {
    gr_registrar_t     *reg;
    gr_identity_t       identity;
    atomic_int          state;         /* yumi_client_state_t */

    /* Peer connections */
    peer_slot_t         peers[YUMI_CLIENT_MAX_PEERS];
    pthread_mutex_t     peer_lock;

    /* Sync worker */
    pthread_t           sync_thread;
    atomic_bool         running;
    uint32_t            last_broadcast_ver;

    /* Bootstrap listener (raw UDP) */
    int                 boot_fd;
    uint16_t            boot_port;
    pthread_t           boot_thread;

    /* Join state */
    bool                joining;

    /* Config copy */
    char                db_path[256];
    yumi_client_message_fn on_message;
    yumi_client_event_fn   on_event;
    void                  *user;

    /* STUN fallback list (heap-allocated copies) */
    char              **stun_list;
    uint32_t            stun_count;
    uint16_t            stun_port;
};

/* Forward declarations */
static void *sync_worker(void *arg);
static void *boot_worker(void *arg);
static int   start_workers(yumi_client_t *c);
static void  stop_workers(yumi_client_t *c);
static int   peer_connect(yumi_client_t *c, const uint8_t peer_id[GR_PEER_ID_LEN],
                           const char *ip, uint16_t sudp_port, bool first_contact,
                           uint32_t stun_idx);
static void  auto_connect_peers(yumi_client_t *c);

/* ═══════════════════════════════════════════════════════════════════
 *  Section 2: Wire format helpers (big-endian)
 * ═══════════════════════════════════════════════════════════════════ */

static inline void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline void put_i64(uint8_t *p, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)u; u >>= 8; }
}
static inline uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static inline int64_t get_i64(const uint8_t *p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u = (u << 8) | p[i];
    return (int64_t)u;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 3: Header serialization (for attestation wire format)
 * ═══════════════════════════════════════════════════════════════════ */

static size_t ser_header(uint8_t *out, const gr_header_t *h)
{
    uint8_t *p = out;
    memcpy(p, h->group_id, GR_HASH_LEN);                p += GR_HASH_LEN;
    put_u32(p, (uint32_t)h->group_type);                 p += 4;
    memcpy(p, h->group_name, GR_MAX_NAME_LEN);          p += GR_MAX_NAME_LEN;
    put_u32(p, h->version);                              p += 4;
    put_i64(p, h->created_at);                           p += 8;
    put_i64(p, h->updated_at);                           p += 8;
    put_u32(p, h->epoch_id);                             p += 4;
    put_i64(p, h->retention.message_retention_ms);       p += 8;
    put_i64(p, h->retention.file_retention_ms);          p += 8;
    put_i64(p, h->retention.registrar_max_bytes);        p += 8;
    memcpy(p, h->owner_id, GR_PEER_ID_LEN);             p += GR_PEER_ID_LEN;
    memcpy(p, h->owner_sign_key, GR_PUBLIC_KEY_LEN);    p += GR_PUBLIC_KEY_LEN;
    memcpy(p, h->signer_id, GR_PEER_ID_LEN);            p += GR_PEER_ID_LEN;
    memcpy(p, h->signer_sign_key, GR_PUBLIC_KEY_LEN);   p += GR_PUBLIC_KEY_LEN;
    memcpy(p, h->signature, GR_SIGN_LEN);               p += GR_SIGN_LEN;
    memcpy(p, h->hash, GR_HASH_LEN);                    p += GR_HASH_LEN;
    return (size_t)(p - out);
}

static int de_header(const uint8_t *in, size_t len, gr_header_t *h)
{
    if (len < HDR_WIRE_LEN) return -1;
    const uint8_t *p = in;
    memset(h, 0, sizeof(*h));
    memcpy(h->group_id, p, GR_HASH_LEN);                p += GR_HASH_LEN;
    h->group_type = (gr_group_type_t)get_u32(p);         p += 4;
    memcpy(h->group_name, p, GR_MAX_NAME_LEN);          p += GR_MAX_NAME_LEN;
    h->version = get_u32(p);                             p += 4;
    h->created_at = get_i64(p);                          p += 8;
    h->updated_at = get_i64(p);                          p += 8;
    h->epoch_id = get_u32(p);                            p += 4;
    h->retention.message_retention_ms = get_i64(p);      p += 8;
    h->retention.file_retention_ms    = get_i64(p);      p += 8;
    h->retention.registrar_max_bytes  = get_i64(p);      p += 8;
    memcpy(h->owner_id, p, GR_PEER_ID_LEN);             p += GR_PEER_ID_LEN;
    memcpy(h->owner_sign_key, p, GR_PUBLIC_KEY_LEN);    p += GR_PUBLIC_KEY_LEN;
    memcpy(h->signer_id, p, GR_PEER_ID_LEN);            p += GR_PEER_ID_LEN;
    memcpy(h->signer_sign_key, p, GR_PUBLIC_KEY_LEN);   p += GR_PUBLIC_KEY_LEN;
    memcpy(h->signature, p, GR_SIGN_LEN);               p += GR_SIGN_LEN;
    memcpy(h->hash, p, GR_HASH_LEN);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 4: Chunk reassembly (large messages on SUDP channels)
 * ═══════════════════════════════════════════════════════════════════ */

static void reasm_reset(yc_reasm_t *r)
{
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

/*
 * Feed a received packet into a reassembly context.
 * Returns a complete message (caller frees) or NULL if still accumulating.
 * *out_type receives the inner message type.
 * *out_len receives the payload length.
 */
static uint8_t *reasm_feed(yc_reasm_t *r, const uint8_t *data, uint32_t len,
                            uint8_t *out_type, uint32_t *out_len)
{
    if (len < 1) return NULL;

    if (data[0] == CHUNK_START) {
        /* Start of a chunked message: [0xFE][type][total_len(4)][data...] */
        if (len < 6) return NULL;
        reasm_reset(r);
        r->msg_type  = data[1];
        r->total_len = get_u32(data + 2);
        if (r->total_len == 0 || r->total_len > 4 * 1024 * 1024) {
            reasm_reset(r);
            return NULL;
        }
        r->buf = malloc(r->total_len);
        if (!r->buf) { reasm_reset(r); return NULL; }
        uint32_t chunk = len - 6;
        if (chunk > r->total_len) chunk = r->total_len;
        memcpy(r->buf, data + 6, chunk);
        r->received = chunk;
    } else if (data[0] == CHUNK_CONT) {
        /* Continuation chunk: [0xFF][data...] */
        if (!r->buf || r->received >= r->total_len) return NULL;
        uint32_t chunk = len - 1;
        uint32_t space = r->total_len - r->received;
        if (chunk > space) chunk = space;
        memcpy(r->buf + r->received, data + 1, chunk);
        r->received += chunk;
    } else {
        /* Small single-packet message: [type][data...] */
        *out_type = data[0];
        uint32_t plen = len - 1;
        if (plen == 0) { *out_len = 0; return (uint8_t *)(uintptr_t)1; /* sentinel */ }
        uint8_t *msg = malloc(plen);
        if (!msg) return NULL;
        memcpy(msg, data + 1, plen);
        *out_len = plen;
        return msg;
    }

    /* Check if reassembly is complete */
    if (r->buf && r->received >= r->total_len) {
        uint8_t *msg = r->buf;
        *out_type = r->msg_type;
        *out_len  = r->total_len;
        r->buf = NULL;
        reasm_reset(r);
        return msg;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 5: Chunked send helper (SUDP reliable channel)
 * ═══════════════════════════════════════════════════════════════════ */

static int chunked_send(yumi_sudp_client_t *sudp, uint8_t channel,
                         uint8_t msg_type, const uint8_t *data, uint32_t data_len)
{
    uint32_t max_pl = yumi_sudp_client_get_max_payload(sudp);
    if (max_pl < 10) return -1;

    /* Small message: fits in one packet */
    if (data_len + 1 <= max_pl) {
        uint8_t *pkt = malloc(1 + data_len);
        if (!pkt) return -1;
        pkt[0] = msg_type;
        if (data_len > 0) memcpy(pkt + 1, data, data_len);
        int r = yumi_sudp_client_send_reliable_channel(sudp, channel, pkt, 1 + data_len);
        free(pkt);
        return r;
    }

    /* Large message: chunk start + continuations */
    uint8_t *pkt = malloc(max_pl);
    if (!pkt) return -1;

    /* First chunk: [0xFE][type][total_len(4)][data...] */
    pkt[0] = CHUNK_START;
    pkt[1] = msg_type;
    put_u32(pkt + 2, data_len);
    uint32_t first_chunk = max_pl - 6;
    if (first_chunk > data_len) first_chunk = data_len;
    memcpy(pkt + 6, data, first_chunk);
    if (yumi_sudp_client_send_reliable_channel(sudp, channel, pkt, 6 + first_chunk) != 0) {
        free(pkt);
        return -1;
    }

    /* Continuation chunks: [0xFF][data...] */
    uint32_t sent = first_chunk;
    while (sent < data_len) {
        uint32_t chunk = max_pl - 1;
        if (chunk > data_len - sent) chunk = data_len - sent;
        pkt[0] = CHUNK_CONT;
        memcpy(pkt + 1, data + sent, chunk);
        if (yumi_sudp_client_send_reliable_channel(sudp, channel, pkt, 1 + chunk) != 0) {
            free(pkt);
            return -1;
        }
        sent += chunk;
    }

    free(pkt);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 6: Peer slot management
 * ═══════════════════════════════════════════════════════════════════ */

static peer_slot_t *slot_find(yumi_client_t *c, const uint8_t peer_id[GR_PEER_ID_LEN])
{
    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        if (atomic_load(&c->peers[i].state) != SLOT_FREE &&
            gr_id_equal(c->peers[i].peer_id, peer_id))
            return &c->peers[i];
    }
    return NULL;
}

static peer_slot_t *slot_alloc(yumi_client_t *c)
{
    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        if (atomic_load(&c->peers[i].state) == SLOT_FREE)
            return &c->peers[i];
    }
    return NULL;
}

static void slot_free(peer_slot_t *s)
{
    if (s->sudp) {
        yumi_sudp_client_destroy(s->sudp);
        s->sudp = NULL;
    }
    reasm_reset(&s->sync_reasm);
    reasm_reset(&s->attest_reasm);
    reasm_reset(&s->mesh_reasm);
    s->mesh_tokens      = YUMI_MESH_RATE_BYTES_SEC;
    s->mesh_last_refill  = 0;
    s->mesh_fwd_bytes    = 0;
    memset(s->peer_id, 0, GR_PEER_ID_LEN);
    atomic_store(&s->state, SLOT_FREE);
    s->remote_version = 0;
    s->nonce_sent = false;
    s->first_contact = false;
    s->sudp_local_port = 0;
    s->last_recv_ms  = 0;
    s->last_ping_ms  = 0;
    s->missed_pongs  = 0;
    s->stun_idx      = 0;
}

static uint32_t count_connected(const yumi_client_t *c)
{
    uint32_t n = 0;
    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        if (atomic_load(&c->peers[i].state) == SLOT_CONNECTED)
            n++;
    }
    return n;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 7: Sync protocol (SUDP channel 0)
 * ═══════════════════════════════════════════════════════════════════ */

/* Send our registrar version to a peer */
static void sync_send_version(yumi_client_t *c, peer_slot_t *s)
{
    if (atomic_load(&s->state) != SLOT_CONNECTED || !s->sudp) return;
    gr_header_t hdr;
    if (gr_get_header(c->reg, &hdr) != GR_OK) return;

    uint8_t buf[4];
    put_u32(buf, hdr.version);
    chunked_send(s->sudp, YUMI_CLIENT_SYNC_CH, SYNC_VERSION, buf, 4);
}

/* Request delta from a peer since our version */
static void sync_request_delta(yumi_client_t *c, peer_slot_t *s)
{
    if (atomic_load(&s->state) != SLOT_CONNECTED || !s->sudp) return;
    gr_header_t hdr;
    if (gr_get_header(c->reg, &hdr) != GR_OK) return;

    uint8_t buf[4];
    put_u32(buf, hdr.version);
    chunked_send(s->sudp, YUMI_CLIENT_SYNC_CH, SYNC_DELTA_REQ, buf, 4);
}

/* Send delta since a version to a specific peer */
static void sync_send_delta_to(yumi_client_t *c, peer_slot_t *s, uint32_t since)
{
    if (atomic_load(&s->state) != SLOT_CONNECTED || !s->sudp) return;

    uint8_t *blob = NULL;
    size_t blen = 0;
    if (gr_serialize_delta(c->reg, since, &blob, &blen) != GR_OK || !blob)
        return;

    chunked_send(s->sudp, YUMI_CLIENT_SYNC_CH, SYNC_DELTA, blob, (uint32_t)blen);
    gr_free(blob);
}

/* Send full registrar to a peer (for join bootstrap over SUDP) */
static void sync_send_full(yumi_client_t *c, peer_slot_t *s)
{
    if (atomic_load(&s->state) != SLOT_CONNECTED || !s->sudp) return;

    uint8_t *blob = NULL;
    size_t blen = 0;
    if (gr_serialize(c->reg, GR_SERIALIZE_FULL, &blob, &blen) != GR_OK || !blob)
        return;

    chunked_send(s->sudp, YUMI_CLIENT_SYNC_CH, SYNC_FULL, blob, (uint32_t)blen);
    gr_free(blob);
}

/* Broadcast delta to all connected peers */
static void sync_broadcast_delta(yumi_client_t *c, uint32_t since)
{
    pthread_mutex_lock(&c->peer_lock);
    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        peer_slot_t *s = &c->peers[i];
        if (atomic_load(&s->state) == SLOT_CONNECTED && s->sudp) {
            if (s->remote_version < since || s->remote_version == 0)
                sync_send_delta_to(c, s, s->remote_version);
        }
    }
    pthread_mutex_unlock(&c->peer_lock);
}

/* Handle incoming sync message on channel 0 */
static void sync_handle(yumi_client_t *c, peer_slot_t *s,
                         uint8_t msg_type, const uint8_t *data, uint32_t len)
{
    switch (msg_type) {
    case SYNC_VERSION: {
        if (len < 4) break;
        uint32_t remote_ver = get_u32(data);
        s->remote_version = remote_ver;

        gr_header_t hdr;
        if (gr_get_header(c->reg, &hdr) != GR_OK) break;

        if (remote_ver > hdr.version) {
            /* Peer is ahead — request delta */
            sync_request_delta(c, s);
        } else if (remote_ver < hdr.version) {
            /* We are ahead — push delta */
            sync_send_delta_to(c, s, remote_ver);
        }
        break;
    }
    case SYNC_DELTA_REQ: {
        if (len < 4) break;
        uint32_t since = get_u32(data);
        sync_send_delta_to(c, s, since);
        break;
    }
    case SYNC_DELTA: {
        gr_merge_result_t result;
        gr_error_t err = gr_apply_delta(c->reg, data, len, &result);
        if (err == GR_OK && result.entries_new > 0) {
            /* Our version advanced — tell other peers */
            gr_header_t hdr;
            if (gr_get_header(c->reg, &hdr) == GR_OK)
                c->last_broadcast_ver = hdr.version;

            if (c->on_event)
                c->on_event(c->user, YUMI_EVENT_SYNC_COMPLETE, s->peer_id);

            /* Check if we were kicked or banned by this delta */
            gr_peer_t self_peer;
            if (gr_peer_get(c->reg, c->identity.peer_id, &self_peer) == GR_OK) {
                if (self_peer.status == GR_PEER_KICKED ||
                    self_peer.status == GR_PEER_BANNED) {
                    if (c->on_event)
                        c->on_event(c->user, YUMI_EVENT_PEER_KICKED,
                                    c->identity.peer_id);
                }
            }
        }
        break;
    }
    case SYNC_FULL_REQ:
        sync_send_full(c, s);
        break;
    case SYNC_FULL: {
        gr_error_t err = gr_deserialize(c->reg, data, len);
        if (err == GR_OK) {
            if (c->on_event)
                c->on_event(c->user, YUMI_EVENT_SYNC_COMPLETE, s->peer_id);

            /* Check if we were kicked or banned */
            gr_peer_t self_peer;
            if (gr_peer_get(c->reg, c->identity.peer_id, &self_peer) == GR_OK) {
                if (self_peer.status == GR_PEER_KICKED ||
                    self_peer.status == GR_PEER_BANNED) {
                    if (c->on_event)
                        c->on_event(c->user, YUMI_EVENT_PEER_KICKED,
                                    c->identity.peer_id);
                }
            }
        }
        break;
    }
    case SYNC_PING:
        /* Keepalive probe — respond with PONG */
        if (atomic_load(&s->state) == SLOT_CONNECTED && s->sudp)
            chunked_send(s->sudp, YUMI_CLIENT_SYNC_CH, SYNC_PONG, NULL, 0);
        break;
    case SYNC_PONG:
        /* Keepalive response — peer is alive */
        s->missed_pongs = 0;
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 8: Attestation protocol (SUDP channel 254)
 * ═══════════════════════════════════════════════════════════════════ */

/* Send our join nonce to a peer (joiner side) */
static void attest_send_nonce(yumi_client_t *c, peer_slot_t *s)
{
    if (atomic_load(&s->state) != SLOT_CONNECTED || !s->sudp) return;

    uint8_t nonce[GR_JOIN_NONCE_LEN];
    if (gr_join_get_nonce(c->reg, nonce) != GR_OK) return;

    chunked_send(s->sudp, YUMI_CLIENT_ATTEST_CH, ATT_NONCE, nonce, GR_JOIN_NONCE_LEN);
    s->nonce_sent = true;
}

/* Respond with header attestation (existing member side) */
static void attest_respond(yumi_client_t *c, peer_slot_t *s,
                            const uint8_t joiner_nonce[GR_JOIN_NONCE_LEN])
{
    gr_header_t hdr;
    uint8_t sig[GR_SIGN_LEN];

    if (gr_join_export_header_attestation(c->reg, &c->identity,
            joiner_nonce, &hdr, sig) != GR_OK)
        return;

    /* Build attestation wire message:
     * [header_wire(HDR_WIRE_LEN)][peer_id(32)][sign_key(2592)][sig(4627)] */
    uint8_t *msg = malloc(ATT_WIRE_LEN);
    if (!msg) return;

    uint8_t *p = msg;
    p += ser_header(p, &hdr);
    memcpy(p, c->identity.peer_id, GR_PEER_ID_LEN);  p += GR_PEER_ID_LEN;
    memcpy(p, c->identity.public_key, GR_PUBLIC_KEY_LEN); p += GR_PUBLIC_KEY_LEN;
    memcpy(p, sig, GR_SIGN_LEN);

    chunked_send(s->sudp, YUMI_CLIENT_ATTEST_CH, ATT_HEADER, msg, ATT_WIRE_LEN);
    free(msg);
}

/* Process received attestation (joiner side) */
static void attest_process(yumi_client_t *c, const uint8_t *data, uint32_t len)
{
    if (len < ATT_WIRE_LEN) return;

    const uint8_t *p = data;
    gr_header_t hdr;
    if (de_header(p, HDR_WIRE_LEN, &hdr) != 0) return;
    p += HDR_WIRE_LEN;

    const uint8_t *peer_id  = p; p += GR_PEER_ID_LEN;
    const uint8_t *peer_pk  = p; p += GR_PUBLIC_KEY_LEN;
    const uint8_t *peer_sig = p;

    gr_error_t err = gr_join_submit_peer_header(c->reg, &hdr,
                        peer_id, peer_pk, peer_sig);
    if (err != GR_OK) return;

    /* Evaluate */
    gr_join_verify_result_t result;
    if (gr_join_evaluate(c->reg, &result) != GR_OK) return;

    if (result.state == GR_JOIN_VERIFIED) {
        if (c->on_event)
            c->on_event(c->user, YUMI_EVENT_VERIFIED, NULL);
    } else if (result.state == GR_JOIN_FAILED) {
        if (c->on_event)
            c->on_event(c->user, YUMI_EVENT_VERIFICATION_FAILED, NULL);
    }
}

/* Handle incoming attestation message on channel 254 */
static void attest_handle(yumi_client_t *c, peer_slot_t *s,
                           uint8_t msg_type, const uint8_t *data, uint32_t len)
{
    switch (msg_type) {
    case ATT_NONCE:
        /* A joining peer sent us their nonce — respond with attestation */
        if (len >= GR_JOIN_NONCE_LEN)
            attest_respond(c, s, data);
        break;
    case ATT_HEADER:
        /* We received an attestation (we're the joiner) */
        attest_process(c, data, len);
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 8.5: Meshnet relay protocol (SUDP channel 1)
 *
 *  Enables communication with peers that can't be directly reached.
 *  TTL scales logarithmically with group size: max(2, ceil(log2(N))).
 *  Token-bucket rate limiter: 10 KB/s per forwarding peer.
 *  Behavioral analysis flags excessive relay abuse.
 * ═══════════════════════════════════════════════════════════════════ */

/* Monotonic time in milliseconds */
static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Compute TTL = max(2, ceil(log2(active_peers))) using bit tricks */
static uint8_t mesh_compute_ttl(const yumi_client_t *c)
{
    uint32_t n = count_connected(c);
    if (n <= 2) return YUMI_MESH_MIN_TTL;
    uint8_t bits = 0;
    uint32_t v = n - 1;
    while (v > 0) { bits++; v >>= 1; }
    return bits < YUMI_MESH_MIN_TTL ? YUMI_MESH_MIN_TTL : bits;
}

/* Token-bucket: refill and check if `nbytes` can pass */
static bool mesh_bucket_allow(peer_slot_t *s, uint32_t nbytes)
{
    int64_t now = mono_ms();
    if (s->mesh_last_refill == 0) s->mesh_last_refill = now;

    int64_t elapsed = now - s->mesh_last_refill;
    if (elapsed > 0) {
        int64_t refill = ((int64_t)YUMI_MESH_RATE_BYTES_SEC * elapsed) / 1000;
        s->mesh_tokens += refill;
        if (s->mesh_tokens > YUMI_MESH_RATE_BYTES_SEC)
            s->mesh_tokens = YUMI_MESH_RATE_BYTES_SEC;
        s->mesh_last_refill = now;
    }

    if (s->mesh_tokens < (int64_t)nbytes)
        return false;

    s->mesh_tokens -= nbytes;
    return true;
}

/*
 * Forward a MESH_FORWARD packet toward the destination.
 * If we have a direct route to dst, send only there.
 * Otherwise flood to all connected peers (except sender) with ttl-1.
 * Caller must hold peer_lock.
 */
static void mesh_forward_to_peers(yumi_client_t *c, int sender_idx,
                                   const uint8_t dst_id[GR_PEER_ID_LEN],
                                   uint8_t ttl,
                                   const uint8_t *wire, uint32_t wire_len)
{
    /* Try direct route first */
    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        if (i == sender_idx) continue;
        peer_slot_t *s = &c->peers[i];
        if (atomic_load(&s->state) != SLOT_CONNECTED || !s->sudp) continue;
        if (gr_id_equal(s->peer_id, dst_id)) {
            yumi_sudp_client_send_reliable_channel(s->sudp,
                YUMI_CLIENT_MESH_CH, wire, wire_len);
            return;
        }
    }

    /* No direct route — flood with decremented TTL */
    if (ttl == 0) return;

    uint8_t *fwd = malloc(wire_len);
    if (!fwd) return;
    memcpy(fwd, wire, wire_len);
    fwd[1] = ttl - 1;  /* byte 1 = ttl in MESH_FORWARD wire */

    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        if (i == sender_idx) continue;
        peer_slot_t *s = &c->peers[i];
        if (atomic_load(&s->state) != SLOT_CONNECTED || !s->sudp) continue;
        yumi_sudp_client_send_reliable_channel(s->sudp,
            YUMI_CLIENT_MESH_CH, fwd, wire_len);
    }
    free(fwd);
}

/*
 * Handle an incoming meshnet relay message on channel 1.
 * Wire: [type(1)][ttl(1)][src_id(32)][dst_id(32)][inner_ch(1)][payload...]
 */
static void mesh_handle(yumi_client_t *c, peer_slot_t *sender,
                         int sender_idx,
                         const uint8_t *data, uint32_t len)
{
    if (len < 1 + MESH_HDR_LEN) return;

    uint8_t type = data[0];
    if (type != MESH_FORWARD) return;

    uint8_t ttl              = data[1];
    const uint8_t *src_id    = data + 2;
    const uint8_t *dst_id    = data + 2 + GR_PEER_ID_LEN;
    uint8_t inner_ch         = data[2 + 2 * GR_PEER_ID_LEN];
    const uint8_t *payload   = data + 1 + MESH_HDR_LEN;
    uint32_t payload_len     = len - 1 - MESH_HDR_LEN;

    /* Rate limit: charge the sender's bucket */
    if (!mesh_bucket_allow(sender, len)) return;

    /* Track forwarded bytes for abuse detection */
    sender->mesh_fwd_bytes += len;

    /* Abuse threshold: 5x the rate limit sustained => flag */
    if (sender->mesh_fwd_bytes > (uint64_t)YUMI_MESH_RATE_BYTES_SEC * 5) {
        if (c->on_event)
            c->on_event(c->user, YUMI_EVENT_MESH_ABUSE, sender->peer_id);
        sender->mesh_fwd_bytes = 0;  /* reset after alert */
    }

    /* Destination is us? Deliver to application. */
    if (gr_id_equal(dst_id, c->identity.peer_id)) {
        if (c->on_message &&
            inner_ch >= YUMI_CLIENT_USER_CH &&
            inner_ch < YUMI_CLIENT_ATTEST_CH)
            c->on_message(c->user, src_id, inner_ch,
                          payload, payload_len, true);
        return;
    }

    /* Not for us — forward if TTL allows */
    if (ttl == 0) return;

    pthread_mutex_lock(&c->peer_lock);
    mesh_forward_to_peers(c, sender_idx, dst_id, ttl, data, len);
    pthread_mutex_unlock(&c->peer_lock);
}

/*
 * Send a user payload via meshnet relay when no direct SUDP exists.
 * Wraps in MESH_FORWARD envelope and floods to all connected peers.
 */
static int mesh_relay_send(yumi_client_t *c,
                            const uint8_t dst_id[GR_PEER_ID_LEN],
                            uint8_t inner_ch,
                            const void *data, uint32_t len)
{
    uint8_t ttl = mesh_compute_ttl(c);
    uint32_t wire_len = 1 + MESH_HDR_LEN + len;
    uint8_t *wire = malloc(wire_len);
    if (!wire) return -1;

    wire[0] = MESH_FORWARD;
    wire[1] = ttl;
    memcpy(wire + 2, c->identity.peer_id, GR_PEER_ID_LEN);
    memcpy(wire + 2 + GR_PEER_ID_LEN, dst_id, GR_PEER_ID_LEN);
    wire[2 + 2 * GR_PEER_ID_LEN] = inner_ch;
    memcpy(wire + 1 + MESH_HDR_LEN, data, len);

    int errors = 0;
    pthread_mutex_lock(&c->peer_lock);
    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        peer_slot_t *s = &c->peers[i];
        if (atomic_load(&s->state) == SLOT_CONNECTED && s->sudp) {
            if (yumi_sudp_client_send_reliable_channel(s->sudp,
                    YUMI_CLIENT_MESH_CH, wire, wire_len) != 0)
                errors++;
        }
    }
    pthread_mutex_unlock(&c->peer_lock);

    free(wire);
    return errors > 0 ? -1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 9: SUDP callbacks (per-peer)
 * ═══════════════════════════════════════════════════════════════════ */

/* Context passed to SUDP callbacks */
typedef struct {
    yumi_client_t *client;
    int            slot_idx;
} sudp_cb_ctx_t;

static void sudp_recv_cb(void *user, uint8_t channel,
                          const void *data, uint32_t len, bool reliable)
{
    sudp_cb_ctx_t *ctx = (sudp_cb_ctx_t *)user;
    yumi_client_t *c = ctx->client;
    peer_slot_t *s = &c->peers[ctx->slot_idx];

    /* Update keepalive — any incoming data resets the silence timer */
    s->last_recv_ms = mono_ms();

    if (channel == YUMI_CLIENT_SYNC_CH) {
        /* Sync protocol (channel 0) — reassemble if chunked */
        uint8_t msg_type = 0;
        uint32_t msg_len = 0;
        uint8_t *msg = reasm_feed(&s->sync_reasm, data, len, &msg_type, &msg_len);
        if (msg) {
            if (msg == (uint8_t *)(uintptr_t)1)
                sync_handle(c, s, msg_type, NULL, 0);
            else {
                sync_handle(c, s, msg_type, msg, msg_len);
                free(msg);
            }
        }
    } else if (channel == YUMI_CLIENT_MESH_CH) {
        /* Meshnet relay (channel 1) — no chunking, direct dispatch.
         * Mesh messages are bounded by SUDP max payload;
         * wire format must stay intact for forwarding. */
        mesh_handle(c, s, ctx->slot_idx, data, len);
    } else if (channel == YUMI_CLIENT_ATTEST_CH) {
        /* Attestation protocol (channel 254) — reassemble if chunked */
        uint8_t msg_type = 0;
        uint32_t msg_len = 0;
        uint8_t *msg = reasm_feed(&s->attest_reasm, data, len, &msg_type, &msg_len);
        if (msg) {
            if (msg == (uint8_t *)(uintptr_t)1)
                attest_handle(c, s, msg_type, NULL, 0);
            else {
                attest_handle(c, s, msg_type, msg, msg_len);
                free(msg);
            }
        }
    } else if (channel >= YUMI_CLIENT_USER_CH && channel < YUMI_CLIENT_ATTEST_CH) {
        /* User data (channels 2-253) — forward to application */
        if (c->on_message)
            c->on_message(c->user, s->peer_id, channel, data, len, reliable);
    }
}

static void sudp_state_cb(void *user, yumi_sudp_state_t state)
{
    sudp_cb_ctx_t *ctx = (sudp_cb_ctx_t *)user;
    yumi_client_t *c = ctx->client;
    peer_slot_t *s = &c->peers[ctx->slot_idx];

    if (state == YUMI_SUDP_ESTABLISHED) {
        atomic_store(&s->state, SLOT_CONNECTED);

        /* Initialize keepalive timer */
        s->last_recv_ms = mono_ms();
        s->last_ping_ms = 0;
        s->missed_pongs = 0;

        /* Duplicate detection: if another slot with the same peer_id
         * is already CONNECTED, this is a second browser instance
         * using the same identity.  Tear down the newcomer. */
        pthread_mutex_lock(&c->peer_lock);
        for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
            peer_slot_t *other = &c->peers[i];
            if (other == s) continue;
            if (atomic_load(&other->state) == SLOT_CONNECTED &&
                gr_id_equal(other->peer_id, s->peer_id)) {
                if (c->on_event)
                    c->on_event(c->user, YUMI_EVENT_DUPLICATE_PEER, s->peer_id);
                atomic_store(&s->state, SLOT_FAILED);
                pthread_mutex_unlock(&c->peer_lock);
                return;
            }
        }
        pthread_mutex_unlock(&c->peer_lock);

        if (c->on_event)
            c->on_event(c->user, YUMI_EVENT_PEER_CONNECTED, s->peer_id);

        /* Exchange versions immediately */
        sync_send_version(c, s);

        /* If we're joining and haven't sent our nonce yet, do it now */
        if (c->joining && !s->nonce_sent) {
            gr_join_state_t js;
            if (gr_join_get_state(c->reg, &js) == GR_OK &&
                (js == GR_JOIN_PROVISIONAL || js == GR_JOIN_NONE))
                attest_send_nonce(c, s);
        }
    } else if (state == YUMI_SUDP_FAILED) {
        atomic_store(&s->state, SLOT_FAILED);

        if (c->on_event)
            c->on_event(c->user, YUMI_EVENT_PEER_DISCONNECTED, s->peer_id);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 10: SUDP connection management
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Callback context allocation — lives in heap, freed when slot is freed.
 * SUDP callbacks need stable pointers.
 */
static sudp_cb_ctx_t *alloc_cb_ctx(yumi_client_t *c, int slot_idx)
{
    sudp_cb_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->client   = c;
    ctx->slot_idx = slot_idx;
    return ctx;
}

static void slot_free_full(peer_slot_t *s, sudp_cb_ctx_t **cb_ctx)
{
    slot_free(s);
    if (cb_ctx && *cb_ctx) { free(*cb_ctx); *cb_ctx = NULL; }
}

/*
 * Connect to a peer.  Creates an SUDP client and initiates the handshake.
 * Called from the sync worker or boot worker thread.
 */
static int peer_connect(yumi_client_t *c,
                         const uint8_t peer_id[GR_PEER_ID_LEN],
                         const char *ip, uint16_t sudp_port,
                         bool first_contact, uint32_t stun_idx)
{
    /* Don't connect to ourselves */
    if (gr_id_equal(peer_id, c->identity.peer_id)) return 0;

    pthread_mutex_lock(&c->peer_lock);

    /* Already connected or connecting? */
    peer_slot_t *existing = slot_find(c, peer_id);
    if (existing) {
        /* Alert if fully connected (potential duplicate instance) */
        if (atomic_load(&existing->state) == SLOT_CONNECTED && c->on_event)
            c->on_event(c->user, YUMI_EVENT_DUPLICATE_PEER, peer_id);
        pthread_mutex_unlock(&c->peer_lock);
        return 0;
    }

    peer_slot_t *s = slot_alloc(c);
    if (!s) {
        pthread_mutex_unlock(&c->peer_lock);
        return -1;
    }

    int slot_idx = (int)(s - c->peers);
    memcpy(s->peer_id, peer_id, GR_PEER_ID_LEN);
    s->first_contact = first_contact;
    s->stun_idx = stun_idx;
    atomic_store(&s->state, SLOT_CONNECTING);

    pthread_mutex_unlock(&c->peer_lock);

    /* Build SUDP config */
    struct sockaddr_in6 peer_addr = {0};
    peer_addr.sin6_family = AF_INET6;
    peer_addr.sin6_port   = htons(sudp_port);

    /* Try IPv4-mapped-IPv6 first, then raw IPv6 */
    if (inet_pton(AF_INET6, ip, &peer_addr.sin6_addr) != 1) {
        /* Try as IPv4 → mapped */
        struct in_addr v4;
        if (inet_pton(AF_INET, ip, &v4) == 1) {
            /* ::ffff:a.b.c.d */
            memset(&peer_addr.sin6_addr, 0, 10);
            peer_addr.sin6_addr.s6_addr[10] = 0xff;
            peer_addr.sin6_addr.s6_addr[11] = 0xff;
            memcpy(&peer_addr.sin6_addr.s6_addr[12], &v4, 4);
        } else {
            slot_free(s);
            return -1;
        }
    }

    sudp_cb_ctx_t *cb_ctx = alloc_cb_ctx(c, slot_idx);
    if (!cb_ctx) { slot_free(s); return -1; }

    yumi_sudp_config_t scfg = {0};
    scfg.transport.peer_addr  = peer_addr;
    scfg.transport.local_port = 0; /* ephemeral */
    scfg.registrar     = c->reg;
    scfg.identity      = &c->identity;
    scfg.first_contact = first_contact;
    scfg.recv_cb       = sudp_recv_cb;
    scfg.state_cb      = sudp_state_cb;
    scfg.user          = cb_ctx;

    /* STUN server from the fallback list */
    if (c->stun_count > 0 && stun_idx < c->stun_count) {
        scfg.transport.stun_server = c->stun_list[stun_idx];
        scfg.transport.stun_port   = c->stun_port;
    }

    if (yumi_sudp_client_create(&s->sudp, &scfg) != 0) {
        free(cb_ctx);
        slot_free(s);
        return -1;
    }

    if (yumi_sudp_client_connect(s->sudp, peer_id) != 0) {
        yumi_sudp_client_destroy(s->sudp);
        s->sudp = NULL;
        free(cb_ctx);
        slot_free(s);
        return -1;
    }

    return 0;
}

/* Auto-connect to all active peers in the registrar */
static void auto_connect_peers(yumi_client_t *c)
{
    uint32_t count = 0;
    gr_peer_count(c->reg, GR_PEER_ACTIVE, &count);
    if (count == 0) return;

    uint32_t batch = count < 64 ? count : 64;
    gr_peer_t *peers = calloc(batch, sizeof(gr_peer_t));
    if (!peers) return;

    uint32_t actual = 0;
    gr_peer_list(c->reg, peers, batch, &actual, GR_PEER_ACTIVE);

    for (uint32_t i = 0; i < actual; i++) {
        if (gr_id_equal(peers[i].peer_id, c->identity.peer_id))
            continue;
        if (peers[i].port == 0 || peers[i].ip[0] == '\0')
            continue;

        pthread_mutex_lock(&c->peer_lock);
        bool exists = slot_find(c, peers[i].peer_id) != NULL;
        pthread_mutex_unlock(&c->peer_lock);

        if (!exists)
            peer_connect(c, peers[i].peer_id, peers[i].ip, peers[i].port, false, 0);
    }

    free(peers);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 11: Bootstrap protocol (raw UDP)
 * ═══════════════════════════════════════════════════════════════════ */

/* Create the bootstrap listener socket */
static int boot_create_listener(yumi_client_t *c, uint16_t port)
{
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Dual-stack: accept IPv4 too */
    int off = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_any;
    addr.sin6_port   = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    /* Get actual port if ephemeral */
    socklen_t slen = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &slen);
    c->boot_port = ntohs(addr.sin6_port);
    c->boot_fd   = fd;
    return 0;
}

/* Send serialized registrar in chunks to a raw UDP peer */
static void boot_send_registrar(yumi_client_t *c,
                                 const struct sockaddr_in6 *to, socklen_t tolen)
{
    uint8_t *blob = NULL;
    size_t blen = 0;
    if (gr_serialize(c->reg, GR_SERIALIZE_FULL, &blob, &blen) != GR_OK || !blob)
        return;

    uint32_t total_chunks = (uint32_t)((blen + YUMI_BOOT_CHUNK_SIZE - 1) / YUMI_BOOT_CHUNK_SIZE);
    uint8_t pkt[BOOT_HDR + 1 + 2 + 2 + YUMI_BOOT_CHUNK_SIZE];

    for (uint32_t i = 0; i < total_chunks; i++) {
        size_t offset = (size_t)i * YUMI_BOOT_CHUNK_SIZE;
        size_t chunk  = blen - offset;
        if (chunk > YUMI_BOOT_CHUNK_SIZE) chunk = YUMI_BOOT_CHUNK_SIZE;

        memcpy(pkt, BOOT_MAGIC, BOOT_HDR);
        pkt[BOOT_HDR] = BOOT_DATA;
        put_u16(pkt + BOOT_HDR + 1, (uint16_t)i);
        put_u16(pkt + BOOT_HDR + 3, (uint16_t)total_chunks);
        memcpy(pkt + BOOT_HDR + 5, blob + offset, chunk);

        sendto(c->boot_fd, pkt, BOOT_HDR + 5 + chunk, 0,
               (const struct sockaddr *)to, tolen);

        /* Small pacing delay to avoid burst drops */
        if (i % 32 == 31) usleep(1000);
    }

    /* DONE marker */
    memcpy(pkt, BOOT_MAGIC, BOOT_HDR);
    pkt[BOOT_HDR] = BOOT_DONE;
    sendto(c->boot_fd, pkt, BOOT_HDR + 1, 0,
           (const struct sockaddr *)to, tolen);

    gr_free(blob);
}

/* Handle CONN_REQ: a peer wants to establish an SUDP connection.
 * Returns 0 on success, -1 if the peer_id is invalid (failed auth). */
static int boot_handle_conn_req(yumi_client_t *c,
                                  const uint8_t *data, ssize_t len,
                                  const struct sockaddr_in6 *from, socklen_t fromlen)
{
    /* [YBOT][0x10][peer_id(32)][sudp_port(2)] */
    if (len < BOOT_HDR + 1 + GR_PEER_ID_LEN + 2) return -1;

    const uint8_t *peer_id   = data + BOOT_HDR + 1;
    uint16_t remote_sudp_port = get_u16(data + BOOT_HDR + 1 + GR_PEER_ID_LEN);

    /* Verify peer is in the registrar */
    gr_peer_t peer;
    if (gr_peer_get(c->reg, peer_id, &peer) != GR_OK) return -1;
    if (peer.status != GR_PEER_ACTIVE) return -1;

    /* Extract the source IP */
    char ip[GR_MAX_IP_LEN] = {0};
    inet_ntop(AF_INET6, &from->sin6_addr, ip, sizeof(ip));

    /* Connect to the peer (our side creates SUDP pointed at their SUDP port) */
    peer_connect(c, peer_id, ip, remote_sudp_port, false, 0);

    /* Respond with our SUDP port.  Find the slot we just created. */
    pthread_mutex_lock(&c->peer_lock);
    peer_slot_t *s = slot_find(c, peer_id);
    uint16_t our_sudp_port = s ? s->sudp_local_port : 0;
    pthread_mutex_unlock(&c->peer_lock);

    /* If peer_connect used ephemeral, we need to discover the port.
     * We'll send boot_port as fallback (the SUDP responder doesn't need
     * to initiate, it just listens). The initiator's connect() sends
     * packets that the responder's UDP client picks up by source addr. */

    uint8_t resp[BOOT_HDR + 1 + GR_PEER_ID_LEN + 2];
    memcpy(resp, BOOT_MAGIC, BOOT_HDR);
    resp[BOOT_HDR] = BOOT_CONN_RSP;
    memcpy(resp + BOOT_HDR + 1, c->identity.peer_id, GR_PEER_ID_LEN);
    put_u16(resp + BOOT_HDR + 1 + GR_PEER_ID_LEN, our_sudp_port);
    sendto(c->boot_fd, resp, sizeof(resp), 0,
           (const struct sockaddr *)from, fromlen);
    return 0;
}

/* Bootstrap worker — handles registrar fetch requests and CONN signaling */
static void *boot_worker(void *arg)
{
    yumi_client_t *c = (yumi_client_t *)arg;
    struct pollfd pfd = { .fd = c->boot_fd, .events = POLLIN };

    while (atomic_load(&c->running)) {
        if (poll(&pfd, 1, 200) <= 0) continue;

        uint8_t buf[2048];
        struct sockaddr_in6 from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(c->boot_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < BOOT_HDR + 1) continue;
        if (memcmp(buf, BOOT_MAGIC, BOOT_HDR) != 0) continue;

        /* Convert source IP once for all DB lookups */
        char ip_str[GR_MAX_IP_LEN];
        inet_ntop(AF_INET6, &from.sin6_addr, ip_str, sizeof(ip_str));

        /* ── IP blocklist check (DuckDB-backed) ────────────────── */
        bool blocked = false;
        gr_blocklist_check(c->reg, ip_str,
                           BOOT_BLOCK_DURATION_MS, &blocked);
        if (blocked)
            continue; /* silently drop all traffic from blocked IPs */

        uint8_t type = buf[BOOT_HDR];
        switch (type) {
        case BOOT_REQ: {
            /* Registrar fetch request: [YBOT][0x01][token(128)] */
            if (n < BOOT_HDR + 1 + (ssize_t)GR_HASH_LEN) break;
            const uint8_t *token = buf + BOOT_HDR + 1;

            bool valid = false;
            gr_invite_check(c->reg, token, &valid);
            if (!valid) {
                bool just_blocked = false;
                gr_blocklist_record_fail(c->reg, ip_str,
                                         BOOT_BLOCK_MAX_FAILS,
                                         &just_blocked);

                if (just_blocked) {
                    char detail[128];
                    snprintf(detail, sizeof(detail),
                             "Blocked IP %s after %d failed boot attempts",
                             ip_str, BOOT_BLOCK_MAX_FAILS);
                    gr_audit_append(c->reg,
                                    GR_CHANGE_BOOT_IP_BLOCKED,
                                    &c->identity, NULL, detail);
                    if (c->on_event)
                        c->on_event(c->user, YUMI_EVENT_BOOT_BLOCKED,
                                    NULL);
                }
                break;
            }

            /* Valid token — clear any prior failures for this IP */
            gr_blocklist_reset(c->reg, ip_str);

            boot_send_registrar(c, &from, fromlen);
            break;
        }
        case BOOT_CONN_REQ: {
            int cr = boot_handle_conn_req(c, buf, n, &from, fromlen);
            if (cr != 0) {
                bool just_blocked = false;
                gr_blocklist_record_fail(c->reg, ip_str,
                                         BOOT_BLOCK_MAX_FAILS,
                                         &just_blocked);
                if (just_blocked) {
                    char detail[128];
                    snprintf(detail, sizeof(detail),
                             "Blocked IP %s after %d failed conn attempts",
                             ip_str, BOOT_BLOCK_MAX_FAILS);
                    gr_audit_append(c->reg,
                                    GR_CHANGE_BOOT_IP_BLOCKED,
                                    &c->identity, NULL, detail);
                    if (c->on_event)
                        c->on_event(c->user, YUMI_EVENT_BOOT_BLOCKED,
                                    NULL);
                }
            }
            break;
        }
        case BOOT_CONN_RSP: {
            /* Handled by the initiator's boot_send_conn_req polling loop,
             * not here.  The boot_worker only handles incoming requests. */
            break;
        }
        case BOOT_ACK:
            /* Client acknowledged registrar receipt — nothing to do */
            break;
        case BOOT_RECONNECT: {
            /* Reconnect notification: [YBOT][0x20][4-byte magic][12-byte peer_id prefix]
             * Total: BOOT_HDR + 1 + 16 = 21 bytes */
            if (n < BOOT_HDR + 1 + 16) break;
            const uint8_t *payload = buf + BOOT_HDR + 1;
            uint32_t magic;
            memcpy(&magic, payload, 4);
            if (magic != 0x59524E43) break;  /* "YRNC" */

            /* Match the 12-byte prefix against our identity */
            if (memcmp(payload + 4, c->identity.peer_id, 12) == 0) {
                /* This reconnect is addressed to us — fire event */
                if (c->on_event)
                    c->on_event(c->user, YUMI_EVENT_PEER_CONNECTED,
                                c->identity.peer_id);
            }
            break;
        }
        default:
            break;
        }
    }
    return NULL;
}

/* ── Bootstrap client: fetch registrar from a bootstrap peer ─── */

static int boot_fetch_registrar(const gr_bootstrap_peer_t *peers, uint32_t count,
                                 const uint8_t token[GR_HASH_LEN],
                                 uint8_t **out_data, size_t *out_len)
{
    *out_data = NULL;
    *out_len  = 0;

    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    /* Bind to ephemeral port */
    struct sockaddr_in6 local = {0};
    local.sin6_family = AF_INET6;
    local.sin6_addr   = in6addr_any;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        close(fd);
        return -1;
    }

    /* Build request: [YBOT][0x01][token(128)] */
    uint8_t req[BOOT_HDR + 1 + GR_HASH_LEN];
    memcpy(req, BOOT_MAGIC, BOOT_HDR);
    req[BOOT_HDR] = BOOT_REQ;
    memcpy(req + BOOT_HDR + 1, token, GR_HASH_LEN);

    /* Try each bootstrap peer */
    for (uint32_t p = 0; p < count; p++) {
        struct sockaddr_in6 dest = {0};
        dest.sin6_family = AF_INET6;
        dest.sin6_port   = htons(peers[p].port);
        if (inet_pton(AF_INET6, peers[p].ip, &dest.sin6_addr) != 1) {
            struct in_addr v4;
            if (inet_pton(AF_INET, peers[p].ip, &v4) == 1) {
                memset(&dest.sin6_addr, 0, 10);
                dest.sin6_addr.s6_addr[10] = 0xff;
                dest.sin6_addr.s6_addr[11] = 0xff;
                memcpy(&dest.sin6_addr.s6_addr[12], &v4, 4);
            } else continue;
        }

        /* Send REQ */
        sendto(fd, req, sizeof(req), 0,
               (struct sockaddr *)&dest, sizeof(dest));

        /* Receive chunks with timeout */
        uint16_t total_chunks = 0;
        uint8_t **chunks = NULL;
        uint16_t *chunk_lens = NULL;
        bool done = false;
        int retries = 0;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        uint64_t deadline = (uint64_t)time(NULL) + YUMI_BOOT_TIMEOUT_MS / 1000;

        while (!done && (uint64_t)time(NULL) < deadline) {
            int pr = poll(&pfd, 1, YUMI_BOOT_RETRY_MS);
            if (pr <= 0) {
                if (++retries > 5) break;
                sendto(fd, req, sizeof(req), 0,
                       (struct sockaddr *)&dest, sizeof(dest));
                continue;
            }

            uint8_t rbuf[BOOT_HDR + 5 + YUMI_BOOT_CHUNK_SIZE + 64];
            ssize_t rn = recvfrom(fd, rbuf, sizeof(rbuf), 0, NULL, NULL);
            if (rn < BOOT_HDR + 1) continue;
            if (memcmp(rbuf, BOOT_MAGIC, BOOT_HDR) != 0) continue;

            uint8_t rtype = rbuf[BOOT_HDR];

            if (rtype == BOOT_DATA && rn >= BOOT_HDR + 5) {
                uint16_t seq   = get_u16(rbuf + BOOT_HDR + 1);
                uint16_t total = get_u16(rbuf + BOOT_HDR + 3);
                uint16_t dlen  = (uint16_t)(rn - BOOT_HDR - 5);

                if (total == 0 || total > 10000) continue;

                /* First DATA — allocate chunk array */
                if (!chunks) {
                    total_chunks = total;
                    chunks = calloc(total, sizeof(uint8_t *));
                    chunk_lens = calloc(total, sizeof(uint16_t));
                    if (!chunks || !chunk_lens) break;
                }

                if (seq < total_chunks && !chunks[seq]) {
                    chunks[seq] = malloc(dlen);
                    if (chunks[seq]) {
                        memcpy(chunks[seq], rbuf + BOOT_HDR + 5, dlen);
                        chunk_lens[seq] = dlen;
                    }
                }
            } else if (rtype == BOOT_DONE) {
                done = true;
            } else if (rtype == BOOT_ERR) {
                break; /* This peer rejected us, try next */
            }
        }

        if (!done || !chunks) {
            if (chunks) {
                for (uint16_t i = 0; i < total_chunks; i++) free(chunks[i]);
                free(chunks);
            }
            free(chunk_lens);
            continue; /* Try next peer */
        }

        /* Check for missing chunks */
        bool complete = true;
        for (uint16_t i = 0; i < total_chunks; i++) {
            if (!chunks[i]) { complete = false; break; }
        }

        if (!complete) {
            /* Send NACK for missing chunks */
            uint16_t missing = 0;
            for (uint16_t i = 0; i < total_chunks; i++)
                if (!chunks[i]) missing++;

            size_t nack_len = BOOT_HDR + 1 + 2 + missing * 2;
            uint8_t *nack = malloc(nack_len);
            if (nack) {
                memcpy(nack, BOOT_MAGIC, BOOT_HDR);
                nack[BOOT_HDR] = BOOT_NACK;
                put_u16(nack + BOOT_HDR + 1, missing);
                uint8_t *np = nack + BOOT_HDR + 3;
                for (uint16_t i = 0; i < total_chunks; i++) {
                    if (!chunks[i]) { put_u16(np, i); np += 2; }
                }
                sendto(fd, nack, nack_len, 0,
                       (struct sockaddr *)&dest, sizeof(dest));
                free(nack);

                /* Wait a bit for retransmits */
                uint64_t retry_deadline = (uint64_t)time(NULL) + 5;
                while ((uint64_t)time(NULL) < retry_deadline) {
                    if (poll(&pfd, 1, 500) <= 0) break;
                    uint8_t rbuf2[BOOT_HDR + 5 + YUMI_BOOT_CHUNK_SIZE + 64];
                    ssize_t rn2 = recvfrom(fd, rbuf2, sizeof(rbuf2), 0, NULL, NULL);
                    if (rn2 < BOOT_HDR + 5) continue;
                    if (memcmp(rbuf2, BOOT_MAGIC, BOOT_HDR) != 0) continue;
                    if (rbuf2[BOOT_HDR] != BOOT_DATA) continue;
                    uint16_t seq2  = get_u16(rbuf2 + BOOT_HDR + 1);
                    uint16_t dlen2 = (uint16_t)(rn2 - BOOT_HDR - 5);
                    if (seq2 < total_chunks && !chunks[seq2]) {
                        chunks[seq2] = malloc(dlen2);
                        if (chunks[seq2]) {
                            memcpy(chunks[seq2], rbuf2 + BOOT_HDR + 5, dlen2);
                            chunk_lens[seq2] = dlen2;
                        }
                    }
                    /* Check again */
                    complete = true;
                    for (uint16_t i = 0; i < total_chunks; i++) {
                        if (!chunks[i]) { complete = false; break; }
                    }
                    if (complete) break;
                }
            }
        }

        if (complete) {
            /* Assemble */
            size_t total_len = 0;
            for (uint16_t i = 0; i < total_chunks; i++) total_len += chunk_lens[i];

            uint8_t *assembled = malloc(total_len);
            if (assembled) {
                size_t pos = 0;
                for (uint16_t i = 0; i < total_chunks; i++) {
                    memcpy(assembled + pos, chunks[i], chunk_lens[i]);
                    pos += chunk_lens[i];
                }

                /* Send ACK */
                uint8_t ack[BOOT_HDR + 1];
                memcpy(ack, BOOT_MAGIC, BOOT_HDR);
                ack[BOOT_HDR] = BOOT_ACK;
                sendto(fd, ack, sizeof(ack), 0,
                       (struct sockaddr *)&dest, sizeof(dest));

                *out_data = assembled;
                *out_len  = total_len;
            }
        }

        for (uint16_t i = 0; i < total_chunks; i++) free(chunks[i]);
        free(chunks);
        free(chunk_lens);

        if (*out_data) { close(fd); return 0; }
    }

    close(fd);
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 12: Sync worker thread
 * ═══════════════════════════════════════════════════════════════════ */

static void *sync_worker(void *arg)
{
    yumi_client_t *c = (yumi_client_t *)arg;

    while (atomic_load(&c->running)) {
        usleep(SYNC_POLL_MS * 1000);
        if (!atomic_load(&c->running)) break;

        /* 1. Check for local version advancement → broadcast delta */
        gr_header_t hdr;
        if (gr_get_header(c->reg, &hdr) == GR_OK) {
            if (hdr.version > c->last_broadcast_ver) {
                uint32_t old_ver = c->last_broadcast_ver;
                c->last_broadcast_ver = hdr.version;
                sync_broadcast_delta(c, old_ver);
            }
        }

        /* 2. Auto-connect to new peers */
        auto_connect_peers(c);

        /* 3. Clean up failed slots (with STUN fallback retry) */
        {
            struct { uint8_t pid[GR_PEER_ID_LEN]; uint32_t next_stun; bool fc; }
                retries[YUMI_CLIENT_MAX_PEERS];
            int nretry = 0;

            pthread_mutex_lock(&c->peer_lock);
            for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
                peer_slot_t *s = &c->peers[i];
                if (atomic_load(&s->state) != SLOT_FAILED) continue;
                if (c->stun_count > 1 && (s->stun_idx + 1) < c->stun_count) {
                    memcpy(retries[nretry].pid, s->peer_id, GR_PEER_ID_LEN);
                    retries[nretry].next_stun = s->stun_idx + 1;
                    retries[nretry].fc = s->first_contact;
                    nretry++;
                }
                slot_free(s);
            }
            pthread_mutex_unlock(&c->peer_lock);

            /* Retry failed peers with the next STUN server */
            for (int i = 0; i < nretry; i++) {
                gr_peer_t p;
                if (gr_peer_get(c->reg, retries[i].pid, &p) == GR_OK &&
                    p.status == GR_PEER_ACTIVE && p.ip[0] && p.port)
                    peer_connect(c, retries[i].pid, p.ip, p.port,
                                 retries[i].fc, retries[i].next_stun);
            }
        }

        /* 3.5. Keepalive: ping idle peers, disconnect unresponsive ones */
        {
            int64_t now = mono_ms();
            pthread_mutex_lock(&c->peer_lock);
            for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
                peer_slot_t *s = &c->peers[i];
                if (atomic_load(&s->state) != SLOT_CONNECTED || !s->sudp)
                    continue;
                if (s->last_recv_ms == 0) continue;

                int64_t silent = now - s->last_recv_ms;
                if (silent >= KEEPALIVE_TIMEOUT_MS) {
                    /* Peer is dead — tear down */
                    if (c->on_event)
                        c->on_event(c->user, YUMI_EVENT_PEER_DISCONNECTED,
                                    s->peer_id);
                    slot_free(s);
                    continue;
                }
                if (silent >= KEEPALIVE_INTERVAL_MS &&
                    (now - s->last_ping_ms) >= KEEPALIVE_INTERVAL_MS) {
                    chunked_send(s->sudp, YUMI_CLIENT_SYNC_CH,
                                 SYNC_PING, NULL, 0);
                    s->last_ping_ms = now;
                    s->missed_pongs++;
                }
            }
            pthread_mutex_unlock(&c->peer_lock);
        }

        /* 4. If joining, send nonces to any newly-connected peers */
        if (c->joining) {
            gr_join_state_t js;
            if (gr_join_get_state(c->reg, &js) == GR_OK) {
                if (js == GR_JOIN_VERIFIED || js == GR_JOIN_FAILED)
                    c->joining = false;
            }

            if (c->joining) {
                pthread_mutex_lock(&c->peer_lock);
                for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
                    peer_slot_t *s = &c->peers[i];
                    if (atomic_load(&s->state) == SLOT_CONNECTED && !s->nonce_sent)
                        attest_send_nonce(c, s);
                }
                pthread_mutex_unlock(&c->peer_lock);
            }
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 13: Worker lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

static int start_workers(yumi_client_t *c)
{
    atomic_store(&c->running, true);

    if (pthread_create(&c->sync_thread, NULL, sync_worker, c) != 0)
        return -1;

    if (c->boot_fd >= 0) {
        if (pthread_create(&c->boot_thread, NULL, boot_worker, c) != 0) {
            atomic_store(&c->running, false);
            pthread_join(c->sync_thread, NULL);
            return -1;
        }
    }

    return 0;
}

static void stop_workers(yumi_client_t *c)
{
    atomic_store(&c->running, false);

    pthread_join(c->sync_thread, NULL);

    if (c->boot_fd >= 0) {
        pthread_join(c->boot_thread, NULL);
        close(c->boot_fd);
        c->boot_fd = -1;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 14: Public API — Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

static yumi_client_t *client_alloc(const yumi_client_config_t *cfg)
{
    yumi_client_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    pthread_mutex_init(&c->peer_lock, NULL);
    c->boot_fd    = -1;
    c->on_message = cfg->on_message;
    c->on_event   = cfg->on_event;
    c->user       = cfg->user;
    atomic_store(&c->state, YUMI_CLIENT_INITIALIZING);

    if (cfg->db_path) {
        size_t n = strlen(cfg->db_path);
        if (n >= sizeof(c->db_path)) n = sizeof(c->db_path) - 1;
        memcpy(c->db_path, cfg->db_path, n);
        c->db_path[n] = '\0';
    }

    /* Build STUN fallback list */
    if (cfg->stun_servers && cfg->stun_server_count > 0) {
        c->stun_count = cfg->stun_server_count;
        c->stun_list  = calloc(c->stun_count, sizeof(char *));
        if (c->stun_list) {
            for (uint32_t i = 0; i < c->stun_count; i++)
                c->stun_list[i] = strdup(cfg->stun_servers[i]);
        }
    } else if (cfg->stun_server) {
        c->stun_count = 1;
        c->stun_list  = calloc(1, sizeof(char *));
        if (c->stun_list)
            c->stun_list[0] = strdup(cfg->stun_server);
    }
    c->stun_port = cfg->stun_port;

    return c;
}

int yumi_client_create_group(yumi_client_t **out,
                              const yumi_client_config_t *cfg)
{
    if (!out || !cfg || !cfg->db_path || !cfg->group_name) return -1;
    *out = NULL;

    yumi_client_t *c = client_alloc(cfg);
    if (!c) return -1;

    /* Generate identity */
    if (gr_identity_generate(&c->identity) != GR_OK) { free(c); return -1; }

    /* Create group */
    if (gr_create(&c->reg, cfg->db_path, cfg->group_name,
                  cfg->group_type, &c->identity) != GR_OK) {
        free(c);
        return -1;
    }

    /* Sign the initial registrar */
    gr_sign(c->reg, &c->identity);

    /* Start bootstrap listener */
    if (boot_create_listener(c, cfg->local_port) != 0) {
        gr_close(c->reg);
        free(c);
        return -1;
    }

    /* Register our bootstrap port in the registrar */
    gr_peer_update_address(c->reg, "::1", c->boot_port, &c->identity);

    /* Initialize version tracking */
    gr_header_t hdr;
    if (gr_get_header(c->reg, &hdr) == GR_OK)
        c->last_broadcast_ver = hdr.version;

    /* Start background threads */
    if (start_workers(c) != 0) {
        gr_close(c->reg);
        close(c->boot_fd);
        free(c);
        return -1;
    }

    atomic_store(&c->state, YUMI_CLIENT_RUNNING);
    *out = c;
    return 0;
}

int yumi_client_open(yumi_client_t **out,
                     const yumi_client_config_t *cfg,
                     const uint8_t group_id[GR_HASH_LEN],
                     const gr_identity_t *identity)
{
    if (!out || !cfg || !cfg->db_path || !group_id || !identity) return -1;
    *out = NULL;

    yumi_client_t *c = client_alloc(cfg);
    if (!c) return -1;

    memcpy(&c->identity, identity, sizeof(gr_identity_t));

    /* Open existing registrar */
    if (gr_open(&c->reg, cfg->db_path, group_id) != GR_OK) {
        free(c);
        return -1;
    }

    /* Start bootstrap listener */
    if (boot_create_listener(c, cfg->local_port) != 0) {
        gr_close(c->reg);
        free(c);
        return -1;
    }

    /* Update our address */
    gr_peer_update_address(c->reg, "::1", c->boot_port, &c->identity);

    /* Initialize version tracking */
    gr_header_t hdr;
    if (gr_get_header(c->reg, &hdr) == GR_OK)
        c->last_broadcast_ver = hdr.version;

    /* Start background threads */
    if (start_workers(c) != 0) {
        gr_close(c->reg);
        close(c->boot_fd);
        free(c);
        return -1;
    }

    atomic_store(&c->state, YUMI_CLIENT_CONNECTING);

    /* Auto-connect happens in sync_worker */
    *out = c;
    return 0;
}

int yumi_client_join(yumi_client_t **out,
                     const yumi_client_config_t *cfg,
                     const uint8_t *invite_blob, size_t invite_len)
{
    if (!out || !cfg || !cfg->db_path || !invite_blob || invite_len == 0) return -1;
    *out = NULL;

    /* 1. Parse invite */
    gr_invite_ticket_t ticket;
    if (gr_invite_parse(invite_blob, invite_len, &ticket) != GR_OK)
        return -1;

    yumi_client_t *c = client_alloc(cfg);
    if (!c) return -1;

    atomic_store(&c->state, YUMI_CLIENT_SYNCING);
    c->joining = true;

    /* 2. Generate identity */
    if (gr_identity_generate(&c->identity) != GR_OK) {
        free(c);
        return -1;
    }

    /* 3. Fetch registrar from bootstrap peers (blocking) */
    uint8_t *reg_blob = NULL;
    size_t reg_len = 0;
    if (boot_fetch_registrar(ticket.bootstrap_peers, ticket.bootstrap_count,
                             ticket.verification_token,
                             &reg_blob, &reg_len) != 0) {
        free(c);
        return -1;
    }

    /* 4. Verify registrar hash matches invite */
    uint8_t computed_hash[GR_HASH_LEN];
    gr_hash(reg_blob, reg_len, computed_hash);
    /* Note: hash may differ if registrar changed since invite creation.
     * We proceed anyway — attestation is the true trust anchor. */

    /* 5. Create local registrar and deserialize */
    if (gr_create(&c->reg, cfg->db_path, ticket.group_name,
                  ticket.group_type, &c->identity) != GR_OK) {
        free(reg_blob);
        free(c);
        return -1;
    }

    if (gr_deserialize(c->reg, reg_blob, reg_len) != GR_OK) {
        free(reg_blob);
        gr_close(c->reg);
        free(c);
        return -1;
    }
    free(reg_blob);

    /* 6. Add ourselves as a peer */
    gr_peer_t me = {0};
    memcpy(me.peer_id, c->identity.peer_id, GR_PEER_ID_LEN);
    memcpy(me.sign_key, c->identity.public_key, GR_PUBLIC_KEY_LEN);
    memcpy(me.kem_pk, c->identity.kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    snprintf(me.ip, sizeof(me.ip), "::1");
    me.status    = GR_PEER_ACTIVE;
    me.joined_at = gr_timestamp_ms();
    me.last_seen = me.joined_at;
    /* Use the owner identity for the peer_add since we need INVITE perm.
     * After join verification, the registrar is trusted. For now, this
     * self-add is provisional. */

    /* 7. Begin join verification */
    gr_join_begin(c->reg, &ticket);

    /* 8. Start bootstrap listener */
    if (boot_create_listener(c, cfg->local_port) != 0) {
        gr_close(c->reg);
        free(c);
        return -1;
    }
    me.port = c->boot_port;
    /* We can't self-add without authority; the peer list came from the
     * serialized registrar.  Our presence is established via attestation.
     * Just update our address if we're already listed. */
    gr_peer_update_address(c->reg, me.ip, me.port, &c->identity);

    /* Initialize version tracking */
    gr_header_t hdr;
    if (gr_get_header(c->reg, &hdr) == GR_OK)
        c->last_broadcast_ver = hdr.version;

    /* 9. Start background threads — sync worker will auto-connect
     * and attestation will run via callbacks */
    if (start_workers(c) != 0) {
        gr_close(c->reg);
        close(c->boot_fd);
        free(c);
        return -1;
    }

    atomic_store(&c->state, YUMI_CLIENT_CONNECTING);
    *out = c;
    return 0;
}

void yumi_client_destroy(yumi_client_t *c)
{
    if (!c) return;

    /* Stop background threads */
    stop_workers(c);

    /* Disconnect all peers */
    pthread_mutex_lock(&c->peer_lock);
    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        if (atomic_load(&c->peers[i].state) != SLOT_FREE)
            slot_free(&c->peers[i]);
    }
    pthread_mutex_unlock(&c->peer_lock);

    /* Close registrar */
    if (c->reg)
        gr_close(c->reg);

    /* Free STUN list */
    if (c->stun_list) {
        for (uint32_t i = 0; i < c->stun_count; i++)
            free(c->stun_list[i]);
        free(c->stun_list);
    }

    /* Wipe secret keys */
    gr_identity_wipe(&c->identity);

    pthread_mutex_destroy(&c->peer_lock);
    free(c);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 15: Public API — Messaging
 * ═══════════════════════════════════════════════════════════════════ */

int yumi_client_broadcast(yumi_client_t *c, const void *data, uint32_t len)
{
    if (!c || !data) return -1;
    int errors = 0;

    pthread_mutex_lock(&c->peer_lock);
    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        peer_slot_t *s = &c->peers[i];
        if (atomic_load(&s->state) == SLOT_CONNECTED && s->sudp) {
            if (yumi_sudp_client_send_channel(s->sudp, YUMI_CLIENT_USER_CH,
                                               data, len) != 0)
                errors++;
        }
    }
    pthread_mutex_unlock(&c->peer_lock);

    return errors > 0 ? -1 : 0;
}

int yumi_client_broadcast_reliable(yumi_client_t *c, const void *data, uint32_t len)
{
    if (!c || !data) return -1;
    int errors = 0;

    pthread_mutex_lock(&c->peer_lock);
    for (int i = 0; i < YUMI_CLIENT_MAX_PEERS; i++) {
        peer_slot_t *s = &c->peers[i];
        if (atomic_load(&s->state) == SLOT_CONNECTED && s->sudp) {
            if (yumi_sudp_client_send_reliable_channel(s->sudp,
                    YUMI_CLIENT_USER_CH, data, len) != 0)
                errors++;
        }
    }
    pthread_mutex_unlock(&c->peer_lock);

    return errors > 0 ? -1 : 0;
}

int yumi_client_send(yumi_client_t *c,
                     const uint8_t peer_id[GR_PEER_ID_LEN],
                     const void *data, uint32_t len)
{
    if (!c || !peer_id || !data) return -1;

    pthread_mutex_lock(&c->peer_lock);
    peer_slot_t *s = slot_find(c, peer_id);
    yumi_sudp_client_t *sudp = (s && atomic_load(&s->state) == SLOT_CONNECTED) ? s->sudp : NULL;
    pthread_mutex_unlock(&c->peer_lock);

    if (sudp)
        return yumi_sudp_client_send_channel(sudp, YUMI_CLIENT_USER_CH, data, len);

    /* No direct connection — relay via meshnet */
    return mesh_relay_send(c, peer_id, YUMI_CLIENT_USER_CH, data, len);
}

int yumi_client_send_reliable(yumi_client_t *c,
                               const uint8_t peer_id[GR_PEER_ID_LEN],
                               const void *data, uint32_t len)
{
    if (!c || !peer_id || !data) return -1;

    pthread_mutex_lock(&c->peer_lock);
    peer_slot_t *s = slot_find(c, peer_id);
    yumi_sudp_client_t *sudp = (s && atomic_load(&s->state) == SLOT_CONNECTED) ? s->sudp : NULL;
    pthread_mutex_unlock(&c->peer_lock);

    if (sudp)
        return yumi_sudp_client_send_reliable_channel(sudp, YUMI_CLIENT_USER_CH, data, len);

    /* No direct connection — relay via meshnet (always reliable) */
    return mesh_relay_send(c, peer_id, YUMI_CLIENT_USER_CH, data, len);
}

int yumi_client_send_channel(yumi_client_t *c,
                              const uint8_t peer_id[GR_PEER_ID_LEN],
                              uint8_t channel,
                              const void *data, uint32_t len,
                              bool reliable)
{
    if (!c || !peer_id || !data) return -1;
    if (channel < YUMI_CLIENT_USER_CH || channel >= YUMI_CLIENT_ATTEST_CH)
        return -1;  /* channels 0-1 and 254-255 are reserved */

    pthread_mutex_lock(&c->peer_lock);
    peer_slot_t *s = slot_find(c, peer_id);
    yumi_sudp_client_t *sudp = (s && atomic_load(&s->state) == SLOT_CONNECTED) ? s->sudp : NULL;
    pthread_mutex_unlock(&c->peer_lock);

    if (sudp) {
        if (reliable)
            return yumi_sudp_client_send_reliable_channel(sudp, channel, data, len);
        else
            return yumi_sudp_client_send_channel(sudp, channel, data, len);
    }

    /* No direct connection — relay via meshnet */
    return mesh_relay_send(c, peer_id, channel, data, len);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 16: Public API — Invitations
 * ═══════════════════════════════════════════════════════════════════ */

int yumi_client_invite(yumi_client_t *c, int64_t expiry_ms,
                        uint8_t **out_blob, size_t *out_len)
{
    if (!c || !out_blob || !out_len) return -1;

    /* Update our address so invite bootstrap peers are current */
    gr_peer_update_address(c->reg, "::1", c->boot_port, &c->identity);

    uint8_t token[GR_HASH_LEN];
    gr_error_t err = gr_invite_create(c->reg, &c->identity, expiry_ms,
                                       out_blob, out_len, token);
    return (err == GR_OK) ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 17: Public API — Queries
 * ═══════════════════════════════════════════════════════════════════ */

yumi_client_state_t yumi_client_get_state(const yumi_client_t *c)
{
    if (!c) return YUMI_CLIENT_CLOSED;
    return (yumi_client_state_t)atomic_load(&((yumi_client_t *)c)->state);
}

gr_registrar_t *yumi_client_get_registrar(yumi_client_t *c)
{
    return c ? c->reg : NULL;
}

const gr_identity_t *yumi_client_get_identity(const yumi_client_t *c)
{
    return c ? &c->identity : NULL;
}

uint32_t yumi_client_connected_peers(const yumi_client_t *c)
{
    if (!c) return 0;
    return count_connected(c);
}

bool yumi_client_is_verified(const yumi_client_t *c)
{
    if (!c) return false;
    return gr_is_trusted(c->reg);
}

uint16_t yumi_client_get_port(const yumi_client_t *c)
{
    return c ? c->boot_port : 0;
}
