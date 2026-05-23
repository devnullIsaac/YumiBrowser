/**
 * @file test_toolbox.c
 * @brief Comprehensive tests for toolbox.h — platform-independent toolbox layout engine.
 *
 * Tests cover: lifecycle, category management, item management, virtual items,
 * grid computation, visible range, layout rebuild, scroll, render query,
 * and massive-scale stress tests.
 */
#define TOOLBOX_IMPLEMENTATION
#include "toolbox.h"

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-60s", #name); name(); printf("PASS\n"); } while(0)

/* ═══════════════════════════════════════════════════════════════
 *  Test Helpers
 * ═══════════════════════════════════════════════════════════════ */

static const float W = 300.0f;
static const float H = 600.0f;
static const float HDR = 32.0f;

static void make_tb(toolbox_t *t) {
    toolbox_init(t, W, H, HDR, TOOLBOX_VERTICAL);
    toolbox_set_expanded(t, 1);
}

/* Provider tracking */
#define MAX_PROVIDER_CALLS 4096
typedef struct {
    int item_index;
    int pool_slot;
    int cat_handle;
} provider_call_t;

static provider_call_t g_calls[MAX_PROVIDER_CALLS];
static int g_call_count = 0;

static void reset_provider(void) { g_call_count = 0; }

static void mock_provider(void *tb, toolbox_category_handle_t cat,
                          int item_index, int pool_slot, void *user) {
    (void)tb; (void)user;
    if (g_call_count < MAX_PROVIDER_CALLS) {
        g_calls[g_call_count].item_index = item_index;
        g_calls[g_call_count].pool_slot  = pool_slot;
        g_calls[g_call_count].cat_handle = cat;
        g_call_count++;
    }
}

static float approx(float a, float b) {
    return fabsf(a - b) < 0.01f;
}

/* ═══════════════════════════════════════════════════════════════
 *  1. Lifecycle
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_init_defaults) {
    toolbox_t t;
    toolbox_init(&t, 200, 400, 30, TOOLBOX_VERTICAL);
    assert(t.width == 200.0f);
    assert(t.height == 400.0f);
    assert(t.header_size == 30.0f);
    assert(t.orientation == TOOLBOX_VERTICAL);
    assert(t.expanded == 0);
    assert(t.scroll_offset == 0.0f);
    assert(t.cat_count == 0);
    assert(t.item_w == TOOLBOX_DEFAULT_ITEM_W);
    assert(t.item_h == TOOLBOX_DEFAULT_ITEM_H);
    assert(t.item_gap == TOOLBOX_DEFAULT_ITEM_GAP);
    assert(t.padding == TOOLBOX_DEFAULT_PADDING);
    toolbox_destroy(&t);
}

TEST(test_init_horizontal) {
    toolbox_t t;
    toolbox_init(&t, 200, 400, 30, TOOLBOX_HORIZONTAL);
    assert(t.orientation == TOOLBOX_HORIZONTAL);
    toolbox_destroy(&t);
}

TEST(test_init_zero_header) {
    toolbox_t t;
    toolbox_init(&t, 200, 400, 0, TOOLBOX_VERTICAL);
    assert(t.header_size == TOOLBOX_DEFAULT_HEADER_SIZE);
    toolbox_destroy(&t);
}

TEST(test_destroy_zeroes) {
    toolbox_t t;
    make_tb(&t);
    toolbox_add_category(&t, 24);
    toolbox_destroy(&t);
    assert(t.cat_count == 0);
    assert(t.width == 0.0f);
    assert(t.expanded == 0);
}

/* ═══════════════════════════════════════════════════════════════
 *  2. Category Management
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_add_category) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    assert(h != TOOLBOX_CATEGORY_INVALID);
    assert(toolbox_category_count(&t) == 1);
    toolbox_destroy(&t);
}

TEST(test_add_multiple_categories) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_category_handle_t h2 = toolbox_add_category(&t, 30);
    toolbox_category_handle_t h3 = toolbox_add_category(&t, 20);
    assert(h1 != TOOLBOX_CATEGORY_INVALID);
    assert(h2 != TOOLBOX_CATEGORY_INVALID);
    assert(h3 != TOOLBOX_CATEGORY_INVALID);
    assert(h1 != h2 && h2 != h3);
    assert(toolbox_category_count(&t) == 3);
    toolbox_destroy(&t);
}

TEST(test_add_max_categories) {
    toolbox_t t;
    make_tb(&t);
    for (int i = 0; i < TOOLBOX_MAX_CATEGORIES; i++) {
        toolbox_category_handle_t h = toolbox_add_category(&t, 24);
        assert(h != TOOLBOX_CATEGORY_INVALID);
    }
    assert(toolbox_category_count(&t) == TOOLBOX_MAX_CATEGORIES);
    /* One more should fail */
    toolbox_category_handle_t overflow = toolbox_add_category(&t, 24);
    assert(overflow == TOOLBOX_CATEGORY_INVALID);
    assert(toolbox_category_count(&t) == TOOLBOX_MAX_CATEGORIES);
    toolbox_destroy(&t);
}

TEST(test_remove_category) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_remove_category(&t, h);
    assert(toolbox_category_count(&t) == 0);
    toolbox_destroy(&t);
}

TEST(test_remove_and_reuse_slot) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_remove_category(&t, h1);
    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    assert(h2 == h1); /* should reuse the freed slot */
    assert(toolbox_category_count(&t) == 1);
    toolbox_destroy(&t);
}

TEST(test_remove_invalid_handle) {
    toolbox_t t;
    make_tb(&t);
    toolbox_remove_category(&t, 0);
    toolbox_remove_category(&t, -1);
    toolbox_remove_category(&t, TOOLBOX_MAX_CATEGORIES + 1);
    assert(toolbox_category_count(&t) == 0);
    toolbox_destroy(&t);
}

TEST(test_category_expand_collapse) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    assert(toolbox_is_category_expanded(&t, h) == 0);
    toolbox_set_category_expanded(&t, h, 1);
    assert(toolbox_is_category_expanded(&t, h) == 1);
    toolbox_set_category_expanded(&t, h, 0);
    assert(toolbox_is_category_expanded(&t, h) == 0);
    toolbox_destroy(&t);
}

TEST(test_category_toggle) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    assert(toolbox_is_category_expanded(&t, h) == 0);
    toolbox_toggle_category(&t, h);
    assert(toolbox_is_category_expanded(&t, h) == 1);
    toolbox_toggle_category(&t, h);
    assert(toolbox_is_category_expanded(&t, h) == 0);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  3. Item Management (non-virtual)
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_add_items) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    int idx0 = toolbox_add_item(&t, h);
    int idx1 = toolbox_add_item(&t, h);
    assert(idx0 == 0);
    assert(idx1 == 1);
    assert(toolbox_item_count(&t, h) == 2);
    toolbox_destroy(&t);
}

TEST(test_remove_item) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_add_item(&t, h);
    toolbox_add_item(&t, h);
    toolbox_remove_item_at(&t, h, 0);
    assert(toolbox_item_count(&t, h) == 1);
    toolbox_destroy(&t);
}

TEST(test_add_item_max) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    for (int i = 0; i < TOOLBOX_MAX_ITEMS_PER_CAT; i++) {
        int idx = toolbox_add_item(&t, h);
        assert(idx == i);
    }
    /* One more should fail */
    int overflow = toolbox_add_item(&t, h);
    assert(overflow == -1);
    toolbox_destroy(&t);
}

TEST(test_remove_item_invalid) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_add_item(&t, h);
    toolbox_remove_item_at(&t, h, -1);  /* no crash */
    toolbox_remove_item_at(&t, h, 5);   /* no crash */
    assert(toolbox_item_count(&t, h) == 1);
    toolbox_destroy(&t);
}

TEST(test_item_count_invalid_handle) {
    toolbox_t t;
    make_tb(&t);
    assert(toolbox_item_count(&t, 0) == 0);
    assert(toolbox_item_count(&t, 99) == 0);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  4. Virtual Items
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_virtual_items) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_virtual_items(&t, h, 500, 32, mock_provider, NULL);
    assert(toolbox_item_count(&t, h) == 500); /* virtual_total */
    toolbox_destroy(&t);
}

TEST(test_virtual_pool_size_clamp) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    /* pool_size > MAX gets clamped */
    toolbox_set_virtual_items(&t, h, 1000, 9999, mock_provider, NULL);
    toolbox_cat_t *cat = &t.categories[h - 1];
    assert(cat->pool_size == TOOLBOX_MAX_ITEMS_PER_CAT);
    toolbox_destroy(&t);
}

TEST(test_virtual_null_provider) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_virtual_items(&t, h, 500, 32, NULL, NULL);
    /* Should not set virtual mode */
    assert(toolbox_item_count(&t, h) == 0);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  5. Expand / State
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_root_expand) {
    toolbox_t t;
    toolbox_init(&t, W, H, HDR, TOOLBOX_VERTICAL);
    assert(toolbox_is_expanded(&t) == 0);
    toolbox_set_expanded(&t, 1);
    assert(toolbox_is_expanded(&t) == 1);
    toolbox_set_expanded(&t, 0);
    assert(toolbox_is_expanded(&t) == 0);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  6. Geometry
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_size) {
    toolbox_t t;
    make_tb(&t);
    toolbox_set_size(&t, 400, 800);
    assert(t.width == 400.0f);
    assert(t.height == 800.0f);
    toolbox_destroy(&t);
}

