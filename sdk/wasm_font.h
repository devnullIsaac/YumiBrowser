/*
    Yumi SDK — Font Loading and Glyph Query WASM Imports
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

#ifndef WASM_FONT_H
#define WASM_FONT_H

/**
 * @file wasm_font.h
 * @brief WebAssembly guest imports for host-side font loading and glyph querying.
 *
 * @details
 * This header declares the host-imported functions that allow WASM guest modules
 * to load TrueType/OpenType fonts, query glyph metrics, and retrieve vector or
 * bitmap glyph data. All functions map 1:1 to implementations in `font_bindings.c`
 * on the host side.
 *
 * ## Font Handles
 * Fonts are referenced by opaque integer handles returned by font_load() or
 * font_load_system(). Handle 0 is reserved and always invalid. Guests must call
 * font_unload() when a font is no longer needed to release host resources.
 *
 * ## Glyph Data Model
 * Each glyph consists of:
 *   - **Metrics** (font_glyph_info_t): Advance width, bounding box, bearings, etc.
 *   - **Segments**: Cubic Bezier curves describing the glyph outline.
 *
 * The segment data is an array of packed records (48 bytes each) stored in a
 * flat buffer. Guests typically cache this data in a font_atlas_t structure.
 *
 * ## Typical Workflow
 * @code
 *   int fh = font_load_system("Arial", 5);
 *   font_metrics_t m;
 *   font_get_metrics(fh, &m);
 *   int gid = font_get_codepoint_glyph_id(fh, 'A');
 *   font_glyph_info_t info;
 *   uint8_t segs[FONT_SEGMENT_SIZE * 64];
 *   font_get_glyph(fh, gid, &info, segs, 64);
 * @endcode
 */

#include <stdint.h>

/** @cond INTERNAL */
#define IMPORT __attribute__((import_module("env")))
/** @endcond */

/* ================================================================== */
/*  Font loading & lifecycle                                           */
/* ================================================================== */

/**
 * @brief Load a font from raw bytes in WASM memory.
 *
 * @details
 * Parses a TrueType or OpenType font file already resident in WASM linear
 * memory. The host creates an internal font object and returns a handle.
 *
 * @param[in] data_ptr  Pointer to the font file bytes in WASM memory.
 * @param[in] data_len  Byte length of the font data.
 * @return A positive font handle on success, or 0 on failure (invalid format,
 *         corrupted data, or host resource exhaustion).
 */
IMPORT __attribute__((import_name("font_load")))
int font_load(const void *data_ptr, int data_len);

/**
 * @brief Load a system font by name.
 *
 * @details
 * Searches the host's system font directories for a font matching the given
 * name (e.g. "Arial", "Times New Roman"). The search is case-insensitive on
 * most platforms. If multiple weights exist, a regular weight is preferred.
 *
 * @param[in] name_ptr  Pointer to the font family name in WASM memory.
 * @param[in] name_len  Byte length of the name string.
 * @return A positive font handle on success, or 0 if the font is not found.
 */
IMPORT __attribute__((import_name("font_load_system")))
int font_load_system(const char *name_ptr, int name_len);

/**
 * @brief Unload a font and release host resources.
 *
 * @details
 * Destroys the internal font object, frees cached glyph data, and releases
 * any file handles. After this call the handle is invalid and must not be used.
 *
 * @param[in] font_handle  Valid font handle returned by font_load() or
 *                         font_load_system().
 */
IMPORT __attribute__((import_name("font_unload")))
void font_unload(int font_handle);

/* ================================================================== */
/*  Metrics & glyph enumeration                                        */
/* ================================================================== */

/**
 * @brief Retrieve font-wide metrics.
 *
 * @details
 * Fills a font_metrics_t structure with global font measurements:
 * units-per-EM, ascender, descender, and line height. These values are in
 * font design units and must be scaled by the desired pixel size / units_per_em.
 *
 * @param[in]  font_handle  Valid font handle.
 * @param[out] out_ptr      Pointer to a font_metrics_t in WASM memory.
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("font_get_metrics")))
int font_get_metrics(int font_handle, void *out_ptr);

/**
 * @brief Get the total number of glyphs in the font.
 *
 * @details
 * Returns the count of glyphs defined in the font's cmap and glyf tables.
 * This is the upper bound for glyph ID values.
 *
 * @param[in] font_handle  Valid font handle.
 * @return Number of glyphs, or 0 on error.
 */
