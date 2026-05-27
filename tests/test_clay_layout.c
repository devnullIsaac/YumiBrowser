/*
 * test_clay_layout.c - Tests for clay_layout.h: the platform-independent layout tree (construction, sizing, alignment, traversal).
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
 * @file test_clay_layout.c
 * @brief Tests for clay_layout.h — platform-independent layout tree.
 */
#define CLAY_LAYOUT_IMPLEMENTATION
#include "clay_layout.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-60s", #name); name(); printf("PASS\n"); } while(0)

/* Fake widget pointers for testing (never dereferenced). */
#define W(n) ((void *)(uintptr_t)(0x1000 + (n)))

/* ═══════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_init_destroy) {
    cl_tree_t t;
    cl_tree_init(&t);
    assert(t.node_count == 1);
    assert(t.nodes[0].alive);
    assert(t.nodes[0].type == CL_NODE_COL);
    assert(cl_child_count(&t, 0) == 0);
    cl_tree_destroy(&t);
    assert(t.node_count == 0);
}

TEST(test_init_state_zeroed) {
    cl_tree_t t;
    cl_tree_init(&t);
    assert(t.focused == NULL);
    assert(t.hovered == NULL);
    assert(t.captured == NULL);
    assert(t.maximized == NULL);
    assert(t.rect_count == 0);
    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Build Phase
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_build_simple_row) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t r = cl_add_row(&t, 0, NULL);
    assert(r != CL_NODE_INVALID);
    assert(cl_get_type(&t, r) == CL_NODE_ROW);
    assert(cl_get_parent(&t, r) == 0);
    assert(cl_child_count(&t, 0) == 1);
    cl_end(&t);
    assert(!t.building);
    cl_tree_destroy(&t);
}

TEST(test_build_nested) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);

    cl_node_t col = cl_add_col(&t, 0, NULL);
    cl_node_t w1  = cl_add_widget(&t, col, W(1), NULL);
    cl_node_t w2  = cl_add_widget(&t, col, W(2), NULL);
    cl_node_t sp  = cl_add_spacer(&t, col, NULL);

    assert(cl_child_count(&t, col) == 3);
    assert(cl_child_at(&t, col, 0) == w1);
    assert(cl_child_at(&t, col, 1) == w2);
    assert(cl_child_at(&t, col, 2) == sp);
    assert(cl_get_widget(&t, w1) == W(1));
    assert(cl_get_widget(&t, w2) == W(2));
    assert(cl_get_type(&t, sp) == CL_NODE_SPACER);

    cl_end(&t);
    cl_tree_destroy(&t);
}

TEST(test_build_clears_previous) {
    cl_tree_t t;
    cl_tree_init(&t);

    cl_begin(&t);
    cl_add_widget(&t, 0, W(1), NULL);
    cl_add_widget(&t, 0, W(2), NULL);
    cl_end(&t);
    assert(cl_child_count(&t, 0) == 2);

    cl_begin(&t);
    cl_add_widget(&t, 0, W(3), NULL);
    cl_end(&t);
    assert(cl_child_count(&t, 0) == 1);
    assert(cl_get_widget(&t, cl_child_at(&t, 0, 0)) == W(3));

    cl_tree_destroy(&t);
}

TEST(test_build_with_style) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);

    cl_style_t s = {
        .sizing_w = { .type = CL_SIZE_FIXED, .min = 100, .max = 100 },
        .sizing_h = { .type = CL_SIZE_GROW },
        .child_gap = 4,
        .bg_color = { 1.0f, 0.0f, 0.0f, 1.0f },
    };
    cl_node_t n = cl_add_row(&t, 0, &s);
    const cl_style_t *got = cl_get_style(&t, n);
    assert(got != NULL);
    assert(got->sizing_w.type == CL_SIZE_FIXED);
    assert(got->sizing_w.min == 100.0f);
    assert(got->child_gap == 4);
    assert(got->bg_color.r == 1.0f);

    cl_end(&t);
    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Node Query
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_is_alive_visible) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);

    cl_node_t n = cl_add_widget(&t, 0, W(1), NULL);
    assert(cl_is_alive(&t, n));
    assert(cl_is_visible(&t, n));

    cl_set_visible(&t, n, false);
    assert(cl_is_alive(&t, n));
    assert(!cl_is_visible(&t, n));

    cl_end(&t);
    cl_tree_destroy(&t);
}

TEST(test_first_child_next_sibling) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);

    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_node_t b = cl_add_widget(&t, 0, W(2), NULL);
    cl_node_t c = cl_add_widget(&t, 0, W(3), NULL);

    assert(cl_first_child(&t, 0) == a);
    assert(cl_next_sibling(&t, a) == b);
    assert(cl_next_sibling(&t, b) == c);
    assert(cl_next_sibling(&t, c) == 0);

    cl_end(&t);
    cl_tree_destroy(&t);
}

TEST(test_query_invalid_node) {
    cl_tree_t t;
    cl_tree_init(&t);
    assert(!cl_is_alive(&t, 9999));
    assert(!cl_is_visible(&t, 9999));
    assert(cl_get_widget(&t, 9999) == NULL);
    assert(cl_get_style(&t, 9999) == NULL);
    assert(cl_get_parent(&t, 9999) == 0);
    assert(cl_child_count(&t, 9999) == 0);
    assert(cl_child_at(&t, 9999, 0) == 0);
    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Live Mutation
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_insert_at_position) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_node_t c = cl_add_widget(&t, 0, W(3), NULL);
    cl_end(&t);
    assert(cl_child_count(&t, 0) == 2);

    /* Insert B between A and C at position 1 */
    cl_node_t b = cl_insert_widget(&t, 0, 1, W(2), NULL);
    assert(cl_child_count(&t, 0) == 3);
    assert(cl_child_at(&t, 0, 0) == a);
    assert(cl_child_at(&t, 0, 1) == b);
    assert(cl_child_at(&t, 0, 2) == c);

    cl_tree_destroy(&t);
}

