/*
 * test_udp_session.c - 5-second continual UDP session stress test: 4 unreliable producers + 1 reliable producer, ACK responder, throughput and CC validation.
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
 * test_udp_session.c — 5-second continual session stress test
 *
 * Client A → peer socket (ACK responder) on loopback, running 5 seconds:
 *   - 4 producer threads blast unreliable data on channels 0-3
 *   - 1 producer thread sends reliable data on channel 4
 *   - ACK responder replies to reliable packets
 *   - After 5 s: validate throughput, reliable delivery,
 *     CC state, stat monotonicity, and retransmit sanity.
 */

#define _GNU_SOURCE
#include "network/yumi_udp_client.h"
#include "network/net.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* ── Test harness ──────────────────────────────────────────────── */

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                        \
    do {                                                              \
        g_tests_run++;                                                \
        if (!(cond)) {                                                \
            g_tests_failed++;                                         \
            fprintf(stderr, "  FAIL [%s:%d] %s\n",                   \
                    __FILE__, __LINE__, msg);                         \
        }                                                             \
    } while (0)

#define TEST_SECTION(name)                                            \
    fprintf(stdout, "── %s\n", name)

/* ── Time helpers ──────────────────────────────────────────────── */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── Loopback helpers ──────────────────────────────────────────── */

static struct sockaddr_in6 make_loopback(uint16_t port)
{
    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(port);
    addr.sin6_addr   = in6addr_loopback;
    return addr;
}

static yumi_udp_client_t *client_alloc(void)
{
    yumi_udp_client_t *c = calloc(1, sizeof(yumi_udp_client_t));
    if (!c) { fprintf(stderr, "FATAL: calloc failed\n"); abort(); }
    return c;
}

static uint16_t bound_port(int fd)
{
    struct sockaddr_in6 bound;
    socklen_t blen = sizeof(bound);
    getsockname(fd, (struct sockaddr *)&bound, &blen);
    return ntohs(bound.sin6_port);
}

/* ── Passthrough (identity) crypto hooks for testing ──────────── */

static int passthrough_encrypt(void *ctx, const uint8_t *pt, uint32_t pt_len,
                               uint8_t *ct, uint32_t *ct_len)
{
    (void)ctx;
    memcpy(ct, pt, pt_len);
    *ct_len = pt_len;
    return 0;
}

static int passthrough_decrypt(void *ctx, const uint8_t *ct, uint32_t ct_len,
                               uint8_t *pt, uint32_t *pt_len)
{
    (void)ctx;
    memcpy(pt, ct, ct_len);
    *pt_len = ct_len;
    return 0;
}

static void cfg_set_passthrough_crypto(yumi_udp_client_config_t *cfg)
{
    cfg->pkt_encrypt        = passthrough_encrypt;
    cfg->pkt_decrypt        = passthrough_decrypt;
    cfg->pkt_crypto_ctx     = NULL;
    cfg->pkt_crypto_overhead = 0;
}

/* ── ACK responder thread (sends ACKs back for reliable packets) ─ */

typedef struct {
    int         fd;
    atomic_bool stop;
    atomic_uint_fast64_t acks_sent;
} ack_responder_t;

static void *ack_responder_fn(void *arg)
{
    ack_responder_t *r = (ack_responder_t *)arg;
    uint8_t *buf = malloc(YUMI_UDP_MAX_DGRAM);
    if (!buf) return NULL;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
    setsockopt(r->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!atomic_load_explicit(&r->stop, memory_order_relaxed)) {
        struct sockaddr_in6 from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(r->fd, buf, YUMI_UDP_MAX_DGRAM, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;
        if ((size_t)n < YUMI_UDP_HDR_SIZE) continue;

        const yumi_udp_hdr_t *hdr = (const yumi_udp_hdr_t *)buf;
        if (hdr->flags & YUMI_UDP_FLAG_ACK) continue;  /* don't ack acks */

        /* Only ACK reliable packets */
        if (hdr->flags & YUMI_UDP_FLAG_RELIABLE) {
            uint8_t ack_buf[YUMI_UDP_HDR_SIZE];
            yumi_udp_hdr_t *ack = (yumi_udp_hdr_t *)ack_buf;
            ack->flags       = YUMI_UDP_FLAG_ACK
                             | (hdr->flags & YUMI_UDP_FLAG_PROBE);
            ack->channel     = hdr->channel;
            ack->seq         = hdr->seq;
            ack->payload_len = 0;

            sendto(r->fd, ack_buf, YUMI_UDP_HDR_SIZE, 0,
                   (struct sockaddr *)&from, fromlen);
            atomic_fetch_add_explicit(&r->acks_sent, 1, memory_order_relaxed);
        }
    }

    free(buf);
    return NULL;
}

/* ── Unreliable sender thread ──────────────────────────────────── */

typedef struct {
    yumi_udp_client_t   *client;
    uint8_t              channel;
    atomic_bool         *stop;
    atomic_uint_fast64_t sent;
    atomic_uint_fast64_t failed;
} sender_args_t;

static void *unreliable_sender_fn(void *arg)
{
    sender_args_t *a = (sender_args_t *)arg;
    uint16_t counter = 0;
    uint32_t max_payload = yumi_udp_client_get_max_payload(a->client);
    uint32_t payload_sz = max_payload < 128 ? max_payload : 128;
    if (payload_sz < 4) payload_sz = 4;

    uint8_t *payload = malloc(payload_sz);
    if (!payload) return NULL;

    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        payload[0] = a->channel;
        payload[1] = (uint8_t)(counter & 0xFF);
        payload[2] = (uint8_t)(counter & 0xFF);  /* fill byte */
        memset(payload + 3, (uint8_t)(counter & 0xFF), payload_sz - 3);

        int rc = yumi_udp_client_send_channel(
            a->client, a->channel, payload, payload_sz);
        if (rc == 0) {
            atomic_fetch_add_explicit(&a->sent, 1, memory_order_relaxed);
            /* Brief yield */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000 }; /* 0.1 ms */
            nanosleep(&ts, NULL);
        } else {
            atomic_fetch_add_explicit(&a->failed, 1, memory_order_relaxed);
            /* Back off on ring-full — avoids spinning at 100% */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
            nanosleep(&ts, NULL);
        }

        counter++;
    }

    free(payload);
    return NULL;
}

/* ── Reliable sender thread ────────────────────────────────────── */

static void *reliable_sender_fn(void *arg)
{
    sender_args_t *a = (sender_args_t *)arg;
    uint16_t counter = 0;
    uint8_t payload[64];

    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        payload[0] = a->channel;
        payload[1] = (uint8_t)(counter & 0xFF);
        payload[2] = (uint8_t)(counter & 0xFF);
        memset(payload + 3, (uint8_t)(counter & 0xFF), sizeof(payload) - 3);

        int rc = yumi_udp_client_send_reliable_channel(
            a->client, a->channel, payload, sizeof(payload));
        if (rc == 0)
            atomic_fetch_add_explicit(&a->sent, 1, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&a->failed, 1, memory_order_relaxed);

        counter++;
        /* Slower cadence — reliable has retransmit overhead */
        sleep_ms(5);
    }

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  15-second continual session test
 * ═══════════════════════════════════════════════════════════════════ */

