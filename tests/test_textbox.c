/*
 * test_textbox.c - Comprehensive tests for textbox.h: the platform-independent styled text editor widget (input, layout, selection, undo/redo).
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
 * @brief Tests for textbox.h — platform-independent styled text editor widget.
 */
#define UNISTRING_IMPLEMENTATION
#define TEXTBOX_IMPLEMENTATION
#include "textbox.h"

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-60s", #name); name(); printf("PASS\n"); } while(0)
#define NEAR(a, b) (fabsf((a) - (b)) < 0.01f)

/* ── Dummy measure callback ──────────────────────────────────── */

static float dummy_measure(uint32_t codepoint, float font_size, void *user) {
    (void)user;
    (void)codepoint;
    return font_size * 0.6f; /* monospace-ish: 60% of font size */
}

/* ── Helpers ──────────────────────────────────────────────────── */

static void make_tb(textbox_t *t, float w, float h, int multiline) {
    textbox_create(t, w, h, 16.0f, 16.0f, 0.0f, dummy_measure, NULL);
    if (multiline) textbox_set_word_wrap(t, 1);
}

static void make_tb_with_text(textbox_t *t, float w, float h,
                              int multiline, const char *text) {
    make_tb(t, w, h, multiline);
    textbox_set_text(t, text);
}

static int tb_streq(textbox_t *t, const char *expected) {
    int len;
    const uint32_t *txt = textbox_get_text(t, &len);
    int elen = (int)strlen(expected);
    if (len != elen) return 0;
    const char *p = expected;
    for (int i = 0; i < len; i++) {
        uint32_t cp = textbox_utf8_decode(&p);
        if (txt[i] != cp) return 0;
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_create_destroy) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_create_multiline) {
    textbox_t t;
    make_tb(&t, 400, 300, 1);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_create_defaults) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    assert(textbox_len(&t) == 0);
    assert(textbox_has_selection(&t) == 0);
    assert(textbox_get_current_style(&t) == TEXTBOX_STYLE_NONE);
    assert(textbox_can_undo(&t) == 0);
    assert(textbox_can_redo(&t) == 0);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Basic Text Operations
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_text) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_text(&t, "Hello");
    assert(textbox_len(&t) == 5);
    assert(tb_streq(&t, "Hello"));
    textbox_destroy(&t);
}

TEST(test_set_text_replaces) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_text(&t, "World");
    assert(textbox_len(&t) == 5);
    assert(tb_streq(&t, "World"));
    textbox_destroy(&t);
}

TEST(test_set_text_empty) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_text(&t, "");
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_set_text_null) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_text(&t, NULL);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_clear) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_clear(&t);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_clear_empty) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_clear(&t);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_char_at) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "ABCD");
    assert(textbox_char_at(&t, 0) == 'A');
    assert(textbox_char_at(&t, 1) == 'B');
    assert(textbox_char_at(&t, 2) == 'C');
    assert(textbox_char_at(&t, 3) == 'D');
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Insert Operations
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_insert_codepoint) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_codepoint(&t, 'A');
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == 'A');
    textbox_destroy(&t);
}

TEST(test_insert_multiple_codepoints) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_codepoint(&t, 'H');
    textbox_insert_codepoint(&t, 'i');
    assert(textbox_len(&t) == 2);
    assert(tb_streq(&t, "Hi"));
    textbox_destroy(&t);
}

TEST(test_insert_codepoints_array) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    uint32_t chars[] = {'H', 'e', 'l', 'l', 'o'};
    textbox_insert_codepoints(&t, chars, 5);
    assert(textbox_len(&t) == 5);
    assert(tb_streq(&t, "Hello"));
    textbox_destroy(&t);
}

TEST(test_insert_utf8) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_utf8(&t, "Hello", 5);
    assert(textbox_len(&t) == 5);
    assert(tb_streq(&t, "Hello"));
    textbox_destroy(&t);
}

TEST(test_insert_utf8_negative_len) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_utf8(&t, "Hello", -1);
    assert(textbox_len(&t) == 5);
    assert(tb_streq(&t, "Hello"));
    textbox_destroy(&t);
}

TEST(test_insert_unicode) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    /* Insert U+00E9 (é) — 2-byte UTF-8: 0xC3 0xA9 */
    textbox_insert_utf8(&t, "\xC3\xA9", 2);
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == 0x00E9);
    textbox_destroy(&t);
}

TEST(test_insert_emoji) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    /* U+1F600 (😀) — 4-byte UTF-8: F0 9F 98 80 */
    textbox_insert_utf8(&t, "\xF0\x9F\x98\x80", 4);
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == 0x1F600);
    textbox_destroy(&t);
}

TEST(test_insert_styled) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    uint32_t cp[] = {'A', 'B', 'C'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_ITALIC, TEXTBOX_STYLE_NONE};
    textbox_insert_styled(&t, cp, st, 3);
    assert(textbox_len(&t) == 3);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(&t, 2) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_insert_at_cursor_position) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "AC");
    textbox_set_cursor(&t, 1, 0);
    textbox_insert_codepoint(&t, 'B');
    assert(textbox_len(&t) == 3);
    assert(tb_streq(&t, "ABC"));
    textbox_destroy(&t);
}

TEST(test_insert_at_beginning) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "BC");
    textbox_set_cursor(&t, 0, 0);
    textbox_insert_codepoint(&t, 'A');
    assert(tb_streq(&t, "ABC"));
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Delete Operations
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_delete_back) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 5, 0);
    textbox_delete_back(&t, 1);
    assert(textbox_len(&t) == 4);
    assert(tb_streq(&t, "Hell"));
    textbox_destroy(&t);
}

TEST(test_delete_back_multiple) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 5, 0);
    textbox_delete_back(&t, 3);
    assert(textbox_len(&t) == 2);
    assert(tb_streq(&t, "He"));
    textbox_destroy(&t);
}

TEST(test_delete_back_at_start) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_delete_back(&t, 1);
    assert(textbox_len(&t) == 5); /* no change */
    textbox_destroy(&t);
}

TEST(test_delete_forward) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_delete_forward(&t, 1);
    assert(textbox_len(&t) == 4);
    assert(tb_streq(&t, "ello"));
    textbox_destroy(&t);
}

TEST(test_delete_forward_multiple) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_delete_forward(&t, 3);
    assert(textbox_len(&t) == 2);
    assert(tb_streq(&t, "lo"));
    textbox_destroy(&t);
}

TEST(test_delete_forward_at_end) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 5, 0);
    textbox_delete_forward(&t, 1);
    assert(textbox_len(&t) == 5); /* no change */
    textbox_destroy(&t);
}

TEST(test_delete_selection) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 5, 1); /* select "Hello" */
    assert(textbox_has_selection(&t));
    int deleted = textbox_delete_selection(&t);
    assert(deleted == 1);
    assert(tb_streq(&t, " World"));
    textbox_destroy(&t);
}

TEST(test_delete_selection_no_selection) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 2, 0);
    int deleted = textbox_delete_selection(&t);
    assert(deleted == 0);
    assert(textbox_len(&t) == 5);
    textbox_destroy(&t);
}

TEST(test_delete_middle) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "ABCDE");
    textbox_set_cursor(&t, 1, 0);
    textbox_delete_forward(&t, 1);
    assert(tb_streq(&t, "ACDE"));
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Cursor & Selection
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_cursor) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 3, 0);
    assert(textbox_has_selection(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_set_cursor_extend) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 1, 0);
    textbox_set_cursor(&t, 4, 1);
    assert(textbox_has_selection(&t) == 1);
    int s, e;
    textbox_selection_range(&t, &s, &e);
    assert(s == 1);
    assert(e == 4);
    textbox_destroy(&t);
}

