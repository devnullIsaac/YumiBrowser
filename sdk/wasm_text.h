/*
    Yumi SDK — Text Shaping / BiDi / Line Break WASM Imports
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

#ifndef WASM_TEXT_H
#define WASM_TEXT_H

/**
 * @file wasm_text.h
 * @brief WASM guest imports for host-side text shaping, bidi, and breaks.
 *
 * The host handles font fallback internally: when the active font
 * lacks a glyph, the host finds a system font automatically.
 * Each shaped glyph includes the font_handle that produced it,
 * so the guest can fetch glyph data from the correct font.
 */

#include <stdint.h>

#define IMPORT __attribute__((import_module("env")))

/** @brief One shaped glyph returned by text_shape(). 24 bytes packed.
 *
 * font_handle identifies which font produced this glyph. The guest
 * uses it to call font_get_glyph() / font_get_glyph_bitmap() for
 * rendering data. If the active font covered the codepoint, this
 * is the active font's handle; otherwise it's a fallback font that
 * the host discovered and loaded automatically.
 */
typedef struct __attribute__((packed)) {
    uint32_t glyph_index;   /**< Font-specific glyph ID (0xFFFFFFFF = missing). */
    float    x_advance;     /**< Horizontal advance in pixels. */
    float    x_offset;      /**< Positioning offset (pixels). */
    float    y_offset;      /**< Positioning offset (pixels). */
    uint32_t cluster;       /**< Index into the input codepoint array. */
    uint32_t font_handle;   /**< Font handle that produced this glyph. */
} text_shaped_glyph_t;

/** @brief One visual run from bidi reordering. 12 bytes packed. */
typedef struct {
    uint32_t offset;    /**< Start index into the logical codepoint array. */
    uint32_t length;    /**< Number of codepoints in this run. */
    uint32_t direction; /**< 0 = LTR, 1 = RTL. */
} text_bidi_run_t;

/**
 * @brief Shape a run of codepoints using host-side HarfBuzz.
 *
 * The host performs font fallback automatically. If the active font
 * (set via text_set_font) lacks a glyph, the host searches system
 * fonts and loads a fallback. Each output glyph includes the
 * font_handle that produced it.
 *
 * @param direction  0 = LTR, 1 = RTL.
 * @return Number of shaped glyphs written.
 */
IMPORT __attribute__((import_name("text_shape")))
int text_shape(const uint32_t *codepoints, int count,
               float font_size, int direction,
               text_shaped_glyph_t *out_glyphs, int max_glyphs);

/**
 * @brief Run ICU's Unicode Bidi Algorithm.
 * @return Low 16 bits: base direction. High 16 bits: run count.
 */
IMPORT __attribute__((import_name("text_bidi")))
int text_bidi(const uint32_t *codepoints, int count,
              uint8_t *out_levels,
              text_bidi_run_t *out_runs, int max_runs);

/** @brief Compute line break opportunities via ICU. */
IMPORT __attribute__((import_name("text_line_breaks")))
void text_line_breaks(const uint32_t *codepoints, int count,
                      uint8_t *out_breaks);

/** @brief Compute grapheme cluster boundaries via ICU. */
IMPORT __attribute__((import_name("text_grapheme_breaks")))
void text_grapheme_breaks(const uint32_t *codepoints, int count,
                          uint8_t *out_flags);

/** @brief Set the active (primary) font for shaping. */
IMPORT __attribute__((import_name("text_set_font")))
int text_set_font(int font_handle);

/**
 * @brief Upload a font to the host for shaping (legacy).
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("text_load_font")))
int text_load_font(const void *font_data, int font_size,
                   const uint32_t *codepoints, int glyph_count);

#undef IMPORT

static inline int text_bidi_base_dir(int rv)   { return rv & 0xFFFF; }
static inline int text_bidi_run_count(int rv)   { return (rv >> 16) & 0xFFFF; }

#endif /* WASM_TEXT_H */