TEST(test_set_item_size) {
    toolbox_t t;
    make_tb(&t);
    toolbox_set_item_size(&t, 64, 64);
    assert(t.item_w == 64.0f);
    assert(t.item_h == 64.0f);
    toolbox_destroy(&t);
}

TEST(test_set_item_size_zero_defaults) {
    toolbox_t t;
    make_tb(&t);
    toolbox_set_item_size(&t, 0, 0);
    assert(t.item_w == TOOLBOX_DEFAULT_ITEM_W);
    assert(t.item_h == TOOLBOX_DEFAULT_ITEM_H);
    toolbox_destroy(&t);
}

TEST(test_set_gap) {
    toolbox_t t;
    make_tb(&t);
    toolbox_set_item_gap(&t, 8);
    assert(t.item_gap == 8.0f);
    toolbox_set_item_gap(&t, -1);
    assert(t.item_gap == TOOLBOX_DEFAULT_ITEM_GAP);
    toolbox_destroy(&t);
}

TEST(test_effective_size_collapsed_vertical) {
    toolbox_t t;
    toolbox_init(&t, 300, 600, 32, TOOLBOX_VERTICAL);
    /* collapsed */
    assert(toolbox_get_effective_width(&t) == 300.0f);
    assert(toolbox_get_effective_height(&t) == 32.0f);
    toolbox_destroy(&t);
}

TEST(test_effective_size_expanded_vertical) {
    toolbox_t t;
    toolbox_init(&t, 300, 600, 32, TOOLBOX_VERTICAL);
    toolbox_set_expanded(&t, 1);
    assert(toolbox_get_effective_width(&t) == 300.0f);
    assert(toolbox_get_effective_height(&t) == 600.0f);
    toolbox_destroy(&t);
}

TEST(test_effective_size_collapsed_horizontal) {
    toolbox_t t;
    toolbox_init(&t, 300, 600, 32, TOOLBOX_HORIZONTAL);
    assert(toolbox_get_effective_width(&t) == 32.0f);
    assert(toolbox_get_effective_height(&t) == 600.0f);
    toolbox_destroy(&t);
}

TEST(test_content_bounds_vertical) {
    toolbox_t t;
    make_tb(&t);
    float cx, cy, cw, ch;
    toolbox_content_bounds(&t, &cx, &cy, &cw, &ch);
    assert(cx == 0.0f);
    assert(cy == HDR);
    assert(cw == W);
    assert(ch == H - HDR);
    toolbox_destroy(&t);
}

TEST(test_content_bounds_horizontal) {
    toolbox_t t;
    toolbox_init(&t, 300, 600, 32, TOOLBOX_HORIZONTAL);
    toolbox_set_expanded(&t, 1);
    float cx, cy, cw, ch;
    toolbox_content_bounds(&t, &cx, &cy, &cw, &ch);
    assert(cx == 32.0f);
    assert(cy == 0.0f);
    assert(cw == 268.0f);
    assert(ch == 600.0f);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  7. Grid Computation (pure math)
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_compute_columns) {
    /* 300 width, 48 item, 4 gap: (300+4)/(48+4) = 5.84 -> 5 cols */
    assert(toolbox_compute_columns(300, 48, 4) == 5);
    /* Too narrow for even one */
    assert(toolbox_compute_columns(10, 48, 4) == 1);
    /* 99px: (99+4)/52 = 1.98 -> 1 col */
    assert(toolbox_compute_columns(99, 48, 4) == 1);
    /* 100px: 48+4+48 = 100, exactly 2 */
    assert(toolbox_compute_columns(100, 48, 4) == 2);
}

TEST(test_compute_rows) {
    assert(toolbox_compute_rows(0, 5) == 0);
    assert(toolbox_compute_rows(1, 5) == 1);
    assert(toolbox_compute_rows(5, 5) == 1);
    assert(toolbox_compute_rows(6, 5) == 2);
    assert(toolbox_compute_rows(10, 5) == 2);
    assert(toolbox_compute_rows(11, 5) == 3);
}

TEST(test_compute_content_height) {
    /* 3 rows, 48h, 4gap, 4pad: 4*2 + 3*48 + 2*4 = 8+144+8 = 160 */
    float h = toolbox_compute_content_height(3, 48, 4, 4);
    assert(approx(h, 160.0f));
    /* 0 rows = 0 */
    assert(toolbox_compute_content_height(0, 48, 4, 4) == 0.0f);
    /* 1 row: 4*2 + 48 + 0 = 56 */
    assert(approx(toolbox_compute_content_height(1, 48, 4, 4), 56.0f));
}

TEST(test_item_position) {
    toolbox_item_pos_t p;
    /* First item: (pad, pad) */
    p = toolbox_item_position(0, 5, 48, 48, 4, 4);
    assert(approx(p.x, 4.0f));
    assert(approx(p.y, 4.0f));
    /* Second col */
    p = toolbox_item_position(1, 5, 48, 48, 4, 4);
    assert(approx(p.x, 56.0f));
    assert(approx(p.y, 4.0f));
    /* Second row, first col */
    p = toolbox_item_position(5, 5, 48, 48, 4, 4);
    assert(approx(p.x, 4.0f));
    assert(approx(p.y, 56.0f));
}

/* ═══════════════════════════════════════════════════════════════
 *  8. Visible Range Computation
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_visible_range_basic) {
    /* 100 items, pool 20, 5 cols, 48h, 4gap, 4pad, viewport 300 */
    toolbox_visible_range_t r = toolbox_compute_visible_range(
        0, 100, 20, 5, 48, 4, 4, 300);
    assert(r.first_item == 0);
    assert(r.vis_count > 0);
    assert(r.vis_count <= 20);
    assert(r.full_h > 0);
    assert(r.content_h > 0);
    assert(r.content_h <= 300.0f);
}

TEST(test_visible_range_scrolled) {
    /* Scroll down 200px: first visible row changes */
    toolbox_visible_range_t r = toolbox_compute_visible_range(
        200, 100, 20, 5, 48, 4, 4, 300);
    /* row_h = 52, first_row = 200/52 = 3 -> first_item = 15 */
    assert(r.first_item == 15);
    assert(r.vis_count > 0);
    assert(r.vis_count <= 20);
}

TEST(test_visible_range_at_end) {
    /* Scroll to the very end */
    toolbox_visible_range_t r0 = toolbox_compute_visible_range(
        0, 100, 20, 5, 48, 4, 4, 300);
    float end = r0.full_h - 300;
    if (end < 0) end = 0;
    toolbox_visible_range_t r = toolbox_compute_visible_range(
        end, 100, 20, 5, 48, 4, 4, 300);
    assert(r.first_item + r.vis_count <= 100);
    assert(r.vis_count > 0);
}

TEST(test_visible_range_empty) {
    toolbox_visible_range_t r = toolbox_compute_visible_range(
        0, 0, 20, 5, 48, 4, 4, 300);
    assert(r.vis_count == 0);
    assert(r.full_h == 0.0f);
}

TEST(test_visible_range_single_item) {
    toolbox_visible_range_t r = toolbox_compute_visible_range(
        0, 1, 20, 5, 48, 4, 4, 300);
    assert(r.first_item == 0);
    assert(r.vis_count == 1);
    assert(r.full_h > 0);
}

/* ═══════════════════════════════════════════════════════════════
 *  9. Apply Visible Range
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_apply_visible_range) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_virtual_items(&t, h, 100, 20, mock_provider, NULL);

    toolbox_visible_range_t r = toolbox_compute_visible_range(
        0, 100, 20, 5, 48, 4, 4, 300);
    r.needs_rotation = 1;
    toolbox_apply_visible_range(&t, h, &r);

    /* Provider should have been called vis_count times */
    assert(g_call_count == r.vis_count);
    /* First call: item_index=0, pool_slot=0 */
    assert(g_calls[0].item_index == 0);
    assert(g_calls[0].pool_slot == 0);

    toolbox_cat_t *cat = &t.categories[h - 1];
    assert(cat->virtual_offset == r.first_item);
    assert(cat->vis_count == r.vis_count);

    toolbox_destroy(&t);
}

TEST(test_apply_no_rotation) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_virtual_items(&t, h, 100, 20, mock_provider, NULL);

    toolbox_visible_range_t r = toolbox_compute_visible_range(
        0, 100, 20, 5, 48, 4, 4, 300);
    r.needs_rotation = 1;
    toolbox_apply_visible_range(&t, h, &r);
    int first_count = g_call_count;

    /* Applying same range again - no rotation since offset unchanged */
    reset_provider();
    toolbox_apply_visible_range(&t, h, &r);
    assert(g_call_count == 0); /* no provider calls since offset same */

    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  10. Layout Rebuild
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_rebuild_single_category) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);

    toolbox_rebuild_layout(&t);

    toolbox_cat_t *cat = &t.categories[h - 1];
    assert(cat->cols >= 1);
    assert(cat->content_h > 0);
    assert(t.total_content_h > 0);
    assert(t.layout_dirty == 0);

    toolbox_destroy(&t);
}

