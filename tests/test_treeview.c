/*
 * test_treeview.c - Comprehensive tests for textbox.h tree-view scenarios: hierarchical layout, expansion, selection, and edit operations.
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
 * @file test_textbox.c
 * @brief Comprehensive tests for textbox.h — platform-independent styled text editor widget.
 */
#define UNISTRING_IMPLEMENTATION
#define TEXTBOX_IMPLEMENTATION
#include "textbox.h"

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-60s", #name); name(); printf("PASS\n"); } while(0)

/* ═══════════════════════════════════════════════════════════════
 *  Mock render services
 * ═══════════════════════════════════════════════════════════════ */

/* Fixed-width measure: each codepoint = 8px */
static float mock_measure(uint32_t codepoint, float font_size, void *user) {
    (void)codepoint; (void)font_size; (void)user;
    return 8.0f;
}

/* Styled measure: bold = 10px, everything else = 8px */
static float mock_styled_measure(uint32_t codepoint, uint16_t style,
                                  float font_size, void *user) {
    (void)codepoint; (void)font_size; (void)user;
    return (style & TEXTBOX_STYLE_BOLD) ? 10.0f : 8.0f;
}

static int mock_shape(const uint32_t *codepoints, int count, float font_size,
                      int direction, textbox_shaped_glyph_t *out, int max_out,
                      void *user) {
    (void)font_size; (void)direction; (void)user;
    int n = count < max_out ? count : max_out;
    for (int i = 0; i < n; i++) {
        out[i].glyph_index = codepoints[i];
        out[i].x_advance = 8.0f;
        out[i].x_offset = 0;
        out[i].y_offset = 0;
        out[i].cluster = (uint32_t)i;
    }
    return n;
}

static textbox_render_services_t mock_svc = {
    .measure = mock_measure,
    .styled_measure = NULL,
    .shape = mock_shape,
    .styled_shape = NULL,
    .user = NULL
};

static int mock_styled_shape(const uint32_t *codepoints, int count, uint16_t style,
                             float font_size, int direction,
                             textbox_shaped_glyph_t *out, int max_out, void *user) {
    (void)font_size; (void)direction; (void)user;
    int n = count < max_out ? count : max_out;
    for (int i = 0; i < n; i++) {
        out[i].glyph_index = codepoints[i];
        out[i].x_advance = (style & TEXTBOX_STYLE_BOLD) ? 10.0f : 8.0f;
        out[i].x_offset = 0;
        out[i].y_offset = 0;
        out[i].cluster = (uint32_t)i;
    }
    return n;
}

static textbox_render_services_t mock_styled_svc = {
    .measure = mock_measure,
    .styled_measure = mock_styled_measure,
    .shape = mock_shape,
    .styled_shape = mock_styled_shape,
    .user = NULL
};

static void make_tb(textbox_t *t, float w, float h) {
    textbox_create(t, w, h, 14.0f, 20.0f, 4.0f, mock_measure, NULL);
    textbox_set_render_services(t, &mock_svc);
}

/* ═══════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_create_destroy) {
    textbox_t t;
    make_tb(&t, 300, 200);
    assert(textbox_len(&t) == 0);
    assert(textbox_has_selection(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_create_defaults) {
    textbox_t t;
    make_tb(&t, 300, 200);
    assert(t.font_size == 14.0f);
    assert(t.line_height == 20.0f);
    assert(t.padding == 4.0f);
    assert(t.box_w == 300.0f);
    assert(t.box_h == 200.0f);
    assert(t.word_wrap == 1);
    assert(t.read_only == 0);
    assert(t.current_style == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_set_render_services) {
    textbox_t t;
    make_tb(&t, 300, 200);
    assert(t.render == &mock_svc);
    textbox_set_render_services(&t, NULL);
    assert(t.render == NULL);
    textbox_set_render_services(&t, &mock_svc);
    assert(t.render == &mock_svc);
    textbox_destroy(&t);
}

TEST(test_set_text_services) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text_services(&t, NULL);
    assert(t.layout_dirty == 1);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Basic Editing
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_insert_codepoint) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_insert_codepoint(&t, 'A');
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == 'A');
    textbox_insert_codepoint(&t, 'B');
    assert(textbox_len(&t) == 2);
    assert(textbox_char_at(&t, 1) == 'B');
    textbox_destroy(&t);
}

TEST(test_insert_codepoints) {
    textbox_t t;
    make_tb(&t, 300, 200);
    uint32_t cp[] = {'H', 'e', 'l', 'l', 'o'};
    textbox_insert_codepoints(&t, cp, 5);
    assert(textbox_len(&t) == 5);
    assert(textbox_char_at(&t, 0) == 'H');
    assert(textbox_char_at(&t, 4) == 'o');
    textbox_destroy(&t);
}

TEST(test_insert_utf8) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_insert_utf8(&t, "Hello", 5);
    assert(textbox_len(&t) == 5);
    assert(textbox_char_at(&t, 0) == 'H');
    assert(textbox_char_at(&t, 4) == 'o');
    textbox_destroy(&t);
}

TEST(test_insert_styled) {
    textbox_t t;
    make_tb(&t, 300, 200);
    uint32_t cp[] = {'A', 'B', 'C'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_ITALIC, TEXTBOX_STYLE_NONE};
    textbox_insert_styled(&t, cp, st, 3);
    assert(textbox_len(&t) == 3);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(&t, 2) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_set_text) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    assert(textbox_len(&t) == 11);
    textbox_set_text(&t, "AB");
    assert(textbox_len(&t) == 2);
    assert(textbox_char_at(&t, 0) == 'A');
    assert(textbox_char_at(&t, 1) == 'B');
    textbox_destroy(&t);
}

TEST(test_set_text_null) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    assert(textbox_len(&t) == 5);
    textbox_set_text(&t, NULL);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_clear) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    assert(textbox_len(&t) == 11);
    textbox_clear(&t);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_delete_back) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "ABC");
    textbox_set_cursor(&t, 3, 0);
    textbox_delete_back(&t, 1);
    assert(textbox_len(&t) == 2);
    assert(textbox_char_at(&t, 0) == 'A');
    assert(textbox_char_at(&t, 1) == 'B');
    textbox_destroy(&t);
}

TEST(test_delete_forward) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "ABC");
    textbox_set_cursor(&t, 0, 0);
    textbox_delete_forward(&t, 1);
    assert(textbox_len(&t) == 2);
    assert(textbox_char_at(&t, 0) == 'B');
    assert(textbox_char_at(&t, 1) == 'C');
    textbox_destroy(&t);
}

TEST(test_delete_selection) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 5, 1); /* select "Hello" */
    assert(textbox_has_selection(&t));
    textbox_delete_selection(&t);
    assert(textbox_len(&t) == 6); /* " World" */
    assert(textbox_char_at(&t, 0) == ' ');
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Unicode / UTF-8
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_insert_unicode) {
    textbox_t t;
    make_tb(&t, 300, 200);
    /* "Héllo" — é is U+00E9, 2-byte UTF-8 */
    textbox_insert_utf8(&t, "H\xc3\xa9llo", 6);
    assert(textbox_len(&t) == 5);
    assert(textbox_char_at(&t, 0) == 'H');
    assert(textbox_char_at(&t, 1) == 0xE9);
    assert(textbox_char_at(&t, 2) == 'l');
    textbox_destroy(&t);
}

