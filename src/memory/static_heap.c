/*
 * static_heap.c - Lock-free per-type static-heap allocator: atomic freelist of pre-allocated nodes used to avoid malloc on hot paths.
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

#include "memory.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <threads.h>
#include <assert.h>

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

static_heap(ClipboardBuffer, YUMI_BROWSER_MAX_CLIPBOARD_COUNT)
static_heap(DashboardGroupCtx, YUMI_BROWSER_MAX_DASHBOARD_COUNT)
static_heap(FileDialogCtx, YUMI_BROWSER_MAX_FILEDIALOG_COUNT)

void initialize_yumi_browser_static_heaps(void)
{
    init_static_heap_ClipboardBuffer();
    init_static_heap_DashboardGroupCtx();
    init_static_heap_FileDialogCtx();
}
#undef static_heap