TEST(test_rebuild_virtual_category) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 500, 32, mock_provider, NULL);

    toolbox_rebuild_layout(&t);

    toolbox_cat_t *cat = &t.categories[h - 1];
    assert(cat->cols >= 1);
    assert(cat->virtual_content_h > 0);
    assert(cat->vis_count > 0);
    assert(cat->vis_count <= 32);
    assert(t.total_content_h > 0);

    toolbox_destroy(&t);
}

TEST(test_rebuild_collapsed_no_content) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    /* Not expanded */
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);

    toolbox_rebuild_layout(&t);
    /* total = just the header height */
    assert(approx(t.total_content_h, 24.0f));

    toolbox_destroy(&t);
}

TEST(test_rebuild_multiple_categories) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    toolbox_set_category_expanded(&t, h2, 1);
    for (int i = 0; i < 10; i++) {
        toolbox_add_item(&t, h1);
        toolbox_add_item(&t, h2);
    }

    toolbox_rebuild_layout(&t);

    float h1_content = t.categories[h1 - 1].content_h;
    float h2_content = t.categories[h2 - 1].content_h;
    assert(h1_content > 0);
    assert(h2_content > 0);
    /* total should be >= both + headers + gap */
    assert(t.total_content_h >= h1_content + h2_content + 24 + 24);

    toolbox_destroy(&t);
}

TEST(test_rebuild_not_expanded_noop) {
    toolbox_t t;
    toolbox_init(&t, W, H, HDR, TOOLBOX_VERTICAL);
    /* NOT expanded */
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);

    toolbox_rebuild_layout(&t);
    /* Should be no-op if root not expanded */
    assert(t.total_content_h == 0);

    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  11. virtual_y Positions
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_virtual_y_ordering) {
    toolbox_t t;
    make_tb(&t);

    toolbox_category_handle_t cats[5];
    for (int i = 0; i < 5; i++) {
        cats[i] = toolbox_add_category(&t, 24);
        toolbox_set_category_expanded(&t, cats[i], 1);
        for (int j = 0; j < 10; j++) toolbox_add_item(&t, cats[i]);
    }

    toolbox_rebuild_layout(&t);

    /* virtual_y must be strictly increasing */
    for (int i = 1; i < 5; i++) {
        float prev_y = t.categories[cats[i - 1] - 1].virtual_y;
        float curr_y = t.categories[cats[i] - 1].virtual_y;
        assert(curr_y > prev_y);
    }

    toolbox_destroy(&t);
}

TEST(test_virtual_y_first_is_zero) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 5; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    assert(t.categories[h - 1].virtual_y == 0.0f);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  12. Scroll
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_scroll_clamp_zero) {
    toolbox_t t;
    make_tb(&t);
    toolbox_scroll_to(&t, -100);
    assert(t.scroll_offset == 0.0f);
    toolbox_destroy(&t);
}

TEST(test_scroll_clamp_max) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 50; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    float max = toolbox_max_scroll(&t);
    toolbox_scroll_to(&t, max + 1000);
    assert(t.scroll_offset <= max + 0.01f);
    toolbox_destroy(&t);
}

TEST(test_scroll_by) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 5000, 64, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    float max = toolbox_max_scroll(&t);
    assert(max > 100.0f);
    toolbox_scroll_by(&t, 50);
    assert(t.scroll_offset == 50.0f);
    toolbox_scroll_by(&t, 50);
    assert(t.scroll_offset == 100.0f);
    toolbox_destroy(&t);
}

TEST(test_max_scroll_no_overflow) {
    toolbox_t t;
    make_tb(&t);
    /* With few items, max_scroll should be 0 (content fits) */
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    float max = toolbox_max_scroll(&t);
    assert(max == 0.0f);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  13. Render Query (toolbox_query_visible)
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_query_not_expanded) {
    toolbox_t t;
    toolbox_init(&t, W, H, HDR, TOOLBOX_VERTICAL);
    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 0);
    toolbox_destroy(&t);
}

TEST(test_query_single_collapsed_cat) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    /* Category collapsed */
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 1);
    assert(vis.entries[0].handle == h);
    assert(vis.entries[0].expanded == 0);
    assert(vis.entries[0].visible == 1);
    assert(vis.entries[0].render_count == 0);
    toolbox_destroy(&t);
}

TEST(test_query_single_expanded_cat) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 1);
    assert(vis.entries[0].expanded == 1);
    assert(vis.entries[0].visible == 1);
    assert(vis.entries[0].render_count == 10);
    assert(vis.entries[0].first_item == 0);
    assert(vis.entries[0].cols >= 1);
    toolbox_destroy(&t);
}

TEST(test_query_screen_y_reflects_scroll) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 5000, 64, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis0;
    toolbox_query_visible(&t, &vis0);
    float y0 = vis0.entries[0].screen_y;

    float scroll_amt = 100.0f;
    toolbox_scroll_to(&t, scroll_amt);
    assert(t.scroll_offset == scroll_amt);
    toolbox_rebuild_layout(&t);
    toolbox_vis_list_t vis1;
    toolbox_query_visible(&t, &vis1);
    float y1 = vis1.entries[0].screen_y;

    assert(approx(y0 - y1, scroll_amt));
    toolbox_destroy(&t);
}

TEST(test_query_multiple_cats_ordering) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    toolbox_category_handle_t h3 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    toolbox_set_category_expanded(&t, h2, 1);
    toolbox_set_category_expanded(&t, h3, 1);
    for (int i = 0; i < 5; i++) {
        toolbox_add_item(&t, h1);
        toolbox_add_item(&t, h2);
        toolbox_add_item(&t, h3);
    }
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 3);
    /* screen_y should be increasing */
    for (int i = 1; i < vis.count; i++) {
        assert(vis.entries[i].screen_y > vis.entries[i - 1].screen_y);
    }
    toolbox_destroy(&t);
}

TEST(test_query_visibility_culling) {
    toolbox_t t;
    make_tb(&t);
    /* Create enough categories with items to push some off screen */
    toolbox_category_handle_t cats[20];
    for (int i = 0; i < 20; i++) {
        cats[i] = toolbox_add_category(&t, 24);
        toolbox_set_category_expanded(&t, cats[i], 1);
        for (int j = 0; j < 20; j++) toolbox_add_item(&t, cats[i]);
    }
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 20);

    /* Some should be visible, some not */
    int visible_count = 0, invisible_count = 0;
    for (int i = 0; i < vis.count; i++) {
        if (vis.entries[i].visible) visible_count++;
        else invisible_count++;
    }
    assert(visible_count > 0);
    assert(invisible_count > 0); /* viewport is 568px, content much larger */

    /* Invisible entries should have render_count=0 */
    for (int i = 0; i < vis.count; i++) {
        if (!vis.entries[i].visible)
            assert(vis.entries[i].render_count == 0);
    }
    toolbox_destroy(&t);
}

TEST(test_query_content_bounds) {
    toolbox_t t;
    make_tb(&t);
    toolbox_add_category(&t, 24);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.content_x == 0.0f);
    assert(vis.content_y == HDR);
    assert(vis.content_w == W);
    assert(vis.content_h == H - HDR);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  14. Vis Item Screen Pos
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_vis_item_screen_pos_basic) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 1);

    toolbox_item_pos_t p0 = toolbox_vis_item_screen_pos(&t, &vis.entries[0], 0);
    /* Should be origin + padding */
    float expect_x = vis.entries[0].item_origin_x + t.padding;
    float expect_y = vis.entries[0].item_origin_y + t.padding;
    assert(approx(p0.x, expect_x));
    assert(approx(p0.y, expect_y));

    toolbox_destroy(&t);
}

TEST(test_vis_item_screen_pos_second_row) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 20; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);

    int cols = vis.entries[0].cols;
    assert(cols >= 1);

    /* First item in second row */
    toolbox_item_pos_t p = toolbox_vis_item_screen_pos(&t, &vis.entries[0], cols);
    toolbox_item_pos_t p0 = toolbox_vis_item_screen_pos(&t, &vis.entries[0], 0);
    assert(approx(p.x, p0.x));
    assert(approx(p.y, p0.y + t.item_h + t.item_gap));

    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  15. Massive Scale: Stress Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_massive_virtual_single_category) {
    /* 10,000 items in one virtual category, pool of 64 */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 10000, 64, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    assert(toolbox_item_count(&t, h) == 10000);
    toolbox_cat_t *cat = &t.categories[h - 1];
    assert(cat->vis_count <= 64);
    assert(cat->vis_count > 0);
    assert(cat->virtual_content_h > 0);

    /* Query visible — all items should be within viewport */
    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 1);
    assert(vis.entries[0].visible == 1);
    assert(vis.entries[0].render_count <= 64);
    assert(vis.entries[0].render_count > 0);

    /* Every rendered item position must be finite */
    for (int i = 0; i < vis.entries[0].render_count; i++) {
        toolbox_item_pos_t p = toolbox_vis_item_screen_pos(&t, &vis.entries[0], i);
        assert(isfinite(p.x));
        assert(isfinite(p.y));
        assert(p.x >= 0);
    }

    toolbox_destroy(&t);
}

