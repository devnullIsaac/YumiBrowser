/*
    Yumi SDK — Lazy Font Registry with Atlas/GPU Caching
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

#ifndef FONT_FALLBACK_H
#define FONT_FALLBACK_H

/**
 * @file font_fallback.h
 * @brief Lazy font registry — caches atlases and GPU buffers by font_handle.
 *
 * The host does font fallback in text_shape(). Each shaped glyph
 * includes the font_handle that produced it. This registry lazily
 * creates atlas + GPU resources the first time a font_handle appears.
 *
 * Grows dynamically — no cap on number of fonts.
 */

#include <stddef.h>
#include "wasm_font.h"
#include "wasm_gpu.h"
#include "wasm_text.h"

#define FALLBACK_INITIAL_CAP 8

typedef struct {
    gpu_buffer_t glyph_buf;
    gpu_buffer_t seg_buf;
    int          glyph_buf_cap;   /* capacity in bytes */
    int          seg_buf_cap;     /* capacity in bytes */
} fallback_gpu_bufs_t;

typedef struct {
    int            handle;
    font_atlas_t   atlas;
    font_metrics_t metrics;
    fallback_gpu_bufs_t gpu;
    int            gpu_ready;
} font_registry_entry_t;

typedef struct {
    font_registry_entry_t *entries;
    int             count;
    int             cap;
    gpu_device_t    device;

    /* Primary font metrics (from first registered font). */
    float units_per_em;
    float ascender;
    float descender;
    float line_height;
} font_fallback_t;

static inline void fallback_init(font_fallback_t *fb, gpu_device_t dev) {
    __builtin_memset(fb, 0, sizeof(*fb));
    fb->device = dev;
    fb->cap = FALLBACK_INITIAL_CAP;
    fb->entries = (font_registry_entry_t *)__builtin_malloc(
        (unsigned)fb->cap * sizeof(font_registry_entry_t));
    __builtin_memset(fb->entries, 0, (unsigned)fb->cap * sizeof(font_registry_entry_t));
}

static inline void fallback__grow(font_fallback_t *fb) {
    if (fb->count < fb->cap) return;
    int new_cap = fb->cap * 2;
    font_registry_entry_t *ne = (font_registry_entry_t *)__builtin_malloc(
        (unsigned)new_cap * sizeof(font_registry_entry_t));
    __builtin_memcpy(ne, fb->entries, (unsigned)fb->count * sizeof(font_registry_entry_t));
    __builtin_memset(ne + fb->count, 0,
        (unsigned)(new_cap - fb->count) * sizeof(font_registry_entry_t));
    __builtin_free(fb->entries);
    fb->entries = ne;
    fb->cap = new_cap;
}

static inline int fallback__create_entry(font_fallback_t *fb, int handle) {
    fallback__grow(fb);
    int idx = fb->count;
    font_registry_entry_t *e = &fb->entries[idx];
    e->handle = handle;
    font_get_metrics(handle, &e->metrics);
    font_atlas_init(&e->atlas, handle);

    int glyph_cap = FONT_ATLAS_GPU_GLYPH_CAP * FONT_GLYPH_INFO_SIZE;
    int seg_cap   = FONT_ATLAS_GPU_SEGMENT_CAP * FONT_SEGMENT_SIZE;

    e->gpu.glyph_buf = wgpu_create_buffer(
        fb->device, GPU_BUFFER_USAGE_STORAGE | GPU_BUFFER_USAGE_COPY_DST,
        glyph_cap, 0);
    e->gpu.seg_buf = wgpu_create_buffer(
        fb->device, GPU_BUFFER_USAGE_STORAGE | GPU_BUFFER_USAGE_COPY_DST,
        seg_cap, 0);
    e->gpu.glyph_buf_cap = glyph_cap;
    e->gpu.seg_buf_cap   = seg_cap;
    e->gpu_ready = 1;

    fb->count++;
    return idx;
}

static inline int fallback_set_primary(font_fallback_t *fb, int handle) {
    int idx = fallback__create_entry(fb, handle);
    if (idx < 0) return -1;

    font_registry_entry_t *e = &fb->entries[idx];
    if (fb->units_per_em == 0 && e->metrics.units_per_em > 0) {
        fb->units_per_em = e->metrics.units_per_em;
        fb->ascender     = e->metrics.ascender;
        fb->descender    = e->metrics.descender;
        fb->line_height  = e->metrics.line_height;
    }
    return idx;
}