TEST(test_get_utf8) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    char buf[64];
    int n = textbox_get_utf8(&t, buf, sizeof(buf));
    assert(n == 5);
    assert(strcmp(buf, "Hello") == 0);
    textbox_destroy(&t);
}

TEST(test_get_text_codepoints) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "ABC");
    int len;
    const uint32_t *cp = textbox_get_text(&t, &len);
    assert(len == 3);
    assert(cp[0] == 'A');
    assert(cp[1] == 'B');
    assert(cp[2] == 'C');
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Cursor & Selection
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_cursor_basic) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 0, 0);
    assert(t.cursor == 0);
    assert(t.anchor == 0);
    textbox_set_cursor(&t, 3, 0);
    assert(t.cursor == 3);
    assert(t.anchor == 3);
    textbox_destroy(&t);
}

TEST(test_cursor_clamp) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hi");
    textbox_set_cursor(&t, 100, 0);
    assert(t.cursor == 2);
    textbox_set_cursor(&t, -5, 0);
    assert(t.cursor == 0);
    textbox_destroy(&t);
}

TEST(test_selection_extend) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_set_cursor(&t, 2, 0);
    textbox_set_cursor(&t, 7, 1); /* extend selection */
    assert(textbox_has_selection(&t));
    int ss, se;
    textbox_selection_range(&t, &ss, &se);
    assert(ss == 2);
    assert(se == 7);
    textbox_destroy(&t);
}

TEST(test_select_all) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_select_all(&t);
    assert(textbox_has_selection(&t));
    int ss, se;
    textbox_selection_range(&t, &ss, &se);
    assert(ss == 0);
    assert(se == 5);
    textbox_destroy(&t);
}

TEST(test_move_left_right) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "ABCDE");
    textbox_set_cursor(&t, 2, 0);
    textbox_move_left(&t, 0);
    assert(t.cursor == 1);
    textbox_move_right(&t, 0);
    assert(t.cursor == 2);
    textbox_destroy(&t);
}

TEST(test_move_left_at_start) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hi");
    textbox_set_cursor(&t, 0, 0);
    textbox_move_left(&t, 0);
    assert(t.cursor == 0);
    textbox_destroy(&t);
}

TEST(test_move_right_at_end) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hi");
    textbox_set_cursor(&t, 2, 0);
    textbox_move_right(&t, 0);
    assert(t.cursor == 2);
    textbox_destroy(&t);
}

TEST(test_move_left_with_extend) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "ABCDE");
    textbox_set_cursor(&t, 3, 0);
    textbox_move_left(&t, 1);
    assert(t.cursor == 2);
    assert(t.anchor == 3);
    assert(textbox_has_selection(&t));
    textbox_destroy(&t);
}

TEST(test_move_up_down) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Line1\nLine2\nLine3");
    textbox_set_cursor(&t, 2, 0); /* middle of Line1 */
    textbox_move_down(&t, 0);
    assert(t.cursor >= 6 && t.cursor <= 11); /* somewhere in Line2 */
    textbox_move_up(&t, 0);
    assert(t.cursor >= 0 && t.cursor <= 5); /* back in Line1 */
    textbox_destroy(&t);
}

TEST(test_move_line_start_end) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_set_cursor(&t, 5, 0);
    textbox_move_line_start(&t, 0);
    assert(t.cursor == 0);
    textbox_move_line_end(&t, 0);
    assert(t.cursor == 11);
    textbox_destroy(&t);
}

TEST(test_move_page_up_down) {
    textbox_t t;
    make_tb(&t, 300, 100);
    /* Create many lines */
    textbox_set_text(&t, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10");
    textbox_set_cursor(&t, 0, 0);
    textbox_move_page_down(&t, 0);
    int after_page = t.cursor;
    assert(after_page > 0);
    textbox_move_page_up(&t, 0);
    assert(t.cursor < after_page);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Selection UTF-8 / Styled export
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_get_selection_utf8) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 5, 1);
    char buf[64];
    int n = textbox_get_selection_utf8(&t, buf, sizeof(buf));
    assert(n == 5);
    assert(strncmp(buf, "Hello", 5) == 0);
    textbox_destroy(&t);
}

TEST(test_get_selection_utf8_no_selection) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    char buf[64];
    int n = textbox_get_selection_utf8(&t, buf, sizeof(buf));
    assert(n == 0);
    textbox_destroy(&t);
}

TEST(test_get_selection_styled) {
    textbox_t t;
    make_tb(&t, 300, 200);
    uint32_t cp[] = {'A', 'B', 'C'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_ITALIC, TEXTBOX_STYLE_NONE};
    textbox_insert_styled(&t, cp, st, 3);
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 3, 1);
    uint32_t co[8];
    uint16_t so[8];
    int n = textbox_get_selection_styled(&t, co, so, 8);
    assert(n == 3);
    assert(co[0] == 'A');
    assert(so[0] == TEXTBOX_STYLE_BOLD);
    assert(co[1] == 'B');
    assert(so[1] == TEXTBOX_STYLE_ITALIC);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Paste
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_paste_utf8) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "AC");
    textbox_set_cursor(&t, 1, 0);
    textbox_paste_utf8(&t, "B", 1);
    assert(textbox_len(&t) == 3);
    assert(textbox_char_at(&t, 0) == 'A');
    assert(textbox_char_at(&t, 1) == 'B');
    assert(textbox_char_at(&t, 2) == 'C');
    textbox_destroy(&t);
}

TEST(test_paste_styled) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "AC");
    textbox_set_cursor(&t, 1, 0);
    uint32_t cp[] = {'X'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD};
    textbox_paste_styled(&t, cp, st, 1);
    assert(textbox_len(&t) == 3);
    assert(textbox_char_at(&t, 1) == 'X');
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_BOLD);
    textbox_destroy(&t);
}

TEST(test_paste_replaces_selection) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 5, 1);
    textbox_paste_utf8(&t, "Bye", 3);
    assert(textbox_len(&t) == 9); /* "Bye World" */
    char buf[64];
    textbox_get_utf8(&t, buf, sizeof(buf));
    assert(strcmp(buf, "Bye World") == 0);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Style System
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_current_style) {
    textbox_t t;
    make_tb(&t, 300, 200);
    assert(textbox_get_current_style(&t) == TEXTBOX_STYLE_NONE);
    textbox_set_current_style(&t, TEXTBOX_STYLE_BOLD);
    assert(textbox_get_current_style(&t) == TEXTBOX_STYLE_BOLD);
    textbox_destroy(&t);
}

TEST(test_toggle_style) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_toggle_style(&t, TEXTBOX_STYLE_BOLD);
    assert(textbox_get_current_style(&t) == TEXTBOX_STYLE_BOLD);
    textbox_toggle_style(&t, TEXTBOX_STYLE_BOLD);
    assert(textbox_get_current_style(&t) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_insert_with_current_style) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_current_style(&t, TEXTBOX_STYLE_BOLD);
    textbox_insert_codepoint(&t, 'A');
    textbox_set_current_style(&t, TEXTBOX_STYLE_ITALIC);
    textbox_insert_codepoint(&t, 'B');
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_ITALIC);
    textbox_destroy(&t);
}