TEST(test_massive_virtual_scrolled) {
    /* 10,000 items, scroll to various positions */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 10000, 64, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    float max = toolbox_max_scroll(&t);
    assert(max > 0);

    /* Scroll to middle */
    toolbox_scroll_to(&t, max / 2);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.entries[0].render_count > 0);
    assert(vis.entries[0].render_count <= 64);
    assert(vis.entries[0].first_item > 0);

    /* Scroll to end */
    toolbox_scroll_to(&t, max);
    toolbox_rebuild_layout(&t);
    toolbox_query_visible(&t, &vis);
    assert(vis.entries[0].render_count > 0);
    assert(vis.entries[0].first_item + vis.entries[0].render_count <= 10000);

    toolbox_destroy(&t);
}

TEST(test_massive_many_categories) {
    /* Fill all 64 categories, each with 500 virtual items */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    toolbox_category_handle_t cats[TOOLBOX_MAX_CATEGORIES];
    for (int i = 0; i < TOOLBOX_MAX_CATEGORIES; i++) {
        cats[i] = toolbox_add_category(&t, 24);
        assert(cats[i] != TOOLBOX_CATEGORY_INVALID);
        toolbox_set_category_expanded(&t, cats[i], 1);
        toolbox_set_virtual_items(&t, cats[i], 500, 32, mock_provider, NULL);
    }

    toolbox_rebuild_layout(&t);

    assert(t.total_content_h > 0);
    assert(t.cat_count == TOOLBOX_MAX_CATEGORIES);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == TOOLBOX_MAX_CATEGORIES);

    /* Count visible categories */
    int vis_cats = 0;
    for (int i = 0; i < vis.count; i++) {
        if (vis.entries[i].visible) vis_cats++;
    }
    assert(vis_cats > 0);
    /* Most should be off-screen */
    assert(vis_cats < TOOLBOX_MAX_CATEGORIES);

    /* All visible entries: items within bounds */
    for (int i = 0; i < vis.count; i++) {
        if (!vis.entries[i].visible) continue;
        for (int s = 0; s < vis.entries[i].render_count; s++) {
            toolbox_item_pos_t p = toolbox_vis_item_screen_pos(
                &t, &vis.entries[i], s);
            assert(isfinite(p.x));
            assert(isfinite(p.y));
            assert(p.x >= 0);
        }
    }

    toolbox_destroy(&t);
}

TEST(test_massive_scroll_through_all) {
    /* 64 categories × 200 virtual items, scroll through entire range */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    for (int i = 0; i < TOOLBOX_MAX_CATEGORIES; i++) {
        toolbox_category_handle_t h = toolbox_add_category(&t, 24);
        toolbox_set_category_expanded(&t, h, 1);
        toolbox_set_virtual_items(&t, h, 200, 32, mock_provider, NULL);
    }

    toolbox_rebuild_layout(&t);
    float max = toolbox_max_scroll(&t);
    assert(max > 0);

    float viewport_h = H - HDR;
    float step = viewport_h / 2; /* scroll half a viewport at a time */
    int steps = 0;

    for (float offset = 0; offset <= max; offset += step) {
        toolbox_scroll_to(&t, offset);
        toolbox_rebuild_layout(&t);

        toolbox_vis_list_t vis;
        toolbox_query_visible(&t, &vis);

        /* At least one category must be visible */
        int any_vis = 0;
        for (int i = 0; i < vis.count; i++) {
            if (vis.entries[i].visible) {
                any_vis = 1;
                /* Visible entry: screen_y + height must overlap viewport */
                float vp_top = vis.content_y;
                float vp_bot = vis.content_y + vis.content_h;
                float top = vis.entries[i].screen_y;
                float bot = vis.entries[i].screen_y + vis.entries[i].height;
                assert(bot > vp_top && top < vp_bot);
            }
        }
        assert(any_vis);
        steps++;
    }
    assert(steps > 10); /* sanity: we scrolled many times */

    toolbox_destroy(&t);
}

TEST(test_massive_50k_items_single_cat) {
    /* 50,000 items in one category */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 50000, 64, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    /* Virtual content height should be huge */
    toolbox_cat_t *cat = &t.categories[h - 1];
    assert(cat->virtual_content_h > 10000.0f);

    /* But vis_count must be bounded by pool */
    assert(cat->vis_count <= 64);

    /* Scroll to middle and verify */
    float max = toolbox_max_scroll(&t);
    toolbox_scroll_to(&t, max / 2);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.entries[0].render_count <= 64);
    assert(vis.entries[0].first_item > 0);
    assert(vis.entries[0].first_item < 50000);

    /* Item positions for all rendered items should be finite */
    for (int i = 0; i < vis.entries[0].render_count; i++) {
        toolbox_item_pos_t p = toolbox_vis_item_screen_pos(&t, &vis.entries[0], i);
        assert(isfinite(p.x));
        assert(isfinite(p.y));
    }

    toolbox_destroy(&t);
}

TEST(test_massive_content_height_consistency) {
    /* Verify total_content_h = sum of category heights + gaps */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    int ncats = 10;
    toolbox_category_handle_t cats[10];
    int item_counts[] = {100, 500, 10, 1000, 50, 200, 5, 2000, 300, 1};
    for (int i = 0; i < ncats; i++) {
        cats[i] = toolbox_add_category(&t, 24);
        toolbox_set_category_expanded(&t, cats[i], 1);
        toolbox_set_virtual_items(&t, cats[i], item_counts[i], 32, mock_provider, NULL);
    }

    toolbox_rebuild_layout(&t);

    /* Manually recompute expected total */
    float cx, cy, cw, ch;
    toolbox_content_bounds(&t, &cx, &cy, &cw, &ch);
    float avail_w = cw - t.padding * 2;
    float expected_h = 0;
    int alive = 0;
    for (int i = 0; i < ncats; i++) {
        toolbox_cat_t *cat = &t.categories[cats[i] - 1];
        float gap = (alive > 0) ? 2.0f : 0.0f;
        expected_h += gap + cat->cat_header_size + cat->virtual_content_h;
        alive++;
    }
    assert(approx(t.total_content_h, expected_h));

    toolbox_destroy(&t);
}

TEST(test_massive_no_items_overdraw_viewport) {
    /*
     * CRITICAL RULE: No rendered item may have a position beyond the
     * toolbox surface dimensions (width × height).
     * Test with many categories and large item counts.
     */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    for (int i = 0; i < 20; i++) {
        toolbox_category_handle_t h = toolbox_add_category(&t, 24);
        toolbox_set_category_expanded(&t, h, 1);
        toolbox_set_virtual_items(&t, h, 1000, 64, mock_provider, NULL);
    }

    toolbox_rebuild_layout(&t);
    float max = toolbox_max_scroll(&t);

    /* Check at multiple scroll positions */
    float positions[] = {0, 50, 200, max / 4, max / 2, max * 3 / 4, max};
    for (int p = 0; p < 7; p++) {
        toolbox_scroll_to(&t, positions[p]);
        toolbox_rebuild_layout(&t);

        toolbox_vis_list_t vis;
        toolbox_query_visible(&t, &vis);

        for (int i = 0; i < vis.count; i++) {
            if (!vis.entries[i].visible) continue;
            if (!vis.entries[i].expanded) continue;

            for (int s = 0; s < vis.entries[i].render_count; s++) {
                toolbox_item_pos_t pos = toolbox_vis_item_screen_pos(
                    &t, &vis.entries[i], s);
                /* X must be within [0, width] */
                assert(pos.x >= 0.0f);
                assert(pos.x + t.item_w <= t.width + 1.0f);
                /* Note: Y can extend beyond viewport for partially visible
                   categories at the bottom, but item_origin_y should be
                   based on screen_y which accounts for scroll. */
            }
        }
    }

    toolbox_destroy(&t);
}

TEST(test_massive_provider_rotation) {
    /* Verify provider is called with correct indices when scrolling */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 5000, 32, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    int initial_calls = g_call_count;
    assert(initial_calls > 0);

    /* Verify first batch starts at 0 */
    assert(g_calls[0].item_index == 0);
    assert(g_calls[0].pool_slot == 0);

    /* Scroll far enough to trigger rotation */
    reset_provider();
    toolbox_scroll_to(&t, 500);
    toolbox_rebuild_layout(&t);

    if (g_call_count > 0) {
        /* Provider calls should have increasing item indices */
        for (int i = 1; i < g_call_count; i++) {
            assert(g_calls[i].item_index == g_calls[i - 1].item_index + 1);
            assert(g_calls[i].pool_slot == g_calls[i - 1].pool_slot + 1);
        }
        /* First item should be > 0 since we scrolled */
        assert(g_calls[0].item_index > 0);
    }

    toolbox_destroy(&t);
}

