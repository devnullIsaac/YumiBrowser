/*
 * text_bindings.c - Implementation of the HarfBuzz + ICU text-shaping WASM bindings: BiDi, line breaking, multi-font selection, glyph buffer output.
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

#include "text_bindings.h"
#include "font_bindings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <harfbuzz/hb.h>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>

#define B ((TextBindings *)env)

#define TEXT_MAX_FONT_SIZE     (16 * 1024 * 1024)
#define TEXT_MAX_CODEPOINTS    (1024 * 1024)
#define TEXT_MAX_GLYPHS_OUT    (256 * 1024)
#define TEXT_MAX_BIDI_RUNS_OUT 4096
#define TEXT_GLYPH_OUT_SIZE    24
#define TEXT_INITIAL_FONT_CAP  8

/* ================================================================== */
/*  Shaping-layer resolved-font cache                                  */
/*                                                                     */
/*  Caches (codepoint, active_font_handle) → resolved font index.      */
/*  This sits above the fontconfig cp_cache in font_bindings.c and     */
/*  avoids the cross-module call entirely for previously seen           */
/*  codepoints.  Open-addressing, 32768 slots, Fibonacci hash.         */
/* ================================================================== */

#define RESOLVE_CACHE_SIZE  32768u
#define RESOLVE_CACHE_MASK  (RESOLVE_CACHE_SIZE - 1u)
#define RESOLVE_CACHE_MAX_PROBE 32

typedef struct {
    uint32_t codepoint;
    uint32_t active_handle;  /* primary font handle when this was resolved */
    int      resolved_idx;   /* index into TextBindings.fonts[] */
    uint8_t  occupied;
} resolve_cache_entry_t;

static resolve_cache_entry_t g_resolve_cache[RESOLVE_CACHE_SIZE];

static void resolve_cache_clear(void) {
    memset(g_resolve_cache, 0, sizeof(g_resolve_cache));
}

static uint32_t resolve_cache_hash(uint32_t cp, uint32_t handle) {
    /* Mix codepoint and handle so different primary fonts don't collide */
    uint32_t h = cp * 2654435761u;
    h ^= handle * 2246822519u;
    return h & RESOLVE_CACHE_MASK;
}

static resolve_cache_entry_t *resolve_cache_find(uint32_t cp, uint32_t active_handle) {
    uint32_t idx = resolve_cache_hash(cp, active_handle);
    for (int probe = 0; probe < RESOLVE_CACHE_MAX_PROBE; probe++) {
        uint32_t i = (idx + (uint32_t)probe) & RESOLVE_CACHE_MASK;
        if (!g_resolve_cache[i].occupied)
            return NULL;
        if (g_resolve_cache[i].codepoint == cp &&
            g_resolve_cache[i].active_handle == active_handle)
            return &g_resolve_cache[i];
    }
    return NULL;
}

static void resolve_cache_insert(uint32_t cp, uint32_t active_handle, int resolved_idx) {
    uint32_t idx = resolve_cache_hash(cp, active_handle);
    uint32_t insert_at = idx;
    for (int probe = 0; probe < RESOLVE_CACHE_MAX_PROBE; probe++) {
        uint32_t i = (idx + (uint32_t)probe) & RESOLVE_CACHE_MASK;
        if (!g_resolve_cache[i].occupied ||
            (g_resolve_cache[i].codepoint == cp &&
             g_resolve_cache[i].active_handle == active_handle)) {
            insert_at = i;
            goto do_insert;
        }
    }
do_insert:
    g_resolve_cache[insert_at].codepoint     = cp;
    g_resolve_cache[insert_at].active_handle  = active_handle;
    g_resolve_cache[insert_at].resolved_idx   = resolved_idx;
    g_resolve_cache[insert_at].occupied       = 1;
}

/* ================================================================== */
/*  Internal helpers                                                   */
/* ================================================================== */

