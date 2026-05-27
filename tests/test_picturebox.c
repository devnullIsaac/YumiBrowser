/*
 * test_picturebox.c - Tests for picturebox.h: the platform-independent picture/animated-image box widget (sizing, frames, playback).
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
 * @file test_picturebox.c
 * @brief Tests for picturebox.h — platform-independent picture/animated-image box widget.
 */
#define PICTUREBOX_IMPLEMENTATION
#include "picturebox.h"

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-50s", #name); name(); printf("PASS\n"); } while(0)
#define NEAR(a, b) (fabsf((a) - (b)) < 0.01f)

/* Dummy image handles for testing */
static int g_dummy_image_a = 1;
static int g_dummy_image_b = 2;
static int g_dummy_frame_0 = 10;
static int g_dummy_frame_1 = 11;
static int g_dummy_frame_2 = 12;

static void make_pb(picturebox_t *pb, float w, float h) {
    picturebox_create(pb, w, h);
}

static void make_pb_with_source(picturebox_t *pb, float w, float h,
                                int src_w, int src_h) {
    picturebox_create(pb, w, h);
    picturebox_set_source(pb, &g_dummy_image_a, src_w, src_h);
}

/* ═══════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_create_destroy) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    assert(pb.box_w == 400.0f);
    assert(pb.box_h == 300.0f);
    assert(pb.source == NULL);
    assert(pb.frame_count == 0);
    assert(pb.zoom == 1.0f);
    picturebox_destroy(&pb);
    assert(pb.box_w == 0.0f);
    assert(pb.source == NULL);
}

TEST(test_create_defaults) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    assert(pb.fit == PICTUREBOX_FIT_CONTAIN);
    assert(pb.align_h == PICTUREBOX_ALIGN_H_CENTER);
    assert(pb.align_v == PICTUREBOX_ALIGN_V_CENTER);
    assert(pb.zoom == 1.0f);
    assert(pb.pan_x == 0.0f);
    assert(pb.pan_y == 0.0f);
    assert(pb.rotation_deg == 0.0f);
    assert(pb.flip == PICTUREBOX_FLIP_NONE);
    assert(pb.anim_state == PICTUREBOX_ANIM_STOPPED);
    assert(pb.anim_direction == 1);
    assert(pb.layout_dirty == 1);
    picturebox_destroy(&pb);
}

TEST(test_destroy_null_safe) {
    picturebox_destroy(NULL);
    /* Should not crash. */
}

TEST(test_destroy_zeroed) {
    picturebox_t pb;
    memset(&pb, 0, sizeof(pb));
    picturebox_destroy(&pb); /* Should be safe on zero-init. */
}

/* ═══════════════════════════════════════════════════════════════
 *  Source
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_source) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_source(&pb, &g_dummy_image_a, 800, 600);
    assert(pb.source == &g_dummy_image_a);
    assert(pb.src_w == 800);
    assert(pb.src_h == 600);
    assert(pb.layout_dirty == 1);
    picturebox_destroy(&pb);
}

TEST(test_set_source_replaces) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_source(&pb, &g_dummy_image_b, 1920, 1080);
    assert(pb.source == &g_dummy_image_b);
    assert(pb.src_w == 1920);
    assert(pb.src_h == 1080);
    picturebox_destroy(&pb);
}

TEST(test_clear_source) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_clear_source(&pb);
    assert(pb.source == NULL);
    assert(pb.src_w == 0);
    assert(pb.src_h == 0);
    assert(pb.frame_count == 0);
    picturebox_destroy(&pb);
}

TEST(test_get_current_source_static) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    assert(picturebox_get_current_source(&pb) == &g_dummy_image_a);
    picturebox_destroy(&pb);
}

TEST(test_get_current_source_animated) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    assert(picturebox_get_current_source(&pb) == &g_dummy_frame_0);
    picturebox_seek_frame(&pb, 1);
    assert(picturebox_get_current_source(&pb) == &g_dummy_frame_1);
    picturebox_destroy(&pb);
}

TEST(test_get_current_source_no_source) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    assert(picturebox_get_current_source(&pb) == NULL);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Geometry
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_position) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_position(&pb, 100.0f, 50.0f);
    assert(picturebox_get_x(&pb) == 100.0f);
    assert(picturebox_get_y(&pb) == 50.0f);
    picturebox_destroy(&pb);
}

TEST(test_set_size) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_size(&pb, 500.0f, 400.0f);
    assert(picturebox_get_width(&pb) == 500.0f);
    assert(picturebox_get_height(&pb) == 400.0f);
    assert(pb.layout_dirty == 1);
    picturebox_destroy(&pb);
}

TEST(test_set_size_clamped) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_size_constraints(&pb, 50.0f, 50.0f, 200.0f, 200.0f);
    picturebox_set_size(&pb, 10.0f, 10.0f);
    assert(picturebox_get_width(&pb) == 50.0f);
    assert(picturebox_get_height(&pb) == 50.0f);
    picturebox_set_size(&pb, 999.0f, 999.0f);
    assert(picturebox_get_width(&pb) == 200.0f);
    assert(picturebox_get_height(&pb) == 200.0f);
    picturebox_destroy(&pb);
}

TEST(test_set_padding) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_padding(&pb, 10.0f);
    assert(pb.padding == 10.0f);
    assert(pb.layout_dirty == 1);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Fit & Alignment
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_fit) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_COVER);
    assert(picturebox_get_fit(&pb) == PICTUREBOX_FIT_COVER);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    assert(picturebox_get_fit(&pb) == PICTUREBOX_FIT_FILL);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_NONE);
    assert(picturebox_get_fit(&pb) == PICTUREBOX_FIT_NONE);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_SCALE_DOWN);
    assert(picturebox_get_fit(&pb) == PICTUREBOX_FIT_SCALE_DOWN);
    picturebox_destroy(&pb);
}

TEST(test_set_align) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_align(&pb, PICTUREBOX_ALIGN_H_LEFT, PICTUREBOX_ALIGN_V_TOP);
    assert(picturebox_get_align_h(&pb) == PICTUREBOX_ALIGN_H_LEFT);
    assert(picturebox_get_align_v(&pb) == PICTUREBOX_ALIGN_V_TOP);
    picturebox_set_align(&pb, PICTUREBOX_ALIGN_H_RIGHT, PICTUREBOX_ALIGN_V_BOTTOM);
    assert(picturebox_get_align_h(&pb) == PICTUREBOX_ALIGN_H_RIGHT);
    assert(picturebox_get_align_v(&pb) == PICTUREBOX_ALIGN_V_BOTTOM);
    picturebox_destroy(&pb);
}

TEST(test_fit_contain_landscape_in_square) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 400, 800, 400);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_CONTAIN);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    /* 800x400 image in 400x400 box: scale = 0.5, dst = 400x200, centered */
    assert(NEAR(r.w, 400.0f));
    assert(NEAR(r.h, 200.0f));
    assert(NEAR(r.y, 100.0f)); /* centered vertically */
    picturebox_destroy(&pb);
}

TEST(test_fit_contain_portrait_in_square) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 400, 400, 800);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_CONTAIN);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    /* 400x800 in 400x400: scale = 0.5, dst = 200x400, centered */
    assert(NEAR(r.w, 200.0f));
    assert(NEAR(r.h, 400.0f));
    assert(NEAR(r.x, 100.0f)); /* centered horizontally */
    picturebox_destroy(&pb);
}

TEST(test_fit_cover) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 400, 800, 400);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_COVER);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    /* 800x400 in 400x400: scale = max(0.5, 1.0) = 1.0, dst = 800x400 */
    assert(NEAR(r.w, 800.0f));
    assert(NEAR(r.h, 400.0f));
    picturebox_destroy(&pb);
}

