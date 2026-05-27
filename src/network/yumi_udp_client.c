/*
 * yumi_udp_client.c - Yumi UDP client implementation: send/recv worker pools, channel-based reliable/unreliable transport, retransmit watchdog, BBR→TDS CC.
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
 * yumi_udp_client.c — Yumi UDP Client: raw transport with channels
 *
 * Decoupled send/recv worker thread pools for pipelined crypto.
 *   - Send workers: drain per-worker MPSC ring, encrypt, sendto.
 *   - Recv workers: recvfrom, decrypt, ACK/reorder/callback.
 *   - Shared state (reliable_table, CC) under state_lock spinlock.
 *   - Per-channel recv state under channel_locks[ch] spinlock.
 *   - Thread-local tl_bufs avoids passing buffers through every call.
 *
 *   - Channels (0-255) encoded as a dedicated byte in the wire header.
 *   - Reliable mode: ACK + retransmit via timerfd watchdog.
 *   - Non-reliable mode: fire-and-forget.
 *   - BBR probes the path, then hands off to TDP for steady-state.
 *   - All buffers and tables are heap-allocated.
 *   - POSIX <pthread.h> worker threads, spinlocks, atomics.
 */

#define _GNU_SOURCE
#include "network/yumi_udp_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/uio.h>

#include <juice/juice.h>
#include <sched.h>    /* sched_yield */
#include <poll.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Constants
 * ════════════════════════════════════════════════════════════════════════ */

#define DEFAULT_RELIABLE_TIMEOUT_US  200000   /* 200 ms */
#define DEFAULT_MAX_RETRANSMITS      5
#define TIMER_INTERVAL_NS            10000000 /* 10 ms */
#define EPOLL_MAX_EVENTS             8

/* IPv6 + UDP header overhead outside our datagram */
#define YUMI_IP6_UDP_OVERHEAD  48   /* 40 (IPv6) + 8 (UDP) */
#define YUMI_MIN_LINK_MTU      1280 /* IPv6 minimum MTU    */

/* ICE pipe writes must be atomic (total <= PIPE_BUF = 4096 on Linux) */
_Static_assert(YUMI_UDP_MAX_DGRAM + 4 <= 4096,
               "ICE pipe write must fit within PIPE_BUF for atomicity");

/* ════════════════════════════════════════════════════════════════════════
 *  Spinlock — lightweight CAS-based, cache-line aligned
 * ════════════════════════════════════════════════════════════════════════ */

static inline void spin_lock(atomic_int *lock)
{
    while (atomic_exchange_explicit(lock, 1, memory_order_acquire))
        while (atomic_load_explicit(lock, memory_order_relaxed))
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield");
#else
            sched_yield();
#endif
}