TEST(test_massive_mixed_virtual_nonvirtual) {
    /* Mix of virtual and non-virtual categories */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    /* 5 non-virtual with few items */
    toolbox_category_handle_t nv[5];
    for (int i = 0; i < 5; i++) {
        nv[i] = toolbox_add_category(&t, 24);
        toolbox_set_category_expanded(&t, nv[i], 1);
        for (int j = 0; j < 5; j++) toolbox_add_item(&t, nv[i]);
    }

    /* 5 virtual with many items */
    toolbox_category_handle_t v[5];
    for (int i = 0; i < 5; i++) {
        v[i] = toolbox_add_category(&t, 24);
        toolbox_set_category_expanded(&t, v[i], 1);
        toolbox_set_virtual_items(&t, v[i], 2000, 32, mock_provider, NULL);
    }

    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 10);

    /* Non-virtual cats: first_item=0, render_count=item_count */
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < vis.count; j++) {
            if (vis.entries[j].handle == nv[i] && vis.entries[j].visible) {
                assert(vis.entries[j].first_item == 0);
                assert(vis.entries[j].render_count == 5);
            }
        }
    }

    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  16. Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_empty_toolbox_query) {
    toolbox_t t;
    make_tb(&t);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 0);
    toolbox_destroy(&t);
}

TEST(test_single_item_category) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.entries[0].render_count == 1);

    toolbox_item_pos_t p = toolbox_vis_item_screen_pos(&t, &vis.entries[0], 0);
    assert(isfinite(p.x));
    assert(isfinite(p.y));
    assert(p.x >= 0);
    assert(p.y >= HDR);
    toolbox_destroy(&t);
}

TEST(test_zero_width_toolbox) {
    toolbox_t t;
    toolbox_init(&t, 0, 600, 32, TOOLBOX_VERTICAL);
    toolbox_set_expanded(&t, 1);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    toolbox_cat_t *cat = &t.categories[h - 1];
    assert(cat->cols >= 1); /* always at least 1 */
    toolbox_destroy(&t);
}

TEST(test_resize_triggers_dirty) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);
    assert(t.layout_dirty == 0);

    toolbox_set_size(&t, 500, 800);
    assert(t.layout_dirty == 1);
    toolbox_destroy(&t);
}

TEST(test_virtual_1_item) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 1, 32, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.entries[0].render_count == 1);
    assert(vis.entries[0].first_item == 0);
    toolbox_destroy(&t);
}

TEST(test_scroll_marks_virtual_dirty) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 5000, 32, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    toolbox_scroll_by(&t, 100);
    assert(t.layout_dirty == 1);
    assert(t.categories[h - 1].layout_dirty == 1);
    toolbox_destroy(&t);
}

TEST(test_massive_narrow_toolbox) {
    /* Very narrow toolbox: 60px wide, items 48px. 1 column only. */
    toolbox_t t;
    toolbox_init(&t, 60, 600, 32, TOOLBOX_VERTICAL);
    toolbox_set_expanded(&t, 1);

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    reset_provider();
    toolbox_set_virtual_items(&t, h, 5000, 64, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    toolbox_cat_t *cat = &t.categories[h - 1];
    assert(cat->cols == 1);

    /* Each row is 1 item, so we need way fewer pool items visible */
    assert(cat->vis_count <= 64);
    assert(cat->vis_count > 0);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    for (int i = 0; i < vis.entries[0].render_count; i++) {
        toolbox_item_pos_t p = toolbox_vis_item_screen_pos(&t, &vis.entries[0], i);
        assert(p.x >= 0);
        assert(p.x + t.item_w <= 60.0f + 1.0f);
    }

    toolbox_destroy(&t);
}

TEST(test_massive_wide_toolbox) {
    /* Very wide toolbox: 2000px. Many columns. */
    toolbox_t t;
    toolbox_init(&t, 2000, 600, 32, TOOLBOX_VERTICAL);
    toolbox_set_expanded(&t, 1);

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    reset_provider();
    toolbox_set_virtual_items(&t, h, 10000, 128, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    toolbox_cat_t *cat = &t.categories[h - 1];
    /* Should have lots of columns: (2000+4)/(48+4) ~= 38 */
    assert(cat->cols > 30);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.entries[0].render_count > 0);
    assert(vis.entries[0].render_count <= 128);

    toolbox_destroy(&t);
}

TEST(test_massive_all_collapsed) {
    /* All categories collapsed: minimal total content height */
    toolbox_t t;
    make_tb(&t);

    for (int i = 0; i < 30; i++) {
        toolbox_category_handle_t h = toolbox_add_category(&t, 24);
        /* NOT expanded */
        reset_provider();
        toolbox_set_virtual_items(&t, h, 5000, 32, mock_provider, NULL);
    }

    toolbox_rebuild_layout(&t);

    /* total = 30 headers + 29 gaps */
    float expected = 30 * 24.0f + 29 * 2.0f;
    assert(approx(t.total_content_h, expected));

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    for (int i = 0; i < vis.count; i++) {
        assert(vis.entries[i].render_count == 0);
    }

    toolbox_destroy(&t);
}

TEST(test_scroll_then_expand) {
    /* Scroll, then expand a category: layout should update */
    toolbox_t t;
    make_tb(&t);

    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    for (int i = 0; i < 50; i++) toolbox_add_item(&t, h1);
    toolbox_rebuild_layout(&t);

    float before = t.total_content_h;

    /* Expand h2 */
    toolbox_set_category_expanded(&t, h2, 1);
    for (int i = 0; i < 30; i++) toolbox_add_item(&t, h2);
    toolbox_rebuild_layout(&t);

    assert(t.total_content_h > before);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  17. Render Position Invariants
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_render_positions_no_overlap) {
    /* Items within a category grid should not overlap */
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 20; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);

    /* Check no two items share the same grid cell */
    for (int i = 0; i < vis.entries[0].render_count; i++) {
        toolbox_item_pos_t pi = toolbox_vis_item_screen_pos(&t, &vis.entries[0], i);
        for (int j = i + 1; j < vis.entries[0].render_count; j++) {
            toolbox_item_pos_t pj = toolbox_vis_item_screen_pos(&t, &vis.entries[0], j);
            /* Two items must not occupy the same cell */
            int same_x = approx(pi.x, pj.x);
            int same_y = approx(pi.y, pj.y);
            assert(!(same_x && same_y));
        }
    }

    toolbox_destroy(&t);
}

TEST(test_render_header_before_items) {
    /* Category items should start below the header */
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.entries[0].item_origin_y >= vis.entries[0].screen_y + 24.0f - 0.01f);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  18. Layout Correctness — Duplicate Detection
 * ═══════════════════════════════════════════════════════════════ */

/* Helper: collect all rendered item positions from a vis_list into a flat array.
   Returns the total count. Positions stored in caller-provided buffer. */
typedef struct { float x, y; int cat_idx; } pos_record_t;

static int collect_all_positions(const toolbox_t *t, const toolbox_vis_list_t *vis,
                                 pos_record_t *buf, int buf_cap) {
    int n = 0;
    for (int vi = 0; vi < vis->count; vi++) {
        const toolbox_vis_entry_t *e = &vis->entries[vi];
        if (!e->visible || !e->expanded || e->render_count == 0) continue;
        for (int j = 0; j < e->render_count; j++) {
            assert(n < buf_cap);
            toolbox_item_pos_t p = toolbox_vis_item_screen_pos(t, e, j);
            buf[n].x = p.x;
            buf[n].y = p.y;
            buf[n].cat_idx = vi;
            n++;
        }
    }
    return n;
}

static void assert_no_duplicate_positions(const pos_record_t *buf, int n) {
    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            int dup = approx(buf[a].x, buf[b].x) && approx(buf[a].y, buf[b].y);
            if (dup) {
                printf("\n  DUPLICATE: item [cat %d] at (%.1f, %.1f) == "
                       "item [cat %d] at (%.1f, %.1f)\n",
                       buf[a].cat_idx, buf[a].x, buf[a].y,
                       buf[b].cat_idx, buf[b].x, buf[b].y);
            }
            assert(!dup);
        }
    }
}

TEST(test_no_dup_positions_multi_cat) {
    /* All items across 3 non-virtual expanded categories must have unique positions */
    toolbox_t t;
    make_tb(&t);
    for (int i = 0; i < 3; i++) {
        toolbox_category_handle_t h = toolbox_add_category(&t, 24);
        toolbox_set_category_expanded(&t, h, 1);
        for (int j = 0; j < 15; j++) toolbox_add_item(&t, h);
    }
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    pos_record_t buf[256];
    int n = collect_all_positions(&t, &vis, buf, 256);
    assert(n == 45);
    assert_no_duplicate_positions(buf, n);
    toolbox_destroy(&t);
}

TEST(test_items_within_category_bounds) {
    /* Every item must fall within its category's visual region [header_bottom, cat_bottom] */
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    for (int i = 0; i < 20; i++) toolbox_add_item(&t, h1);

    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h2, 1);
    for (int i = 0; i < 20; i++) toolbox_add_item(&t, h2);

    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);

    for (int vi = 0; vi < vis.count; vi++) {
        toolbox_vis_entry_t *e = &vis.entries[vi];
        if (!e->visible || !e->expanded) continue;

        float cat_top = e->screen_y + 24.0f; /* below header */
        float cat_bot = e->screen_y + e->height;

        for (int j = 0; j < e->render_count; j++) {
            toolbox_item_pos_t p = toolbox_vis_item_screen_pos(&t, e, j);
            assert(p.y >= cat_top - 0.01f);
            assert(p.y + t.item_h <= cat_bot + 0.01f);
        }
    }
    toolbox_destroy(&t);
}