TEST(test_insert_row_col_spacer) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_end(&t);

    cl_node_t r = cl_insert_row(&t, 0, 0, NULL);
    cl_node_t c = cl_insert_col(&t, 0, 1, NULL);
    cl_node_t s = cl_insert_spacer(&t, 0, 2, NULL);

    assert(cl_get_type(&t, r) == CL_NODE_ROW);
    assert(cl_get_type(&t, c) == CL_NODE_COL);
    assert(cl_get_type(&t, s) == CL_NODE_SPACER);
    assert(cl_child_count(&t, 0) == 3);

    cl_tree_destroy(&t);
}

TEST(test_remove_node) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_node_t b = cl_add_widget(&t, 0, W(2), NULL);
    cl_node_t c = cl_add_widget(&t, 0, W(3), NULL);
    cl_end(&t);

    cl_remove(&t, b);
    assert(cl_child_count(&t, 0) == 2);
    assert(cl_child_at(&t, 0, 0) == a);
    assert(cl_child_at(&t, 0, 1) == c);
    assert(!cl_is_alive(&t, b));

    cl_tree_destroy(&t);
}

TEST(test_remove_subtree) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t col = cl_add_col(&t, 0, NULL);
    cl_node_t w1  = cl_add_widget(&t, col, W(1), NULL);
    cl_node_t w2  = cl_add_widget(&t, col, W(2), NULL);
    cl_end(&t);

    cl_remove(&t, col);
    assert(cl_child_count(&t, 0) == 0);
    assert(!cl_is_alive(&t, col));
    assert(!cl_is_alive(&t, w1));
    assert(!cl_is_alive(&t, w2));

    cl_tree_destroy(&t);
}

TEST(test_remove_invalid) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_remove(&t, 0);      /* root — should not crash */
    cl_remove(&t, 9999);   /* out of range */
    cl_tree_destroy(&t);
}

TEST(test_reparent) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t row1 = cl_add_row(&t, 0, NULL);
    cl_node_t row2 = cl_add_row(&t, 0, NULL);
    cl_node_t w    = cl_add_widget(&t, row1, W(1), NULL);
    cl_end(&t);

    assert(cl_child_count(&t, row1) == 1);
    assert(cl_child_count(&t, row2) == 0);

    cl_reparent(&t, w, row2, 0);
    assert(cl_child_count(&t, row1) == 0);
    assert(cl_child_count(&t, row2) == 1);
    assert(cl_get_parent(&t, w) == row2);

    cl_tree_destroy(&t);
}

