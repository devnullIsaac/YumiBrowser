/*
 * handle_table.h - Integer-handle ↔ host-pointer mapping table for WASM interop, with generation tracking for use-after-free detection.
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
 * @file handle_table.h
 * @brief Integer handle ↔ pointer mapping table for WASM interop.
 *
 * Maps 1-indexed integer handles (safe to pass across the WASM boundary)
 * to host-side pointers. Handle 0 is always invalid/null. The table
 * grows automatically when full and tracks generations for use-after-free
 * detection.
 *
 * ## Example
 *
 * @code{.c}
 * #include "handle_table.h"
 *
 * HandleTable ht;
 * htable_init(&ht, 64);
 *
 * // Insert a pointer, get a handle back
 * MyObject *obj = create_object();
 * uint32_t handle = htable_insert(&ht, obj);
 *
 * // Retrieve the pointer from the handle
 * MyObject *got = htable_get(&ht, handle);
 * assert(got == obj);
 *
 * // Remove the handle (frees the slot for reuse)
 * htable_remove(&ht, handle);
 * assert(htable_get(&ht, handle) == NULL);
 *
 * htable_destroy(&ht);
 * @endcode
 */

#ifndef HANDLE_TABLE_H
#define HANDLE_TABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Handle table mapping integer handles to host pointers.
 *
 * Slots are 1-indexed; handle 0 is reserved as "null". A freelist
 * manages recycled slots, and a generation counter per slot detects
 * use-after-free.
 */
typedef struct {
    void   **slots;          /**< Pointer storage (indexed by handle - 1). */
    uint32_t *generations;   /**< Per-slot generation counter. */
    uint32_t  capacity;      /**< Current allocated slot count. */
    uint32_t  first_free;    /**< (unused — freelist stack used instead). */
    uint32_t *freelist;      /**< Stack of free slot indices. */
    uint32_t  free_count;    /**< Number of entries in the freelist. */
} HandleTable;

/**
 * @brief Initialize a handle table with a given starting capacity.
 *
 * @param[out] t            Table to initialize.
 * @param[in]  initial_cap  Number of slots to pre-allocate.
 */
static inline void htable_init(HandleTable *t, uint32_t initial_cap) {
    t->capacity   = initial_cap;
    t->slots       = (void **)calloc(initial_cap, sizeof(void *));
    t->generations = (uint32_t *)calloc(initial_cap, sizeof(uint32_t));
    t->freelist    = (uint32_t *)malloc(initial_cap * sizeof(uint32_t));
    t->free_count  = initial_cap;
    for (uint32_t i = 0; i < initial_cap; i++)
        t->freelist[i] = initial_cap - 1 - i; /* stack: pop gives lowest first */
}

/**
 * @brief Destroy a handle table and free all internal memory.
 *
 * Does NOT free the objects that handles point to — the caller must
 * release those before calling this.
 *
 * @param[in,out] t  Table to destroy (zeroed after return).
 */
static inline void htable_destroy(HandleTable *t) {
    free(t->slots);
    free(t->generations);
    free(t->freelist);
    memset(t, 0, sizeof(*t));
}

/**
 * @brief Insert a pointer into the table and return its handle.
 *
 * If the table is full it doubles in capacity automatically.
 *
 * @param[in,out] t    Handle table.
 * @param[in]     ptr  Pointer to store (must not be NULL).
 * @return A non-zero handle on success, or 0 if @p ptr is NULL.
 */
static inline uint32_t htable_insert(HandleTable *t, void *ptr) {
    if (!ptr) return 0;
    if (t->free_count == 0) {
        /* Grow */
        uint32_t old_cap = t->capacity;
        uint32_t new_cap = old_cap * 2;
        t->slots       = (void **)realloc(t->slots,       new_cap * sizeof(void *));
        t->generations = (uint32_t *)realloc(t->generations,  new_cap * sizeof(uint32_t));
        t->freelist    = (uint32_t *)realloc(t->freelist,     new_cap * sizeof(uint32_t));
        memset(t->slots + old_cap,       0, (new_cap - old_cap) * sizeof(void *));
        memset(t->generations + old_cap, 0, (new_cap - old_cap) * sizeof(uint32_t));
        t->free_count = new_cap - old_cap;
        for (uint32_t i = 0; i < t->free_count; i++)
            t->freelist[i] = new_cap - 1 - i;
        t->capacity = new_cap;
    }
    uint32_t idx = t->freelist[--t->free_count];
    t->slots[idx] = ptr;
    return idx + 1;  /* 1-indexed */
}

/**
 * @brief Look up a pointer by its handle.
 *
 * @param[in] t       Handle table.
 * @param[in] handle  Handle returned by htable_insert().
 * @return The stored pointer, or NULL if the handle is invalid or was removed.
 */
static inline void *htable_get(HandleTable *t, uint32_t handle) {
    if (handle == 0 || handle > t->capacity) return NULL;
    return t->slots[handle - 1];
}

/**
 * @brief Remove a handle, returning its slot to the free pool.
 *
 * Increments the slot's generation counter so stale handles resolve to NULL.
 *
 * @param[in,out] t       Handle table.
 * @param[in]     handle  Handle to remove (0 and out-of-range are no-ops).
 */
static inline void htable_remove(HandleTable *t, uint32_t handle) {
    if (handle == 0 || handle > t->capacity) return;
    uint32_t idx = handle - 1;
    t->slots[idx] = NULL;
    t->generations[idx]++;
    t->freelist[t->free_count++] = idx;
}

#endif