static void test_5s_session(void)
{
    TEST_SECTION("3-second continual session");

    const int SESSION_SECONDS = 3;

    /* ── Peer socket (raw, for ACK responder) ──────────────────── */

    int peer_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    TEST_ASSERT(peer_fd >= 0, "peer socket created");
    {
        int off = 0;
        setsockopt(peer_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 addr = make_loopback(0);
        int rc = bind(peer_fd, (struct sockaddr *)&addr, sizeof(addr));
        TEST_ASSERT(rc == 0, "peer socket bound");
    }
    uint16_t peer_port = bound_port(peer_fd);

    /* ── ACK responder ─────────────────────────────────────────── */

    ack_responder_t *resp = calloc(1, sizeof(ack_responder_t));
    TEST_ASSERT(resp != NULL, "ack responder allocated");
    resp->fd = peer_fd;
    atomic_store(&resp->stop, false);
    atomic_store(&resp->acks_sent, 0);

    pthread_t resp_thrd;
    TEST_ASSERT(pthread_create(&resp_thrd, NULL, ack_responder_fn, resp) == 0,
                "ACK responder thread started");

    /* ── Client A (sender) ─────────────────────────────────────── */

    yumi_udp_client_config_t cfg_a = {0};
    cfg_a.peer_addr           = make_loopback(peer_port);
    cfg_a.reliable_timeout_us = 50000;  /* 50 ms for faster retransmit in test */
    cfg_a.max_retransmits     = 5;
    cfg_set_passthrough_crypto(&cfg_a);

    yumi_udp_client_t *client_a = client_alloc();
    int rc = yumi_udp_client_create(client_a, &cfg_a);
    TEST_ASSERT(rc == 0, "client A created");

    /* ── Validate MTU discovery ────────────────────────────────── */

    uint32_t mtu_a = yumi_udp_client_get_mtu(client_a);
    uint32_t max_pl = yumi_udp_client_get_max_payload(client_a);
    TEST_ASSERT(mtu_a >= 1280, "MTU >= IPv6 minimum 1280");
    TEST_ASSERT(max_pl > 0, "max_payload > 0");
    TEST_ASSERT(max_pl <= YUMI_UDP_MAX_PAYLOAD, "max_payload <= compile-time max");
    fprintf(stdout, "    MTU=%u  max_payload=%u\n", mtu_a, max_pl);

    /* ── Launch sender threads ─────────────────────────────────── */

    #define NUM_UNRELIABLE_SENDERS 4
    #define NUM_RELIABLE_SENDERS   1
    #define TOTAL_SENDERS (NUM_UNRELIABLE_SENDERS + NUM_RELIABLE_SENDERS)

    atomic_bool stop_flag = false;

    sender_args_t *senders = calloc(TOTAL_SENDERS, sizeof(sender_args_t));
    pthread_t *sender_thrds = calloc(TOTAL_SENDERS, sizeof(pthread_t));
    TEST_ASSERT(senders && sender_thrds, "sender data allocated");

    /* Unreliable senders on channels 0-3 */
    for (int i = 0; i < NUM_UNRELIABLE_SENDERS; i++) {
        senders[i].client  = client_a;
        senders[i].channel = (uint8_t)i;
        senders[i].stop    = &stop_flag;
        atomic_store(&senders[i].sent, 0);
        atomic_store(&senders[i].failed, 0);
        TEST_ASSERT(
            pthread_create(&sender_thrds[i], NULL, unreliable_sender_fn, &senders[i])
                == 0,
            "unreliable sender thread started");
    }

    /* Reliable sender on channel 4 */
    {
        int i = NUM_UNRELIABLE_SENDERS;
        senders[i].client  = client_a;
        senders[i].channel = 4;
        senders[i].stop    = &stop_flag;
        atomic_store(&senders[i].sent, 0);
        atomic_store(&senders[i].failed, 0);
        TEST_ASSERT(
            pthread_create(&sender_thrds[i], NULL, reliable_sender_fn, &senders[i])
                == 0,
            "reliable sender thread started");
    }

    /* ── Run for SESSION_SECONDS, sampling stats every second ──── */

    fprintf(stdout, "    Running %d-second session...\n", SESSION_SECONDS);

    uint64_t prev_tx = 0;
    uint64_t start_time = now_us();

    for (int sec = 0; sec < SESSION_SECONDS; sec++) {
        sleep_ms(1000);

        uint64_t tx = yumi_udp_client_stat_tx_packets(client_a);
        uint64_t delta = tx - prev_tx;
        prev_tx = tx;

        /* Stat monotonicity: tx should never decrease */
        TEST_ASSERT(tx >= prev_tx - delta, "tx_packets monotonic");

        if (sec == SESSION_SECONDS - 1) {
            uint64_t tx_bytes = yumi_udp_client_stat_tx_bytes(client_a);
            uint64_t rx_acks  = yumi_udp_client_stat_rx_packets(client_a);
            fprintf(stdout, "    [%2ds] tx_pkts=%-8lu tx_bytes=%-10lu "
                    "rx_acks=%-8lu\n",
                    sec + 1,
                    (unsigned long)tx,
                    (unsigned long)tx_bytes,
                    (unsigned long)rx_acks);
        }
    }

    uint64_t elapsed_us = now_us() - start_time;

    /* ── Stop senders ──────────────────────────────────────────── */

    atomic_store(&stop_flag, true);
    for (int i = 0; i < TOTAL_SENDERS; i++) {
        pthread_join(sender_thrds[i], NULL);
    }

    /* Let remaining packets flush */
    sleep_ms(200);

    /* ── Stop ACK responder ────────────────────────────────────── */

    atomic_store(&resp->stop, true);
    {
        pthread_join(resp_thrd, NULL);
    }

    /* ── Collect final stats ───────────────────────────────────── */

    uint64_t tx_pkts  = yumi_udp_client_stat_tx_packets(client_a);
    uint64_t tx_bytes = yumi_udp_client_stat_tx_bytes(client_a);
    uint64_t rx_pkts  = yumi_udp_client_stat_rx_packets(client_a);
    uint64_t retx     = atomic_load_explicit(&client_a->stat_retransmits,
                                              memory_order_relaxed);
    uint64_t acks_sent = atomic_load_explicit(&resp->acks_sent,
                                               memory_order_relaxed);

    uint64_t total_sent = 0, total_failed = 0;
    for (int i = 0; i < TOTAL_SENDERS; i++) {
        total_sent   += atomic_load(&senders[i].sent);
        total_failed += atomic_load(&senders[i].failed);
    }
    uint64_t reliable_sent =
        atomic_load(&senders[NUM_UNRELIABLE_SENDERS].sent);

    fprintf(stdout, "\n    ── Session Results (%lu.%01lu s) ──\n",
            (unsigned long)(elapsed_us / 1000000),
            (unsigned long)((elapsed_us / 100000) % 10));
    fprintf(stdout, "    Sender enqueued    : %lu  (failed: %lu)\n",
            (unsigned long)total_sent, (unsigned long)total_failed);
    fprintf(stdout, "    TX packets (wire)  : %lu\n", (unsigned long)tx_pkts);
    fprintf(stdout, "    TX bytes   (wire)  : %lu\n", (unsigned long)tx_bytes);
    fprintf(stdout, "    RX packets (ACKs)  : %lu\n", (unsigned long)rx_pkts);
    fprintf(stdout, "    Retransmits        : %lu\n", (unsigned long)retx);
    fprintf(stdout, "    ACK responder sent : %lu\n", (unsigned long)acks_sent);

    /* ── Assertions ────────────────────────────────────────────── */

    TEST_SECTION("session assertions");

    /* 1. Throughput: transport was productive (>100 pkt/s average) */
    TEST_ASSERT(tx_pkts >= 100ULL * SESSION_SECONDS,
                "throughput >= 100 pkt/s average");

    /* 2. Senders enqueued a meaningful amount — ring-full rejects
     *    are expected under pacing (backpressure), so we only check
     *    that the transport actually moved data. */
    TEST_ASSERT(total_sent > 0, "at least some sends succeeded");
    TEST_ASSERT(tx_pkts > 0, "transport sent packets on the wire");

    /* 3. TX bytes should be consistent with packets */
    TEST_ASSERT(tx_bytes >= tx_pkts * YUMI_UDP_HDR_SIZE,
                "tx_bytes >= tx_pkts * HDR_SIZE");

    /* 4. Reliable sender should have sent packets */
    TEST_ASSERT(reliable_sent > 0, "reliable sender sent > 0 packets");

    /* 5. ACK responder should have processed ACKs */
    TEST_ASSERT(acks_sent > 0, "ACK responder sent > 0 ACKs");

    /* 6. Client A should have received ACKs */
    TEST_ASSERT(rx_pkts > 0, "client A received ACKs (rx_pkts > 0)");

    /* 7. Per-sender counts: each unreliable sender should be active */
    for (int i = 0; i < NUM_UNRELIABLE_SENDERS; i++) {
        uint64_t s = atomic_load(&senders[i].sent);
        char msg[64];
        snprintf(msg, sizeof(msg), "unreliable sender %d sent > 100", i);
        TEST_ASSERT(s > 100, msg);
    }

    /* 9. Stat monotonicity (final check) */
    {
        uint64_t tx2 = yumi_udp_client_stat_tx_packets(client_a);
        TEST_ASSERT(tx2 >= tx_pkts, "tx_packets still monotonic after stop");
    }

    /* 10. CC state should have progressed */
    {
        yumi_mode_t mode = yumi_cc_get_mode(&client_a->cc);
        uint64_t rate = yumi_cc_get_send_rate(&client_a->cc);
        fprintf(stdout, "    CC mode: %s  send_rate: %lu B/s\n",
                mode == YUMI_MODE_TDS ? "TDS" : "PROBE",
                (unsigned long)rate);
        /* After 5s of reliable traffic, CC should have seen some ACKs */
        TEST_ASSERT(rate > 0, "CC send_rate > 0");
    }

    /* 11. Retransmit count should be reasonable (not runaway) */
    TEST_ASSERT(retx < tx_pkts, "retransmits < total tx (no runaway)");

    /* ── Cleanup ───────────────────────────────────────────────── */

    yumi_udp_client_destroy(client_a);
    free(client_a);
    close(peer_fd);
    free(resp);
    free(senders);
    free(sender_thrds);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Bidirectional throughput benchmark (mirrors SUDP sustained session)
 *
 *  Two full yumi_udp_client_t instances on loopback, exchanging
 *  reliable + unreliable traffic for 5 seconds.  Reports throughput
 *  in MB/s, pkt/s, and per-packet cost.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    _Alignas(64) atomic_uint_fast64_t recv_count;
    _Alignas(64) atomic_uint_fast64_t recv_bytes;
} bidir_cb_state_t;

static void bidir_recv_cb(void *user, const void *data,
                           uint32_t len, bool reliable, uint32_t seq)
{
    (void)data; (void)reliable; (void)seq;
    bidir_cb_state_t *s = (bidir_cb_state_t *)user;
    atomic_fetch_add_explicit(&s->recv_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->recv_bytes, len, memory_order_relaxed);
}

static double ts_diff_s(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec)
         + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static void test_bidirectional_throughput(void)
{
    TEST_SECTION("Bidirectional throughput benchmark (3 s)");

    struct timespec t_section_start, t_phase, t_now;
    clock_gettime(CLOCK_MONOTONIC, &t_section_start);

    bidir_cb_state_t sa_s = {0}, sb_s = {0};
    atomic_store(&sa_s.recv_count, 0);
    atomic_store(&sa_s.recv_bytes, 0);
    atomic_store(&sb_s.recv_count, 0);
    atomic_store(&sb_s.recv_bytes, 0);

    /* ── Create two clients pointed at each other ──────────────── */

    /* Bind A first to discover its port, then create B pointed at A,
     * then create A pointed at B.  Use ephemeral ports. */

    /* We need to know both ports up front.  Bind two raw sockets to
     * discover ports, close them, then use those ports for the clients. */
    int tmp_a = socket(AF_INET6, SOCK_DGRAM, 0);
    int tmp_b = socket(AF_INET6, SOCK_DGRAM, 0);
    {
        int off = 0;
        setsockopt(tmp_a, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        setsockopt(tmp_b, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 addr = make_loopback(0);
        bind(tmp_a, (struct sockaddr *)&addr, sizeof(addr));
        bind(tmp_b, (struct sockaddr *)&addr, sizeof(addr));
    }
    uint16_t port_a = bound_port(tmp_a);
    uint16_t port_b = bound_port(tmp_b);
    close(tmp_a);
    close(tmp_b);

    yumi_udp_client_config_t cfg_a = {0};
    cfg_a.peer_addr           = make_loopback(port_b);
    cfg_a.local_port          = port_a;
    cfg_a.reliable_timeout_us = 50000;
    cfg_a.max_retransmits     = 5;
    cfg_a.recv_cb             = bidir_recv_cb;
    cfg_a.recv_user           = &sa_s;
    cfg_set_passthrough_crypto(&cfg_a);

    yumi_udp_client_config_t cfg_b = {0};
    cfg_b.peer_addr           = make_loopback(port_a);
    cfg_b.local_port          = port_b;
    cfg_b.reliable_timeout_us = 50000;
    cfg_b.max_retransmits     = 5;
    cfg_b.recv_cb             = bidir_recv_cb;
    cfg_b.recv_user           = &sb_s;
    cfg_set_passthrough_crypto(&cfg_b);

    clock_gettime(CLOCK_MONOTONIC, &t_phase);

    yumi_udp_client_t *client_a = client_alloc();
    yumi_udp_client_t *client_b = client_alloc();
    int rc_a = yumi_udp_client_create(client_a, &cfg_a);
    int rc_b = yumi_udp_client_create(client_b, &cfg_b);
    TEST_ASSERT(rc_a == 0, "client A created");
    TEST_ASSERT(rc_b == 0, "client B created");

    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] client create:     %7.3f s\n",
            ts_diff_s(&t_phase, &t_now));

    uint32_t max_pl = yumi_udp_client_get_max_payload(client_a);
    TEST_ASSERT(max_pl > 0, "max_payload > 0");
    fprintf(stdout, "    max_payload = %u bytes\n", max_pl);

    /* Prepare payload (fill with pattern) */
    uint32_t payload_sz = max_pl < 1024 ? max_pl : 1024;
    uint8_t *payload = malloc(payload_sz);
    TEST_ASSERT(payload != NULL, "payload alloc");
    for (uint32_t i = 0; i < payload_sz; i++)
        payload[i] = (uint8_t)(i & 0xFF);

    /* ── Send loop: 3 seconds, alternating reliable/unreliable ── */

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint64_t a_sent = 0, b_sent = 0;
    int send_errors = 0;
    double elapsed = 0.0;

    fprintf(stdout, "    running for 3 seconds ...\n");

    while (elapsed < 3.0) {
        int iter_fails = 0;

        /* A → B: alternate reliable / unreliable */
        if (a_sent % 2 == 0) {
            if (yumi_udp_client_send_reliable(client_a, payload, payload_sz) == 0)
                a_sent++;
            else { send_errors++; iter_fails++; }
        } else {
            if (yumi_udp_client_send(client_a, payload, payload_sz) == 0)
                a_sent++;
            else { send_errors++; iter_fails++; }
        }

        /* B → A: same pattern */
        if (b_sent % 2 == 0) {
            if (yumi_udp_client_send_reliable(client_b, payload, payload_sz) == 0)
                b_sent++;
            else { send_errors++; iter_fails++; }
        } else {
            if (yumi_udp_client_send(client_b, payload, payload_sz) == 0)
                b_sent++;
            else { send_errors++; iter_fails++; }
        }

        /* Yield only on backpressure (ring full) to let workers drain */
        if (iter_fails)
            usleep(1);

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - start.tv_sec)
                + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] send loop:         %7.3f s\n",
            ts_diff_s(&start, &t_now));

    /* Let stragglers arrive */
    clock_gettime(CLOCK_MONOTONIC, &t_phase);
    usleep(200000);
    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] drain wait:        %7.3f s\n",
            ts_diff_s(&t_phase, &t_now));

    /* ── Collect results ───────────────────────────────────────── */

    uint64_t b_rx       = atomic_load(&sb_s.recv_count);
    uint64_t a_rx       = atomic_load(&sa_s.recv_count);
    uint64_t b_rx_bytes = atomic_load(&sb_s.recv_bytes);
    uint64_t a_rx_bytes = atomic_load(&sa_s.recv_bytes);

    fprintf(stdout, "    elapsed      = %.1f s\n", elapsed);
    fprintf(stdout, "    A sent       = %llu packets\n", (unsigned long long)a_sent);
    fprintf(stdout, "    B sent       = %llu packets\n", (unsigned long long)b_sent);
    fprintf(stdout, "    B received   = %llu packets (%llu bytes)\n",
            (unsigned long long)b_rx, (unsigned long long)b_rx_bytes);
    fprintf(stdout, "    A received   = %llu packets (%llu bytes)\n",
            (unsigned long long)a_rx, (unsigned long long)a_rx_bytes);
    fprintf(stdout, "    send errors  = %d\n", send_errors);

    /* Throughput summary */
    uint64_t total_rx_bytes = b_rx_bytes + a_rx_bytes;
    uint64_t total_rx_pkts  = b_rx + a_rx;
    double   loop_s         = elapsed > 0.0 ? elapsed : 1.0;
    fprintf(stdout, "    throughput   = %.2f MB/s  (%.0f pkt/s, bidir)\n",
            (double)total_rx_bytes / loop_s / (1024.0 * 1024.0),
            (double)total_rx_pkts / loop_s);
    fprintf(stdout, "    per-pkt cost = %.3f ms  (send+recv, avg)\n",
            total_rx_pkts > 0
                ? (loop_s * 1000.0) / (double)total_rx_pkts
                : 0.0);

    /* ── Assertions ────────────────────────────────────────────── */

    TEST_SECTION("bidirectional throughput assertions");

    TEST_ASSERT(b_rx > 100, "B received enough packets from A");
    TEST_ASSERT(a_rx > 100, "A received enough packets from B");
    TEST_ASSERT(b_rx_bytes > 0, "B received bytes > 0");
    TEST_ASSERT(a_rx_bytes > 0, "A received bytes > 0");

    /* CC state should have progressed for both */
    {
        yumi_mode_t mode_a = yumi_cc_get_mode(&client_a->cc);
        uint64_t rate_a = yumi_cc_get_send_rate(&client_a->cc);
        yumi_mode_t mode_b = yumi_cc_get_mode(&client_b->cc);
        uint64_t rate_b = yumi_cc_get_send_rate(&client_b->cc);
        fprintf(stdout, "    CC A: mode=%s  rate=%lu B/s\n",
                mode_a == YUMI_MODE_TDS ? "TDS" : "PROBE",
                (unsigned long)rate_a);
        fprintf(stdout, "    CC B: mode=%s  rate=%lu B/s\n",
                mode_b == YUMI_MODE_TDS ? "TDS" : "PROBE",
                (unsigned long)rate_b);
        TEST_ASSERT(rate_a > 0, "CC A send_rate > 0");
        TEST_ASSERT(rate_b > 0, "CC B send_rate > 0");
    }

    /* Retransmits should be reasonable */
    {
        uint64_t retx_a = atomic_load_explicit(&client_a->stat_retransmits,
                                                memory_order_relaxed);
        uint64_t retx_b = atomic_load_explicit(&client_b->stat_retransmits,
                                                memory_order_relaxed);
        uint64_t tx_a = yumi_udp_client_stat_tx_packets(client_a);
        uint64_t tx_b = yumi_udp_client_stat_tx_packets(client_b);
        fprintf(stdout, "    A: tx=%llu retx=%llu\n",
                (unsigned long long)tx_a, (unsigned long long)retx_a);
        fprintf(stdout, "    B: tx=%llu retx=%llu\n",
                (unsigned long long)tx_b, (unsigned long long)retx_b);
        TEST_ASSERT(retx_a < tx_a, "A retransmits < total tx");
        TEST_ASSERT(retx_b < tx_b, "B retransmits < total tx");
    }

    /* ── Cleanup ───────────────────────────────────────────────── */

    free(payload);

    clock_gettime(CLOCK_MONOTONIC, &t_phase);
    yumi_udp_client_destroy(client_a);
    yumi_udp_client_destroy(client_b);
    free(client_a);
    free(client_b);
    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] destroy:           %7.3f s\n",
            ts_diff_s(&t_phase, &t_now));

    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] section total:     %7.3f s\n",
            ts_diff_s(&t_section_start, &t_now));
}