TEST(test_category_regions_no_overlap) {
    /* Each category's screen_y region must not overlap the next category's */
    toolbox_t t;
    make_tb(&t);
    for (int i = 0; i < 4; i++) {
        toolbox_category_handle_t h = toolbox_add_category(&t, 24);
        toolbox_set_category_expanded(&t, h, 1);
        for (int j = 0; j < 15; j++) toolbox_add_item(&t, h);
    }
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 4);

    for (int i = 0; i < vis.count - 1; i++) {
        float bot = vis.entries[i].screen_y + vis.entries[i].height;
        float top_next = vis.entries[i + 1].screen_y;
        assert(top_next >= bot - 0.01f);
    }
    toolbox_destroy(&t);
}

TEST(test_cross_category_items_no_y_overlap) {
    /* Items from category A must not share Y-extents with items from category B */
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    for (int i = 0; i < 20; i++) toolbox_add_item(&t, h1);

    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h2, 1);
    for (int i = 0; i < 20; i++) toolbox_add_item(&t, h2);

    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 2);

    /* Find max item bottom of cat 0, min item top of cat 1 */
    float max_bot_0 = -99999;
    for (int j = 0; j < vis.entries[0].render_count; j++) {
        toolbox_item_pos_t p = toolbox_vis_item_screen_pos(&t, &vis.entries[0], j);
        float bot = p.y + t.item_h;
        if (bot > max_bot_0) max_bot_0 = bot;
    }
    float min_top_1 = 99999;
    for (int j = 0; j < vis.entries[1].render_count; j++) {
        toolbox_item_pos_t p = toolbox_vis_item_screen_pos(&t, &vis.entries[1], j);
        if (p.y < min_top_1) min_top_1 = p.y;
    }
    assert(min_top_1 > max_bot_0);
    toolbox_destroy(&t);
}

TEST(test_positions_unique_at_every_scroll_nonvirtual) {
    /* Sweep scroll positions on non-virtual categories; all positions unique */
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    for (int i = 0; i < 30; i++) toolbox_add_item(&t, h1);

    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h2, 1);
    for (int i = 0; i < 30; i++) toolbox_add_item(&t, h2);

    toolbox_rebuild_layout(&t);

    for (float s = 0; s < t.total_content_h; s += 13.0f) {
        toolbox_scroll_to(&t, s);
        toolbox_rebuild_layout(&t);

        toolbox_vis_list_t vis;
        toolbox_query_visible(&t, &vis);

        pos_record_t buf[256];
        int n = collect_all_positions(&t, &vis, buf, 256);
        assert_no_duplicate_positions(buf, n);
    }
    toolbox_destroy(&t);
}

TEST(test_positions_unique_at_every_scroll_virtual) {
    /* Same as above but with virtual categories */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    toolbox_set_virtual_items(&t, h1, 500, 32, mock_provider, NULL);

    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h2, 1);
    toolbox_set_virtual_items(&t, h2, 500, 32, mock_provider, NULL);

    toolbox_rebuild_layout(&t);

    for (float s = 0; s < 3000 && s < t.total_content_h; s += 47.0f) {
        toolbox_scroll_to(&t, s);
        toolbox_rebuild_layout(&t);

        toolbox_vis_list_t vis;
        toolbox_query_visible(&t, &vis);

        pos_record_t buf[256];
        int n = collect_all_positions(&t, &vis, buf, 256);
        assert_no_duplicate_positions(buf, n);
    }
    toolbox_destroy(&t);
}

TEST(test_virtual_item_origin_matches_logical_pos) {
    /* Pool slot 0's screen Y must match the logical grid position of first_item */
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 5000, 64, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    /* Scroll to get first_item > 0 */
    toolbox_scroll_to(&t, 300);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count >= 1);
    toolbox_vis_entry_t *e = &vis.entries[0];
    assert(e->first_item > 0);

    /* Pool slot 0 should render at the logical grid position of first_item */
    toolbox_item_pos_t actual = toolbox_vis_item_screen_pos(&t, e, 0);

    float base_origin_y = e->screen_y + 24.0f; /* header */
    toolbox_item_pos_t logical = toolbox_item_position(
        e->first_item, e->cols, t.item_w, t.item_h, t.item_gap, t.padding);
    float expected_y = base_origin_y + logical.y;
    assert(approx(actual.y, expected_y));
    toolbox_destroy(&t);
}

TEST(test_virtual_cross_cat_no_overlap_at_boundary) {
    /* Two virtual categories: items must not overlap at the boundary scroll offset */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    toolbox_set_virtual_items(&t, h1, 1000, 32, mock_provider, NULL);

    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h2, 1);
    toolbox_set_virtual_items(&t, h2, 1000, 32, mock_provider, NULL);

    toolbox_rebuild_layout(&t);

    /* Scroll to the exact boundary between cat 1 and cat 2 */
    float cat1_virtual_h = t.categories[0].virtual_content_h + 24.0f;
    float offsets[] = {
        cat1_virtual_h - 100, cat1_virtual_h - 50, cat1_virtual_h,
        cat1_virtual_h + 50, cat1_virtual_h + 100
    };

    for (int oi = 0; oi < 5; oi++) {
        toolbox_scroll_to(&t, offsets[oi]);
        toolbox_rebuild_layout(&t);

        toolbox_vis_list_t vis;
        toolbox_query_visible(&t, &vis);

        pos_record_t buf[256];
        int n = collect_all_positions(&t, &vis, buf, 256);
        assert_no_duplicate_positions(buf, n);

        /* Also check category regions don't overlap */
        for (int i = 0; i < vis.count - 1; i++) {
            float bot = vis.entries[i].screen_y + vis.entries[i].height;
            assert(vis.entries[i + 1].screen_y >= bot - 0.01f);
        }
    }
    toolbox_destroy(&t);
}

TEST(test_mixed_virtual_nonvirtual_no_dup) {
    /* Mix non-virtual and virtual categories, verify no duplicates */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    for (int i = 0; i < 25; i++) toolbox_add_item(&t, h1);

    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h2, 1);
    toolbox_set_virtual_items(&t, h2, 2000, 32, mock_provider, NULL);

    toolbox_category_handle_t h3 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h3, 1);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h3);

    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 3);

    pos_record_t buf[512];
    int n = collect_all_positions(&t, &vis, buf, 512);
    assert_no_duplicate_positions(buf, n);

    /* Category regions must not overlap */
    for (int i = 0; i < vis.count - 1; i++) {
        if (!vis.entries[i].visible || !vis.entries[i + 1].visible) continue;
        float bot = vis.entries[i].screen_y + vis.entries[i].height;
        assert(vis.entries[i + 1].screen_y >= bot - 0.01f);
    }
    toolbox_destroy(&t);
}

TEST(test_render_count_matches_vis_count) {
    /* render_count must equal vis_count for virtual categories, not pool_size */
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 5000, 64, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    toolbox_cat_t *cat = &t.categories[0];
    assert(vis.entries[0].render_count == cat->vis_count);
    assert(vis.entries[0].render_count <= cat->pool_size);
    toolbox_destroy(&t);
}

TEST(test_collapsed_cat_no_items_rendered) {
    /* Collapsed category must have render_count == 0, and its region must
       not overlap adjacent expanded categories' items */
    toolbox_t t;
    make_tb(&t);

    toolbox_category_handle_t h1 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h1, 1);
    for (int i = 0; i < 15; i++) toolbox_add_item(&t, h1);

    toolbox_category_handle_t h2 = toolbox_add_category(&t, 24);
    /* h2 stays collapsed */
    for (int i = 0; i < 15; i++) toolbox_add_item(&t, h2);

    toolbox_category_handle_t h3 = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h3, 1);
    for (int i = 0; i < 15; i++) toolbox_add_item(&t, h3);

    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    assert(vis.count == 3);

    /* Collapsed cat must have render_count 0 */
    assert(vis.entries[1].render_count == 0);
    assert(!vis.entries[1].expanded);

    /* Cat 1 (collapsed) region is just its header */
    assert(approx(vis.entries[1].height, 24.0f));

    /* Cat 3 items must not overlap cat 1 header region */
    float collapsed_bot = vis.entries[1].screen_y + vis.entries[1].height;
    assert(vis.entries[2].screen_y >= collapsed_bot - 0.01f);

    /* All rendered items (from cat 1 and 3) must be unique */
    pos_record_t buf[128];
    int n = collect_all_positions(&t, &vis, buf, 128);
    assert_no_duplicate_positions(buf, n);
    toolbox_destroy(&t);
}

