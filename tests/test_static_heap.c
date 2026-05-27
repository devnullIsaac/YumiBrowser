/*
 * test_static_heap.c - Multithreaded tests for the static_heap allocator: per-thread caches, refill batching, contention, ABA-safety, and exhaustion.
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

/**
 * @file test_static_heap.c
 * @brief Multithreaded tests for static_heap allocator
 */

#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h>
#include <string.h>
#include <stdlib.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-40s", #name); name(); printf("PASS\n"); } while(0)

#ifndef YUMI_CACHE_LINE_SIZE
#define YUMI_CACHE_LINE_SIZE 64
#endif

#ifndef YUMI_STATIC_HEAP_MAX_THREADS
#define YUMI_STATIC_HEAP_MAX_THREADS 64
#endif
_Static_assert(YUMI_STATIC_HEAP_MAX_THREADS > 0,
    "YUMI_STATIC_HEAP_MAX_THREADS must be > 0");

#ifndef YUMI_STATIC_HEAP_REFILL_BATCH
#define YUMI_STATIC_HEAP_REFILL_BATCH 32
#endif

#ifndef YUMI_STATIC_HEAP_TLS_CACHE_SIZE
#define YUMI_STATIC_HEAP_TLS_CACHE_SIZE 64
#endif
_Static_assert(YUMI_STATIC_HEAP_TLS_CACHE_SIZE >= YUMI_STATIC_HEAP_REFILL_BATCH,
    "TLS cache must be at least one refill batch in size");
_Static_assert((YUMI_STATIC_HEAP_TLS_CACHE_SIZE & 1) == 0,
    "TLS cache size must be even (spill takes half)");

#if defined(__GNUC__) || defined(__clang__)
#define YUMI_LIKELY(x)        __builtin_expect(!!(x), 1)
#define YUMI_UNLIKELY(x)      __builtin_expect(!!(x), 0)
#define YUMI_COLD             __attribute__((cold, noinline))
#define YUMI_ALWAYS_INLINE    __attribute__((always_inline)) inline
#define YUMI_DO_NOT_OPTIMIZE(p) __asm__ __volatile__("" : : "g"(p) : "memory")
#else
#define YUMI_LIKELY(x)        (x)
#define YUMI_UNLIKELY(x)      (x)
#define YUMI_COLD
#define YUMI_ALWAYS_INLINE    inline
#define YUMI_DO_NOT_OPTIMIZE(p) ((void)(p))
#endif

