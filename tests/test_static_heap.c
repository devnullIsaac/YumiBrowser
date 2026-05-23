/**
 * @file test_static_heap.c
 * @brief Multithreaded tests for static_heap allocator
 */

#include <stdio.h>
#include <assert.h>
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

/* Error codes */
typedef enum {
    YUMI_MEMORY_ALLOC_SUCCESS = 0,
    YUMI_MEMORY_ALLOC_INVALID_IN_POINTER,
    YUMI_MEMORY_ALLOC_INVALID_OUT_POINTER,
    YUMI_MEMORY_ALLOC_UNAVAILABLE_MEMORY
} yumi_memory_alloc_error_enum;

/* Test structure */
typedef struct {
    uint32_t id;
    uint32_t magic;
    uint8_t data[56]; /* Total 64 bytes */
} TestObject;

/* Instantiate the allocator.
 *
 * Two-tier free list:
 *   - A global lock-free LIFO (atomic head, CAS push/pop) holds the buffer
 *     initially and acts as the shared pool.
 *   - Each thread keeps a private LIFO (thread-local head) that absorbs
 *     release()/lease() pairs without touching the global atomic. This
 *     removes cache-line contention on the common hot path.
 *
 * lease():   pop from TLS first; if empty, pop one node from the global pool.
 * release(): push onto TLS only (never touches the global atomic).
 * thread exit: a tss_t destructor splices the TLS list back to the global
 *              pool so nodes are not stranded when the thread dies.
 * init():    rebuilds the global free list AND clears the calling thread's
 *            TLS head. Must not be called concurrently with lease/release.
 */