static inline int fallback_find_or_create(font_fallback_t *fb, int handle) {
    for (int i = 0; i < fb->count; i++)
        if (fb->entries[i].handle == handle)
            return i;
    return fallback__create_entry(fb, handle);
}

static inline int fallback_atlas_get(font_fallback_t *fb,
                                     int entry_idx, uint32_t glyph_id) {
    if (entry_idx < 0 || entry_idx >= fb->count) return -1;
    return font_atlas_get(&fb->entries[entry_idx].atlas, glyph_id);
}

static inline void fallback_upload_dirty(font_fallback_t *fb) {
    for (int i = 0; i < fb->count; i++) {
        font_registry_entry_t *e = &fb->entries[i];
        font_atlas_t *a = &e->atlas;
        if (!font_atlas_is_dirty(a) || !e->gpu_ready) continue;

        /* ---- Grow glyph GPU buffer if the atlas outgrew it ---- */
        if (a->glyph_count > 0 && a->glyphs) {
            int need = (int)((uint32_t)a->glyph_count * FONT_GLYPH_INFO_SIZE);
            if (need > e->gpu.glyph_buf_cap) {
                int new_cap = e->gpu.glyph_buf_cap * 2;
                while (new_cap < need) new_cap *= 2;
                if (e->gpu.glyph_buf)
                    wgpu_buffer_destroy(e->gpu.glyph_buf);
                e->gpu.glyph_buf = wgpu_create_buffer(
                    fb->device,
                    GPU_BUFFER_USAGE_STORAGE | GPU_BUFFER_USAGE_COPY_DST,
                    new_cap, 0);
                e->gpu.glyph_buf_cap = new_cap;
            }
            wgpu_buffer_write(e->gpu.glyph_buf, 0, a->glyphs, need);
        }

        /* ---- Grow segment GPU buffer if the atlas outgrew it ---- */
        if (a->segment_count > 0 && a->segment_buf) {
            int need = (int)((uint32_t)a->segment_count * FONT_SEGMENT_SIZE);
            if (need > e->gpu.seg_buf_cap) {
                int new_cap = e->gpu.seg_buf_cap * 2;
                while (new_cap < need) new_cap *= 2;
                if (e->gpu.seg_buf)
                    wgpu_buffer_destroy(e->gpu.seg_buf);
                e->gpu.seg_buf = wgpu_create_buffer(
                    fb->device,
                    GPU_BUFFER_USAGE_STORAGE | GPU_BUFFER_USAGE_COPY_DST,
                    new_cap, 0);
                e->gpu.seg_buf_cap = new_cap;
            }
            wgpu_buffer_write(e->gpu.seg_buf, 0, a->segment_buf, need);
        }

        font_atlas_clear_dirty(a);
    }
}

static inline float fallback_measure_codepoint(
    font_fallback_t *fb, uint32_t codepoint, float font_size)
{
    text_shaped_glyph_t tmp;
    int n = text_shape(&codepoint, 1, font_size, 0, &tmp, 1);
    if (n > 0 && tmp.x_advance > 0.0f) {
        int ei = fallback_find_or_create(fb, (int)tmp.font_handle);
        if (ei >= 0)
            fallback_atlas_get(fb, ei, tmp.glyph_index);
        return tmp.x_advance;
    }
    return font_size * 0.5f;
}

static inline void fallback_destroy(font_fallback_t *fb) {
    for (int i = 0; i < fb->count; i++) {
        font_atlas_destroy(&fb->entries[i].atlas);
        if (fb->entries[i].gpu.glyph_buf)
            wgpu_buffer_destroy(fb->entries[i].gpu.glyph_buf);
        if (fb->entries[i].gpu.seg_buf)
            wgpu_buffer_destroy(fb->entries[i].gpu.seg_buf);
    }
    __builtin_free(fb->entries);
    fb->entries = NULL;
    fb->count = 0;
    fb->cap = 0;
}

#endif /* FONT_FALLBACK_H */