TEST(test_fit_fill) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    /* Fill stretches to available area */
    assert(NEAR(r.w, 400.0f));
    assert(NEAR(r.h, 300.0f));
    picturebox_destroy(&pb);
}

TEST(test_fit_none) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 100, 80);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_NONE);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    /* 1:1 pixels */
    assert(NEAR(r.w, 100.0f));
    assert(NEAR(r.h, 80.0f));
    picturebox_destroy(&pb);
}

TEST(test_fit_scale_down_small_image) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 100, 80);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_SCALE_DOWN);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    /* Image smaller than box: scale capped at 1.0 */
    assert(NEAR(r.w, 100.0f));
    assert(NEAR(r.h, 80.0f));
    picturebox_destroy(&pb);
}

TEST(test_fit_scale_down_large_image) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_SCALE_DOWN);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    /* Same as contain for images larger than box */
    assert(NEAR(r.w, 400.0f));
    assert(NEAR(r.h, 300.0f));
    picturebox_destroy(&pb);
}

TEST(test_align_top_left) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 400, 200, 200);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_NONE);
    picturebox_set_align(&pb, PICTUREBOX_ALIGN_H_LEFT, PICTUREBOX_ALIGN_V_TOP);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    assert(NEAR(r.x, 0.0f));
    assert(NEAR(r.y, 0.0f));
    picturebox_destroy(&pb);
}

TEST(test_align_bottom_right) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 400, 200, 200);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_NONE);
    picturebox_set_align(&pb, PICTUREBOX_ALIGN_H_RIGHT, PICTUREBOX_ALIGN_V_BOTTOM);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    assert(NEAR(r.x, 200.0f));
    assert(NEAR(r.y, 200.0f));
    picturebox_destroy(&pb);
}

TEST(test_align_center) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 400, 200, 200);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_NONE);
    picturebox_set_align(&pb, PICTUREBOX_ALIGN_H_CENTER, PICTUREBOX_ALIGN_V_CENTER);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    assert(NEAR(r.x, 100.0f));
    assert(NEAR(r.y, 100.0f));
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Transform
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_zoom) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_zoom(&pb, 2.0f);
    assert(NEAR(picturebox_get_zoom(&pb), 2.0f));
    assert(pb.layout_dirty == 1);
    picturebox_destroy(&pb);
}

TEST(test_zoom_clamp) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_zoom(&pb, 0.001f);
    assert(picturebox_get_zoom(&pb) >= PICTUREBOX_MIN_ZOOM);
    picturebox_set_zoom(&pb, 999.0f);
    assert(picturebox_get_zoom(&pb) <= PICTUREBOX_MAX_ZOOM);
    picturebox_destroy(&pb);
}

TEST(test_zoom_by) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    float old = picturebox_get_zoom(&pb);
    picturebox_zoom_by(&pb, 2.0f, 200.0f, 150.0f);
    assert(NEAR(picturebox_get_zoom(&pb), old * 2.0f));
    picturebox_destroy(&pb);
}

TEST(test_zoom_by_noop_at_limit) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_zoom(&pb, PICTUREBOX_MAX_ZOOM);
    pb.layout_dirty = 0;
    picturebox_zoom_by(&pb, 2.0f, 200.0f, 150.0f);
    /* Zoom should not have changed */
    assert(NEAR(picturebox_get_zoom(&pb), PICTUREBOX_MAX_ZOOM));
    picturebox_destroy(&pb);
}

TEST(test_set_pan) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_pan(&pb, 50.0f, -30.0f);
    float px, py;
    picturebox_get_pan(&pb, &px, &py);
    assert(NEAR(px, 50.0f));
    assert(NEAR(py, -30.0f));
    picturebox_destroy(&pb);
}

TEST(test_pan_by) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_pan(&pb, 10.0f, 20.0f);
    picturebox_pan_by(&pb, 5.0f, -10.0f);
    float px, py;
    picturebox_get_pan(&pb, &px, &py);
    assert(NEAR(px, 15.0f));
    assert(NEAR(py, 10.0f));
    picturebox_destroy(&pb);
}

TEST(test_get_pan_null_out) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_pan(&pb, 10.0f, 20.0f);
    /* Should not crash with NULL out params */
    picturebox_get_pan(&pb, NULL, NULL);
    float px;
    picturebox_get_pan(&pb, &px, NULL);
    assert(NEAR(px, 10.0f));
    picturebox_destroy(&pb);
}

TEST(test_set_rotation) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_rotation(&pb, 90.0f);
    assert(NEAR(picturebox_get_rotation(&pb), 90.0f));
    picturebox_destroy(&pb);
}

TEST(test_rotate_by) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_rotation(&pb, 45.0f);
    picturebox_rotate_by(&pb, 45.0f);
    assert(NEAR(picturebox_get_rotation(&pb), 90.0f));
    picturebox_destroy(&pb);
}

TEST(test_set_flip) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_flip(&pb, PICTUREBOX_FLIP_HORIZONTAL);
    assert(picturebox_get_flip(&pb) == PICTUREBOX_FLIP_HORIZONTAL);
    picturebox_set_flip(&pb, PICTUREBOX_FLIP_VERTICAL);
    assert(picturebox_get_flip(&pb) == PICTUREBOX_FLIP_VERTICAL);
    picturebox_set_flip(&pb, PICTUREBOX_FLIP_BOTH);
    assert(picturebox_get_flip(&pb) == PICTUREBOX_FLIP_BOTH);
    picturebox_destroy(&pb);
}

TEST(test_reset_transform) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_zoom(&pb, 3.0f);
    picturebox_set_pan(&pb, 50, 50);
    picturebox_set_rotation(&pb, 45.0f);
    picturebox_set_flip(&pb, PICTUREBOX_FLIP_BOTH);
    picturebox_reset_transform(&pb);
    assert(NEAR(picturebox_get_zoom(&pb), 1.0f));
    float px, py;
    picturebox_get_pan(&pb, &px, &py);
    assert(NEAR(px, 0.0f));
    assert(NEAR(py, 0.0f));
    assert(NEAR(picturebox_get_rotation(&pb), 0.0f));
    assert(picturebox_get_flip(&pb) == PICTUREBOX_FLIP_NONE);
    picturebox_destroy(&pb);
}

TEST(test_zoom_to_fit) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_zoom(&pb, 5.0f);
    picturebox_set_pan(&pb, 100, 100);
    picturebox_zoom_to_fit(&pb);
    assert(NEAR(picturebox_get_zoom(&pb), 1.0f));
    assert(picturebox_get_fit(&pb) == PICTUREBOX_FIT_CONTAIN);
    float px, py;
    picturebox_get_pan(&pb, &px, &py);
    assert(NEAR(px, 0.0f));
    assert(NEAR(py, 0.0f));
    picturebox_destroy(&pb);
}

TEST(test_zoom_affects_dst_rect) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 400, 200, 200);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_NONE);
    picturebox_layout(&pb);
    picturebox_rect_t r1 = picturebox_get_dst_rect(&pb);

    picturebox_set_zoom(&pb, 2.0f);
    picturebox_layout(&pb);
    picturebox_rect_t r2 = picturebox_get_dst_rect(&pb);

    assert(NEAR(r2.w, r1.w * 2.0f));
    assert(NEAR(r2.h, r1.h * 2.0f));
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Layout
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_layout_clears_dirty) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    assert(pb.layout_dirty == 1);
    picturebox_layout(&pb);
    assert(pb.layout_dirty == 0);
    picturebox_destroy(&pb);
}

TEST(test_layout_no_source) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    assert(r.w == 0.0f);
    assert(r.h == 0.0f);
    picturebox_destroy(&pb);
}

