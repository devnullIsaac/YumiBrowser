/*
    Logging WASM Bindings
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
 * @file log_bindings.h
 * @brief Logging WASM bindings for Yumi Browser.
 *
 * Exposes a `console_log` (and related) import so guest WASM modules
 * can print messages to the host's standard output.
 *
 * ## Example
 *
 * @code{.c}
 * #include "log_bindings.h"
 *
 * LogBindings lb;
 * log_bindings_init(&lb);
 * log_bindings_set_memory(&lb, wasm_memory);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t n = log_bindings_get_imports(&lb, store, &names, &funcs);
 *
 * log_bindings_destroy(&lb);
 * @endcode
 */

#ifndef LOG_BINDINGS_H
#define LOG_BINDINGS_H

#include "deps.h"

/**
 * @brief Log binding state.
 */
typedef struct {
    wasm_memory_t *memory;  /**< Guest WASM linear memory. */
} LogBindings;

/**
 * @brief Initialize log bindings.
 * @param[out] b  Bindings to initialize.
 */
void     log_bindings_init(LogBindings *b);

/**
 * @brief Destroy log bindings.
 * @param[in,out] b  Bindings to destroy.
 */
void     log_bindings_destroy(LogBindings *b);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] b    Bindings.
 * @param[in]     mem  Guest WASM memory.
 */
void     log_bindings_set_memory(LogBindings *b, wasm_memory_t *mem);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] b          Bindings.
 * @param[in]     store      WASM store.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t   log_bindings_get_imports(LogBindings *b, wasm_store_t *store,
                                  const char ***out_names,
                                  wasm_func_t ***out_funcs);

#endif /* LOG_BINDINGS_H */