TEST(test_move_left) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 3, 0);
    textbox_move_left(&t, 0);
    /* Cursor should now be at 2 */
    textbox_insert_codepoint(&t, 'X');
    assert(tb_streq(&t, "HeXllo"));
    textbox_destroy(&t);
}

TEST(test_move_right) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 2, 0);
    textbox_move_right(&t, 0);
    textbox_insert_codepoint(&t, 'X');
    assert(tb_streq(&t, "HelXlo"));
    textbox_destroy(&t);
}

TEST(test_move_left_extend) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 3, 0);
    textbox_move_left(&t, 1);
    textbox_move_left(&t, 1);
    assert(textbox_has_selection(&t) == 1);
    int s, e;
    textbox_selection_range(&t, &s, &e);
    assert(s == 1);
    assert(e == 3);
    textbox_destroy(&t);
}

TEST(test_move_right_extend) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 1, 0);
    textbox_move_right(&t, 1);
    textbox_move_right(&t, 1);
    assert(textbox_has_selection(&t) == 1);
    int s, e;
    textbox_selection_range(&t, &s, &e);
    assert(s == 1);
    assert(e == 3);
    textbox_destroy(&t);
}

TEST(test_move_left_at_start) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hi");
    textbox_set_cursor(&t, 0, 0);
    textbox_move_left(&t, 0);
    textbox_insert_codepoint(&t, 'X');
    assert(tb_streq(&t, "XHi"));
    textbox_destroy(&t);
}

TEST(test_move_right_at_end) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hi");
    textbox_set_cursor(&t, 2, 0);
    textbox_move_right(&t, 0);
    textbox_insert_codepoint(&t, 'X');
    assert(tb_streq(&t, "HiX"));
    textbox_destroy(&t);
}

TEST(test_select_all) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_select_all(&t);
    assert(textbox_has_selection(&t) == 1);
    int s, e;
    textbox_selection_range(&t, &s, &e);
    assert(s == 0);
    assert(e == 11);
    textbox_destroy(&t);
}

TEST(test_select_all_empty) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_select_all(&t);
    assert(textbox_has_selection(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_selection_range_reversed) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 4, 0);
    textbox_set_cursor(&t, 1, 1);
    int s, e;
    textbox_selection_range(&t, &s, &e);
    assert(s == 1);
    assert(e == 4);
    textbox_destroy(&t);
}

TEST(test_insert_replaces_selection) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_set_cursor(&t, 5, 0);
    textbox_set_cursor(&t, 11, 1); /* select " World" */
    textbox_insert_codepoint(&t, '!');
    assert(tb_streq(&t, "Hello!"));
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Style
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_current_style) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_current_style(&t, TEXTBOX_STYLE_BOLD);
    assert(textbox_get_current_style(&t) == TEXTBOX_STYLE_BOLD);
    textbox_destroy(&t);
}

TEST(test_toggle_style) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    assert(textbox_get_current_style(&t) == TEXTBOX_STYLE_NONE);
    textbox_toggle_style(&t, TEXTBOX_STYLE_BOLD);
    assert(textbox_get_current_style(&t) == TEXTBOX_STYLE_BOLD);
    textbox_toggle_style(&t, TEXTBOX_STYLE_BOLD);
    assert(textbox_get_current_style(&t) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_toggle_style_combined) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_toggle_style(&t, TEXTBOX_STYLE_BOLD);
    textbox_toggle_style(&t, TEXTBOX_STYLE_ITALIC);
    assert(textbox_get_current_style(&t) == (TEXTBOX_STYLE_BOLD | TEXTBOX_STYLE_ITALIC));
    textbox_destroy(&t);
}

TEST(test_insert_with_style) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_current_style(&t, TEXTBOX_STYLE_BOLD);
    textbox_insert_codepoint(&t, 'A');
    textbox_set_current_style(&t, TEXTBOX_STYLE_ITALIC);
    textbox_insert_codepoint(&t, 'B');
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_ITALIC);
    textbox_destroy(&t);
}

TEST(test_style_at) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_current_style(&t, TEXTBOX_STYLE_UNDERLINE);
    textbox_insert_utf8(&t, "ABC", 3);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_UNDERLINE);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_UNDERLINE);
    assert(textbox_style_at(&t, 2) == TEXTBOX_STYLE_UNDERLINE);
    textbox_destroy(&t);
}

TEST(test_set_range_style) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_range_style(&t, 1, 4, TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_NONE);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 2) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 3) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 4) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_apply_style_to_selection) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 1, 0);
    textbox_set_cursor(&t, 4, 1);
    textbox_apply_style_to_selection(&t, TEXTBOX_STYLE_ITALIC, TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_NONE);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(&t, 2) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(&t, 3) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(&t, 4) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_toggle_selection_style) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "ABCDE");
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 5, 1);
    textbox_toggle_selection_style(&t, TEXTBOX_STYLE_BOLD);
    for (int i = 0; i < 5; i++)
        assert(textbox_style_at(&t, i) == TEXTBOX_STYLE_BOLD);
    /* Toggle again to remove */
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 5, 1);
    textbox_toggle_selection_style(&t, TEXTBOX_STYLE_BOLD);
    for (int i = 0; i < 5; i++)
        assert(textbox_style_at(&t, i) == TEXTBOX_STYLE_NONE);
    textbox_destroy(&t);
}

TEST(test_get_selection_style_union) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_current_style(&t, TEXTBOX_STYLE_BOLD);
    textbox_insert_codepoint(&t, 'A');
    textbox_set_current_style(&t, TEXTBOX_STYLE_ITALIC);
    textbox_insert_codepoint(&t, 'B');
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 2, 1);
    uint16_t u = textbox_get_selection_style_union(&t);
    assert(u & TEXTBOX_STYLE_BOLD);
    assert(u & TEXTBOX_STYLE_ITALIC);
    textbox_destroy(&t);
}

TEST(test_get_selection_style_intersect) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_current_style(&t, TEXTBOX_STYLE_BOLD | TEXTBOX_STYLE_ITALIC);
    textbox_insert_codepoint(&t, 'A');
    textbox_set_current_style(&t, TEXTBOX_STYLE_BOLD);
    textbox_insert_codepoint(&t, 'B');
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 2, 1);
    uint16_t inter = textbox_get_selection_style_intersect(&t);
    assert(inter & TEXTBOX_STYLE_BOLD);
    assert(!(inter & TEXTBOX_STYLE_ITALIC));
    textbox_destroy(&t);
}

TEST(test_all_style_flags) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    uint16_t all = TEXTBOX_STYLE_BOLD | TEXTBOX_STYLE_ITALIC | TEXTBOX_STYLE_UNDERLINE |
                   TEXTBOX_STYLE_STRIKETHROUGH | TEXTBOX_STYLE_SUBSCRIPT |
                   TEXTBOX_STYLE_SUPERSCRIPT | TEXTBOX_STYLE_CODE | TEXTBOX_STYLE_HIGHLIGHT;
    textbox_set_current_style(&t, all);
    textbox_insert_codepoint(&t, 'X');
    assert(textbox_style_at(&t, 0) == all);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  UTF-8 Selection & Paste
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_get_selection_utf8) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 5, 1);
    char buf[64];
    int len = textbox_get_selection_utf8(&t, buf, sizeof(buf));
    assert(len == 5);
    assert(memcmp(buf, "Hello", 5) == 0);
    textbox_destroy(&t);
}

TEST(test_get_selection_utf8_empty) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 2, 0);
    char buf[64];
    int len = textbox_get_selection_utf8(&t, buf, sizeof(buf));
    assert(len == 0);
    textbox_destroy(&t);
}

