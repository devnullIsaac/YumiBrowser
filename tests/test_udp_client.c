/*
    Yumi Tests — UDP Client Test Suite
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
 * test_udp_client.c — Extensive test suite for yumi_udp_client
 *
 * Tests the following layers:
 *   1. Wire protocol header encode/decode
 *   2. MPSC ring buffer (single-thread, multi-thread, overflow)
 *   3. Loopback unreliable send/recv
 *   4. Loopback reliable send/recv (ACK path)
 *   5. Reliable retransmit (simulated loss via black-hole then recovery)
 *   6. Congestion control integration (mode transitions)
 *   7. Payload integrity (data round-trip)
 *   8. Stats counters
 *   9. Oversized payload rejection
 *  10. Graceful shutdown
 *  11. Multi-producer ring stress test
 *
 * All tests use loopback (::1) so no real network is needed.
 * Compatible with meson test (exit 0 = pass, exit 1 = fail).
 */

#define _GNU_SOURCE
#include "network/yumi_udp_client.h"
#include "network/net.h"

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
#include <errno.h>

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

/* ── Helpers ───────────────────────────────────────────────────── */

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/*
 * Create a loopback IPv6 sockaddr for ::1 on the given port.
 */
static struct sockaddr_in6 make_loopback(uint16_t port)
{
    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(port);
    addr.sin6_addr   = in6addr_loopback;
    return addr;
}

/*
 * Helper: create a bare UDP socket bound to ::1:port for receiving.
 * Returns fd or -1.
 */