TEST(test_set_style_live) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t n = cl_add_row(&t, 0, NULL);
    cl_end(&t);

    cl_style_t s = { .child_gap = 42 };
    cl_set_style(&t, n, &s);
    assert(cl_get_style(&t, n)->child_gap == 42);

    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Rect Cache
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_rects_add_clear) {
    cl_tree_t t;
    cl_tree_init(&t);

    assert(cl_rect_count(&t) == 0);
    int i = cl_rects_add(&t, W(1), 10, 20, 100, 50);
    assert(i == 0);
    assert(cl_rect_count(&t) == 1);

    const cl_rect_t *r = cl_rect_at(&t, 0);
    assert(r != NULL);
    assert(r->widget == W(1));
    assert(r->x == 10.0f && r->y == 20.0f);
    assert(r->w == 100.0f && r->h == 50.0f);

    cl_rects_clear(&t);
    assert(cl_rect_count(&t) == 0);
    assert(cl_rect_at(&t, 0) == NULL);

    cl_tree_destroy(&t);
}

TEST(test_rects_multiple) {
    cl_tree_t t;
    cl_tree_init(&t);

    cl_rects_add(&t, W(1), 0, 0, 50, 50);
    cl_rects_add(&t, W(2), 50, 0, 50, 50);
    cl_rects_add(&t, W(3), 0, 50, 50, 50);

    assert(cl_rect_count(&t) == 3);
    assert(cl_rect_at(&t, 1)->widget == W(2));
    assert(cl_rect_at(&t, 2)->x == 0.0f);
    assert(cl_rect_at(&t, 2)->y == 50.0f);

    cl_tree_destroy(&t);
}

TEST(test_rect_at_invalid) {
    cl_tree_t t;
    cl_tree_init(&t);
    assert(cl_rect_at(&t, -1) == NULL);
    assert(cl_rect_at(&t, 0) == NULL);
    assert(cl_rect_at(&t, 999) == NULL);
    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Hit Test
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_point_in_rect) {
    assert(cl_point_in_rect(10, 10, 0, 0, 20, 20));
    assert(!cl_point_in_rect(-1, 10, 0, 0, 20, 20));
    assert(!cl_point_in_rect(20, 10, 0, 0, 20, 20));  /* edge exclusive */
    assert(cl_point_in_rect(0, 0, 0, 0, 20, 20));      /* corner inclusive */
}

TEST(test_hit_test_basic) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_rects_add(&t, W(1), 0, 0, 100, 50);
    cl_rects_add(&t, W(2), 100, 0, 100, 50);

    assert(cl_hit_test(&t, 50, 25, NULL, NULL) == 0);
    assert(cl_hit_test(&t, 150, 25, NULL, NULL) == 1);
    assert(cl_hit_test(&t, 250, 25, NULL, NULL) == -1);

    cl_tree_destroy(&t);
}

TEST(test_hit_test_overlap_last_wins) {
    cl_tree_t t;
    cl_tree_init(&t);
    /* Two overlapping rects */
    cl_rects_add(&t, W(1), 0, 0, 100, 100);
    cl_rects_add(&t, W(2), 0, 0, 100, 100);

    /* Last one (index 1) wins */
    assert(cl_hit_test(&t, 50, 50, NULL, NULL) == 1);

    cl_tree_destroy(&t);
}

static bool filter_w2_only(void *widget, void *user) {
    (void)user;
    return widget == W(2);
}

TEST(test_hit_test_with_filter) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_rects_add(&t, W(1), 0, 0, 100, 100);
    cl_rects_add(&t, W(2), 0, 0, 100, 100);

    /* Filter only accepts W(2) */
    int idx = cl_hit_test(&t, 50, 50, filter_w2_only, NULL);
    assert(idx == 1);
    assert(cl_rect_at(&t, idx)->widget == W(2));

    cl_tree_destroy(&t);
}

TEST(test_hit_test_empty) {
    cl_tree_t t;
    cl_tree_init(&t);
    assert(cl_hit_test(&t, 0, 0, NULL, NULL) == -1);
    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Focus / Hover / Capture
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_focus_state) {
    cl_tree_t t;
    cl_tree_init(&t);

    assert(cl_get_focused(&t) == NULL);
    cl_set_focused(&t, W(1));
    assert(cl_get_focused(&t) == W(1));
    cl_set_focused(&t, NULL);
    assert(cl_get_focused(&t) == NULL);

    cl_tree_destroy(&t);
}

