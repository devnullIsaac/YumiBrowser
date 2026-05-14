/*
 * net.h — Yumi UDP wire protocol and shared types
 *
 * Wire format, MPSC lock-free ring buffer, channel definitions.
 * All data structures are designed for heap allocation.
 */

#ifndef YUMI_NET_WIRE_H
#define YUMI_NET_WIRE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <netinet/in.h>

/* ── Wire Protocol ─────────────────────────────────────────────────────── */

#define YUMI_UDP_HDR_SIZE     10            /* 1 + 1 + 4 + 4               */

/*
 * Compile-time max payload — sized for standard 1500 MTU Ethernet:
 *   1500 - 40 (IPv6) - 8 (UDP) - 10 (Yumi hdr) = 1442
 * Runtime max_payload (from NIC MTU discovery) may be smaller.
 */
#define YUMI_UDP_MAX_PAYLOAD  1442
#define YUMI_UDP_MAX_DGRAM    (YUMI_UDP_HDR_SIZE + YUMI_UDP_MAX_PAYLOAD)

/* Flag bits in the flags byte (bits 3-7 reserved for future use) */
#define YUMI_UDP_FLAG_RELIABLE       0x01
#define YUMI_UDP_FLAG_ACK            0x02
#define YUMI_UDP_FLAG_PROBE          0x04   /* internal CC probe packet    */

#define YUMI_UDP_MAX_CHANNELS  256

/*
 * Packed wire header — exactly 10 bytes.
 *   flags       (1)   reliability flags
 *   channel     (1)   channel ID (0-255)
 *   seq         (4)   monotonic sequence number (uint32_t, wraps)
 *   payload_len (4)   payload byte count
 */
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  channel;
    uint32_t seq;
    uint32_t payload_len;
} yumi_udp_hdr_t;

_Static_assert(sizeof(yumi_udp_hdr_t) == YUMI_UDP_HDR_SIZE,
               "yumi_udp_hdr_t must be exactly 10 bytes");

/* ── MPSC Ring Buffer with backpressure (heap-allocated) ───────────────── */

#define YUMI_RING_CAPACITY      4096
#define YUMI_RING_MASK          (YUMI_RING_CAPACITY - 1)

/* Backpressure: when full, spin SPIN_MAX times, then usleep(1 ms)
 * up to SLEEP_MAX times (~1 s).  After that, return error. */
#define YUMI_RING_SPIN_MAX      256
#define YUMI_RING_SLEEP_US      1000
#define YUMI_RING_SLEEP_MAX     1000

typedef enum {
    YUMI_WORK_SEND_UNRELIABLE = 0,
    YUMI_WORK_SEND_RELIABLE   = 1,
    YUMI_WORK_SHUTDOWN        = 2,
    YUMI_WORK_USER_CB         = 3,
    YUMI_WORK_SEND_RAW        = 4,  /* pre-built header: encrypt+sendto only */
} yumi_work_type_t;

typedef struct {
    yumi_work_type_t    type;
    uint32_t            len;
    uint8_t             channel;
    uint8_t             raw_flags;   /* SEND_RAW: wire flags              */
    uint32_t            raw_seq;     /* SEND_RAW: sequence number         */
    bool                has_dest;
    struct sockaddr_in6 dest;
    uint8_t             data[YUMI_UDP_MAX_PAYLOAD];
} yumi_work_item_t;

/*
 * MPSC ring with backpressure: multiple producers CAS on head, single
 * consumer owns tail.  When full, producers spin briefly then sleep
 * in 1 ms increments (up to 1 second) to let the worker drain.
 *
 * _Alignas(64) prevents false sharing between head and tail.
 */
typedef struct {
    _Alignas(64) atomic_uint_fast64_t head;
    _Alignas(64) atomic_uint_fast64_t tail;
    yumi_work_item_t                 *slots;
    atomic_int                       *ready;
} yumi_ring_t;

/* Ring buffer API (implemented in yumi_udp.c) */
yumi_ring_t *yumi_ring_create(void);
void         yumi_ring_destroy(yumi_ring_t *r);
void         yumi_ring_init(yumi_ring_t *r);
bool         yumi_ring_push(yumi_ring_t *r, const yumi_work_item_t *item);
bool         yumi_ring_pop(yumi_ring_t *r, yumi_work_item_t *out);

/* Zero-copy consumer: peek at the next committed slot without copying.
 * Returns NULL if empty or slot not committed yet.
 * Call yumi_ring_advance() to release after processing. */
const yumi_work_item_t *yumi_ring_peek(yumi_ring_t *r);
void                     yumi_ring_advance(yumi_ring_t *r);

/* Zero-copy enqueue: reserve a slot (CAS on head), fill it directly,
 * then commit.  Eliminates the per-enqueue malloc/copy/free.
 * If the ring is full, applies same spin→sleep backpressure. */
yumi_work_item_t *yumi_ring_reserve(yumi_ring_t *r);
yumi_work_item_t *yumi_ring_try_reserve(yumi_ring_t *r);
void              yumi_ring_commit(yumi_ring_t *r, yumi_work_item_t *slot);

#endif /* YUMI_NET_WIRE_H */