TEST(test_virtual_items_contiguous_grid) {
    /* Virtual items: pool slots 0..N must map to a contiguous grid block
       starting at first_item's logical row/col position. No gaps or overlaps. */
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 5000, 64, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    /* Scroll to get a non-zero first_item */
    toolbox_scroll_to(&t, 500);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    toolbox_vis_entry_t *e = &vis.entries[0];
    assert(e->first_item > 0);
    assert(e->render_count > 1);

    /* Check that pool slots map to sequential grid positions matching
       logical items first_item, first_item+1, ... */
    for (int j = 0; j < e->render_count; j++) {
        toolbox_item_pos_t actual = toolbox_vis_item_screen_pos(&t, e, j);

        /* Expected: base_origin + position of logical item (first_item + j) */
        float base_y = e->screen_y + 24.0f;
        toolbox_item_pos_t logical = toolbox_item_position(
            e->first_item + j, e->cols, t.item_w, t.item_h, t.item_gap, t.padding);
        float expected_x = e->item_origin_x + logical.x;
        float expected_y = base_y + logical.y;

        assert(approx(actual.x, expected_x));
        assert(approx(actual.y, expected_y));
    }
    toolbox_destroy(&t);
}

TEST(test_sweep_full_scroll_range_5_cats) {
    /* 5 mixed categories, sweep the entire scroll range at fine granularity */
    toolbox_t t;
    make_tb(&t);
    reset_provider();

    /* Cat 0: 30 non-virtual items */
    toolbox_category_handle_t cats[5];
    cats[0] = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, cats[0], 1);
    for (int j = 0; j < 30; j++) toolbox_add_item(&t, cats[0]);

    /* Cat 1: 800 virtual items */
    cats[1] = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, cats[1], 1);
    toolbox_set_virtual_items(&t, cats[1], 800, 32, mock_provider, NULL);

    /* Cat 2: collapsed, 20 items */
    cats[2] = toolbox_add_category(&t, 24);
    for (int j = 0; j < 20; j++) toolbox_add_item(&t, cats[2]);

    /* Cat 3: 10 non-virtual items */
    cats[3] = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, cats[3], 1);
    for (int j = 0; j < 10; j++) toolbox_add_item(&t, cats[3]);

    /* Cat 4: 2000 virtual items */
    cats[4] = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, cats[4], 1);
    toolbox_set_virtual_items(&t, cats[4], 2000, 48, mock_provider, NULL);

    toolbox_rebuild_layout(&t);

    float step = 31.0f; /* prime-ish step for better coverage */
    for (float s = 0; s <= t.total_content_h; s += step) {
        toolbox_scroll_to(&t, s);
        toolbox_rebuild_layout(&t);

        toolbox_vis_list_t vis;
        toolbox_query_visible(&t, &vis);
        assert(vis.count == 5);

        pos_record_t buf[512];
        int n = collect_all_positions(&t, &vis, buf, 512);
        assert_no_duplicate_positions(buf, n);

        /* Category ordering: each next visible category starts after previous */
        for (int i = 0; i < vis.count - 1; i++) {
            float bot = vis.entries[i].screen_y + vis.entries[i].height;
            assert(vis.entries[i + 1].screen_y >= bot - 0.01f);
        }
    }
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  19. Drag-and-Drop
 * ═══════════════════════════════════════════════════════════════ */

/* Drop-callback tracking */
typedef struct {
    int              called;
    int              item_index;
    int              cat_handle;
    float            x, y;
} drop_record_t;

static drop_record_t g_drop;

static void mock_drop_cb(void *tb, toolbox_category_handle_t cat,
                         int item_index, float x, float y, void *user) {
    (void)tb;
    drop_record_t *r = (drop_record_t *)user;
    r->called = 1;
    r->cat_handle = cat;
    r->item_index = item_index;
    r->x = x;
    r->y = y;
}

TEST(test_drag_idle_by_default) {
    toolbox_t t;
    make_tb(&t);
    assert(t.drag.phase == TOOLBOX_DRAG_IDLE);
    assert(!toolbox_is_dragging(&t));
    toolbox_destroy(&t);
}

TEST(test_drag_begin_sets_pending) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);

    toolbox_drag_begin(&t, h, 3, 3, 100, 200);
    assert(t.drag.phase == TOOLBOX_DRAG_PENDING);
    assert(t.drag.src_category == h);
    assert(t.drag.src_item_index == 3);
    assert(t.drag.src_pool_slot == 3);
    assert(approx(t.drag.start_x, 100));
    assert(approx(t.drag.start_y, 200));
    assert(!toolbox_is_dragging(&t));
    toolbox_destroy(&t);
}

TEST(test_drag_threshold_not_exceeded) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 0, 0, 100, 200);

    /* Move less than threshold */
    int active = toolbox_drag_move(&t, 102, 201);
    assert(!active);
    assert(t.drag.phase == TOOLBOX_DRAG_PENDING);
    toolbox_destroy(&t);
}

TEST(test_drag_threshold_exceeded) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 5, 5, 100, 200);

    /* Move past threshold */
    int active = toolbox_drag_move(&t, 110, 200);
    assert(active);
    assert(t.drag.phase == TOOLBOX_DRAG_ACTIVE);
    assert(toolbox_is_dragging(&t));
    toolbox_destroy(&t);
}

TEST(test_drag_end_fires_callback) {
    toolbox_t t;
    make_tb(&t);
    memset(&g_drop, 0, sizeof(g_drop));
    toolbox_set_drop_callback(&t, mock_drop_cb, &g_drop);

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 7, 7, 50, 50);
    toolbox_drag_move(&t, 100, 100); /* exceed threshold */
    assert(toolbox_is_dragging(&t));

    int dropped = toolbox_drag_end(&t, 300, 400);
    assert(dropped);
    assert(g_drop.called);
    assert(g_drop.cat_handle == h);
    assert(g_drop.item_index == 7);
    assert(approx(g_drop.x, 300));
    assert(approx(g_drop.y, 400));
    assert(t.drag.phase == TOOLBOX_DRAG_IDLE);
    toolbox_destroy(&t);
}

TEST(test_drag_end_without_active_no_callback) {
    toolbox_t t;
    make_tb(&t);
    memset(&g_drop, 0, sizeof(g_drop));
    toolbox_set_drop_callback(&t, mock_drop_cb, &g_drop);

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 0, 0, 50, 50);
    /* Don't move past threshold */
    toolbox_drag_move(&t, 52, 51);

    int dropped = toolbox_drag_end(&t, 52, 51);
    assert(!dropped);
    assert(!g_drop.called);
    assert(t.drag.phase == TOOLBOX_DRAG_IDLE);
    toolbox_destroy(&t);
}

TEST(test_drag_cancel) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 2, 2, 50, 50);
    toolbox_drag_move(&t, 200, 200); /* active */
    assert(toolbox_is_dragging(&t));

    toolbox_drag_cancel(&t);
    assert(t.drag.phase == TOOLBOX_DRAG_IDLE);
    assert(!toolbox_is_dragging(&t));
    toolbox_destroy(&t);
}

TEST(test_drag_cancel_no_callback) {
    toolbox_t t;
    make_tb(&t);
    memset(&g_drop, 0, sizeof(g_drop));
    toolbox_set_drop_callback(&t, mock_drop_cb, &g_drop);

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 2, 2, 50, 50);
    toolbox_drag_move(&t, 200, 200);
    toolbox_drag_cancel(&t);
    assert(!g_drop.called);
    toolbox_destroy(&t);
}

TEST(test_drag_no_double_begin) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 1, 1, 50, 50);
    /* Second begin should be ignored while pending */
    toolbox_drag_begin(&t, h, 9, 9, 300, 300);
    assert(t.drag.src_item_index == 1); /* unchanged */
    toolbox_destroy(&t);
}

TEST(test_drag_get_state) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 4, 4, 60, 70);
    toolbox_drag_move(&t, 80, 90);

    const toolbox_drag_state_t *s = toolbox_get_drag_state(&t);
    assert(s->phase == TOOLBOX_DRAG_ACTIVE);
    assert(s->src_item_index == 4);
    assert(approx(s->cur_x, 80));
    assert(approx(s->cur_y, 90));
    assert(approx(s->item_w, t.item_w));
    assert(approx(s->item_h, t.item_h));
    toolbox_destroy(&t);
}

TEST(test_drag_from_virtual_category) {
    toolbox_t t;
    make_tb(&t);
    reset_provider();
    memset(&g_drop, 0, sizeof(g_drop));
    toolbox_set_drop_callback(&t, mock_drop_cb, &g_drop);

    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    toolbox_set_virtual_items(&t, h, 5000, 32, mock_provider, NULL);
    toolbox_rebuild_layout(&t);

    /* Scroll to get a nonzero first_item */
    toolbox_scroll_to(&t, 400);
    toolbox_rebuild_layout(&t);

    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    int first = vis.entries[0].first_item;
    assert(first > 0);

    /* Drag logical item first+5, pool slot 5 */
    toolbox_drag_begin(&t, h, first + 5, 5, 50, 50);
    toolbox_drag_move(&t, 300, 300);
    toolbox_drag_end(&t, 500, 500);

    assert(g_drop.called);
    assert(g_drop.item_index == first + 5);
    assert(g_drop.cat_handle == h);
    toolbox_destroy(&t);
}