TEST(test_hover_state) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_set_hovered(&t, W(2));
    assert(cl_get_hovered(&t) == W(2));
    cl_tree_destroy(&t);
}

TEST(test_capture_state) {
    cl_tree_t t;
    cl_tree_init(&t);
    assert(!cl_has_capture(&t));
    cl_set_captured(&t, W(3));
    assert(cl_has_capture(&t));
    assert(cl_get_captured(&t) == W(3));
    cl_set_captured(&t, NULL);
    assert(!cl_has_capture(&t));
    cl_tree_destroy(&t);
}

TEST(test_maximized_state) {
    cl_tree_t t;
    cl_tree_init(&t);
    assert(cl_get_maximized(&t) == NULL);
    cl_set_maximized(&t, W(5));
    assert(cl_get_maximized(&t) == W(5));
    cl_tree_destroy(&t);
}

TEST(test_mouse_viewport) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_set_mouse(&t, 100.5f, 200.5f);
    assert(t.mouse_x == 100.5f);
    assert(t.mouse_y == 200.5f);
    cl_set_viewport(&t, 1920, 1080);
    assert(t.vp_w == 1920.0f);
    assert(t.vp_h == 1080.0f);
    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Focus Cycling
 * ═══════════════════════════════════════════════════════════════ */

static bool always_ok(void *widget, void *user) {
    (void)widget; (void)user;
    return true;
}

TEST(test_focus_next) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_rects_add(&t, W(1), 0, 0, 10, 10);
    cl_rects_add(&t, W(2), 10, 0, 10, 10);
    cl_rects_add(&t, W(3), 20, 0, 10, 10);

    cl_set_focused(&t, W(1));
    void *next = cl_focus_next(&t, always_ok, NULL);
    assert(next == W(2));

    next = cl_focus_next(&t, always_ok, NULL);
    assert(next == W(3));

    /* Wrap around */
    next = cl_focus_next(&t, always_ok, NULL);
    assert(next == W(1));

    cl_tree_destroy(&t);
}

TEST(test_focus_prev) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_rects_add(&t, W(1), 0, 0, 10, 10);
    cl_rects_add(&t, W(2), 10, 0, 10, 10);
    cl_rects_add(&t, W(3), 20, 0, 10, 10);

    cl_set_focused(&t, W(1));
    void *prev = cl_focus_prev(&t, always_ok, NULL);
    assert(prev == W(3));  /* wraps */

    prev = cl_focus_prev(&t, always_ok, NULL);
    assert(prev == W(2));

    cl_tree_destroy(&t);
}

static bool skip_w2(void *widget, void *user) {
    (void)user;
    return widget != W(2);
}

TEST(test_focus_next_with_filter) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_rects_add(&t, W(1), 0, 0, 10, 10);
    cl_rects_add(&t, W(2), 10, 0, 10, 10);
    cl_rects_add(&t, W(3), 20, 0, 10, 10);

    cl_set_focused(&t, W(1));
    void *next = cl_focus_next(&t, skip_w2, NULL);
    assert(next == W(3));  /* skips W(2) */

    cl_tree_destroy(&t);
}

TEST(test_focus_next_empty) {
    cl_tree_t t;
    cl_tree_init(&t);
    void *r = cl_focus_next(&t, always_ok, NULL);
    assert(r == NULL);
    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Tree Traversal
 * ═══════════════════════════════════════════════════════════════ */

static int walk_count;
static bool count_visitor(cl_node_t node, const cl_node_data_t *data,
                            int depth, void *user) {
    (void)node; (void)data; (void)depth; (void)user;
    walk_count++;
    return true;
}

TEST(test_walk_simple) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t col = cl_add_col(&t, 0, NULL);
    cl_add_widget(&t, col, W(1), NULL);
    cl_add_widget(&t, col, W(2), NULL);
    cl_end(&t);

    walk_count = 0;
    cl_walk(&t, count_visitor, NULL);
    assert(walk_count == 3);  /* col + 2 widgets */

    cl_tree_destroy(&t);
}

static bool skip_subtree_visitor(cl_node_t node, const cl_node_data_t *data,
                                   int depth, void *user) {
    (void)node; (void)depth; (void)user;
    walk_count++;
    return data->type != CL_NODE_COL;  /* skip col's subtree */
}