static inline void spin_unlock(atomic_int *lock)
{
    atomic_store_explicit(lock, 0, memory_order_release);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Lockless CC cache — updated under state_lock, read without locking.
 *  Eliminates the per-pacing-iteration lock that was the #1 contention.
 * ════════════════════════════════════════════════════════════════════════ */

static inline void cc_cache_update(yumi_udp_client_t *c)
{
    atomic_store_explicit(&c->cc_rate_cache,
        yumi_cc_get_send_rate(&c->cc), memory_order_release);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Deferred send — collects retransmit / probe packets to send after
 *  releasing state_lock, so encrypt + sendto never runs under spinlock.
 *
 *  Since the multi-threaded split, ALL outgoing packets from the recv
 *  worker are forwarded to the send worker via the ring.  This ensures
 *  the recv worker does NO encryption, keeping it free to decrypt
 *  incoming packets at full speed.
 * ════════════════════════════════════════════════════════════════════════ */

/* Forward declarations */
static void worker_send_raw(yumi_udp_client_t *c,
                             uint8_t flags, uint8_t channel, uint32_t seq,
                             const struct sockaddr_in6 *dest,
                             const void *payload, uint32_t len);

static void build_header(uint8_t *buf, uint8_t flags, uint8_t channel,
                          uint32_t seq, uint32_t payload_len);

#define DEFERRED_SEND_MAX 32

typedef struct {
    uint8_t              flags;
    uint8_t              channel;
    uint32_t             seq;
    struct sockaddr_in6  dest;
    uint32_t             payload_len;
    uint8_t              payload[YUMI_UDP_MAX_PAYLOAD];
} deferred_send_t;

/* ════════════════════════════════════════════════════════════════════════
 *  Per-worker scratch buffers — thread-local pointer avoids passing
 *  through every internal function.  Set once at thread entry.
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t *send_buf;     /* [YUMI_UDP_MAX_DGRAM]                      */
    uint8_t *crypto_buf;   /* [YUMI_UDP_MAX_DGRAM + crypto_overhead]    */
} yumi_bufs_t;

static _Thread_local yumi_bufs_t *tl_bufs = NULL;

/* ════════════════════════════════════════════════════════════════════════
 *  Worker context types
 * ════════════════════════════════════════════════════════════════════════ */

struct yumi_send_worker {
    yumi_udp_client_t *client;
    uint32_t     id;
    pthread_t    thread;
    yumi_ring_t *ring;           /* this worker's MPSC ring (data)      */
    yumi_ring_t *ack_ring;       /* high-priority ring (ACKs/SACKs)    */
    int          event_fd;       /* eventfd to wake this worker         */
    yumi_bufs_t  bufs;
    /* Per-worker pacing state */
    int64_t      pace_credit;
    uint64_t     pace_last_us;
    _Alignas(64) atomic_int polling; /* 1 = blocked on eventfd read     */
};

struct yumi_recv_worker {
    yumi_udp_client_t *client;
    uint32_t     id;
    pthread_t    thread;
    yumi_bufs_t  bufs;           /* crypto_buf for decrypt only         */
};

/* ────────────────────────────────────────────────────────────────────────
 *  enqueue_raw_send — push a raw (header-described) packet to the send
 *  worker's ack_ring for encrypt+sendto.  Non-blocking: drops if ring
 *  full.  Called from the recv worker (ACKs, SACKs).
 *
 *  When no packet crypto is configured, we send directly from the recv
 *  worker — keeps recv/send paths parallel (ACKs are just header +
 *  sendto, no crypto work to offload).
 * ──────────────────────────────────────────────────────────────────────── */

static void enqueue_raw_send(yumi_udp_client_t *c, uint8_t flags,
                              uint8_t channel, uint32_t seq,
                              const struct sockaddr_in6 *dest,
                              const void *payload, uint32_t payload_len)
{
    /* Fast path: no crypto → send ACK directly from recv worker */
    if (!c->pkt_encrypt) {
        worker_send_raw(c, flags, channel, seq,
                        dest ? dest : &c->peer_addr, payload, payload_len);
        return;
    }

    struct yumi_send_worker *sw = &c->send_wk[0]; /* ACKs always to worker 0 */
    yumi_work_item_t *slot = yumi_ring_try_reserve(sw->ack_ring);
    if (!slot) return;  /* ring full — drop; sender will retransmit */

    slot->type      = YUMI_WORK_SEND_RAW;
    slot->raw_flags = flags;
    slot->channel   = channel;
    slot->raw_seq   = seq;
    slot->len       = payload_len;
    if (dest) {
        slot->has_dest = true;
        slot->dest     = *dest;
    } else {
        slot->has_dest = false;
    }
    if (payload_len > 0 && payload)
        memcpy(slot->data, payload, payload_len);

    yumi_ring_commit(sw->ack_ring, slot);

    /* Wake send worker if it's blocked on its eventfd */
    if (atomic_exchange_explicit(&sw->polling, 0, memory_order_acq_rel)) {
        uint64_t val = 1;
        (void)write(sw->event_fd, &val, sizeof(val));
    }
}

static inline void flush_deferred(yumi_udp_client_t *c,
                                  deferred_send_t *d, int count)
{
    for (int i = 0; i < count; i++)
        worker_send_raw(c, d[i].flags, d[i].channel, d[i].seq,
                        &d[i].dest, d[i].payload_len ? d[i].payload : NULL,
                        d[i].payload_len);
}

/* ════════════════════════════════════════════════════════════════════════
 *  ICE / libjuice callbacks
 *
 *  These run on libjuice's internal thread.  They must not block.
 *  Data received via cb_recv is forwarded through a pipe to the worker
 *  thread's epoll loop, keeping all protocol processing single-threaded.
 * ════════════════════════════════════════════════════════════════════════ */

static yumi_ice_state_t juice_to_yumi_state(juice_state_t s)
{
    switch (s) {
    case JUICE_STATE_DISCONNECTED: return YUMI_ICE_DISCONNECTED;
    case JUICE_STATE_GATHERING:    return YUMI_ICE_GATHERING;
    case JUICE_STATE_CONNECTING:   return YUMI_ICE_CONNECTING;
    case JUICE_STATE_CONNECTED:    return YUMI_ICE_CONNECTED;
    case JUICE_STATE_COMPLETED:    return YUMI_ICE_COMPLETED;
    case JUICE_STATE_FAILED:       return YUMI_ICE_FAILED;
    default:                       return YUMI_ICE_DISCONNECTED;
    }
}

static void ice_on_state_changed(juice_agent_t *agent, juice_state_t state,
                                  void *user_ptr)
{
    (void)agent;
    yumi_udp_client_t *c = (yumi_udp_client_t *)user_ptr;
    yumi_ice_state_t ys = juice_to_yumi_state(state);
    atomic_store_explicit(&c->ice_state, (int)ys, memory_order_release);
    if (c->ice_state_cb)
        c->ice_state_cb(c->ice_user, ys);
}

static void ice_on_candidate(juice_agent_t *agent, const char *sdp,
                              void *user_ptr)
{
    (void)agent;
    yumi_udp_client_t *c = (yumi_udp_client_t *)user_ptr;
    if (c->ice_candidate_cb)
        c->ice_candidate_cb(c->ice_user, sdp);
}

static void ice_on_gathering_done(juice_agent_t *agent, void *user_ptr)
{
    (void)agent;
    yumi_udp_client_t *c = (yumi_udp_client_t *)user_ptr;
    if (c->ice_gathering_done_cb)
        c->ice_gathering_done_cb(c->ice_user);
}

/*
 * ice_on_recv — data arrives from the ICE peer.
 *
 * We write (len_header + data) into the pipe.  The worker thread reads
 * complete messages from the pipe read-end and feeds them into the
 * normal packet processing path (worker_recv_ice).
 *
 * The 4-byte length prefix lets the reader frame variable-length messages
 * on the pipe, which is a byte stream.
 */
static void ice_on_recv(juice_agent_t *agent, const char *data, size_t size,
                         void *user_ptr)
{
    (void)agent;
    yumi_udp_client_t *c = (yumi_udp_client_t *)user_ptr;
    if (size > YUMI_UDP_MAX_DGRAM || c->ice_recv_fd < 0) return;
    uint32_t len = (uint32_t)size;
    /* Write is atomic for sizes <= PIPE_BUF (4096 on Linux).
     * Our max dgram (1452) + 4 < PIPE_BUF, so this is safe. */
    struct iovec iov[2] = {
        { .iov_base = &len,          .iov_len = 4    },
        { .iov_base = (void *)data,  .iov_len = size },
    };
    (void)writev(c->ice_recv_fd, iov, 2);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Time helper
 * ════════════════════════════════════════════════════════════════════════ */

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Retransmit min-heap (binary heap, lazy deletion)
 *
 *  Entries are keyed by expiry time.  On ACK the reliable table entry is
 *  marked inactive; the stale heap entry is skipped when later popped.
 *  This gives O(log n) insert and O(k) drain where k = expired entries,
 *  replacing the previous O(256) linear scan every 10 ms.
 * ════════════════════════════════════════════════════════════════════════ */

static yumi_retx_heap_t *retx_heap_create(int initial_capacity)
{
    yumi_retx_heap_t *h = calloc(1, sizeof(yumi_retx_heap_t));
    if (!h) return NULL;
    h->entries = calloc(initial_capacity, sizeof(yumi_retx_entry_t));
    if (!h->entries) { free(h); return NULL; }
    h->count = 0;
    h->capacity = initial_capacity;
    return h;
}

static void retx_heap_destroy(yumi_retx_heap_t *h)
{
    if (h) { free(h->entries); free(h); }
}

static void retx_heap_sift_up(yumi_retx_heap_t *h, int i)
{
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (h->entries[parent].expiry <= h->entries[i].expiry) break;
        yumi_retx_entry_t tmp = h->entries[parent];
        h->entries[parent] = h->entries[i];
        h->entries[i] = tmp;
        i = parent;
    }
}

static void retx_heap_sift_down(yumi_retx_heap_t *h, int i)
{
    for (;;) {
        int smallest = i;
        int left  = 2 * i + 1;
        int right = 2 * i + 2;
        if (left  < h->count &&
            h->entries[left].expiry  < h->entries[smallest].expiry)
            smallest = left;
        if (right < h->count &&
            h->entries[right].expiry < h->entries[smallest].expiry)
            smallest = right;
        if (smallest == i) break;
        yumi_retx_entry_t tmp = h->entries[smallest];
        h->entries[smallest] = h->entries[i];
        h->entries[i] = tmp;
        i = smallest;
    }
}

static bool retx_heap_push(yumi_retx_heap_t *h, uint64_t expiry,
                            uint32_t table_idx, uint32_t seq, uint8_t flags,
                            uint8_t channel)
{
    if (h->count >= h->capacity) {
        int new_cap = h->capacity * 2;
        yumi_retx_entry_t *grown = realloc(h->entries,
                                           (size_t)new_cap * sizeof(yumi_retx_entry_t));
        if (!grown) return false;
        h->entries = grown;
        h->capacity = new_cap;
    }
    int i = h->count++;
    h->entries[i].expiry    = expiry;
    h->entries[i].table_idx = table_idx;
    h->entries[i].seq       = seq;
    h->entries[i].flags     = flags;
    h->entries[i].channel   = channel;
    retx_heap_sift_up(h, i);
    return true;
}

static bool retx_heap_peek(const yumi_retx_heap_t *h, yumi_retx_entry_t *out)
{
    if (h->count == 0) return false;
    *out = h->entries[0];
    return true;
}

static void retx_heap_pop(yumi_retx_heap_t *h)
{
    if (h->count == 0) return;
    h->count--;
    if (h->count > 0) {
        h->entries[0] = h->entries[h->count];
        retx_heap_sift_down(h, 0);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  MTU discovery from NIC via Linux API
 *
 *  Strategy:
 *    1. connect() the UDP socket to the peer (stateless for UDP, just
 *       lets the kernel resolve the route).
 *    2. getsockopt(IPV6_MTU) — returns the path MTU from the routing
 *       table entry.
 *    3. Fallback: getsockname() to find the local address the kernel
 *       chose, then getifaddrs() + ioctl(SIOCGIFMTU) on the matching
 *       interface.
 *    4. Unconnect the socket (AF_UNSPEC) so sendto() keeps working.
 *    5. Floor at 1280 (IPv6 minimum).
 * ════════════════════════════════════════════════════════════════════════ */

static uint32_t discover_link_mtu(int fd, const struct sockaddr_in6 *peer)
{
    uint32_t mtu = 0;

    /* Connect to let the kernel resolve the outbound route */
    if (connect(fd, (const struct sockaddr *)peer, sizeof(*peer)) < 0)
        return YUMI_MIN_LINK_MTU;

    /* Attempt 1: IPV6_MTU getsockopt (cheapest, most direct) */
    {
        int val = 0;
        socklen_t len = sizeof(val);
        if (getsockopt(fd, IPPROTO_IPV6, IPV6_MTU, &val, &len) == 0
            && val > 0) {
            mtu = (uint32_t)val;
            goto done;
        }
    }

    /* Attempt 2: match local address to interface, read SIOCGIFMTU */
    {
        struct sockaddr_in6 local;
        socklen_t slen = sizeof(local);
        if (getsockname(fd, (struct sockaddr *)&local, &slen) < 0)
            goto done;

        struct ifaddrs *ifap = NULL;
        if (getifaddrs(&ifap) < 0)
            goto done;

        for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET6)
                continue;
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)ifa->ifa_addr;
            if (memcmp(&a->sin6_addr, &local.sin6_addr,
                       sizeof(local.sin6_addr)) == 0) {
                struct ifreq ifr;
                memset(&ifr, 0, sizeof(ifr));
                strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
                if (ioctl(fd, SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu > 0)
                    mtu = (uint32_t)ifr.ifr_mtu;
                break;
            }
        }
        freeifaddrs(ifap);
    }

done:
    /* Unconnect — restores sendto() to arbitrary destinations */
    {
        struct sockaddr_in6 unspec;
        memset(&unspec, 0, sizeof(unspec));
        unspec.sin6_family = AF_UNSPEC;
        connect(fd, (struct sockaddr *)&unspec, sizeof(unspec));
    }

    return mtu >= YUMI_MIN_LINK_MTU ? mtu : YUMI_MIN_LINK_MTU;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Datagram-level crypto helper — encrypt then send to socket/ICE
 * ════════════════════════════════════════════════════════════════════════ */

static ssize_t worker_dgram_out(yumi_udp_client_t *c,
                                 const uint8_t *pkt, uint32_t pkt_len,
                                 const struct sockaddr_in6 *dest)
{
    const uint8_t *out = pkt;
    uint32_t out_len = pkt_len;

    if (c->pkt_encrypt) {
        uint32_t ct_len;
        if (c->pkt_encrypt(c->pkt_crypto_ctx, pkt, pkt_len,
                            tl_bufs->crypto_buf, &ct_len) != 0)
            return -1;
        out = tl_bufs->crypto_buf;
        out_len = ct_len;
    }

    if (c->ice_agent) {
        int r = juice_send(c->ice_agent, (const char *)out, out_len);
        return (r == JUICE_ERR_SUCCESS) ? (ssize_t)out_len : -1;
    }
    return sendto(c->fd, out, out_len, 0,
                  (const struct sockaddr *)dest, sizeof(*dest));
}

/* ════════════════════════════════════════════════════════════════════════
 *  Packet building (heap buffer)
 * ════════════════════════════════════════════════════════════════════════ */

static void build_header(uint8_t *buf, uint8_t flags, uint8_t channel,
                          uint32_t seq, uint32_t payload_len)
{
    yumi_udp_hdr_t *hdr = (yumi_udp_hdr_t *)buf;
    hdr->flags       = flags;
    hdr->channel     = channel;
    hdr->seq         = seq;
    hdr->payload_len = payload_len;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Worker: raw send datagram
 * ════════════════════════════════════════════════════════════════════════ */

static void worker_send_raw(yumi_udp_client_t *c,
                             uint8_t flags, uint8_t channel, uint32_t seq,
                             const struct sockaddr_in6 *dest,
                             const void *payload, uint32_t len)
{
    uint32_t total = YUMI_UDP_HDR_SIZE + len;
    uint8_t *buf = tl_bufs->send_buf;

    build_header(buf, flags, channel, seq, len);
    if (len > 0 && payload)
        memcpy(buf + YUMI_UDP_HDR_SIZE, payload, len);

    ssize_t sent = worker_dgram_out(c, buf, total, dest);
    if (sent > 0) {
        atomic_fetch_add_explicit(&c->stat_tx_packets, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&c->stat_tx_bytes, (uint64_t)sent,
                                  memory_order_relaxed);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Worker: reliable table (no sharing — worker-owned, hash-indexed)
 * ════════════════════════════════════════════════════════════════════════ */

static inline uint32_t reliable_hash(uint8_t channel, uint8_t flags,
                                      uint32_t seq, uint32_t capacity)
{
    bool probe = (flags & YUMI_UDP_FLAG_PROBE) != 0;
    return (seq + (uint32_t)channel * 37U + (probe ? 131U : 0U))
           & (capacity - 1);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Worker: reliable table resize — 2× growth, rehash, rebuild retx heap
 *
 *  Called when worker_reliable_add fails to find a free slot.
 *  Worker-owned — no thread safety concerns.
 * ════════════════════════════════════════════════════════════════════════ */

static bool worker_reliable_resize(yumi_udp_client_t *c)
{
    uint32_t old_cap = c->reliable_capacity;
    uint32_t new_cap = old_cap * 2;

    yumi_reliable_entry_t *new_table = calloc(new_cap,
                                              sizeof(yumi_reliable_entry_t));
    if (!new_table) return false;

    /* Rehash all active entries into the new table */
    for (uint32_t i = 0; i < old_cap; i++) {
        yumi_reliable_entry_t *e = &c->reliable_table[i];
        if (!e->active) continue;
        uint32_t base = reliable_hash(e->channel, e->flags, e->seq, new_cap);
        for (uint32_t j = 0; j < new_cap; j++) {
            uint32_t idx = (base + j) & (new_cap - 1);
            if (!new_table[idx].active) {
                new_table[idx] = *e;
                break;
            }
        }
    }

    /* Rebuild retx_heap from active entries in the new table */
    c->retx_heap->count = 0;
    for (uint32_t i = 0; i < new_cap; i++) {
        yumi_reliable_entry_t *e = &new_table[i];
        if (!e->active) continue;
        uint64_t expiry = e->send_time_us + e->timeout_us;
        retx_heap_push(c->retx_heap, expiry, i, e->seq, e->flags, e->channel);
    }

    explicit_bzero(c->reliable_table,
                   old_cap * sizeof(yumi_reliable_entry_t));
    free(c->reliable_table);
    c->reliable_table = new_table;
    c->reliable_capacity = new_cap;
    return true;
}

static void worker_reliable_add(yumi_udp_client_t *c,
                                 uint8_t flags, uint8_t channel, uint32_t seq,
                                 const struct sockaddr_in6 *dest,
                                 const yumi_packet_t *cc_pkt,
                                 const void *payload, uint32_t len)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t cap = c->reliable_capacity;
        uint32_t base = reliable_hash(channel, flags, seq, cap);
        for (uint32_t i = 0; i < cap; i++) {
            uint32_t idx = (base + i) & (cap - 1);
            yumi_reliable_entry_t *e = &c->reliable_table[idx];
            if (!e->active) {
                uint64_t now        = monotonic_us();
                e->seq              = seq;
                e->send_time_us     = now;
                e->timeout_us       = c->reliable_timeout_us;
                e->retransmit_count = 0;
                e->max_retransmits  = c->max_retransmits;
                e->payload_len      = len;
                e->flags            = flags;
                e->channel          = channel;
                e->active           = true;
                e->cc_pkt           = *cc_pkt;
                e->dest             = *dest;
                if (len > 0)
                    memcpy(e->payload, payload, len);
                retx_heap_push(c->retx_heap, now + c->reliable_timeout_us,
                               idx, seq, flags, channel);
                if (flags & YUMI_UDP_FLAG_PROBE)
                    atomic_fetch_add_explicit(&c->probe_inflight, 1,
                                             memory_order_relaxed);
                return;
            }
        }
        /* First attempt failed — try to grow the table */
        if (attempt == 0 && !worker_reliable_resize(c))
            break;
    }
#ifdef YUMI_DEBUG
    atomic_fetch_add_explicit(&c->stat_reliable_full, 1, memory_order_relaxed);
#endif
}

static void worker_reliable_ack(yumi_udp_client_t *c, uint32_t seq,
                                 uint8_t channel, bool probe)
{
    uint8_t match_flags = YUMI_UDP_FLAG_RELIABLE
                        | (probe ? YUMI_UDP_FLAG_PROBE : 0);
    uint32_t cap = c->reliable_capacity;
    uint32_t base = reliable_hash(channel, match_flags, seq, cap);
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t idx = (base + i) & (cap - 1);
        yumi_reliable_entry_t *e = &c->reliable_table[idx];
        if (!e->active) continue;
        if (e->seq != seq) continue;
        if (e->channel == channel &&
            ((e->flags & YUMI_UDP_FLAG_PROBE) != 0) == probe) {
            e->active = false;
            if (probe)
                atomic_fetch_sub_explicit(&c->probe_inflight, 1,
                                         memory_order_relaxed);

            /* Feed ACK into congestion control with preserved delivery state */
            uint64_t now = monotonic_us();
            uint64_t rtt = now - e->send_time_us;
            yumi_cc_on_ack(&c->cc, &e->cc_pkt, e->cc_pkt.size, rtt, now);
            return;
        }
    }
}

static void worker_reliable_retransmit(yumi_udp_client_t *c,
                                        deferred_send_t *deferred,
                                        int *defer_count)
{
    uint64_t now = monotonic_us();
    yumi_retx_entry_t top;

    while (retx_heap_peek(c->retx_heap, &top) && top.expiry <= now) {
        retx_heap_pop(c->retx_heap);

        yumi_reliable_entry_t *e = &c->reliable_table[top.table_idx];

        /* Stale entry — already ACK’d or replaced */
        if (!e->active || e->seq != top.seq || e->flags != top.flags)
            continue;

        /* send_time may have been pushed forward by a SACK retransmit */
        uint64_t actual_expiry = e->send_time_us + e->timeout_us;
        if (now < actual_expiry) {
            retx_heap_push(c->retx_heap, actual_expiry,
                           top.table_idx, top.seq, top.flags, top.channel);
            continue;
        }

        if (e->retransmit_count >= e->max_retransmits) {
            /* Give up — report loss to CC */
            yumi_cc_on_loss(&c->cc, YUMI_UDP_HDR_SIZE + e->payload_len);
            if (e->flags & YUMI_UDP_FLAG_PROBE)
                atomic_fetch_sub_explicit(&c->probe_inflight, 1,
                                         memory_order_relaxed);
            e->active = false;
            continue;
        }

        /* Need a deferred slot to actually retransmit — stop if full.
         * Entry stays in the heap, will be retried on next timer tick. */
        if (*defer_count >= DEFERRED_SEND_MAX) {
            /* Re-push what we popped (entry was already popped above) */
            retx_heap_push(c->retx_heap, top.expiry,
                           top.table_idx, top.seq, top.flags, top.channel);
            break;
        }

        deferred_send_t *d = &deferred[*defer_count];
        d->flags       = e->flags;
        d->channel     = e->channel;
        d->seq         = e->seq;
        d->dest        = e->dest;
        d->payload_len = e->payload_len;
        if (e->payload_len > 0)
            memcpy(d->payload, e->payload, e->payload_len);
        (*defer_count)++;

        e->send_time_us = now;
        e->retransmit_count++;
        atomic_fetch_add_explicit(&c->stat_retransmits, 1,
                                  memory_order_relaxed);

        /* Schedule next retransmit check */
        retx_heap_push(c->retx_heap, now + e->timeout_us,
                       top.table_idx, top.seq, top.flags, top.channel);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Worker: CC probe — empty reliable sends to feed BBR during PROBE
 *
 *  BBR needs ACK feedback to measure BtlBw and RTprop. If user traffic
 *  is mostly unreliable (which produces no ACKs), PROBE starves and
 *  discovers a tiny rate.  We fix this by sending zero-payload reliable
 *  packets during PROBE mode on each timer tick (every 10 ms).  These
 *  generate ACKs from the peer which feed straight into on_ack.  Once
 *  BBR graduates to TDS, we stop — the discovered rate is established.
 *
 *  Cap at 4 in-flight probes to avoid flooding the reliable table and
 *  generating excessive retransmit overhead when no peer responds.
 * ════════════════════════════════════════════════════════════════════════ */

#define CC_PROBE_MAX_INFLIGHT 4

static void worker_cc_probe(yumi_udp_client_t *c,
                             deferred_send_t *deferred,
                             int *defer_count)
{
    if (yumi_cc_get_mode(&c->cc) != YUMI_MODE_PROBE)
        return;

    /* O(1) check via atomic counter */
    int inflight = atomic_load_explicit(&c->probe_inflight,
                                        memory_order_relaxed);
    if (inflight >= CC_PROBE_MAX_INFLIGHT)
        return;

    /* Must have room to actually send — skip entirely if full */
    if (*defer_count >= DEFERRED_SEND_MAX)
        return;

    uint32_t seq = atomic_fetch_add_explicit(&c->probe_seq, 1,
                                             memory_order_relaxed);
    uint8_t flags = YUMI_UDP_FLAG_RELIABLE | YUMI_UDP_FLAG_PROBE;

    deferred_send_t *d = &deferred[*defer_count];
    d->flags       = flags;
    d->channel     = 0;
    d->seq         = seq;
    d->dest        = c->peer_addr;
    d->payload_len = 0;
    (*defer_count)++;

    yumi_packet_t cc_pkt = {
        .seq  = seq,
        .size = YUMI_UDP_HDR_SIZE,
    };
    yumi_cc_on_send(&c->cc, &cc_pkt, monotonic_us());
    worker_reliable_add(c, flags, 0, seq, &c->peer_addr, &cc_pkt, NULL, 0);
}

/* (Old worker_process_item removed — replaced by send_worker_process_item) */

/* ════════════════════════════════════════════════════════════════════════
 *  Worker: ACK / SACK helpers
 *
 *  SACK (Selective ACK) provides gap recovery.  When the receiver buffers
 *  an out-of-order reliable packet, it replies with a SACK instead of a
 *  plain ACK.  The SACK payload (8 bytes) carries:
 *    - uint32_t cumulative : recv_rel_next[ch] (everything below received)
 *    - uint32_t bitmap     : bit i = have seq (cumulative + i)
 *  The sender parses the bitmap and retransmits missing seqs immediately,
 *  cutting recovery from timeout_us to one RTT.
 * ════════════════════════════════════════════════════════════════════════ */



static yumi_reliable_entry_t *worker_reliable_find(yumi_udp_client_t *c,
                                                     uint8_t channel,
                                                     uint32_t seq)
{
    uint8_t flags = YUMI_UDP_FLAG_RELIABLE;
    uint32_t cap = c->reliable_capacity;
    uint32_t base = reliable_hash(channel, flags, seq, cap);
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t idx = (base + i) & (cap - 1);
        yumi_reliable_entry_t *e = &c->reliable_table[idx];
        if (!e->active) continue;
        if (e->seq != seq) continue;
        if (e->channel == channel && !(e->flags & YUMI_UDP_FLAG_PROBE))
            return e;
    }
    return NULL;
}

static void worker_handle_sack(yumi_udp_client_t *c,
                                const uint8_t *payload,
                                uint32_t payload_len,
                                uint8_t channel,
                                deferred_send_t *deferred,
                                int *defer_count)
{
    if (payload_len < 8) return;

    uint32_t cumulative, bitmap;
    memcpy(&cumulative, payload, 4);
    memcpy(&bitmap, payload + 4, 4);

    uint64_t now = monotonic_us();

    for (uint32_t i = 0; i < YUMI_REORDER_WINDOW; i++) {
        if (bitmap & (1U << i)) continue;   /* receiver has this seq */
        uint32_t missing_seq = cumulative + i;
        yumi_reliable_entry_t *e = worker_reliable_find(c, channel,
                                                         missing_seq);
        if (!e) continue;

        /* Debounce: skip if recently sent (by timeout or previous SACK) */
        if (now - e->send_time_us < e->timeout_us / 2) continue;

        /* Defer retransmit — skip if buffer full (next SACK will retry) */
        if (*defer_count >= DEFERRED_SEND_MAX)
            continue;

        deferred_send_t *d = &deferred[*defer_count];
        d->flags       = e->flags;
        d->channel     = e->channel;
        d->seq         = e->seq;
        d->dest        = e->dest;
        d->payload_len = e->payload_len;
        if (e->payload_len > 0)
            memcpy(d->payload, e->payload, e->payload_len);
        (*defer_count)++;

        e->send_time_us = now;
        atomic_fetch_add_explicit(&c->stat_retransmits, 1,
                                  memory_order_relaxed);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Worker: receive path — auto-ACK/SACK, per-channel dedup, reorder
 * ════════════════════════════════════════════════════════════════════════ */

static void worker_reorder_drain(yumi_udp_client_t *c, uint8_t ch)
{
    for (;;) {
        uint32_t next = c->recv_rel_next[ch];
        uint32_t slot_idx = (uint32_t)ch * YUMI_REORDER_WINDOW
                          + (next & YUMI_REORDER_MASK);
        yumi_reorder_slot_t *rs = &c->reorder_buf[slot_idx];
        if (!rs->occupied || rs->seq != next)
            break;
        const void *data = rs->len > 0 ? rs->data : NULL;
        c->recv_cb(c->recv_user, data, rs->len, true, rs->seq);
        rs->occupied = false;
        c->recv_rel_next[ch] = next + 1;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  ICE pipe receiver — reads framed messages from the pipe written by
 *  ice_on_recv.  Each message is (uint32_t len)(payload[len]).
 * ════════════════════════════════════════════════════════════════════════ */

static void worker_recv_ice(yumi_udp_client_t *c, uint8_t *buf)
{
    for (;;) {
        /* Phase 1: accumulate 4-byte length header */
        while (c->ice_pipe_hdr_pos < 4) {
            ssize_t r = read(c->ice_pipe_rd,
                             c->ice_pipe_hdr + c->ice_pipe_hdr_pos,
                             4 - c->ice_pipe_hdr_pos);
            if (r <= 0) return;
            c->ice_pipe_hdr_pos += (uint8_t)r;
        }

        uint32_t len;
        memcpy(&len, c->ice_pipe_hdr, 4);
        if (len > YUMI_UDP_MAX_DGRAM) { /* corrupt — drain and reset */
            uint8_t discard[256];
            while (read(c->ice_pipe_rd, discard, sizeof(discard)) > 0) {}
            c->ice_pipe_hdr_pos = 0;
            c->ice_pipe_msg_pos = 0;
            return;
        }

        /* Phase 2: accumulate payload */
        while (c->ice_pipe_msg_pos < len) {
            ssize_t r = read(c->ice_pipe_rd,
                             buf + c->ice_pipe_msg_pos,
                             len - c->ice_pipe_msg_pos);
            if (r <= 0) return;
            c->ice_pipe_msg_pos += (uint32_t)r;
        }

        /* Complete message — reset carry state */
        ssize_t nr = (ssize_t)len;
        c->ice_pipe_hdr_pos = 0;
        c->ice_pipe_msg_pos = 0;

        /* Datagram-level decrypt (if enabled) */
        if (c->pkt_decrypt) {
            uint32_t pt_len;
            if (c->pkt_decrypt(c->pkt_crypto_ctx, buf, (uint32_t)nr,
                                tl_bufs->crypto_buf, &pt_len) != 0)
                continue; /* auth failure — drop */
            memcpy(buf, tl_bufs->crypto_buf, pt_len);
            nr = (ssize_t)pt_len;
        }

        if ((size_t)nr < YUMI_UDP_HDR_SIZE) continue;

        const yumi_udp_hdr_t *hdr = (const yumi_udp_hdr_t *)buf;

        atomic_fetch_add_explicit(&c->stat_rx_packets, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&c->stat_rx_bytes, (uint64_t)nr,
                                  memory_order_relaxed);

        /* ACK / SACK */
        if (hdr->flags & YUMI_UDP_FLAG_ACK) {
            uint8_t ack_ch = hdr->channel;
            bool ack_probe = (hdr->flags & YUMI_UDP_FLAG_PROBE) != 0;
            deferred_send_t ice_deferred[DEFERRED_SEND_MAX];
            int ice_defer_n = 0;
            spin_lock(&c->state_lock);
            worker_reliable_ack(c, hdr->seq, ack_ch, ack_probe);
            if (!ack_probe) {
                uint32_t actual_pl = (uint32_t)((size_t)nr - YUMI_UDP_HDR_SIZE);
                uint32_t sack_len  = hdr->payload_len <= actual_pl
                                   ? hdr->payload_len : actual_pl;
                if (sack_len >= 8)
                    worker_handle_sack(c, buf + YUMI_UDP_HDR_SIZE,
                                       sack_len, ack_ch,
                                       ice_deferred, &ice_defer_n);
            }
            cc_cache_update(c);
            spin_unlock(&c->state_lock);
            flush_deferred(c, ice_deferred, ice_defer_n);
            continue;
        }

        /* CC probe */
        if (hdr->flags & YUMI_UDP_FLAG_PROBE) {
            if (hdr->flags & YUMI_UDP_FLAG_RELIABLE)
                enqueue_raw_send(c, YUMI_UDP_FLAG_ACK | YUMI_UDP_FLAG_PROBE,
                                 hdr->channel, hdr->seq, NULL, NULL, 0);
            continue;
        }

        /* Data packet */
        if (!c->recv_cb) continue;
        uint8_t ch = hdr->channel;
        if (ch >= YUMI_UDP_MAX_CHANNELS) continue;

        uint32_t seq = hdr->seq;
        bool reliable = (hdr->flags & YUMI_UDP_FLAG_RELIABLE) != 0;
        uint32_t actual_payload = (uint32_t)((size_t)nr - YUMI_UDP_HDR_SIZE);
        uint32_t safe_len = hdr->payload_len <= actual_payload
                          ? hdr->payload_len : actual_payload;

        uint8_t ack_flags = YUMI_UDP_FLAG_ACK;

        spin_lock(&c->channel_locks[ch]);
        if (reliable) {
            if (!c->recv_rel_started[ch]) {
                c->recv_rel_started[ch] = true;
                c->recv_rel_next[ch]    = seq;
            }
            int32_t dist = (int32_t)(seq - c->recv_rel_next[ch]);
            if (dist < 0) {
                spin_unlock(&c->channel_locks[ch]);
                enqueue_raw_send(c, ack_flags, ch, seq, NULL, NULL, 0);
                continue;
            }
            if (dist == 0) {
                const void *payload = safe_len > 0
                                    ? buf + YUMI_UDP_HDR_SIZE : NULL;
                c->recv_cb(c->recv_user, payload, safe_len, true, seq);
                c->recv_rel_next[ch] = seq + 1;
                worker_reorder_drain(c, ch);
                spin_unlock(&c->channel_locks[ch]);
                enqueue_raw_send(c, ack_flags, ch, seq, NULL, NULL, 0);
            } else if ((uint32_t)dist < YUMI_REORDER_WINDOW) {
                uint32_t slot_idx = (uint32_t)ch * YUMI_REORDER_WINDOW
                              + (seq & YUMI_REORDER_MASK);
                yumi_reorder_slot_t *rs = &c->reorder_buf[slot_idx];
                if (!rs->occupied) {
                    rs->occupied = true;
                    rs->seq = seq;
                    rs->len = safe_len;
                    if (safe_len > 0)
                        memcpy(rs->data, buf + YUMI_UDP_HDR_SIZE, safe_len);
                }
                spin_unlock(&c->channel_locks[ch]);
                {
                    uint32_t cumulative = c->recv_rel_next[ch];
                    uint32_t bitmap = 0;
                    for (uint32_t si = 0; si < YUMI_REORDER_WINDOW; si++) {
                        uint32_t ck = cumulative + si;
                        uint32_t sidx = (uint32_t)ch * YUMI_REORDER_WINDOW
                                      + (ck & YUMI_REORDER_MASK);
                        yumi_reorder_slot_t *rsl = &c->reorder_buf[sidx];
                        if (rsl->occupied && rsl->seq == ck)
                            bitmap |= (1U << si);
                    }
                    uint8_t sack_data[8];
                    memcpy(sack_data, &cumulative, 4);
                    memcpy(sack_data + 4, &bitmap, 4);
                    enqueue_raw_send(c, YUMI_UDP_FLAG_ACK, hdr->channel,
                                     hdr->seq, NULL, sack_data, 8);
                }
            } else {
                spin_unlock(&c->channel_locks[ch]);
            }
            /* else: too far ahead — drop */
        } else {
            if (!c->recv_unrel_started[ch]) {
                c->recv_unrel_started[ch] = true;
                c->recv_unrel_next[ch]    = seq;
            }
            int32_t dist = (int32_t)(seq - c->recv_unrel_next[ch]);
            if (dist < 0) {
                spin_unlock(&c->channel_locks[ch]);
                continue;
            }
            c->recv_unrel_next[ch] = seq + 1;
            const void *payload = safe_len > 0
                                ? buf + YUMI_UDP_HDR_SIZE : NULL;
            spin_unlock(&c->channel_locks[ch]);
            c->recv_cb(c->recv_user, payload, safe_len, false, seq);
        }
    }
}

static void worker_recv(yumi_udp_client_t *c, uint8_t *buf)
{
    struct sockaddr_in6 from;
    socklen_t fromlen;

    for (;;) {
        fromlen = sizeof(from);
        ssize_t n = recvfrom(c->fd, buf, YUMI_UDP_MAX_DGRAM, MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) break;

        /* Datagram-level decrypt (if enabled) */
        const uint8_t *pkt = buf;
        if (c->pkt_decrypt) {
            uint32_t pt_len;
            if (c->pkt_decrypt(c->pkt_crypto_ctx, buf, (uint32_t)n,
                                tl_bufs->crypto_buf, &pt_len) != 0)
                continue; /* auth failure — drop */
            pkt = tl_bufs->crypto_buf;
            n = (ssize_t)pt_len;
        }

        if ((size_t)n < YUMI_UDP_HDR_SIZE) continue;

        const yumi_udp_hdr_t *hdr = (const yumi_udp_hdr_t *)pkt;

        atomic_fetch_add_explicit(&c->stat_rx_packets, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&c->stat_rx_bytes, (uint64_t)n,
                                  memory_order_relaxed);

        /* ACK / SACK — complete a reliable send */
        if (hdr->flags & YUMI_UDP_FLAG_ACK) {
            uint8_t ack_ch = hdr->channel;
            bool ack_probe = (hdr->flags & YUMI_UDP_FLAG_PROBE) != 0;
            deferred_send_t recv_deferred[DEFERRED_SEND_MAX];
            int recv_defer_n = 0;
            spin_lock(&c->state_lock);
            worker_reliable_ack(c, hdr->seq, ack_ch, ack_probe);

            /* SACK: ACK with payload >= 8 bytes carries gap info */
            if (!ack_probe) {
                uint32_t actual_pl = (uint32_t)((size_t)n - YUMI_UDP_HDR_SIZE);
                uint32_t sack_len  = hdr->payload_len <= actual_pl
                                   ? hdr->payload_len : actual_pl;
                if (sack_len >= 8)
                    worker_handle_sack(c, pkt + YUMI_UDP_HDR_SIZE,
                                       sack_len, ack_ch,
                                       recv_deferred, &recv_defer_n);
            }
            cc_cache_update(c);
            spin_unlock(&c->state_lock);
            flush_deferred(c, recv_deferred, recv_defer_n);
            continue;
        }

        /* CC probe — ACK it, don't deliver to user */
        if (hdr->flags & YUMI_UDP_FLAG_PROBE) {
            if (hdr->flags & YUMI_UDP_FLAG_RELIABLE)
                enqueue_raw_send(c, YUMI_UDP_FLAG_ACK | YUMI_UDP_FLAG_PROBE,
                                 hdr->channel, hdr->seq, &from, NULL, 0);
            continue;
        }

        /* Data packet — per-channel dedup + reliable reorder */
        if (!c->recv_cb) continue;

        uint8_t ch = hdr->channel;
        if (ch >= YUMI_UDP_MAX_CHANNELS) continue;

        uint32_t seq = hdr->seq;
        bool reliable = (hdr->flags & YUMI_UDP_FLAG_RELIABLE) != 0;

        /* Clamp payload_len to actual received bytes */
        uint32_t actual_payload = (uint32_t)((size_t)n - YUMI_UDP_HDR_SIZE);
        uint32_t safe_len = hdr->payload_len <= actual_payload
                          ? hdr->payload_len : actual_payload;

        uint8_t ack_flags = YUMI_UDP_FLAG_ACK;

        spin_lock(&c->channel_locks[ch]);
        if (reliable) {
            /* Reliable ordered delivery with reorder buffer */
            if (!c->recv_rel_started[ch]) {
                c->recv_rel_started[ch] = true;
                c->recv_rel_next[ch]    = seq;
            }
            int32_t dist = (int32_t)(seq - c->recv_rel_next[ch]);

            if (dist < 0) {
                /* Duplicate — still ACK (sender may have missed ours) */
                spin_unlock(&c->channel_locks[ch]);
                enqueue_raw_send(c, ack_flags, ch, hdr->seq, &from, NULL, 0);
                continue;
            }
            if (dist == 0) {
                /* In-order — deliver, drain reorder buffer, plain ACK */
                const void *payload = safe_len > 0
                                      ? pkt + YUMI_UDP_HDR_SIZE : NULL;
                c->recv_cb(c->recv_user, payload, safe_len, true, seq);
                c->recv_rel_next[ch] = seq + 1;
                worker_reorder_drain(c, ch);
                spin_unlock(&c->channel_locks[ch]);
                enqueue_raw_send(c, ack_flags, ch, hdr->seq, &from, NULL, 0);
            } else if ((uint32_t)dist < YUMI_REORDER_WINDOW) {
                /* Out-of-order — buffer and send SACK */
                uint32_t slot_idx = (uint32_t)ch * YUMI_REORDER_WINDOW
                                  + (seq & YUMI_REORDER_MASK);
                yumi_reorder_slot_t *rs = &c->reorder_buf[slot_idx];
                if (!rs->occupied) {
                    rs->occupied = true;
                    rs->seq      = seq;
                    rs->len      = safe_len;
                    if (safe_len > 0)
                        memcpy(rs->data, pkt + YUMI_UDP_HDR_SIZE, safe_len);
                }
                spin_unlock(&c->channel_locks[ch]);
                {
                    uint32_t cumulative = c->recv_rel_next[ch];
                    uint32_t bitmap = 0;
                    for (uint32_t si = 0; si < YUMI_REORDER_WINDOW; si++) {
                        uint32_t ck = cumulative + si;
                        uint32_t sidx = (uint32_t)ch * YUMI_REORDER_WINDOW
                                      + (ck & YUMI_REORDER_MASK);
                        yumi_reorder_slot_t *rsl = &c->reorder_buf[sidx];
                        if (rsl->occupied && rsl->seq == ck)
                            bitmap |= (1U << si);
                    }
                    uint8_t sack_data[8];
                    memcpy(sack_data, &cumulative, 4);
                    memcpy(sack_data + 4, &bitmap, 4);
                    enqueue_raw_send(c, YUMI_UDP_FLAG_ACK, hdr->channel,
                                     hdr->seq, &from, sack_data, 8);
                }
            } else {
                spin_unlock(&c->channel_locks[ch]);
            }
            /* else: too far ahead — drop */
        } else {
            /* Unreliable: deliver immediately, advance past gap */
            if (!c->recv_unrel_started[ch]) {
                c->recv_unrel_started[ch] = true;
                c->recv_unrel_next[ch]    = seq;
            }
            int32_t dist = (int32_t)(seq - c->recv_unrel_next[ch]);
            if (dist < 0) {
                spin_unlock(&c->channel_locks[ch]);
                continue;
            }
            c->recv_unrel_next[ch] = seq + 1;
            const void *payload = safe_len > 0
                                  ? pkt + YUMI_UDP_HDR_SIZE : NULL;
            spin_unlock(&c->channel_locks[ch]);
            c->recv_cb(c->recv_user, payload, safe_len, false, seq);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Send worker: pacing governor (per-worker, token-bucket)
 * ════════════════════════════════════════════════════════════════════════ */

static void send_worker_pace_refill(struct yumi_send_worker *sw,
                                     uint64_t rate)
{
    uint64_t now = monotonic_us();
    uint64_t elapsed = now - sw->pace_last_us;
    if (elapsed == 0) return;
    sw->pace_last_us = now;

    if (rate == 0) {
        sw->pace_credit = (int64_t)(YUMI_UDP_MAX_DGRAM * 64);
        return;
    }

    int64_t added = (int64_t)((double)rate * (double)elapsed / 1e6);
    sw->pace_credit += added;

    int64_t cap = (int64_t)(rate * 2);
    int64_t pkt_cap = (int64_t)(YUMI_UDP_MAX_DGRAM * 64);
    if (cap < pkt_cap) cap = pkt_cap;
    if (sw->pace_credit > cap)
        sw->pace_credit = cap;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Send worker: process a work item from the ring
 * ════════════════════════════════════════════════════════════════════════ */

static void send_worker_process_item(yumi_udp_client_t *c,
                                      struct yumi_send_worker *sw,
                                      const yumi_work_item_t *item)
{
    /* Per-channel, per-mode sequence number (atomic, no lock) */
    uint32_t seq;
    if (item->type == YUMI_WORK_SEND_RELIABLE)
        seq = atomic_fetch_add_explicit(&c->rel_seq[item->channel], 1,
                                         memory_order_relaxed);
    else
        seq = atomic_fetch_add_explicit(&c->unrel_seq[item->channel], 1,
                                         memory_order_relaxed);

    uint8_t flags = 0;
    if (item->type == YUMI_WORK_SEND_RELIABLE)
        flags |= YUMI_UDP_FLAG_RELIABLE;

    uint32_t dgram_size = YUMI_UDP_HDR_SIZE + item->len;
    sw->pace_credit -= (int64_t)dgram_size;

    const struct sockaddr_in6 *dest = item->has_dest
                                    ? &item->dest : &c->peer_addr;

    /* Build + encrypt + send (per-worker buffers via tl_bufs, no lock) */
    worker_send_raw(c, flags, item->channel, seq, dest, item->data, item->len);

    /* CC + reliable table (shared state, needs lock) */
    if (item->type == YUMI_WORK_SEND_RELIABLE) {
        yumi_packet_t cc_pkt = {
            .seq  = seq,
            .size = dgram_size,
        };
        spin_lock(&c->state_lock);
        yumi_cc_on_send(&c->cc, &cc_pkt, monotonic_us());
        worker_reliable_add(c, flags, item->channel, seq, dest, &cc_pkt,
                            item->data, item->len);
        cc_cache_update(c);
        spin_unlock(&c->state_lock);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Send worker thread — drains its own MPSC ring, encrypts, sends
 * ════════════════════════════════════════════════════════════════════════ */

static void *send_worker_thread(void *arg)
{
    struct yumi_send_worker *sw = (struct yumi_send_worker *)arg;
    yumi_udp_client_t *c = sw->client;

    tl_bufs = &sw->bufs;

    while (atomic_load_explicit(&c->running, memory_order_acquire)) {
        /* ── Drain rings: ack_ring first (priority), then data ── */
        bool did_work = false;

        /* ── 1. Drain all pending ACKs/SACKs (no pacing) ──────── */
        {
            const yumi_work_item_t *item;
            while ((item = yumi_ring_peek(sw->ack_ring)) != NULL) {
                const struct sockaddr_in6 *dest = item->has_dest
                    ? &item->dest : &c->peer_addr;
                worker_send_raw(c, item->raw_flags, item->channel,
                                item->raw_seq, dest,
                                item->len > 0 ? item->data : NULL,
                                item->len);
                yumi_ring_advance(sw->ack_ring);
                did_work = true;
            }
        }

        /* ── 2. Drain data ring with pacing ────────────────────── */
        for (int retry = 0; retry < 32; retry++) {
            uint64_t send_rate;

            /* Lockless CC reads — updated atomically under state_lock */
            send_rate = atomic_load_explicit(
                            &c->cc_rate_cache, memory_order_acquire);

            /* Always use pacing — even during PROBE.  BBR's pacing_rate
             * starts at a reasonable initial estimate and ramps up via
             * pacing_gain during STARTUP.  Unlimited credit was safe in
             * the old single-threaded model (send/recv serialized), but
             * with separate send/recv workers the send side overwhelms
             * the recv side when crypto is involved (~5 µs per packet).
             * pace_refill handles rate==0 by giving large credit, so
             * bootstrap before BBR's first RTT sample is fine. */
            send_worker_pace_refill(sw, send_rate);

            bool drained_any = false;
            const yumi_work_item_t *item;
            while ((item = yumi_ring_peek(sw->ring)) != NULL) {
                if (item->type == YUMI_WORK_SHUTDOWN)
                    goto done;
                if (item->type == YUMI_WORK_USER_CB) {
                    if (c->user_work_cb)
                        c->user_work_cb(c->user_work_ctx,
                                        item->data, item->len);
                    yumi_ring_advance(sw->ring);
                    continue;
                }
                /* Data items require pacing credit */
                if (sw->pace_credit <= 0) break;
                send_worker_process_item(c, sw, item);
                yumi_ring_advance(sw->ring);
                drained_any = true;
                did_work = true;
                if (sw->pace_credit <= 0) break;
            }

            if (!drained_any)
                break;
        }

        if (!did_work) {
            /* Block on eventfd until producer wakes us */
            atomic_store_explicit(&sw->polling, 1, memory_order_release);
            uint64_t val;
            (void)read(sw->event_fd, &val, sizeof(val));
            atomic_store_explicit(&sw->polling, 0, memory_order_relaxed);
        }
    }

done:
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Recv worker thread — recvfrom, decrypts, dispatches ACK/data
 *
 *  Worker 0: epoll (socket + timer + ICE pipe + shutdown eventfd).
 *  Workers 1+: tight recvfrom loop on the shared socket fd.
 * ════════════════════════════════════════════════════════════════════════ */

static void *recv_worker_thread(void *arg)
{
    struct yumi_recv_worker *rw = (struct yumi_recv_worker *)arg;
    yumi_udp_client_t *c = rw->client;

    tl_bufs = &rw->bufs;

    uint8_t *recv_buf = malloc(YUMI_UDP_MAX_DGRAM);
    struct epoll_event *events = NULL;

    if (rw->id == 0) {
        events = malloc(EPOLL_MAX_EVENTS * sizeof(struct epoll_event));
        if (!events) { free(recv_buf); return NULL; }
    }
    if (!recv_buf) { free(events); return NULL; }

    while (atomic_load_explicit(&c->running, memory_order_acquire)) {
        if (rw->id == 0) {
            /* ── Primary recv worker: epoll-driven ─────────────── */
            int nfds = epoll_wait(c->epoll_fd, events, EPOLL_MAX_EVENTS, 1);

            for (int i = 0; i < nfds; i++) {
                int evfd = events[i].data.fd;
                if (evfd == c->fd) {
                    worker_recv(c, recv_buf);
                } else if (evfd == c->recv_event_fd) {
                    uint64_t val;
                    (void)read(c->recv_event_fd, &val, sizeof(val));
                } else if (evfd == c->ice_pipe_rd && c->ice_pipe_rd >= 0) {
                    worker_recv_ice(c, recv_buf);
                } else if (evfd == c->timer_fd) {
                    uint64_t expirations;
                    (void)read(c->timer_fd, &expirations, sizeof(expirations));

                    deferred_send_t tmr_deferred[DEFERRED_SEND_MAX];
                    int tmr_defer_n = 0;
                    spin_lock(&c->state_lock);
                    worker_reliable_retransmit(c, tmr_deferred, &tmr_defer_n);
                    worker_cc_probe(c, tmr_deferred, &tmr_defer_n);
                    cc_cache_update(c);
                    spin_unlock(&c->state_lock);
                    flush_deferred(c, tmr_deferred, tmr_defer_n);
                }
            }
        } else {
            /* ── Secondary recv worker: direct recvfrom ────────── */
            worker_recv(c, recv_buf);
        }
    }

    explicit_bzero(recv_buf, YUMI_UDP_MAX_DGRAM);
    free(recv_buf);
    free(events);
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Enqueue helper — producer side (any thread)
 *
 *  Round-robins across send workers' rings.  Each send worker is the
 *  sole consumer of its ring (MPSC — multiple producers, single consumer).
 * ════════════════════════════════════════════════════════════════════════ */

static _Alignas(64) atomic_uint_fast64_t g_send_rr = 0;

static int enqueue_work(yumi_udp_client_t *c, yumi_work_type_t type,
                         uint8_t channel, const struct sockaddr_in6 *dest,
                         const void *data, uint32_t len)
{
    if (len > c->max_payload) return -1;

    /* Round-robin across send workers */
    uint_fast64_t rr = atomic_fetch_add_explicit(&g_send_rr, 1,
                                                  memory_order_relaxed);
    uint32_t wid = (uint32_t)(rr % c->num_send_workers);
    struct yumi_send_worker *sw = &c->send_wk[wid];

    yumi_work_item_t *slot = yumi_ring_reserve(sw->ring);
    if (!slot) return -1;

    slot->type    = type;
    slot->len     = len;
    slot->channel = channel;
    if (dest) {
        slot->has_dest = true;
        slot->dest     = *dest;
    } else {
        slot->has_dest = false;
    }
    if (len > 0 && data)
        memcpy(slot->data, data, len);

    yumi_ring_commit(sw->ring, slot);

    /* Wake send worker if it's blocked on its eventfd */
    if (atomic_exchange_explicit(&sw->polling, 0,
                                  memory_order_acq_rel)) {
        uint64_t val = 1;
        (void)write(sw->event_fd, &val, sizeof(val));
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — create / destroy
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_udp_client_create(yumi_udp_client_t *c,
                            const yumi_udp_client_config_t *cfg)
{
    /* Sentinel fd values */
    c->fd            = -1;
    c->epoll_fd      = -1;
    c->timer_fd      = -1;
    c->recv_event_fd = -1;
    c->ice_agent     = NULL;
    c->ice_recv_fd   = -1;
    c->ice_pipe_rd   = -1;

    /* Worker pool counts (default: 1 each) */
    c->num_send_workers = cfg->num_send_workers > 0 ? cfg->num_send_workers : 1;
    c->num_recv_workers = cfg->num_recv_workers > 0 ? cfg->num_recv_workers : 1;
    c->send_wk = NULL;
    c->recv_wk = NULL;

    c->peer_addr = cfg->peer_addr;

    c->reliable_timeout_us = cfg->reliable_timeout_us > 0
                             ? cfg->reliable_timeout_us
                             : DEFAULT_RELIABLE_TIMEOUT_US;
    c->max_retransmits     = cfg->max_retransmits > 0
                             ? cfg->max_retransmits
                             : DEFAULT_MAX_RETRANSMITS;

    c->recv_cb   = cfg->recv_cb;
    c->recv_user = cfg->recv_user;

    /* ICE callback config */
    c->ice_state_cb          = cfg->ice_state_cb;
    c->ice_candidate_cb      = cfg->ice_candidate_cb;
    c->ice_gathering_done_cb = cfg->ice_gathering_done_cb;
    c->ice_user              = cfg->ice_user;
    atomic_store(&c->ice_state, (int)YUMI_ICE_DISCONNECTED);

    /* Datagram-level crypto hooks — MANDATORY */
    c->pkt_encrypt        = cfg->pkt_encrypt;
    c->pkt_decrypt        = cfg->pkt_decrypt;
    c->pkt_crypto_ctx     = cfg->pkt_crypto_ctx;
    c->pkt_crypto_overhead = cfg->pkt_crypto_overhead;

    if (!c->pkt_encrypt || !c->pkt_decrypt) {
        fprintf(stderr,
                "\033[1;31m[FATAL] This software is not secure. "
                "The cryptographic hooks aren't established in UDP Client.\033[0m\n");
        exit(1);
        return -1;
    }

    /* Worker-thread user callback */
    c->user_work_cb   = cfg->user_work_cb;
    c->user_work_ctx  = cfg->user_work_ctx;

    /* ── Spinlocks ─────────────────────────────────────────────── */

    atomic_store_explicit(&c->state_lock, 0, memory_order_relaxed);

    c->channel_locks = calloc(YUMI_UDP_MAX_CHANNELS, sizeof(atomic_int));
    if (!c->channel_locks) goto fail;
    for (int i = 0; i < YUMI_UDP_MAX_CHANNELS; i++)
        atomic_store_explicit(&c->channel_locks[i], 0, memory_order_relaxed);

    /* ── Heap allocations ──────────────────────────────────────── */

    c->reliable_capacity = YUMI_RELIABLE_INIT_SLOTS;
    c->reliable_table = calloc(YUMI_RELIABLE_INIT_SLOTS,
                                sizeof(yumi_reliable_entry_t));
    if (!c->reliable_table) goto fail;

    c->retx_heap = retx_heap_create(YUMI_RELIABLE_INIT_SLOTS * 4);
    if (!c->retx_heap) goto fail;

    c->recv_rel_next = calloc(YUMI_UDP_MAX_CHANNELS, sizeof(uint32_t));
    if (!c->recv_rel_next) goto fail;

    c->recv_rel_started = calloc(YUMI_UDP_MAX_CHANNELS, sizeof(bool));
    if (!c->recv_rel_started) goto fail;

    c->recv_unrel_next = calloc(YUMI_UDP_MAX_CHANNELS, sizeof(uint32_t));
    if (!c->recv_unrel_next) goto fail;

    c->recv_unrel_started = calloc(YUMI_UDP_MAX_CHANNELS, sizeof(bool));
    if (!c->recv_unrel_started) goto fail;

    c->reorder_buf = calloc((size_t)YUMI_UDP_MAX_CHANNELS * YUMI_REORDER_WINDOW,
                            sizeof(yumi_reorder_slot_t));
    if (!c->reorder_buf) goto fail;

    c->rel_seq = calloc(YUMI_UDP_MAX_CHANNELS, sizeof(atomic_uint));
    if (!c->rel_seq) goto fail;

    c->unrel_seq = calloc(YUMI_UDP_MAX_CHANNELS, sizeof(atomic_uint));
    if (!c->unrel_seq) goto fail;

    /* ── Atomics ───────────────────────────────────────────────── */

    atomic_store(&c->stat_tx_packets,  0);
    atomic_store(&c->stat_tx_bytes,    0);
    atomic_store(&c->stat_rx_packets,  0);
    atomic_store(&c->stat_rx_bytes,    0);
    atomic_store(&c->stat_retransmits, 0);
#ifdef YUMI_DEBUG
    atomic_store(&c->stat_reliable_full, 0);
#endif
    atomic_store(&c->probe_seq, 0);
    atomic_store(&c->probe_inflight, 0);
    for (int i = 0; i < YUMI_UDP_MAX_CHANNELS; i++) {
        atomic_store(&c->rel_seq[i], 0);
        atomic_store(&c->unrel_seq[i], 0);
    }

    /* ── Transport: ICE (libjuice) or raw UDP socket ─────────── */

    if (cfg->stun_server) {
        /* ── ICE mode ──────────────────────────────────────────── */
        int pipefd[2];
        if (pipe2(pipefd, O_NONBLOCK) < 0)
            goto fail;
        c->ice_pipe_rd = pipefd[0];
        c->ice_recv_fd = pipefd[1];
        c->ice_pipe_hdr_pos = 0;
        c->ice_pipe_msg_pos = 0;

        juice_config_t jcfg;
        memset(&jcfg, 0, sizeof(jcfg));
        jcfg.stun_server_host = cfg->stun_server;
        jcfg.stun_server_port = cfg->stun_port ? cfg->stun_port : 3478;

        /* TURN servers */
        juice_turn_server_t *jturns = NULL;
        if (cfg->turn_servers_count > 0 && cfg->turn_servers) {
            jturns = calloc((size_t)cfg->turn_servers_count,
                            sizeof(juice_turn_server_t));
            if (!jturns) goto fail;
            for (int i = 0; i < cfg->turn_servers_count; i++) {
                jturns[i].host     = cfg->turn_servers[i].host;
                jturns[i].port     = cfg->turn_servers[i].port;
                jturns[i].username = cfg->turn_servers[i].username;
                jturns[i].password = cfg->turn_servers[i].password;
            }
            jcfg.turn_servers       = jturns;
            jcfg.turn_servers_count = cfg->turn_servers_count;
        }

        jcfg.cb_state_changed   = ice_on_state_changed;
        jcfg.cb_candidate       = ice_on_candidate;
        jcfg.cb_gathering_done  = ice_on_gathering_done;
        jcfg.cb_recv            = ice_on_recv;
        jcfg.user_ptr           = c;

        c->ice_agent = juice_create(&jcfg);
        free(jturns);
        if (!c->ice_agent) goto fail;

        juice_gather_candidates(c->ice_agent);

        memset(&c->peer_addr, 0, sizeof(c->peer_addr));

        c->link_mtu    = YUMI_MIN_LINK_MTU;
        c->max_payload = YUMI_UDP_MAX_PAYLOAD;
        {
            uint32_t overhead = YUMI_IP6_UDP_OVERHEAD + YUMI_UDP_HDR_SIZE
                              + c->pkt_crypto_overhead;
            uint32_t mtu_pay = c->link_mtu > overhead
                             ? c->link_mtu - overhead : 0;
            if (mtu_pay < c->max_payload)
                c->max_payload = mtu_pay;
        }
    } else {
        /* ── Raw UDP mode ──────────────────────────────────────── */
        c->fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (c->fd < 0) goto fail;

        int off = 0;
        setsockopt(c->fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

        {
            int bufsz = 4 * 1024 * 1024;
            setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
            setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
        }

        struct sockaddr_in6 local = {0};
        local.sin6_family = AF_INET6;
        local.sin6_port   = htons(cfg->local_port);
        local.sin6_addr   = in6addr_any;
        if (bind(c->fd, (struct sockaddr *)&local, sizeof(local)) < 0)
            goto fail;

        {
            int pmtu = IPV6_PMTUDISC_DO;
            setsockopt(c->fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER,
                       &pmtu, sizeof(pmtu));
        }

        c->link_mtu = discover_link_mtu(c->fd, &c->peer_addr);
        {
            uint32_t overhead = YUMI_IP6_UDP_OVERHEAD + YUMI_UDP_HDR_SIZE
                              + c->pkt_crypto_overhead;
            c->max_payload = c->link_mtu > overhead
                           ? c->link_mtu - overhead : 0;
            if (c->max_payload > YUMI_UDP_MAX_PAYLOAD)
                c->max_payload = YUMI_UDP_MAX_PAYLOAD;
        }
    }

    if (c->pkt_crypto_overhead > 0) {
        uint32_t wire_cap = (YUMI_UDP_MAX_DGRAM > YUMI_UDP_HDR_SIZE + c->pkt_crypto_overhead)
                          ? YUMI_UDP_MAX_DGRAM - YUMI_UDP_HDR_SIZE - c->pkt_crypto_overhead
                          : 0;
        if (c->max_payload > wire_cap)
            c->max_payload = wire_cap;
    }

    /* ── Congestion control (BBR probe → TDS cruise) ───────────── */

    yumi_cc_init(&c->cc, YUMI_UDP_HDR_SIZE + c->max_payload);
    cc_cache_update(c);

    /* ── epoll (used by recv worker 0) ─────────────────────────── */

    c->epoll_fd = epoll_create1(0);
    if (c->epoll_fd < 0) goto fail;

    /* ── recv_event_fd (shutdown wake for recv worker 0) ────────── */

    c->recv_event_fd = eventfd(0, EFD_NONBLOCK);
    if (c->recv_event_fd < 0) goto fail;

    /* ── timerfd (retransmit watchdog, non-blocking) ───────────── */

    c->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (c->timer_fd < 0) goto fail;

    {
        struct itimerspec its = {
            .it_interval = { .tv_sec = 0, .tv_nsec = TIMER_INTERVAL_NS },
            .it_value    = { .tv_sec = 0, .tv_nsec = TIMER_INTERVAL_NS },
        };
        timerfd_settime(c->timer_fd, 0, &its, NULL);
    }

    /* ── Register fds with epoll (recv worker 0) ───────────────── */

    struct epoll_event ev;

    if (c->ice_agent) {
        ev.events  = EPOLLIN;
        ev.data.fd = c->ice_pipe_rd;
        epoll_ctl(c->epoll_fd, EPOLL_CTL_ADD, c->ice_pipe_rd, &ev);
    } else {
        ev.events  = EPOLLIN;
        ev.data.fd = c->fd;
        epoll_ctl(c->epoll_fd, EPOLL_CTL_ADD, c->fd, &ev);
    }

    ev.events  = EPOLLIN;
    ev.data.fd = c->recv_event_fd;
    epoll_ctl(c->epoll_fd, EPOLL_CTL_ADD, c->recv_event_fd, &ev);

    ev.events  = EPOLLIN;
    ev.data.fd = c->timer_fd;
    epoll_ctl(c->epoll_fd, EPOLL_CTL_ADD, c->timer_fd, &ev);

    /* ── Allocate worker pools ─────────────────────────────────── */

    uint32_t crypto_bufsz = YUMI_UDP_MAX_DGRAM + c->pkt_crypto_overhead;

    c->send_wk = calloc(c->num_send_workers, sizeof(struct yumi_send_worker));
    if (!c->send_wk) goto fail;

    for (uint32_t i = 0; i < c->num_send_workers; i++) {
        struct yumi_send_worker *sw = &c->send_wk[i];
        sw->client       = c;
        sw->id           = i;
        sw->event_fd     = -1;
        sw->bufs.send_buf  = malloc(YUMI_UDP_MAX_DGRAM);
        sw->bufs.crypto_buf = malloc(crypto_bufsz);
        sw->ring         = yumi_ring_create();
        sw->ack_ring     = yumi_ring_create();
        sw->event_fd     = eventfd(0, EFD_NONBLOCK);
        sw->pace_credit  = (int64_t)(YUMI_UDP_MAX_DGRAM * 64);
        sw->pace_last_us = 0;
        atomic_store_explicit(&sw->polling, 0, memory_order_relaxed);
        if (!sw->bufs.send_buf || !sw->bufs.crypto_buf ||
            !sw->ring || !sw->ack_ring || sw->event_fd < 0)
            goto fail;
    }

    c->recv_wk = calloc(c->num_recv_workers, sizeof(struct yumi_recv_worker));
    if (!c->recv_wk) goto fail;

    for (uint32_t i = 0; i < c->num_recv_workers; i++) {
        struct yumi_recv_worker *rw = &c->recv_wk[i];
        rw->client          = c;
        rw->id              = i;
        rw->bufs.send_buf   = malloc(YUMI_UDP_MAX_DGRAM);
        rw->bufs.crypto_buf = malloc(crypto_bufsz);
        if (!rw->bufs.send_buf || !rw->bufs.crypto_buf)
            goto fail;
    }

    /* ── Start worker threads ──────────────────────────────────── */

    atomic_store(&c->running, true);

    for (uint32_t i = 0; i < c->num_send_workers; i++) {
        struct yumi_send_worker *sw = &c->send_wk[i];
        sw->pace_last_us = monotonic_us();
        if (pthread_create(&sw->thread, NULL, send_worker_thread, sw) != 0)
            goto fail_threads;
    }

    for (uint32_t i = 0; i < c->num_recv_workers; i++) {
        struct yumi_recv_worker *rw = &c->recv_wk[i];
        if (pthread_create(&rw->thread, NULL, recv_worker_thread, rw) != 0)
            goto fail_threads;
    }

    return 0;

fail_threads:
    atomic_store_explicit(&c->running, false, memory_order_release);
    /* Wake all workers so they see the shutdown flag */
    for (uint32_t i = 0; i < c->num_send_workers; i++) {
        if (c->send_wk[i].event_fd >= 0) {
            uint64_t val = 1;
            (void)write(c->send_wk[i].event_fd, &val, sizeof(val));
        }
    }
    if (c->recv_event_fd >= 0) {
        uint64_t val = 1;
        (void)write(c->recv_event_fd, &val, sizeof(val));
    }
    /* Join any threads that started successfully */
    for (uint32_t i = 0; i < c->num_send_workers; i++) {
        if (c->send_wk[i].thread)
            pthread_join(c->send_wk[i].thread, NULL);
    }
    for (uint32_t i = 0; i < c->num_recv_workers; i++) {
        if (c->recv_wk[i].thread)
            pthread_join(c->recv_wk[i].thread, NULL);
    }
    /* fall through to fail */

fail:
    if (c->ice_agent)     { juice_destroy(c->ice_agent); c->ice_agent = NULL; }
    if (c->ice_recv_fd >= 0) { close(c->ice_recv_fd); c->ice_recv_fd = -1; }
    if (c->ice_pipe_rd >= 0) { close(c->ice_pipe_rd); c->ice_pipe_rd = -1; }
    if (c->fd >= 0)       { close(c->fd);       c->fd       = -1; }
    if (c->epoll_fd >= 0) { close(c->epoll_fd); c->epoll_fd = -1; }
    if (c->timer_fd >= 0) { close(c->timer_fd); c->timer_fd = -1; }
    if (c->recv_event_fd >= 0) { close(c->recv_event_fd); c->recv_event_fd = -1; }
    /* Destroy send worker resources */
    if (c->send_wk) {
        for (uint32_t i = 0; i < c->num_send_workers; i++) {
            struct yumi_send_worker *sw = &c->send_wk[i];
            if (sw->event_fd >= 0) close(sw->event_fd);
            if (sw->ring) yumi_ring_destroy(sw->ring);
            if (sw->ack_ring) yumi_ring_destroy(sw->ack_ring);
            free(sw->bufs.send_buf);
            free(sw->bufs.crypto_buf);
        }
        free(c->send_wk); c->send_wk = NULL;
    }
    if (c->recv_wk) {
        for (uint32_t i = 0; i < c->num_recv_workers; i++) {
            struct yumi_recv_worker *rw = &c->recv_wk[i];
            free(rw->bufs.send_buf);
            free(rw->bufs.crypto_buf);
        }
        free(c->recv_wk); c->recv_wk = NULL;
    }
    if (c->reliable_table) { free(c->reliable_table); c->reliable_table = NULL; }
    if (c->retx_heap)      { retx_heap_destroy(c->retx_heap); c->retx_heap = NULL; }
    free(c->channel_locks); c->channel_locks = NULL;
    free(c->recv_rel_next); c->recv_rel_next = NULL;
    free(c->recv_rel_started); c->recv_rel_started = NULL;
    free(c->recv_unrel_next); c->recv_unrel_next = NULL;
    free(c->recv_unrel_started); c->recv_unrel_started = NULL;
    free(c->reorder_buf); c->reorder_buf = NULL;
    free(c->rel_seq); c->rel_seq = NULL;
    free(c->unrel_seq); c->unrel_seq = NULL;
    return -1;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Free per-worker resources (called from destroy)
 * ════════════════════════════════════════════════════════════════════════ */

static void free_worker_pools(yumi_udp_client_t *c)
{
    if (c->send_wk) {
        for (uint32_t i = 0; i < c->num_send_workers; i++) {
            struct yumi_send_worker *sw = &c->send_wk[i];
            if (sw->event_fd >= 0) close(sw->event_fd);
            if (sw->ring) yumi_ring_destroy(sw->ring);
            if (sw->ack_ring) yumi_ring_destroy(sw->ack_ring);
            if (sw->bufs.send_buf) {
                explicit_bzero(sw->bufs.send_buf, YUMI_UDP_MAX_DGRAM);
                free(sw->bufs.send_buf);
            }
            if (sw->bufs.crypto_buf) {
                explicit_bzero(sw->bufs.crypto_buf,
                               YUMI_UDP_MAX_DGRAM + c->pkt_crypto_overhead);
                free(sw->bufs.crypto_buf);
            }
        }
        free(c->send_wk);
        c->send_wk = NULL;
    }
    if (c->recv_wk) {
        for (uint32_t i = 0; i < c->num_recv_workers; i++) {
            struct yumi_recv_worker *rw = &c->recv_wk[i];
            if (rw->bufs.send_buf) {
                explicit_bzero(rw->bufs.send_buf, YUMI_UDP_MAX_DGRAM);
                free(rw->bufs.send_buf);
            }
            if (rw->bufs.crypto_buf) {
                explicit_bzero(rw->bufs.crypto_buf,
                               YUMI_UDP_MAX_DGRAM + c->pkt_crypto_overhead);
                free(rw->bufs.crypto_buf);
            }
        }
        free(c->recv_wk);
        c->recv_wk = NULL;
    }
}

void yumi_udp_client_destroy(yumi_udp_client_t *c)
{
    /* Signal shutdown */
    atomic_store_explicit(&c->running, false, memory_order_release);

    /* Push a shutdown sentinel into each send worker's ring */
    for (uint32_t i = 0; i < c->num_send_workers; i++) {
        struct yumi_send_worker *sw = &c->send_wk[i];
        yumi_work_item_t *slot = yumi_ring_reserve(sw->ring);
        if (slot) {
            slot->type = YUMI_WORK_SHUTDOWN;
            yumi_ring_commit(sw->ring, slot);
        }
        /* Wake the send worker */
        uint64_t val = 1;
        (void)write(sw->event_fd, &val, sizeof(val));
    }

    /* Wake recv worker 0 via eventfd */
    {
        uint64_t val = 1;
        (void)write(c->recv_event_fd, &val, sizeof(val));
    }

    /* Join all workers */
    for (uint32_t i = 0; i < c->num_send_workers; i++)
        pthread_join(c->send_wk[i].thread, NULL);
    for (uint32_t i = 0; i < c->num_recv_workers; i++)
        pthread_join(c->recv_wk[i].thread, NULL);

    /* Close fds */
    if (c->ice_agent)     { juice_destroy(c->ice_agent); c->ice_agent = NULL; }
    if (c->ice_recv_fd >= 0) { close(c->ice_recv_fd); c->ice_recv_fd = -1; }
    if (c->ice_pipe_rd >= 0) { close(c->ice_pipe_rd); c->ice_pipe_rd = -1; }
    if (c->fd >= 0)       { close(c->fd);       c->fd       = -1; }
    if (c->epoll_fd >= 0) { close(c->epoll_fd); c->epoll_fd = -1; }
    if (c->timer_fd >= 0) { close(c->timer_fd); c->timer_fd = -1; }
    if (c->recv_event_fd >= 0) { close(c->recv_event_fd); c->recv_event_fd = -1; }

    /* Free worker pools (rings, per-worker buffers) */
    free_worker_pools(c);

    /* Free shared heap resources */
    if (c->reliable_table) {
        explicit_bzero(c->reliable_table,
                       c->reliable_capacity * sizeof(yumi_reliable_entry_t));
        free(c->reliable_table);
        c->reliable_table = NULL;
    }
    retx_heap_destroy(c->retx_heap);
    c->retx_heap = NULL;

    free(c->channel_locks);
    c->channel_locks = NULL;
    free(c->recv_rel_next);
    c->recv_rel_next = NULL;
    free(c->recv_rel_started);
    c->recv_rel_started = NULL;
    free(c->recv_unrel_next);
    c->recv_unrel_next = NULL;
    free(c->recv_unrel_started);
    c->recv_unrel_started = NULL;

    if (c->reorder_buf) {
        explicit_bzero(c->reorder_buf,
                       (size_t)YUMI_UDP_MAX_CHANNELS * YUMI_REORDER_WINDOW
                       * sizeof(yumi_reorder_slot_t));
        free(c->reorder_buf);
        c->reorder_buf = NULL;
    }
    free(c->rel_seq);
    c->rel_seq = NULL;
    free(c->unrel_seq);
    c->unrel_seq = NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — send (default channel 0)
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_udp_client_send(yumi_udp_client_t *c,
                          const void *data, uint32_t len)
{
    return enqueue_work(c, YUMI_WORK_SEND_UNRELIABLE, 0, NULL, data, len);
}

int yumi_udp_client_send_reliable(yumi_udp_client_t *c,
                                   const void *data, uint32_t len)
{
    return enqueue_work(c, YUMI_WORK_SEND_RELIABLE, 0, NULL, data, len);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — send (explicit channel)
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_udp_client_send_channel(yumi_udp_client_t *c, uint8_t channel,
                                  const void *data, uint32_t len)
{
    if (channel >= YUMI_UDP_MAX_CHANNELS) return -1;
    return enqueue_work(c, YUMI_WORK_SEND_UNRELIABLE, channel, NULL, data, len);
}

int yumi_udp_client_send_reliable_channel(yumi_udp_client_t *c,
                                           uint8_t channel,
                                           const void *data, uint32_t len)
{
    if (channel >= YUMI_UDP_MAX_CHANNELS) return -1;
    return enqueue_work(c, YUMI_WORK_SEND_RELIABLE, channel, NULL, data, len);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — send to specific destination (multi-peer)
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_udp_client_send_to(yumi_udp_client_t *c,
                             const struct sockaddr_in6 *dest,
                             const void *data, uint32_t len)
{
    if (c->ice_agent) return -1; /* ICE has a single negotiated peer */
    return enqueue_work(c, YUMI_WORK_SEND_UNRELIABLE, 0, dest, data, len);
}

int yumi_udp_client_send_reliable_to(yumi_udp_client_t *c,
                                      const struct sockaddr_in6 *dest,
                                      const void *data, uint32_t len)
{
    if (c->ice_agent) return -1; /* ICE has a single negotiated peer */
    return enqueue_work(c, YUMI_WORK_SEND_RELIABLE, 0, dest, data, len);
}

int yumi_udp_client_send_channel_to(yumi_udp_client_t *c,
                                     uint8_t channel,
                                     const struct sockaddr_in6 *dest,
                                     const void *data, uint32_t len)
{
    if (c->ice_agent) return -1; /* ICE has a single negotiated peer */
    if (channel >= YUMI_UDP_MAX_CHANNELS) return -1;
    return enqueue_work(c, YUMI_WORK_SEND_UNRELIABLE, channel, dest, data, len);
}

int yumi_udp_client_send_reliable_channel_to(yumi_udp_client_t *c,
                                              uint8_t channel,
                                              const struct sockaddr_in6 *dest,
                                              const void *data, uint32_t len)
{
    if (c->ice_agent) return -1; /* ICE has a single negotiated peer */
    if (channel >= YUMI_UDP_MAX_CHANNELS) return -1;
    return enqueue_work(c, YUMI_WORK_SEND_RELIABLE, channel, dest, data, len);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — post a user-callback work item to the worker ring
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_udp_client_post_user(yumi_udp_client_t *c,
                               const void *data, uint32_t len)
{
    if (len > YUMI_UDP_MAX_PAYLOAD) return -1;

    /* Route user work items to send worker 0's ring */
    struct yumi_send_worker *sw = &c->send_wk[0];

    yumi_work_item_t *slot = yumi_ring_reserve(sw->ring);
    if (!slot) return -1;

    slot->type    = YUMI_WORK_USER_CB;
    slot->len     = len;
    slot->channel = 0;
    slot->has_dest = false;
    if (len > 0 && data)
        memcpy(slot->data, data, len);

    yumi_ring_commit(sw->ring, slot);

    if (atomic_exchange_explicit(&sw->polling, 0,
                                  memory_order_acq_rel)) {
        uint64_t val = 1;
        (void)write(sw->event_fd, &val, sizeof(val));
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — stats (lockless atomic reads)
 * ════════════════════════════════════════════════════════════════════════ */

uint64_t yumi_udp_client_stat_tx_packets(const yumi_udp_client_t *c)
{
    return atomic_load_explicit(
        (atomic_uint_fast64_t *)&c->stat_tx_packets, memory_order_relaxed);
}

uint64_t yumi_udp_client_stat_tx_bytes(const yumi_udp_client_t *c)
{
    return atomic_load_explicit(
        (atomic_uint_fast64_t *)&c->stat_tx_bytes, memory_order_relaxed);
}

uint64_t yumi_udp_client_stat_rx_packets(const yumi_udp_client_t *c)
{
    return atomic_load_explicit(
        (atomic_uint_fast64_t *)&c->stat_rx_packets, memory_order_relaxed);
}

uint64_t yumi_udp_client_stat_rx_bytes(const yumi_udp_client_t *c)
{
    return atomic_load_explicit(
        (atomic_uint_fast64_t *)&c->stat_rx_bytes, memory_order_relaxed);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — MTU queries
 * ════════════════════════════════════════════════════════════════════════ */

uint32_t yumi_udp_client_get_mtu(const yumi_udp_client_t *c)
{
    return c->link_mtu;
}

uint32_t yumi_udp_client_get_max_payload(const yumi_udp_client_t *c)
{
    return c->max_payload;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — ICE (libjuice wrappers)
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_udp_client_ice_get_local_sdp(yumi_udp_client_t *c,
                                       char *buf, size_t size)
{
    if (!c->ice_agent) return -1;
    return juice_get_local_description(c->ice_agent, buf, size);
}

int yumi_udp_client_ice_set_remote_sdp(yumi_udp_client_t *c,
                                        const char *sdp)
{
    if (!c->ice_agent) return -1;
    return juice_set_remote_description(c->ice_agent, sdp);
}

int yumi_udp_client_ice_add_remote_candidate(yumi_udp_client_t *c,
                                              const char *sdp)
{
    if (!c->ice_agent) return -1;
    return juice_add_remote_candidate(c->ice_agent, sdp);
}

int yumi_udp_client_ice_set_remote_gathering_done(yumi_udp_client_t *c)
{
    if (!c->ice_agent) return -1;
    return juice_set_remote_gathering_done(c->ice_agent);
}

yumi_ice_state_t yumi_udp_client_ice_get_state(const yumi_udp_client_t *c)
{
    return (yumi_ice_state_t)atomic_load_explicit(
        (atomic_int *)&c->ice_state, memory_order_acquire);
}

bool yumi_udp_client_ice_enabled(const yumi_udp_client_t *c)
{
    return c->ice_agent != NULL;
}
