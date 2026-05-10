/*
    Yumi SDK — Render-Side Line Shaping using Host Text Services
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

#ifndef TEXT_SHAPING_H
#define TEXT_SHAPING_H

/**
 * @file text_shaping.h
 * @brief Render-side line shaping using host text services.
 *
 * Runs BiDi analysis, shapes each visual run via text_shape()
 * (host handles font fallback), and builds render-ready glyph data
 * with atlas references.
 *
 * No guest-side font selection — the host returns font_handle per
 * glyph, and the font registry lazily creates atlas entries.
 */

#include "font_fallback.h"
#include "wasm_text.h"
#include <stddef.h>
#include <stdint.h>

#define SHAPED_LINE_MAX_GLYPHS 256
#define SHAPED_LINE_MAX_RUNS 32

typedef struct {
  uint32_t atlas_index;
  int font_idx; /**< Index into font_fallback_t.entries[] */
  float x;
  float y;
  float advance;
  uint32_t cluster;
  uint32_t glyph_id;
  uint8_t is_bitmap;
} shaped_glyph_t;

typedef struct {
  shaped_glyph_t glyphs[SHAPED_LINE_MAX_GLYPHS];
  int count;
  float width;
  int base_dir;
} shaped_line_t;

/**
 * @brief Shape a line of codepoints for rendering.
 *
 * 1. Run BiDi to get visual runs.
 * 2. Shape each run (host does font fallback).
 * 3. Look up atlas entries via the font registry.
 */
static void shape_line(const uint32_t *codepoints, int count, float font_size,
                       font_fallback_t *fb, shaped_line_t *out) {
  out->count = 0;
  out->width = 0.0f;
  out->base_dir = 0;
  if (count <= 0)
    return;

  /* BiDi analysis */
  uint8_t levels[4096];
  text_bidi_run_t bidi_runs[SHAPED_LINE_MAX_RUNS];
  int bidi_count = count;
  if (bidi_count > 4096)
    bidi_count = 4096;
  int bidi_rv = text_bidi(codepoints, bidi_count, levels, bidi_runs,
                          SHAPED_LINE_MAX_RUNS);
  out->base_dir = text_bidi_base_dir(bidi_rv);
  int num_runs = text_bidi_run_count(bidi_rv);
  if (num_runs <= 0) {
    num_runs = 1;
    bidi_runs[0].offset = 0;
    bidi_runs[0].length = (uint32_t)count;
    bidi_runs[0].direction = 0;
  }

  float cursor_x = 0.0f;
  text_shaped_glyph_t shaped_tmp[SHAPED_LINE_MAX_GLYPHS];

  for (int ri = 0; ri < num_runs && out->count < SHAPED_LINE_MAX_GLYPHS; ri++) {
    uint32_t run_off = bidi_runs[ri].offset;
    uint32_t run_len = bidi_runs[ri].length;
    uint32_t run_dir = bidi_runs[ri].direction;
    if (run_len == 0)
      continue;
    if (run_off + run_len > (uint32_t)count)
      run_len = (uint32_t)count - run_off;

    /* Single shape call per bidi run — host handles font fallback */
    int max_out = SHAPED_LINE_MAX_GLYPHS - out->count;
    if (max_out > SHAPED_LINE_MAX_GLYPHS)
      max_out = SHAPED_LINE_MAX_GLYPHS;
    if (max_out <= 0)
      break;

    int sg_count = text_shape(codepoints + run_off, (int)run_len, font_size,
                              (int)run_dir, shaped_tmp, max_out);

    for (int gi = 0; gi < sg_count && out->count < SHAPED_LINE_MAX_GLYPHS;
         gi++) {
      text_shaped_glyph_t *sg = &shaped_tmp[gi];

      /* Find or create registry entry for this font */
      int entry_idx = fallback_find_or_create(fb, (int)sg->font_handle);
      if (entry_idx < 0)
        entry_idx = 0; /* fall back to primary */

      /* Look up atlas entry */
      int atlas_idx = -1;
      uint32_t glyph_id = sg->glyph_index;
      if (glyph_id != 0xFFFFFFFFu && glyph_id != 0)
        atlas_idx = fallback_atlas_get(fb, entry_idx, glyph_id);

      int is_bmp = 0;
      if (atlas_idx >= 0) {
        is_bmp = font_atlas_is_bitmap(&fb->entries[entry_idx].atlas, atlas_idx);
      } else if (glyph_id != 0xFFFFFFFFu && glyph_id != 0) {
        /* Atlas lookup failed — likely a bitmap-only font (e.g. color emoji)
         * with no vector outlines. Mark as bitmap so the bitmap renderer
         * can rasterize it via font_get_glyph_bitmap. */
        is_bmp = 1;
      }

      shaped_glyph_t *out_g = &out->glyphs[out->count];
      out_g->atlas_index = (atlas_idx >= 0) ? (uint32_t)atlas_idx : 0xFFFFFFFFu;
      out_g->font_idx = entry_idx;
      out_g->x = cursor_x + sg->x_offset;
      out_g->y = sg->y_offset;
      out_g->advance = sg->x_advance;
      out_g->cluster = run_off + sg->cluster;
      out_g->glyph_id = glyph_id;
      out_g->is_bitmap = (uint8_t)is_bmp;
      out->count++;
      cursor_x += sg->x_advance;
    }
  }
  out->width = cursor_x;
}

#endif /* TEXT_SHAPING_H */