static int validate_font_magic(const uint8_t *data, int size) {
    if (size < 12) return 0;
    uint8_t b0=data[0],b1=data[1],b2=data[2],b3=data[3];
    if (b0==0x00&&b1==0x01&&b2==0x00&&b3==0x00) return 1;
    if (b0==0x4F&&b1==0x54&&b2==0x54&&b3==0x4F) return 1;
    if (b0==0x74&&b1==0x74&&b2==0x63&&b3==0x66) return 1;
    return 0;
}

static uint8_t *wasm_mem_base(TextBindings *b) {
    return b->memory ? (uint8_t *)wasm_memory_data(b->memory) : NULL;
}
static size_t wasm_mem_size(TextBindings *b) {
    return b->memory ? wasm_memory_data_size(b->memory) : 0;
}

#define ARG_I32(n) (args->data[(n)].of.i32)
#define ARG_F32(n) (args->data[(n)].of.f32)
#define RET_I32(v) do { res->data[0]=(wasm_val_t){.kind=WASM_I32,.of.i32=(v)}; } while(0)

static void ensure_u16(TextBindings *b, int32_t need) {
    if (b->u16cap >= need) return;
    b->u16cap = need + 256;
    b->u16buf = (UChar *)realloc(b->u16buf, b->u16cap * sizeof(UChar));
}

static int32_t utf32_to_u16(TextBindings *b, const uint32_t *cp, int32_t count, int32_t *cp_to_u16) {
    ensure_u16(b, count * 2 + 1);
    int32_t u16len = 0;
    for (int32_t i = 0; i < count; i++) {
        if (cp_to_u16) cp_to_u16[i] = u16len;
        uint32_t c = cp[i];
        if (c <= 0xFFFF) {
            b->u16buf[u16len++] = (UChar)c;
        } else if (c <= 0x10FFFF) {
            c -= 0x10000;
            b->u16buf[u16len++] = (UChar)(0xD800 + (c >> 10));
            b->u16buf[u16len++] = (UChar)(0xDC00 + (c & 0x3FF));
        } else {
            b->u16buf[u16len++] = 0xFFFD;
        }
    }
    b->u16buf[u16len] = 0;
    return u16len;
}

static void build_u16_to_cp(const int32_t *cp_to_u16, int32_t cp_count,
                            int32_t u16len, int32_t *u16_to_cp) {
    int32_t ci = 0;
    for (int32_t i = 0; i < u16len; i++) {
        while (ci + 1 < cp_count && cp_to_u16[ci + 1] <= i) ci++;
        u16_to_cp[i] = ci;
    }
}

static int text_find_font_for_handle(TextBindings *b, uint32_t font_handle) {
    for (int i = 0; i < b->font_count; i++)
        if (b->fonts[i].font_handle == font_handle) return i;
    return -1;
}

static void text_ensure_font_cap(TextBindings *b) {
    if (b->font_count < b->font_cap) return;
    int new_cap = b->font_cap ? b->font_cap * 2 : TEXT_INITIAL_FONT_CAP;
    b->fonts = (TextFont *)realloc(b->fonts, new_cap * sizeof(TextFont));
    memset(b->fonts + b->font_cap, 0, (new_cap - b->font_cap) * sizeof(TextFont));
    b->font_cap = new_cap;
}

static int text_add_font_from_data(TextBindings *b, const uint8_t *data,
                                   uint32_t size, uint32_t font_handle) {
    text_ensure_font_cap(b);
    int idx = b->font_count;
    TextFont *tf = &b->fonts[idx];
    tf->data = malloc(size);
    memcpy(tf->data, data, size);
    tf->data_size = size;
    tf->font_handle = font_handle;
    tf->blob = hb_blob_create((const char *)tf->data, size,
                               HB_MEMORY_MODE_READONLY, NULL, NULL);
    if (!tf->blob || hb_blob_get_length(tf->blob) == 0) {
        free(tf->data);
        memset(tf, 0, sizeof(*tf));
        return -1;
    }
    tf->face = hb_face_create(tf->blob, 0);
    tf->font = hb_font_create(tf->face);
    b->font_count++;
    printf("[text] Font %d loaded for handle %u (%u UPM)\n",
           idx, font_handle, hb_face_get_upem(tf->face));
    return idx;
}