/* ═══════════════════════════════════════════════════════════════════
 *  Threefish-1024 AEAD crypto hooks for throughput testing
 *
 *  Same wire format as SUDP: [nonce(16)][AEAD(pt) || tag(128)]
 *  Both sides share the same key + pre-derived subkeys.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    yumi_aead_subkeys_t subkeys;
    _Alignas(64) atomic_uint_fast64_t nonce_counter;
} threefish_ctx_t;

static int threefish_encrypt(void *ctx, const uint8_t *pt, uint32_t pt_len,
                              uint8_t *ct, uint32_t *ct_len)
{
    threefish_ctx_t *tc = (threefish_ctx_t *)ctx;

    uint8_t nonce[YUMI_AEAD_NONCE_LEN];
    memset(nonce, 0, 8);
    uint64_t ctr = atomic_fetch_add_explicit(&tc->nonce_counter, 1,
                                              memory_order_relaxed);
    for (int i = 7; i >= 0; i--) {
        nonce[8 + i] = (uint8_t)(ctr & 0xFF);
        ctr >>= 8;
    }

    memcpy(ct, nonce, YUMI_AEAD_NONCE_LEN);

    size_t aead_out;
    if (yumi_aead_encrypt_keyed(ct + YUMI_AEAD_NONCE_LEN, &aead_out,
                                 pt, pt_len, NULL, 0,
                                 nonce, &tc->subkeys) != YUMI_CRYPTO_OK)
        return -1;

    *ct_len = (uint32_t)(YUMI_AEAD_NONCE_LEN + aead_out);
    return 0;
}

static int threefish_decrypt(void *ctx, const uint8_t *ct, uint32_t ct_len,
                              uint8_t *pt, uint32_t *pt_len)
{
    threefish_ctx_t *tc = (threefish_ctx_t *)ctx;

    if (ct_len < YUMI_AEAD_NONCE_LEN + YUMI_AEAD_TAG_LEN + 1)
        return -1;

    const uint8_t *nonce   = ct;
    const uint8_t *aead_ct = ct + YUMI_AEAD_NONCE_LEN;
    uint32_t aead_len      = ct_len - YUMI_AEAD_NONCE_LEN;

    size_t out_len;
    if (yumi_aead_decrypt_keyed(pt, &out_len, aead_ct, aead_len,
                                 NULL, 0, nonce,
                                 &tc->subkeys) != YUMI_CRYPTO_OK)
        return -1;

    *pt_len = (uint32_t)out_len;
    return 0;
}

#define THREEFISH_OVERHEAD (YUMI_AEAD_NONCE_LEN + YUMI_AEAD_TAG_LEN) /* 144 */

