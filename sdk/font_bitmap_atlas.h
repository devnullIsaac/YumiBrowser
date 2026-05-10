/*
    Yumi SDK — CPU-Side Glyph Bitmap Atlas with Lazy GPU Upload
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
 * @file font_bitmap_atlas.h
 * @brief CPU-side glyph bitmap atlas with lazy GPU upload.
 *
 * @details
 * This header provides a self-contained, header-only bitmap atlas for caching
 * rasterized glyph bitmaps. It works alongside `wasm_font.h` to store grayscale
 * glyph images in a contiguous CPU pixel buffer, then uploads the entire atlas
 * to a GPU storage buffer when needed for shader-based text rendering.
 *
 * ## Design
 *   - **Lazy rasterization**: Glyphs are rasterized on demand via `font_get_glyph_bitmap()`.
 *   - **Linear pixel storage**: All glyph bitmaps are packed into a single `uint32_t*`
 *     array (RGBA8 format, grayscale in alpha channel).
 *   - **GPU upload**: When `dirty` is set, the caller should upload `pixels` to
 *     `pixel_buf` and clear the dirty flag.
 *   - **Lookup by cache key**: Entries are keyed by `(font_idx, glyph_id, request_px)`.
 *
 * ## Thread Safety
 * This structure is **not** thread-safe. All operations should happen on the
 * same thread (typically the main render thread).
 */

#ifndef FONT_BITMAP_ATLAS_H
#define FONT_BITMAP_ATLAS_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "wasm_gpu.h"
#include "wasm_font.h"
#include "wasm_log.h"

/** @brief Initial capacity of the pixel buffer, in pixels. */
#define BITMAP_ATLAS_INITIAL_PIXELS   (4 * 1024)

/** @brief Maximum dimension of any cached glyph bitmap (width or height). */
#define BITMAP_GLYPH_MAX_SIZE         136

/** @brief Byte size of the scratch info buffer for `font_get_glyph_bitmap()`. */
#define BITMAP_SCRATCH_INFO_SIZE      24

/** @brief Byte size of the scratch pixel buffer for rasterization. */
#define BITMAP_SCRATCH_PIXEL_SIZE     (BITMAP_GLYPH_MAX_SIZE * BITMAP_GLYPH_MAX_SIZE * 4)

/**
 * @brief One cached glyph bitmap entry in the atlas.
 *
 * @details
 * Each entry records where a glyph's pixels live in the atlas pixel buffer,
 * along with metrics needed for positioning during text layout.
 */
typedef struct {
    int      font_idx;       /**< Index into the application's font array. */
    int      font_handle;    /**< Host font handle used for rasterization. */
    uint32_t glyph_id;       /**< Font-specific glyph identifier. */
    uint32_t pixel_offset;   /**< Offset in pixels into the atlas pixel buffer. */
    uint32_t width;          /**< Bitmap width in pixels. */
    uint32_t height;         /**< Bitmap height in pixels. */
    float    advance_x;      /**< Horizontal advance in pixels. */
    float    bearing_x;      /**< Left-side bearing in pixels. */
    float    bearing_y;      /**< Top-side bearing in pixels. */
    int      pixel_size;     /**< Actual rasterization size used for rendering math. */
    int      request_px;     /**< Requested pixel size (used as cache key). */
} bitmap_atlas_entry_t;

/**
 * @brief Bitmap glyph atlas — CPU-side cache with GPU upload support.
 *
 * @details
 * The atlas grows its pixel and entry buffers dynamically on demand.
 * When a new glyph is loaded, the `dirty` flag is set to indicate that
 * the GPU buffer needs to be re-uploaded before the next render pass.
 */
typedef struct {
    uint32_t             *pixels;         /**< Flat RGBA8 pixel buffer. */
    int                   pixel_count;    /**< Number of pixels currently used. */
    int                   pixel_cap;      /**< Capacity of pixels buffer. */
    bitmap_atlas_entry_t *entries;        /**< Array of cached glyph entries. */
    int                   entry_count;    /**< Number of entries. */
    int                   entry_cap;      /**< Capacity of entries array. */
    uint8_t              *scratch_info;   /**< Temp buffer for font_get_glyph_bitmap info. */
    uint8_t              *scratch_pixels; /**< Temp buffer for font_get_glyph_bitmap pixels. */
    gpu_buffer_t          pixel_buf;      /**< GPU buffer handle (uploaded by caller). */
    int                   pixel_buf_size; /**< Size of last GPU upload, in bytes. */
    int                   dirty;          /**< Nonzero if GPU upload is needed. */
} bitmap_atlas_t;