TEST(test_set_range_style) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    /* Make chars 1..3 bold */
    textbox_set_range_style(&t, 1, 3, TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_NONE);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 2) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 3) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_apply_style_to_selection) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 1, 0);
    textbox_set_cursor(&t, 4, 1);
    textbox_apply_style_to_selection(&t, TEXTBOX_STYLE_UNDERLINE, TEXTBOX_STYLE_UNDERLINE);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_NONE);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_UNDERLINE);
    assert(textbox_style_at(&t, 3) == TEXTBOX_STYLE_UNDERLINE);
    assert(textbox_style_at(&t, 4) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_toggle_selection_style) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 5, 1);
    /* Toggle bold on */
    textbox_toggle_selection_style(&t, TEXTBOX_STYLE_BOLD);
    for (int i = 0; i < 5; i++)
        assert(textbox_style_at(&t, i) == TEXTBOX_STYLE_BOLD);
    /* Toggle bold off */
    textbox_toggle_selection_style(&t, TEXTBOX_STYLE_BOLD);
    for (int i = 0; i < 5; i++)
        assert(textbox_style_at(&t, i) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_selection_style_union) {
    textbox_t t;
    make_tb(&t, 300, 200);
    uint32_t cp[] = {'A', 'B', 'C'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_ITALIC, TEXTBOX_STYLE_BOLD | TEXTBOX_STYLE_ITALIC};
    textbox_insert_styled(&t, cp, st, 3);
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 3, 1);
    uint16_t u = textbox_get_selection_style_union(&t);
    assert((u & TEXTBOX_STYLE_BOLD) != 0);
    assert((u & TEXTBOX_STYLE_ITALIC) != 0);
    textbox_destroy(&t);
}

TEST(test_selection_style_intersect) {
    textbox_t t;
    make_tb(&t, 300, 200);
    uint32_t cp[] = {'A', 'B', 'C'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD | TEXTBOX_STYLE_ITALIC,
                     TEXTBOX_STYLE_BOLD,
                     TEXTBOX_STYLE_BOLD | TEXTBOX_STYLE_UNDERLINE};
    textbox_insert_styled(&t, cp, st, 3);
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 3, 1);
    uint16_t inter = textbox_get_selection_style_intersect(&t);
    assert((inter & TEXTBOX_STYLE_BOLD) != 0);
    assert((inter & TEXTBOX_STYLE_ITALIC) == 0);
    assert((inter & TEXTBOX_STYLE_UNDERLINE) == 0);
    textbox_destroy(&t);
}

TEST(test_get_styles) {
    textbox_t t;
    make_tb(&t, 300, 200);
    uint32_t cp[] = {'A', 'B'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_ITALIC};
    textbox_insert_styled(&t, cp, st, 2);
    int len;
    const uint16_t *styles = textbox_get_styles(&t, &len);
    assert(len == 2);
    assert(styles[0] == TEXTBOX_STYLE_BOLD);
    assert(styles[1] == TEXTBOX_STYLE_ITALIC);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Style Runs
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_style_runs) {
    textbox_t t;
    make_tb(&t, 300, 200);
    uint32_t cp[] = {'A', 'B', 'C', 'D', 'E'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_BOLD,
                     TEXTBOX_STYLE_ITALIC, TEXTBOX_STYLE_ITALIC, TEXTBOX_STYLE_ITALIC};
    textbox_insert_styled(&t, cp, st, 5);
    textbox_visible_line_t vl[8];
    int nv = textbox_get_visible_lines(&t, vl, 8);
    assert(nv >= 1);
    textbox_style_run_t runs[8];
    int nr = textbox_get_style_runs(&vl[0], runs, 8);
    assert(nr == 2);
    assert(runs[0].start == 0);
    assert(runs[0].count == 2);
    assert(runs[0].style == TEXTBOX_STYLE_BOLD);
    assert(runs[1].start == 2);
    assert(runs[1].count == 3);
    assert(runs[1].style == TEXTBOX_STYLE_ITALIC);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Layout
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_layout_empty) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_layout(&t);
    assert(t.line_count >= 1);
    textbox_destroy(&t);
}

TEST(test_layout_single_line) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_layout(&t);
    assert(t.line_count >= 1);
    assert(t.lines[0].count == 5);
    textbox_destroy(&t);
}

TEST(test_layout_multiline) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Line1\nLine2\nLine3");
    textbox_layout(&t);
    assert(t.line_count >= 3);
    assert(t.lines[0].hard_break == 1);
    assert(t.lines[0].count == 5);
    textbox_destroy(&t);
}

TEST(test_layout_word_wrap) {
    textbox_t t;
    /* Small width: 8px per char, usable = 60 - 8 padding = 52, ~6 chars fit */
    make_tb(&t, 60, 200);
    textbox_set_word_wrap(&t, 1);
    textbox_set_text(&t, "Hello World Test");
    textbox_layout(&t);
    assert(t.line_count >= 2); /* should wrap */
    textbox_destroy(&t);
}

TEST(test_layout_no_word_wrap) {
    textbox_t t;
    make_tb(&t, 60, 200);
    textbox_set_word_wrap(&t, 0);
    textbox_set_text(&t, "Hello World Test");
    textbox_layout(&t);
    /* Without hard breaks, expect single line + trailing empty */
    assert(t.line_count <= 2);
    textbox_destroy(&t);
}

TEST(test_content_height) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "A\nB\nC");
    textbox_layout(&t);
    /* At least 3 visible lines * 20px line height, plus trailing empty line */
    assert(t.content_height >= 60.0f);
    textbox_destroy(&t);
}

TEST(test_layout_idempotent) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello\nWorld");
    textbox_layout(&t);
    int lc1 = t.line_count;
    float ch1 = t.content_height;
    textbox_layout(&t); /* should be no-op */
    assert(t.line_count == lc1);
    assert(t.content_height == ch1);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Visible Lines
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_visible_line_count) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "A\nB\nC");
    int n = textbox_visible_line_count(&t);
    assert(n >= 3);
    textbox_destroy(&t);
}

TEST(test_get_visible_lines) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello\nWorld");
    textbox_visible_line_t vl[16];
    int nv = textbox_get_visible_lines(&t, vl, 16);
    assert(nv >= 2);
    assert(vl[0].count == 5); /* "Hello" */
    assert(vl[0].codepoints[0] == 'H');
    textbox_destroy(&t);
}

