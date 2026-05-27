/*
 * yumi_udp.c - Shared UDP utilities: lock-free MPSC ring buffer with spin-then-sleep backpressure for the Yumi UDP transport layer.
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
 * yumi_udp.c — Shared UDP utilities: MPSC ring buffer with backpressure
 *
 * Lock-free MPSC ring buffer for the Yumi UDP transport layer.
 * When the ring is full, producers spin briefly (256 iterations),
 * then sleep in 1 ms increments for up to 1 second before giving up.
 * This provides natural backpressure when the worker thread (doing
 * real crypto) can't drain fast enough.
 */

#define _GNU_SOURCE
#include "network/net.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Ring buffer — heap allocation with backpressure
 * ════════════════════════════════════════════════════════════════════════ */

yumi_ring_t *yumi_ring_create(void)
{
    yumi_ring_t *r = calloc(1, sizeof(yumi_ring_t));
    if (!r) return NULL;

    r->slots = calloc(YUMI_RING_CAPACITY, sizeof(yumi_work_item_t));
    r->ready = calloc(YUMI_RING_CAPACITY, sizeof(atomic_int));
    if (!r->slots || !r->ready) {
        free(r->slots);
        free(r->ready);
        free(r);
        return NULL;
    }

    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    return r;
}

void yumi_ring_destroy(yumi_ring_t *r)
{
    if (!r) return;
    free(r->slots);
    free(r->ready);
    free(r);
}

void yumi_ring_init(yumi_ring_t *r)
{
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    for (int i = 0; i < YUMI_RING_CAPACITY; i++)
        atomic_store_explicit(&r->ready[i], 0, memory_order_relaxed);
}

/* ────────────────────────────────────────────────────────────────────────
 *  Backpressure helper — spin then sleep, return false after 1 second
 *
 *  Phase 1 (spins < SPIN_MAX): tight CAS-retry loop, no syscalls.
 *  Phase 2 (sleeps < SLEEP_MAX): usleep(1 ms) per iteration.
 *  Phase 3: give up — return false.
 * ──────────────────────────────────────────────────────────────────────── */

static inline bool ring_backpressure(int *spins, int *sleeps)
{
    if (*spins < YUMI_RING_SPIN_MAX) {
        (*spins)++;
        return true;   /* keep trying */
    }
    if (*sleeps < YUMI_RING_SLEEP_MAX) {
        usleep(YUMI_RING_SLEEP_US);
        (*sleeps)++;
        return true;   /* keep trying */
    }
    return false;      /* 1 second elapsed — give up */
}

/* ────────────────────────────────────────────────────────────────────────
 *  MPSC push — safe from multiple producer threads.
 *  Applies backpressure when the ring is full.
 * ──────────────────────────────────────────────────────────────────────── */

bool yumi_ring_push(yumi_ring_t *r, const yumi_work_item_t *item)
{
    int spins = 0, sleeps = 0;
    uint_fast64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    for (;;) {
        uint_fast64_t tail = atomic_load_explicit(&r->tail,
                                                   memory_order_acquire);
        if (head - tail >= YUMI_RING_CAPACITY) {
            if (!ring_backpressure(&spins, &sleeps))
                return false;
            head = atomic_load_explicit(&r->head, memory_order_relaxed);
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(
                &r->head, &head, head + 1,
                memory_order_acq_rel, memory_order_relaxed)) {
            uint_fast64_t idx = head & YUMI_RING_MASK;
            r->slots[idx] = *item;
            atomic_store_explicit(&r->ready[idx], 1, memory_order_release);
            return true;
        }
    }
}

/* ────────────────────────────────────────────────────────────────────────
 *  Single-consumer pop — only the worker thread calls this.
 * ──────────────────────────────────────────────────────────────────────── */

bool yumi_ring_pop(yumi_ring_t *r, yumi_work_item_t *out)
{
    uint_fast64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint_fast64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail >= head)
        return false;   /* empty */

    uint_fast64_t idx = tail & YUMI_RING_MASK;
    if (!atomic_load_explicit(&r->ready[idx], memory_order_acquire))
        return false;   /* slot not committed yet */

    *out = r->slots[idx];
    atomic_store_explicit(&r->ready[idx], 0, memory_order_release);
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return true;
}

/* ────────────────────────────────────────────────────────────────────────
 *  Zero-copy consumer: peek at the next item without copying, then
 *  advance after processing.  Only the single consumer thread may call.
 * ──────────────────────────────────────────────────────────────────────── */

const yumi_work_item_t *yumi_ring_peek(yumi_ring_t *r)
{
    uint_fast64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint_fast64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail >= head)
        return NULL;

    uint_fast64_t idx = tail & YUMI_RING_MASK;
    if (!atomic_load_explicit(&r->ready[idx], memory_order_acquire))
        return NULL;

    return &r->slots[idx];
}

void yumi_ring_advance(yumi_ring_t *r)
{
    uint_fast64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint_fast64_t idx = tail & YUMI_RING_MASK;
    atomic_store_explicit(&r->ready[idx], 0, memory_order_release);
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
}

/* ────────────────────────────────────────────────────────────────────────
 *  Zero-copy reserve+commit.
 *
 *  reserve() CAS-reserves a slot.  If the ring is full, applies
 *  spin→sleep backpressure for up to 1 second before returning NULL.
 * ──────────────────────────────────────────────────────────────────────── */

yumi_work_item_t *yumi_ring_reserve(yumi_ring_t *r)
{
    int spins = 0, sleeps = 0;
    uint_fast64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    for (;;) {
        uint_fast64_t tail = atomic_load_explicit(&r->tail,
                                                   memory_order_acquire);
        if (head - tail >= YUMI_RING_CAPACITY) {
            if (!ring_backpressure(&spins, &sleeps))
                return NULL;
            head = atomic_load_explicit(&r->head, memory_order_relaxed);
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(
                &r->head, &head, head + 1,
                memory_order_acq_rel, memory_order_relaxed)) {
            return &r->slots[head & YUMI_RING_MASK];
        }
    }
}

/* Non-blocking try-reserve: single CAS attempt, returns NULL on
 * contention or if the ring is full.  Used by recv workers to enqueue
 * ACKs/retransmits without blocking the recv path. */
yumi_work_item_t *yumi_ring_try_reserve(yumi_ring_t *r)
{
    uint_fast64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint_fast64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - tail >= YUMI_RING_CAPACITY)
        return NULL;
    if (!atomic_compare_exchange_strong_explicit(
            &r->head, &head, head + 1,
            memory_order_acq_rel, memory_order_relaxed))
        return NULL;
    return &r->slots[head & YUMI_RING_MASK];
}

void yumi_ring_commit(yumi_ring_t *r, yumi_work_item_t *slot)
{
    uint_fast64_t idx = (uint_fast64_t)(slot - r->slots);
    atomic_store_explicit(&r->ready[idx], 1, memory_order_release);
}