static void test_threefish_throughput(void)
{
    TEST_SECTION("Threefish-1024 AEAD throughput benchmark (3 s)");

    struct timespec t_section_start, t_phase, t_now;
    clock_gettime(CLOCK_MONOTONIC, &t_section_start);

    /* ── Shared key — both sides use the same key ──────────────── */
    uint8_t key[YUMI_AEAD_KEY_LEN];
    yumi_randombytes(key, sizeof(key));

    threefish_ctx_t ctx_a = {0}, ctx_b = {0};
    yumi_aead_derive_subkeys(&ctx_a.subkeys, key);
    yumi_aead_derive_subkeys(&ctx_b.subkeys, key);
    atomic_store(&ctx_a.nonce_counter, 0);
    atomic_store(&ctx_b.nonce_counter, 1ULL << 62);  /* different nonce space */
    yumi_memzero(key, sizeof(key));

    bidir_cb_state_t sa_s = {0}, sb_s = {0};
    atomic_store(&sa_s.recv_count, 0);
    atomic_store(&sa_s.recv_bytes, 0);
    atomic_store(&sb_s.recv_count, 0);
    atomic_store(&sb_s.recv_bytes, 0);

    /* ── Discover ports ────────────────────────────────────────── */
    int tmp_a = socket(AF_INET6, SOCK_DGRAM, 0);
    int tmp_b = socket(AF_INET6, SOCK_DGRAM, 0);
    {
        int off = 0;
        setsockopt(tmp_a, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        setsockopt(tmp_b, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 addr = make_loopback(0);
        bind(tmp_a, (struct sockaddr *)&addr, sizeof(addr));
        bind(tmp_b, (struct sockaddr *)&addr, sizeof(addr));
    }
    uint16_t port_a = bound_port(tmp_a);
    uint16_t port_b = bound_port(tmp_b);
    close(tmp_a);
    close(tmp_b);

    /* ── Client configs with Threefish AEAD ────────────────────── */
    yumi_udp_client_config_t cfg_a = {0};
    cfg_a.peer_addr            = make_loopback(port_b);
    cfg_a.local_port           = port_a;
    cfg_a.reliable_timeout_us  = 50000;
    cfg_a.max_retransmits      = 5;
    cfg_a.recv_cb              = bidir_recv_cb;
    cfg_a.recv_user            = &sa_s;
    cfg_a.pkt_encrypt          = threefish_encrypt;
    cfg_a.pkt_decrypt          = threefish_decrypt;
    cfg_a.pkt_crypto_ctx       = &ctx_a;
    cfg_a.pkt_crypto_overhead  = THREEFISH_OVERHEAD;

    yumi_udp_client_config_t cfg_b = {0};
    cfg_b.peer_addr            = make_loopback(port_a);
    cfg_b.local_port           = port_b;
    cfg_b.reliable_timeout_us  = 50000;
    cfg_b.max_retransmits      = 5;
    cfg_b.recv_cb              = bidir_recv_cb;
    cfg_b.recv_user            = &sb_s;
    cfg_b.pkt_encrypt          = threefish_encrypt;
    cfg_b.pkt_decrypt          = threefish_decrypt;
    cfg_b.pkt_crypto_ctx       = &ctx_b;
    cfg_b.pkt_crypto_overhead  = THREEFISH_OVERHEAD;

    clock_gettime(CLOCK_MONOTONIC, &t_phase);

    yumi_udp_client_t *client_a = client_alloc();
    yumi_udp_client_t *client_b = client_alloc();
    int rc_a = yumi_udp_client_create(client_a, &cfg_a);
    int rc_b = yumi_udp_client_create(client_b, &cfg_b);
    TEST_ASSERT(rc_a == 0, "client A created (threefish)");
    TEST_ASSERT(rc_b == 0, "client B created (threefish)");

    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] client create:     %7.3f s\n",
            ts_diff_s(&t_phase, &t_now));

    uint32_t max_pl = yumi_udp_client_get_max_payload(client_a);
    TEST_ASSERT(max_pl > 0, "max_payload > 0");
    fprintf(stdout, "    max_payload = %u bytes (crypto overhead = %u)\n",
            max_pl, THREEFISH_OVERHEAD);

    /* Prepare payload (same size as SUDP test) */
    uint32_t payload_sz = max_pl < 1024 ? max_pl : 1024;
    uint8_t *payload = malloc(payload_sz);
    TEST_ASSERT(payload != NULL, "payload alloc");
    for (uint32_t i = 0; i < payload_sz; i++)
        payload[i] = (uint8_t)(i & 0xFF);

    /* ── Send loop: 3 seconds, alternating reliable/unreliable ── */
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint64_t a_sent = 0, b_sent = 0;
    int send_errors = 0;
    double elapsed = 0.0;

    fprintf(stdout, "    running for 3 seconds ...\n");

    while (elapsed < 3.0) {
        int iter_fails = 0;

        if (a_sent % 2 == 0) {
            if (yumi_udp_client_send_reliable(client_a, payload, payload_sz) == 0)
                a_sent++;
            else { send_errors++; iter_fails++; }
        } else {
            if (yumi_udp_client_send(client_a, payload, payload_sz) == 0)
                a_sent++;
            else { send_errors++; iter_fails++; }
        }

        if (b_sent % 2 == 0) {
            if (yumi_udp_client_send_reliable(client_b, payload, payload_sz) == 0)
                b_sent++;
            else { send_errors++; iter_fails++; }
        } else {
            if (yumi_udp_client_send(client_b, payload, payload_sz) == 0)
                b_sent++;
            else { send_errors++; iter_fails++; }
        }

        if (iter_fails)
            usleep(1);

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - start.tv_sec)
                + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] send loop:         %7.3f s\n",
            ts_diff_s(&start, &t_now));

    clock_gettime(CLOCK_MONOTONIC, &t_phase);
    usleep(200000);
    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] drain wait:        %7.3f s\n",
            ts_diff_s(&t_phase, &t_now));

    /* ── Collect results ───────────────────────────────────────── */
    uint64_t b_rx       = atomic_load(&sb_s.recv_count);
    uint64_t a_rx       = atomic_load(&sa_s.recv_count);
    uint64_t b_rx_bytes = atomic_load(&sb_s.recv_bytes);
    uint64_t a_rx_bytes = atomic_load(&sa_s.recv_bytes);

    fprintf(stdout, "    elapsed      = %.1f s\n", elapsed);
    fprintf(stdout, "    A sent       = %llu packets\n", (unsigned long long)a_sent);
    fprintf(stdout, "    B sent       = %llu packets\n", (unsigned long long)b_sent);
    fprintf(stdout, "    B received   = %llu packets (%llu bytes)\n",
            (unsigned long long)b_rx, (unsigned long long)b_rx_bytes);
    fprintf(stdout, "    A received   = %llu packets (%llu bytes)\n",
            (unsigned long long)a_rx, (unsigned long long)a_rx_bytes);
    fprintf(stdout, "    send errors  = %d\n", send_errors);

    uint64_t total_rx_bytes = b_rx_bytes + a_rx_bytes;
    uint64_t total_rx_pkts  = b_rx + a_rx;
    double   loop_s         = elapsed > 0.0 ? elapsed : 1.0;
    fprintf(stdout, "    throughput   = %.2f MB/s  (%.0f pkt/s, bidir)\n",
            (double)total_rx_bytes / loop_s / (1024.0 * 1024.0),
            (double)total_rx_pkts / loop_s);
    fprintf(stdout, "    per-pkt cost = %.3f ms  (encrypt+send+recv+decrypt, avg)\n",
            total_rx_pkts > 0
                ? (loop_s * 1000.0) / (double)total_rx_pkts
                : 0.0);

    /* ── Assertions ────────────────────────────────────────────── */
    TEST_SECTION("threefish throughput assertions");

    TEST_ASSERT(b_rx > 100, "B received enough packets");
    TEST_ASSERT(a_rx > 100, "A received enough packets");
    TEST_ASSERT(b_rx_bytes > 0, "B received bytes > 0");
    TEST_ASSERT(a_rx_bytes > 0, "A received bytes > 0");

    {
        yumi_mode_t mode_a = yumi_cc_get_mode(&client_a->cc);
        uint64_t rate_a = yumi_cc_get_send_rate(&client_a->cc);
        yumi_mode_t mode_b = yumi_cc_get_mode(&client_b->cc);
        uint64_t rate_b = yumi_cc_get_send_rate(&client_b->cc);
        fprintf(stdout, "    CC A: mode=%s  rate=%lu B/s\n",
                mode_a == YUMI_MODE_TDS ? "TDS" : "PROBE",
                (unsigned long)rate_a);
        fprintf(stdout, "    CC B: mode=%s  rate=%lu B/s\n",
                mode_b == YUMI_MODE_TDS ? "TDS" : "PROBE",
                (unsigned long)rate_b);
        TEST_ASSERT(rate_a > 0, "CC A send_rate > 0");
        TEST_ASSERT(rate_b > 0, "CC B send_rate > 0");
    }

    {
        uint64_t retx_a = atomic_load_explicit(&client_a->stat_retransmits,
                                                memory_order_relaxed);
        uint64_t retx_b = atomic_load_explicit(&client_b->stat_retransmits,
                                                memory_order_relaxed);
        uint64_t tx_a = yumi_udp_client_stat_tx_packets(client_a);
        uint64_t tx_b = yumi_udp_client_stat_tx_packets(client_b);
        fprintf(stdout, "    A: tx=%llu retx=%llu\n",
                (unsigned long long)tx_a, (unsigned long long)retx_a);
        fprintf(stdout, "    B: tx=%llu retx=%llu\n",
                (unsigned long long)tx_b, (unsigned long long)retx_b);
        TEST_ASSERT(retx_a < tx_a, "A retransmits < total tx");
        TEST_ASSERT(retx_b < tx_b, "B retransmits < total tx");
    }

    /* ── Cleanup ───────────────────────────────────────────────── */
    free(payload);

    clock_gettime(CLOCK_MONOTONIC, &t_phase);
    yumi_udp_client_destroy(client_a);
    yumi_udp_client_destroy(client_b);
    free(client_a);
    free(client_b);
    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] destroy:           %7.3f s\n",
            ts_diff_s(&t_phase, &t_now));

    yumi_aead_subkeys_wipe(&ctx_a.subkeys);
    yumi_aead_subkeys_wipe(&ctx_b.subkeys);

    clock_gettime(CLOCK_MONOTONIC, &t_now);
    fprintf(stdout, "    [timer] section total:     %7.3f s\n",
            ts_diff_s(&t_section_start, &t_now));
}