TEST(test_layout_with_padding) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_padding(&pb, 20.0f);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    /* Available = 400-40 × 300-40 = 360 × 260 */
    assert(NEAR(r.w, 360.0f));
    assert(NEAR(r.h, 260.0f));
    assert(NEAR(r.x, 20.0f));
    assert(NEAR(r.y, 20.0f));
    picturebox_destroy(&pb);
}

TEST(test_get_dst_rect_lazy) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    assert(pb.layout_dirty == 1);
    /* get_dst_rect should trigger lazy layout */
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    assert(pb.layout_dirty == 0);
    assert(r.w > 0.0f);
    picturebox_destroy(&pb);
}

TEST(test_is_dirty) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    assert(picturebox_is_dirty(&pb) == 1);
    picturebox_layout(&pb);
    assert(picturebox_is_dirty(&pb) == 0);
    picturebox_set_zoom(&pb, 2.0f);
    assert(picturebox_is_dirty(&pb) == 1);
    picturebox_destroy(&pb);
}

TEST(test_get_source_rect_static) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_rect_t sr = picturebox_get_source_rect(&pb);
    assert(NEAR(sr.x, 0.0f));
    assert(NEAR(sr.y, 0.0f));
    assert(NEAR(sr.w, 800.0f));
    assert(NEAR(sr.h, 600.0f));
    picturebox_destroy(&pb);
}

TEST(test_get_source_rect_frame_subrect) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 256, 256);
    picturebox_frame_t f = {0};
    f.user        = &g_dummy_frame_0;
    f.duration_ms = 100.0f;
    f.src_x       = 32;
    f.src_y       = 64;
    f.src_w       = 128;
    f.src_h       = 128;
    picturebox_add_frame_ex(&pb, &f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f); /* full-size frame */

    picturebox_seek_frame(&pb, 0);
    picturebox_rect_t sr = picturebox_get_source_rect(&pb);
    assert(NEAR(sr.x, 32.0f));
    assert(NEAR(sr.y, 64.0f));
    assert(NEAR(sr.w, 128.0f));
    assert(NEAR(sr.h, 128.0f));
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Coordinate Mapping
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_box_to_image) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);

    float ix, iy;
    int inside = picturebox_box_to_image(&pb, r.x, r.y, &ix, &iy);
    assert(inside == 1);
    assert(NEAR(ix, 0.0f));
    assert(NEAR(iy, 0.0f));

    inside = picturebox_box_to_image(&pb, r.x + r.w, r.y + r.h, &ix, &iy);
    /* At the very edge — should be at 800,600 which is out of bounds */
    assert(NEAR(ix, 800.0f));
    assert(NEAR(iy, 600.0f));
    picturebox_destroy(&pb);
}

TEST(test_box_to_image_no_source) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_layout(&pb);
    float ix, iy;
    int inside = picturebox_box_to_image(&pb, 100, 100, &ix, &iy);
    assert(inside == 0);
    picturebox_destroy(&pb);
}

TEST(test_image_to_box) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_layout(&pb);

    float bx, by;
    int ok = picturebox_image_to_box(&pb, 0, 0, &bx, &by);
    assert(ok == 1);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    assert(NEAR(bx, r.x));
    assert(NEAR(by, r.y));
    picturebox_destroy(&pb);
}

TEST(test_box_to_image_flipped) {
    picturebox_t pb;
    make_pb_with_source(&pb, 200, 200, 200, 200);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_flip(&pb, PICTUREBOX_FLIP_HORIZONTAL);
    picturebox_layout(&pb);

    float ix, iy;
    picturebox_box_to_image(&pb, 0, 0, &ix, &iy);
    /* Horizontal flip: x = 200 - 0 = 200 */
    assert(NEAR(ix, 200.0f));
    assert(NEAR(iy, 0.0f));
    picturebox_destroy(&pb);
}

TEST(test_roundtrip_mapping) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_CONTAIN);
    picturebox_layout(&pb);

    float ix = 400.0f, iy = 300.0f;
    float bx, by, ix2, iy2;
    picturebox_image_to_box(&pb, ix, iy, &bx, &by);
    picturebox_box_to_image(&pb, bx, by, &ix2, &iy2);
    assert(NEAR(ix, ix2));
    assert(NEAR(iy, iy2));
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Animation — Frame Management
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_add_frame) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    int ok = picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    assert(ok == 1);
    assert(picturebox_get_frame_count(&pb) == 1);
    picturebox_frame_t *f = picturebox_get_frame(&pb, 0);
    assert(f != NULL);
    assert(f->user == &g_dummy_frame_0);
    assert(NEAR(f->duration_ms, 100.0f));
    picturebox_destroy(&pb);
}

TEST(test_add_frame_default_duration) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 0.0f);
    picturebox_frame_t *f = picturebox_get_frame(&pb, 0);
    assert(NEAR(f->duration_ms, (float)PICTUREBOX_DEFAULT_FRAME_MS));
    picturebox_destroy(&pb);
}

TEST(test_add_multiple_frames) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 50.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_2, 150.0f);
    assert(picturebox_get_frame_count(&pb) == 3);
    assert(picturebox_get_frame(&pb, 0)->user == &g_dummy_frame_0);
    assert(picturebox_get_frame(&pb, 1)->user == &g_dummy_frame_1);
    assert(picturebox_get_frame(&pb, 2)->user == &g_dummy_frame_2);
    picturebox_destroy(&pb);
}

TEST(test_add_frame_ex) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 256, 256);
    picturebox_frame_t f = {0};
    f.user        = &g_dummy_frame_0;
    f.duration_ms = 200.0f;
    f.src_x       = 10;
    f.src_y       = 20;
    f.src_w       = 64;
    f.src_h       = 64;
    int ok = picturebox_add_frame_ex(&pb, &f);
    assert(ok == 1);
    picturebox_frame_t *out = picturebox_get_frame(&pb, 0);
    assert(out->src_x == 10);
    assert(out->src_y == 20);
    assert(out->src_w == 64);
    assert(out->src_h == 64);
    picturebox_destroy(&pb);
}

TEST(test_clear_frames) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_play(&pb);
    picturebox_clear_frames(&pb);
    assert(picturebox_get_frame_count(&pb) == 0);
    assert(picturebox_get_current_frame(&pb) == 0);
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_STOPPED);
    picturebox_destroy(&pb);
}

TEST(test_get_frame_bounds) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    assert(picturebox_get_frame(&pb, -1) == NULL);
    assert(picturebox_get_frame(&pb, 1) == NULL);
    assert(picturebox_get_frame(&pb, 999) == NULL);
    picturebox_destroy(&pb);
}

TEST(test_set_frame_duration) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_set_frame_duration(&pb, 0, 250.0f);
    assert(NEAR(picturebox_get_frame(&pb, 0)->duration_ms, 250.0f));
    picturebox_destroy(&pb);
}

TEST(test_set_frame_duration_bounds) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    /* Out of bounds — should be no-op */
    picturebox_set_frame_duration(&pb, 5, 200.0f);
    picturebox_set_frame_duration(&pb, -1, 200.0f);
    assert(NEAR(picturebox_get_frame(&pb, 0)->duration_ms, 100.0f));
    picturebox_destroy(&pb);
}

TEST(test_total_duration) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 50.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_2, 150.0f);
    assert(NEAR(picturebox_get_total_duration_ms(&pb), 300.0f));
    picturebox_destroy(&pb);
}

TEST(test_is_animated) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    assert(picturebox_is_animated(&pb) == 0);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    assert(picturebox_is_animated(&pb) == 0); /* 1 frame = not animated */
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    assert(picturebox_is_animated(&pb) == 1);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Animation — Playback
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_play_pause_stop) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);

    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_STOPPED);
    picturebox_play(&pb);
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_PLAYING);
    picturebox_pause(&pb);
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_PAUSED);
    picturebox_play(&pb);
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_PLAYING);
    picturebox_stop(&pb);
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_STOPPED);
    assert(picturebox_get_current_frame(&pb) == 0);
    picturebox_destroy(&pb);
}