static inline void bitmap_atlas_init(bitmap_atlas_t *a) {
    a->pixel_cap   = BITMAP_ATLAS_INITIAL_PIXELS;
    a->pixel_count = 0;
    a->pixels      = (uint32_t *)malloc((uint32_t)a->pixel_cap * sizeof(uint32_t));
    a->entry_cap   = 32;
    a->entry_count = 0;
    a->entries     = (bitmap_atlas_entry_t *)malloc((uint32_t)a->entry_cap * sizeof(bitmap_atlas_entry_t));
    a->scratch_info   = (uint8_t *)malloc(BITMAP_SCRATCH_INFO_SIZE);
    a->scratch_pixels = (uint8_t *)malloc(BITMAP_SCRATCH_PIXEL_SIZE);
    a->pixel_buf      = 0;
    a->pixel_buf_size = 0;
    a->dirty          = 0;
}

static inline void bitmap_atlas_destroy(bitmap_atlas_t *a) {
    free(a->pixels);
    free(a->entries);
    free(a->scratch_info);
    free(a->scratch_pixels);
    a->pixels = 0; a->entries = 0; a->scratch_info = 0; a->scratch_pixels = 0;
    a->pixel_count = 0; a->entry_count = 0;
}

static inline int bitmap_atlas_find(const bitmap_atlas_t *a,
                                    int font_idx, uint32_t glyph_id,
                                    int pixel_size) {
    for (int i = 0; i < a->entry_count; i++)
        if (a->entries[i].font_idx == font_idx &&
            a->entries[i].glyph_id == glyph_id &&
            a->entries[i].request_px == pixel_size)   /* FIX: match on requested size */
            return i;
    return -1;
}

static inline int bitmap_atlas_load(bitmap_atlas_t *a, int font_handle,
                                    uint32_t glyph_id, int font_idx,
                                    int pixel_size) {
    int bytes = font_get_glyph_bitmap(font_handle, (int)glyph_id, pixel_size,
                                      a->scratch_info, a->scratch_pixels,
                                      BITMAP_SCRATCH_PIXEL_SIZE);
    if (bytes <= 0) return -1;

    uint32_t w, h, ps; float adv, bx, by;
    w   = *(uint32_t*)(a->scratch_info);          // offset 0
    h   = *(uint32_t*)(a->scratch_info + 4);      // offset 4
    adv = *(float*)(a->scratch_info + 8);          // offset 8
    bx  = *(float*)(a->scratch_info + 12);         // offset 12
    by  = *(float*)(a->scratch_info + 16);         // offset 16
    ps  = *(uint32_t*)(a->scratch_info + 20);      // offset 20
    uint32_t pixel_count = w * h;
    if (pixel_count == 0) return -1;

    if (a->pixel_count + (int)pixel_count > a->pixel_cap) {
        int new_cap = a->pixel_cap * 2;
        while (a->pixel_count + (int)pixel_count > new_cap) new_cap *= 2;
        a->pixels = realloc(a->pixels, new_cap * sizeof(uint32_t));
        a->pixel_cap = new_cap;
    }
    uint32_t offset = (uint32_t)a->pixel_count;
    memcpy(a->pixels + offset, a->scratch_pixels, pixel_count * sizeof(uint32_t));
    a->pixel_count += (int)pixel_count;

    if (a->entry_count >= a->entry_cap) {
        int new_cap = a->entry_cap * 2;
        bitmap_atlas_entry_t *ne = (bitmap_atlas_entry_t *)malloc(
            (uint32_t)new_cap * sizeof(bitmap_atlas_entry_t));
        memcpy(ne, a->entries, (uint32_t)a->entry_count * sizeof(bitmap_atlas_entry_t));
        free(a->entries);
        a->entries = ne; a->entry_cap = new_cap;
    }

    int idx = a->entry_count;
    bitmap_atlas_entry_t *e = &a->entries[idx];
    e->font_idx = font_idx; e->font_handle = font_handle; e->glyph_id = glyph_id;
    e->pixel_offset = offset; e->width = w; e->height = h;
    e->advance_x = adv; e->bearing_x = bx; e->bearing_y = by;
    e->pixel_size = (int)ps;       /* actual size — used for rendering math */
    e->request_px = pixel_size;    /* FIX: store requested size for cache lookup */
    a->entry_count++;
    a->dirty = 1;
    return idx;
}

static inline int bitmap_atlas_get(bitmap_atlas_t *a, int font_handle,
                                   uint32_t glyph_id, int font_idx,
                                   int pixel_size) {
    int idx = bitmap_atlas_find(a, font_idx, glyph_id, pixel_size);
    if (idx >= 0) return idx;
    return bitmap_atlas_load(a, font_handle, glyph_id, font_idx, pixel_size);
}

static inline void bitmap_atlas_upload(bitmap_atlas_t *a) {
    if (!a->dirty || !a->pixel_buf || a->pixel_count == 0) return;
    int byte_size = a->pixel_count * (int)sizeof(uint32_t);
    if (byte_size <= a->pixel_buf_size)
        wgpu_buffer_write(a->pixel_buf, 0, a->pixels, byte_size);
    a->dirty = 0;
}

#endif /* FONT_BITMAP_ATLAS_H */
