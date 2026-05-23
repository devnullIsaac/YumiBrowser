#include "memory.h"
#include <stdbool.h>
#include <stdatomic.h>

#define static_heap(x, y) \
typedef struct x##_linked_list_node { \
    x bindings; \
    struct x##_linked_list_node* next; \
} *x##_linked_list_node; \
struct x##_linked_list_node x##_buffer[y] = {0}; \
_Atomic(x##_linked_list_node) x##_linked_list_node_head; \
void init_static_heap_##x(void) \
{ \
    const size_t limit = sizeof(x##_buffer) / sizeof(struct x##_linked_list_node); \
    for (size_t i = 1; i < limit; ++i) \
    { \
        x##_buffer[i - 1].next = &x##_buffer[i]; \
    } \
    x##_buffer[limit - 1].next = NULL; \
    atomic_store_explicit(&x##_linked_list_node_head, &x##_buffer[0], memory_order_release); \
} \
yumi_memory_alloc_error_enum release_##x(x* node) { \
    yumi_memory_alloc_error_enum res = YUMI_MEMORY_ALLOC_SUCCESS; \
    if (node == NULL) \
    { \
        res = YUMI_MEMORY_ALLOC_INVALID_IN_POINTER; \
    } \
    if (res == YUMI_MEMORY_ALLOC_SUCCESS) \
    { \
        x##_linked_list_node item = (x##_linked_list_node)node; \
        int32_t retry = 0; \
        for (; retry < 32; ++retry) \
        { \
             x##_linked_list_node current = atomic_load_explicit(&x##_linked_list_node_head, memory_order_relaxed); \
             item->next = current; \
             if (atomic_compare_exchange_weak_explicit(&x##_linked_list_node_head, &current, item, memory_order_release, memory_order_relaxed) == true) \
                 break; \
        } \
    } \
    return res; \
} \
yumi_memory_alloc_error_enum lease_##x(x** outNode) \
{ \
    yumi_memory_alloc_error_enum res = YUMI_MEMORY_ALLOC_SUCCESS; \
    if (outNode == NULL) \
    { \
        res = YUMI_MEMORY_ALLOC_INVALID_OUT_POINTER; \
    } \
    if ((res == YUMI_MEMORY_ALLOC_SUCCESS) && (atomic_load_explicit(&x##_linked_list_node_head, memory_order_relaxed) == NULL)) { \
        res = YUMI_MEMORY_ALLOC_UNAVAILABLE_MEMORY; \
    } \
    if (res == YUMI_MEMORY_ALLOC_SUCCESS) \
    { \
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
                    break; \
                } \
            } \
            else \
            { \
                /* MISRA-C: Exit loop, a workaround for only one break per loop rule. */ \
                retry = 10;\
            } \
        } \
        if (retry == 10) \
        { \
            res = YUMI_MEMORY_ALLOC_UNAVAILABLE_MEMORY; \
        } \
    } \
    return res; \
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