TEST(test_play_requires_two_frames) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_play(&pb);
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_STOPPED);
    picturebox_destroy(&pb);
}

TEST(test_pause_only_when_playing) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_pause(&pb); /* Not playing — should be no-op */
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_STOPPED);
    picturebox_destroy(&pb);
}

TEST(test_seek_frame) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_2, 100.0f);

    picturebox_seek_frame(&pb, 2);
    assert(picturebox_get_current_frame(&pb) == 2);
    picturebox_seek_frame(&pb, 0);
    assert(picturebox_get_current_frame(&pb) == 0);
    picturebox_destroy(&pb);
}

TEST(test_seek_frame_clamp) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);

    picturebox_seek_frame(&pb, -5);
    assert(picturebox_get_current_frame(&pb) == 0);
    picturebox_seek_frame(&pb, 999);
    assert(picturebox_get_current_frame(&pb) == 1);
    picturebox_destroy(&pb);
}

TEST(test_seek_ms) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_2, 100.0f);

    picturebox_seek_ms(&pb, 150.0f); /* halfway into frame 1 */
    assert(picturebox_get_current_frame(&pb) == 1);
    picturebox_seek_ms(&pb, 250.0f); /* halfway into frame 2 */
    assert(picturebox_get_current_frame(&pb) == 2);
    picturebox_seek_ms(&pb, 999.0f); /* past end */
    assert(picturebox_get_current_frame(&pb) == 2);
    picturebox_destroy(&pb);
}

TEST(test_seek_empty) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    /* Should be safe on empty frame list */
    picturebox_seek_frame(&pb, 0);
    picturebox_seek_ms(&pb, 100.0f);
    assert(picturebox_get_current_frame(&pb) == 0);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Animation — Tick
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_tick_advances_frame) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_set_loop_mode(&pb, PICTUREBOX_LOOP_FOREVER, 0);
    picturebox_play(&pb);

    assert(picturebox_get_current_frame(&pb) == 0);
    picturebox_event_result_t r = picturebox_tick(&pb, 50.0f);
    assert(r.frame_changed == 0); /* Still in frame 0 */
    assert(picturebox_get_current_frame(&pb) == 0);

    r = picturebox_tick(&pb, 60.0f); /* 50+60=110 > 100 */
    assert(r.frame_changed == 1);
    assert(r.redraw == 1);
    assert(picturebox_get_current_frame(&pb) == 1);
    picturebox_destroy(&pb);
}

TEST(test_tick_noop_when_stopped) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    /* Not playing */
    picturebox_event_result_t r = picturebox_tick(&pb, 200.0f);
    assert(r.frame_changed == 0);
    assert(picturebox_get_current_frame(&pb) == 0);
    picturebox_destroy(&pb);
}

TEST(test_tick_noop_when_paused) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_play(&pb);
    picturebox_pause(&pb);
    picturebox_event_result_t r = picturebox_tick(&pb, 200.0f);
    assert(r.frame_changed == 0);
    picturebox_destroy(&pb);
}

TEST(test_tick_loop_none_stops) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_set_loop_mode(&pb, PICTUREBOX_LOOP_NONE, 0);
    picturebox_play(&pb);

    picturebox_tick(&pb, 150.0f); /* advance to frame 1, 50ms into it */
    assert(picturebox_get_current_frame(&pb) == 1);
    picturebox_tick(&pb, 100.0f); /* past end of frame 1 */
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_STOPPED);
    assert(picturebox_get_current_frame(&pb) == 1);
    picturebox_destroy(&pb);
}

TEST(test_tick_loop_forever) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_set_loop_mode(&pb, PICTUREBOX_LOOP_FOREVER, 0);
    picturebox_play(&pb);

    /* Advance through 2 full cycles + into frame 0 again */
    picturebox_tick(&pb, 250.0f); /* frame 0(100) + frame 1(100) + 50ms into frame 0 */
    assert(picturebox_get_current_frame(&pb) == 0);
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_PLAYING);
    assert(picturebox_get_loops_done(&pb) >= 1);
    picturebox_destroy(&pb);
}

TEST(test_tick_loop_ping_pong) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_2, 100.0f);
    picturebox_set_loop_mode(&pb, PICTUREBOX_LOOP_PING_PONG, 0);
    picturebox_play(&pb);

    /* Forward: 0 → 1 → 2, then reverse: 2 → 1 → 0 */
    picturebox_tick(&pb, 100.0f); /* frame 1 */
    assert(picturebox_get_current_frame(&pb) == 1);
    picturebox_tick(&pb, 100.0f); /* frame 2 */
    assert(picturebox_get_current_frame(&pb) == 2);
    picturebox_tick(&pb, 100.0f); /* reverse: frame 1 */
    assert(picturebox_get_current_frame(&pb) == 1);
    picturebox_tick(&pb, 100.0f); /* reverse: frame 0 */
    assert(picturebox_get_current_frame(&pb) == 0);
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_PLAYING);
    picturebox_destroy(&pb);
}

TEST(test_tick_loop_count) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 50.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 50.0f);
    picturebox_set_loop_mode(&pb, PICTUREBOX_LOOP_COUNT, 2);
    picturebox_play(&pb);

    /* 2 loops × 100ms each = 200ms total */
    picturebox_tick(&pb, 100.0f); /* 1 loop done, back to frame 0 */
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_PLAYING);
    assert(picturebox_get_loops_done(&pb) == 1);
    picturebox_tick(&pb, 100.0f); /* 2 loops done, should stop */
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_STOPPED);
    assert(picturebox_get_loops_done(&pb) == 2);
    picturebox_destroy(&pb);
}

TEST(test_tick_large_dt) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 10.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 10.0f);
    picturebox_set_loop_mode(&pb, PICTUREBOX_LOOP_NONE, 0);
    picturebox_play(&pb);

    /* Very large dt should not infinite loop — should stop at last frame */
    picturebox_event_result_t r = picturebox_tick(&pb, 10000.0f);
    assert(r.frame_changed == 1);
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_STOPPED);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Interaction Config
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_interaction_flags) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    assert(pb.interactive_pan == 0);
    assert(pb.interactive_zoom == 0);
    assert(pb.interactive_move == 0);
    assert(pb.interactive_resize == 0);
    assert(pb.interactive_rotate == 0);

    picturebox_set_interactive_pan(&pb, 1);
    picturebox_set_interactive_zoom(&pb, 1);
    picturebox_set_interactive_move(&pb, 1);
    picturebox_set_interactive_resize(&pb, 1);
    picturebox_set_interactive_rotate(&pb, 1);
    picturebox_set_show_handles(&pb, 1);

    assert(pb.interactive_pan == 1);
    assert(pb.interactive_zoom == 1);
    assert(pb.interactive_move == 1);
    assert(pb.interactive_resize == 1);
    assert(pb.interactive_rotate == 1);
    assert(pb.show_handles == 1);
    picturebox_destroy(&pb);
}

TEST(test_size_constraints) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_size_constraints(&pb, 100.0f, 80.0f, 800.0f, 600.0f);
    assert(NEAR(pb.min_w, 100.0f));
    assert(NEAR(pb.min_h, 80.0f));
    assert(NEAR(pb.max_w, 800.0f));
    assert(NEAR(pb.max_h, 600.0f));
    picturebox_destroy(&pb);
}

