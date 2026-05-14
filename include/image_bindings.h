/**
 * @file image_bindings.h
 * @brief Still/animated image decode → WebGPU texture WASM bindings.
 *
 * Opens images by file path or WASM memory buffer, decodes all frames
 * via FFmpeg (and optionally LibRaw for camera RAW), and uploads them
 * as BGRA WebGPU textures.
 *
 * ## Example
 *
 * @code{.c}
 * #include "image_bindings.h"
 *
 * ImageBindings ib;
 * image_bindings_init(&ib, &wgpu_bindings);
 * image_bindings_set_memory(&ib, wasm_memory);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t n = image_bindings_get_imports(&ib, store, &names, &funcs);
 * // ... register imports, then WASM guest calls image_open / image_frame ...
 *
 * image_bindings_destroy(&ib);
 * @endcode
 */

#ifndef IMAGE_BINDINGS_H
#define IMAGE_BINDINGS_H

#include "deps.h"
#include "handle_table.h"
#include "wgpu_bindings.h"

/**
 * @brief Image binding state.
 */
typedef struct {
    WgpuBindings  *wgpu;       /**< WebGPU bindings for texture creation. */
    HandleTable    ht_image;    /**< Maps handle → ImageContext*. */
    wasm_memory_t *memory;      /**< Guest WASM linear memory. */
} ImageBindings;

/**
 * @brief Initialize image bindings.
 * @param[out] ib    Bindings to initialize.
 * @param[in]  wgpu  WebGPU bindings (for texture upload).
 */
void   image_bindings_init(ImageBindings *ib, WgpuBindings *wgpu);

/**
 * @brief Destroy image bindings and free all decoded images.
 * @param[in,out] ib  Bindings to destroy.
 */
void   image_bindings_destroy(ImageBindings *ib);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] ib   Bindings.
 * @param[in]     mem  Guest WASM memory.
 */
void   image_bindings_set_memory(ImageBindings *ib, wasm_memory_t *mem);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] ib         Bindings.
 * @param[in]     store      WASM store.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t image_bindings_get_imports(ImageBindings  *ib,
                                  wasm_store_t   *store,
                                  const char   ***out_names,
                                  wasm_func_t  ***out_funcs);

#endif /* IMAGE_BINDINGS_H */