IMPORT __attribute__((import_name("font_glyph_count")))
int font_glyph_count(int font_handle);

/**
 * @brief Check whether the font contains a specific Unicode codepoint.
 *
 * @details
 * Looks up the codepoint in the font's character map (cmap). This is a fast
 * operation that does not load glyph data.
 *
 * @param[in] font_handle  Valid font handle.
 * @param[in] codepoint    Unicode scalar value to check.
 * @return 1 if the font has a glyph for this codepoint, 0 otherwise.
 */
IMPORT __attribute__((import_name("font_has_glyph")))
int font_has_glyph(int font_handle, int codepoint);

/**
 * @brief Map a Unicode codepoint to a font glyph ID.
 *
 * @details
 * Returns the glyph index (GID) used internally by the font for the given
 * Unicode codepoint. If the codepoint is not present, returns 0 (the .notdef
 * glyph).
 *
 * @param[in] font_handle  Valid font handle.
 * @param[in] codepoint    Unicode scalar value.
 * @return Glyph ID, or 0 if not present.
 */
IMPORT __attribute__((import_name("font_get_codepoint_glyph_id")))
int font_get_codepoint_glyph_id(int font_handle, int codepoint);

/* ================================================================== */
/*  Glyph data retrieval                                               */
/* ================================================================== */

/**
 * @brief Retrieve vector outline data for a glyph.
 *
 * @details
 * Fills @p out_info_ptr with a font_glyph_info_t describing the glyph's
 * metrics, and writes up to @p max_segs segment records into @p out_segs_ptr.
 * Each segment is FONT_SEGMENT_SIZE bytes. The actual number of segments
 * written is stored in the returned font_glyph_info_t::segment_count field.
 *
 * @param[in]  font_handle   Valid font handle.
 * @param[in]  glyph_id      Glyph ID to query.
 * @param[out] out_info_ptr  Pointer to a font_glyph_info_t in WASM memory.
 * @param[out] out_segs_ptr  Pointer to a buffer for segment data.
 * @param[in]  max_segs      Maximum number of segments to write.
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("font_get_glyph")))
int font_get_glyph(int font_handle, int glyph_id,
                   void *out_info_ptr, void *out_segs_ptr, int max_segs);

/**
 * @brief Rasterize a glyph to a bitmap.
 *
 * @details
 * Renders the glyph at the requested pixel size into an 8-bit grayscale bitmap.
 * The bitmap dimensions and baseline offset are written to @p out_info, and
 * the pixel data is written to @p out_pixels.
 *
 * @param[in]  font_handle  Valid font handle.
 * @param[in]  glyph_id     Glyph ID to rasterize.
 * @param[in]  pixel_size   Requested pixel height (e.g. 16, 24, 48).
 * @param[out] out_info     Pointer to a bitmap info struct in WASM memory.
 * @param[out] out_pixels   Pointer to a buffer for 8-bit grayscale pixels.
 * @param[in]  max_bytes    Capacity of @p out_pixels in bytes.
 * @return 1 on success, 0 on failure or if @p max_bytes is too small.
 */
IMPORT __attribute__((import_name("font_get_glyph_bitmap")))
int font_get_glyph_bitmap(int font_handle, int glyph_id, int pixel_size,
                          void *out_info, void *out_pixels, int max_bytes);

/** @cond INTERNAL */
#undef IMPORT
/** @endcond */

/* ================================================================== */
/*  Size constants                                                     */
/* ================================================================== */

/** @brief Byte size of a font_glyph_info_t structure (packed). */
#define FONT_GLYPH_INFO_SIZE  48

/** @brief Byte size of one glyph segment record (packed). */
#define FONT_SEGMENT_SIZE     48

/* ================================================================== */
/*  Capacity constants                                                 */
/* ================================================================== */

/**
 * @brief Maximum number of glyphs the CPU-side atlas can hold.
 * @details Override before including this header if your application needs more.
 */
#ifndef FONT_ATLAS_MAX_GLYPHS
#define FONT_ATLAS_MAX_GLYPHS 4096
#endif

/**
 * @brief Maximum segments per glyph for CPU-side buffers.
 * @details Override before including this header if your glyphs are complex.
 */