static int text_ensure_hb_font(TextBindings *b, uint32_t font_handle) {
    if (!font_handle) return -1;
    int idx = text_find_font_for_handle(b, font_handle);
    if (idx >= 0) return idx;
    if (!b->font_bindings) return -1;
    uint32_t data_size = 0;
    const uint8_t *data = font_bindings_get_data(b->font_bindings, font_handle, &data_size);
    if (!data || data_size == 0) return -1;
    return text_add_font_from_data(b, data, data_size, font_handle);
}

/* ================================================================== */
/*  Font fallback — fontconfig-authoritative                           */
/*                                                                     */
/*  Fontconfig knows which font is the canonical match for a given     */
/*  codepoint's script. We trust its answer over a simple cmap check   */
/*  because a font may have cmap entries for a script it can't shape   */
/*  (e.g. NotoSans-Regular has Arabic cmap but no Arabic GSUB).        */
/* ================================================================== */

/**
 * @brief Resolve the best HarfBuzz font index for a codepoint.
 *
 * Strategy:
 *  0. Check the shaping-layer resolve cache first (pure O(1) lookup).
 *  1. If the active font has the glyph, use it — this respects explicit
 *     font-variant selection (bold, italic, etc.) made by the guest via
 *     text_set_font().  Do NOT ask fontconfig in this case; fontconfig
 *     returns the canonical face for a script (usually Regular) and would
 *     silently override the bold/italic variant the guest requested.
 *  2. Only if the active font lacks the glyph (returns glyph ID 0) do we
 *     ask fontconfig for a fallback that actually covers the codepoint.
 *  3. If fontconfig has no answer, fall back to the active font anyway.
 */
static int text_resolve_font_for_codepoint(TextBindings *b, uint32_t codepoint,
                                           int active_idx) {
    uint32_t active_handle = b->fonts[active_idx].font_handle;

    /* Fast path: check shaping-layer cache */
    resolve_cache_entry_t *cached = resolve_cache_find(codepoint, active_handle);
    if (cached)
        return cached->resolved_idx;

    int result_idx;

    /* Step 1: active font has the glyph — use it unconditionally.
     * This is the normal path for bold/italic/etc. variants. */
    {
        hb_codepoint_t gid;
        if (hb_font_get_nominal_glyph(b->fonts[active_idx].font, codepoint, &gid) && gid != 0) {
            result_idx = active_idx;
            goto cache_and_return;
        }
    }

    /* Step 2: active font lacks the glyph — ask fontconfig for a fallback. */
    if (b->font_bindings) {
        uint32_t fc_handle = font_bindings_find_font_for_codepoint(
            b->font_bindings, codepoint);
        if (fc_handle && fc_handle != active_handle) {
            int idx = text_ensure_hb_font(b, fc_handle);
            if (idx >= 0) {
                hb_codepoint_t gid;
                if (hb_font_get_nominal_glyph(b->fonts[idx].font, codepoint, &gid) && gid != 0) {
                    result_idx = idx;
                    goto cache_and_return;
                }
            }
        }
    }

    /* Step 3: nothing better found — stay with the active font. */
    result_idx = active_idx;

cache_and_return:
    resolve_cache_insert(codepoint, active_handle, result_idx);
    return result_idx;
}

/* ================================================================== */
/*  Host functions                                                     */
/* ================================================================== */