#ifdef YUMI_STATIC_HEAP_DEBUG
#define YUMI_STATIC_HEAP_DEBUG_RELEASE_CHECK(x, item) do { \
    assert((char*)(item) >= (char*)&x##_buffer[0]); \
    assert((char*)(item) < ((char*)&x##_buffer[0]) + sizeof(x##_buffer)); \
    for (int32_t _dbg_i = 0; _dbg_i < x##_tls.top; ++_dbg_i) { \
        assert(x##_tls.stack[_dbg_i] != (item) && "static_heap: double release detected"); \
    } \
} while (0)
#define YUMI_STATIC_HEAP_DEBUG_LEASE_MARK(x, item) do { \
    assert((char*)(item) >= (char*)&x##_buffer[0]); \
    assert((char*)(item) < ((char*)&x##_buffer[0]) + sizeof(x##_buffer)); \
} while (0)
#else
#define YUMI_STATIC_HEAP_DEBUG_RELEASE_CHECK(x, item) ((void)0)
#define YUMI_STATIC_HEAP_DEBUG_LEASE_MARK(x, item)    ((void)0)
#endif

/* Error codes */
typedef enum {
    YUMI_MEMORY_ALLOC_SUCCESS = 0,
    YUMI_MEMORY_ALLOC_INVALID_IN_POINTER,
    YUMI_MEMORY_ALLOC_INVALID_OUT_POINTER,
    YUMI_MEMORY_ALLOC_UNAVAILABLE_MEMORY,
    YUMI_MEMORY_ALLOC_INTERNAL_ERROR
} yumi_memory_alloc_error_enum;

/* Test structure */
typedef struct {
    uint32_t id;
    uint32_t magic;
    uint8_t data[56]; /* Total 64 bytes */
} TestObject;

#define static_heap(x, y) \
typedef struct x##_linked_list_node { \
    x bindings; \
    struct x##_linked_list_node* next; \
} *x##_linked_list_node; \
_Static_assert(offsetof(struct x##_linked_list_node, bindings) == 0, \
    "bindings must be at offset 0 (release_inline relies on this cast)"); \
_Static_assert(_Alignof(struct x##_linked_list_node) >= _Alignof(x), \
    "node alignment must satisfy user struct alignment"); \
static struct x##_linked_list_node x##_buffer[y] = {0}; \
static x##_linked_list_node x##_linked_list_node_head = NULL; \
static size_t x##_global_count = 0; \
static mtx_t x##_global_mtx; \
static bool x##_global_mtx_ok = false; \
static once_flag x##_global_mtx_once = ONCE_FLAG_INIT; \
static _Atomic(uintptr_t) x##_recovery_slots[YUMI_STATIC_HEAP_MAX_THREADS] = {0}; \
static YUMI_ALWAYS_INLINE size_t x##_recovery_index_for_current_thread(void) \
{ \
    thrd_t tid = thrd_current(); \
    const unsigned char* bytes = (const unsigned char*)&tid; \
    size_t h = 1469598103934665603ull; \
    for (size_t i = 0; i < sizeof(tid); ++i) \
    { \
        h ^= (size_t)bytes[i]; \
        h *= 1099511628211ull; \
    } \
    return h % (size_t)YUMI_STATIC_HEAP_MAX_THREADS; \
} \
static YUMI_ALWAYS_INLINE void x##_recovery_push(x##_linked_list_node item) \
{ \
    size_t idx = x##_recovery_index_for_current_thread(); \
    _Atomic(uintptr_t)* slot = &x##_recovery_slots[idx]; \
    uintptr_t expected = atomic_load_explicit(slot, memory_order_relaxed); \
    for (;;) \
    { \
        item->next = (x##_linked_list_node)expected; \
        if (atomic_compare_exchange_weak_explicit( \
                slot, \
                &expected, \
                (uintptr_t)item, \
                memory_order_release, \
                memory_order_relaxed)) \
        { \
            break; \
        } \
    } \
} \
static YUMI_ALWAYS_INLINE x##_linked_list_node x##_recovery_pop_any(void) \
{ \
    x##_linked_list_node out = NULL; \
    size_t start = x##_recovery_index_for_current_thread(); \
    for (size_t probe = 0; probe < (size_t)YUMI_STATIC_HEAP_MAX_THREADS; ++probe) \
    { \
        size_t idx = (start + probe) % (size_t)YUMI_STATIC_HEAP_MAX_THREADS; \
        _Atomic(uintptr_t)* slot = &x##_recovery_slots[idx]; \
        uintptr_t head = atomic_load_explicit(slot, memory_order_acquire); \
        while (head != (uintptr_t)0) \
        { \
            x##_linked_list_node item = (x##_linked_list_node)head; \
            uintptr_t next = (uintptr_t)item->next; \
            if (atomic_compare_exchange_weak_explicit( \
                    slot, \
                    &head, \
                    next, \
                    memory_order_acq_rel, \
                    memory_order_acquire)) \
            { \
                out = item; \
                break; \
            } \
        } \
        if (out != NULL) \
        { \
            break; \
        } \
    } \
    return out; \
} \
static void x##_global_mtx_init(void) \
{ \
    x##_global_mtx_ok = (mtx_init(&x##_global_mtx, mtx_plain) == thrd_success); \
} \
YUMI_COLD static bool x##_lock_global(void) \
{ \
    bool ok = false; \
    call_once(&x##_global_mtx_once, x##_global_mtx_init); \
    if (x##_global_mtx_ok == true) \
    { \
        if (mtx_lock(&x##_global_mtx) == thrd_success) \
        { \
            ok = true; \
        } \
    } \
    return ok; \
} \
typedef struct { \
    _Alignas(YUMI_CACHE_LINE_SIZE) int32_t top; \
    bool registered; \
    x##_linked_list_node stack[YUMI_STATIC_HEAP_TLS_CACHE_SIZE]; \
} x##_tls_slot_t; \
static thread_local x##_tls_slot_t x##_tls = {0}; \
static tss_t x##_tls_key; \
static bool x##_tls_key_ok = false; \
static once_flag x##_tls_once = ONCE_FLAG_INIT; \
static void x##_tls_flush(void* unused_arg) \
{ \
    (void)unused_arg; \
    int32_t cached = x##_tls.top; \
    if (cached > 0) \
    { \
        int32_t i = 0; \
        for (; i < cached - 1; ++i) \
        { \
            x##_tls.stack[i]->next = x##_tls.stack[i + 1]; \
        } \
        x##_linked_list_node chain_head = x##_tls.stack[0]; \
        x##_linked_list_node chain_tail = x##_tls.stack[cached - 1]; \
        if (x##_lock_global() == true) \
        { \
            x##_tls.top = 0; \
            chain_tail->next = x##_linked_list_node_head; \
            x##_linked_list_node_head = chain_head; \
            x##_global_count += (size_t)cached; \
            (void)mtx_unlock(&x##_global_mtx); \
        } \
        else \
        { \
            for (i = 0; i < cached; ++i) \
            { \
                x##_recovery_push(x##_tls.stack[i]); \
            } \
            x##_tls.top = 0; \
        } \
    } \
} \
static void x##_tls_key_init(void) \
{ \
    x##_tls_key_ok = (tss_create(&x##_tls_key, x##_tls_flush) == thrd_success); \
} \
YUMI_COLD static bool x##_register_tls(void) \
{ \
    bool ok = false; \
    call_once(&x##_tls_once, x##_tls_key_init); \
    if (x##_tls_key_ok == true) \
    { \
        if (tss_set(x##_tls_key, (void*)&x##_tls) == thrd_success) \
        { \
            x##_tls.registered = true; \
            ok = true; \
        } \
    } \
    return ok; \
} \
static YUMI_ALWAYS_INLINE bool x##_ensure_tls(void) \
{ \
    bool ok = true; \
    if (YUMI_UNLIKELY(x##_tls.registered == false)) \
    { \
        ok = x##_register_tls(); \
    } \
    return ok; \
} \
void init_static_heap_##x(void) \
{ \
    bool locked = x##_lock_global(); \
    if (locked == true) \
    { \
        const size_t limit = sizeof(x##_buffer) / sizeof(struct x##_linked_list_node); \
        for (size_t i = 1; i < limit; ++i) \
        { \
            x##_buffer[i - 1].next = &x##_buffer[i]; \
        } \
        x##_buffer[limit - 1].next = NULL; \
        x##_tls.top = 0; \
        x##_linked_list_node_head = &x##_buffer[0]; \
        x##_global_count = limit; \
        for (size_t i = 0; i < (size_t)YUMI_STATIC_HEAP_MAX_THREADS; ++i) \
        { \
            atomic_store_explicit(&x##_recovery_slots[i], (uintptr_t)0, memory_order_relaxed); \
        } \
        (void)mtx_unlock(&x##_global_mtx); \
    } \
} \
YUMI_COLD static yumi_memory_alloc_error_enum x##_refill_from_global(x** outNode) \
{ \
    yumi_memory_alloc_error_enum res = YUMI_MEMORY_ALLOC_UNAVAILABLE_MEMORY; \
    if (x##_ensure_tls() == false) \
    { \
        return YUMI_MEMORY_ALLOC_INTERNAL_ERROR; \
    } \
    if (x##_lock_global() == true) \
    { \
        x##_linked_list_node head = x##_linked_list_node_head; \
        if (head != NULL) \
        { \
            size_t fair_share = (x##_global_count + 1U) / 2U; \
            size_t want = (size_t)YUMI_STATIC_HEAP_REFILL_BATCH; \
            if (want > fair_share) { want = fair_share; } \
            if (want < 1U) { want = 1U; } \
            x##_linked_list_node cur = head; \
            int32_t stored = 0; \
            size_t want_in_tls = want - 1U; \
            for (; ((size_t)stored < want_in_tls) && (cur->next != NULL); ++stored) \
            { \
                x##_tls.stack[stored] = cur; \
                cur = cur->next; \
            } \
            x##_linked_list_node next_head = cur->next; \
            x##_linked_list_node_head = next_head; \
            size_t taken = (size_t)stored + 1U; \
            x##_global_count -= taken; \
            x##_tls.top = stored; \
            YUMI_STATIC_HEAP_DEBUG_LEASE_MARK(x, cur); \
            *outNode = &cur->bindings; \
            res = YUMI_MEMORY_ALLOC_SUCCESS; \
        } \
        (void)mtx_unlock(&x##_global_mtx); \
    } \
    if (res != YUMI_MEMORY_ALLOC_SUCCESS) \
    { \
        x##_linked_list_node recovered = x##_recovery_pop_any(); \
        if (recovered != NULL) \
        { \
            YUMI_STATIC_HEAP_DEBUG_LEASE_MARK(x, recovered); \
            *outNode = &recovered->bindings; \
            res = YUMI_MEMORY_ALLOC_SUCCESS; \
        } \
        else if (x##_global_mtx_ok == false) \
        { \
            res = YUMI_MEMORY_ALLOC_INTERNAL_ERROR; \
        } \
    } \
    return res; \
} \
YUMI_COLD static bool x##_spill_to_global(x##_linked_list_node new_item) \
{ \
    bool ok = x##_lock_global(); \
    if (ok == true) \
    { \
        const int32_t half = YUMI_STATIC_HEAP_TLS_CACHE_SIZE / 2; \
        int32_t i = 0; \
        for (; i < half - 1; ++i) \
        { \
            x##_tls.stack[i]->next = x##_tls.stack[i + 1]; \
        } \
        x##_linked_list_node chain_head = x##_tls.stack[0]; \
        x##_linked_list_node chain_tail = x##_tls.stack[half - 1]; \
        for (i = 0; i < half; ++i) \
        { \
            x##_tls.stack[i] = x##_tls.stack[half + i]; \
        } \
        x##_tls.stack[half] = new_item; \
        x##_tls.top = half + 1; \
        chain_tail->next = x##_linked_list_node_head; \
        x##_linked_list_node_head = chain_head; \
        x##_global_count += (size_t)half; \
        (void)mtx_unlock(&x##_global_mtx); \
    } \
    else \
    { \
        x##_recovery_push(new_item); \
        ok = true; \
    } \
    return ok; \
} \
static inline yumi_memory_alloc_error_enum release_##x##_inline(x* node) \
{ \
    yumi_memory_alloc_error_enum res = YUMI_MEMORY_ALLOC_SUCCESS; \
    if (node == NULL) \
    { \
        res = YUMI_MEMORY_ALLOC_INVALID_IN_POINTER; \
    } \
    if (res == YUMI_MEMORY_ALLOC_SUCCESS) \
    { \
        if (x##_ensure_tls() == false) \
        { \
            res = YUMI_MEMORY_ALLOC_INTERNAL_ERROR; \
        } \
    } \
    if (res == YUMI_MEMORY_ALLOC_SUCCESS) \
    { \
        x##_linked_list_node item = (x##_linked_list_node)node; \
        YUMI_STATIC_HEAP_DEBUG_RELEASE_CHECK(x, item); \
        if (YUMI_LIKELY(x##_tls.top < YUMI_STATIC_HEAP_TLS_CACHE_SIZE)) \
        { \
            x##_tls.stack[x##_tls.top] = item; \
            x##_tls.top++; \
        } \
        else \
        { \
            if (x##_spill_to_global(item) == false) \
            { \
                res = YUMI_MEMORY_ALLOC_INTERNAL_ERROR; \
            } \
        } \
    } \
    return res; \
} \
static inline yumi_memory_alloc_error_enum lease_##x##_inline(x** outNode) \
{ \
    yumi_memory_alloc_error_enum res = YUMI_MEMORY_ALLOC_SUCCESS; \
    if (outNode == NULL) \
    { \
        res = YUMI_MEMORY_ALLOC_INVALID_OUT_POINTER; \
    } \
    if (res == YUMI_MEMORY_ALLOC_SUCCESS) \
    { \
        if (YUMI_LIKELY(x##_tls.top > 0)) \
        { \
            x##_tls.top--; \
            x##_linked_list_node item = x##_tls.stack[x##_tls.top]; \
            YUMI_STATIC_HEAP_DEBUG_LEASE_MARK(x, item); \
            *outNode = &item->bindings; \
        } \
        else \
        { \
            res = x##_refill_from_global(outNode); \
        } \
    } \
    return res; \
} \
static inline x* lease_##x##_ptr(void) \
{ \
    x##_tls_slot_t* p = &x##_tls; \
    x* out = NULL; \
    if (YUMI_LIKELY(p->top > 0)) \
    { \
        p->top--; \
        x##_linked_list_node item = p->stack[p->top]; \
        YUMI_STATIC_HEAP_DEBUG_LEASE_MARK(x, item); \
        out = &item->bindings; \
    } \
    else \
    { \
        (void)x##_refill_from_global(&out); \
    } \
    return out; \
} \
static inline void release_##x##_ptr(x* node) \
{ \
    x##_tls_slot_t* p = &x##_tls; \
    x##_linked_list_node item = (x##_linked_list_node)node; \
    YUMI_STATIC_HEAP_DEBUG_RELEASE_CHECK(x, item); \
    if (YUMI_LIKELY(p->top < YUMI_STATIC_HEAP_TLS_CACHE_SIZE)) \
    { \
        p->stack[p->top] = item; \
        p->top++; \
    } \
    else \
    { \
        (void)x##_spill_to_global(item); \
    } \
} \
yumi_memory_alloc_error_enum release_##x(x* node) \
{ \
    return release_##x##_inline(node); \
} \
yumi_memory_alloc_error_enum lease_##x(x** outNode) \
{ \
    return lease_##x##_inline(outNode); \
}

#define NUM_OBJ 1000

/* Create heap with 100 objects */
static_heap(TestObject, NUM_OBJ)

/* ── Basic functionality tests ──────────────────────────────── */

TEST(test_init_heap) {
    init_static_heap_TestObject();
    assert(TestObject_linked_list_node_head != NULL);
}

TEST(test_lease_single) {
    init_static_heap_TestObject();
    TestObject* obj = NULL;
    yumi_memory_alloc_error_enum err = lease_TestObject(&obj);
    assert(err == YUMI_MEMORY_ALLOC_SUCCESS);
    assert(obj != NULL);
}

TEST(test_release_single) {
    init_static_heap_TestObject();
    TestObject* obj = NULL;
    lease_TestObject(&obj);
    yumi_memory_alloc_error_enum err = release_TestObject(obj);
    assert(err == YUMI_MEMORY_ALLOC_SUCCESS);
}

TEST(test_lease_release_cycle) {
    init_static_heap_TestObject();
    TestObject* obj = NULL;

    lease_TestObject(&obj);
    obj->id = 42;
    obj->magic = 0xDEADBEEF;
    release_TestObject(obj);

    TestObject* obj2 = NULL;
    lease_TestObject(&obj2);
    assert(obj2 == obj); /* Should get same object back */
    assert(obj2->id == 42); /* Data should be preserved */
}

TEST(test_lease_all) {
    init_static_heap_TestObject();
    TestObject* objs[NUM_OBJ];

    for (int i = 0; i < NUM_OBJ; i++) {
        yumi_memory_alloc_error_enum err = lease_TestObject(&objs[i]);
        assert(err == YUMI_MEMORY_ALLOC_SUCCESS);
        assert(objs[i] != NULL);
        objs[i]->id = i;
    }

    /* Next lease should fail */
    TestObject* extra = NULL;
    yumi_memory_alloc_error_enum err = lease_TestObject(&extra);
    assert(err == YUMI_MEMORY_ALLOC_UNAVAILABLE_MEMORY);
    assert(extra == NULL);

    /* Release all */
    for (int i = 0; i < NUM_OBJ; i++) {
        release_TestObject(objs[i]);
    }
}

TEST(test_null_pointer_handling) {
    init_static_heap_TestObject();

    /* Lease with NULL output */
    yumi_memory_alloc_error_enum err = lease_TestObject(NULL);
    assert(err == YUMI_MEMORY_ALLOC_INVALID_OUT_POINTER);

    /* Release NULL */
    err = release_TestObject(NULL);
    assert(err == YUMI_MEMORY_ALLOC_INVALID_IN_POINTER);
}

/* ── Multithreaded tests ────────────────────────────────────── */

typedef struct {
    int thread_id;
    int iterations;
    _Atomic(int) *success_count;
    _Atomic(int) *failure_count;
} thread_args_t;

static int lease_release_worker(void* arg) {
    thread_args_t* args = (thread_args_t*)arg;

    for (int i = 0; i < args->iterations; i++) {
        TestObject* obj = NULL;
        yumi_memory_alloc_error_enum err = lease_TestObject(&obj);

        if (err == YUMI_MEMORY_ALLOC_SUCCESS) {
            atomic_fetch_add(args->success_count, 1);

            /* Write thread-specific data */
            obj->id = args->thread_id;
            obj->magic = 0xCAFEBABE + args->thread_id;
            memset(obj->data, (uint8_t)args->thread_id, sizeof(obj->data));

            /* Release immediately - no verification */
            err = release_TestObject(obj);
            assert(err == YUMI_MEMORY_ALLOC_SUCCESS);
        } else {
            atomic_fetch_add(args->failure_count, 1);
        }
    }

    return 0;
}

TEST(test_concurrent_lease_release) {
    init_static_heap_TestObject();

    const int NUM_THREADS = 4;
    const int ITERATIONS = 1000;

    thrd_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];
    _Atomic(int) success_count = 0;
    _Atomic(int) failure_count = 0;

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = ITERATIONS;
        args[i].success_count = &success_count;
        args[i].failure_count = &failure_count;

        int res = thrd_create(&threads[i], lease_release_worker, &args[i]);
        assert(res == thrd_success);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    int total = atomic_load(&success_count) + atomic_load(&failure_count);
    assert(total == NUM_THREADS * ITERATIONS);
    printf("\n      (successes: %d, failures: %d) ",
           atomic_load(&success_count),
           atomic_load(&failure_count));
}

static int lease_hold_worker(void* arg) {
    thread_args_t* args = (thread_args_t*)arg;
    TestObject* objs[25]; /* Each thread tries to hold 25 objects */
    int leased = 0;

    /* Lease as many as possible */
    for (int i = 0; i < 25; i++) {
        yumi_memory_alloc_error_enum err = lease_TestObject(&objs[i]);
        if (err == YUMI_MEMORY_ALLOC_SUCCESS) {
            objs[i]->id = args->thread_id * 1000 + i;
            objs[i]->magic = 0xDEADBEEF;
            leased++;
        } else {
            break;  // ← Don't increment failure_count here
        }
    }

    /* Store how many this thread got */
    atomic_fetch_add(args->success_count, leased);

    /* Verify all held objects */
    for (int i = 0; i < leased; i++) {
        assert(objs[i]->id == args->thread_id * 1000 + i);
        assert(objs[i]->magic == 0xDEADBEEF);
    }

    /* Release all */
    for (int i = 0; i < leased; i++) {
        release_TestObject(objs[i]);
    }

    return 0;
}

TEST(test_concurrent_exhaustion) {
    init_static_heap_TestObject();

    const int NUM_THREADS = 8;

    thrd_t threads[NUM_THREADS];
    thread_args_t args[NUM_THREADS];
    _Atomic(int) success_count = 0;
    _Atomic(int) failure_count = 0;

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].success_count = &success_count;
        args[i].failure_count = &failure_count;

        int res = thrd_create(&threads[i], lease_hold_worker, &args[i]);
        assert(res == thrd_success);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    /* Since threads release objects, total can exceed 100.
       Just verify we got reasonable allocation behavior */
    int total = atomic_load(&success_count);
    assert(total >= 100); /* At minimum, all 100 should be allocated once */
    assert(total <= 200); /* Each object leased at most twice (8 threads * 25 = 200 max) */
    printf("\n      (total allocations across threads: %d) ", total);
}


typedef struct {
    int producer_id;
    int items_to_produce;
    _Atomic(int) *items_produced;
} producer_args_t;

typedef struct {
    int consumer_id;
    int items_to_consume;
    _Atomic(int) *items_consumed;
} consumer_args_t;

static int producer_worker(void* arg) {
    producer_args_t* args = (producer_args_t*)arg;

    for (int i = 0; i < args->items_to_produce; i++) {
        TestObject* obj = NULL;

        /* Keep trying until we get one */
        while (lease_TestObject(&obj) != YUMI_MEMORY_ALLOC_SUCCESS) {
            thrd_yield();
        }

        obj->id = args->producer_id * 10000 + i;
        obj->magic = 0xABCDEF00 + args->producer_id;

        atomic_fetch_add(args->items_produced, 1);

        /* Immediately release for consumers */
        release_TestObject(obj);
    }

    return 0;
}

static int consumer_worker(void* arg) {
    consumer_args_t* args = (consumer_args_t*)arg;

    for (int i = 0; i < args->items_to_consume; i++) {
        TestObject* obj = NULL;

        /* Keep trying until we get one */
        while (lease_TestObject(&obj) != YUMI_MEMORY_ALLOC_SUCCESS) {
            thrd_yield();
        }

        atomic_fetch_add(args->items_consumed, 1);

        release_TestObject(obj);
    }

    return 0;
}

TEST(test_producer_consumer) {
    init_static_heap_TestObject();

    const int NUM_PRODUCERS = 3;
    const int NUM_CONSUMERS = 3;
    const int ITEMS_PER_PRODUCER = 100;

    thrd_t producers[NUM_PRODUCERS];
    thrd_t consumers[NUM_CONSUMERS];
    producer_args_t producer_args[NUM_PRODUCERS];
    consumer_args_t consumer_args[NUM_CONSUMERS];
    _Atomic(int) items_produced = 0;
    _Atomic(int) items_consumed = 0;

    /* Start producers */
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producer_args[i].producer_id = i;
        producer_args[i].items_to_produce = ITEMS_PER_PRODUCER;
        producer_args[i].items_produced = &items_produced;

        int res = thrd_create(&producers[i], producer_worker, &producer_args[i]);
        assert(res == thrd_success);
    }

    /* Start consumers */
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        consumer_args[i].consumer_id = i;
        consumer_args[i].items_to_consume = ITEMS_PER_PRODUCER;
        consumer_args[i].items_consumed = &items_consumed;

        int res = thrd_create(&consumers[i], consumer_worker, &consumer_args[i]);
        assert(res == thrd_success);
    }

    /* Wait for completion */
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        thrd_join(producers[i], NULL);
    }
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        thrd_join(consumers[i], NULL);
    }

    assert(atomic_load(&items_produced) == NUM_PRODUCERS * ITEMS_PER_PRODUCER);
    assert(atomic_load(&items_consumed) == NUM_CONSUMERS * ITEMS_PER_PRODUCER);
}

/* ── Memory consistency tests ───────────────────────────────── */

TEST(test_data_visibility) {
    init_static_heap_TestObject();

    const uint32_t MAGIC = 0x12345678;
    const uint32_t ID = 999;

    TestObject* obj = NULL;
    lease_TestObject(&obj);

    /* Write data */
    obj->id = ID;
    obj->magic = MAGIC;
    for (int i = 0; i < 56; i++) {
        obj->data[i] = (uint8_t)i;
    }

    /* Release */
    release_TestObject(obj);

    /* Lease again */
    TestObject* obj2 = NULL;
    lease_TestObject(&obj2);

    /* Verify all data is visible */
    assert(obj2->id == ID);
    assert(obj2->magic == MAGIC);
    for (int i = 0; i < 56; i++) {
        assert(obj2->data[i] == (uint8_t)i);
    }

    release_TestObject(obj2);
}

/* ── Long-running stress test ────────────────────────────────── */

typedef struct {
    int thread_id;
    _Atomic(bool) *keep_running;
    _Atomic(uint64_t) *total_leases;
    _Atomic(uint64_t) *total_failures;
    _Atomic(uint64_t) *total_releases;
} stress_test_args_t;

static int stress_test_worker(void* arg) {
    stress_test_args_t* args = (stress_test_args_t*)arg;
    uint64_t local_leases = 0;
    uint64_t local_failures = 0;
    uint64_t local_releases = 0;

    while (atomic_load(args->keep_running)) {
        TestObject* obj = NULL;
        yumi_memory_alloc_error_enum err = lease_TestObject_inline(&obj);

        if (err == YUMI_MEMORY_ALLOC_SUCCESS) {
            local_leases++;

            /* Simulate some work */
            obj->id = args->thread_id;
            obj->magic = 0xDEADC0DE;

            /* Small delay to increase contention */
            for (volatile int i = 0; i < 100; i++);

            err = release_TestObject_inline(obj);
            if (err == YUMI_MEMORY_ALLOC_SUCCESS) {
                local_releases++;
            }
        } else {
            local_failures++;
            thrd_yield(); /* Back off on failure */
        }
    }

    /* Update global counters */
    atomic_fetch_add(args->total_leases, local_leases);
    atomic_fetch_add(args->total_failures, local_failures);
    atomic_fetch_add(args->total_releases, local_releases);

    return 0;
}

TEST(test_30_second_stress) {
    init_static_heap_TestObject();

    const int NUM_THREADS = 8;
    const int DURATION_SECONDS = 30;

    thrd_t threads[NUM_THREADS];
    stress_test_args_t args[NUM_THREADS];
    _Atomic(bool) keep_running = true;
    _Atomic(uint64_t) total_leases = 0;
    _Atomic(uint64_t) total_failures = 0;
    _Atomic(uint64_t) total_releases = 0;

    printf("\n      Running 30-second stress test with %d threads...\n", NUM_THREADS);

    /* Start threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].keep_running = &keep_running;
        args[i].total_leases = &total_leases;
        args[i].total_failures = &total_failures;
        args[i].total_releases = &total_releases;

        int res = thrd_create(&threads[i], stress_test_worker, &args[i]);
        assert(res == thrd_success);
    }

    /* Run for specified duration */
    thrd_sleep(&(struct timespec){.tv_sec = DURATION_SECONDS}, NULL);

    /* Stop threads */
    atomic_store(&keep_running, false);

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    /* Final statistics */
    uint64_t final_leases = atomic_load(&total_leases);
    uint64_t final_failures = atomic_load(&total_failures);
    uint64_t final_releases = atomic_load(&total_releases);
    uint64_t total_ops = final_leases + final_failures;
    double success_rate = total_ops > 0 ? (100.0 * final_leases / total_ops) : 0.0;

    printf("\n      Final Results:\n");
    printf("      - Total operations: %lu\n", total_ops);
    printf("      - Successful leases: %lu\n", final_leases);
    printf("      - Failed leases: %lu\n", final_failures);
    printf("      - Releases: %lu\n", final_releases);
    printf("      - Success rate: %.2f%%\n", success_rate);
    printf("      - Ops/second: %.0f\n", (double)total_ops / DURATION_SECONDS);
    printf("      - Leases/second: %.0f\n", (double)final_leases / DURATION_SECONDS);

    /* Verify no leaks - all leased objects should be released */
    assert(final_leases == final_releases);

    /* Success rate should be reasonable even under stress */
    assert(success_rate > 95.0); /* At least 95% success rate */
}


/* ── malloc/free baseline stress test ──────────────────────── */

typedef struct {
    int thread_id;
    _Atomic(bool) *keep_running;
    _Atomic(uint64_t) *total_allocs;
    _Atomic(uint64_t) *total_failures;
    _Atomic(uint64_t) *total_frees;
} malloc_stress_args_t;

static int malloc_stress_worker(void* arg) {
    malloc_stress_args_t* args = (malloc_stress_args_t*)arg;
    uint64_t local_allocs = 0;
    uint64_t local_failures = 0;
    uint64_t local_frees = 0;

    while (atomic_load(args->keep_running)) {
        TestObject* obj = (TestObject*)malloc(sizeof(TestObject));

        if (obj != NULL) {
            local_allocs++;

            /* Identical work to static_heap stress test */
            obj->id = args->thread_id;
            obj->magic = 0xDEADC0DE;

            /* Same delay to keep conditions comparable */
            for (volatile int i = 0; i < 100; i++);

            free(obj);
            local_frees++;
        } else {
            local_failures++;
            thrd_yield();
        }
    }

    atomic_fetch_add(args->total_allocs, local_allocs);
    atomic_fetch_add(args->total_failures, local_failures);
    atomic_fetch_add(args->total_frees, local_frees);

    return 0;
}

TEST(test_30_second_malloc_baseline) {
    const int NUM_THREADS = 8;
    const int DURATION_SECONDS = 30;

    thrd_t threads[NUM_THREADS];
    malloc_stress_args_t args[NUM_THREADS];
    _Atomic(bool) keep_running = true;
    _Atomic(uint64_t) total_allocs = 0;
    _Atomic(uint64_t) total_failures = 0;
    _Atomic(uint64_t) total_frees = 0;

    printf("\n      Running 30-second malloc/free baseline with %d threads...\n", NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].keep_running = &keep_running;
        args[i].total_allocs = &total_allocs;
        args[i].total_failures = &total_failures;
        args[i].total_frees = &total_frees;

        int res = thrd_create(&threads[i], malloc_stress_worker, &args[i]);
        assert(res == thrd_success);
    }

    thrd_sleep(&(struct timespec){.tv_sec = DURATION_SECONDS}, NULL);
    atomic_store(&keep_running, false);

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    uint64_t final_allocs   = atomic_load(&total_allocs);
    uint64_t final_failures = atomic_load(&total_failures);
    uint64_t final_frees    = atomic_load(&total_frees);
    uint64_t total_ops      = final_allocs + final_failures;
    double success_rate     = total_ops > 0 ? (100.0 * final_allocs / total_ops) : 0.0;

    printf("\n      Final Results:\n");
    printf("      - Total operations: %lu\n", total_ops);
    printf("      - Successful allocs: %lu\n", final_allocs);
    printf("      - Failed allocs:     %lu\n", final_failures);
    printf("      - Frees:             %lu\n", final_frees);
    printf("      - Success rate:      %.2f%%\n", success_rate);
    printf("      - Ops/second:        %.0f\n", (double)total_ops / DURATION_SECONDS);
    printf("      - Allocs/second:     %.0f\n", (double)final_allocs / DURATION_SECONDS);

    assert(final_allocs == final_frees);
}

/* ── No-delay throughput tests ─────────────────────────────────
 * These remove the `for (volatile int i = 0; i < 100; i++);` filler
 * loop that dominated >60%% of cycles in the original stress tests,
 * exposing the true allocator hot-path cost.
 */

static int stress_test_worker_nodelay(void* arg) {
    stress_test_args_t* args = (stress_test_args_t*)arg;
    uint64_t local_leases = 0;
    uint64_t local_failures = 0;
    uint64_t local_releases = 0;

    while (atomic_load_explicit(args->keep_running, memory_order_relaxed)) {
        TestObject* obj = NULL;
        yumi_memory_alloc_error_enum err = lease_TestObject_inline(&obj);

        if (err == YUMI_MEMORY_ALLOC_SUCCESS) {
            local_leases++;
            obj->id = args->thread_id;
            obj->magic = 0xDEADC0DE;
            YUMI_DO_NOT_OPTIMIZE(obj);
            err = release_TestObject_inline(obj);
            if (err == YUMI_MEMORY_ALLOC_SUCCESS) {
                local_releases++;
            }
        } else {
            local_failures++;
            thrd_yield();
        }
    }

    atomic_fetch_add(args->total_leases, local_leases);
    atomic_fetch_add(args->total_failures, local_failures);
    atomic_fetch_add(args->total_releases, local_releases);
    return 0;
}

TEST(test_10_second_stress_nodelay) {
    init_static_heap_TestObject();

    const int NUM_THREADS = 8;
    const int DURATION_SECONDS = 10;

    thrd_t threads[NUM_THREADS];
    stress_test_args_t args[NUM_THREADS];
    _Atomic(bool) keep_running = true;
    _Atomic(uint64_t) total_leases = 0;
    _Atomic(uint64_t) total_failures = 0;
    _Atomic(uint64_t) total_releases = 0;

    printf("\n      Running %d-second NO-DELAY stress test with %d threads...\n", DURATION_SECONDS, NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].keep_running = &keep_running;
        args[i].total_leases = &total_leases;
        args[i].total_failures = &total_failures;
        args[i].total_releases = &total_releases;
        int res = thrd_create(&threads[i], stress_test_worker_nodelay, &args[i]);
        assert(res == thrd_success);
    }

    thrd_sleep(&(struct timespec){.tv_sec = DURATION_SECONDS}, NULL);
    atomic_store(&keep_running, false);

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    uint64_t final_leases = atomic_load(&total_leases);
    uint64_t final_failures = atomic_load(&total_failures);
    uint64_t final_releases = atomic_load(&total_releases);
    uint64_t total_ops = final_leases + final_failures;

    printf("\n      Final Results:\n");
    printf("      - Total operations: %lu\n", total_ops);
    printf("      - Successful leases: %lu\n", final_leases);
    printf("      - Failed leases: %lu\n", final_failures);
    printf("      - Releases: %lu\n", final_releases);
    printf("      - Ops/second: %.0f\n", (double)total_ops / DURATION_SECONDS);

    assert(final_leases == final_releases);
}

static int malloc_stress_worker_nodelay(void* arg) {
    malloc_stress_args_t* args = (malloc_stress_args_t*)arg;
    uint64_t local_allocs = 0;
    uint64_t local_failures = 0;
    uint64_t local_frees = 0;

    while (atomic_load_explicit(args->keep_running, memory_order_relaxed)) {
        TestObject* obj = (TestObject*)malloc(sizeof(TestObject));
        if (obj != NULL) {
            local_allocs++;
            obj->id = args->thread_id;
            obj->magic = 0xDEADC0DE;
            YUMI_DO_NOT_OPTIMIZE(obj);
            free(obj);
            local_frees++;
        } else {
            local_failures++;
            thrd_yield();
        }
    }

    atomic_fetch_add(args->total_allocs, local_allocs);
    atomic_fetch_add(args->total_failures, local_failures);
    atomic_fetch_add(args->total_frees, local_frees);
    return 0;
}

TEST(test_10_second_malloc_baseline_nodelay) {
    const int NUM_THREADS = 8;
    const int DURATION_SECONDS = 10;

    thrd_t threads[NUM_THREADS];
    malloc_stress_args_t args[NUM_THREADS];
    _Atomic(bool) keep_running = true;
    _Atomic(uint64_t) total_allocs = 0;
    _Atomic(uint64_t) total_failures = 0;
    _Atomic(uint64_t) total_frees = 0;

    printf("\n      Running %d-second NO-DELAY malloc/free baseline with %d threads...\n", DURATION_SECONDS, NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].keep_running = &keep_running;
        args[i].total_allocs = &total_allocs;
        args[i].total_failures = &total_failures;
        args[i].total_frees = &total_frees;
        int res = thrd_create(&threads[i], malloc_stress_worker_nodelay, &args[i]);
        assert(res == thrd_success);
    }

    thrd_sleep(&(struct timespec){.tv_sec = DURATION_SECONDS}, NULL);
    atomic_store(&keep_running, false);

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    uint64_t final_allocs = atomic_load(&total_allocs);
    uint64_t final_frees = atomic_load(&total_frees);
    uint64_t final_failures = atomic_load(&total_failures);
    uint64_t total_ops = final_allocs + final_failures;

    printf("\n      Final Results:\n");
    printf("      - Total operations: %lu\n", total_ops);
    printf("      - Successful allocs: %lu\n", final_allocs);
    printf("      - Failed allocs:     %lu\n", final_failures);
    printf("      - Frees:             %lu\n", final_frees);
    printf("      - Ops/second:        %.0f\n", (double)total_ops / DURATION_SECONDS);

    assert(final_allocs == final_frees);
}

/* ── No-delay throughput test using the fast pointer-returning API ──
 * Exercises lease_TestObject_ptr / release_TestObject_ptr which skip
 * the enum return, out-parameter convention, NULL checks, and the
 * redundant ensure_tls() on release. Direct head-to-head with the
 * existing _inline path and with glibc malloc/free.
 */
static int stress_test_worker_nodelay_ptr(void* arg) {
    stress_test_args_t* args = (stress_test_args_t*)arg;
    uint64_t local_leases = 0;
    uint64_t local_failures = 0;
    uint64_t local_releases = 0;

    while (atomic_load_explicit(args->keep_running, memory_order_relaxed)) {
        TestObject* obj = lease_TestObject_ptr();

        if (obj != NULL) {
            local_leases++;
            obj->id = args->thread_id;
            obj->magic = 0xDEADC0DE;
            YUMI_DO_NOT_OPTIMIZE(obj);
            release_TestObject_ptr(obj);
            local_releases++;
        } else {
            local_failures++;
            thrd_yield();
        }
    }

    atomic_fetch_add(args->total_leases, local_leases);
    atomic_fetch_add(args->total_failures, local_failures);
    atomic_fetch_add(args->total_releases, local_releases);
    return 0;
}

TEST(test_10_second_stress_nodelay_ptr) {
    init_static_heap_TestObject();

    const int NUM_THREADS = 8;
    const int DURATION_SECONDS = 10;

    thrd_t threads[NUM_THREADS];
    stress_test_args_t args[NUM_THREADS];
    _Atomic(bool) keep_running = true;
    _Atomic(uint64_t) total_leases = 0;
    _Atomic(uint64_t) total_failures = 0;
    _Atomic(uint64_t) total_releases = 0;

    printf("\n      Running %d-second NO-DELAY PTR-API stress test with %d threads...\n", DURATION_SECONDS, NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].keep_running = &keep_running;
        args[i].total_leases = &total_leases;
        args[i].total_failures = &total_failures;
        args[i].total_releases = &total_releases;
        int res = thrd_create(&threads[i], stress_test_worker_nodelay_ptr, &args[i]);
        assert(res == thrd_success);
    }

    thrd_sleep(&(struct timespec){.tv_sec = DURATION_SECONDS}, NULL);
    atomic_store(&keep_running, false);

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    uint64_t final_leases = atomic_load(&total_leases);
    uint64_t final_failures = atomic_load(&total_failures);
    uint64_t final_releases = atomic_load(&total_releases);
    uint64_t total_ops = final_leases + final_failures;

    printf("\n      Final Results:\n");
    printf("      - Total operations: %lu\n", total_ops);
    printf("      - Successful leases: %lu\n", final_leases);
    printf("      - Failed leases: %lu\n", final_failures);
    printf("      - Releases: %lu\n", final_releases);
    printf("      - Ops/second: %.0f\n", (double)total_ops / DURATION_SECONDS);

    assert(final_leases == final_releases);
}

/* ── Mixed-size heaps: 3 different struct sizes side-by-side ──────────
 * Demonstrates the static_heap allocator's behaviour with multiple
 * concurrent object pools of different sizes, vs glibc malloc/free
 * doing the same mix.
 *
 * SmallObj  =   16 B  (tiny, fits in 1/4 cache line)
 * MediumObj =  128 B  (2 cache lines)
 * LargeObj  =  512 B  (8 cache lines)
 *
 * Both workers do the same access pattern: round-robin S, M, L per
 * iteration, touching the first and last bytes of each payload so the
 * compiler can't elide stores (the YUMI_DO_NOT_OPTIMIZE barrier between
 * write and release/free also enforces this).
 */

typedef struct {
    uint32_t id;
    uint32_t magic;
    uint8_t  data[8];
} SmallObj;   /* 16 bytes */
_Static_assert(sizeof(SmallObj) == 16, "SmallObj must be 16 bytes");

typedef struct {
    uint32_t id;
    uint32_t magic;
    uint8_t  data[120];
} MediumObj;  /* 128 bytes */
_Static_assert(sizeof(MediumObj) == 128, "MediumObj must be 128 bytes");

typedef struct {
    uint32_t id;
    uint32_t magic;
    uint8_t  data[504];
} LargeObj;   /* 512 bytes */
_Static_assert(sizeof(LargeObj) == 512, "LargeObj must be 512 bytes");

#define NUM_MIXED 1000
static_heap(SmallObj,  NUM_MIXED)
static_heap(MediumObj, NUM_MIXED)
static_heap(LargeObj,  NUM_MIXED)

static int mixed_static_heap_worker_nodelay(void* arg) {
    stress_test_args_t* args = (stress_test_args_t*)arg;
    uint64_t local_leases = 0;
    uint64_t local_failures = 0;
    uint64_t local_releases = 0;

    while (atomic_load_explicit(args->keep_running, memory_order_relaxed)) {
        SmallObj*  s = lease_SmallObj_ptr();
        MediumObj* m = lease_MediumObj_ptr();
        LargeObj*  l = lease_LargeObj_ptr();

        if (s != NULL && m != NULL && l != NULL) {
            local_leases += 3;
            /* Touch first + last byte of each payload so the compiler */
            /* must materialise the writes. */
            s->id = args->thread_id;
            s->magic = 0xDEADC0DE;
            s->data[sizeof(s->data) - 1] = (uint8_t)args->thread_id;
            m->id = args->thread_id;
            m->magic = 0xDEADC0DE;
            m->data[sizeof(m->data) - 1] = (uint8_t)args->thread_id;
            l->id = args->thread_id;
            l->magic = 0xDEADC0DE;
            l->data[sizeof(l->data) - 1] = (uint8_t)args->thread_id;
            YUMI_DO_NOT_OPTIMIZE(s);
            YUMI_DO_NOT_OPTIMIZE(m);
            YUMI_DO_NOT_OPTIMIZE(l);
            release_LargeObj_ptr(l);
            release_MediumObj_ptr(m);
            release_SmallObj_ptr(s);
            local_releases += 3;
        } else {
            /* Partial success: return whatever we did get. */
            if (l != NULL) { release_LargeObj_ptr(l); }
            if (m != NULL) { release_MediumObj_ptr(m); }
            if (s != NULL) { release_SmallObj_ptr(s); }
            local_failures++;
            thrd_yield();
        }
    }

    atomic_fetch_add(args->total_leases,   local_leases);
    atomic_fetch_add(args->total_failures, local_failures);
    atomic_fetch_add(args->total_releases, local_releases);
    return 0;
}

TEST(test_10_second_mixed_sizes_static_heap) {
    init_static_heap_SmallObj();
    init_static_heap_MediumObj();
    init_static_heap_LargeObj();

    const int NUM_THREADS = 8;
    const int DURATION_SECONDS = 10;

    thrd_t threads[NUM_THREADS];
    stress_test_args_t args[NUM_THREADS];
    _Atomic(bool) keep_running = true;
    _Atomic(uint64_t) total_leases = 0;
    _Atomic(uint64_t) total_failures = 0;
    _Atomic(uint64_t) total_releases = 0;

    printf("\n      Running %d-second MIXED-SIZES (S=16B, M=128B, L=512B) static_heap test with %d threads...\n",
           DURATION_SECONDS, NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].keep_running = &keep_running;
        args[i].total_leases = &total_leases;
        args[i].total_failures = &total_failures;
        args[i].total_releases = &total_releases;
        int res = thrd_create(&threads[i], mixed_static_heap_worker_nodelay, &args[i]);
        assert(res == thrd_success);
    }

    thrd_sleep(&(struct timespec){.tv_sec = DURATION_SECONDS}, NULL);
    atomic_store(&keep_running, false);

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    uint64_t final_leases = atomic_load(&total_leases);
    uint64_t final_failures = atomic_load(&total_failures);
    uint64_t final_releases = atomic_load(&total_releases);
    uint64_t total_ops = final_leases;

    printf("\n      Final Results:\n");
    printf("      - Total lease+release ops: %lu (across 3 pools)\n", total_ops);
    printf("      - Successful leases:       %lu\n", final_leases);
    printf("      - Partial-failure rounds:  %lu\n", final_failures);
    printf("      - Releases:                %lu\n", final_releases);
    printf("      - Ops/second:              %.0f\n", (double)total_ops / DURATION_SECONDS);
    printf("      - Ops/second per pool:     %.0f\n", (double)total_ops / DURATION_SECONDS / 3.0);

    assert(final_leases == final_releases);
}

static int mixed_malloc_worker_nodelay(void* arg) {
    malloc_stress_args_t* args = (malloc_stress_args_t*)arg;
    uint64_t local_allocs = 0;
    uint64_t local_failures = 0;
    uint64_t local_frees = 0;

    while (atomic_load_explicit(args->keep_running, memory_order_relaxed)) {
        SmallObj*  s = (SmallObj*) malloc(sizeof(SmallObj));
        MediumObj* m = (MediumObj*)malloc(sizeof(MediumObj));
        LargeObj*  l = (LargeObj*) malloc(sizeof(LargeObj));

        if (s != NULL && m != NULL && l != NULL) {
            local_allocs += 3;
            s->id = args->thread_id;
            s->magic = 0xDEADC0DE;
            s->data[sizeof(s->data) - 1] = (uint8_t)args->thread_id;
            m->id = args->thread_id;
            m->magic = 0xDEADC0DE;
            m->data[sizeof(m->data) - 1] = (uint8_t)args->thread_id;
            l->id = args->thread_id;
            l->magic = 0xDEADC0DE;
            l->data[sizeof(l->data) - 1] = (uint8_t)args->thread_id;
            YUMI_DO_NOT_OPTIMIZE(s);
            YUMI_DO_NOT_OPTIMIZE(m);
            YUMI_DO_NOT_OPTIMIZE(l);
            free(l);
            free(m);
            free(s);
            local_frees += 3;
        } else {
            if (l != NULL) { free(l); }
            if (m != NULL) { free(m); }
            if (s != NULL) { free(s); }
            local_failures++;
            thrd_yield();
        }
    }

    atomic_fetch_add(args->total_allocs,   local_allocs);
    atomic_fetch_add(args->total_failures, local_failures);
    atomic_fetch_add(args->total_frees,    local_frees);
    return 0;
}

TEST(test_10_second_mixed_sizes_malloc_baseline) {
    const int NUM_THREADS = 8;
    const int DURATION_SECONDS = 10;

    thrd_t threads[NUM_THREADS];
    malloc_stress_args_t args[NUM_THREADS];
    _Atomic(bool) keep_running = true;
    _Atomic(uint64_t) total_allocs = 0;
    _Atomic(uint64_t) total_failures = 0;
    _Atomic(uint64_t) total_frees = 0;

    printf("\n      Running %d-second MIXED-SIZES (S=16B, M=128B, L=512B) malloc/free baseline with %d threads...\n",
           DURATION_SECONDS, NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].keep_running = &keep_running;
        args[i].total_allocs = &total_allocs;
        args[i].total_failures = &total_failures;
        args[i].total_frees = &total_frees;
        int res = thrd_create(&threads[i], mixed_malloc_worker_nodelay, &args[i]);
        assert(res == thrd_success);
    }

    thrd_sleep(&(struct timespec){.tv_sec = DURATION_SECONDS}, NULL);
    atomic_store(&keep_running, false);

    for (int i = 0; i < NUM_THREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    uint64_t final_allocs   = atomic_load(&total_allocs);
    uint64_t final_frees    = atomic_load(&total_frees);
    uint64_t final_failures = atomic_load(&total_failures);
    uint64_t total_ops = final_allocs;

    printf("\n      Final Results:\n");
    printf("      - Total alloc+free ops:   %lu (across 3 sizes)\n", total_ops);
    printf("      - Successful allocs:      %lu\n", final_allocs);
    printf("      - Partial-failure rounds: %lu\n", final_failures);
    printf("      - Frees:                  %lu\n", final_frees);
    printf("      - Ops/second:             %.0f\n", (double)total_ops / DURATION_SECONDS);
    printf("      - Ops/second per size:    %.0f\n", (double)total_ops / DURATION_SECONDS / 3.0);

    assert(final_allocs == final_frees);
}

/* ── 5-minute stability test: 5 pools, 2 threads per pool ────────────
 * Goal: maximize contention and pressure while continuously checking
 * for allocator correctness issues:
 *   - double lease (same slot leased concurrently)
 *   - double free / invalid release transitions
 *   - pointer corruption / out-of-range pointers
 *   - payload corruption while object is leased
 */

typedef struct {
    _Alignas(32) uint8_t bytes[32];
} StabilityObjA32;
_Static_assert(sizeof(StabilityObjA32) == 32, "StabilityObjA32 must be 32 bytes");
_Static_assert(_Alignof(StabilityObjA32) == 32, "StabilityObjA32 must be 32-byte aligned");

typedef struct {
    uint8_t bytes[33];
} StabilityObjB33;
_Static_assert(sizeof(StabilityObjB33) == 33, "StabilityObjB33 must be 33 bytes");

typedef struct {
    _Alignas(32) uint8_t bytes[95];
} StabilityObjC96;
_Static_assert(sizeof(StabilityObjC96) == 96, "StabilityObjC96 must round to 96 bytes");
_Static_assert(_Alignof(StabilityObjC96) == 32, "StabilityObjC96 must be 32-byte aligned");

typedef struct {
    uint8_t bytes[65];
} StabilityObjD65;
_Static_assert(sizeof(StabilityObjD65) == 65, "StabilityObjD65 must be 65 bytes");

typedef struct {
    _Alignas(32) uint8_t bytes[159];
} StabilityObjE160;
_Static_assert(sizeof(StabilityObjE160) == 160, "StabilityObjE160 must round to 160 bytes");
_Static_assert(_Alignof(StabilityObjE160) == 32, "StabilityObjE160 must be 32-byte aligned");

#define STABILITY_POOL_A_COUNT 47
#define STABILITY_POOL_B_COUNT 61
#define STABILITY_POOL_C_COUNT 29
#define STABILITY_POOL_D_COUNT 73
#define STABILITY_POOL_E_COUNT 37

static_heap(StabilityObjA32, STABILITY_POOL_A_COUNT)
static_heap(StabilityObjB33, STABILITY_POOL_B_COUNT)
static_heap(StabilityObjC96, STABILITY_POOL_C_COUNT)
static_heap(StabilityObjD65, STABILITY_POOL_D_COUNT)
static_heap(StabilityObjE160, STABILITY_POOL_E_COUNT)

static void* lease_StabilityObjA32_void(void) { return (void*)lease_StabilityObjA32_ptr(); }
static void* lease_StabilityObjB33_void(void) { return (void*)lease_StabilityObjB33_ptr(); }
static void* lease_StabilityObjC96_void(void) { return (void*)lease_StabilityObjC96_ptr(); }
static void* lease_StabilityObjD65_void(void) { return (void*)lease_StabilityObjD65_ptr(); }
static void* lease_StabilityObjE160_void(void) { return (void*)lease_StabilityObjE160_ptr(); }

static void release_StabilityObjA32_void(void* p) { release_StabilityObjA32_ptr((StabilityObjA32*)p); }
static void release_StabilityObjB33_void(void* p) { release_StabilityObjB33_ptr((StabilityObjB33*)p); }
static void release_StabilityObjC96_void(void* p) { release_StabilityObjC96_ptr((StabilityObjC96*)p); }
static void release_StabilityObjD65_void(void* p) { release_StabilityObjD65_ptr((StabilityObjD65*)p); }
static void release_StabilityObjE160_void(void* p) { release_StabilityObjE160_ptr((StabilityObjE160*)p); }

typedef struct {
    const char* pool_name;
    int pool_id;
    int worker_id;
    size_t object_size;
    size_t node_size;
    size_t pool_capacity;
    const uint8_t* buffer_base;
    _Atomic(uint8_t)* lease_state;
    void* (*lease_fn)(void);
    void (*release_fn)(void*);
    _Atomic(bool)* keep_running;
    _Atomic(uint64_t)* lease_ok;
    _Atomic(uint64_t)* release_ok;
    _Atomic(uint64_t)* lease_fail;
    _Atomic(uint64_t)* double_lease;
    _Atomic(uint64_t)* double_free;
    _Atomic(uint64_t)* corruption;
    _Atomic(uint64_t)* invalid_pointer;
} stability_worker_args_t;

static void stability_write_pattern(uint8_t* mem, size_t size, uint32_t tag)
{
    const uint8_t fill = (uint8_t)((tag ^ (tag >> 8U)) & 0xFFU);
    uint32_t tail = ~tag;
    memset(mem, (int)fill, size);
    if (size >= (sizeof(tag) + sizeof(tail)))
    {
        memcpy(mem, &tag, sizeof(tag));
        memcpy(mem + size - sizeof(tail), &tail, sizeof(tail));
    }
}

static bool stability_verify_pattern(const uint8_t* mem, size_t size, uint32_t tag)
{
    bool ok = true;
    const uint8_t fill = (uint8_t)((tag ^ (tag >> 8U)) & 0xFFU);
    uint32_t expected_tail = ~tag;
    uint32_t observed_head = 0U;
    uint32_t observed_tail = 0U;

    if (size < (sizeof(expected_tail) + sizeof(observed_head)))
    {
        return false;
    }

    memcpy(&observed_head, mem, sizeof(observed_head));
    memcpy(&observed_tail, mem + size - sizeof(observed_tail), sizeof(observed_tail));
    if ((observed_head != tag) || (observed_tail != expected_tail))
    {
        ok = false;
    }
    if (ok == true)
    {
        for (size_t i = sizeof(observed_head); i < size - sizeof(observed_tail); ++i)
        {
            if (mem[i] != fill)
            {
                ok = false;
                break;
            }
        }
    }
    return ok;
}

static bool stability_index_from_pointer(
    const uint8_t* ptr,
    const uint8_t* buffer_base,
    size_t node_size,
    size_t pool_capacity,
    size_t* out_index)
{
    bool ok = false;
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t b = (uintptr_t)buffer_base;
    if ((p >= b) && (node_size > 0U))
    {
        size_t diff = (size_t)(p - b);
        if ((diff % node_size) == 0U)
        {
            size_t index = diff / node_size;
            if (index < pool_capacity)
            {
                *out_index = index;
                ok = true;
            }
        }
    }
    return ok;
}

static int stability_pool_worker(void* arg)
{
    enum { STABILITY_BURST = 8 };
    stability_worker_args_t* args = (stability_worker_args_t*)arg;
    void* held[STABILITY_BURST] = {0};
    size_t held_index[STABILITY_BURST] = {0};
    uint32_t held_tag[STABILITY_BURST] = {0};

    uint64_t local_lease_ok = 0;
    uint64_t local_release_ok = 0;
    uint64_t local_lease_fail = 0;
    uint64_t local_double_lease = 0;
    uint64_t local_double_free = 0;
    uint64_t local_corruption = 0;
    uint64_t local_invalid_pointer = 0;
    uint32_t sequence = 1U;

    while (atomic_load_explicit(args->keep_running, memory_order_relaxed))
    {
        int acquired = 0;
        for (int slot = 0; slot < STABILITY_BURST; ++slot)
        {
            void* obj = args->lease_fn();
            if (obj == NULL)
            {
                local_lease_fail++;
                break;
            }

            size_t idx = 0U;
            if (stability_index_from_pointer((const uint8_t*)obj,
                                             args->buffer_base,
                                             args->node_size,
                                             args->pool_capacity,
                                             &idx) == false)
            {
                local_invalid_pointer++;
                atomic_store(args->keep_running, false);
                break;
            }

            uint8_t expected_free = 0U;
            if (atomic_compare_exchange_strong(&args->lease_state[idx], &expected_free, 1U) == false)
            {
                local_double_lease++;
                atomic_store(args->keep_running, false);
                break;
            }

            uint32_t tag = (uint32_t)((args->pool_id << 24) ^ (args->worker_id << 16) ^ (int)sequence ^ slot);
            stability_write_pattern((uint8_t*)obj, args->object_size, tag);
            YUMI_DO_NOT_OPTIMIZE(obj);

            held[acquired] = obj;
            held_index[acquired] = idx;
            held_tag[acquired] = tag;
            acquired++;
            local_lease_ok++;
            sequence++;
        }

        for (int i = acquired - 1; i >= 0; --i)
        {
            uint8_t* mem = (uint8_t*)held[i];
            if (stability_verify_pattern(mem, args->object_size, held_tag[i]) == false)
            {
                local_corruption++;
                atomic_store(args->keep_running, false);
            }

            uint8_t expected_leased = 1U;
            if (atomic_compare_exchange_strong(&args->lease_state[held_index[i]], &expected_leased, 0U) == false)
            {
                local_double_free++;
                atomic_store(args->keep_running, false);
            }

            args->release_fn(held[i]);
            local_release_ok++;
        }

        if (acquired == 0)
        {
            thrd_yield();
        }
    }

    atomic_fetch_add(args->lease_ok, local_lease_ok);
    atomic_fetch_add(args->release_ok, local_release_ok);
    atomic_fetch_add(args->lease_fail, local_lease_fail);
    atomic_fetch_add(args->double_lease, local_double_lease);
    atomic_fetch_add(args->double_free, local_double_free);
    atomic_fetch_add(args->corruption, local_corruption);
    atomic_fetch_add(args->invalid_pointer, local_invalid_pointer);
    return 0;
}

TEST(test_5_minute_five_pool_stability_high_contention) {
    const int THREADS_PER_POOL = 2;
    const int POOL_COUNT = 5;
    const int TOTAL_THREADS = THREADS_PER_POOL * POOL_COUNT;
    const int DURATION_SECONDS = 300;

    static _Atomic(uint8_t) state_a[STABILITY_POOL_A_COUNT];
    static _Atomic(uint8_t) state_b[STABILITY_POOL_B_COUNT];
    static _Atomic(uint8_t) state_c[STABILITY_POOL_C_COUNT];
    static _Atomic(uint8_t) state_d[STABILITY_POOL_D_COUNT];
    static _Atomic(uint8_t) state_e[STABILITY_POOL_E_COUNT];

    init_static_heap_StabilityObjA32();
    init_static_heap_StabilityObjB33();
    init_static_heap_StabilityObjC96();
    init_static_heap_StabilityObjD65();
    init_static_heap_StabilityObjE160();

    for (size_t i = 0; i < STABILITY_POOL_A_COUNT; ++i) { atomic_store(&state_a[i], 0U); }
    for (size_t i = 0; i < STABILITY_POOL_B_COUNT; ++i) { atomic_store(&state_b[i], 0U); }
    for (size_t i = 0; i < STABILITY_POOL_C_COUNT; ++i) { atomic_store(&state_c[i], 0U); }
    for (size_t i = 0; i < STABILITY_POOL_D_COUNT; ++i) { atomic_store(&state_d[i], 0U); }
    for (size_t i = 0; i < STABILITY_POOL_E_COUNT; ++i) { atomic_store(&state_e[i], 0U); }

    typedef struct {
        const char* pool_name;
        int pool_id;
        size_t object_size;
        size_t node_size;
        size_t pool_capacity;
        const uint8_t* buffer_base;
        _Atomic(uint8_t)* lease_state;
        void* (*lease_fn)(void);
        void (*release_fn)(void*);
    } stability_pool_config_t;

    const stability_pool_config_t configs[POOL_COUNT] = {
        { "A32-aligned", 0, sizeof(StabilityObjA32), sizeof(struct StabilityObjA32_linked_list_node), STABILITY_POOL_A_COUNT, (const uint8_t*)&StabilityObjA32_buffer[0], state_a, lease_StabilityObjA32_void, release_StabilityObjA32_void },
        { "B33-off+1",   1, sizeof(StabilityObjB33), sizeof(struct StabilityObjB33_linked_list_node), STABILITY_POOL_B_COUNT, (const uint8_t*)&StabilityObjB33_buffer[0], state_b, lease_StabilityObjB33_void, release_StabilityObjB33_void },
        { "C96-aligned", 2, sizeof(StabilityObjC96), sizeof(struct StabilityObjC96_linked_list_node), STABILITY_POOL_C_COUNT, (const uint8_t*)&StabilityObjC96_buffer[0], state_c, lease_StabilityObjC96_void, release_StabilityObjC96_void },
        { "D65-off+1",   3, sizeof(StabilityObjD65), sizeof(struct StabilityObjD65_linked_list_node), STABILITY_POOL_D_COUNT, (const uint8_t*)&StabilityObjD65_buffer[0], state_d, lease_StabilityObjD65_void, release_StabilityObjD65_void },
        { "E160-aligned",4, sizeof(StabilityObjE160), sizeof(struct StabilityObjE160_linked_list_node), STABILITY_POOL_E_COUNT, (const uint8_t*)&StabilityObjE160_buffer[0], state_e, lease_StabilityObjE160_void, release_StabilityObjE160_void }
    };

    thrd_t threads[TOTAL_THREADS];
    stability_worker_args_t args[TOTAL_THREADS];
    _Atomic(bool) keep_running = true;
    _Atomic(uint64_t) lease_ok[POOL_COUNT];
    _Atomic(uint64_t) release_ok[POOL_COUNT];
    _Atomic(uint64_t) lease_fail[POOL_COUNT];
    _Atomic(uint64_t) double_lease[POOL_COUNT];
    _Atomic(uint64_t) double_free[POOL_COUNT];
    _Atomic(uint64_t) corruption[POOL_COUNT];
    _Atomic(uint64_t) invalid_pointer[POOL_COUNT];

    for (int i = 0; i < POOL_COUNT; ++i) {
        atomic_store(&lease_ok[i], 0);
        atomic_store(&release_ok[i], 0);
        atomic_store(&lease_fail[i], 0);
        atomic_store(&double_lease[i], 0);
        atomic_store(&double_free[i], 0);
        atomic_store(&corruption[i], 0);
        atomic_store(&invalid_pointer[i], 0);
    }

    printf("\n      Running %d-second STABILITY test across %d pools, %d threads per pool...\n",
           DURATION_SECONDS, POOL_COUNT, THREADS_PER_POOL);

    int t = 0;
    for (int p = 0; p < POOL_COUNT; ++p)
    {
        for (int w = 0; w < THREADS_PER_POOL; ++w)
        {
            args[t].pool_name = configs[p].pool_name;
            args[t].pool_id = configs[p].pool_id;
            args[t].worker_id = w;
            args[t].object_size = configs[p].object_size;
            args[t].node_size = configs[p].node_size;
            args[t].pool_capacity = configs[p].pool_capacity;
            args[t].buffer_base = configs[p].buffer_base;
            args[t].lease_state = configs[p].lease_state;
            args[t].lease_fn = configs[p].lease_fn;
            args[t].release_fn = configs[p].release_fn;
            args[t].keep_running = &keep_running;
            args[t].lease_ok = &lease_ok[p];
            args[t].release_ok = &release_ok[p];
            args[t].lease_fail = &lease_fail[p];
            args[t].double_lease = &double_lease[p];
            args[t].double_free = &double_free[p];
            args[t].corruption = &corruption[p];
            args[t].invalid_pointer = &invalid_pointer[p];

            int res = thrd_create(&threads[t], stability_pool_worker, &args[t]);
            assert(res == thrd_success);
            t++;
        }
    }

    thrd_sleep(&(struct timespec){.tv_sec = DURATION_SECONDS}, NULL);
    atomic_store(&keep_running, false);

    for (int i = 0; i < TOTAL_THREADS; ++i)
    {
        thrd_join(threads[i], NULL);
    }

    printf("\n      Stability Results (per pool):\n");
    for (int p = 0; p < POOL_COUNT; ++p)
    {
        uint64_t ok_lease = atomic_load(&lease_ok[p]);
        uint64_t ok_release = atomic_load(&release_ok[p]);
        uint64_t fail_lease = atomic_load(&lease_fail[p]);
        uint64_t err_double_lease = atomic_load(&double_lease[p]);
        uint64_t err_double_free = atomic_load(&double_free[p]);
        uint64_t err_corruption = atomic_load(&corruption[p]);
        uint64_t err_invalid_ptr = atomic_load(&invalid_pointer[p]);

        printf("      - %-12s leases=%lu releases=%lu lease_fail=%lu | errs: double_lease=%lu double_free=%lu corruption=%lu invalid_ptr=%lu\n",
               configs[p].pool_name,
               ok_lease,
               ok_release,
               fail_lease,
               err_double_lease,
               err_double_free,
               err_corruption,
               err_invalid_ptr);

        assert(ok_lease == ok_release);
        assert(err_double_lease == 0);
        assert(err_double_free == 0);
        assert(err_corruption == 0);
        assert(err_invalid_ptr == 0);
    }

    for (size_t i = 0; i < STABILITY_POOL_A_COUNT; ++i) { assert(atomic_load(&state_a[i]) == 0U); }
    for (size_t i = 0; i < STABILITY_POOL_B_COUNT; ++i) { assert(atomic_load(&state_b[i]) == 0U); }
    for (size_t i = 0; i < STABILITY_POOL_C_COUNT; ++i) { assert(atomic_load(&state_c[i]) == 0U); }
    for (size_t i = 0; i < STABILITY_POOL_D_COUNT; ++i) { assert(atomic_load(&state_d[i]) == 0U); }
    for (size_t i = 0; i < STABILITY_POOL_E_COUNT; ++i) { assert(atomic_load(&state_e[i]) == 0U); }
}

/* ── Main ───────────────────────────────────────────────────── */

int main(void) {
    printf("static_heap tests:\n");

    /* Basic functionality */
    RUN(test_init_heap);
    RUN(test_lease_single);
    RUN(test_release_single);
    RUN(test_lease_release_cycle);
    RUN(test_lease_all);
    RUN(test_null_pointer_handling);

    /* Multithreaded */
    RUN(test_concurrent_lease_release);
    RUN(test_concurrent_exhaustion);
    RUN(test_producer_consumer);

    /* Memory consistency */
    RUN(test_data_visibility);

    /* Long-running stress test */
    RUN(test_30_second_stress);

    /* malloc/free baseline */
    RUN(test_30_second_malloc_baseline);

    /* No-delay throughput tests (true allocator hot-path cost) */
    RUN(test_10_second_stress_nodelay);
    RUN(test_10_second_stress_nodelay_ptr);
    RUN(test_10_second_malloc_baseline_nodelay);

    /* Mixed-size workload across 3 pools, head-to-head with glibc */
    RUN(test_10_second_mixed_sizes_static_heap);
    RUN(test_10_second_mixed_sizes_malloc_baseline);

    /* 5-minute high-contention stability test across 5 distinct pools */
    RUN(test_5_minute_five_pool_stability_high_contention);

    printf("\nAll tests passed.\n");
    return 0;
}