/* ═══════════════════════════════════════════════════════════════════
 *  Diagnostic benchmark — detailed periodic profiling
 *
 *  Runs side-by-side plain (passthrough) and Threefish sessions for
 *  20 seconds each, sampling every second:
 *    - instantaneous throughput (MB/s per second)
 *    - cumulative throughput
 *    - CC mode + send_rate for both sides
 *    - retransmit count + delta
 *    - rx/tx packet counts + deltas
 *    - asymmetry ratio (A-rx vs B-rx)
 *
 *  Goal: pinpoint exactly where and how throughput degrades over time.
 * ═══════════════════════════════════════════════════════════════════ */

#define DIAG_DURATION_S  3
#define DIAG_SAMPLE_HZ    1  /* samples per second */

typedef struct {
    double   wall_s;
    uint64_t a_tx, b_tx;
    uint64_t a_rx, b_rx;
    uint64_t a_rx_bytes, b_rx_bytes;
    uint64_t retx_a, retx_b;
    uint64_t cc_rate_a, cc_rate_b;
    int      cc_mode_a, cc_mode_b;   /* 0=PROBE, 1=TDS */
} diag_sample_t;

static const char *mode_str(int m) { return m == 1 ? "TDS" : "PROBE"; }

static void diag_run_session(const char *label,
                              yumi_udp_client_config_t *cfg_a,
                              yumi_udp_client_config_t *cfg_b)
{
    fprintf(stdout, "\n    ┌─── %s (%d s) ───\n", label, DIAG_DURATION_S);

    bidir_cb_state_t sa_s = {0}, sb_s = {0};
    atomic_store(&sa_s.recv_count, 0);
    atomic_store(&sa_s.recv_bytes, 0);
    atomic_store(&sb_s.recv_count, 0);
    atomic_store(&sb_s.recv_bytes, 0);

    cfg_a->recv_cb   = bidir_recv_cb;
    cfg_a->recv_user = &sa_s;
    cfg_b->recv_cb   = bidir_recv_cb;
    cfg_b->recv_user = &sb_s;

    yumi_udp_client_t *ca = client_alloc();
    yumi_udp_client_t *cb = client_alloc();
    int rc_a = yumi_udp_client_create(ca, cfg_a);
    int rc_b = yumi_udp_client_create(cb, cfg_b);
    TEST_ASSERT(rc_a == 0, "diag client A created");
    TEST_ASSERT(rc_b == 0, "diag client B created");

    uint32_t max_pl = yumi_udp_client_get_max_payload(ca);
    uint32_t payload_sz = max_pl < 1024 ? max_pl : 1024;
    uint8_t *payload = malloc(payload_sz);
    for (uint32_t i = 0; i < payload_sz; i++)
        payload[i] = (uint8_t)(i & 0xFF);

    fprintf(stdout, "    │  max_payload = %u bytes\n", max_pl);
    fprintf(stdout, "    │\n");
    fprintf(stdout, "    │  %4s  %8s %8s  %8s %8s  %7s %7s  %6s %6s  "
                    "%6s %6s  %5s\n",
            "t(s)", "A→B/s", "B→A/s", "A→Bcum", "B→Acum",
            "rateA", "rateB", "modeA", "modeB",
            "retxA", "retxB", "asym");
    fprintf(stdout, "    │  %4s  %8s %8s  %8s %8s  %7s %7s  %6s %6s  "
                    "%6s %6s  %5s\n",
            "────", "────────", "────────", "────────", "────────",
            "───────", "───────", "──────", "──────",
            "──────", "──────", "─────");

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint64_t a_sent = 0, b_sent = 0;
    int send_errors = 0;
    int next_sample = 1;

    /* Previous sample values for deltas */
    uint64_t prev_b_rx_bytes = 0, prev_a_rx_bytes = 0;
    uint64_t prev_retx_a = 0, prev_retx_b = 0;

    double elapsed = 0.0;
    while (elapsed < (double)DIAG_DURATION_S) {
        /* Enqueue one packet per side per iteration (matches real API
         * usage pattern — avoids artificial microbursts that overflow
         * loopback socket buffers). */
        int fails = 0;
        if (a_sent % 2 == 0) {
            if (yumi_udp_client_send_reliable(ca, payload, payload_sz) == 0)
                a_sent++;
            else { send_errors++; fails++; }
        } else {
            if (yumi_udp_client_send(ca, payload, payload_sz) == 0)
                a_sent++;
            else { send_errors++; fails++; }
        }
        if (b_sent % 2 == 0) {
            if (yumi_udp_client_send_reliable(cb, payload, payload_sz) == 0)
                b_sent++;
            else { send_errors++; fails++; }
        } else {
            if (yumi_udp_client_send(cb, payload, payload_sz) == 0)
                b_sent++;
            else { send_errors++; fails++; }
        }
        if (fails) usleep(1);

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - start.tv_sec)
                + (double)(now.tv_nsec - start.tv_nsec) / 1e9;

        /* Periodic sample */
        if (elapsed >= (double)next_sample && next_sample <= DIAG_DURATION_S) {
            uint64_t b_rx_bytes = atomic_load(&sb_s.recv_bytes);
            uint64_t a_rx_bytes = atomic_load(&sa_s.recv_bytes);
            uint64_t b_rx       = atomic_load(&sb_s.recv_count);
            uint64_t a_rx       = atomic_load(&sa_s.recv_count);
            uint64_t retx_a = atomic_load_explicit(&ca->stat_retransmits,
                                                    memory_order_relaxed);
            uint64_t retx_b = atomic_load_explicit(&cb->stat_retransmits,
                                                    memory_order_relaxed);
            uint64_t rate_a = yumi_cc_get_send_rate(&ca->cc);
            uint64_t rate_b = yumi_cc_get_send_rate(&cb->cc);
            yumi_mode_t mode_a = yumi_cc_get_mode(&ca->cc);
            yumi_mode_t mode_b = yumi_cc_get_mode(&cb->cc);

            /* Instant throughput = delta bytes in this 1-second window */
            double inst_b = (double)(b_rx_bytes - prev_b_rx_bytes)
                          / (1024.0 * 1024.0);
            double inst_a = (double)(a_rx_bytes - prev_a_rx_bytes)
                          / (1024.0 * 1024.0);

            /* Cumulative throughput */
            double cum_b = (double)b_rx_bytes / (1024.0 * 1024.0);
            double cum_a = (double)a_rx_bytes / (1024.0 * 1024.0);

            /* Asymmetry: ratio of smaller/larger rx count (1.0 = perfect) */
            double asym = (b_rx > 0 && a_rx > 0)
                        ? (double)(b_rx < a_rx ? b_rx : a_rx)
                        / (double)(b_rx > a_rx ? b_rx : a_rx)
                        : 0.0;

            fprintf(stdout,
                    "    │  %4d  %7.1f  %7.1f   %7.0f  %7.0f   "
                    "%6.1fM %6.1fM  %5s  %5s  "
                    "%5llu  %5llu  %.3f\n",
                    next_sample,
                    inst_b, inst_a,
                    cum_b, cum_a,
                    (double)rate_a / 1e6, (double)rate_b / 1e6,
                    mode_str(mode_a == YUMI_MODE_TDS),
                    mode_str(mode_b == YUMI_MODE_TDS),
                    (unsigned long long)(retx_a - prev_retx_a),
                    (unsigned long long)(retx_b - prev_retx_b),
                    asym);

            prev_b_rx_bytes = b_rx_bytes;
            prev_a_rx_bytes = a_rx_bytes;
            prev_retx_a = retx_a;
            prev_retx_b = retx_b;
            next_sample++;
        }
    }

    /* Drain */
    usleep(200000);

    /* Final summary */
    uint64_t b_rx       = atomic_load(&sb_s.recv_count);
    uint64_t a_rx       = atomic_load(&sa_s.recv_count);
    uint64_t b_rx_bytes = atomic_load(&sb_s.recv_bytes);
    uint64_t a_rx_bytes = atomic_load(&sa_s.recv_bytes);
    uint64_t total_rx   = b_rx_bytes + a_rx_bytes;
    double   final_mbps = (double)total_rx / elapsed / (1024.0 * 1024.0);
    uint64_t tx_a = yumi_udp_client_stat_tx_packets(ca);
    uint64_t tx_b = yumi_udp_client_stat_tx_packets(cb);
    uint64_t retx_a = atomic_load_explicit(&ca->stat_retransmits,
                                            memory_order_relaxed);
    uint64_t retx_b = atomic_load_explicit(&cb->stat_retransmits,
                                            memory_order_relaxed);

    fprintf(stdout, "    │\n");
    fprintf(stdout, "    │  ── Summary ──\n");
    fprintf(stdout, "    │  elapsed       = %.1f s\n", elapsed);
    fprintf(stdout, "    │  A enqueued    = %llu  tx = %llu  retx = %llu (%.2f%%)\n",
            (unsigned long long)a_sent, (unsigned long long)tx_a,
            (unsigned long long)retx_a,
            tx_a > 0 ? 100.0 * (double)retx_a / (double)tx_a : 0.0);
    fprintf(stdout, "    │  B enqueued    = %llu  tx = %llu  retx = %llu (%.2f%%)\n",
            (unsigned long long)b_sent, (unsigned long long)tx_b,
            (unsigned long long)retx_b,
            tx_b > 0 ? 100.0 * (double)retx_b / (double)tx_b : 0.0);
    fprintf(stdout, "    │  B received    = %llu pkts  (%llu bytes)\n",
            (unsigned long long)b_rx, (unsigned long long)b_rx_bytes);
    fprintf(stdout, "    │  A received    = %llu pkts  (%llu bytes)\n",
            (unsigned long long)a_rx, (unsigned long long)a_rx_bytes);
    fprintf(stdout, "    │  throughput    = %.2f MB/s  (bidir)\n", final_mbps);
    fprintf(stdout, "    │  send errors   = %d\n", send_errors);

    yumi_mode_t fm_a = yumi_cc_get_mode(&ca->cc);
    yumi_mode_t fm_b = yumi_cc_get_mode(&cb->cc);
    uint64_t fr_a = yumi_cc_get_send_rate(&ca->cc);
    uint64_t fr_b = yumi_cc_get_send_rate(&cb->cc);
    fprintf(stdout, "    │  CC A: mode=%-5s  rate=%.1f MB/s  rtprop=%llu µs  btlbw=%llu B/s  loss_exit=%s\n",
            mode_str(fm_a == YUMI_MODE_TDS),
            (double)fr_a / 1e6,
            (unsigned long long)ca->cc.probe.rtprop_us,
            (unsigned long long)ca->cc.probe.btl_bw,
            ca->cc.probe.startup_loss_exit ? "yes" : "no");
    fprintf(stdout, "    │  CC B: mode=%-5s  rate=%.1f MB/s  rtprop=%llu µs  btlbw=%llu B/s  loss_exit=%s\n",
            mode_str(fm_b == YUMI_MODE_TDS),
            (double)fr_b / 1e6,
            (unsigned long long)cb->cc.probe.rtprop_us,
            (unsigned long long)cb->cc.probe.btl_bw,
            cb->cc.probe.startup_loss_exit ? "yes" : "no");
    fprintf(stdout, "    └───\n");

    TEST_ASSERT(b_rx > 1000, "diag: B received enough");
    TEST_ASSERT(a_rx > 1000, "diag: A received enough");
    TEST_ASSERT(final_mbps > 1.0, "diag: throughput > 1 MB/s");

    free(payload);
    yumi_udp_client_destroy(ca);
    yumi_udp_client_destroy(cb);
    free(ca);
    free(cb);
}