#ifndef FONT_ATLAS_MAX_SEGS_PER_GLYPH
#define FONT_ATLAS_MAX_SEGS_PER_GLYPH 256
#endif

/**
 * @brief Maximum glyph capacity for GPU-side storage buffers.
 * @details GPU memory is separate from WASM heap, so this can be large.
 */
#define FONT_ATLAS_GPU_GLYPH_CAP    4096

/**
 * @brief Maximum segment capacity for GPU-side storage buffers.
 * @details GPU memory is separate from WASM heap, so this can be large.
 */
#define FONT_ATLAS_GPU_SEGMENT_CAP  8192

/**
 * @brief Initial CPU heap allocation size for glyph array.
 * @details The array grows on demand; this keeps initial memory small.
 */
#define FONT_ATLAS_INITIAL_GLYPHS     32

/**
 * @brief Initial CPU heap allocation size for segment buffer.
 * @details The buffer grows on demand; this keeps initial memory small.
 */
#define FONT_ATLAS_INITIAL_SEGMENTS   64

typedef struct {
    float units_per_em;
    float ascender;
    float descender;
    float line_height;
} font_metrics_t;

typedef struct {
    uint32_t codepoint;
    float    advance_x;
    float    bearing_x;
    float    bearing_y;
    float    bbox_min_x;
    float    bbox_min_y;
    float    bbox_max_x;
    float    bbox_max_y;
    uint32_t segment_offset;
    uint32_t segment_count;
    uint32_t contour_count;
    float    total_arc_length;
} font_glyph_info_t;

typedef struct {
    int font_handle;

    int     *gid_to_atlas;
    uint32_t gid_map_size;

    font_glyph_info_t *glyphs;
    int                glyph_count;
    int                glyph_cap;

    uint8_t  *is_bitmap;

    uint8_t  *segment_buf;
    uint32_t  segment_buf_cap;
    uint32_t  segment_count;

    uint8_t  *seg_scratch;

    int dirty;
} font_atlas_t;

static inline void font_atlas_init(font_atlas_t *a, int font_handle) {
    a->font_handle   = font_handle;
    a->glyph_count   = 0;
    a->segment_count = 0;
    a->dirty         = 0;

    a->glyph_cap = FONT_ATLAS_INITIAL_GLYPHS;
    a->glyphs    = (font_glyph_info_t *)__builtin_malloc(
                       (uint32_t)a->glyph_cap * sizeof(font_glyph_info_t));
    a->is_bitmap  = (uint8_t *)__builtin_malloc((uint32_t)a->glyph_cap);
    __builtin_memset(a->is_bitmap, 0, (uint32_t)a->glyph_cap);

    a->segment_buf     = 0;
    a->segment_buf_cap = 0;
    a->seg_scratch     = 0;

    int gc = font_glyph_count(font_handle);
    if (gc <= 0) gc = 64;
    a->gid_map_size = (uint32_t)gc + 1;
    a->gid_to_atlas = (int *)__builtin_malloc(a->gid_map_size * sizeof(int));
    for (uint32_t i = 0; i < a->gid_map_size; i++)
        a->gid_to_atlas[i] = -1;
}

static inline void font_atlas_destroy(font_atlas_t *a) {
    __builtin_free(a->gid_to_atlas);
    __builtin_free(a->segment_buf);
    __builtin_free(a->seg_scratch);
    __builtin_free(a->glyphs);
    __builtin_free(a->is_bitmap);
    a->gid_to_atlas    = 0;
    a->segment_buf     = 0;
    a->seg_scratch     = 0;
    a->glyphs          = 0;
    a->is_bitmap       = 0;
    a->gid_map_size    = 0;
    a->segment_buf_cap = 0;
    a->glyph_cap       = 0;
}

static inline void font_atlas__grow_glyphs(font_atlas_t *a) {
    if (a->glyph_count < a->glyph_cap) return;
    int new_cap = a->glyph_cap * 2;
    if (new_cap > FONT_ATLAS_MAX_GLYPHS) new_cap = FONT_ATLAS_MAX_GLYPHS;

    font_glyph_info_t *ng = (font_glyph_info_t *)__builtin_malloc(
        (uint32_t)new_cap * sizeof(font_glyph_info_t));
    __builtin_memcpy(ng, a->glyphs,
                     (uint32_t)a->glyph_count * sizeof(font_glyph_info_t));
    __builtin_free(a->glyphs);
    a->glyphs = ng;

    uint8_t *nb = (uint8_t *)__builtin_malloc((uint32_t)new_cap);
    __builtin_memcpy(nb, a->is_bitmap, (uint32_t)a->glyph_count);
    __builtin_memset(nb + a->glyph_count, 0,
                     (uint32_t)(new_cap - a->glyph_count));
    __builtin_free(a->is_bitmap);
    a->is_bitmap = nb;

    a->glyph_cap = new_cap;
}

