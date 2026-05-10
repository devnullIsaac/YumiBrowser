/*
    Clipboard WASM Bindings
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

#ifndef CLIPBOARD_BINDINGS_H
#define CLIPBOARD_BINDINGS_H

#include "deps.h"
#include <stdint.h>

/**
 * @file clipboard_bindings.h
 * @brief Host-side clipboard WASM bindings for Yumi Browser.
 *
 * Exports three functions into the guest "env" module:
 * `clipboard_available`, `clipboard_get`, `clipboard_set`.
 *
 * The dashboard app mediates clipboard access so no data leaks to a
 * webapp without user knowledge. The host never intercepts Ctrl+C/V/X;
 * the dashboard handles shortcut detection and decides whether to forward.
 *
 * ## Example
 *
 * @code{.c}
 * #include "clipboard_bindings.h"
 *
 * ClipboardBindings cb;
 * clipboard_bindings_init(&cb);
 * clipboard_bindings_set_memory(&cb, wasm_memory);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t count = clipboard_bindings_get_imports(&cb, store, &names, &funcs);
 * // Register the returned imports into the WASM instance.
 *
 * clipboard_bindings_destroy(&cb);
 * @endcode
 */

/**
 * @brief Clipboard binding state.
 */
typedef struct {
    wasm_memory_t *memory;  /**< Pointer to guest WASM linear memory. */
} ClipboardBindings;

/**
 * @brief Initialize clipboard bindings (zeroes internal state).
 * @param[out] b  Bindings to initialize.
 */
void   clipboard_bindings_init(ClipboardBindings *b);

/**
 * @brief Destroy clipboard bindings and release any resources.
 * @param[in,out] b  Bindings to destroy.
 */
void   clipboard_bindings_destroy(ClipboardBindings *b);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] b    Bindings.
 * @param[in]     mem  WASM memory from the guest instance.
 */
void   clipboard_bindings_set_memory(ClipboardBindings *b, wasm_memory_t *mem);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] b          Bindings (provides closure context).
 * @param[in]     store      WASM store for function creation.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports (name/func pairs).
 */
size_t clipboard_bindings_get_imports(ClipboardBindings *b, wasm_store_t *store,
                                      const char ***out_names,
                                      wasm_func_t ***out_funcs);

#endif /* CLIPBOARD_BINDINGS_H */