TEST(test_lock_aspect) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_lock_aspect(&pb, 1);
    assert(pb.lock_aspect == 1);
    picturebox_set_lock_aspect(&pb, 0);
    assert(pb.lock_aspect == 0);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Appearance
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_bg_color) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_bg_color(&pb, PICTUREBOX_COLOR(0.2f, 0.3f, 0.4f, 1.0f));
    assert(NEAR(pb.bg_color.r, 0.2f));
    assert(NEAR(pb.bg_color.g, 0.3f));
    assert(NEAR(pb.bg_color.b, 0.4f));
    assert(NEAR(pb.bg_color.a, 1.0f));
    picturebox_destroy(&pb);
}

TEST(test_set_checkerboard) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_checkerboard(&pb, 1, 16.0f);
    assert(pb.checkerboard == 1);
    assert(NEAR(pb.checker_size, 16.0f));
    picturebox_set_checkerboard(&pb, 1, 0.0f);
    assert(NEAR(pb.checker_size, 8.0f)); /* default fallback */
    picturebox_destroy(&pb);
}

TEST(test_set_border) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_border(&pb, PICTUREBOX_COLOR(1, 0, 0, 1), 2.0f, 4.0f);
    assert(NEAR(pb.border_color.r, 1.0f));
    assert(NEAR(pb.border_width, 2.0f));
    assert(NEAR(pb.corner_radius, 4.0f));
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Hit Testing
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_hit_test_image) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_layout(&pb);

    picturebox_hit_result_t hr = picturebox_hit_test(&pb, 200.0f, 150.0f);
    assert(hr.region == PICTUREBOX_HIT_IMAGE);
    assert(hr.image_x >= 0.0f && hr.image_x <= 800.0f);
    assert(hr.image_y >= 0.0f && hr.image_y <= 600.0f);
    picturebox_destroy(&pb);
}

TEST(test_hit_test_background) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 400, 200, 200);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_NONE);
    picturebox_set_align(&pb, PICTUREBOX_ALIGN_H_CENTER, PICTUREBOX_ALIGN_V_CENTER);
    picturebox_layout(&pb);
    /* Image is 200×200, centered in 400×400 box. Hit at top-left corner = background */
    picturebox_hit_result_t hr = picturebox_hit_test(&pb, 10.0f, 10.0f);
    assert(hr.region == PICTUREBOX_HIT_BACKGROUND);
    picturebox_destroy(&pb);
}

TEST(test_hit_test_none) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    /* Point completely outside the box */
    picturebox_hit_result_t hr = picturebox_hit_test(&pb, -10.0f, -10.0f);
    assert(hr.region == PICTUREBOX_HIT_NONE);
    hr = picturebox_hit_test(&pb, 500.0f, 400.0f);
    assert(hr.region == PICTUREBOX_HIT_NONE);
    picturebox_destroy(&pb);
}

TEST(test_hit_test_handles) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_resize(&pb, 1);
    picturebox_set_show_handles(&pb, 1);
    picturebox_layout(&pb);

    /* Top-left corner = NW handle */
    picturebox_hit_result_t hr = picturebox_hit_test(&pb, 0.0f, 0.0f);
    assert(hr.region == PICTUREBOX_HIT_HANDLE_NW);

    /* Bottom-right corner = SE handle */
    hr = picturebox_hit_test(&pb, 400.0f, 300.0f);
    assert(hr.region == PICTUREBOX_HIT_HANDLE_SE);

    /* Top-right = NE */
    hr = picturebox_hit_test(&pb, 400.0f, 0.0f);
    assert(hr.region == PICTUREBOX_HIT_HANDLE_NE);

    /* Bottom-left = SW */
    hr = picturebox_hit_test(&pb, 0.0f, 300.0f);
    assert(hr.region == PICTUREBOX_HIT_HANDLE_SW);
    picturebox_destroy(&pb);
}

TEST(test_hit_test_handles_disabled) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_interactive_resize(&pb, 0);
    picturebox_layout(&pb);

    /* Even at corners, should be IMAGE not a handle */
    picturebox_hit_result_t hr = picturebox_hit_test(&pb, 2.0f, 2.0f);
    assert(hr.region != PICTUREBOX_HIT_HANDLE_NW);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Callbacks
 * ═══════════════════════════════════════════════════════════════ */

static int g_frame_cb_called = 0;
static int g_frame_cb_index = -1;
static void test_frame_cb(void *pb, int idx, void *data) {
    (void)pb; (void)data;
    g_frame_cb_called = 1;
    g_frame_cb_index = idx;
}

TEST(test_frame_callback) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 100.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 100.0f);
    picturebox_set_loop_mode(&pb, PICTUREBOX_LOOP_FOREVER, 0);
    picturebox_set_frame_callback(&pb, test_frame_cb, NULL);
    picturebox_play(&pb);

    g_frame_cb_called = 0;
    g_frame_cb_index = -1;
    picturebox_tick(&pb, 150.0f);
    assert(g_frame_cb_called == 1);
    assert(g_frame_cb_index == 1);
    picturebox_destroy(&pb);
}

static int g_transform_cb_called = 0;
static void test_transform_cb(void *pb, void *data) {
    (void)pb; (void)data;
    g_transform_cb_called = 1;
}

TEST(test_transform_callback_zoom) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_zoom(&pb, 1);
    picturebox_set_transform_callback(&pb, test_transform_cb, NULL);
    g_transform_cb_called = 0;
    picturebox_handle_mouse_wheel(&pb, 0, 1.0f, 200.0f, 150.0f);
    assert(g_transform_cb_called == 1);
    picturebox_destroy(&pb);
}

static int g_click_cb_called = 0;
static float g_click_cb_ix = -1, g_click_cb_iy = -1;
static int g_click_cb_button = -1, g_click_cb_clicks = -1;
static void test_click_cb(void *pb, float ix, float iy,
                           int button, int clicks, void *data) {
    (void)pb; (void)data;
    g_click_cb_called = 1;
    g_click_cb_ix = ix;
    g_click_cb_iy = iy;
    g_click_cb_button = button;
    g_click_cb_clicks = clicks;
}

TEST(test_click_callback) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_click_callback(&pb, test_click_cb, NULL);
    picturebox_layout(&pb);

    g_click_cb_called = 0;
    picturebox_handle_mouse_button_down(&pb, 200.0f, 150.0f, 1, 1);
    assert(g_click_cb_called == 1);
    assert(g_click_cb_button == 1);
    assert(g_click_cb_clicks == 1);
    picturebox_destroy(&pb);
}

static int g_move_cb_called = 0;
static void test_move_cb(void *pb, float nx, float ny, void *data) {
    (void)pb; (void)data; (void)nx; (void)ny;
    g_move_cb_called = 1;
}

TEST(test_move_callback) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_interactive_move(&pb, 1);
    picturebox_set_move_callback(&pb, test_move_cb, NULL);
    picturebox_layout(&pb);

    g_move_cb_called = 0;
    /* Start drag */
    picturebox_handle_mouse_button_down(&pb, 200.0f, 150.0f, 1, 1);
    /* Move */
    picturebox_handle_mouse_move(&pb, 210.0f, 160.0f, 10.0f, 10.0f, 1);
    assert(g_move_cb_called == 1);
    picturebox_handle_mouse_button_up(&pb, 210.0f, 160.0f, 1, 1);
    picturebox_destroy(&pb);
}

static int g_resize_cb_called = 0;
static void test_resize_cb(void *pb, float nw, float nh, void *data) {
    (void)pb; (void)nw; (void)nh; (void)data;
    g_resize_cb_called = 1;
}

TEST(test_resize_callback) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_resize(&pb, 1);
    picturebox_set_show_handles(&pb, 1);
    picturebox_set_resize_callback(&pb, test_resize_cb, NULL);
    picturebox_layout(&pb);

    g_resize_cb_called = 0;
    /* Click on SE handle (400, 300) */
    picturebox_handle_mouse_button_down(&pb, 400.0f, 300.0f, 1, 1);
    assert(pb.drag_mode == PICTUREBOX_DRAG_RESIZE);
    /* Drag */
    picturebox_handle_mouse_move(&pb, 420.0f, 320.0f, 20.0f, 20.0f, 1);
    assert(g_resize_cb_called == 1);
    picturebox_handle_mouse_button_up(&pb, 420.0f, 320.0f, 1, 1);
    picturebox_destroy(&pb);
}