TEST(test_visible_lines_viewport_clip) {
    textbox_t t;
    /* Small viewport: 20px line height, box_h=30, padding=4, usable=22, ~1 line */
    make_tb(&t, 300, 30);
    textbox_set_text(&t, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10");
    textbox_visible_line_t vl[32];
    int nv = textbox_get_visible_lines(&t, vl, 32);
    assert(nv >= 1 && nv <= 3); /* only a few lines visible */
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Scroll
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_scroll_basic) {
    textbox_t t;
    make_tb(&t, 300, 50);
    textbox_set_text(&t, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10");
    float ms = textbox_max_scroll_y(&t);
    assert(ms > 0);
    textbox_scroll_to(&t, ms);
    assert(fabsf(t.scroll_y - ms) < 0.01f);
    textbox_scroll_to(&t, 0);
    assert(t.scroll_y == 0.0f);
    textbox_destroy(&t);
}

TEST(test_scroll_clamp) {
    textbox_t t;
    make_tb(&t, 300, 50);
    textbox_set_text(&t, "L1\nL2\nL3\nL4\nL5");
    textbox_scroll_to(&t, -100.0f);
    assert(t.scroll_y == 0.0f);
    float ms = textbox_max_scroll_y(&t);
    textbox_scroll_to(&t, 99999.0f);
    assert(fabsf(t.scroll_y - ms) < 0.01f);
    textbox_destroy(&t);
}

TEST(test_scroll_by) {
    textbox_t t;
    make_tb(&t, 300, 50);
    textbox_set_text(&t, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10");
    textbox_scroll_to(&t, 0);
    textbox_scroll_by(&t, 0, 20.0f);
    assert(fabsf(t.scroll_y - 20.0f) < 0.01f);
    textbox_destroy(&t);
}

TEST(test_scroll_to_cursor) {
    textbox_t t;
    make_tb(&t, 300, 50);
    textbox_set_text(&t, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10");
    /* Move cursor to end */
    textbox_set_cursor(&t, textbox_len(&t), 0);
    textbox_scroll_to_cursor(&t);
    assert(t.scroll_y > 0);
    textbox_destroy(&t);
}

TEST(test_scroll_no_overflow) {
    textbox_t t;
    make_tb(&t, 300, 400); /* big viewport */
    textbox_set_text(&t, "Short");
    assert(textbox_max_scroll_y(&t) == 0);
    textbox_scroll_to(&t, 10);
    assert(t.scroll_y == 0.0f);
    textbox_destroy(&t);
}

TEST(test_scrollbar_v) {
    textbox_t t;
    make_tb(&t, 300, 50);
    textbox_set_text(&t, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8");
    textbox_scrollbar_t sb;
    textbox_get_scrollbar_v(&t, &sb);
    assert(sb.visible == 1);
    assert(sb.max > 0);
    textbox_set_scrollbar_v(&t, 0.5f);
    assert(t.scroll_y > 0);
    textbox_destroy(&t);
}

TEST(test_scrollbar_h) {
    textbox_t t;
    make_tb(&t, 80, 200);
    textbox_set_word_wrap(&t, 0);
    textbox_set_text(&t, "This is a very long line of text that should overflow horizontally");
    textbox_scrollbar_t sb;
    textbox_get_scrollbar_h(&t, &sb);
    /* May or may not be visible depending on content width */
    if (sb.visible) {
        textbox_set_scrollbar_h(&t, 0.5f);
        assert(t.scroll_x > 0);
    }
    textbox_destroy(&t);
}

TEST(test_max_scroll_x) {
    textbox_t t;
    make_tb(&t, 80, 200);
    textbox_set_word_wrap(&t, 0);
    textbox_set_text(&t, "A very long line that exceeds the viewport width easily");
    float msx = textbox_max_scroll_x(&t);
    /* With word wrap off and narrow viewport, there should be horizontal scroll */
    assert(msx >= 0);
    textbox_scroll_to_x(&t, msx);
    assert(fabsf(t.scroll_x - msx) < 0.01f);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Hit Test
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_hit_test_basic) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "ABCDE");
    textbox_layout(&t);
    /* Click at padding + 0 = beginning */
    int pos = textbox_hit_test(&t, 4.0f, 14.0f);
    assert(pos == 0);
    /* Click far right — should be at or near end */
    pos = textbox_hit_test(&t, 200.0f, 14.0f);
    assert(pos == 5);
    textbox_destroy(&t);
}

TEST(test_hit_test_multiline) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Line1\nLine2");
    textbox_layout(&t);
    /* Click in second line area: y = padding(4) + line_height(20) + something */
    int pos = textbox_hit_test(&t, 4.0f, 30.0f);
    assert(pos >= 6); /* should be in "Line2" */
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Cursor Rect & Selection Rects
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_cursor_rect) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_rect_t cr = textbox_cursor_rect(&t, 2.0f);
    assert(cr.w == 2.0f);
    assert(cr.h == 20.0f); /* line_height */
    assert(fabsf(cr.x - 4.0f) < 0.1f); /* at padding */
    textbox_destroy(&t);
}

TEST(test_cursor_rect_mid_text) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 3, 0);
    textbox_rect_t cr = textbox_cursor_rect(&t, 1.0f);
    /* 3 chars * 8px each + padding(4) = 28 */
    assert(fabsf(cr.x - 28.0f) < 0.1f);
    textbox_destroy(&t);
}

TEST(test_selection_rects) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_set_cursor(&t, 2, 0);
    textbox_set_cursor(&t, 7, 1);
    textbox_rect_t rects[16];
    int nr = textbox_selection_rects(&t, rects, 16);
    assert(nr >= 1);
    assert(rects[0].w > 0);
    assert(rects[0].h > 0);
    textbox_destroy(&t);
}

TEST(test_selection_rects_none) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_rect_t rects[16];
    int nr = textbox_selection_rects(&t, rects, 16);
    assert(nr == 0);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Word/Line Bounds
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_find_word_bounds) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    int ws, we;
    textbox_find_word_bounds(&t, 2, &ws, &we);
    assert(ws == 0);
    assert(we == 5);
    textbox_find_word_bounds(&t, 8, &ws, &we);
    assert(ws == 6);
    assert(we == 11);
    textbox_destroy(&t);
}

TEST(test_find_line_bounds) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Line1\nLine2\nLine3");
    int ls, le;
    textbox_find_line_bounds(&t, 8, &ls, &le);
    /* pos 8 is in "Line2" (starts at 6) */
    assert(ls == 6);
    assert(le >= 11);
    textbox_destroy(&t);
}

TEST(test_line_for_pos) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "AA\nBB\nCC");
    textbox_layout(&t);
    assert(textbox_line_for_pos(&t, 0) == 0);
    assert(textbox_line_for_pos(&t, 3) == 1);
    assert(textbox_line_for_pos(&t, 6) == 2);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Read Only
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_read_only) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Locked");
    textbox_set_read_only(&t, 1);
    textbox_insert_codepoint(&t, 'X');
    assert(textbox_len(&t) == 6); /* unchanged */
    textbox_delete_back(&t, 1);
    assert(textbox_len(&t) == 6);
    textbox_clear(&t);
    assert(textbox_len(&t) == 6);
    textbox_set_read_only(&t, 0);
    textbox_insert_codepoint(&t, 'X');
    assert(textbox_len(&t) == 7);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Max Length
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_max_length) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_max_length(&t, 5);
    textbox_insert_utf8(&t, "Hello World", 11);
    assert(textbox_len(&t) == 5);
    textbox_insert_codepoint(&t, 'X');
    assert(textbox_len(&t) == 5); /* can't exceed */
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Undo / Redo
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_undo_basic) {
    textbox_t t;
    make_tb(&t, 300, 200);
    assert(textbox_can_undo(&t) == 0);
    textbox_insert_utf8(&t, "Hello", 5);
    assert(textbox_can_undo(&t) == 1);
    textbox_undo(&t);
    assert(textbox_len(&t) == 0);
    assert(textbox_can_redo(&t) == 1);
    textbox_destroy(&t);
}

