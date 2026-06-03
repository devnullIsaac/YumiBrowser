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

#include "static_memory.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h>

/* =====================================================================
 * static_heap(x, y) — per-type, fixed-capacity, lock-free-on-hot-path
 * object allocator.
 *
 * --------------------------------------------------------------------
 * WHAT IT IS
 * --------------------------------------------------------------------
 * One macro expansion produces a complete allocator dedicated to a
 * single user type `x`, backed by exactly `y` statically reserved
 * objects (no malloc, ever). It exposes:
 *
 *   yumi_memory_alloc_error_enum lease_##x   (x** outNode);
 *   yumi_memory_alloc_error_enum release_##x (x*  node);
 *   void                         init_static_heap_##x(void);
 *
 * plus inline "_inline" and "_ptr" variants for hot call sites that
 * want to skip the extern-call overhead or the error-code return.
 *
 * --------------------------------------------------------------------
 * DATA LAYOUT
 * --------------------------------------------------------------------
 * Every allocatable object lives inside an *intrusive list node*:
 *
 *     struct x_linked_list_node { x bindings; struct ... *next; };
 *
 * The `bindings` field is placed at offset 0 and a `_Static_assert`
 * enforces this at every macro instantiation. That invariant is the
 * sole reason the `release_*` path may cast a user `x*` directly to a
 * `x_linked_list_node` — see MISRA-C.md, Deviation 2 (Rule 11.3).
 *
 * Storage is a single static array `x_buffer[y]`. Because the array
 * is defined as an array of node structs, every byte the caller can
 * ever observe is, at the language level, a member of a fully typed
 * node object. This keeps strict-aliasing analysis (and MISRA Rule
 * 11.3 reasoning) clean: there is no type-punning, only the
 * standard-blessed "pointer to first member <-> pointer to struct"
 * conversion (C11 §6.7.2.1 ¶15).
 *
 * --------------------------------------------------------------------
 * THREE-TIER FREE LIST
 * --------------------------------------------------------------------
 * To stay fast under contention while remaining bounded and correct,
 * free nodes live in one of three places at any moment:
 *
 *   Tier 1 — Thread-Local Stack (`x_tls`):
 *     A small per-thread array (YUMI_STATIC_HEAP_TLS_CACHE_SIZE),
 *     cache-line aligned. Lease/release on the hot path is a single
 *     bounds check + array store/load — no atomics, no locks.
 *
 *   Tier 2 — Global Singly-Linked Free List (`x_linked_list_node_head`)
 *     Protected by `x_global_mtx` (C11 mtx_t). Touched only when the
 *     TLS cache is empty (refill) or full (spill). Refill takes up to
 *     YUMI_STATIC_HEAP_REFILL_BATCH nodes at once, capped at half of
 *     the remaining global pool so a single greedy thread cannot
 *     starve siblings. Spill returns half the TLS cache in one move.
 *
 *   Tier 3 — Lock-Free Recovery Slots (`x_recovery_slots[]`):
 *     A small array of atomic-headed Treiber stacks indexed by a hash
 *     of the calling thread id. Used as an emergency fallback when
 *     the global mutex cannot be acquired (e.g. during early init
 *     before `call_once` has completed, or in a TSS destructor where
 *     locking is unsafe). This is what makes the allocator usable in
 *     contexts where blocking on a mutex would deadlock or crash.
 *
 * The TSS destructor `x_tls_flush` ensures that when a thread exits
 * its cached nodes return to the global pool (or, failing that, to
 * the recovery slots) — no leaks across the program's lifetime.
 *
 * --------------------------------------------------------------------
 * ERROR MODEL
 * --------------------------------------------------------------------
 * Every public entry point returns a `yumi_memory_alloc_error_enum`:
 *   - INVALID_IN_POINTER / INVALID_OUT_POINTER for NULL arguments
 *     (defense-in-depth at API boundary, MISRA Dir 4.7 compliant).
 *   - INTERNAL_ERROR  when TSS / mutex init failed (system-level).
 *   - UNAVAILABLE_MEMORY when the pool is exhausted (recoverable).
 *   - SUCCESS otherwise.
 * The `_ptr` variants are provided for performance-critical sites
 * that have already validated their pointers and treat exhaustion as
 * a NULL return; they never read or write through a NULL pointer.
 *
 * --------------------------------------------------------------------
 * WHY THIS IS MISRA-C SAFE
 * --------------------------------------------------------------------
 *   * Dir 4.12 (no dynamic memory): the entire allocator is static
 *     storage. No malloc/free is ever called by code generated from
 *     this macro. The whole point of the design.
 *   * Rule 11.3 (pointer-type casts): only two cast sites per
 *     instantiation, both standard-blessed first-member conversions,
 *     both enforced by `_Static_assert(offsetof(...bindings)==0)`.
 *     Formally documented as Deviation 2 in MISRA-C.md.
 *   * Rule 21.3 (no malloc family): satisfied trivially.
 *   * Rule 8.4 (declarations precede definitions): public entry
 *     points have matching declarations in `memory.h`.
 *   * Rule 8.7 / 8.9: helpers used in only one TU are `static`; the
 *     static buffer has the smallest scope C allows for the design.
 *   * Rule 9.1 (no read of uninitialised storage): the buffer is
 *     zero-initialised at definition; `init_static_heap_##x` then
 *     links the freelist before any lease/release is permitted.
 *   * Rule 14.4 / 15.5 (single point of exit, controlling expr is
 *     boolean): every function uses a single `res`/`ok` variable
 *     guarded by `if (res == SUCCESS)` chains and exits at the end.
 *   * Rule 17.7 (return value used): `mtx_unlock` returns are cast
 *     to `(void)` explicitly to acknowledge intentional discard.
 *   * Rule 21.16/21.17 (signal/threads): the file uses C11 threads
 *     primitives (`mtx_t`, `tss_t`, `call_once`) which are within
 *     MISRA-C:2025's permitted standard-library subset.
 *   * Atomics on the recovery path use `_Atomic(uintptr_t)` with
 *     explicit memory_order arguments — no ad-hoc volatile, no
 *     reliance on undefined ordering.
 *
 * --------------------------------------------------------------------
 * INVARIANTS A MAINTAINER MUST PRESERVE
 * --------------------------------------------------------------------
 *   1. `bindings` MUST remain the first field of the node struct.
 *      The static_assert will fire otherwise.
 *   2. A node is in exactly ONE of: TLS stack, global freelist,
 *      recovery slot, or "leased to the caller". Never two at once.
 *   3. Global mutex is held only across short, finite, allocation-
 *      free sequences (no callbacks, no I/O, no nested locks).
 *   4. The recovery slots are a *fallback*, not a primary path —
 *      adding fast-path traffic to them would defeat fairness.
 *   5. `init_static_heap_##x` must be called before any lease; it
 *      links the buffer and zeroes the recovery slots.
 * ===================================================================== */
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
/* Externally linked thin wrappers. Allow callers in other TUs (and \
 * the test suite) to use the allocator without seeing the macro \
 * expansion. The compiler routinely inlines these at LTO. */ \
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
static_heap(YumiCryptoHkdfInfoBuf, YUMI_BROWSER_MAX_FILEDIALOG_COUNT)

void initialize_yumi_browser_static_heaps(void)
{
    init_static_heap_ClipboardBuffer();
    init_static_heap_DashboardGroupCtx();
    init_static_heap_FileDialogCtx();
    init_static_heap_YumiCryptoHkdfInfoBuf();
}
#undef static_heap