static int g_anim_done_called = 0;
static int g_anim_done_loops = -1;
static void test_anim_done_cb(void *pb, int loops, void *data) {
    (void)pb; (void)data;
    g_anim_done_called = 1;
    g_anim_done_loops = loops;
}

TEST(test_anim_done_callback) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_add_frame(&pb, &g_dummy_frame_0, 50.0f);
    picturebox_add_frame(&pb, &g_dummy_frame_1, 50.0f);
    picturebox_set_loop_mode(&pb, PICTUREBOX_LOOP_NONE, 0);
    picturebox_set_anim_done_callback(&pb, test_anim_done_cb, NULL);
    picturebox_play(&pb);

    g_anim_done_called = 0;
    g_anim_done_loops = -1;
    picturebox_tick(&pb, 200.0f);
    assert(g_anim_done_called == 1);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Mouse Events
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_mouse_wheel_zoom) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_zoom(&pb, 1);
    picturebox_layout(&pb);

    float z0 = picturebox_get_zoom(&pb);
    picturebox_event_result_t r = picturebox_handle_mouse_wheel(
        &pb, 0, 1.0f, 200.0f, 150.0f);
    assert(r.transform_changed == 1);
    assert(r.redraw == 1);
    assert(picturebox_get_zoom(&pb) > z0);
    picturebox_destroy(&pb);
}

TEST(test_mouse_wheel_zoom_disabled) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_zoom(&pb, 0);
    picturebox_layout(&pb);

    float z0 = picturebox_get_zoom(&pb);
    picturebox_event_result_t r = picturebox_handle_mouse_wheel(
        &pb, 0, 1.0f, 200.0f, 150.0f);
    assert(r.transform_changed == 0);
    assert(NEAR(picturebox_get_zoom(&pb), z0));
    picturebox_destroy(&pb);
}

TEST(test_mouse_pan) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_interactive_pan(&pb, 1);
    picturebox_layout(&pb);

    picturebox_handle_mouse_button_down(&pb, 200.0f, 150.0f, 1, 1);
    assert(pb.drag_mode == PICTUREBOX_DRAG_PAN);

    picturebox_event_result_t r = picturebox_handle_mouse_move(
        &pb, 220.0f, 170.0f, 20.0f, 20.0f, 1);
    assert(r.transform_changed == 1);
    assert(r.redraw == 1);

    float px, py;
    picturebox_get_pan(&pb, &px, &py);
    assert(px != 0.0f || py != 0.0f);

    picturebox_handle_mouse_button_up(&pb, 220.0f, 170.0f, 1, 1);
    assert(pb.drag_mode == PICTUREBOX_DRAG_NONE);
    picturebox_destroy(&pb);
}

TEST(test_mouse_move_box) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_interactive_move(&pb, 1);
    picturebox_layout(&pb);

    float x0 = picturebox_get_x(&pb);
    float y0 = picturebox_get_y(&pb);

    picturebox_handle_mouse_button_down(&pb, 200.0f, 150.0f, 1, 1);
    assert(pb.drag_mode == PICTUREBOX_DRAG_MOVE);

    picturebox_event_result_t r = picturebox_handle_mouse_move(
        &pb, 230.0f, 180.0f, 30.0f, 30.0f, 1);
    assert(r.moved == 1);
    assert(r.redraw == 1);
    assert(picturebox_get_x(&pb) != x0 || picturebox_get_y(&pb) != y0);

    picturebox_handle_mouse_button_up(&pb, 230.0f, 180.0f, 1, 1);
    picturebox_destroy(&pb);
}

TEST(test_mouse_resize) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_resize(&pb, 1);
    picturebox_set_show_handles(&pb, 1);
    picturebox_layout(&pb);

    float w0 = picturebox_get_width(&pb);
    /* Click SE handle */
    picturebox_handle_mouse_button_down(&pb, 400.0f, 300.0f, 1, 1);
    assert(pb.drag_mode == PICTUREBOX_DRAG_RESIZE);

    picturebox_event_result_t r = picturebox_handle_mouse_move(
        &pb, 450.0f, 350.0f, 50.0f, 50.0f, 1);
    assert(r.size_changed == 1);
    assert(picturebox_get_width(&pb) > w0);

    picturebox_handle_mouse_button_up(&pb, 450.0f, 350.0f, 1, 1);
    picturebox_destroy(&pb);
}

TEST(test_mouse_double_click_toggle_zoom) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_layout(&pb);

    picturebox_event_result_t r = picturebox_handle_mouse_button_down(
        &pb, 200.0f, 150.0f, 1, 2);
    assert(r.transform_changed == 1);
    assert(NEAR(picturebox_get_zoom(&pb), 2.0f));

    /* Double click again resets */
    r = picturebox_handle_mouse_button_down(&pb, 200.0f, 150.0f, 1, 2);
    assert(r.transform_changed == 1);
    assert(NEAR(picturebox_get_zoom(&pb), 1.0f));
    picturebox_destroy(&pb);
}

TEST(test_mouse_right_click_ignored) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_interactive_pan(&pb, 1);
    picturebox_layout(&pb);

    picturebox_event_result_t r = picturebox_handle_mouse_button_down(
        &pb, 200.0f, 150.0f, 3, 1); /* Right button */
    assert(pb.drag_mode == PICTUREBOX_DRAG_NONE);
    (void)r;
    picturebox_destroy(&pb);
}

TEST(test_mouse_enter_leave) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_event_result_t r = picturebox_handle_mouse_enter(&pb, 100, 100);
    assert(r.redraw == 1);
    assert(pb.hovered == 1);
    r = picturebox_handle_mouse_leave(&pb, 100, 100);
    assert(r.redraw == 1);
    assert(pb.hovered == 0);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Touch Events
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_touch_pan) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_interactive_pan(&pb, 1);
    picturebox_layout(&pb);

    picturebox_handle_touch_down(&pb, 0, 200.0f, 150.0f, 1.0f);
    assert(pb.drag_mode == PICTUREBOX_DRAG_PAN);
    picturebox_event_result_t r = picturebox_handle_touch_move(
        &pb, 0, 210.0f, 160.0f, 1.0f);
    assert(r.transform_changed == 1);
    picturebox_handle_touch_up(&pb, 0, 210.0f, 160.0f, 1.0f);
    assert(pb.drag_mode == PICTUREBOX_DRAG_NONE);
    picturebox_destroy(&pb);
}

TEST(test_touch_cancel) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_interactive_pan(&pb, 1);
    picturebox_layout(&pb);

    picturebox_handle_touch_down(&pb, 0, 200.0f, 150.0f, 1.0f);
    assert(pb.drag_mode == PICTUREBOX_DRAG_PAN);
    picturebox_event_result_t r = picturebox_handle_touch_cancel(&pb, 0);
    assert(r.redraw == 1);
    assert(pb.drag_mode == PICTUREBOX_DRAG_NONE);
    picturebox_destroy(&pb);
}

TEST(test_touch_nonzero_finger_ignored) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_interactive_pan(&pb, 1);
    picturebox_layout(&pb);

    picturebox_event_result_t r = picturebox_handle_touch_down(
        &pb, 1, 200.0f, 150.0f, 1.0f);
    assert(pb.drag_mode == PICTUREBOX_DRAG_NONE);
    (void)r;
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Keyboard Events
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_key_pan) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_pan(&pb, 1);
    picturebox_layout(&pb);

    float px0, py0;
    picturebox_get_pan(&pb, &px0, &py0);
    picturebox_event_result_t r = picturebox_handle_key_down(
        &pb, 0, 0x4F, 0, 0); /* Right arrow */
    assert(r.transform_changed == 1);
    float px1, py1;
    picturebox_get_pan(&pb, &px1, &py1);
    assert(px1 > px0);
    (void)py0; (void)py1;
    picturebox_destroy(&pb);
}