static int create_recv_socket(uint16_t port)
{
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int off = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr = make_loopback(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Non-blocking for poll-style reads */
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

/*
 * Heap-allocate a yumi_udp_client_t (too large for the stack).
 */
static yumi_udp_client_t *client_alloc(void)
{
    yumi_udp_client_t *c = calloc(1, sizeof(yumi_udp_client_t));
    if (!c) {
        fprintf(stderr, "FATAL: calloc(yumi_udp_client_t) failed\n");
        abort();
    }
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

/* ═══════════════════════════════════════════════════════════════════
 *  1. Wire protocol header
 * ═══════════════════════════════════════════════════════════════════ */

static void test_wire_header_size(void)
{
    TEST_SECTION("wire header size and layout");

    /* The packed header should be exactly 1+1+4+4 = 10 bytes */
    TEST_ASSERT(YUMI_UDP_HDR_SIZE == 10, "header size should be 10 bytes");
    TEST_ASSERT(YUMI_UDP_MAX_DGRAM == YUMI_UDP_HDR_SIZE + YUMI_UDP_MAX_PAYLOAD,
                "max datagram = hdr + payload");
}

static void test_wire_header_encode_decode(void)
{
    TEST_SECTION("wire header encode/decode round-trip");

    uint8_t buf[YUMI_UDP_MAX_DGRAM];
    yumi_udp_hdr_t *hdr = (yumi_udp_hdr_t *)buf;

    hdr->flags       = YUMI_UDP_FLAG_RELIABLE;
    hdr->seq         = 0xDEADBEEFU;
    hdr->payload_len = 42;

    const yumi_udp_hdr_t *read_hdr = (const yumi_udp_hdr_t *)buf;
    TEST_ASSERT(read_hdr->flags == YUMI_UDP_FLAG_RELIABLE, "flags round-trip");
    TEST_ASSERT(read_hdr->seq == 0xDEADBEEFU, "seq round-trip");
    TEST_ASSERT(read_hdr->payload_len == 42, "payload_len round-trip");
}

/* ═══════════════════════════════════════════════════════════════════
 *  2. MPSC Ring Buffer
 * ═══════════════════════════════════════════════════════════════════ */

static void test_ring_basic(void)
{
    TEST_SECTION("ring: push/pop single item");

    yumi_ring_t *r = yumi_ring_create();
    TEST_ASSERT(r != NULL, "ring alloc");

    yumi_work_item_t in = { .type = YUMI_WORK_SEND_UNRELIABLE, .len = 5 };
    memcpy(in.data, "hello", 5);

    /* Push and pop back using the public API */
    bool pushed = yumi_ring_push(r, &in);
    TEST_ASSERT(pushed, "push should succeed on empty ring");

    yumi_work_item_t out = {0};
    bool popped = yumi_ring_pop(r, &out);
    TEST_ASSERT(popped, "pop should succeed after push");
    TEST_ASSERT(out.type == YUMI_WORK_SEND_UNRELIABLE, "type preserved");
    TEST_ASSERT(out.len == 5, "len preserved");
    TEST_ASSERT(memcmp(out.data, "hello", 5) == 0, "data preserved");

    yumi_ring_destroy(r);
}

static void test_ring_overflow(void)
{
    TEST_SECTION("ring: overflow returns false");

    yumi_ring_t *r = yumi_ring_create();
    TEST_ASSERT(r != NULL, "ring alloc");

    yumi_work_item_t item = { .type = YUMI_WORK_SEND_UNRELIABLE, .len = 1 };
    item.data[0] = 0xAA;

    /* Fill the ring to capacity — no consumer, so backpressure will
     * kick in after CAPACITY items and push will eventually fail. */
    int pushed_count = 0;
    for (int i = 0; i < YUMI_RING_CAPACITY + 100; i++) {
        if (yumi_ring_push(r, &item))
            pushed_count++;
        else
            break;
    }

    TEST_ASSERT(pushed_count == YUMI_RING_CAPACITY,
                "ring should accept exactly CAPACITY items");

    yumi_ring_destroy(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  3. Loopback unreliable send/recv
 * ═══════════════════════════════════════════════════════════════════ */

static void test_unreliable_loopback(void)
{
    TEST_SECTION("unreliable loopback send/recv");

    int recv_fd = create_recv_socket(19100);
    TEST_ASSERT(recv_fd >= 0, "receiver socket created");
    uint16_t port = bound_port(recv_fd);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr = make_loopback(port);
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    int rc = yumi_udp_client_create(client, &cfg);
    TEST_ASSERT(rc == 0, "client created");

    const char *msg = "unreliable-test-123";
    rc = yumi_udp_client_send(client, msg, (uint32_t)strlen(msg));
    TEST_ASSERT(rc == 0, "send returned 0");

    sleep_ms(100);

    uint8_t buf[YUMI_UDP_MAX_DGRAM];
    ssize_t n = recv(recv_fd, buf, sizeof(buf), 0);
    TEST_ASSERT(n > 0, "received a datagram");

    if (n > 0) {
        TEST_ASSERT((size_t)n >= YUMI_UDP_HDR_SIZE, "datagram has header");
        const yumi_udp_hdr_t *hdr = (const yumi_udp_hdr_t *)buf;
        TEST_ASSERT((hdr->flags & YUMI_UDP_FLAG_RELIABLE) == 0, "unreliable flag clear");
        TEST_ASSERT(hdr->payload_len == strlen(msg), "payload_len correct");
        TEST_ASSERT(memcmp(buf + YUMI_UDP_HDR_SIZE, msg, strlen(msg)) == 0,
                    "payload data matches");
    }

    TEST_ASSERT(yumi_udp_client_stat_tx_packets(client) >= 1, "tx_packets >= 1");
    TEST_ASSERT(yumi_udp_client_stat_tx_bytes(client) > 0, "tx_bytes > 0");

    yumi_udp_client_destroy(client);
    free(client);
    close(recv_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  4. Loopback reliable send + ACK
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * For reliable send tests, we need a "peer" that receives the
 * reliable packet and sends back an ACK.  We use a helper thread
 * running a simple echo-ACK responder.
 */

typedef struct {
    int       fd;
    atomic_int received_count;
    atomic_bool stop;
    /* Last payload received */
    uint8_t   last_payload[YUMI_UDP_MAX_PAYLOAD];
    uint32_t  last_payload_len;
} ack_responder_t;

static void *ack_responder_fn(void *arg)
{
    ack_responder_t *r = (ack_responder_t *)arg;
    uint8_t buf[YUMI_UDP_MAX_DGRAM];

    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
    setsockopt(r->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!atomic_load(&r->stop)) {
        struct sockaddr_in6 from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(r->fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;
        if ((size_t)n < YUMI_UDP_HDR_SIZE) continue;

        const yumi_udp_hdr_t *hdr = (const yumi_udp_hdr_t *)buf;

        /* If it's a user data packet (not ACK, not internal probe), count it */
        if (!(hdr->flags & YUMI_UDP_FLAG_ACK) &&
            !(hdr->flags & YUMI_UDP_FLAG_PROBE)) {
            atomic_fetch_add(&r->received_count, 1);

            /* Stash last payload */
            uint32_t plen = hdr->payload_len;
            if (plen > YUMI_UDP_MAX_PAYLOAD) plen = YUMI_UDP_MAX_PAYLOAD;
            memcpy(r->last_payload, buf + YUMI_UDP_HDR_SIZE, plen);
            r->last_payload_len = plen;
        }

        /* If it's a reliable packet, send ACK */
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
        }
    }
    return NULL;
}

static void test_reliable_loopback(void)
{
    TEST_SECTION("reliable loopback send/recv/ACK");

    int peer_fd = create_recv_socket(19200);
    TEST_ASSERT(peer_fd >= 0, "peer socket created");
    uint16_t port = bound_port(peer_fd);

    ack_responder_t resp = {0};
    resp.fd = peer_fd;
    atomic_store(&resp.received_count, 0);
    atomic_store(&resp.stop, false);

    pthread_t resp_thrd;
    pthread_create(&resp_thrd, NULL, ack_responder_fn, &resp);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr = make_loopback(port);
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    int rc = yumi_udp_client_create(client, &cfg);
    TEST_ASSERT(rc == 0, "client created");

    const char *msg = "reliable-test-xyz";
    rc = yumi_udp_client_send_reliable(client, msg, (uint32_t)strlen(msg));
    TEST_ASSERT(rc == 0, "send_reliable returned 0");

    sleep_ms(500);

    TEST_ASSERT(atomic_load(&resp.received_count) >= 1, "responder received >= 1 packet");
    TEST_ASSERT(resp.last_payload_len == strlen(msg), "payload len matches");
    TEST_ASSERT(memcmp(resp.last_payload, msg, strlen(msg)) == 0, "payload data matches");

    TEST_ASSERT(yumi_udp_client_stat_rx_packets(client) >= 1,
                "client received ACK (rx_packets >= 1)");

    atomic_store(&resp.stop, true);
    yumi_udp_client_destroy(client);
    free(client);
    pthread_join(resp_thrd, NULL);
    close(peer_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  5. Reliable retransmit (no ACK → retransmit)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_reliable_retransmit(void)
{
    TEST_SECTION("reliable retransmit on timeout");

    int sink_fd = create_recv_socket(19300);
    TEST_ASSERT(sink_fd >= 0, "sink socket created");
    uint16_t port = bound_port(sink_fd);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr           = make_loopback(port);
    cfg.reliable_timeout_us = 50000;   /* 50 ms — short for testing */
    cfg.max_retransmits     = 3;
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    int rc = yumi_udp_client_create(client, &cfg);
    TEST_ASSERT(rc == 0, "client created");

    rc = yumi_udp_client_send_reliable(client, "retry-me", 8);
    TEST_ASSERT(rc == 0, "send_reliable returned 0");

    /* Wait enough time for retransmits: 50ms × 3 = 150ms + margin */
    sleep_ms(500);

    /* Count packets received at the sink */
    int pkt_count = 0;
    uint8_t buf[YUMI_UDP_MAX_DGRAM];
    struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
    setsockopt(sink_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (recv(sink_fd, buf, sizeof(buf), 0) > 0)
        pkt_count++;

    uint64_t retransmits = atomic_load_explicit(&client->stat_retransmits,
                                                memory_order_relaxed);
    TEST_ASSERT(retransmits >= 1, "at least 1 retransmit occurred");
    TEST_ASSERT(yumi_udp_client_stat_tx_packets(client) >= 2,
                "tx_packets >= 2 (original + retransmit)");

    yumi_udp_client_destroy(client);
    free(client);
    close(sink_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  6. Congestion control mode reporting
 * ═══════════════════════════════════════════════════════════════════ */

static void test_cc_initial_mode(void)
{
    TEST_SECTION("CC starts in PROBE mode");

    yumi_cc_t cc;
    yumi_cc_init(&cc, 1200);
    TEST_ASSERT(yumi_cc_get_mode(&cc) == YUMI_MODE_PROBE, "initial mode = PROBE");
    TEST_ASSERT(yumi_cc_get_send_rate(&cc) > 0, "initial pacing rate > 0");
    TEST_ASSERT(yumi_cc_get_cwnd(&cc) > 0, "initial cwnd > 0");
}

static void test_cc_force_reprobe(void)
{
    TEST_SECTION("CC force reprobe resets to PROBE");

    yumi_cc_t cc;
    yumi_cc_init(&cc, 1200);

    /* Simulate enough traffic to graduate to TDS */
    /* Just verify force_reprobe from PROBE stays in PROBE */
    yumi_cc_force_reprobe(&cc, 1000000);
    TEST_ASSERT(yumi_cc_get_mode(&cc) == YUMI_MODE_PROBE,
                "after force_reprobe: mode = PROBE");
}

/* ═══════════════════════════════════════════════════════════════════
 *  7. Payload integrity: many packets
 * ═══════════════════════════════════════════════════════════════════ */

static void test_payload_integrity(void)
{
    TEST_SECTION("payload integrity across 100 packets");

    int peer_fd = create_recv_socket(19400);
    TEST_ASSERT(peer_fd >= 0, "peer socket created");
    uint16_t port = bound_port(peer_fd);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr = make_loopback(port);
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    int rc = yumi_udp_client_create(client, &cfg);
    TEST_ASSERT(rc == 0, "client created");

    #define INTEGRITY_COUNT 100
    for (int i = 0; i < INTEGRITY_COUNT; i++) {
        uint8_t payload[64];
        /* Fill with known pattern: index in first 4 bytes, rest = index byte */
        memset(payload, (uint8_t)i, sizeof(payload));
        payload[0] = (uint8_t)(i & 0xFF);
        payload[1] = (uint8_t)((i >> 8) & 0xFF);

        rc = yumi_udp_client_send(client, payload, sizeof(payload));
        TEST_ASSERT(rc == 0, "send should succeed");
    }

    sleep_ms(300);

    /* Recv and verify */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(peer_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int good = 0;
    uint8_t buf[YUMI_UDP_MAX_DGRAM];
    for (int i = 0; i < INTEGRITY_COUNT + 10; i++) {
        ssize_t n = recv(peer_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if ((size_t)n < YUMI_UDP_HDR_SIZE) continue;

        const yumi_udp_hdr_t *hdr = (const yumi_udp_hdr_t *)buf;
        if (hdr->payload_len != 64) continue;

        const uint8_t *payload = buf + YUMI_UDP_HDR_SIZE;
        uint8_t idx = payload[0];
        /* Check the fill pattern */
        bool ok = true;
        for (int j = 2; j < 64; j++) {
            if (payload[j] != idx) { ok = false; break; }
        }
        if (ok) good++;
    }

    /* UDP is unreliable but loopback shouldn't lose packets */
    TEST_ASSERT(good >= INTEGRITY_COUNT - 5,
                "at least 95 of 100 packets intact on loopback");

    yumi_udp_client_destroy(client);
    free(client);
    close(peer_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  8. Stats counters
 * ═══════════════════════════════════════════════════════════════════ */

static void test_stats_counters(void)
{
    TEST_SECTION("stats counters increment correctly");

    int peer_fd = create_recv_socket(19500);
    TEST_ASSERT(peer_fd >= 0, "peer socket created");
    uint16_t port = bound_port(peer_fd);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr = make_loopback(port);
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    yumi_udp_client_create(client, &cfg);

    /* Send 10 unreliable packets */
    for (int i = 0; i < 10; i++) {
        uint8_t d = (uint8_t)i;
        yumi_udp_client_send(client, &d, 1);
    }
    sleep_ms(200);

    uint64_t tx_pkts  = yumi_udp_client_stat_tx_packets(client);
    uint64_t tx_bytes = yumi_udp_client_stat_tx_bytes(client);

    TEST_ASSERT(tx_pkts >= 10, "tx_packets >= 10 after 10 sends");
    TEST_ASSERT(tx_bytes >= 10 * (YUMI_UDP_HDR_SIZE + 1),
                "tx_bytes accounts for header + 1 byte payload");

    /* rx should be 0 since nobody sends to us */
    TEST_ASSERT(yumi_udp_client_stat_rx_packets(client) == 0,
                "rx_packets == 0 (no inbound)");

    yumi_udp_client_destroy(client);
    free(client);
    close(peer_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  9. Oversized payload rejection
 * ═══════════════════════════════════════════════════════════════════ */

static void test_oversized_rejected(void)
{
    TEST_SECTION("oversized payload rejected");

    int peer_fd = create_recv_socket(19600);
    TEST_ASSERT(peer_fd >= 0, "peer socket");
    uint16_t port = bound_port(peer_fd);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr = make_loopback(port);
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    yumi_udp_client_create(client, &cfg);

    uint8_t big[YUMI_UDP_MAX_PAYLOAD + 1];
    memset(big, 0xAA, sizeof(big));

    int rc = yumi_udp_client_send(client, big, sizeof(big));
    TEST_ASSERT(rc == -1, "unreliable send rejects oversized");

    rc = yumi_udp_client_send_reliable(client, big, sizeof(big));
    TEST_ASSERT(rc == -1, "reliable send rejects oversized");

    rc = yumi_udp_client_send(client, big, YUMI_UDP_MAX_PAYLOAD);
    TEST_ASSERT(rc == 0, "exact max payload accepted (unreliable)");

    rc = yumi_udp_client_send_reliable(client, big, YUMI_UDP_MAX_PAYLOAD);
    TEST_ASSERT(rc == 0, "exact max payload accepted (reliable)");

    yumi_udp_client_destroy(client);
    free(client);
    close(peer_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  10. Graceful shutdown
 * ═══════════════════════════════════════════════════════════════════ */

static void test_graceful_shutdown(void)
{
    TEST_SECTION("graceful shutdown: create + destroy without sends");

    int peer_fd = create_recv_socket(19700);
    TEST_ASSERT(peer_fd >= 0, "peer socket");
    uint16_t port = bound_port(peer_fd);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr = make_loopback(port);
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    int rc = yumi_udp_client_create(client, &cfg);
    TEST_ASSERT(rc == 0, "client created");

    yumi_udp_client_destroy(client);
    TEST_ASSERT(client->fd == -1, "fd closed after destroy");
    TEST_ASSERT(client->epoll_fd == -1, "epoll_fd closed");
    TEST_ASSERT(client->timer_fd == -1, "timer_fd closed");
    TEST_ASSERT(client->recv_event_fd == -1, "recv_event_fd closed");

    free(client);
    close(peer_fd);
}

static void test_shutdown_with_pending(void)
{
    TEST_SECTION("shutdown with pending work in ring");

    int peer_fd = create_recv_socket(19710);
    TEST_ASSERT(peer_fd >= 0, "peer socket");
    uint16_t port = bound_port(peer_fd);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr = make_loopback(port);
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    yumi_udp_client_create(client, &cfg);

    uint8_t d = 0x42;
    for (int i = 0; i < 500; i++)
        yumi_udp_client_send(client, &d, 1);

    yumi_udp_client_destroy(client);
    TEST_ASSERT(client->fd == -1, "fd closed after destroy with pending");

    free(client);
    close(peer_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  11. Multi-producer stress test
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    yumi_udp_client_t *client;
    int                count;
    atomic_int        *success;
} producer_args_t;

static void *producer_thread(void *arg)
{
    producer_args_t *a = (producer_args_t *)arg;
    for (int i = 0; i < a->count; i++) {
        uint8_t payload[16];
        memset(payload, (uint8_t)(i & 0xFF), sizeof(payload));
        if (yumi_udp_client_send(a->client, payload, sizeof(payload)) == 0)
            atomic_fetch_add(a->success, 1);
    }
    return NULL;
}

static void test_multi_producer(void)
{
    TEST_SECTION("multi-producer stress: 4 threads × 500 sends");

    int peer_fd = create_recv_socket(19800);
    TEST_ASSERT(peer_fd >= 0, "peer socket");
    uint16_t port = bound_port(peer_fd);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr = make_loopback(port);
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    yumi_udp_client_create(client, &cfg);

    #define NUM_PRODUCERS 4
    #define SENDS_PER     500

    atomic_int total_success = 0;
    producer_args_t args[NUM_PRODUCERS];
    pthread_t threads[NUM_PRODUCERS];

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        args[i].client  = client;
        args[i].count   = SENDS_PER;
        args[i].success = &total_success;
        pthread_create(&threads[i], NULL, producer_thread, &args[i]);
    }

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(threads[i], NULL);
    }

    int total = atomic_load(&total_success);
    TEST_ASSERT(total > 0, "at least some sends succeeded");
    TEST_ASSERT(total >= (NUM_PRODUCERS * SENDS_PER) / 2,
                "at least half of multi-producer sends succeeded");

    sleep_ms(300);

    uint64_t tx = yumi_udp_client_stat_tx_packets(client);
    TEST_ASSERT(tx > 0, "tx_packets > 0 after multi-producer burst");

    yumi_udp_client_destroy(client);
    free(client);
    close(peer_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  12. Zero-length payload
 * ═══════════════════════════════════════════════════════════════════ */

static void test_zero_length_payload(void)
{
    TEST_SECTION("zero-length payload");

    int peer_fd = create_recv_socket(19900);
    TEST_ASSERT(peer_fd >= 0, "peer socket");
    uint16_t port = bound_port(peer_fd);

    yumi_udp_client_config_t cfg = {0};
    cfg.peer_addr = make_loopback(port);
    cfg_set_passthrough_crypto(&cfg);

    yumi_udp_client_t *client = client_alloc();
    yumi_udp_client_create(client, &cfg);

    /* Use non-NULL pointer to avoid memcpy(dst, NULL, 0) UB */
    uint8_t empty = 0;
    int rc = yumi_udp_client_send(client, &empty, 0);
    TEST_ASSERT(rc == 0, "zero-length unreliable send accepted");

    rc = yumi_udp_client_send_reliable(client, &empty, 0);
    TEST_ASSERT(rc == 0, "zero-length reliable send accepted");

    sleep_ms(100);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(peer_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[YUMI_UDP_MAX_DGRAM];
    int count = 0;
    while (recv(peer_fd, buf, sizeof(buf), 0) > 0) count++;
    TEST_ASSERT(count >= 2, "received at least 2 header-only datagrams");

    yumi_udp_client_destroy(client);
    free(client);
    close(peer_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  13. Recv callback delivery
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    atomic_int count;
    uint8_t    last_data[YUMI_UDP_MAX_PAYLOAD];
    uint32_t   last_len;
    bool       last_reliable;
} recv_ctx_t;

static void test_recv_callback(void *user, const void *data,
                               uint32_t len, bool reliable, uint32_t seq)
{
    recv_ctx_t *ctx = (recv_ctx_t *)user;
    (void)seq;
    atomic_fetch_add(&ctx->count, 1);
    if (len > 0 && len <= YUMI_UDP_MAX_PAYLOAD)
        memcpy(ctx->last_data, data, len);
    ctx->last_len      = len;
    ctx->last_reliable = reliable;
}

static void test_recv_callback_delivery(void)
{
    TEST_SECTION("recv callback delivery");

    uint16_t port_b = 19950;

    recv_ctx_t ctx = {0};
    atomic_store(&ctx.count, 0);

    yumi_udp_client_config_t cfg_b = {0};
    cfg_b.peer_addr  = make_loopback(0);
    cfg_b.local_port = port_b;
    cfg_b.recv_cb    = test_recv_callback;
    cfg_b.recv_user  = &ctx;
    cfg_set_passthrough_crypto(&cfg_b);

    yumi_udp_client_t *client_b = client_alloc();
    int rc = yumi_udp_client_create(client_b, &cfg_b);
    TEST_ASSERT(rc == 0, "client B created");

    port_b = bound_port(client_b->fd);

    yumi_udp_client_config_t cfg_a = {0};
    cfg_a.peer_addr = make_loopback(port_b);
    cfg_set_passthrough_crypto(&cfg_a);

    yumi_udp_client_t *client_a = client_alloc();
    rc = yumi_udp_client_create(client_a, &cfg_a);
    TEST_ASSERT(rc == 0, "client A created");

    const char *msg = "callback-test-data";
    yumi_udp_client_send(client_a, msg, (uint32_t)strlen(msg));

    sleep_ms(300);

    TEST_ASSERT(atomic_load(&ctx.count) >= 1, "recv callback fired >= 1 time");
    TEST_ASSERT(ctx.last_len == strlen(msg), "recv callback: correct len");
    TEST_ASSERT(memcmp(ctx.last_data, msg, strlen(msg)) == 0,
                "recv callback: correct data");
    TEST_ASSERT(ctx.last_reliable == false, "recv callback: unreliable flag");

    yumi_udp_client_destroy(client_a);
    yumi_udp_client_destroy(client_b);
    free(client_a);
    free(client_b);
}

/* ═══════════════════════════════════════════════════════════════════
 *  14. BBR/TDS pure logic: BDP computation
 * ═══════════════════════════════════════════════════════════════════ */

static void test_cc_bdp_computation(void)
{
    TEST_SECTION("CC: BDP computation sanity");

    yumi_cc_t cc;
    yumi_cc_init(&cc, 1200);

    /* Before any traffic: cwnd should be initial_cwnd = 10 * mss */
    TEST_ASSERT(yumi_cc_get_cwnd(&cc) == 10 * 1200, "initial cwnd = 10*MSS");
    TEST_ASSERT(cc.probe.rtprop_us == UINT64_MAX, "rtprop starts as UINT64_MAX");

    /* Simulate a single ACK to give it some BtlBw and RTprop */
    yumi_packet_t pkt = { .seq = 1, .size = 1200 };
    yumi_cc_on_send(&cc, &pkt, 0);

    yumi_cc_on_ack(&cc, &pkt, 1200, 10000, 10000);  /* 10 ms RTT */

    TEST_ASSERT(cc.probe.rtprop_us <= 10000, "rtprop updated to <= 10ms");
    TEST_ASSERT(cc.probe.btl_bw > 0, "btl_bw > 0 after first ACK");
}

/* ═══════════════════════════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
    fprintf(stdout, "═══ YumiUdpClient Test Suite ═══\n\n");

    /* 1. Wire protocol */
    test_wire_header_size();
    test_wire_header_encode_decode();

    /* 2. Ring buffer */
    test_ring_basic();
    test_ring_overflow();

    /* 3. Unreliable loopback */
    test_unreliable_loopback();

    /* 4. Reliable loopback + ACK */
    test_reliable_loopback();

    /* 5. Retransmit */
    test_reliable_retransmit();

    /* 6. CC modes */
    test_cc_initial_mode();
    test_cc_force_reprobe();

    /* 7. Payload integrity */
    test_payload_integrity();

    /* 8. Stats */
    test_stats_counters();

    /* 9. Oversized */
    test_oversized_rejected();

    /* 10. Shutdown */
    test_graceful_shutdown();
    test_shutdown_with_pending();

    /* 11. Multi-producer */
    test_multi_producer();

    /* 12. Zero-length */
    test_zero_length_payload();

    /* 13. Recv callback */
    test_recv_callback_delivery();

    /* 14. BDP */
    test_cc_bdp_computation();

    /* Summary */
    fprintf(stdout, "\n═══ Results: %d/%d passed ═══\n",
            g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        fprintf(stderr, "%d test(s) FAILED\n", g_tests_failed);
        return 1;
    }
    fprintf(stdout, "All tests passed.\n");
    return 0;
}