TEST(test_get_selection_styled) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_current_style(&t, TEXTBOX_STYLE_BOLD);
    textbox_insert_utf8(&t, "AB", 2);
    textbox_set_cursor(&t, 0, 0);
    textbox_set_cursor(&t, 2, 1);
    uint32_t cp[8];
    uint16_t st[8];
    int n = textbox_get_selection_styled(&t, cp, st, 8);
    assert(n == 2);
    assert(cp[0] == 'A' && cp[1] == 'B');
    assert(st[0] == TEXTBOX_STYLE_BOLD && st[1] == TEXTBOX_STYLE_BOLD);
    textbox_destroy(&t);
}

TEST(test_paste_utf8) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 5, 0);
    textbox_paste_utf8(&t, " World", 6);
    assert(tb_streq(&t, "Hello World"));
    textbox_destroy(&t);
}

TEST(test_paste_utf8_negative_len) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_paste_utf8(&t, "Test", -1);
    assert(tb_streq(&t, "Test"));
    textbox_destroy(&t);
}

TEST(test_paste_utf8_replaces_selection) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_set_cursor(&t, 5, 0);
    textbox_set_cursor(&t, 11, 1);
    textbox_paste_utf8(&t, "!", 1);
    assert(tb_streq(&t, "Hello!"));
    textbox_destroy(&t);
}

TEST(test_paste_styled) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    uint32_t cp[] = {'X', 'Y'};
    uint16_t st[] = {TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_ITALIC};
    textbox_paste_styled(&t, cp, st, 2);
    assert(textbox_len(&t) == 2);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(&t, 1) == TEXTBOX_STYLE_ITALIC);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Text/Style Export
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_get_text) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    int len;
    const uint32_t *text = textbox_get_text(&t, &len);
    assert(len == 5);
    assert(text[0] == 'H');
    assert(text[4] == 'o');
    textbox_destroy(&t);
}

TEST(test_get_styles) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_current_style(&t, TEXTBOX_STYLE_CODE);
    textbox_insert_utf8(&t, "ABC", 3);
    int len;
    const uint16_t *styles = textbox_get_styles(&t, &len);
    assert(len == 3);
    for (int i = 0; i < 3; i++)
        assert(styles[i] == TEXTBOX_STYLE_CODE);
    textbox_destroy(&t);
}

TEST(test_get_utf8) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    char buf[64];
    int len = textbox_get_utf8(&t, buf, sizeof(buf));
    assert(len > 0);
    assert(memcmp(buf, "Hello", 5) == 0);
    textbox_destroy(&t);
}

TEST(test_get_utf8_unicode) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_utf8(&t, "\xC3\xA9", 2); /* é */
    char buf[64];
    int len = textbox_get_utf8(&t, buf, sizeof(buf));
    assert(len == 2);
    assert((unsigned char)buf[0] == 0xC3);
    assert((unsigned char)buf[1] == 0xA9);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Read-only
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_read_only_blocks_insert) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_read_only(&t, 1);
    textbox_insert_codepoint(&t, 'X');
    assert(textbox_len(&t) == 5);
    assert(tb_streq(&t, "Hello"));
    textbox_destroy(&t);
}

TEST(test_read_only_blocks_delete) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_read_only(&t, 1);
    textbox_set_cursor(&t, 5, 0);
    textbox_delete_back(&t, 1);
    assert(textbox_len(&t) == 5);
    textbox_destroy(&t);
}

TEST(test_read_only_blocks_clear) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_read_only(&t, 1);
    textbox_clear(&t);
    assert(textbox_len(&t) == 5);
    textbox_destroy(&t);
}

TEST(test_read_only_blocks_paste) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_read_only(&t, 1);
    textbox_paste_utf8(&t, " World", 6);
    assert(textbox_len(&t) == 5);
    textbox_destroy(&t);
}

TEST(test_read_only_allows_selection) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_read_only(&t, 1);
    textbox_select_all(&t);
    assert(textbox_has_selection(&t) == 1);
    textbox_destroy(&t);
}

TEST(test_read_only_toggle) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_read_only(&t, 1);
    textbox_insert_codepoint(&t, 'X');
    assert(textbox_len(&t) == 5);
    textbox_set_read_only(&t, 0);
    textbox_set_cursor(&t, 5, 0);
    textbox_insert_codepoint(&t, 'X');
    assert(textbox_len(&t) == 6);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Max Length
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_max_length) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_max_length(&t, 5);
    textbox_insert_utf8(&t, "Hello World", 11);
    assert(textbox_len(&t) <= 5);
    textbox_destroy(&t);
}

TEST(test_max_length_codepoint) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_max_length(&t, 3);
    textbox_insert_codepoint(&t, 'A');
    textbox_insert_codepoint(&t, 'B');
    textbox_insert_codepoint(&t, 'C');
    textbox_insert_codepoint(&t, 'D');
    assert(textbox_len(&t) == 3);
    textbox_destroy(&t);
}

TEST(test_max_length_zero_unlimited) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_max_length(&t, 0);
    textbox_insert_utf8(&t, "Hello World this is a long string", -1);
    assert(textbox_len(&t) > 5);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Word & Line Bounds
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_is_word_char) {
    assert(textbox_is_word_char('A') == 1);
    assert(textbox_is_word_char('z') == 1);
    assert(textbox_is_word_char('0') == 1);
    assert(textbox_is_word_char('_') == 1);
    assert(textbox_is_word_char(' ') == 0);
    assert(textbox_is_word_char('.') == 0);
}

TEST(test_find_word_bounds) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    int ws, we;
    textbox_find_word_bounds(&t, 2, &ws, &we);
    assert(ws == 0);
    assert(we == 5);
    textbox_find_word_bounds(&t, 8, &ws, &we);
    assert(ws == 6);
    assert(we == 11);
    textbox_destroy(&t);
}

TEST(test_find_word_bounds_at_space) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    int ws, we;
    textbox_find_word_bounds(&t, 5, &ws, &we);
    /* At space boundary: exact behavior depends on implementation,
       but start and end should be valid */
    assert(ws >= 0 && we <= 11);
    assert(ws <= we);
    textbox_destroy(&t);
}

TEST(test_find_line_bounds_single) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    int ls, le;
    textbox_find_line_bounds(&t, 2, &ls, &le);
    assert(ls == 0);
    assert(le == 5);
    textbox_destroy(&t);
}

TEST(test_find_line_bounds_multiline) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello\nWorld");
    int ls, le;
    textbox_find_line_bounds(&t, 2, &ls, &le);
    assert(ls == 0);

    textbox_find_line_bounds(&t, 8, &ls, &le);
    assert(ls == 6);
    assert(le == 11);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Layout & Scroll
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_layout) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello\nWorld\nLine3");
    textbox_layout(&t);
    int vlc = textbox_visible_line_count(&t);
    assert(vlc >= 1);
    textbox_destroy(&t);
}

TEST(test_layout_empty) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_layout(&t);
    int vlc = textbox_visible_line_count(&t);
    assert(vlc >= 0);
    textbox_destroy(&t);
}

TEST(test_scroll_to) {
    textbox_t t;
    make_tb_with_text(&t, 200, 50, 1, "Line1\nLine2\nLine3\nLine4\nLine5\nLine6\nLine7\nLine8");
    textbox_layout(&t);
    textbox_scroll_to(&t, 0.0f);
    float max_y = textbox_max_scroll_y(&t);
    assert(max_y >= 0.0f);
    textbox_scroll_to(&t, max_y);
    textbox_destroy(&t);
}