static void test_diagnostic_benchmark(void)
{
    TEST_SECTION("Diagnostic benchmark — per-second profiling");

    /* ── Discover ports for plain session ───────────────── */
    int tmp_a = socket(AF_INET6, SOCK_DGRAM, 0);
    int tmp_b = socket(AF_INET6, SOCK_DGRAM, 0);
    {
        int off = 0;
        setsockopt(tmp_a, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        setsockopt(tmp_b, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 addr = make_loopback(0);
        bind(tmp_a, (struct sockaddr *)&addr, sizeof(addr));
        bind(tmp_b, (struct sockaddr *)&addr, sizeof(addr));
    }
    uint16_t pa1 = bound_port(tmp_a);
    uint16_t pb1 = bound_port(tmp_b);
    close(tmp_a);
    close(tmp_b);

    /* ── Plain (passthrough crypto) ─────────────────────── */
    {
        yumi_udp_client_config_t ca_cfg = {0}, cb_cfg = {0};
        ca_cfg.peer_addr           = make_loopback(pb1);
        ca_cfg.local_port          = pa1;
        ca_cfg.reliable_timeout_us = 50000;
        ca_cfg.max_retransmits     = 5;
        cfg_set_passthrough_crypto(&ca_cfg);

        cb_cfg.peer_addr           = make_loopback(pa1);
        cb_cfg.local_port          = pb1;
        cb_cfg.reliable_timeout_us = 50000;
        cb_cfg.max_retransmits     = 5;
        cfg_set_passthrough_crypto(&cb_cfg);

        diag_run_session("Plain (passthrough)", &ca_cfg, &cb_cfg);
    }

    /* ── Discover ports for Threefish session ──────────── */
    tmp_a = socket(AF_INET6, SOCK_DGRAM, 0);
    tmp_b = socket(AF_INET6, SOCK_DGRAM, 0);
    {
        int off = 0;
        setsockopt(tmp_a, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        setsockopt(tmp_b, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 addr = make_loopback(0);
        bind(tmp_a, (struct sockaddr *)&addr, sizeof(addr));
        bind(tmp_b, (struct sockaddr *)&addr, sizeof(addr));
    }
    uint16_t pa2 = bound_port(tmp_a);
    uint16_t pb2 = bound_port(tmp_b);
    close(tmp_a);
    close(tmp_b);

    /* ── Threefish-1024 AEAD ────────────────────────────── */
    {
        uint8_t key[YUMI_AEAD_KEY_LEN];
        yumi_randombytes(key, sizeof(key));

        threefish_ctx_t ctx_a = {0}, ctx_b = {0};
        yumi_aead_derive_subkeys(&ctx_a.subkeys, key);
        yumi_aead_derive_subkeys(&ctx_b.subkeys, key);
        atomic_store(&ctx_a.nonce_counter, 0);
        atomic_store(&ctx_b.nonce_counter, 1ULL << 62);
        yumi_memzero(key, sizeof(key));

        yumi_udp_client_config_t ca_cfg = {0}, cb_cfg = {0};
        ca_cfg.peer_addr            = make_loopback(pb2);
        ca_cfg.local_port           = pa2;
        ca_cfg.reliable_timeout_us  = 50000;
        ca_cfg.max_retransmits      = 5;
        ca_cfg.pkt_encrypt          = threefish_encrypt;
        ca_cfg.pkt_decrypt          = threefish_decrypt;
        ca_cfg.pkt_crypto_ctx       = &ctx_a;
        ca_cfg.pkt_crypto_overhead  = THREEFISH_OVERHEAD;

        cb_cfg.peer_addr            = make_loopback(pa2);
        cb_cfg.local_port           = pb2;
        cb_cfg.reliable_timeout_us  = 50000;
        cb_cfg.max_retransmits      = 5;
        cb_cfg.pkt_encrypt          = threefish_encrypt;
        cb_cfg.pkt_decrypt          = threefish_decrypt;
        cb_cfg.pkt_crypto_ctx       = &ctx_b;
        cb_cfg.pkt_crypto_overhead  = THREEFISH_OVERHEAD;

        diag_run_session("Threefish-1024 AEAD", &ca_cfg, &cb_cfg);

        yumi_aead_subkeys_wipe(&ctx_a.subkeys);
        yumi_aead_subkeys_wipe(&ctx_b.subkeys);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
    fprintf(stdout, "═══ YumiUdpClient 5s Session Test ═══\n\n");

    test_5s_session();
    test_bidirectional_throughput();
    test_threefish_throughput();
    test_diagnostic_benchmark();

    fprintf(stdout, "\n═══ Results: %d/%d passed ═══\n",
            g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        fprintf(stderr, "%d test(s) FAILED\n", g_tests_failed);
        return 1;
    }
    fprintf(stdout, "All tests passed.\n");
    return 0;
}