TEST(test_walk_skip_subtree) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t col = cl_add_col(&t, 0, NULL);
    cl_add_widget(&t, col, W(1), NULL);
    cl_add_widget(&t, col, W(2), NULL);
    cl_add_widget(&t, 0, W(3), NULL);  /* sibling of col */
    cl_end(&t);

    walk_count = 0;
    cl_walk(&t, skip_subtree_visitor, NULL);
    assert(walk_count == 2);  /* col (skipped), W(3) */

    cl_tree_destroy(&t);
}

TEST(test_walk_hidden_skipped) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_add_widget(&t, 0, W(2), NULL);
    cl_end(&t);

    cl_set_visible(&t, a, false);

    walk_count = 0;
    cl_walk(&t, count_visitor, NULL);
    assert(walk_count == 1);  /* only W(2) */

    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Alive Count / Collect Widgets
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_alive_count) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_add_widget(&t, 0, W(1), NULL);
    cl_add_widget(&t, 0, W(2), NULL);
    cl_add_row(&t, 0, NULL);
    cl_end(&t);

    assert(cl_alive_count(&t) == 3);

    cl_tree_destroy(&t);
}

TEST(test_collect_widgets) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t row = cl_add_row(&t, 0, NULL);
    cl_add_widget(&t, row, W(1), NULL);
    cl_add_widget(&t, row, W(2), NULL);
    cl_add_spacer(&t, row, NULL);
    cl_end(&t);

    void *out[10];
    int n = cl_collect_widgets(&t, out, 10);
    assert(n == 2);
    assert(out[0] == W(1));
    assert(out[1] == W(2));

    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Pool Reuse
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_pool_reuse_after_remove) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_end(&t);

    cl_remove(&t, a);
    assert(!cl_is_alive(&t, a));

    /* New node should reuse freed slot */
    cl_node_t b = cl_insert_widget(&t, 0, 0, W(2), NULL);
    assert(cl_is_alive(&t, b));
    assert(cl_get_widget(&t, b) == W(2));

    cl_tree_destroy(&t);
}

TEST(test_pool_reuse_after_rebuild) {
    cl_tree_t t;
    cl_tree_init(&t);

    /* First build: allocate nodes */
    cl_begin(&t);
    for (int i = 0; i < 10; i++)
        cl_add_widget(&t, 0, W(i), NULL);
    cl_end(&t);
    assert(cl_alive_count(&t) == 10);

    /* Second build: pool should reuse */
    cl_begin(&t);
    for (int i = 0; i < 5; i++)
        cl_add_widget(&t, 0, W(100 + i), NULL);
    cl_end(&t);
    assert(cl_alive_count(&t) == 5);
    assert(cl_child_count(&t, 0) == 5);

    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Deep Tree
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_deep_tree) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);

    cl_node_t parent = 0;
    for (int i = 0; i < 20; i++) {
        cl_node_t child = cl_add_col(&t, parent, NULL);
        assert(child != CL_NODE_INVALID);
        parent = child;
    }
    cl_node_t leaf = cl_add_widget(&t, parent, W(99), NULL);
    cl_end(&t);

    assert(cl_alive_count(&t) == 21);
    assert(cl_get_widget(&t, leaf) == W(99));

    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Wide Tree
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_wide_tree) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    for (int i = 0; i < 100; i++)
        cl_add_widget(&t, 0, W(i), NULL);
    cl_end(&t);

    assert(cl_child_count(&t, 0) == 100);
    assert(cl_alive_count(&t) == 100);

    /* Verify traversal */
    cl_node_t cur = cl_first_child(&t, 0);
    int count = 0;
    while (cur) {
        count++;
        cur = cl_next_sibling(&t, cur);
    }
    assert(count == 100);

    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_remove_first_child) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_node_t b = cl_add_widget(&t, 0, W(2), NULL);
    cl_end(&t);

    cl_remove(&t, a);
    assert(cl_child_count(&t, 0) == 1);
    assert(cl_first_child(&t, 0) == b);

    cl_tree_destroy(&t);
}

TEST(test_remove_last_child) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_node_t b = cl_add_widget(&t, 0, W(2), NULL);
    cl_end(&t);

    cl_remove(&t, b);
    assert(cl_child_count(&t, 0) == 1);
    assert(cl_first_child(&t, 0) == a);
    assert(cl_next_sibling(&t, a) == 0);

    cl_tree_destroy(&t);
}