static wasm_trap_t *fn_text_load_font(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t font_ptr = (uint32_t)ARG_I32(0);
    int32_t  font_size = ARG_I32(1);
    uint32_t cp_ptr = (uint32_t)ARG_I32(2);
    int32_t  glyph_count = ARG_I32(3);
    uint8_t *mem = wasm_mem_base(B);
    size_t   msz = wasm_mem_size(B);

    /* New path: NULL data pointer means "load for font handle" */
    if (font_ptr == 0 && font_size == 0) {
        uint32_t font_handle = cp_ptr;
        if (!B->font_bindings) { RET_I32(0); return NULL; }
        uint32_t data_size = 0;
        const uint8_t *data = font_bindings_get_data(B->font_bindings, font_handle, &data_size);
        if (!data || data_size == 0) { RET_I32(0); return NULL; }
        int idx = text_add_font_from_data(B, data, data_size, font_handle);
        if (idx < 0) { RET_I32(0); return NULL; }
        B->active_font = idx;
        RET_I32(1); return NULL;
    }

    /* Legacy path */
    if (!mem || font_size <= 0 || (size_t)font_ptr + font_size > msz) { RET_I32(0); return NULL; }
    if (font_size > TEXT_MAX_FONT_SIZE) { RET_I32(0); return NULL; }
    if (!validate_font_magic(mem + font_ptr, font_size)) { RET_I32(0); return NULL; }

    if (B->hb_font)  { hb_font_destroy(B->hb_font); B->hb_font = NULL; }
    if (B->hb_face)  { hb_face_destroy(B->hb_face); B->hb_face = NULL; }
    if (B->hb_blob)  { hb_blob_destroy(B->hb_blob); B->hb_blob = NULL; }
    free(B->font_data); B->font_data = NULL;
    free(B->gid_to_guest); B->gid_to_guest = NULL;
    B->loaded = 0;

    B->font_data_size = (uint32_t)font_size;
    B->font_data = malloc(font_size);
    memcpy(B->font_data, mem + font_ptr, font_size);
    B->hb_blob = hb_blob_create((const char *)B->font_data, font_size,
                                 HB_MEMORY_MODE_READONLY, NULL, NULL);
    if (!B->hb_blob || hb_blob_get_length(B->hb_blob) == 0) { RET_I32(0); return NULL; }
    B->hb_face = hb_face_create(B->hb_blob, 0);
    B->hb_font = hb_font_create(B->hb_face);

    if (glyph_count > 0 && cp_ptr != 0 && (size_t)cp_ptr + glyph_count * 4 <= msz) {
        const uint32_t *codepoints = (const uint32_t *)(mem + cp_ptr);
        unsigned int num_font_glyphs = hb_face_get_glyph_count(B->hb_face);
        B->gid_map_size = num_font_glyphs;
        B->gid_to_guest = calloc(num_font_glyphs, sizeof(uint32_t));
        for (uint32_t i = 0; i < num_font_glyphs; i++) B->gid_to_guest[i] = 0xFFFFFFFFu;
        for (int32_t gi = 0; gi < glyph_count; gi++) {
            hb_codepoint_t gid;
            if (hb_font_get_nominal_glyph(B->hb_font, codepoints[gi], &gid))
                if (gid < num_font_glyphs) B->gid_to_guest[gid] = (uint32_t)gi;
        }
    }
    B->loaded = 1;
    RET_I32(1); return NULL;
}