TEST(test_key_zoom) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_zoom(&pb, 1);
    picturebox_layout(&pb);

    float z0 = picturebox_get_zoom(&pb);
    picturebox_handle_key_down(&pb, 0, '+', 0, 0);
    assert(picturebox_get_zoom(&pb) > z0);

    float z1 = picturebox_get_zoom(&pb);
    picturebox_handle_key_down(&pb, 0, '-', 0, 0);
    assert(picturebox_get_zoom(&pb) < z1);
    picturebox_destroy(&pb);
}

TEST(test_key_reset) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_zoom(&pb, 1);
    picturebox_set_zoom(&pb, 3.0f);
    picturebox_set_pan(&pb, 50, 50);
    picturebox_handle_key_down(&pb, 0, '0', 0, 0);
    assert(NEAR(picturebox_get_zoom(&pb), 1.0f));
    float px, py;
    picturebox_get_pan(&pb, &px, &py);
    assert(NEAR(px, 0.0f));
    assert(NEAR(py, 0.0f));
    picturebox_destroy(&pb);
}

TEST(test_key_up_noop) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_event_result_t r = picturebox_handle_key_up(&pb, 0, 0x50, 0);
    assert(r.redraw == 0);
    picturebox_destroy(&pb);
}

TEST(test_key_pan_disabled) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_pan(&pb, 0);

    float px0, py0;
    picturebox_get_pan(&pb, &px0, &py0);
    picturebox_handle_key_down(&pb, 0, 0x4F, 0, 0); /* Right arrow */
    float px1, py1;
    picturebox_get_pan(&pb, &px1, &py1);
    assert(NEAR(px0, px1));
    (void)py0; (void)py1;
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Focus Events
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_focus) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_event_result_t r = picturebox_handle_focus_gained(&pb);
    assert(r.redraw == 1);
    assert(pb.focused == 1);
    r = picturebox_handle_focus_lost(&pb);
    assert(r.redraw == 1);
    assert(pb.focused == 0);
    picturebox_destroy(&pb);
}

TEST(test_focus_lost_cancels_drag) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_interactive_pan(&pb, 1);
    picturebox_layout(&pb);

    picturebox_handle_mouse_button_down(&pb, 200.0f, 150.0f, 1, 1);
    assert(pb.drag_mode == PICTUREBOX_DRAG_PAN);
    picturebox_handle_focus_lost(&pb);
    assert(pb.drag_mode == PICTUREBOX_DRAG_NONE);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Resize Event
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_resize) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_event_result_t r = picturebox_handle_resize(
        &pb, 400, 300, 600, 500);
    assert(r.size_changed == 1);
    assert(r.redraw == 1);
    assert(NEAR(picturebox_get_width(&pb), 600.0f));
    assert(NEAR(picturebox_get_height(&pb), 500.0f));
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_layout_zero_source) {
    picturebox_t pb;
    make_pb(&pb, 400, 300);
    picturebox_set_source(&pb, &g_dummy_image_a, 0, 0);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    assert(r.w == 0.0f && r.h == 0.0f);
    picturebox_destroy(&pb);
}

TEST(test_layout_zero_box) {
    picturebox_t pb;
    picturebox_create(&pb, 0, 0);
    picturebox_set_source(&pb, &g_dummy_image_a, 800, 600);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    assert(r.w == 0.0f && r.h == 0.0f);
    picturebox_destroy(&pb);
}

TEST(test_event_result_none) {
    picturebox_event_result_t r = PICTUREBOX_EVENT_NONE;
    assert(r.redraw == 0);
    assert(r.frame_changed == 0);
    assert(r.transform_changed == 0);
    assert(r.size_changed == 0);
    assert(r.moved == 0);
    assert(r.cursor_changed == 0);
}

TEST(test_position_offset_layout) {
    picturebox_t pb;
    make_pb_with_source(&pb, 200, 200, 200, 200);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_set_position(&pb, 100.0f, 50.0f);
    picturebox_layout(&pb);
    picturebox_rect_t r = picturebox_get_dst_rect(&pb);
    assert(NEAR(r.x, 100.0f));
    assert(NEAR(r.y, 50.0f));
    picturebox_destroy(&pb);
}

TEST(test_pan_offset_in_dst_rect) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 400, 300);
    picturebox_set_fit(&pb, PICTUREBOX_FIT_FILL);
    picturebox_layout(&pb);
    picturebox_rect_t r0 = picturebox_get_dst_rect(&pb);

    picturebox_set_pan(&pb, 20.0f, 10.0f);
    picturebox_layout(&pb);
    picturebox_rect_t r1 = picturebox_get_dst_rect(&pb);
    assert(NEAR(r1.x, r0.x + 20.0f));
    assert(NEAR(r1.y, r0.y + 10.0f));
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Stress
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_stress_many_frames) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 256, 256);
    for (int i = 0; i < 1000; i++) {
        int ok = picturebox_add_frame(&pb, NULL, 16.0f);
        assert(ok == 1);
    }
    assert(picturebox_get_frame_count(&pb) == 1000);
    assert(NEAR(picturebox_get_total_duration_ms(&pb), 16000.0f));
    picturebox_destroy(&pb);
}

TEST(test_stress_rapid_layout) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    for (int i = 0; i < 1000; i++) {
        picturebox_set_zoom(&pb, 1.0f + (float)(i % 10) * 0.1f);
        picturebox_layout(&pb);
    }
    assert(pb.layout_dirty == 0);
    picturebox_destroy(&pb);
}

TEST(test_stress_rapid_tick) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 64, 64);
    for (int i = 0; i < 60; i++)
        picturebox_add_frame(&pb, NULL, 16.67f);
    picturebox_set_loop_mode(&pb, PICTUREBOX_LOOP_FOREVER, 0);
    picturebox_play(&pb);
    /* Simulate 10 seconds at ~60fps */
    for (int i = 0; i < 600; i++) {
        picturebox_tick(&pb, 16.67f);
    }
    assert(picturebox_get_anim_state(&pb) == PICTUREBOX_ANIM_PLAYING);
    assert(picturebox_get_loops_done(&pb) >= 9);
    picturebox_destroy(&pb);
}