TEST(test_scroll_to_x) {
    textbox_t t;
    make_tb_with_text(&t, 100, 50, 0, "A very long line of text that is wider than the box");
    textbox_layout(&t);
    float max_x = textbox_max_scroll_x(&t);
    textbox_scroll_to_x(&t, max_x);
    textbox_destroy(&t);
}

TEST(test_scroll_by) {
    textbox_t t;
    make_tb_with_text(&t, 200, 50, 1, "Line1\nLine2\nLine3\nLine4\nLine5");
    textbox_layout(&t);
    textbox_scroll_to(&t, 0.0f);
    textbox_scroll_by(&t, 0.0f, 10.0f);
    textbox_destroy(&t);
}

TEST(test_scroll_to_cursor) {
    textbox_t t;
    make_tb_with_text(&t, 200, 50, 1, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10");
    textbox_layout(&t);
    textbox_set_cursor(&t, textbox_len(&t), 0);
    textbox_scroll_to_cursor(&t);
    /* Cursor should be visible now; no crash */
    textbox_destroy(&t);
}

TEST(test_max_scroll) {
    textbox_t t;
    make_tb_with_text(&t, 200, 50, 1, "A\nB\nC\nD\nE\nF\nG\nH");
    textbox_layout(&t);
    float ms = textbox_max_scroll(&t);
    float msy = textbox_max_scroll_y(&t);
    assert(NEAR(ms, msy));
    textbox_destroy(&t);
}

TEST(test_scrollbar_v) {
    textbox_t t;
    make_tb_with_text(&t, 200, 50, 1, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8");
    textbox_layout(&t);
    textbox_scrollbar_t sb;
    textbox_get_scrollbar_v(&t, &sb);
    /* Just verify it doesn't crash and returns valid data */
    assert(sb.visible >= 0);
    textbox_destroy(&t);
}

TEST(test_scrollbar_h) {
    textbox_t t;
    make_tb_with_text(&t, 100, 50, 0, "A very long line that overflows");
    textbox_layout(&t);
    textbox_scrollbar_t sb;
    textbox_get_scrollbar_h(&t, &sb);
    assert(sb.visible >= 0);
    textbox_destroy(&t);
}

TEST(test_set_scrollbar_v) {
    textbox_t t;
    make_tb_with_text(&t, 200, 50, 1, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8");
    textbox_layout(&t);
    textbox_set_scrollbar_v(&t, 0.5f);
    textbox_destroy(&t);
}

TEST(test_set_scrollbar_h) {
    textbox_t t;
    make_tb_with_text(&t, 100, 50, 0, "A very long line that overflows the box");
    textbox_layout(&t);
    textbox_set_scrollbar_h(&t, 0.5f);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Query
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_visible_line_count) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Line1\nLine2\nLine3");
    textbox_layout(&t);
    int vlc = textbox_visible_line_count(&t);
    assert(vlc >= 1);
    textbox_destroy(&t);
}

TEST(test_get_visible_lines) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello\nWorld");
    textbox_layout(&t);
    textbox_visible_line_t lines[16];
    int n = textbox_get_visible_lines(&t, lines, 16);
    assert(n >= 1);
    textbox_destroy(&t);
}

TEST(test_cursor_rect) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 2, 0);
    textbox_layout(&t);
    textbox_rect_t cr = textbox_cursor_rect(&t, 2.0f);
    assert(cr.w >= 0.0f);
    assert(cr.h > 0.0f);
    textbox_destroy(&t);
}

TEST(test_cursor_rect_at_start) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_layout(&t);
    textbox_rect_t cr = textbox_cursor_rect(&t, 2.0f);
    assert(NEAR(cr.x, 0.0f));
    textbox_destroy(&t);
}

TEST(test_selection_rects) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello\nWorld");
    textbox_set_cursor(&t, 2, 0);
    textbox_set_cursor(&t, 8, 1);
    textbox_layout(&t);
    textbox_rect_t rects[16];
    int n = textbox_selection_rects(&t, rects, 16);
    assert(n >= 1);
    for (int i = 0; i < n; i++) {
        assert(rects[i].w > 0.0f);
        assert(rects[i].h > 0.0f);
    }
    textbox_destroy(&t);
}

TEST(test_selection_rects_no_selection) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 2, 0);
    textbox_layout(&t);
    textbox_rect_t rects[16];
    int n = textbox_selection_rects(&t, rects, 16);
    assert(n == 0);
    textbox_destroy(&t);
}

TEST(test_hit_test) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_layout(&t);
    int pos = textbox_hit_test(&t, 0.0f, 0.0f);
    assert(pos >= 0);
    assert(pos <= textbox_len(&t));
    textbox_destroy(&t);
}

TEST(test_hit_test_middle) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_layout(&t);
    /* Click somewhere in the middle — should return a valid position */
    int pos = textbox_hit_test(&t, 30.0f, 8.0f);
    assert(pos >= 0 && pos <= 5);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Movement (Multiline)
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_move_up) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello\nWorld");
    textbox_layout(&t);
    textbox_set_cursor(&t, 8, 0); /* middle of "World" */
    textbox_move_up(&t, 0);
    /* Should be somewhere in the first line */
    textbox_insert_codepoint(&t, 'X');
    int len;
    const uint32_t *txt = textbox_get_text(&t, &len);
    /* X should appear before position 6 */
    int found = 0;
    for (int i = 0; i < 6; i++) {
        if (txt[i] == 'X') { found = 1; break; }
    }
    assert(found);
    textbox_destroy(&t);
}

TEST(test_move_down) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello\nWorld");
    textbox_layout(&t);
    textbox_set_cursor(&t, 2, 0);
    textbox_move_down(&t, 0);
    /* Should be somewhere in the second line */
    textbox_insert_codepoint(&t, 'X');
    int len;
    const uint32_t *txt = textbox_get_text(&t, &len);
    int found = 0;
    for (int i = 6; i < len; i++) {
        if (txt[i] == 'X') { found = 1; break; }
    }
    assert(found);
    textbox_destroy(&t);
}

TEST(test_move_line_start) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_set_cursor(&t, 8, 0);
    textbox_layout(&t);
    textbox_move_line_start(&t, 0);
    textbox_insert_codepoint(&t, 'X');
    assert(textbox_char_at(&t, 0) == 'X');
    textbox_destroy(&t);
}

TEST(test_move_line_end) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 2, 0);
    textbox_layout(&t);
    textbox_move_line_end(&t, 0);
    textbox_insert_codepoint(&t, '!');
    assert(tb_streq(&t, "Hello!"));
    textbox_destroy(&t);
}

TEST(test_move_line_start_extend) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 3, 0);
    textbox_layout(&t);
    textbox_move_line_start(&t, 1);
    assert(textbox_has_selection(&t) == 1);
    int s, e;
    textbox_selection_range(&t, &s, &e);
    assert(s == 0);
    assert(e == 3);
    textbox_destroy(&t);
}

TEST(test_move_line_end_extend) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 2, 0);
    textbox_layout(&t);
    textbox_move_line_end(&t, 1);
    assert(textbox_has_selection(&t) == 1);
    int s, e;
    textbox_selection_range(&t, &s, &e);
    assert(s == 2);
    assert(e == 5);
    textbox_destroy(&t);
}

TEST(test_move_page_up) {
    textbox_t t;
    char buf[256] = {0};
    for (int i = 0; i < 20; i++) {
        int off = (int)strlen(buf);
        snprintf(buf + off, sizeof(buf) - (size_t)off, "Line %d\n", i);
    }
    make_tb_with_text(&t, 200, 80, 1, buf);
    textbox_layout(&t);
    textbox_set_cursor(&t, textbox_len(&t), 0);
    textbox_move_page_up(&t, 0);
    /* Just verify no crash */
    textbox_destroy(&t);
}