TEST(test_redo_basic) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_insert_utf8(&t, "Hello", 5);
    textbox_undo(&t);
    assert(textbox_len(&t) == 0);
    textbox_redo(&t);
    assert(textbox_len(&t) == 5);
    textbox_destroy(&t);
}

TEST(test_undo_delete) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 5, 0);
    textbox_undo_break(&t);
    textbox_delete_back(&t, 1);
    assert(textbox_len(&t) == 4);
    textbox_undo(&t);
    assert(textbox_len(&t) == 5);
    textbox_destroy(&t);
}

TEST(test_undo_multiple_steps) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_undo_break(&t);
    textbox_insert_utf8(&t, "A", 1);
    textbox_undo_break(&t);
    textbox_insert_utf8(&t, "B", 1);
    textbox_undo_break(&t);
    textbox_insert_utf8(&t, "C", 1);
    assert(textbox_len(&t) == 3);
    textbox_undo(&t);
    assert(textbox_len(&t) == 2);
    textbox_undo(&t);
    assert(textbox_len(&t) == 1);
    textbox_undo(&t);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_undo_redo_depth) {
    textbox_t t;
    make_tb(&t, 300, 200);
    assert(textbox_undo_depth(&t) == 0);
    assert(textbox_redo_depth(&t) == 0);
    textbox_insert_codepoint(&t, 'A');
    assert(textbox_undo_depth(&t) > 0);
    textbox_undo(&t);
    assert(textbox_redo_depth(&t) > 0);
    textbox_destroy(&t);
}

TEST(test_undo_break) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_insert_codepoint(&t, 'A');
    textbox_insert_codepoint(&t, 'B');
    textbox_undo_break(&t);
    textbox_insert_codepoint(&t, 'C');
    assert(textbox_len(&t) == 3);
    /* Undo should remove just 'C' */
    textbox_undo(&t);
    assert(textbox_len(&t) == 2);
    assert(textbox_char_at(&t, 0) == 'A');
    assert(textbox_char_at(&t, 1) == 'B');
    textbox_destroy(&t);
}

TEST(test_undo_clears_redo) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_undo_break(&t);
    textbox_insert_codepoint(&t, 'A');
    textbox_undo(&t);
    assert(textbox_can_redo(&t) == 1);
    textbox_insert_codepoint(&t, 'B');
    assert(textbox_can_redo(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_undo_style) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_range_style(&t, 0, 3, TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_BOLD);
    textbox_undo(&t);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_NONE);
    textbox_redo(&t);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_BOLD);
    textbox_destroy(&t);
}

TEST(test_set_text_clears_undo) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_insert_codepoint(&t, 'A');
    assert(textbox_can_undo(&t) == 1);
    textbox_set_text(&t, "New");
    assert(textbox_can_undo(&t) == 0);
    assert(textbox_can_redo(&t) == 0);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Event Handling — Keys
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_char) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_event_result_t r = textbox_handle_char(&t, 'A');
    assert(r.redraw == 1);
    assert(r.cursor_moved == 1);
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == 'A');
    textbox_destroy(&t);
}

TEST(test_handle_char_read_only) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_read_only(&t, 1);
    textbox_event_result_t r = textbox_handle_char(&t, 'A');
    assert(r.redraw == 0);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_handle_key_backspace) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "AB");
    textbox_set_cursor(&t, 2, 0);
    textbox_event_result_t r = textbox_handle_key(&t, TEXTBOX_KEY_BACKSPACE, 0);
    assert(r.redraw == 1);
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == 'A');
    textbox_destroy(&t);
}

TEST(test_handle_key_delete) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "AB");
    textbox_set_cursor(&t, 0, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_DELETE, 0);
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == 'B');
    textbox_destroy(&t);
}

TEST(test_handle_key_return) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "AB");
    textbox_set_cursor(&t, 1, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_RETURN, 0);
    assert(textbox_len(&t) == 3);
    assert(textbox_char_at(&t, 1) == '\n');
    textbox_destroy(&t);
}

TEST(test_handle_key_tab) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_handle_key(&t, TEXTBOX_KEY_TAB, 0);
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == '\t');
    textbox_destroy(&t);
}

TEST(test_handle_key_arrows) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 2, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_LEFT, 0);
    assert(t.cursor == 1);
    textbox_handle_key(&t, TEXTBOX_KEY_RIGHT, 0);
    assert(t.cursor == 2);
    textbox_destroy(&t);
}

TEST(test_handle_key_home_end) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 2, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_HOME, 0);
    assert(t.cursor == 0);
    textbox_handle_key(&t, TEXTBOX_KEY_END, 0);
    assert(t.cursor == 5);
    textbox_destroy(&t);
}

TEST(test_handle_key_up_down) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Line1\nLine2\nLine3");
    textbox_set_cursor(&t, 2, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_DOWN, 0);
    assert(t.cursor >= 6);
    textbox_handle_key(&t, TEXTBOX_KEY_UP, 0);
    assert(t.cursor <= 5);
    textbox_destroy(&t);
}

/* ── Shortcut keys ───────────────────────────────────────────── */

TEST(test_handle_key_select_all) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_handle_key(&t, TEXTBOX_KEY_A, TEXTBOX_MOD_SHORTCUT);
    assert(textbox_has_selection(&t));
    int ss, se;
    textbox_selection_range(&t, &ss, &se);
    assert(ss == 0 && se == 5);
    textbox_destroy(&t);
}

TEST(test_handle_key_copy) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_select_all(&t);
    textbox_event_result_t r = textbox_handle_key(&t, TEXTBOX_KEY_C, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_copy == 1);
    textbox_destroy(&t);
}

TEST(test_handle_key_cut) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_select_all(&t);
    textbox_event_result_t r = textbox_handle_key(&t, TEXTBOX_KEY_X, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_cut == 1);
    textbox_destroy(&t);
}

TEST(test_handle_key_paste) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_event_result_t r = textbox_handle_key(&t, TEXTBOX_KEY_V, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_paste == 1);
    textbox_destroy(&t);
}

TEST(test_handle_key_undo_redo) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_handle_char(&t, 'A');
    textbox_undo_break(&t);
    textbox_handle_char(&t, 'B');
    assert(textbox_len(&t) == 2);
    textbox_handle_key(&t, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(textbox_len(&t) == 1);
    textbox_handle_key(&t, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT | TEXTBOX_MOD_SHIFT);
    assert(textbox_len(&t) == 2);
    textbox_destroy(&t);
}

TEST(test_handle_key_redo_y) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_handle_char(&t, 'A');
    textbox_undo_break(&t);
    textbox_handle_char(&t, 'B');
    textbox_handle_key(&t, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(textbox_len(&t) == 1);
    textbox_handle_key(&t, TEXTBOX_KEY_Y, TEXTBOX_MOD_SHORTCUT);
    assert(textbox_len(&t) == 2);
    textbox_destroy(&t);
}

