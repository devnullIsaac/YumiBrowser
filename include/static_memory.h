/*
 * memory.h - Static-heap allocator pool sizes and macro plumbing for per-type lock-free freelist allocators used throughout Yumi Browser.
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

#ifndef YUMI_STATIC_MEMORY_H
#define YUMI_STATIC_MEMORY_H
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "crypto.h"

#ifndef YUMI_CACHE_LINE_SIZE
#define YUMI_CACHE_LINE_SIZE 64
#endif

#ifndef YUMI_STATIC_HEAP_MAX_THREADS
#define YUMI_STATIC_HEAP_MAX_THREADS 64
#endif

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
#define YUMI_LIKELY(x)          __builtin_expect(!!(x), 1)
#define YUMI_UNLIKELY(x)        __builtin_expect(!!(x), 0)
#define YUMI_COLD               __attribute__((cold, noinline))
#define YUMI_ALWAYS_INLINE      __attribute__((always_inline)) inline
#define YUMI_DO_NOT_OPTIMIZE(p) __asm__ __volatile__("" : : "g"(p) : "memory")
#else
#define YUMI_LIKELY(x)          (x)
#define YUMI_UNLIKELY(x)        (x)
#define YUMI_COLD
#define YUMI_ALWAYS_INLINE      inline
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

#define YUMI_BROWSER_MAX_CONNECTIONS 128
#define YUMI_BROWSER_MAX_RUNTIME_COUNT 64
#define YUMI_BROWSER_MAX_CLIPBOARD_COUNT 2
#define YUMI_BROWSER_MAX_DASHBOARD_COUNT 1
#define YUMI_BROWSER_MAX_FILEDIALOG_COUNT 2
#define YUMI_BROWSER_MAX_HKDF_INFO_BUF_COUNT YUMI_BROWSER_MAX_CONNECTIONS

#define CLIPBOARD_MAX_SIZE (1024 * 1024) + 1  /* 1 MB + 1 byte for null terminator */

typedef struct DashboardRuntime DashboardRuntime;

typedef enum YUMI_MEMORY_ALLOC_ERROR_ENUM {
    YUMI_MEMORY_ALLOC_UNKNOWN = 0,
    YUMI_MEMORY_ALLOC_SUCCESS,
    YUMI_MEMORY_ALLOC_OUT_OF_RANGE,
    YUMI_MEMORY_ALLOC_INVALID_OUT_POINTER,
    YUMI_MEMORY_ALLOC_UNAVAILABLE_MEMORY,
    YUMI_MEMORY_ALLOC_INVALID_IN_POINTER,
    YUMI_MEMORY_ALLOC_INTERNAL_ERROR,
} yumi_memory_alloc_error_enum;

void initialize_yumi_browser_static_heaps(void);

typedef struct ClipboardBuffer {
    char buffer[CLIPBOARD_MAX_SIZE];
} ClipboardBuffer;

yumi_memory_alloc_error_enum lease_ClipboardBuffer(ClipboardBuffer** outNode);
yumi_memory_alloc_error_enum release_ClipboardBuffer(ClipboardBuffer* node);

/* Event callback context for dashboard group events */
typedef struct DashboardGroupCtx {
    DashboardRuntime *d;
    uint32_t          group_index;
} DashboardGroupCtx;

yumi_memory_alloc_error_enum lease_DashboardGroupCtx(DashboardGroupCtx** outNode);
yumi_memory_alloc_error_enum release_DashboardGroupCtx(DashboardGroupCtx* node);

/* Context passed to SDL3 async file dialog callbacks */
typedef struct FileDialogCtx {
    DashboardRuntime *d;
    uint32_t          slot_index;
    uint32_t          handle;     /* IPC_FILE_OPEN_PENDING / SAVE / FOLDER */
} FileDialogCtx;

yumi_memory_alloc_error_enum lease_FileDialogCtx(FileDialogCtx** outNode);
yumi_memory_alloc_error_enum release_FileDialogCtx(FileDialogCtx* node);

typedef struct YumiCryptoHkdfInfoBuf {
    uint8_t info[256];
} YumiCryptoHkdfInfoBuf;

yumi_memory_alloc_error_enum lease_YumiCryptoHkdfInfoBuf(YumiCryptoHkdfInfoBuf** outNode);
yumi_memory_alloc_error_enum release_YumiCryptoHkdfInfoBuf(YumiCryptoHkdfInfoBuf* node);
#endif
