/*
 * font_bindings.h - Host-side font management (FreeType) exposed to WASM guests as integer handles, with system font lookup and on-disk file loading.
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
 * @file font_bindings.h
 * @brief Font loading and rendering WASM bindings for Yumi Browser.
 *
 * Provides host-side font management backed by FreeType. Fonts are loaded
 * by name (system font lookup) or by file path, stored in a handle table,
 * and exposed to WASM guests as integer handles.
 *
 * ## Example
 *
 * @code{.c}
 * #include "font_bindings.h"
 *
 * FontBindings fb;
 * font_bindings_init(&fb);
 * font_bindings_set_memory(&fb, wasm_memory);
 *
 * // Load a system font by name (host-only convenience)
 * uint32_t handle = font_bindings_load_system_direct(&fb, "DejaVu Sans");
 *
 * // Check if the font has a particular glyph
 * if (font_bindings_has_glyph_direct(&fb, handle, 0x1F600))
 *     printf("Font has emoji glyph!\n");
 *
 * font_bindings_destroy(&fb);
 * @endcode
 */

#ifndef FONT_BINDINGS_H
#define FONT_BINDINGS_H

#include "deps.h"
#include "handle_table.h"
#include <stdint.h>

/**
 * @brief Font binding state.
 */
typedef struct FontBindings {
    wasm_memory_t *memory;      /**< Guest WASM linear memory. */
    HandleTable    fonts;        /**< Font handle → FontEntry mapping. */

    char         **font_dirs;    /**< System font search directories. */
    int            font_dir_count; /**< Number of search directories. */
} FontBindings;

/** @brief Opaque forward declaration for a loaded font entry. */
typedef struct FontEntry FontEntry;

/**
 * @brief Initialize font bindings and discover system font paths.
 * @param[out] b  Bindings to initialize.
 */
void   font_bindings_init(FontBindings *b);

/**
 * @brief Destroy font bindings and release all loaded fonts.
 * @param[in,out] b  Bindings to destroy.
 */
void   font_bindings_destroy(FontBindings *b);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] b    Bindings.
 * @param[in]     mem  Guest WASM memory.
 */
void   font_bindings_set_memory(FontBindings *b, wasm_memory_t *mem);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] b          Bindings.
 * @param[in]     store      WASM store.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t font_bindings_get_imports(FontBindings *b, wasm_store_t *store,
                                 const char ***out_names,
                                 wasm_func_t ***out_funcs);

/**
 * @brief Load a system font by name (host-only, not callable from WASM).
 *
 * @param[in,out] b     Bindings.
 * @param[in]     name  Font family name (e.g. "DejaVu Sans").
 * @return Font handle (>0) on success, 0 if not found.
 */
uint32_t font_bindings_load_system_direct(FontBindings *b, const char *name);

/**
 * @brief Check if a loaded font contains a glyph for a codepoint.
 *
 * @param[in] b          Bindings.
 * @param[in] handle     Font handle from a load function.
 * @param[in] codepoint  Unicode codepoint to test.
 * @return 1 if the glyph exists, 0 otherwise.
 */
int      font_bindings_has_glyph_direct(FontBindings *b, uint32_t handle, uint32_t codepoint);

/**
 * @brief Get raw font file bytes for a loaded font handle.
 *
 * Used by text_bindings to create a HarfBuzz font from the same data.
 *
 * @param[in]  b         Bindings.
 * @param[in]  handle    Font handle.
 * @param[out] out_size  Receives the byte count.
 * @return Pointer to the font data, or NULL if the handle is invalid.
 */
const uint8_t *font_bindings_get_data(FontBindings *b, uint32_t handle, uint32_t *out_size);

/**
 * @brief Find a system font covering a given codepoint via fontconfig.
 *
 * Loads the font into the handle table if not already loaded.
 *
 * @param[in,out] b          Bindings.
 * @param[in]     codepoint  Unicode codepoint to search for.
 * @return Font handle (>0) on success, 0 if no font found.
 */
uint32_t font_bindings_find_font_for_codepoint(FontBindings *b, uint32_t codepoint);

#endif /* FONT_BINDINGS_H */