TEST(test_handle_key_bold_shortcut) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 5, 1);
    textbox_handle_key(&t, TEXTBOX_KEY_B, TEXTBOX_MOD_SHORTCUT);
    for (int i = 0; i < 5; i++)
        assert((textbox_style_at(&t, i) & TEXTBOX_STYLE_BOLD) != 0);
    textbox_destroy(&t);
}

TEST(test_handle_key_italic_shortcut) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_select_all(&t);
    textbox_handle_key(&t, TEXTBOX_KEY_I, TEXTBOX_MOD_SHORTCUT);
    for (int i = 0; i < 5; i++)
        assert((textbox_style_at(&t, i) & TEXTBOX_STYLE_ITALIC) != 0);
    textbox_destroy(&t);
}

TEST(test_handle_key_underline_shortcut) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_select_all(&t);
    textbox_handle_key(&t, TEXTBOX_KEY_U, TEXTBOX_MOD_SHORTCUT);
    for (int i = 0; i < 5; i++)
        assert((textbox_style_at(&t, i) & TEXTBOX_STYLE_UNDERLINE) != 0);
    textbox_destroy(&t);
}

TEST(test_handle_key_shift_arrows) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_RIGHT, TEXTBOX_MOD_SHIFT);
    assert(textbox_has_selection(&t));
    assert(t.cursor == 1);
    assert(t.anchor == 0);
    textbox_destroy(&t);
}

TEST(test_handle_key_shortcut_home_end) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello\nWorld");
    textbox_set_cursor(&t, 5, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_HOME, TEXTBOX_MOD_SHORTCUT);
    assert(t.cursor == 0);
    textbox_handle_key(&t, TEXTBOX_KEY_END, TEXTBOX_MOD_SHORTCUT);
    assert(t.cursor == textbox_len(&t));
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Event Handling — Mouse
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_mouse_down) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_event_result_t r = textbox_handle_mouse_down(&t, 4.0f, 14.0f, 0.0f, 0);
    assert(r.redraw == 1);
    assert(r.cursor_moved == 1);
    assert(t.cursor == 0);
    textbox_destroy(&t);
}

TEST(test_handle_mouse_drag) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_handle_mouse_down(&t, 4.0f, 14.0f, 0.0f, 0);
    textbox_event_result_t r = textbox_handle_mouse_drag(&t, 44.0f, 14.0f);
    assert(r.redraw == 1);
    assert(textbox_has_selection(&t));
    textbox_handle_mouse_up(&t);
    assert(t.dragging == 0);
    textbox_destroy(&t);
}

TEST(test_handle_double_click_word_select) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    /* First click */
    textbox_handle_mouse_down(&t, 20.0f, 14.0f, 0.0f, 0);
    textbox_handle_mouse_up(&t);
    /* Second click within multi-click time and distance */
    textbox_handle_mouse_down(&t, 20.0f, 14.0f, 0.1f, 0);
    textbox_handle_mouse_up(&t);
    /* Should have selected a word */
    assert(textbox_has_selection(&t));
    textbox_destroy(&t);
}

TEST(test_handle_triple_click_line_select) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_handle_mouse_down(&t, 20.0f, 14.0f, 0.0f, 0);
    textbox_handle_mouse_up(&t);
    textbox_handle_mouse_down(&t, 20.0f, 14.0f, 0.1f, 0);
    textbox_handle_mouse_up(&t);
    textbox_handle_mouse_down(&t, 20.0f, 14.0f, 0.2f, 0);
    textbox_handle_mouse_up(&t);
    /* Should have selected the whole line */
    assert(textbox_has_selection(&t));
    int ss, se;
    textbox_selection_range(&t, &ss, &se);
    assert(ss == 0);
    assert(se == 11);
    textbox_destroy(&t);
}

TEST(test_handle_shift_click_extend) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World");
    textbox_handle_mouse_down(&t, 4.0f, 14.0f, 0.0f, 0);
    textbox_handle_mouse_up(&t);
    /* Shift-click further right */
    textbox_handle_mouse_down(&t, 60.0f, 14.0f, 1.0f, TEXTBOX_MOD_SHIFT);
    assert(textbox_has_selection(&t));
    textbox_destroy(&t);
}

/* ── Scroll events ───────────────────────────────────────────── */

TEST(test_handle_scroll) {
    textbox_t t;
    make_tb(&t, 300, 50);
    textbox_set_text(&t, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8");
    textbox_event_result_t r = textbox_handle_scroll(&t, 0, -1.0f);
    assert(r.redraw == 1);
    assert(t.scroll_y > 0);
    textbox_destroy(&t);
}

/* ── Focus events ────────────────────────────────────────────── */

TEST(test_handle_focus) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_event_result_t r = textbox_handle_focus(&t, 1);
    assert(r.redraw == 1);
    assert(t.has_focus == 1);
    r = textbox_handle_focus(&t, 0);
    assert(t.has_focus == 0);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Glyph Resolution
 * ═══════════════════════════════════════════════════════════════ */

static int mock_find_glyph(uint32_t codepoint, void *user) {
    (void)user;
    if (codepoint >= 'A' && codepoint <= 'Z') return (int)(codepoint - 'A');
    if (codepoint >= 'a' && codepoint <= 'z') return (int)(codepoint - 'a' + 26);
    return -1;
}

TEST(test_resolve_glyphs) {
    uint32_t cp[] = {'A', 'B', 'Z', '!'};
    uint32_t out[4];
    int n = textbox_resolve_glyphs(cp, 4, mock_find_glyph, NULL, out, 4);
    assert(n == 4);
    assert(out[0] == 0);  /* A */
    assert(out[1] == 1);  /* B */
    assert(out[2] == 25); /* Z */
    assert(out[3] == 0xFFFFFFFFu); /* ! not found */
}

TEST(test_resolve_visible_glyphs) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "ABC");
    uint32_t out[64];
    int n = textbox_resolve_visible_glyphs(&t, mock_find_glyph, NULL, out, 64);
    assert(n == 3);
    assert(out[0] == 0);  /* A */
    assert(out[1] == 1);  /* B */
    assert(out[2] == 2);  /* C */
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Styled Measure
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_styled_measure) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_render_services(&t, &mock_styled_svc);
    uint32_t cp[] = {'A', 'B'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_NONE};
    textbox_insert_styled(&t, cp, st, 2);
    textbox_layout(&t);
    /* Bold char is 10px, normal is 8px, so line width should be 18 */
    assert(fabsf(t.lines[0].width - 18.0f) < 0.1f);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Simple layout (no render services)
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_simple_layout_fallback) {
    textbox_t t;
    textbox_create(&t, 300, 200, 14.0f, 20.0f, 4.0f, mock_measure, NULL);
    /* No render services — uses simple layout */
    textbox_set_text(&t, "Hello\nWorld");
    textbox_layout(&t);
    assert(t.line_count >= 2);
    assert(t.lines[0].hard_break == 1);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Word Wrap in simple layout
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_simple_layout_word_wrap) {
    textbox_t t;
    textbox_create(&t, 60, 200, 14.0f, 20.0f, 4.0f, mock_measure, NULL);
    textbox_set_word_wrap(&t, 1);
    textbox_set_text(&t, "Hello World Foo");
    textbox_layout(&t);
    assert(t.line_count >= 2);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_empty_textbox_operations) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_layout(&t);
    assert(t.line_count >= 1);
    assert(textbox_len(&t) == 0);
    textbox_move_left(&t, 0);
    textbox_move_right(&t, 0);
    textbox_move_up(&t, 0);
    textbox_move_down(&t, 0);
    textbox_move_line_start(&t, 0);
    textbox_move_line_end(&t, 0);
    textbox_delete_back(&t, 1);
    textbox_delete_forward(&t, 1);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_delete_selection_no_selection) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hi");
    int r = textbox_delete_selection(&t);
    assert(r == 0);
    assert(textbox_len(&t) == 2);
    textbox_destroy(&t);
}