TEST(test_move_page_down) {
    textbox_t t;
    char buf[256] = {0};
    for (int i = 0; i < 20; i++) {
        int off = (int)strlen(buf);
        snprintf(buf + off, sizeof(buf) - (size_t)off, "Line %d\n", i);
    }
    make_tb_with_text(&t, 200, 80, 1, buf);
    textbox_layout(&t);
    textbox_set_cursor(&t, 0, 0);
    textbox_move_page_down(&t, 0);
    textbox_destroy(&t);
}

TEST(test_line_for_pos) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello\nWorld\nLine3");
    textbox_layout(&t);
    int l0 = textbox_line_for_pos(&t, 0);
    int l1 = textbox_line_for_pos(&t, 7);
    int l2 = textbox_line_for_pos(&t, 13);
    assert(l0 == 0);
    assert(l1 > l0);
    assert(l2 > l1);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Word Wrap
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_word_wrap_enable) {
    textbox_t t;
    make_tb_with_text(&t, 100, 300, 1, "This is a long line that should wrap");
    textbox_set_word_wrap(&t, 1);
    textbox_layout(&t);
    int vlc = textbox_visible_line_count(&t);
    assert(vlc >= 2); /* should wrap into multiple visual lines */
    textbox_destroy(&t);
}

TEST(test_word_wrap_disable) {
    textbox_t t;
    make_tb_with_text(&t, 100, 300, 1, "This is a long line that should not wrap");
    textbox_set_word_wrap(&t, 0);
    textbox_layout(&t);
    int vlc = textbox_visible_line_count(&t);
    assert(vlc >= 1);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Event Handling — Keys
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_char) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_event_result_t r = textbox_handle_char(&t, 'A');
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == 'A');
    (void)r;
    textbox_destroy(&t);
}

TEST(test_handle_key_backspace) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 5, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_BACKSPACE, 0);
    assert(textbox_len(&t) == 4);
    assert(tb_streq(&t, "Hell"));
    textbox_destroy(&t);
}

TEST(test_handle_key_delete) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 0, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_DELETE, 0);
    assert(textbox_len(&t) == 4);
    assert(tb_streq(&t, "ello"));
    textbox_destroy(&t);
}

TEST(test_handle_key_left) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hi");
    textbox_set_cursor(&t, 2, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_LEFT, 0);
    textbox_handle_char(&t, 'X');
    assert(tb_streq(&t, "HXi"));
    textbox_destroy(&t);
}

TEST(test_handle_key_right) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hi");
    textbox_set_cursor(&t, 0, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_RIGHT, 0);
    textbox_handle_char(&t, 'X');
    assert(tb_streq(&t, "HXi"));
    textbox_destroy(&t);
}

TEST(test_handle_key_home) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 3, 0);
    textbox_layout(&t);
    textbox_handle_key(&t, TEXTBOX_KEY_HOME, 0);
    textbox_handle_char(&t, 'X');
    assert(textbox_char_at(&t, 0) == 'X');
    textbox_destroy(&t);
}

TEST(test_handle_key_end) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 2, 0);
    textbox_layout(&t);
    textbox_handle_key(&t, TEXTBOX_KEY_END, 0);
    textbox_handle_char(&t, '!');
    assert(tb_streq(&t, "Hello!"));
    textbox_destroy(&t);
}

TEST(test_handle_key_shift_left_select) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 3, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_LEFT, TEXTBOX_MOD_SHIFT);
    assert(textbox_has_selection(&t) == 1);
    textbox_destroy(&t);
}

TEST(test_handle_key_shift_right_select) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 1, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_RIGHT, TEXTBOX_MOD_SHIFT);
    assert(textbox_has_selection(&t) == 1);
    textbox_destroy(&t);
}

TEST(test_handle_key_select_all) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_handle_key(&t, TEXTBOX_KEY_A, TEXTBOX_MOD_SHORTCUT);
    assert(textbox_has_selection(&t) == 1);
    int s, e;
    textbox_selection_range(&t, &s, &e);
    assert(s == 0);
    assert(e == 5);
    textbox_destroy(&t);
}

TEST(test_handle_key_copy) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_select_all(&t);
    textbox_event_result_t r = textbox_handle_key(&t, TEXTBOX_KEY_C, TEXTBOX_MOD_SHORTCUT);
    /* Copy should request clipboard copy */
    (void)r;
    /* Text should remain unchanged */
    assert(textbox_len(&t) == 5);
    textbox_destroy(&t);
}

TEST(test_handle_key_cut) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_select_all(&t);
    textbox_event_result_t r = textbox_handle_key(&t, TEXTBOX_KEY_X, TEXTBOX_MOD_SHORTCUT);
    (void)r;
    /* Text should be deleted after cut */
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_handle_key_paste_request) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_event_result_t r = textbox_handle_key(&t, TEXTBOX_KEY_V, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_paste == 1);
    textbox_destroy(&t);
}

TEST(test_handle_key_return_multiline) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "AB");
    textbox_set_cursor(&t, 1, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_RETURN, 0);
    assert(textbox_len(&t) == 3); /* A, \n, B */
    assert(textbox_char_at(&t, 1) == '\n');
    textbox_destroy(&t);
}

TEST(test_handle_key_up_down) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello\nWorld");
    textbox_layout(&t);
    textbox_set_cursor(&t, 2, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_DOWN, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_UP, 0);
    /* Just verify no crash */
    textbox_destroy(&t);
}

TEST(test_handle_key_page_up_down) {
    textbox_t t;
    char buf[256] = {0};
    for (int i = 0; i < 20; i++) {
        int off = (int)strlen(buf);
        snprintf(buf + off, sizeof(buf) - (size_t)off, "L%d\n", i);
    }
    make_tb_with_text(&t, 200, 80, 1, buf);
    textbox_layout(&t);
    textbox_handle_key(&t, TEXTBOX_KEY_PAGE_DOWN, 0);
    textbox_handle_key(&t, TEXTBOX_KEY_PAGE_UP, 0);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Event Handling — Mouse
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_mouse_down) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_layout(&t);
    textbox_event_result_t r = textbox_handle_mouse_down(&t, 30.0f, 8.0f, 0.0f, 0);
    (void)r;
    /* Cursor should have moved */
    textbox_destroy(&t);
}

TEST(test_handle_mouse_drag_select) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_layout(&t);
    textbox_handle_mouse_down(&t, 10.0f, 8.0f, 0.0f, 0);
    textbox_handle_mouse_drag(&t, 60.0f, 8.0f);
    assert(textbox_has_selection(&t) == 1);
    textbox_handle_mouse_up(&t);
    textbox_destroy(&t);
}

TEST(test_handle_mouse_shift_click_extend) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_layout(&t);
    textbox_handle_mouse_down(&t, 10.0f, 8.0f, 0.0f, 0);
    textbox_handle_mouse_up(&t);
    textbox_handle_mouse_down(&t, 80.0f, 8.0f, 0.5f, TEXTBOX_MOD_SHIFT);
    assert(textbox_has_selection(&t) == 1);
    textbox_handle_mouse_up(&t);
    textbox_destroy(&t);
}

TEST(test_handle_mouse_double_click_word) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    textbox_layout(&t);
    /* Double-click: two clicks quickly at same position */
    textbox_handle_mouse_down(&t, 20.0f, 8.0f, 0.0f, 0);
    textbox_handle_mouse_up(&t);
    textbox_handle_mouse_down(&t, 20.0f, 8.0f, 0.1f, 0);
    textbox_handle_mouse_up(&t);
    /* Should select a word */
    if (textbox_has_selection(&t)) {
        int s, e;
        textbox_selection_range(&t, &s, &e);
        assert(e > s);
    }
    textbox_destroy(&t);
}