TEST(test_remove_middle_child) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_node_t b = cl_add_widget(&t, 0, W(2), NULL);
    cl_node_t c = cl_add_widget(&t, 0, W(3), NULL);
    cl_end(&t);

    cl_remove(&t, b);
    assert(cl_child_count(&t, 0) == 2);
    assert(cl_first_child(&t, 0) == a);
    assert(cl_next_sibling(&t, a) == c);
    assert(cl_child_at(&t, 0, 1) == c);

    cl_tree_destroy(&t);
}

TEST(test_insert_at_zero) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_end(&t);

    cl_node_t b = cl_insert_widget(&t, 0, 0, W(0), NULL);
    assert(cl_child_at(&t, 0, 0) == b);
    assert(cl_child_at(&t, 0, 1) == a);

    cl_tree_destroy(&t);
}

TEST(test_insert_past_end) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t a = cl_add_widget(&t, 0, W(1), NULL);
    cl_end(&t);

    /* pos=999 should append */
    cl_node_t b = cl_insert_widget(&t, 0, 999, W(2), NULL);
    assert(cl_child_at(&t, 0, 0) == a);
    assert(cl_child_at(&t, 0, 1) == b);

    cl_tree_destroy(&t);
}

TEST(test_reparent_to_root) {
    cl_tree_t t;
    cl_tree_init(&t);
    cl_begin(&t);
    cl_node_t col = cl_add_col(&t, 0, NULL);
    cl_node_t w   = cl_add_widget(&t, col, W(1), NULL);
    cl_end(&t);

    cl_reparent(&t, w, 0, 0);
    assert(cl_child_count(&t, col) == 0);
    assert(cl_child_count(&t, 0) == 2);
    assert(cl_child_at(&t, 0, 0) == w);

    cl_tree_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("clay_layout.h tests:\n");

    printf("\n Lifecycle:\n");
    RUN(test_init_destroy);
    RUN(test_init_state_zeroed);

    printf("\n Build Phase:\n");
    RUN(test_build_simple_row);
    RUN(test_build_nested);
    RUN(test_build_clears_previous);
    RUN(test_build_with_style);

    printf("\n Node Query:\n");
    RUN(test_is_alive_visible);
    RUN(test_first_child_next_sibling);
    RUN(test_query_invalid_node);

    printf("\n Live Mutation:\n");
    RUN(test_insert_at_position);
    RUN(test_insert_row_col_spacer);
    RUN(test_remove_node);
    RUN(test_remove_subtree);
    RUN(test_remove_invalid);
    RUN(test_reparent);
    RUN(test_set_style_live);

    printf("\n Rect Cache:\n");
    RUN(test_rects_add_clear);
    RUN(test_rects_multiple);
    RUN(test_rect_at_invalid);

    printf("\n Hit Test:\n");
    RUN(test_point_in_rect);
    RUN(test_hit_test_basic);
    RUN(test_hit_test_overlap_last_wins);
    RUN(test_hit_test_with_filter);
    RUN(test_hit_test_empty);

    printf("\n Focus / Hover / Capture:\n");
    RUN(test_focus_state);
    RUN(test_hover_state);
    RUN(test_capture_state);
    RUN(test_maximized_state);
    RUN(test_mouse_viewport);

    printf("\n Focus Cycling:\n");
    RUN(test_focus_next);
    RUN(test_focus_prev);
    RUN(test_focus_next_with_filter);
    RUN(test_focus_next_empty);

    printf("\n Tree Traversal:\n");
    RUN(test_walk_simple);
    RUN(test_walk_skip_subtree);
    RUN(test_walk_hidden_skipped);

    printf("\n Alive Count / Collect Widgets:\n");
    RUN(test_alive_count);
    RUN(test_collect_widgets);

    printf("\n Pool Reuse:\n");
    RUN(test_pool_reuse_after_remove);
    RUN(test_pool_reuse_after_rebuild);

    printf("\n Deep / Wide Trees:\n");
    RUN(test_deep_tree);
    RUN(test_wide_tree);

    printf("\n Edge Cases:\n");
    RUN(test_remove_first_child);
    RUN(test_remove_last_child);
    RUN(test_remove_middle_child);
    RUN(test_insert_at_zero);
    RUN(test_insert_past_end);
    RUN(test_reparent_to_root);

    printf("\nAll tests passed.\n");
    return 0;
}
