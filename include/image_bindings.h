/*
    Still/Animated Image Decode WASM Bindings
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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