TEST(test_handle_mouse_triple_click_line) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello World\nSecond Line");
    textbox_layout(&t);
    /* Triple-click */
    textbox_handle_mouse_down(&t, 20.0f, 8.0f, 0.0f, 0);
    textbox_handle_mouse_up(&t);
    textbox_handle_mouse_down(&t, 20.0f, 8.0f, 0.1f, 0);
    textbox_handle_mouse_up(&t);
    textbox_handle_mouse_down(&t, 20.0f, 8.0f, 0.2f, 0);
    textbox_handle_mouse_up(&t);
    /* Should select at least something */
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Event Handling — Scroll
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_scroll) {
    textbox_t t;
    make_tb_with_text(&t, 200, 50, 1, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8");
    textbox_layout(&t);
    textbox_event_result_t r = textbox_handle_scroll(&t, 0.0f, 3.0f);
    (void)r;
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Event Handling — Focus
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_focus_gained) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_event_result_t r = textbox_handle_focus(&t, 1);
    (void)r;
    textbox_destroy(&t);
}

TEST(test_handle_focus_lost) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_handle_focus(&t, 1);
    textbox_event_result_t r = textbox_handle_focus(&t, 0);
    (void)r;
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Undo / Redo
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_undo_insert) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_utf8(&t, "Hello", 5);
    assert(textbox_len(&t) == 5);
    textbox_undo_break(&t);
    assert(textbox_can_undo(&t) == 1);
    textbox_undo(&t);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_redo_insert) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_utf8(&t, "Hello", 5);
    textbox_undo_break(&t);
    textbox_undo(&t);
    assert(textbox_len(&t) == 0);
    assert(textbox_can_redo(&t) == 1);
    textbox_redo(&t);
    assert(textbox_len(&t) == 5);
    assert(tb_streq(&t, "Hello"));
    textbox_destroy(&t);
}

TEST(test_undo_delete) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_set_cursor(&t, 5, 0);
    textbox_undo_break(&t);
    textbox_delete_back(&t, 3);
    textbox_undo_break(&t);
    assert(textbox_len(&t) == 2);
    textbox_undo(&t);
    assert(textbox_len(&t) == 5);
    assert(tb_streq(&t, "Hello"));
    textbox_destroy(&t);
}

TEST(test_undo_redo_depth) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    assert(textbox_undo_depth(&t) == 0);
    assert(textbox_redo_depth(&t) == 0);
    textbox_insert_codepoint(&t, 'A');
    textbox_undo_break(&t);
    assert(textbox_undo_depth(&t) >= 1);
    textbox_undo(&t);
    assert(textbox_redo_depth(&t) >= 1);
    textbox_destroy(&t);
}

TEST(test_undo_break) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_codepoint(&t, 'A');
    textbox_undo_break(&t);
    textbox_insert_codepoint(&t, 'B');
    textbox_undo_break(&t);
    assert(textbox_len(&t) == 2);
    textbox_undo(&t);
    assert(textbox_len(&t) == 1);
    assert(textbox_char_at(&t, 0) == 'A');
    textbox_undo(&t);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_undo_on_empty) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    assert(textbox_can_undo(&t) == 0);
    int r = textbox_undo(&t);
    assert(r == 0);
    textbox_destroy(&t);
}

TEST(test_redo_on_empty) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    assert(textbox_can_redo(&t) == 0);
    int r = textbox_redo(&t);
    assert(r == 0);
    textbox_destroy(&t);
}

TEST(test_undo_redo_style) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "ABC");
    textbox_undo_break(&t);
    textbox_set_range_style(&t, 0, 3, TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_BOLD);
    textbox_undo_break(&t);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_BOLD);
    textbox_undo(&t);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_NONE);
    textbox_redo(&t);
    assert(textbox_style_at(&t, 0) == TEXTBOX_STYLE_BOLD);
    textbox_destroy(&t);
}

TEST(test_new_edit_clears_redo) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_codepoint(&t, 'A');
    textbox_undo_break(&t);
    textbox_undo(&t);
    assert(textbox_can_redo(&t) == 1);
    textbox_insert_codepoint(&t, 'B');
    assert(textbox_can_redo(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_undo_ctrl_z_key) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_handle_char(&t, 'A');
    textbox_undo_break(&t);
    assert(textbox_len(&t) == 1);
    textbox_handle_key(&t, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_redo_ctrl_y_key) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_handle_char(&t, 'A');
    textbox_undo_break(&t);
    textbox_handle_key(&t, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(textbox_len(&t) == 0);
    textbox_handle_key(&t, TEXTBOX_KEY_Y, TEXTBOX_MOD_SHORTCUT);
    assert(textbox_len(&t) == 1);
    textbox_destroy(&t);
}

TEST(test_multiple_undo_redo) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_insert_codepoint(&t, 'A');
    textbox_undo_break(&t);
    textbox_insert_codepoint(&t, 'B');
    textbox_undo_break(&t);
    textbox_insert_codepoint(&t, 'C');
    textbox_undo_break(&t);
    assert(tb_streq(&t, "ABC"));

    textbox_undo(&t);
    assert(tb_streq(&t, "AB"));
    textbox_undo(&t);
    assert(tb_streq(&t, "A"));
    textbox_undo(&t);
    assert(textbox_len(&t) == 0);

    textbox_redo(&t);
    assert(tb_streq(&t, "A"));
    textbox_redo(&t);
    assert(tb_streq(&t, "AB"));
    textbox_redo(&t);
    assert(tb_streq(&t, "ABC"));
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  UTF-8 Encode / Decode
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_utf8_encode_ascii) {
    char buf[8];
    int n = textbox_utf8_encode('A', buf);
    assert(n == 1);
    assert(buf[0] == 'A');
}

TEST(test_utf8_encode_2byte) {
    char buf[8];
    int n = textbox_utf8_encode(0x00E9, buf); /* é */
    assert(n == 2);
    assert((unsigned char)buf[0] == 0xC3);
    assert((unsigned char)buf[1] == 0xA9);
}

TEST(test_utf8_encode_3byte) {
    char buf[8];
    int n = textbox_utf8_encode(0x4E16, buf); /* 世 */
    assert(n == 3);
}

TEST(test_utf8_encode_4byte) {
    char buf[8];
    int n = textbox_utf8_encode(0x1F600, buf); /* 😀 */
    assert(n == 4);
}

TEST(test_utf8_decode_ascii) {
    const char *p = "A";
    uint32_t cp = textbox_utf8_decode(&p);
    assert(cp == 'A');
}

TEST(test_utf8_decode_2byte) {
    const char *p = "\xC3\xA9"; /* é */
    uint32_t cp = textbox_utf8_decode(&p);
    assert(cp == 0x00E9);
}

TEST(test_utf8_decode_4byte) {
    const char *p = "\xF0\x9F\x98\x80"; /* 😀 */
    uint32_t cp = textbox_utf8_decode(&p);
    assert(cp == 0x1F600);
}