#define static_heap(x, y) \
typedef struct x##_linked_list_node { \
    x bindings; \
    struct x##_linked_list_node* next; \
} *x##_linked_list_node; \
struct x##_linked_list_node x##_buffer[y] = {0}; \
_Atomic(x##_linked_list_node) x##_linked_list_node_head; \
/* Recovery array: when a thread exits and the CAS retry budget is */ \
/* exhausted trying to splice its TLS list back into the global pool, */ \
/* the chain is parked here so a future lease() can rescue it. */ \
static _Atomic(x##_linked_list_node) x##_recovery[YUMI_STATIC_HEAP_MAX_THREADS]; \
typedef struct { \
    _Alignas(YUMI_CACHE_LINE_SIZE) x##_linked_list_node head; \
    bool registered; \
    char _pad[YUMI_CACHE_LINE_SIZE - sizeof(x##_linked_list_node) - sizeof(bool)]; \
} x##_tls_slot_t; \
_Static_assert(sizeof(x##_tls_slot_t) == YUMI_CACHE_LINE_SIZE, "TLS slot must be exactly one cache line"); \
static thread_local x##_tls_slot_t x##_tls = {0}; \
static tss_t x##_tls_key; \
static once_flag x##_tls_once = ONCE_FLAG_INIT; \
static void x##_tls_flush(void* unused_arg) \
{ \
    (void)unused_arg; \
    x##_linked_list_node node = x##_tls.head; \
    x##_tls.head = NULL; \
    if (node != NULL) \
    { \
        x##_linked_list_node tail = node; \
        while (tail->next != NULL) \
        { \
            tail = tail->next; \
        } \
        bool deposited = false; \
        int32_t retry = 0; \
        for (; retry < 32; ++retry) \
        { \
            x##_linked_list_node current = atomic_load_explicit(&x##_linked_list_node_head, memory_order_relaxed); \
            tail->next = current; \
            if (atomic_compare_exchange_weak_explicit(&x##_linked_list_node_head, &current, node, memory_order_release, memory_order_relaxed) == true) \
            { \
                deposited = true; \
                retry = 32; \
            } \
        } \
        if (deposited == false) \
        { \
            /* Global head was too contended. Park the chain in a recovery */ \
            /* slot; lease() will rescue it. Detach the chain first so the */ \
            /* parked tail does not point at whatever the global head was. */ \
            tail->next = NULL; \
            int32_t slot = 0; \
            for (; slot < YUMI_STATIC_HEAP_MAX_THREADS; ++slot) \
            { \
                x##_linked_list_node expected = NULL; \
                if (atomic_compare_exchange_strong_explicit(&x##_recovery[slot], &expected, node, memory_order_release, memory_order_relaxed) == true) \
                { \
                    deposited = true; \
                    slot = YUMI_STATIC_HEAP_MAX_THREADS; \
                } \
            } \
            /* If every slot is occupied and global is contended, the chain */ \
            /* is leaked. With MAX_THREADS sized appropriately this cannot */ \
            /* happen: at most one chain per thread can ever be in flight. */ \
        } \
    } \
} \
static void x##_tls_key_init(void) \
{ \
    (void)tss_create(&x##_tls_key, x##_tls_flush); \
} \
static inline void x##_ensure_tls(void) \
{ \
    if (x##_tls.registered == false) \
    { \
        call_once(&x##_tls_once, x##_tls_key_init); \
        (void)tss_set(x##_tls_key, (void*)(uintptr_t)1); \
        x##_tls.registered = true; \
    } \
} \
void init_static_heap_##x(void) \
{ \
    const size_t limit = sizeof(x##_buffer) / sizeof(struct x##_linked_list_node); \
    for (size_t i = 1; i < limit; ++i) \
    { \
        x##_buffer[i - 1].next = &x##_buffer[i]; \
    } \
    x##_buffer[limit - 1].next = NULL; \
    x##_tls.head = NULL; \
    atomic_store_explicit(&x##_linked_list_node_head, &x##_buffer[0], memory_order_release); \
} \
yumi_memory_alloc_error_enum release_##x(x* node); \
yumi_memory_alloc_error_enum lease_##x(x** outNode); \
static inline yumi_memory_alloc_error_enum release_##x##_inline(x* node) \
{ \
    yumi_memory_alloc_error_enum res = YUMI_MEMORY_ALLOC_SUCCESS; \
    if (node == NULL) \
    { \
        res = YUMI_MEMORY_ALLOC_INVALID_IN_POINTER; \
    } \
    if (res == YUMI_MEMORY_ALLOC_SUCCESS) \
    { \
        x##_linked_list_node item = (x##_linked_list_node)node; \
        item->next = x##_tls.head; \
        x##_tls.head = item; \
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
        x##_ensure_tls(); \
        if (x##_tls.head != NULL) \
        { \
            x##_linked_list_node item = x##_tls.head; \
            x##_tls.head = item->next; \
            *outNode = &item->bindings; \
        } \
        else \
        { \
            bool found = false; \
            int32_t retry = 0; \
            for (; retry < 10; ++retry) \
            { \
                x##_linked_list_node current = atomic_load_explicit(&x##_linked_list_node_head, memory_order_relaxed); \
                if (current != NULL) \
                { \
                    bool casRes = atomic_compare_exchange_weak_explicit(&x##_linked_list_node_head, &current, current->next, memory_order_acquire, memory_order_relaxed); \
                    if (casRes == true) \
                    { \
                        *outNode = &current->bindings; \
                        found = true; \
                        retry = 10; \
                    } \
                } \
                else \
                { \
                    /* MISRA-C: Exit loop, a workaround for only one break per loop rule. */ \
                    retry = 10; \
                } \
            } \
            if (found == false) \
            { \
                /* Global pool empty (or unreachable). Scan the recovery */ \
                /* array for chains parked by exiting threads. */ \
                int32_t slot = 0; \
                for (; slot < YUMI_STATIC_HEAP_MAX_THREADS; ++slot) \
                { \
                    x##_linked_list_node chain = atomic_exchange_explicit(&x##_recovery[slot], NULL, memory_order_acquire); \
                    if (chain != NULL) \
                    { \
                        *outNode = &chain->bindings; \
                        /* Park the remainder of the rescued chain in our */ \
                        /* TLS list (which is empty here) for cheap reuse. */ \
                        x##_tls.head = chain->next; \
                        found = true; \
                        slot = YUMI_STATIC_HEAP_MAX_THREADS; \
                    } \
                } \
            } \
            if (found == false) \
            { \
                res = YUMI_MEMORY_ALLOC_UNAVAILABLE_MEMORY; \
            } \
        } \
    } \
    return res; \
} \
yumi_memory_alloc_error_enum release_##x(x* node) \
{ \
    return release_##x##_inline(node); \
} \
yumi_memory_alloc_error_enum lease_##x(x** outNode) \
{ \
    return lease_##x##_inline(outNode); \
}

/* Create heap with 100 objects */
static_heap(TestObject, 1000)

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
    TestObject* objs[1000];

    for (int i = 0; i < 1000; i++) {
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
    for (int i = 0; i < 1000; i++) {
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

    printf("\nAll tests passed.\n");
    return 0;
}