TEST(test_paste_null) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_paste_utf8(&t, NULL, 0);
    assert(textbox_len(&t) == 0);
    textbox_paste_styled(&t, NULL, NULL, 0);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_insert_zero_count) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_insert_codepoints(&t, NULL, 0);
    assert(textbox_len(&t) == 0);
    textbox_insert_utf8(&t, "test", 0);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_char_at_out_of_bounds) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "AB");
    /* char_at on gap buffer — just ensure no crash */
    uint32_t c = textbox_char_at(&t, 0);
    assert(c == 'A');
    c = textbox_char_at(&t, 1);
    assert(c == 'B');
    textbox_destroy(&t);
}

TEST(test_style_at_out_of_bounds) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "AB");
    uint16_t s = textbox_style_at(&t, 0);
    assert(s == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_word_wrap_toggle) {
    textbox_t t;
    make_tb(&t, 300, 200);
    assert(t.word_wrap == 1);
    textbox_set_word_wrap(&t, 0);
    assert(t.word_wrap == 0);
    assert(t.layout_dirty == 1);
    textbox_set_word_wrap(&t, 0); /* same value — no change */
    textbox_set_word_wrap(&t, 1);
    assert(t.word_wrap == 1);
    textbox_destroy(&t);
}

TEST(test_undo_on_empty) {
    textbox_t t;
    make_tb(&t, 300, 200);
    int r = textbox_undo(&t);
    assert(r == 0);
    r = textbox_redo(&t);
    assert(r == 0);
    textbox_destroy(&t);
}

TEST(test_handle_char_control_chars_ignored) {
    textbox_t t;
    make_tb(&t, 300, 200);
    /* Control chars below 32 (except \n and \t) should be ignored */
    textbox_event_result_t r = textbox_handle_char(&t, 0x01);
    assert(r.redraw == 0);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Stress Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_stress_many_chars) {
    textbox_t t;
    make_tb(&t, 400, 200);
    for (int i = 0; i < 1000; i++)
        textbox_insert_codepoint(&t, 'A' + (i % 26));
    assert(textbox_len(&t) == 1000);
    textbox_layout(&t);
    assert(t.line_count >= 1);
    textbox_clear(&t);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_stress_many_lines) {
    textbox_t t;
    make_tb(&t, 300, 100);
    for (int i = 0; i < 200; i++) {
        textbox_insert_codepoint(&t, 'L');
        textbox_insert_codepoint(&t, '\n');
    }
    assert(textbox_len(&t) == 400);
    textbox_layout(&t);
    assert(t.line_count >= 200);
    textbox_destroy(&t);
}

TEST(test_stress_undo_redo_cycle) {
    textbox_t t;
    make_tb(&t, 300, 200);
    /* Stay within TEXTBOX_MAX_UNDO (64) ring capacity */
    for (int i = 0; i < 50; i++) {
        textbox_undo_break(&t);
        textbox_insert_codepoint(&t, 'A' + (i % 26));
    }
    assert(textbox_len(&t) == 50);
    /* Undo all */
    while (textbox_can_undo(&t)) textbox_undo(&t);
    assert(textbox_len(&t) == 0);
    /* Redo all */
    while (textbox_can_redo(&t)) textbox_redo(&t);
    assert(textbox_len(&t) == 50);
    textbox_destroy(&t);
}

TEST(test_stress_repeated_layout) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello World\nLine Two\nLine Three");
    for (int i = 0; i < 50; i++) {
        t.layout_dirty = 1;
        textbox_layout(&t);
    }
    assert(t.line_count >= 3);
    textbox_destroy(&t);
}

TEST(test_stress_insert_delete_cycle) {
    textbox_t t;
    make_tb(&t, 300, 200);
    for (int i = 0; i < 100; i++) {
        textbox_insert_codepoint(&t, 'X');
        textbox_set_cursor(&t, textbox_len(&t), 0);
        textbox_delete_back(&t, 1);
    }
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  BiDi / Visual Move (basic — with default LTR services)
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_visual_move_ltr) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_visual_move(&t, 1, 0); /* move right */
    assert(t.cursor == 1);
    textbox_visual_move(&t, -1, 0); /* move left */
    assert(t.cursor == 0);
    textbox_destroy(&t);
}