TEST(test_utf8_roundtrip) {
    uint32_t cps[] = { 'A', 0x00E9, 0x4E16, 0x1F600 };
    for (int i = 0; i < 4; i++) {
        char buf[8];
        int n = textbox_utf8_encode(cps[i], buf);
        const char *p = buf;
        uint32_t decoded = textbox_utf8_decode(&p);
        assert(decoded == cps[i]);
        assert(p == buf + n);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Style Runs
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_get_style_runs) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_set_current_style(&t, TEXTBOX_STYLE_BOLD);
    textbox_insert_utf8(&t, "AB", 2);
    textbox_set_current_style(&t, TEXTBOX_STYLE_NONE);
    textbox_insert_utf8(&t, "CD", 2);
    textbox_layout(&t);

    textbox_visible_line_t lines[8];
    int nlines = textbox_get_visible_lines(&t, lines, 8);
    assert(nlines >= 1);

    textbox_style_run_t runs[16];
    int nr = textbox_get_style_runs(&lines[0], runs, 16);
    assert(nr >= 2);
    assert(runs[0].style == TEXTBOX_STYLE_BOLD);
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Multiline Specific
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_multiline_newline) {
    textbox_t t;
    make_tb(&t, 400, 300, 1);
    textbox_insert_utf8(&t, "Hello\nWorld", -1);
    assert(textbox_len(&t) == 11);
    assert(textbox_char_at(&t, 5) == '\n');
    textbox_destroy(&t);
}

TEST(test_multiline_layout_lines) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Line1\nLine2\nLine3");
    textbox_layout(&t);
    textbox_visible_line_t lines[16];
    int n = textbox_get_visible_lines(&t, lines, 16);
    assert(n >= 3);
    textbox_destroy(&t);
}

TEST(test_multiline_insert_newline) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "AB");
    textbox_set_cursor(&t, 1, 0);
    textbox_insert_codepoint(&t, '\n');
    assert(textbox_len(&t) == 3);
    assert(textbox_char_at(&t, 0) == 'A');
    assert(textbox_char_at(&t, 1) == '\n');
    assert(textbox_char_at(&t, 2) == 'B');
    textbox_destroy(&t);
}

TEST(test_multiline_delete_newline) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "A\nB");
    textbox_set_cursor(&t, 2, 0); /* after \n */
    textbox_delete_back(&t, 1);
    assert(textbox_len(&t) == 2);
    assert(tb_streq(&t, "AB"));
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Visual Move / BiDi
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_visual_move_ltr) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_layout(&t);
    textbox_set_cursor(&t, 0, 0);
    textbox_visual_move(&t, 1, 0); /* move visually right */
    textbox_insert_codepoint(&t, 'X');
    /* X should appear after position 1 */
    assert(textbox_char_at(&t, 1) == 'X');
    textbox_destroy(&t);
}

TEST(test_cursor_is_rtl) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello");
    textbox_layout(&t);
    textbox_set_cursor(&t, 0, 0);
    int rtl = textbox_cursor_is_rtl(&t);
    assert(rtl == 0); /* Latin text is LTR */
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_empty_textbox_operations) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    textbox_layout(&t);
    textbox_cursor_rect(&t, 2.0f);
    textbox_delete_back(&t, 1);
    textbox_delete_forward(&t, 1);
    assert(textbox_len(&t) == 0);
    assert(textbox_has_selection(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_cursor_clamp) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hi");
    textbox_set_cursor(&t, 100, 0);
    /* Cursor should be clamped to [0..len] */
    textbox_insert_codepoint(&t, '!');
    /* Should not crash */
    assert(textbox_len(&t) == 3);
    textbox_destroy(&t);
}

TEST(test_cursor_negative_clamp) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hi");
    textbox_set_cursor(&t, -5, 0);
    textbox_insert_codepoint(&t, 'X');
    assert(textbox_len(&t) == 3);
    textbox_destroy(&t);
}

TEST(test_delete_more_than_available_back) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hi");
    textbox_set_cursor(&t, 1, 0);
    textbox_delete_back(&t, 10);
    /* Should delete only what's available */
    assert(textbox_len(&t) <= 2);
    textbox_destroy(&t);
}

TEST(test_delete_more_than_available_forward) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hi");
    textbox_set_cursor(&t, 1, 0);
    textbox_delete_forward(&t, 10);
    assert(textbox_len(&t) <= 2);
    textbox_destroy(&t);
}