TEST(test_stress_resize_with_aspect_lock) {
    picturebox_t pb;
    make_pb_with_source(&pb, 400, 300, 800, 600);
    picturebox_set_interactive_resize(&pb, 1);
    picturebox_set_show_handles(&pb, 1);
    picturebox_set_lock_aspect(&pb, 1);
    picturebox_layout(&pb);

    /* Simulate dragging SE handle */
    picturebox_handle_mouse_button_down(&pb, 400.0f, 300.0f, 1, 1);
    for (int i = 0; i < 50; i++) {
        float x = 400.0f + (float)i * 2.0f;
        float y = 300.0f + (float)i * 3.0f;
        picturebox_handle_mouse_move(&pb, x, y, 2.0f, 3.0f, 1);
    }
    picturebox_handle_mouse_button_up(&pb, 500.0f, 450.0f, 1, 1);
    picturebox_destroy(&pb);
}

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
    int test_count = 0;
    printf("picturebox.h tests:\n");

    /* Lifecycle */
    printf("\n  -- Lifecycle --\n");
    RUN(test_create_destroy); test_count++;
    RUN(test_create_defaults); test_count++;
    RUN(test_destroy_null_safe); test_count++;
    RUN(test_destroy_zeroed); test_count++;

    /* Source */
    printf("\n  -- Source --\n");
    RUN(test_set_source); test_count++;
    RUN(test_set_source_replaces); test_count++;
    RUN(test_clear_source); test_count++;
    RUN(test_get_current_source_static); test_count++;
    RUN(test_get_current_source_animated); test_count++;
    RUN(test_get_current_source_no_source); test_count++;

    /* Geometry */
    printf("\n  -- Geometry --\n");
    RUN(test_set_position); test_count++;
    RUN(test_set_size); test_count++;
    RUN(test_set_size_clamped); test_count++;
    RUN(test_set_padding); test_count++;

    /* Fit & Alignment */
    printf("\n  -- Fit & Alignment --\n");
    RUN(test_set_fit); test_count++;
    RUN(test_set_align); test_count++;
    RUN(test_fit_contain_landscape_in_square); test_count++;
    RUN(test_fit_contain_portrait_in_square); test_count++;
    RUN(test_fit_cover); test_count++;
    RUN(test_fit_fill); test_count++;
    RUN(test_fit_none); test_count++;
    RUN(test_fit_scale_down_small_image); test_count++;
    RUN(test_fit_scale_down_large_image); test_count++;
    RUN(test_align_top_left); test_count++;
    RUN(test_align_bottom_right); test_count++;
    RUN(test_align_center); test_count++;

    /* Transform */
    printf("\n  -- Transform --\n");
    RUN(test_set_zoom); test_count++;
    RUN(test_zoom_clamp); test_count++;
    RUN(test_zoom_by); test_count++;
    RUN(test_zoom_by_noop_at_limit); test_count++;
    RUN(test_set_pan); test_count++;
    RUN(test_pan_by); test_count++;
    RUN(test_get_pan_null_out); test_count++;
    RUN(test_set_rotation); test_count++;
    RUN(test_rotate_by); test_count++;
    RUN(test_set_flip); test_count++;
    RUN(test_reset_transform); test_count++;
    RUN(test_zoom_to_fit); test_count++;
    RUN(test_zoom_affects_dst_rect); test_count++;

    /* Layout */
    printf("\n  -- Layout --\n");
    RUN(test_layout_clears_dirty); test_count++;
    RUN(test_layout_no_source); test_count++;
    RUN(test_layout_with_padding); test_count++;
    RUN(test_get_dst_rect_lazy); test_count++;
    RUN(test_is_dirty); test_count++;
    RUN(test_get_source_rect_static); test_count++;
    RUN(test_get_source_rect_frame_subrect); test_count++;

    /* Coordinate Mapping */
    printf("\n  -- Coordinate Mapping --\n");
    RUN(test_box_to_image); test_count++;
    RUN(test_box_to_image_no_source); test_count++;
    RUN(test_image_to_box); test_count++;
    RUN(test_box_to_image_flipped); test_count++;
    RUN(test_roundtrip_mapping); test_count++;

    /* Animation — Frame Management */
    printf("\n  -- Animation: Frame Management --\n");
    RUN(test_add_frame); test_count++;
    RUN(test_add_frame_default_duration); test_count++;
    RUN(test_add_multiple_frames); test_count++;
    RUN(test_add_frame_ex); test_count++;
    RUN(test_clear_frames); test_count++;
    RUN(test_get_frame_bounds); test_count++;
    RUN(test_set_frame_duration); test_count++;
    RUN(test_set_frame_duration_bounds); test_count++;
    RUN(test_total_duration); test_count++;
    RUN(test_is_animated); test_count++;

    /* Animation — Playback */
    printf("\n  -- Animation: Playback --\n");
    RUN(test_play_pause_stop); test_count++;
    RUN(test_play_requires_two_frames); test_count++;
    RUN(test_pause_only_when_playing); test_count++;
    RUN(test_seek_frame); test_count++;
    RUN(test_seek_frame_clamp); test_count++;
    RUN(test_seek_ms); test_count++;
    RUN(test_seek_empty); test_count++;

    /* Animation — Tick */
    printf("\n  -- Animation: Tick --\n");
    RUN(test_tick_advances_frame); test_count++;
    RUN(test_tick_noop_when_stopped); test_count++;
    RUN(test_tick_noop_when_paused); test_count++;
    RUN(test_tick_loop_none_stops); test_count++;
    RUN(test_tick_loop_forever); test_count++;
    RUN(test_tick_loop_ping_pong); test_count++;
    RUN(test_tick_loop_count); test_count++;
    RUN(test_tick_large_dt); test_count++;

    /* Interaction Config */
    printf("\n  -- Interaction Config --\n");
    RUN(test_interaction_flags); test_count++;
    RUN(test_size_constraints); test_count++;
    RUN(test_lock_aspect); test_count++;

    /* Appearance */
    printf("\n  -- Appearance --\n");
    RUN(test_set_bg_color); test_count++;
    RUN(test_set_checkerboard); test_count++;
    RUN(test_set_border); test_count++;

    /* Hit Testing */
    printf("\n  -- Hit Testing --\n");
    RUN(test_hit_test_image); test_count++;
    RUN(test_hit_test_background); test_count++;
    RUN(test_hit_test_none); test_count++;
    RUN(test_hit_test_handles); test_count++;
    RUN(test_hit_test_handles_disabled); test_count++;

    /* Callbacks */
    printf("\n  -- Callbacks --\n");
    RUN(test_frame_callback); test_count++;
    RUN(test_transform_callback_zoom); test_count++;
    RUN(test_click_callback); test_count++;
    RUN(test_move_callback); test_count++;
    RUN(test_resize_callback); test_count++;
    RUN(test_anim_done_callback); test_count++;

    /* Mouse Events */
    printf("\n  -- Mouse Events --\n");
    RUN(test_mouse_wheel_zoom); test_count++;
    RUN(test_mouse_wheel_zoom_disabled); test_count++;
    RUN(test_mouse_pan); test_count++;
    RUN(test_mouse_move_box); test_count++;
    RUN(test_mouse_resize); test_count++;
    RUN(test_mouse_double_click_toggle_zoom); test_count++;
    RUN(test_mouse_right_click_ignored); test_count++;
    RUN(test_mouse_enter_leave); test_count++;

    /* Touch Events */
    printf("\n  -- Touch Events --\n");
    RUN(test_touch_pan); test_count++;
    RUN(test_touch_cancel); test_count++;
    RUN(test_touch_nonzero_finger_ignored); test_count++;

    /* Keyboard Events */
    printf("\n  -- Keyboard Events --\n");
    RUN(test_key_pan); test_count++;
    RUN(test_key_zoom); test_count++;
    RUN(test_key_reset); test_count++;
    RUN(test_key_up_noop); test_count++;
    RUN(test_key_pan_disabled); test_count++;

    /* Focus Events */
    printf("\n  -- Focus Events --\n");
    RUN(test_focus); test_count++;
    RUN(test_focus_lost_cancels_drag); test_count++;

    /* Resize Event */
    printf("\n  -- Resize Event --\n");
    RUN(test_handle_resize); test_count++;

    /* Edge Cases */
    printf("\n  -- Edge Cases --\n");
    RUN(test_layout_zero_source); test_count++;
    RUN(test_layout_zero_box); test_count++;
    RUN(test_event_result_none); test_count++;
    RUN(test_position_offset_layout); test_count++;
    RUN(test_pan_offset_in_dst_rect); test_count++;

    /* Stress */
    printf("\n  -- Stress --\n");
    RUN(test_stress_many_frames); test_count++;
    RUN(test_stress_rapid_layout); test_count++;
    RUN(test_stress_rapid_tick); test_count++;
    RUN(test_stress_resize_with_aspect_lock); test_count++;

    printf("\nAll %d tests passed.\n", test_count);
    return 0;
}