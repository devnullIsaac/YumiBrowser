#ifndef YUMI_STATIC_MEMORY_H
#include <stdint.h>
#include <stddef.h>
#include "crypto.h"
#include "webapp_runtime.h"

#ifndef YUMI_CACHE_LINE_SIZE
#define YUMI_CACHE_LINE_SIZE 64
#endif

#ifndef YUMI_STATIC_HEAP_MAX_THREADS
#define YUMI_STATIC_HEAP_MAX_THREADS 64
#endif

#define YUMI_BROWSER_MAX_CONNECTIONS 128
#define YUMI_BROWSER_MAX_RUNTIME_COUNT 64
#define YUMI_BROWSER_MAX_CLIPBOARD_COUNT 2
#define YUMI_BROWSER_MAX_DASHBOARD_COUNT 1
#define YUMI_BROWSER_MAX_FILEDIALOG_COUNT 2

#define CLIPBOARD_MAX_SIZE (1024 * 1024) + 1  /* 1 MB + 1 byte for null terminator */

typedef enum YUMI_MEMORY_ALLOC_ERROR_ENUM {
    YUMI_MEMORY_ALLOC_UNKNOWN = 0,
    YUMI_MEMORY_ALLOC_SUCCESS,
    YUMI_MEMORY_ALLOC_OUT_OF_RANGE,
    YUMI_MEMORY_ALLOC_INVALID_OUT_POINTER,
    YUMI_MEMORY_ALLOC_UNAVAILABLE_MEMORY,
    YUMI_MEMORY_ALLOC_INVALID_IN_POINTER,
} yumi_memory_alloc_error_enum;

void initialize_yumi_browser_static_heaps(void);

typedef struct ClipboardBuffer {
    char buffer[CLIPBOARD_MAX_SIZE];
} ClipboardBuffer;

yumi_memory_alloc_error_enum lease_ClipboardBuffer(ClipboardBuffer** outNode);
yumi_memory_alloc_error_enum release_ClipboardBuffer(ClipboardBuffer* node);

/* Event callback context for dashboard group events */
typedef struct {
    DashboardRuntime *d;
    uint32_t          group_index;
} DashboardGroupCtx;

yumi_memory_alloc_error_enum lease_DashboardGroupCtx(DashboardGroupCtx** outNode);
yumi_memory_alloc_error_enum release_DashboardGroupCtx(DashboardGroupCtx* node);

/* Context passed to SDL3 async file dialog callbacks */
typedef struct {
    DashboardRuntime *d;
    uint32_t          slot_index;
    uint32_t          handle;     /* IPC_FILE_OPEN_PENDING / SAVE / FOLDER */
} FileDialogCtx;

yumi_memory_alloc_error_enum lease_FileDialogCtx(FileDialogCtx** outNode);
yumi_memory_alloc_error_enum release_FileDialogCtx(FileDialogCtx* node);
#endif