TEST(test_set_text_long) {
    textbox_t t;
    make_tb(&t, 400, 300, 1);
    char buf[2048];
    memset(buf, 'A', sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    textbox_set_text(&t, buf);
    assert(textbox_len(&t) == 2047);
    textbox_destroy(&t);
}

TEST(test_event_result_none) {
    textbox_event_result_t r = TEXTBOX_EVENT_NONE;
    assert(r.request_paste == 0);
}

/* ═══════════════════════════════════════════════════════════════
 *  Stress Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_stress_insert_delete) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    for (int i = 0; i < 500; i++) {
        textbox_insert_codepoint(&t, 'A' + (uint32_t)(i % 26));
    }
    assert(textbox_len(&t) == 500);
    for (int i = 0; i < 500; i++) {
        textbox_delete_back(&t, 1);
    }
    assert(textbox_len(&t) == 0);
    textbox_destroy(&t);
}

TEST(test_stress_rapid_layout) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 1, "Hello\nWorld\nLine3\nLine4\nLine5");
    for (int i = 0; i < 100; i++) {
        textbox_layout(&t);
    }
    textbox_destroy(&t);
}

TEST(test_stress_undo_redo_cycle) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    for (int i = 0; i < 50; i++) {
        textbox_insert_codepoint(&t, 'A' + (uint32_t)(i % 26));
        textbox_undo_break(&t);
    }
    for (int i = 0; i < 50; i++) {
        textbox_undo(&t);
    }
    assert(textbox_len(&t) == 0);
    for (int i = 0; i < 50; i++) {
        textbox_redo(&t);
    }
    assert(textbox_len(&t) == 50);
    textbox_destroy(&t);
}

TEST(test_stress_cursor_movement) {
    textbox_t t;
    make_tb_with_text(&t, 200, 100, 1, "Hello World\nSecond Line\nThird Line");
    textbox_layout(&t);
    for (int i = 0; i < 100; i++) {
        textbox_move_right(&t, 0);
    }
    for (int i = 0; i < 100; i++) {
        textbox_move_left(&t, 0);
    }
    textbox_destroy(&t);
}

TEST(test_stress_mixed_styles) {
    textbox_t t;
    make_tb(&t, 400, 300, 0);
    uint16_t styles[] = {
        TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_ITALIC, TEXTBOX_STYLE_UNDERLINE,
        TEXTBOX_STYLE_STRIKETHROUGH, TEXTBOX_STYLE_CODE, TEXTBOX_STYLE_HIGHLIGHT
    };
    for (int i = 0; i < 60; i++) {
        textbox_set_current_style(&t, styles[i % 6]);
        textbox_insert_codepoint(&t, 'A' + (uint32_t)(i % 26));
    }
    assert(textbox_len(&t) == 60);
    for (int i = 0; i < 60; i++) {
        assert(textbox_style_at(&t, i) == styles[i % 6]);
    }
    textbox_destroy(&t);
}

TEST(test_stress_selection_paste) {
    textbox_t t;
    make_tb_with_text(&t, 400, 300, 0, "Hello World");
    for (int i = 0; i < 20; i++) {
        textbox_select_all(&t);
        textbox_paste_utf8(&t, "Replaced", -1);
    }
    assert(tb_streq(&t, "Replaced"));
    textbox_destroy(&t);
}

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_textbox\n");

    printf(" Lifecycle:\n");
    RUN(test_create_destroy);
    RUN(test_create_multiline);
    RUN(test_create_defaults);

    printf(" Basic Text Operations:\n");
    RUN(test_set_text);
    RUN(test_set_text_replaces);
    RUN(test_set_text_empty);
    RUN(test_set_text_null);
    RUN(test_clear);
    RUN(test_clear_empty);
    RUN(test_char_at);

    printf(" Insert Operations:\n");
    RUN(test_insert_codepoint);
    RUN(test_insert_multiple_codepoints);
    RUN(test_insert_codepoints_array);
    RUN(test_insert_utf8);
    RUN(test_insert_utf8_negative_len);
    RUN(test_insert_unicode);
    RUN(test_insert_emoji);
    RUN(test_insert_styled);
    RUN(test_insert_at_cursor_position);
    RUN(test_insert_at_beginning);

    printf(" Delete Operations:\n");
    RUN(test_delete_back);
    RUN(test_delete_back_multiple);
    RUN(test_delete_back_at_start);
    RUN(test_delete_forward);
    RUN(test_delete_forward_multiple);
    RUN(test_delete_forward_at_end);
    RUN(test_delete_selection);
    RUN(test_delete_selection_no_selection);
    RUN(test_delete_middle);

    printf(" Cursor & Selection:\n");
    RUN(test_set_cursor);
    RUN(test_set_cursor_extend);
    RUN(test_move_left);
    RUN(test_move_right);
    RUN(test_move_left_extend);
    RUN(test_move_right_extend);
    RUN(test_move_left_at_start);
    RUN(test_move_right_at_end);
    RUN(test_select_all);
    RUN(test_select_all_empty);
    RUN(test_selection_range_reversed);
    RUN(test_insert_replaces_selection);

    printf(" Style:\n");
    RUN(test_set_current_style);
    RUN(test_toggle_style);
    RUN(test_toggle_style_combined);
    RUN(test_insert_with_style);
    RUN(test_style_at);
    RUN(test_set_range_style);
    RUN(test_apply_style_to_selection);
    RUN(test_toggle_selection_style);
    RUN(test_get_selection_style_union);
    RUN(test_get_selection_style_intersect);
    RUN(test_all_style_flags);
    RUN(test_get_style_runs);

    printf(" UTF-8 Selection & Paste:\n");
    RUN(test_get_selection_utf8);
    RUN(test_get_selection_utf8_empty);
    RUN(test_get_selection_styled);
    RUN(test_paste_utf8);
    RUN(test_paste_utf8_negative_len);
    RUN(test_paste_utf8_replaces_selection);
    RUN(test_paste_styled);

    printf(" Text/Style Export:\n");
    RUN(test_get_text);
    RUN(test_get_styles);
    RUN(test_get_utf8);
    RUN(test_get_utf8_unicode);

    printf(" Read-only:\n");
    RUN(test_read_only_blocks_insert);
    RUN(test_read_only_blocks_delete);
    RUN(test_read_only_blocks_clear);
    RUN(test_read_only_blocks_paste);
    RUN(test_read_only_allows_selection);
    RUN(test_read_only_toggle);

    printf(" Max Length:\n");
    RUN(test_max_length);
    RUN(test_max_length_codepoint);
    RUN(test_max_length_zero_unlimited);

    printf(" Word & Line Bounds:\n");
    RUN(test_is_word_char);
    RUN(test_find_word_bounds);
    RUN(test_find_word_bounds_at_space);
    RUN(test_find_line_bounds_single);
    RUN(test_find_line_bounds_multiline);

    printf(" Layout & Scroll:\n");
    RUN(test_layout);
    RUN(test_layout_empty);
    RUN(test_scroll_to);
    RUN(test_scroll_to_x);
    RUN(test_scroll_by);
    RUN(test_scroll_to_cursor);
    RUN(test_max_scroll);
    RUN(test_scrollbar_v);
    RUN(test_scrollbar_h);
    RUN(test_set_scrollbar_v);
    RUN(test_set_scrollbar_h);

    printf(" Query:\n");
    RUN(test_visible_line_count);
    RUN(test_get_visible_lines);
    RUN(test_cursor_rect);
    RUN(test_cursor_rect_at_start);
    RUN(test_selection_rects);
    RUN(test_selection_rects_no_selection);
    RUN(test_hit_test);
    RUN(test_hit_test_middle);

    printf(" Movement (Multiline):\n");
    RUN(test_move_up);
    RUN(test_move_down);
    RUN(test_move_line_start);
    RUN(test_move_line_end);
    RUN(test_move_line_start_extend);
    RUN(test_move_line_end_extend);
    RUN(test_move_page_up);
    RUN(test_move_page_down);
    RUN(test_line_for_pos);

    printf(" Word Wrap:\n");
    RUN(test_word_wrap_enable);
    RUN(test_word_wrap_disable);

    printf(" Event Handling — Keys:\n");
    RUN(test_handle_char);
    RUN(test_handle_key_backspace);
    RUN(test_handle_key_delete);
    RUN(test_handle_key_left);
    RUN(test_handle_key_right);
    RUN(test_handle_key_home);
    RUN(test_handle_key_end);
    RUN(test_handle_key_shift_left_select);
    RUN(test_handle_key_shift_right_select);
    RUN(test_handle_key_select_all);
    RUN(test_handle_key_copy);
    RUN(test_handle_key_cut);
    RUN(test_handle_key_paste_request);
    RUN(test_handle_key_return_multiline);
    RUN(test_handle_key_up_down);
    RUN(test_handle_key_page_up_down);

    printf(" Event Handling — Mouse:\n");
    RUN(test_handle_mouse_down);
    RUN(test_handle_mouse_drag_select);
    RUN(test_handle_mouse_shift_click_extend);
    RUN(test_handle_mouse_double_click_word);
    RUN(test_handle_mouse_triple_click_line);

    printf(" Event Handling — Scroll:\n");
    RUN(test_handle_scroll);

    printf(" Event Handling — Focus:\n");
    RUN(test_handle_focus_gained);
    RUN(test_handle_focus_lost);

    printf(" Undo / Redo:\n");
    RUN(test_undo_insert);
    RUN(test_redo_insert);
    RUN(test_undo_delete);
    RUN(test_undo_redo_depth);
    RUN(test_undo_break);
    RUN(test_undo_on_empty);
    RUN(test_redo_on_empty);
    RUN(test_undo_redo_style);
    RUN(test_new_edit_clears_redo);
    RUN(test_undo_ctrl_z_key);
    RUN(test_redo_ctrl_y_key);
    RUN(test_multiple_undo_redo);

    printf(" UTF-8 Encode / Decode:\n");
    RUN(test_utf8_encode_ascii);
    RUN(test_utf8_encode_2byte);
    RUN(test_utf8_encode_3byte);
    RUN(test_utf8_encode_4byte);
    RUN(test_utf8_decode_ascii);
    RUN(test_utf8_decode_2byte);
    RUN(test_utf8_decode_4byte);
    RUN(test_utf8_roundtrip);

    printf(" Multiline Specific:\n");
    RUN(test_multiline_newline);
    RUN(test_multiline_layout_lines);
    RUN(test_multiline_insert_newline);
    RUN(test_multiline_delete_newline);

    printf(" Visual Move / BiDi:\n");
    RUN(test_visual_move_ltr);
    RUN(test_cursor_is_rtl);

    printf(" Edge Cases:\n");
    RUN(test_empty_textbox_operations);
    RUN(test_cursor_clamp);
    RUN(test_cursor_negative_clamp);
    RUN(test_delete_more_than_available_back);
    RUN(test_delete_more_than_available_forward);
    RUN(test_set_text_long);
    RUN(test_event_result_none);

    printf(" Stress Tests:\n");
    RUN(test_stress_insert_delete);
    RUN(test_stress_rapid_layout);
    RUN(test_stress_undo_redo_cycle);
    RUN(test_stress_cursor_movement);
    RUN(test_stress_mixed_styles);
    RUN(test_stress_selection_paste);

    printf("\nAll textbox tests passed.\n");
    return 0;
}