static wasm_trap_t *fn_text_shape(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t cp_ptr     = (uint32_t)ARG_I32(0);
    int32_t  cp_count   = ARG_I32(1);
    float    font_size  = ARG_F32(2);
    int32_t  direction  = ARG_I32(3);
    uint32_t out_ptr    = (uint32_t)ARG_I32(4);
    int32_t  max_glyphs = ARG_I32(5);

    uint8_t *mem = wasm_mem_base(B);
    size_t   msz = wasm_mem_size(B);

    if (!mem || cp_count <= 0 || cp_count > TEXT_MAX_CODEPOINTS ||
        max_glyphs <= 0 || max_glyphs > TEXT_MAX_GLYPHS_OUT ||
        (size_t)cp_ptr + cp_count * 4 > msz ||
        (size_t)out_ptr + max_glyphs * TEXT_GLYPH_OUT_SIZE > msz) {
        RET_I32(0); return NULL;
    }

    int active_idx = -1;
    if (B->font_count > 0 && B->active_font >= 0 && B->active_font < B->font_count)
        active_idx = B->active_font;
    if (active_idx < 0) { RET_I32(0); return NULL; }

    const uint32_t *codepoints = (const uint32_t *)(mem + cp_ptr);
    uint8_t *out = mem + out_ptr;

    /* Determine font index for each codepoint.
     *
     * We do NOT just check hb_font_get_nominal_glyph on the primary font,
     * because it may have cmap entries for scripts it can't shape properly
     * (e.g. NotoSans-Regular has Arabic cmap but no Arabic GSUB features,
     * resulting in disconnected isolated forms).
     *
     * Instead, we ask fontconfig for the canonical font for each codepoint.
     * Fontconfig knows about script-specific shaping support. With the
     * resolve cache + codepoint cache this is O(1) for repeated codepoints. */
    int stack_fonts[4096];
    int *cp_font = cp_count <= 4096 ? stack_fonts : (int *)malloc(cp_count * sizeof(int));

    for (int i = 0; i < cp_count; i++)
        cp_font[i] = text_resolve_font_for_codepoint(B, codepoints[i], active_idx);

    /* Process sub-runs grouped by font */
    int total = 0;
    int sub_start = 0;
    while (sub_start < cp_count && total < max_glyphs) {
        int fi = cp_font[sub_start];
        int sub_end = sub_start + 1;
        while (sub_end < cp_count && cp_font[sub_end] == fi)
            sub_end++;
        int sub_len = sub_end - sub_start;

        TextFont *tf = &B->fonts[fi];
        unsigned int upem = hb_face_get_upem(tf->face);
        hb_font_set_scale(tf->font, (int)upem, (int)upem);

        hb_buffer_t *buf = hb_buffer_create();
        hb_buffer_add_codepoints(buf, codepoints + sub_start, sub_len, 0, sub_len);
        hb_buffer_set_direction(buf, direction ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(buf);
        hb_buffer_set_direction(buf, direction ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_shape(tf->font, buf, NULL, 0);

        unsigned int glyph_count;
        hb_glyph_info_t     *gi = hb_buffer_get_glyph_infos(buf, &glyph_count);
        hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(buf, &glyph_count);

        float scale = font_size / (float)upem;
        int n = (int)glyph_count;
        if (n > max_glyphs - total) n = max_glyphs - total;

        for (int i = 0; i < n; i++) {
            uint32_t out_gid = gi[i].codepoint;
            float xa = (float)gp[i].x_advance * scale;
            float xo = (float)gp[i].x_offset  * scale;
            float yo = (float)gp[i].y_offset  * scale;
            uint32_t cluster = gi[i].cluster + (uint32_t)sub_start;
            uint32_t font_handle = tf->font_handle;

            uint8_t *p = out + total * TEXT_GLYPH_OUT_SIZE;
            memcpy(p +  0, &out_gid, 4);
            memcpy(p +  4, &xa, 4);
            memcpy(p +  8, &xo, 4);
            memcpy(p + 12, &yo, 4);
            memcpy(p + 16, &cluster, 4);
            memcpy(p + 20, &font_handle, 4);
            total++;
        }

        hb_buffer_destroy(buf);
        sub_start = sub_end;
    }

    if (cp_font != stack_fonts) free(cp_font);
    RET_I32(total);
    return NULL;
}

static wasm_trap_t *fn_text_set_font(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t font_handle = (uint32_t)ARG_I32(0);
    int idx = text_find_font_for_handle(B, font_handle);
    if (idx >= 0) { B->active_font = idx; RET_I32(1); return NULL; }
    if (!B->font_bindings) { RET_I32(0); return NULL; }
    uint32_t data_size = 0;
    const uint8_t *data = font_bindings_get_data(B->font_bindings, font_handle, &data_size);
    if (!data || data_size == 0) { RET_I32(0); return NULL; }
    idx = text_add_font_from_data(B, data, data_size, font_handle);
    if (idx < 0) { RET_I32(0); return NULL; }
    B->active_font = idx;
    RET_I32(1); return NULL;
}

static wasm_trap_t *fn_text_bidi(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t cp_ptr   = (uint32_t)ARG_I32(0);
    int32_t  count    = ARG_I32(1);
    uint32_t lev_ptr  = (uint32_t)ARG_I32(2);
    uint32_t run_ptr  = (uint32_t)ARG_I32(3);
    int32_t  max_runs = ARG_I32(4);

    uint8_t *mem = wasm_mem_base(B);
    size_t   msz = wasm_mem_size(B);

    if (!mem || count <= 0 || count > TEXT_MAX_CODEPOINTS ||
        max_runs <= 0 || max_runs > TEXT_MAX_BIDI_RUNS_OUT ||
        (size_t)cp_ptr + count * 4 > msz ||
        (size_t)lev_ptr + count > msz ||
        (size_t)run_ptr + max_runs * 12 > msz) {
        RET_I32(0); return NULL;
    }

    const uint32_t *codepoints = (const uint32_t *)(mem + cp_ptr);

    int32_t *cp_to_u16 = malloc(count * sizeof(int32_t));
    if (!cp_to_u16) { RET_I32(0); return NULL; }
    int32_t u16len = utf32_to_u16(B, codepoints, count, cp_to_u16);

    UErrorCode err = U_ZERO_ERROR;
    ubidi_setPara(B->ubidi, B->u16buf, u16len, UBIDI_DEFAULT_LTR, NULL, &err);
    if (U_FAILURE(err)) { free(cp_to_u16); RET_I32(0); return NULL; }

    const UBiDiLevel *u16levels = ubidi_getLevels(B->ubidi, &err);
    uint8_t *out_levels = mem + lev_ptr;
    for (int32_t i = 0; i < count; i++)
        out_levels[i] = (uint8_t)u16levels[cp_to_u16[i]];

    int32_t n_runs = ubidi_countRuns(B->ubidi, &err);
    int32_t *u16_to_cp = malloc(u16len * sizeof(int32_t));
    if (!u16_to_cp) { free(cp_to_u16); RET_I32(0); return NULL; }
    build_u16_to_cp(cp_to_u16, count, u16len, u16_to_cp);

    int n = n_runs < max_runs ? n_runs : max_runs;
    uint8_t *out_runs = mem + run_ptr;
    for (int32_t i = 0; i < n; i++) {
        int32_t logical_start, length;
        UBiDiDirection dir = ubidi_getVisualRun(B->ubidi, i, &logical_start, &length);
        int32_t cp_start = u16_to_cp[logical_start];
        int32_t cp_end_u16 = logical_start + length;
        int32_t cp_end = (cp_end_u16 >= u16len) ? count : u16_to_cp[cp_end_u16];
        int32_t cp_len = cp_end - cp_start;

        uint32_t off = (uint32_t)cp_start;
        uint32_t len = (uint32_t)cp_len;
        uint32_t d   = (dir == UBIDI_RTL) ? 1 : 0;
        memcpy(out_runs + i * 12 + 0, &off, 4);
        memcpy(out_runs + i * 12 + 4, &len, 4);
        memcpy(out_runs + i * 12 + 8, &d,   4);
    }

    UBiDiDirection base = ubidi_getBaseDirection(B->u16buf, u16len);
    int base_rtl = (base == UBIDI_RTL) ? 1 : 0;

    free(cp_to_u16);
    free(u16_to_cp);
    RET_I32((n << 16) | base_rtl);
    return NULL;
}

static wasm_trap_t *fn_text_line_breaks(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t cp_ptr  = (uint32_t)ARG_I32(0);
    int32_t  count   = ARG_I32(1);
    uint32_t out_ptr = (uint32_t)ARG_I32(2);

    uint8_t *mem = wasm_mem_base(B);
    size_t   msz = wasm_mem_size(B);

    if (!mem || count <= 0 || count > TEXT_MAX_CODEPOINTS ||
        (size_t)cp_ptr + count * 4 > msz ||
        (size_t)out_ptr + count > msz) {
        return NULL;
    }

    const uint32_t *codepoints = (const uint32_t *)(mem + cp_ptr);
    uint8_t *out = mem + out_ptr;

    int32_t *cp_to_u16 = malloc(count * sizeof(int32_t));
    if (!cp_to_u16) return NULL;
    int32_t u16len = utf32_to_u16(B, codepoints, count, cp_to_u16);

    int32_t *u16_to_cp = malloc(u16len * sizeof(int32_t));
    if (!u16_to_cp) { free(cp_to_u16); return NULL; }
    build_u16_to_cp(cp_to_u16, count, u16len, u16_to_cp);

    memset(out, 0, count);

    UErrorCode err = U_ZERO_ERROR;
    UBreakIterator *brk = ubrk_open(UBRK_LINE, NULL, B->u16buf, u16len, &err);
    if (U_SUCCESS(err)) {
        int32_t pos = ubrk_next(brk);
        while (pos != UBRK_DONE) {
            int32_t cp_pos = (pos >= u16len) ? count : u16_to_cp[pos];
            if (cp_pos > 0 && cp_pos <= count) {
                int32_t status = ubrk_getRuleStatus(brk);
                out[cp_pos - 1] = (status == UBRK_LINE_HARD) ? 2 : 1;
            }
            pos = ubrk_next(brk);
        }
        ubrk_close(brk);
    }

    free(cp_to_u16);
    free(u16_to_cp);
    return NULL;
}

static wasm_trap_t *fn_text_grapheme_breaks(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t cp_ptr  = (uint32_t)ARG_I32(0);
    int32_t  count   = ARG_I32(1);
    uint32_t out_ptr = (uint32_t)ARG_I32(2);

    uint8_t *mem = wasm_mem_base(B);
    size_t   msz = wasm_mem_size(B);

    if (!mem || count <= 0 || count > TEXT_MAX_CODEPOINTS ||
        (size_t)cp_ptr + count * 4 > msz ||
        (size_t)out_ptr + count > msz) {
        return NULL;
    }

    const uint32_t *codepoints = (const uint32_t *)(mem + cp_ptr);
    uint8_t *out = mem + out_ptr;

    int32_t *cp_to_u16 = malloc(count * sizeof(int32_t));
    if (!cp_to_u16) return NULL;
    int32_t u16len = utf32_to_u16(B, codepoints, count, cp_to_u16);

    memset(out, 0, count);

    UErrorCode err = U_ZERO_ERROR;
    UBreakIterator *brk = ubrk_open(UBRK_CHARACTER, NULL, B->u16buf, u16len, &err);
    if (U_SUCCESS(err)) {
        int32_t pos = 0;
        while (pos != UBRK_DONE) {
            int32_t cp_pos = 0;
            if (pos < u16len) {
                for (int32_t i = 0; i < count; i++) {
                    if (cp_to_u16[i] == pos) { cp_pos = i; break; }
                    if (cp_to_u16[i] > pos)  { cp_pos = i > 0 ? i : 0; break; }
                    cp_pos = i;
                }
            }
            if (cp_pos >= 0 && cp_pos < count) out[cp_pos] = 1;
            pos = ubrk_next(brk);
        }
        ubrk_close(brk);
    }

    if (count > 0) out[0] = 1;
    free(cp_to_u16);
    return NULL;
}

/* ================================================================== */
/*  Binding table                                                      */
/* ================================================================== */

typedef struct {
    const char *name;
    wasm_func_callback_with_env_t cb;
    uint32_t np; wasm_valkind_t params[6];
    uint32_t nr; wasm_valkind_t results[1];
} TextBindingEntry;

#define I WASM_I32
#define F WASM_F32

static const TextBindingEntry TEXT_BINDINGS[] = {
    {"text_load_font",       fn_text_load_font,       4, {I,I,I,I},     1, {I}},
    {"text_shape",           fn_text_shape,            6, {I,I,F,I,I,I}, 1, {I}},
    {"text_set_font",        fn_text_set_font,         1, {I,0,0,0,0,0}, 1, {I}},
    {"text_bidi",            fn_text_bidi,             5, {I,I,I,I,I},   1, {I}},
    {"text_line_breaks",     fn_text_line_breaks,      3, {I,I,I},       0, {0}},
    {"text_grapheme_breaks", fn_text_grapheme_breaks,  3, {I,I,I},       0, {0}},
};

#undef I
#undef F

#define NUM_TEXT_BINDINGS (sizeof(TEXT_BINDINGS)/sizeof(TEXT_BINDINGS[0]))

static wasm_functype_t *make_ft(uint32_t np, const wasm_valkind_t p[],
                                uint32_t nr, const wasm_valkind_t r[]) {
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[6];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else {
        wasm_valtype_vec_new_empty(&params);
    }
    if (nr > 0) {
        wasm_valtype_t *rt[1];
        rt[0] = wasm_valtype_new(r[0]);
        wasm_valtype_vec_new(&results, nr, rt);
    } else {
        wasm_valtype_vec_new_empty(&results);
    }
    return wasm_functype_new(&params, &results);
}

void text_bindings_init(TextBindings *b) {
    memset(b, 0, sizeof(*b));
    b->ubidi = ubidi_open();
    b->active_font = -1;
    b->fonts = NULL;
    b->font_count = 0;
    b->font_cap = 0;
    resolve_cache_clear();
    printf("[text] Text bindings ready (%zu imports, ICU + HarfBuzz + fontconfig)\n",
           NUM_TEXT_BINDINGS);
}

void text_bindings_set_memory(TextBindings *b, wasm_memory_t *mem) {
    b->memory = mem;
}

void text_bindings_set_font_bindings(TextBindings *b, struct FontBindings *fb) {
    b->font_bindings = fb;
}

size_t text_bindings_get_imports(TextBindings *b, wasm_store_t *store,
                                 const char ***out_names,
                                 wasm_func_t ***out_funcs) {
    static const char *names[NUM_TEXT_BINDINGS];
    static wasm_func_t *funcs[NUM_TEXT_BINDINGS];
    for (size_t i = 0; i < NUM_TEXT_BINDINGS; i++) {
        names[i] = TEXT_BINDINGS[i].name;
        wasm_functype_t *ft = make_ft(
            TEXT_BINDINGS[i].np, TEXT_BINDINGS[i].params,
            TEXT_BINDINGS[i].nr, TEXT_BINDINGS[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft, TEXT_BINDINGS[i].cb, b, NULL);
        wasm_functype_delete(ft);
    }
    *out_names = names;
    *out_funcs = funcs;
    return NUM_TEXT_BINDINGS;
}

void text_bindings_destroy(TextBindings *b) {
    for (int i = 0; i < b->font_count; i++) {
        TextFont *tf = &b->fonts[i];
        if (tf->font) hb_font_destroy(tf->font);
        if (tf->face) hb_face_destroy(tf->face);
        if (tf->blob) hb_blob_destroy(tf->blob);
        free(tf->data);
    }
    free(b->fonts);
    if (b->hb_font)  hb_font_destroy(b->hb_font);
    if (b->hb_face)  hb_face_destroy(b->hb_face);
    if (b->hb_blob)  hb_blob_destroy(b->hb_blob);
    if (b->ubidi)    ubidi_close(b->ubidi);
    free(b->font_data);
    free(b->gid_to_guest);
    free(b->u16buf);
    resolve_cache_clear();
    memset(b, 0, sizeof(*b));
}