TEST(test_hit_test_item) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 10; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    /* Get the position of item 0 */
    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    toolbox_item_pos_t p0 = toolbox_vis_item_screen_pos(&t, &vis.entries[0], 0);

    toolbox_category_handle_t cat;
    int item, slot;
    /* Hit the center of item 0 */
    int hit = toolbox_hit_test_item(&t, p0.x + t.item_w / 2,
                                    p0.y + t.item_h / 2, &cat, &item, &slot);
    assert(hit);
    assert(cat == h);
    assert(item == 0);
    assert(slot == 0);

    /* Miss: empty space */
    hit = toolbox_hit_test_item(&t, 0, 0, &cat, &item, &slot);
    assert(!hit);
    toolbox_destroy(&t);
}

TEST(test_hit_test_item_second_row) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_set_category_expanded(&t, h, 1);
    for (int i = 0; i < 20; i++) toolbox_add_item(&t, h);
    toolbox_rebuild_layout(&t);

    /* With W=300, item_w=48, gap=4, padding=4: cols=5
       Item 7 is row 1, col 2 */
    toolbox_vis_list_t vis;
    toolbox_query_visible(&t, &vis);
    toolbox_item_pos_t p7 = toolbox_vis_item_screen_pos(&t, &vis.entries[0], 7);

    toolbox_category_handle_t cat;
    int item, slot;
    int hit = toolbox_hit_test_item(&t, p7.x + 1, p7.y + 1, &cat, &item, &slot);
    assert(hit);
    assert(cat == h);
    assert(item == 7);
    assert(slot == 7);
    toolbox_destroy(&t);
}

TEST(test_drag_preserves_item_size) {
    toolbox_t t;
    make_tb(&t);
    toolbox_set_item_size(&t, 64, 80);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);

    toolbox_drag_begin(&t, h, 0, 0, 10, 10);
    assert(approx(t.drag.item_w, 64));
    assert(approx(t.drag.item_h, 80));
    toolbox_destroy(&t);
}

TEST(test_drag_end_resets_state_fully) {
    toolbox_t t;
    make_tb(&t);
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 3, 3, 50, 50);
    toolbox_drag_move(&t, 200, 200);
    toolbox_drag_end(&t, 200, 200);

    /* Every field should be zeroed */
    assert(t.drag.phase == TOOLBOX_DRAG_IDLE);
    assert(t.drag.src_category == 0);
    assert(t.drag.src_item_index == 0);
    assert(t.drag.start_x == 0);
    assert(t.drag.cur_x == 0);
    toolbox_destroy(&t);
}

TEST(test_drop_callback_null_safe) {
    toolbox_t t;
    make_tb(&t);
    /* No callback set */
    toolbox_category_handle_t h = toolbox_add_category(&t, 24);
    toolbox_drag_begin(&t, h, 0, 0, 50, 50);
    toolbox_drag_move(&t, 200, 200);
    int dropped = toolbox_drag_end(&t, 200, 200);
    /* Should still report "dropped" even without callback */
    assert(dropped);
    assert(t.drag.phase == TOOLBOX_DRAG_IDLE);
    toolbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("=== toolbox.h test suite ===\n\n");

    printf("[Lifecycle]\n");
    RUN(test_init_defaults);
    RUN(test_init_horizontal);
    RUN(test_init_zero_header);
    RUN(test_destroy_zeroes);

    printf("\n[Categories]\n");
    RUN(test_add_category);
    RUN(test_add_multiple_categories);
    RUN(test_add_max_categories);
    RUN(test_remove_category);
    RUN(test_remove_and_reuse_slot);
    RUN(test_remove_invalid_handle);
    RUN(test_category_expand_collapse);
    RUN(test_category_toggle);

    printf("\n[Items]\n");
    RUN(test_add_items);
    RUN(test_remove_item);
    RUN(test_add_item_max);
    RUN(test_remove_item_invalid);
    RUN(test_item_count_invalid_handle);

    printf("\n[Virtual Items]\n");
    RUN(test_set_virtual_items);
    RUN(test_virtual_pool_size_clamp);
    RUN(test_virtual_null_provider);

    printf("\n[Expand/State]\n");
    RUN(test_root_expand);

    printf("\n[Geometry]\n");
    RUN(test_set_size);
    RUN(test_set_item_size);
    RUN(test_set_item_size_zero_defaults);
    RUN(test_set_gap);
    RUN(test_effective_size_collapsed_vertical);
    RUN(test_effective_size_expanded_vertical);
    RUN(test_effective_size_collapsed_horizontal);
    RUN(test_content_bounds_vertical);
    RUN(test_content_bounds_horizontal);

    printf("\n[Grid Computation]\n");
    RUN(test_compute_columns);
    RUN(test_compute_rows);
    RUN(test_compute_content_height);
    RUN(test_item_position);

    printf("\n[Visible Range]\n");
    RUN(test_visible_range_basic);
    RUN(test_visible_range_scrolled);
    RUN(test_visible_range_at_end);
    RUN(test_visible_range_empty);
    RUN(test_visible_range_single_item);

    printf("\n[Apply Visible Range]\n");
    RUN(test_apply_visible_range);
    RUN(test_apply_no_rotation);

    printf("\n[Layout Rebuild]\n");
    RUN(test_rebuild_single_category);
    RUN(test_rebuild_virtual_category);
    RUN(test_rebuild_collapsed_no_content);
    RUN(test_rebuild_multiple_categories);
    RUN(test_rebuild_not_expanded_noop);

    printf("\n[Virtual Y Positions]\n");
    RUN(test_virtual_y_ordering);
    RUN(test_virtual_y_first_is_zero);

    printf("\n[Scroll]\n");
    RUN(test_scroll_clamp_zero);
    RUN(test_scroll_clamp_max);
    RUN(test_scroll_by);
    RUN(test_max_scroll_no_overflow);

    printf("\n[Render Query]\n");
    RUN(test_query_not_expanded);
    RUN(test_query_single_collapsed_cat);
    RUN(test_query_single_expanded_cat);
    RUN(test_query_screen_y_reflects_scroll);
    RUN(test_query_multiple_cats_ordering);
    RUN(test_query_visibility_culling);
    RUN(test_query_content_bounds);

    printf("\n[Vis Item Screen Pos]\n");
    RUN(test_vis_item_screen_pos_basic);
    RUN(test_vis_item_screen_pos_second_row);

    printf("\n[MASSIVE SCALE STRESS TESTS]\n");
    RUN(test_massive_virtual_single_category);
    RUN(test_massive_virtual_scrolled);
    RUN(test_massive_many_categories);
    RUN(test_massive_scroll_through_all);
    RUN(test_massive_50k_items_single_cat);
    RUN(test_massive_content_height_consistency);
    RUN(test_massive_no_items_overdraw_viewport);
    RUN(test_massive_provider_rotation);
    RUN(test_massive_mixed_virtual_nonvirtual);
    RUN(test_massive_narrow_toolbox);
    RUN(test_massive_wide_toolbox);
    RUN(test_massive_all_collapsed);

    printf("\n[Edge Cases]\n");
    RUN(test_empty_toolbox_query);
    RUN(test_single_item_category);
    RUN(test_zero_width_toolbox);
    RUN(test_resize_triggers_dirty);
    RUN(test_virtual_1_item);
    RUN(test_scroll_marks_virtual_dirty);
    RUN(test_scroll_then_expand);

    printf("\n[Render Position Invariants]\n");
    RUN(test_render_positions_no_overlap);
    RUN(test_render_header_before_items);

    printf("\n[Layout Correctness — Duplicate Detection]\n");
    RUN(test_no_dup_positions_multi_cat);
    RUN(test_items_within_category_bounds);
    RUN(test_category_regions_no_overlap);
    RUN(test_cross_category_items_no_y_overlap);
    RUN(test_positions_unique_at_every_scroll_nonvirtual);
    RUN(test_positions_unique_at_every_scroll_virtual);
    RUN(test_virtual_item_origin_matches_logical_pos);
    RUN(test_virtual_cross_cat_no_overlap_at_boundary);
    RUN(test_mixed_virtual_nonvirtual_no_dup);
    RUN(test_render_count_matches_vis_count);
    RUN(test_collapsed_cat_no_items_rendered);
    RUN(test_virtual_items_contiguous_grid);
    RUN(test_sweep_full_scroll_range_5_cats);

    printf("\n[Drag-and-Drop]\n");
    RUN(test_drag_idle_by_default);
    RUN(test_drag_begin_sets_pending);
    RUN(test_drag_threshold_not_exceeded);
    RUN(test_drag_threshold_exceeded);
    RUN(test_drag_end_fires_callback);
    RUN(test_drag_end_without_active_no_callback);
    RUN(test_drag_cancel);
    RUN(test_drag_cancel_no_callback);
    RUN(test_drag_no_double_begin);
    RUN(test_drag_get_state);
    RUN(test_drag_from_virtual_category);
    RUN(test_hit_test_item);
    RUN(test_hit_test_item_second_row);
    RUN(test_drag_preserves_item_size);
    RUN(test_drag_end_resets_state_fully);
    RUN(test_drop_callback_null_safe);

    printf("\n=== ALL 111 TESTS PASSED ===\n");
    return 0;
}