TEST(test_cursor_is_rtl) {
    textbox_t t;
    make_tb(&t, 300, 200);
    textbox_set_text(&t, "Hello");
    textbox_set_cursor(&t, 2, 0);
    /* With default services, everything is LTR */
    assert(textbox_cursor_is_rtl(&t) == 0);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
    int test_count = 0;
    printf("textbox.h tests:\n");

    /* Lifecycle */
    printf("\n  -- Lifecycle --\n");
    RUN(test_create_destroy); test_count++;
    RUN(test_create_defaults); test_count++;
    RUN(test_set_render_services); test_count++;
    RUN(test_set_text_services); test_count++;

    /* Basic Editing */
    printf("\n  -- Basic Editing --\n");
    RUN(test_insert_codepoint); test_count++;
    RUN(test_insert_codepoints); test_count++;
    RUN(test_insert_utf8); test_count++;
    RUN(test_insert_styled); test_count++;
    RUN(test_set_text); test_count++;
    RUN(test_set_text_null); test_count++;
    RUN(test_clear); test_count++;
    RUN(test_delete_back); test_count++;
    RUN(test_delete_forward); test_count++;
    RUN(test_delete_selection); test_count++;

    /* Unicode / UTF-8 */
    printf("\n  -- Unicode / UTF-8 --\n");
    RUN(test_insert_unicode); test_count++;
    RUN(test_get_utf8); test_count++;
    RUN(test_get_text_codepoints); test_count++;

    /* Cursor & Selection */
    printf("\n  -- Cursor & Selection --\n");
    RUN(test_cursor_basic); test_count++;
    RUN(test_cursor_clamp); test_count++;
    RUN(test_selection_extend); test_count++;
    RUN(test_select_all); test_count++;
    RUN(test_move_left_right); test_count++;
    RUN(test_move_left_at_start); test_count++;
    RUN(test_move_right_at_end); test_count++;
    RUN(test_move_left_with_extend); test_count++;
    RUN(test_move_up_down); test_count++;
    RUN(test_move_line_start_end); test_count++;
    RUN(test_move_page_up_down); test_count++;

    /* Selection UTF-8 / Styled export */
    printf("\n  -- Selection Export --\n");
    RUN(test_get_selection_utf8); test_count++;
    RUN(test_get_selection_utf8_no_selection); test_count++;
    RUN(test_get_selection_styled); test_count++;

    /* Paste */
    printf("\n  -- Paste --\n");
    RUN(test_paste_utf8); test_count++;
    RUN(test_paste_styled); test_count++;
    RUN(test_paste_replaces_selection); test_count++;

    /* Style System */
    printf("\n  -- Style System --\n");
    RUN(test_current_style); test_count++;
    RUN(test_toggle_style); test_count++;
    RUN(test_insert_with_current_style); test_count++;
    RUN(test_set_range_style); test_count++;
    RUN(test_apply_style_to_selection); test_count++;
    RUN(test_toggle_selection_style); test_count++;
    RUN(test_selection_style_union); test_count++;
    RUN(test_selection_style_intersect); test_count++;
    RUN(test_get_styles); test_count++;

    /* Style Runs */
    printf("\n  -- Style Runs --\n");
    RUN(test_style_runs); test_count++;

    /* Layout */
    printf("\n  -- Layout --\n");
    RUN(test_layout_empty); test_count++;
    RUN(test_layout_single_line); test_count++;
    RUN(test_layout_multiline); test_count++;
    RUN(test_layout_word_wrap); test_count++;
    RUN(test_layout_no_word_wrap); test_count++;
    RUN(test_content_height); test_count++;
    RUN(test_layout_idempotent); test_count++;

    /* Visible Lines */
    printf("\n  -- Visible Lines --\n");
    RUN(test_visible_line_count); test_count++;
    RUN(test_get_visible_lines); test_count++;
    RUN(test_visible_lines_viewport_clip); test_count++;

    /* Scroll */
    printf("\n  -- Scroll --\n");
    RUN(test_scroll_basic); test_count++;
    RUN(test_scroll_clamp); test_count++;
    RUN(test_scroll_by); test_count++;
    RUN(test_scroll_to_cursor); test_count++;
    RUN(test_scroll_no_overflow); test_count++;
    RUN(test_scrollbar_v); test_count++;
    RUN(test_scrollbar_h); test_count++;
    RUN(test_max_scroll_x); test_count++;

    /* Hit Test */
    printf("\n  -- Hit Test --\n");
    RUN(test_hit_test_basic); test_count++;
    RUN(test_hit_test_multiline); test_count++;

    /* Cursor Rect & Selection Rects */
    printf("\n  -- Cursor Rect & Selection Rects --\n");
    RUN(test_cursor_rect); test_count++;
    RUN(test_cursor_rect_mid_text); test_count++;
    RUN(test_selection_rects); test_count++;
    RUN(test_selection_rects_none); test_count++;

    /* Word/Line Bounds */
    printf("\n  -- Word/Line Bounds --\n");
    RUN(test_find_word_bounds); test_count++;
    RUN(test_find_line_bounds); test_count++;
    RUN(test_line_for_pos); test_count++;

    /* Read Only */
    printf("\n  -- Read Only --\n");
    RUN(test_read_only); test_count++;

    /* Max Length */
    printf("\n  -- Max Length --\n");
    RUN(test_max_length); test_count++;

    /* Undo / Redo */
    printf("\n  -- Undo / Redo --\n");
    RUN(test_undo_basic); test_count++;
    RUN(test_redo_basic); test_count++;
    RUN(test_undo_delete); test_count++;
    RUN(test_undo_multiple_steps); test_count++;
    RUN(test_undo_redo_depth); test_count++;
    RUN(test_undo_break); test_count++;
    RUN(test_undo_clears_redo); test_count++;
    RUN(test_undo_style); test_count++;
    RUN(test_set_text_clears_undo); test_count++;

    /* Event Handling — Keys */
    printf("\n  -- Event Handling: Keys --\n");
    RUN(test_handle_char); test_count++;
    RUN(test_handle_char_read_only); test_count++;
    RUN(test_handle_key_backspace); test_count++;
    RUN(test_handle_key_delete); test_count++;
    RUN(test_handle_key_return); test_count++;
    RUN(test_handle_key_tab); test_count++;
    RUN(test_handle_key_arrows); test_count++;
    RUN(test_handle_key_home_end); test_count++;
    RUN(test_handle_key_up_down); test_count++;
    RUN(test_handle_key_select_all); test_count++;
    RUN(test_handle_key_copy); test_count++;
    RUN(test_handle_key_cut); test_count++;
    RUN(test_handle_key_paste); test_count++;
    RUN(test_handle_key_undo_redo); test_count++;
    RUN(test_handle_key_redo_y); test_count++;
    RUN(test_handle_key_bold_shortcut); test_count++;
    RUN(test_handle_key_italic_shortcut); test_count++;
    RUN(test_handle_key_underline_shortcut); test_count++;
    RUN(test_handle_key_shift_arrows); test_count++;
    RUN(test_handle_key_shortcut_home_end); test_count++;

    /* Event Handling — Mouse */
    printf("\n  -- Event Handling: Mouse --\n");
    RUN(test_handle_mouse_down); test_count++;
    RUN(test_handle_mouse_drag); test_count++;
    RUN(test_handle_double_click_word_select); test_count++;
    RUN(test_handle_triple_click_line_select); test_count++;
    RUN(test_handle_shift_click_extend); test_count++;
    RUN(test_handle_scroll); test_count++;
    RUN(test_handle_focus); test_count++;

    /* Glyph Resolution */
    printf("\n  -- Glyph Resolution --\n");
    RUN(test_resolve_glyphs); test_count++;
    RUN(test_resolve_visible_glyphs); test_count++;

    /* Styled Measure */
    printf("\n  -- Styled Measure --\n");
    RUN(test_styled_measure); test_count++;

    /* Simple Layout Fallback */
    printf("\n  -- Simple Layout Fallback --\n");
    RUN(test_simple_layout_fallback); test_count++;
    RUN(test_simple_layout_word_wrap); test_count++;

    /* Edge Cases */
    printf("\n  -- Edge Cases --\n");
    RUN(test_empty_textbox_operations); test_count++;
    RUN(test_delete_selection_no_selection); test_count++;
    RUN(test_paste_null); test_count++;
    RUN(test_insert_zero_count); test_count++;
    RUN(test_char_at_out_of_bounds); test_count++;
    RUN(test_style_at_out_of_bounds); test_count++;
    RUN(test_word_wrap_toggle); test_count++;
    RUN(test_undo_on_empty); test_count++;
    RUN(test_handle_char_control_chars_ignored); test_count++;

    /* BiDi / Visual Move */
    printf("\n  -- BiDi / Visual Move --\n");
    RUN(test_visual_move_ltr); test_count++;
    RUN(test_cursor_is_rtl); test_count++;

    /* Stress */
    printf("\n  -- Stress --\n");
    RUN(test_stress_many_chars); test_count++;
    RUN(test_stress_many_lines); test_count++;
    RUN(test_stress_undo_redo_cycle); test_count++;
    RUN(test_stress_repeated_layout); test_count++;
    RUN(test_stress_insert_delete_cycle); test_count++;

    printf("\nAll %d tests passed.\n", test_count);
    return 0;
}
