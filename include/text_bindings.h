/*
 * text_bindings.h - HarfBuzz + ICU text shaping WASM bindings with full Unicode BiDi support, using FreeType fonts from font_bindings.
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
 * @file text_bindings.h
 * @brief Text shaping WASM bindings using HarfBuzz + ICU.
 *
 * Provides host-side text shaping with full Unicode BiDi support.
 * Fonts are loaded via font_bindings (FreeType) and then shaped
 * with HarfBuzz. Supports multiple loaded fonts with an active-font
 * selector.
 *
 * ## Example
 *
 * @code{.c}
 * #include "text_bindings.h"
 *
 * TextBindings tb;
 * text_bindings_init(&tb);
 * text_bindings_set_memory(&tb, wasm_memory);
 * text_bindings_set_font_bindings(&tb, &font_bindings);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t n = text_bindings_get_imports(&tb, store, &names, &funcs);
 * // Guest can now call text_load_font, text_shape, etc.
 *
 * text_bindings_destroy(&tb);
 * @endcode
 */

#ifndef TEXT_BINDINGS_H
#define TEXT_BINDINGS_H

#include "deps.h"
#include <stdint.h>

#include <harfbuzz/hb.h>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/ustring.h>

struct FontBindings;

/**
 * @brief A loaded text font ready for HarfBuzz shaping.
 */
typedef struct {
    hb_blob_t  *blob;        /**< HarfBuzz blob referencing font data. */
    hb_face_t  *face;        /**< HarfBuzz face. */
    hb_font_t  *font;        /**< HarfBuzz font object for shaping. */
    uint8_t    *data;         /**< Copy of font bytes (kept alive for HarfBuzz). */
    uint32_t    data_size;    /**< Size of @p data in bytes. */
    uint32_t    font_handle;  /**< Corresponding font_bindings handle. */
} TextFont;

/**
 * @brief Text binding state with multi-font support and ICU BiDi.
 */
typedef struct {
    wasm_memory_t *memory;       /**< Guest WASM linear memory. */

    TextFont      *fonts;        /**< Dynamic array of loaded fonts. */
    int            font_count;   /**< Number of loaded fonts. */
    int            font_cap;     /**< Allocated capacity of @p fonts. */
    int            active_font;  /**< Index into fonts[] used by text_shape. */

    uint8_t       *font_data;       /**< Legacy single-font data. */
    uint32_t       font_data_size;  /**< Legacy font data size. */
    hb_blob_t     *hb_blob;         /**< Legacy HarfBuzz blob. */
    hb_face_t     *hb_face;         /**< Legacy HarfBuzz face. */
    hb_font_t     *hb_font;         /**< Legacy HarfBuzz font. */
    uint32_t      *gid_to_guest;    /**< Legacy glyph ID mapping. */
    uint32_t       gid_map_size;    /**< Size of legacy glyph map. */
    int            loaded;          /**< Legacy loaded flag. */

    UBiDi         *ubidi;        /**< Reusable ICU BiDi object. */
    UChar         *u16buf;       /**< Reusable UTF-16 conversion buffer. */
    int32_t        u16cap;       /**< Capacity of @p u16buf. */

    struct FontBindings *font_bindings;  /**< Cross-module font data access. */
} TextBindings;

/**
 * @brief Initialize text bindings.
 * @param[out] b  Bindings to initialize.
 */
void     text_bindings_init(TextBindings *b);

/**
 * @brief Destroy text bindings and release all HarfBuzz/ICU resources.
 * @param[in,out] b  Bindings to destroy.
 */
void     text_bindings_destroy(TextBindings *b);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] b    Bindings.
 * @param[in]     mem  Guest WASM memory.
 */
void     text_bindings_set_memory(TextBindings *b, wasm_memory_t *mem);

/**
 * @brief Connect to font_bindings for cross-module font data access.
 * @param[in,out] b   Text bindings.
 * @param[in]     fb  Font bindings instance.
 */
void     text_bindings_set_font_bindings(TextBindings *b, struct FontBindings *fb);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] b          Bindings.
 * @param[in]     store      WASM store.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t   text_bindings_get_imports(TextBindings *b, wasm_store_t *store,
                                   const char ***out_names,
                                   wasm_func_t ***out_funcs);

#endif /* TEXT_BINDINGS_H */