static inline void font_atlas__ensure_scratch(font_atlas_t *a) {
    if (a->seg_scratch) return;
    a->seg_scratch = (uint8_t *)__builtin_malloc(
        FONT_ATLAS_MAX_SEGS_PER_GLYPH * FONT_SEGMENT_SIZE);
}

static inline void font_atlas__grow_segments(font_atlas_t *a, uint32_t need) {
    uint32_t required = a->segment_count + need;
    if (a->segment_buf && required <= a->segment_buf_cap) return;

    uint32_t new_cap = a->segment_buf_cap;
    if (new_cap == 0) new_cap = FONT_ATLAS_INITIAL_SEGMENTS;
    while (new_cap < required) new_cap *= 2;

    uint8_t *nb = (uint8_t *)__builtin_malloc(new_cap * FONT_SEGMENT_SIZE);
    if (a->segment_buf && a->segment_count > 0) {
        __builtin_memcpy(nb, a->segment_buf,
                         a->segment_count * FONT_SEGMENT_SIZE);
    }
    __builtin_free(a->segment_buf);
    a->segment_buf     = nb;
    a->segment_buf_cap = new_cap;
}

static inline int font_atlas_get(font_atlas_t *a, uint32_t glyph_id) {
    if (glyph_id < a->gid_map_size && a->gid_to_atlas[glyph_id] >= 0)
        return a->gid_to_atlas[glyph_id];

    if (a->glyph_count >= FONT_ATLAS_MAX_GLYPHS)
        return -1;

    font_atlas__ensure_scratch(a);

    font_glyph_info_t info;
    int seg_count = font_get_glyph(a->font_handle, (int)glyph_id,
                                   &info, a->seg_scratch,
                                   FONT_ATLAS_MAX_SEGS_PER_GLYPH);

    font_atlas__grow_glyphs(a);

    int atlas_idx = a->glyph_count;

    info.segment_offset = a->segment_count;
    info.segment_count  = (uint32_t)(seg_count > 0 ? seg_count : 0);

    a->glyphs[atlas_idx] = info;
    a->is_bitmap[atlas_idx] = (seg_count == 0 && info.advance_x > 0.0f) ? 1 : 0;

    if (seg_count > 0) {
        font_atlas__grow_segments(a, (uint32_t)seg_count);
        __builtin_memcpy(
            a->segment_buf + a->segment_count * FONT_SEGMENT_SIZE,
            a->seg_scratch,
            (uint32_t)seg_count * FONT_SEGMENT_SIZE);
        a->segment_count += (uint32_t)seg_count;
    }

    a->glyph_count++;

    if (glyph_id >= a->gid_map_size) {
        uint32_t new_size = glyph_id + 256;
        int *new_map = (int *)__builtin_malloc(new_size * sizeof(int));
        __builtin_memcpy(new_map, a->gid_to_atlas,
                         a->gid_map_size * sizeof(int));
        for (uint32_t i = a->gid_map_size; i < new_size; i++)
            new_map[i] = -1;
        __builtin_free(a->gid_to_atlas);
        a->gid_to_atlas = new_map;
        a->gid_map_size = new_size;
    }

    a->gid_to_atlas[glyph_id] = atlas_idx;
    a->dirty = 1;

    return atlas_idx;
}

static inline int font_atlas_is_bitmap(const font_atlas_t *a, int atlas_idx) {
    if (atlas_idx < 0 || atlas_idx >= a->glyph_count || !a->is_bitmap) return 0;
    return a->is_bitmap[atlas_idx];
}

static inline int font_atlas_is_dirty(const font_atlas_t *a) {
    return a->dirty;
}

static inline void font_atlas_clear_dirty(font_atlas_t *a) {
    a->dirty = 0;
}

#endif /* WASM_FONT_H */
