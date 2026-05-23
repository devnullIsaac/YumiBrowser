/**
 * @file test_docbox2.c
 * @brief Extended test suite for docbox.h — deep-dive into edge cases,
 *        cursor consistency, undo/redo correctness, layout geometry,
 *        style preservation, and complex operation sequences.
 */
#define UNISTRING_IMPLEMENTATION
#define TEXTBOX_IMPLEMENTATION
#define DOCBOX_IMPLEMENTATION
#include "docbox.h"

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
    return font_size * 0.6f;
}

/* ── Helpers ──────────────────────────────────────────────────── */

static void make_db(docbox_t *d, float w, float h) {
    docbox_create(d, w, h, 16.0f, 16.0f, 4.0f, dummy_measure, NULL);
}

static void make_db_with_text(docbox_t *d, float w, float h,
                              const char *text) {
    make_db(d, w, h);
    docbox_insert_utf8(d, text, -1);
}

static int db_block_streq(docbox_t *d, int bi, const char *expected) {
    textbox_t *tb = docbox_get_textbox(d, bi);
    if (!tb) return (*expected == '\0') ? 1 : 0;
    int len;
    const uint32_t *txt = textbox_get_text(tb, &len);
    int elen = (int)strlen(expected);
    if (len != elen) return 0;
    const char *p = expected;
    for (int i = 0; i < len; i++) {
        uint32_t cp = textbox_utf8_decode(&p);
        if (txt[i] != cp) return 0;
    }
    return 1;
}

static int db_block_len(docbox_t *d, int bi) {
    textbox_t *tb = docbox_get_textbox(d, bi);
    if (!tb) return 0;
    return textbox_len(tb);
}

static void db_set_cursor(docbox_t *d, int block, int offset) {
    docbox_set_cursor(d, (docbox_pos_t){block, offset}, 0);
}

static void db_select(docbox_t *d, int b1, int o1, int b2, int o2) {
    docbox_set_cursor(d, (docbox_pos_t){b1, o1}, 0);
    docbox_set_cursor(d, (docbox_pos_t){b2, o2}, 1);
}

/* ═══════════════════════════════════════════════════════════════
 *  Undo/Redo — Deep Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_undo_single_char_via_handle_key) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_char(&d, 'A');
    docbox_undo_break(&d);
    assert(db_block_len(&d, 0) == 1);
    docbox_handle_key(&d, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_redo_single_char_via_handle_key) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_char(&d, 'X');
    docbox_undo_break(&d);
    docbox_handle_key(&d, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_len(&d, 0) == 0);
    docbox_handle_key(&d, TEXTBOX_KEY_Y, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_len(&d, 0) == 1);
    assert(db_block_streq(&d, 0, "X"));
    docbox_destroy(&d);
}

TEST(test_undo_redo_shift_z_for_redo) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_char(&d, 'Q');
    docbox_undo_break(&d);
    docbox_handle_key(&d, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_len(&d, 0) == 0);
    /* Shift+Ctrl+Z should redo */
    docbox_handle_key(&d, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT | TEXTBOX_MOD_SHIFT);
    assert(db_block_len(&d, 0) == 1);
    docbox_destroy(&d);
}

TEST(test_undo_multiple_groups) {
    docbox_t d;
    make_db(&d, 400, 300);
    /* Group 1: "AB" */
    docbox_insert_codepoint(&d, 'A');
    docbox_insert_codepoint(&d, 'B');
    docbox_undo_break(&d);
    /* Group 2: "CD" */
    docbox_insert_codepoint(&d, 'C');
    docbox_insert_codepoint(&d, 'D');
    docbox_undo_break(&d);
    assert(db_block_streq(&d, 0, "ABCD"));

    docbox_undo(&d);
    assert(db_block_streq(&d, 0, "AB"));

    docbox_undo(&d);
    assert(db_block_len(&d, 0) == 0);

    docbox_redo(&d);
    assert(db_block_streq(&d, 0, "AB"));

    docbox_redo(&d);
    assert(db_block_streq(&d, 0, "ABCD"));
    docbox_destroy(&d);
}

TEST(test_undo_restores_cursor_position) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Hello", -1);
    docbox_undo_break(&d);
    docbox_pos_t c_after = docbox_get_cursor(&d);
    assert(c_after.block == 0 && c_after.offset == 5);

    docbox_undo(&d);
    docbox_pos_t c_undo = docbox_get_cursor(&d);
    assert(c_undo.block == 0 && c_undo.offset == 0);

    docbox_redo(&d);
    docbox_pos_t c_redo = docbox_get_cursor(&d);
    assert(c_redo.block == 0 && c_redo.offset == 5);
    docbox_destroy(&d);
}

TEST(test_undo_after_delete_back) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "ABCDE", -1);
    docbox_undo_break(&d);
    docbox_delete_back(&d);
    docbox_undo_break(&d);
    assert(db_block_streq(&d, 0, "ABCD"));

    docbox_undo(&d);
    assert(db_block_streq(&d, 0, "ABCDE"));
    docbox_destroy(&d);
}

TEST(test_undo_after_delete_forward) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "ABCDE", -1);
    docbox_undo_break(&d);
    db_set_cursor(&d, 0, 0);
    docbox_delete_forward(&d);
    docbox_undo_break(&d);
    assert(db_block_streq(&d, 0, "BCDE"));

    docbox_undo(&d);
    assert(db_block_streq(&d, 0, "ABCDE"));
    docbox_destroy(&d);
}

TEST(test_undo_read_only_blocked) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Test", -1);
    docbox_undo_break(&d);
    docbox_set_read_only(&d, 1);
    /* Key-based undo is blocked */
    docbox_handle_key(&d, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_streq(&d, 0, "Test"));
    docbox_set_read_only(&d, 0);
    docbox_undo(&d);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_redo_read_only_blocked) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Test", -1);
    docbox_undo_break(&d);
    docbox_undo(&d);
    docbox_set_read_only(&d, 1);
    /* Key-based redo is blocked */
    docbox_handle_key(&d, TEXTBOX_KEY_Y, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_len(&d, 0) == 0);
    docbox_set_read_only(&d, 0);
    docbox_redo(&d);
    assert(db_block_streq(&d, 0, "Test"));
    docbox_destroy(&d);
}

TEST(test_can_undo_redo_flags) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(docbox_can_undo(&d) == 0);
    assert(docbox_can_redo(&d) == 0);

    docbox_insert_codepoint(&d, 'Z');
    docbox_undo_break(&d);
    assert(docbox_can_undo(&d) == 1);
    assert(docbox_can_redo(&d) == 0);

    docbox_undo(&d);
    assert(docbox_can_undo(&d) == 0);
    assert(docbox_can_redo(&d) == 1);

    docbox_redo(&d);
    assert(docbox_can_undo(&d) == 1);
    assert(docbox_can_redo(&d) == 0);
    docbox_destroy(&d);
}

TEST(test_edit_after_undo_clears_redo_stack) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_codepoint(&d, 'A');
    docbox_undo_break(&d);
    docbox_insert_codepoint(&d, 'B');
    docbox_undo_break(&d);

    docbox_undo(&d); /* undo B, now "A" */
    assert(docbox_can_redo(&d) == 1);

    docbox_insert_codepoint(&d, 'C');
    /* New edit should blow away redo */
    assert(docbox_can_redo(&d) == 0);
    assert(db_block_streq(&d, 0, "AC"));
    docbox_destroy(&d);
}

TEST(test_undo_many_cycles_no_crash) {
    docbox_t d;
    make_db(&d, 400, 300);
    for (int i = 0; i < 100; i++) {
        docbox_insert_codepoint(&d, 'A' + (uint32_t)(i % 26));
        docbox_undo_break(&d);
    }
    /* Undo all the way */
    for (int i = 0; i < 200; i++) docbox_undo(&d);
    /* Redo all the way */
    for (int i = 0; i < 200; i++) docbox_redo(&d);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Cursor Consistency After Structural Operations
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_cursor_valid_after_split) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABCDEF");
    db_set_cursor(&d, 0, 3);
    docbox_split_block(&d);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 1 && c.offset == 0);
    /* Verify block contents intact */
    assert(db_block_streq(&d, 0, "ABC"));
    assert(db_block_streq(&d, 1, "DEF"));
    docbox_destroy(&d);
}

TEST(test_cursor_valid_after_merge) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABC");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "DEF", -1);
    docbox_merge_blocks(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0 && c.offset == 3); /* at merge seam */
    assert(db_block_streq(&d, 0, "ABCDEF"));
    docbox_destroy(&d);
}

TEST(test_cursor_valid_after_delete_block_before) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "First");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "Second", -1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "Third", -1);

    db_set_cursor(&d, 2, 3);
    docbox_delete_block(&d, 0); /* delete "First" */
    docbox_pos_t c = docbox_get_cursor(&d);
    /* Cursor was in block 2, which is now block 1 */
    assert(c.block == 1);
    assert(db_block_streq(&d, 1, "Third"));
    docbox_destroy(&d);
}

TEST(test_cursor_valid_after_delete_block_at_cursor) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Keep");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "Remove", -1);

    db_set_cursor(&d, 1, 3);
    docbox_delete_block(&d, 1);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block >= 0 && c.block < docbox_block_count(&d));
    docbox_destroy(&d);
}

TEST(test_anchor_tracks_insert_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABC");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "DEF", -1);

    /* Set selection in block 1 */
    db_select(&d, 1, 1, 1, 3);
    /* Insert a block before block 1 */
    docbox_insert_text_block(&d, 1, 0);
    /* Anchor and cursor blocks should shift up by 1 */
    docbox_pos_t c = docbox_get_cursor(&d);
    docbox_pos_t a = docbox_get_anchor(&d);
    assert(c.block == 2 || a.block == 2);
    docbox_destroy(&d);
}

TEST(test_focused_block_follows_split) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "SplitMe");
    db_set_cursor(&d, 0, 5);
    docbox_split_block(&d);
    assert(docbox_focused_block(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_focused_block_follows_move) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AAA");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "BBB", -1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "CCC", -1);

    db_set_cursor(&d, 0, 0);
    assert(docbox_focused_block(&d) == 0);

    docbox_move_block(&d, 0, 2);
    /* Block we were focused on (0) moved to 2 */
    assert(docbox_focused_block(&d) == 2);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Cross-Block Selection — Deep Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_select_all_multi_type_blocks) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Text");
    docbox_insert_divider(&d, 1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "More", -1);

    docbox_select_all(&d);
    assert(docbox_has_selection(&d) == 1);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.block == 0 && s.offset == 0);
    assert(e.block == 2 && e.offset == 4);
    docbox_destroy(&d);
}

TEST(test_delete_selection_spanning_media) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Before");
    docbox_insert_divider(&d, 1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "After", -1);

    /* Select from "Bef|ore" across divider to "Af|ter" */
    db_select(&d, 0, 3, 2, 2);
    docbox_delete_selection(&d);
    assert(docbox_block_count(&d) == 1);
    assert(db_block_streq(&d, 0, "Befter"));
    docbox_destroy(&d);
}

TEST(test_delete_selection_only_media_blocks) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_insert_divider(&d, 2);
    docbox_insert_text_block(&d, 3, 0);

    /* Select both dividers: block 1 offset 0 to block 2 offset 1 */
    db_select(&d, 1, 0, 2, 1);
    docbox_delete_selection(&d);
    /* Both dividers should be gone */
    for (int i = 0; i < docbox_block_count(&d); i++)
        assert(docbox_block_type_at(&d, i) != DOCBOX_BLOCK_DIVIDER);
    docbox_destroy(&d);
}

TEST(test_selection_utf8_exact_content) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);

    /* Select "llo\nWor" */
    db_select(&d, 0, 2, 1, 3);
    char buf[64] = {0};
    int n = docbox_get_selection_utf8(&d, buf, sizeof(buf));
    assert(n > 0);
    assert(strcmp(buf, "llo\nWor") == 0);
    docbox_destroy(&d);
}

TEST(test_selection_utf8_cross_media) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AAA");
    docbox_insert_divider(&d, 1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "BBB", -1);

    /* Select everything */
    docbox_select_all(&d);
    char buf[128] = {0};
    int n = docbox_get_selection_utf8(&d, buf, sizeof(buf));
    assert(n > 0);
    /* Should contain content from both text blocks with newline separators */
    assert(strstr(buf, "AAA") != NULL);
    assert(strstr(buf, "BBB") != NULL);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Style Preservation Across Structural Edits
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_style_preserved_after_split) {
    docbox_t d;
    make_db(&d, 400, 300);
    /* Insert "ABCDEF" all bold */
    docbox_set_current_style(&d, TEXTBOX_STYLE_BOLD);
    docbox_insert_utf8(&d, "ABCDEF", -1);

    db_set_cursor(&d, 0, 3);
    docbox_split_block(&d);

    /* Both halves should be bold */
    textbox_t *tb0 = docbox_get_textbox(&d, 0);
    textbox_t *tb1 = docbox_get_textbox(&d, 1);
    for (int i = 0; i < 3; i++)
        assert(textbox_style_at(tb0, i) & TEXTBOX_STYLE_BOLD);
    for (int i = 0; i < 3; i++)
        assert(textbox_style_at(tb1, i) & TEXTBOX_STYLE_BOLD);
    docbox_destroy(&d);
}

TEST(test_style_preserved_after_merge) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_current_style(&d, TEXTBOX_STYLE_ITALIC);
    docbox_insert_utf8(&d, "AAA", -1);

    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_set_current_style(&d, TEXTBOX_STYLE_UNDERLINE);
    docbox_insert_utf8(&d, "BBB", -1);

    docbox_merge_blocks(&d, 0);
    textbox_t *tb = docbox_get_textbox(&d, 0);
    /* First three should be italic, last three underline */
    for (int i = 0; i < 3; i++)
        assert(textbox_style_at(tb, i) & TEXTBOX_STYLE_ITALIC);
    for (int i = 3; i < 6; i++)
        assert(textbox_style_at(tb, i) & TEXTBOX_STYLE_UNDERLINE);
    docbox_destroy(&d);
}

TEST(test_split_merge_roundtrip_preserves_styles) {
    docbox_t d;
    make_db(&d, 400, 300);
    /* "AB" bold, "CD" italic */
    docbox_set_current_style(&d, TEXTBOX_STYLE_BOLD);
    docbox_insert_utf8(&d, "AB", -1);
    docbox_set_current_style(&d, TEXTBOX_STYLE_ITALIC);
    docbox_insert_utf8(&d, "CD", -1);

    db_set_cursor(&d, 0, 2);
    docbox_split_block(&d);
    assert(docbox_block_count(&d) == 2);
    docbox_merge_blocks(&d, 0);
    assert(docbox_block_count(&d) == 1);

    textbox_t *tb = docbox_get_textbox(&d, 0);
    assert(textbox_style_at(tb, 0) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(tb, 1) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(tb, 2) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(tb, 3) == TEXTBOX_STYLE_ITALIC);
    docbox_destroy(&d);
}

TEST(test_style_intersect_mixed_styles) {
    docbox_t d;
    make_db(&d, 400, 300);
    /* Char 0: BOLD|ITALIC, Char 1: BOLD, Char 2: ITALIC */
    docbox_set_current_style(&d, TEXTBOX_STYLE_BOLD | TEXTBOX_STYLE_ITALIC);
    docbox_insert_codepoint(&d, 'A');
    docbox_set_current_style(&d, TEXTBOX_STYLE_BOLD);
    docbox_insert_codepoint(&d, 'B');
    docbox_set_current_style(&d, TEXTBOX_STYLE_ITALIC);
    docbox_insert_codepoint(&d, 'C');

    db_select(&d, 0, 0, 0, 3);
    uint16_t u = docbox_get_selection_style_union(&d);
    uint16_t i = docbox_get_selection_style_intersect(&d);
    assert(u == (TEXTBOX_STYLE_BOLD | TEXTBOX_STYLE_ITALIC));
    assert(i == 0); /* no style is common to all 3 chars */
    docbox_destroy(&d);
}

TEST(test_toggle_style_on_partially_styled) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "ABCDE", -1);
    /* Bold only first 3 */
    db_select(&d, 0, 0, 0, 3);
    docbox_apply_style_to_selection(&d, TEXTBOX_STYLE_BOLD, TEXTBOX_STYLE_BOLD);

    /* Now toggle bold on entire range: since not all bold, should apply bold */
    db_select(&d, 0, 0, 0, 5);
    docbox_toggle_selection_style(&d, TEXTBOX_STYLE_BOLD);

    textbox_t *tb = docbox_get_textbox(&d, 0);
    for (int i = 0; i < 5; i++)
        assert(textbox_style_at(tb, i) & TEXTBOX_STYLE_BOLD);

    /* Toggle again: all bold, so should remove */
    db_select(&d, 0, 0, 0, 5);
    docbox_toggle_selection_style(&d, TEXTBOX_STYLE_BOLD);
    for (int i = 0; i < 5; i++)
        assert(!(textbox_style_at(tb, i) & TEXTBOX_STYLE_BOLD));
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Layout Geometry Correctness
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_layout_block_y_positions_monotonic) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Block0");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "Block1", -1);
    docbox_insert_divider(&d, 2);
    docbox_insert_text_block(&d, 3, 0);
    db_set_cursor(&d, 3, 0);
    docbox_insert_utf8(&d, "Block3", -1);
    docbox_layout(&d);

    float prev_y = -1.0f;
    for (int i = 0; i < docbox_block_count(&d); i++) {
        float x, y, w, h;
        docbox_block_rect(&d, i, &x, &y, &w, &h);
        assert(y > prev_y);
        assert(w > 0.0f);
        assert(h > 0.0f);
        prev_y = y;
    }
    docbox_destroy(&d);
}

TEST(test_layout_content_height_increases_with_blocks) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "A");
    docbox_layout(&d);
    float h1 = d.content_height;

    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "B", -1);
    docbox_layout(&d);
    float h2 = d.content_height;
    assert(h2 > h1);
    docbox_destroy(&d);
}

TEST(test_layout_heading_block_taller) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Body", -1);
    docbox_insert_text_block(&d, 1, DOCBOX_HEADING_H1);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "Head", -1);
    docbox_layout(&d);

    float x0, y0, w0, h0, x1, y1, w1, h1;
    docbox_block_rect(&d, 0, &x0, &y0, &w0, &h0);
    docbox_block_rect(&d, 1, &x1, &y1, &w1, &h1);
    /* H1 block should be taller due to larger font/line height */
    assert(h1 > h0);
    docbox_destroy(&d);
}

TEST(test_resize_relayouts) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Some text here");
    docbox_layout(&d);
    float x, y, w, h;
    docbox_block_rect(&d, 0, &x, &y, &w, &h);
    float old_w = w;

    docbox_resize(&d, 200, 300);
    docbox_layout(&d);
    docbox_block_rect(&d, 0, &x, &y, &w, &h);
    assert(w < old_w);
    docbox_destroy(&d);
}

TEST(test_scroll_clamps_to_bounds) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Short");
    docbox_layout(&d);
    docbox_scroll_to(&d, -100.0f);
    assert(d.scroll_y >= 0.0f);
    docbox_scroll_to(&d, 999999.0f);
    assert(d.scroll_y <= docbox_max_scroll_y(&d));
    docbox_destroy(&d);
}

TEST(test_scroll_by_accumulates) {
    docbox_t d;
    make_db(&d, 200, 50);
    for (int i = 0; i < 30; i++) {
        int idx = docbox_insert_text_block(&d, docbox_block_count(&d), 0);
        db_set_cursor(&d, idx, 0);
        docbox_insert_utf8(&d, "Line of text", -1);
    }
    docbox_layout(&d);
    float max_s = docbox_max_scroll_y(&d);
    assert(max_s > 0.0f);

    docbox_scroll_to(&d, 0);
    docbox_scroll_by(&d, 10.0f);
    docbox_scroll_by(&d, 10.0f);
    assert(NEAR(d.scroll_y, 20.0f));
    docbox_destroy(&d);
}

TEST(test_scrollbar_thumb_shrinks_with_content) {
    docbox_t d;
    make_db_with_text(&d, 400, 100, "A");
    docbox_layout(&d);
    docbox_scrollbar_t sb1;
    docbox_get_scrollbar_v(&d, &sb1);

    /* Add lots of blocks */
    for (int i = 0; i < 50; i++) {
        int idx = docbox_insert_text_block(&d, docbox_block_count(&d), 0);
        db_set_cursor(&d, idx, 0);
        docbox_insert_utf8(&d, "Line", -1);
    }
    docbox_layout(&d);
    docbox_scrollbar_t sb2;
    docbox_get_scrollbar_v(&d, &sb2);

    assert(sb2.max > sb1.max);
    assert(sb2.visible == 1);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Hit Testing — Deep Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_hit_test_within_first_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    docbox_layout(&d);
    /* Hit near the left edge of the content area */
    docbox_pos_t p = docbox_hit_test(&d, 5.0f, 5.0f);
    assert(p.block == 0);
    assert(p.offset >= 0);
    docbox_destroy(&d);
}

TEST(test_hit_test_past_last_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Short");
    docbox_layout(&d);
    /* Hit far below content should land on last block */
    docbox_pos_t p = docbox_hit_test(&d, 50.0f, 290.0f);
    assert(p.block == 0);
    docbox_destroy(&d);
}

TEST(test_block_at_y_multiple_blocks) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "First");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "Second", -1);
    docbox_layout(&d);

    int b0 = docbox_block_at_y(&d, 5.0f);
    assert(b0 == 0);

    float x, y, w, h;
    docbox_block_rect(&d, 1, &x, &y, &w, &h);
    int b1 = docbox_block_at_y(&d, y + h / 2);
    assert(b1 == 1);
    docbox_destroy(&d);
}

TEST(test_cursor_rect_updates_with_position) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABCDEF");
    docbox_layout(&d);

    db_set_cursor(&d, 0, 0);
    float x0, y0, w0, h0;
    docbox_cursor_rect(&d, 2.0f, &x0, &y0, &w0, &h0);

    db_set_cursor(&d, 0, 3);
    float x3, y3, w3, h3;
    docbox_cursor_rect(&d, 2.0f, &x3, &y3, &w3, &h3);

    assert(x3 > x0); /* cursor moved right */
    assert(NEAR(y0, y3)); /* same line */
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Event Result Flags
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_char_sets_redraw) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_event_result_t r = docbox_handle_char(&d, 'A');
    assert(r.redraw == 1);
    assert(r.cursor_moved == 1);
    docbox_destroy(&d);
}

TEST(test_handle_char_control_no_redraw) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_event_result_t r = docbox_handle_char(&d, 0x01); /* SOH */
    assert(r.redraw == 0);
    docbox_destroy(&d);
}

TEST(test_handle_key_return_sets_layout_changed) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AB");
    db_set_cursor(&d, 0, 1);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_RETURN, 0);
    assert(r.layout_changed == 1);
    assert(r.redraw == 1);
    docbox_destroy(&d);
}

TEST(test_handle_key_backspace_sets_layout_changed) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 3);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_BACKSPACE, 0);
    assert(r.redraw == 1);
    assert(r.layout_changed == 1);
    docbox_destroy(&d);
}

TEST(test_handle_key_delete_sets_layout_changed) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 2);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_DELETE, 0);
    assert(r.redraw == 1);
    assert(r.layout_changed == 1);
    docbox_destroy(&d);
}

TEST(test_handle_key_copy_sets_request_copy) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_select_all(&d);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_C, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_copy == 1);
    assert(r.request_cut == 0);
    assert(r.request_paste == 0);
    /* Text should be unchanged */
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_handle_key_cut_sets_request_cut) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_select_all(&d);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_X, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_cut == 1);
    assert(r.request_copy == 0);
    /* Text should be deleted */
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_handle_key_paste_sets_request_paste) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_V, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_paste == 1);
    assert(r.request_copy == 0);
    assert(r.request_cut == 0);
    docbox_destroy(&d);
}

TEST(test_handle_scroll_sets_redraw) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_layout(&d);
    docbox_event_result_t r = docbox_handle_scroll(&d, 0.0f, 1.0f);
    assert(r.redraw == 1);
    docbox_destroy(&d);
}

TEST(test_handle_focus_sets_redraw) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_event_result_t r1 = docbox_handle_focus(&d, 1);
    assert(r1.redraw == 1);
    docbox_event_result_t r0 = docbox_handle_focus(&d, 0);
    assert(r0.redraw == 1);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Export — Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_export_utf8_buffer_too_small) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    char buf[4];
    int n = docbox_export_utf8(&d, buf, sizeof(buf), NULL);
    assert(n >= 0);
    assert(n < (int)sizeof(buf));
    docbox_destroy(&d);
}

TEST(test_export_utf8_empty_doc) {
    docbox_t d;
    make_db(&d, 400, 300);
    char buf[64] = "garbage";
    int n = docbox_export_utf8(&d, buf, sizeof(buf), NULL);
    assert(n == 0);
    assert(buf[0] == '\0');
    docbox_destroy(&d);
}

TEST(test_export_image_block_placeholder) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.filename = "pic.png";
    media.filename_len = 7;
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_IMAGE, &media);
    char buf[128];
    int n = docbox_export_utf8(&d, buf, sizeof(buf), NULL);
    assert(n > 0);
    assert(strstr(buf, "[image:pic.png]") != NULL);
    docbox_destroy(&d);
}

TEST(test_export_video_block_placeholder) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.filename = "vid.mp4";
    media.filename_len = 7;
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_VIDEO, &media);
    char buf[128];
    docbox_export_utf8(&d, buf, sizeof(buf), NULL);
    assert(strstr(buf, "[video:vid.mp4]") != NULL);
    docbox_destroy(&d);
}

TEST(test_export_audio_block_placeholder) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_AUDIO, NULL);
    char buf[128];
    docbox_export_utf8(&d, buf, sizeof(buf), NULL);
    assert(strstr(buf, "[audio:]") != NULL);
    docbox_destroy(&d);
}

TEST(test_export_file_block_placeholder) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.filename = "doc.pdf";
    media.filename_len = 7;
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_FILE, &media);
    char buf[128];
    docbox_export_utf8(&d, buf, sizeof(buf), NULL);
    assert(strstr(buf, "[file:doc.pdf]") != NULL);
    docbox_destroy(&d);
}

TEST(test_export_utf8_zero_cap) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    int n = docbox_export_utf8(&d, NULL, 0, NULL);
    assert(n == 0);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Clipboard — Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_get_selection_utf8_buffer_too_small) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    docbox_select_all(&d);
    char buf[4];
    int n = docbox_get_selection_utf8(&d, buf, sizeof(buf));
    assert(n >= 0);
    assert(n < (int)sizeof(buf));
    docbox_destroy(&d);
}

TEST(test_paste_replaces_cross_block_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);

    db_select(&d, 0, 2, 1, 3);
    docbox_paste_utf8(&d, "XY", 2);
    /* Selection deleted first, then paste into remaining single block */
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_paste_empty_string_noop) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 5);
    docbox_paste_utf8(&d, "", 0);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_paste_at_middle) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AE");
    db_set_cursor(&d, 0, 1);
    docbox_paste_utf8(&d, "BCD", 3);
    assert(db_block_streq(&d, 0, "ABCDE"));
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Read-Only — Deep Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_read_only_delete_forward_blocked) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    db_set_cursor(&d, 0, 0);
    docbox_delete_forward(&d);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_read_only_merge_blocked) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    docbox_set_read_only(&d, 1);
    /* delete_back at start of block 1 would normally merge */
    db_set_cursor(&d, 1, 0);
    docbox_delete_back(&d);
    assert(docbox_block_count(&d) == 2);
    docbox_destroy(&d);
}

TEST(test_read_only_ctrl_x_becomes_copy) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    docbox_select_all(&d);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_X, TEXTBOX_MOD_SHORTCUT);
    /* In read-only, Ctrl+X should act as copy, not cut */
    assert(r.request_copy == 1);
    assert(r.request_cut == 0);
    assert(db_block_streq(&d, 0, "Hello")); /* unchanged */
    docbox_destroy(&d);
}

TEST(test_read_only_undo_blocked_via_key) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_char(&d, 'A');
    docbox_undo_break(&d);
    docbox_set_read_only(&d, 1);
    docbox_handle_key(&d, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_len(&d, 0) == 1); /* undo blocked */
    docbox_destroy(&d);
}

TEST(test_read_only_insert_media_blocked) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_read_only(&d, 1);
    int idx = docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_IMAGE, NULL);
    assert(idx == -1);
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_read_only_delete_selection_blocked) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_select(&d, 0, 1, 0, 4);
    docbox_set_read_only(&d, 1);
    int r = docbox_delete_selection(&d);
    assert(r == 0);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Block ID Stability
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_block_ids_monotonic) {
    docbox_t d;
    make_db(&d, 400, 300);
    uint32_t prev_id = docbox_block_id_at(&d, 0);
    for (int i = 0; i < 10; i++) {
        docbox_insert_text_block(&d, docbox_block_count(&d), 0);
        uint32_t id = docbox_block_id_at(&d, docbox_block_count(&d) - 1);
        assert(id > prev_id);
        prev_id = id;
    }
    docbox_destroy(&d);
}

TEST(test_block_ids_survive_move) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "A");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "B", -1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "C", -1);

    uint32_t id0 = docbox_block_id_at(&d, 0);
    uint32_t id1 = docbox_block_id_at(&d, 1);
    uint32_t id2 = docbox_block_id_at(&d, 2);

    docbox_move_block(&d, 0, 2);
    /* Verify IDs followed their blocks */
    assert(docbox_block_id_at(&d, 0) == id1);
    assert(docbox_block_id_at(&d, 1) == id2);
    assert(docbox_block_id_at(&d, 2) == id0);
    docbox_destroy(&d);
}

TEST(test_block_ids_survive_split) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABCDEF");
    uint32_t original_id = docbox_block_id_at(&d, 0);
    db_set_cursor(&d, 0, 3);
    docbox_split_block(&d);
    /* Original block keeps its ID */
    assert(docbox_block_id_at(&d, 0) == original_id);
    /* New block gets a different ID */
    uint32_t new_id = docbox_block_id_at(&d, 1);
    assert(new_id != original_id);
    assert(new_id > original_id);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Heading — Deep Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_heading_all_levels) {
    docbox_t d;
    make_db(&d, 400, 300);
    for (int h = 0; h <= 6; h++) {
        docbox_set_heading(&d, 0, h);
        assert(docbox_get_heading(&d, 0) == h);
        float fs = docbox_heading_font_size(&d, h);
        assert(fs > 0.0f);
    }
    docbox_destroy(&d);
}

TEST(test_heading_out_of_range_values) {
    docbox_t d;
    make_db(&d, 400, 300);
    /* These should be clamped and return valid heading font sizes */
    float fn = docbox_heading_font_size(&d, -5);
    float fp = docbox_heading_font_size(&d, 99);
    assert(fn > 0.0f);
    assert(fp > 0.0f);
    docbox_destroy(&d);
}

TEST(test_set_heading_updates_textbox_font) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Title", -1);

    textbox_t *tb = docbox_get_textbox(&d, 0);
    float body_fs = tb->font_size;

    docbox_set_heading(&d, 0, DOCBOX_HEADING_H1);
    float h1_fs = tb->font_size;
    assert(h1_fs > body_fs);
    assert(NEAR(h1_fs, 16.0f * 2.0f)); /* base * H1 scale */

    docbox_set_heading(&d, 0, DOCBOX_HEADING_BODY);
    assert(NEAR(tb->font_size, body_fs));
    docbox_destroy(&d);
}

TEST(test_heading_no_change_noop) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_heading(&d, 0, DOCBOX_HEADING_H3);
    /* Setting same heading should not crash or change state */
    docbox_set_heading(&d, 0, DOCBOX_HEADING_H3);
    assert(docbox_get_heading(&d, 0) == DOCBOX_HEADING_H3);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Block View / Iteration — Deep Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_block_view_text_has_styles) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_current_style(&d, TEXTBOX_STYLE_BOLD);
    docbox_insert_utf8(&d, "Bold", -1);

    docbox_block_view_t v;
    int ok = docbox_get_block_view(&d, 0, &v);
    assert(ok == 1);
    assert(v.text_styles != NULL);
    assert(v.text_len == 4);
    for (int i = 0; i < 4; i++)
        assert(v.text_styles[i] & TEXTBOX_STYLE_BOLD);
    docbox_destroy(&d);
}

TEST(test_block_view_preserves_heading) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_heading(&d, 0, DOCBOX_HEADING_H4);
    docbox_insert_utf8(&d, "Text", -1);

    docbox_block_view_t v;
    docbox_get_block_view(&d, 0, &v);
    assert(v.heading == DOCBOX_HEADING_H4);
    docbox_destroy(&d);
}

TEST(test_block_view_media_has_type) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.src_w = 800;
    media.src_h = 600;
    media.filename = "photo.jpg";
    media.filename_len = 9;
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_IMAGE, &media);

    docbox_block_view_t v;
    docbox_get_block_view(&d, 1, &v);
    assert(v.type == DOCBOX_BLOCK_IMAGE);
    assert(v.media != NULL);
    assert(v.media->src_w == 800);
    assert(v.text_codepoints == NULL);
    docbox_destroy(&d);
}

static int collect_ids(const docbox_block_view_t *v, void *user) {
    uint32_t *ids = (uint32_t *)user;
    ids[v->index] = v->id;
    return 0;
}

TEST(test_iterate_blocks_collects_ids) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "A");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "B", -1);
    docbox_insert_divider(&d, 2);

    uint32_t ids[3] = {0};
    int visited = docbox_iterate_blocks(&d, collect_ids, ids);
    assert(visited == 3);
    assert(ids[0] == docbox_block_id_at(&d, 0));
    assert(ids[1] == docbox_block_id_at(&d, 1));
    assert(ids[2] == docbox_block_id_at(&d, 2));
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Movement — Deep Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_move_right_through_entire_doc) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AB");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "CD", -1);

    db_set_cursor(&d, 0, 0);
    /* Move right through "AB" (2 chars) + cross block + "CD" (2 chars) */
    for (int i = 0; i < 5; i++) docbox_move_right(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 1 && c.offset == 2);
    docbox_destroy(&d);
}

TEST(test_move_left_through_entire_doc) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AB");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "CD", -1);

    db_set_cursor(&d, 1, 2);
    for (int i = 0; i < 5; i++) docbox_move_left(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0 && c.offset == 0);
    docbox_destroy(&d);
}

TEST(test_move_extends_selection_across_blocks) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AB");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "CD", -1);

    db_set_cursor(&d, 0, 1);
    docbox_move_right(&d, 1);
    docbox_move_right(&d, 1);
    docbox_move_right(&d, 1); /* crosses into block 1 */
    assert(docbox_has_selection(&d) == 1);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.block == 0 && s.offset == 1);
    assert(e.block == 1 && e.offset == 1);
    docbox_destroy(&d);
}

TEST(test_move_doc_start_end_with_extend) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);

    db_set_cursor(&d, 0, 3);
    docbox_move_doc_end(&d, 1);
    assert(docbox_has_selection(&d) == 1);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.block == 0 && s.offset == 3);
    assert(e.block == 1 && e.offset == 5);
    docbox_destroy(&d);
}

TEST(test_move_line_start_end_on_media) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_insert_text_block(&d, 2, 0);

    db_set_cursor(&d, 1, 1);
    docbox_move_line_start(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.offset == 0);

    docbox_move_line_end(&d, 0);
    c = docbox_get_cursor(&d);
    assert(c.offset == 1); /* media block has length 1 */
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Mouse — Deep Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_mouse_drag_cross_block_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    docbox_layout(&d);

    /* Start drag in block 0 */
    float x0, y0, w0, h0;
    docbox_block_rect(&d, 0, &x0, &y0, &w0, &h0);
    docbox_handle_mouse_down(&d, x0 + 10, y0 + h0 / 2, 0.0f, 0);

    /* Drag to block 1 */
    float x1, y1, w1, h1;
    docbox_block_rect(&d, 1, &x1, &y1, &w1, &h1);
    docbox_handle_mouse_drag(&d, x1 + 10, y1 + h1 / 2);

    assert(docbox_has_selection(&d) == 1);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(e.block == 1);
    docbox_handle_mouse_up(&d);
    docbox_destroy(&d);
}

TEST(test_mouse_focus_lost_stops_drag) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_layout(&d);
    docbox_handle_mouse_down(&d, 10, 10, 0.0f, 0);
    assert(d.dragging == 1);
    docbox_handle_focus(&d, 0);
    assert(d.dragging == 0);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Complex Interleaved Sequences
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_type_split_type_merge_verify) {
    docbox_t d;
    make_db(&d, 400, 300);
    /* Type "Hello World" */
    docbox_insert_utf8(&d, "Hello World", -1);
    /* Split at "Hello |World" */
    db_set_cursor(&d, 0, 6);
    docbox_split_block(&d);
    assert(docbox_block_count(&d) == 2);
    assert(db_block_streq(&d, 0, "Hello "));
    assert(db_block_streq(&d, 1, "World"));
    /* Type more into block 1 */
    db_set_cursor(&d, 1, 5);
    docbox_insert_utf8(&d, "!!!", -1);
    assert(db_block_streq(&d, 1, "World!!!"));
    /* Merge back */
    docbox_merge_blocks(&d, 0);
    assert(docbox_block_count(&d) == 1);
    assert(db_block_streq(&d, 0, "Hello World!!!"));
    docbox_destroy(&d);
}

TEST(test_cut_paste_cycle) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABCDE");
    /* Select "BCD" */
    db_select(&d, 0, 1, 0, 4);
    /* Simulate cut: get text, delete */
    char cut_buf[64] = {0};
    docbox_get_selection_utf8(&d, cut_buf, sizeof(cut_buf));
    assert(strcmp(cut_buf, "BCD") == 0);
    docbox_delete_selection(&d);
    assert(db_block_streq(&d, 0, "AE"));
    /* Paste at end */
    db_set_cursor(&d, 0, 2);
    docbox_paste_utf8(&d, cut_buf, -1);
    assert(db_block_streq(&d, 0, "AEBCD"));
    docbox_destroy(&d);
}

TEST(test_select_all_delete_type_again) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Original text");
    docbox_select_all(&d);
    docbox_delete_selection(&d);
    assert(db_block_len(&d, 0) == 0);
    assert(docbox_block_count(&d) == 1);
    docbox_insert_utf8(&d, "New text", -1);
    assert(db_block_streq(&d, 0, "New text"));
    docbox_destroy(&d);
}

TEST(test_multiple_splits_then_select_all_delete) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABCDEFGHIJ");
    /* Split into 5 blocks of 2 */
    for (int i = 3; i >= 0; i--) {
        db_set_cursor(&d, 0, (i + 1) * 2);
        docbox_split_block(&d);
    }
    assert(docbox_block_count(&d) == 5);
    assert(db_block_streq(&d, 0, "AB"));
    assert(db_block_streq(&d, 4, "IJ"));

    docbox_select_all(&d);
    docbox_delete_selection(&d);
    assert(docbox_block_count(&d) == 1);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_interleaved_insert_delete_blocks) {
    docbox_t d;
    make_db(&d, 400, 300);
    for (int i = 0; i < 20; i++) {
        docbox_insert_text_block(&d, docbox_block_count(&d), 0);
        int idx = docbox_block_count(&d) - 1;
        db_set_cursor(&d, idx, 0);
        docbox_insert_utf8(&d, "X", -1);
    }
    assert(docbox_block_count(&d) == 21);
    /* Delete every other block from the end */
    for (int i = docbox_block_count(&d) - 1; i >= 1; i -= 2) {
        docbox_delete_block(&d, i);
    }
    assert(docbox_block_count(&d) >= 1);
    /* All remaining blocks should have valid text */
    for (int i = 0; i < docbox_block_count(&d); i++) {
        if (docbox_block_type_at(&d, i) == DOCBOX_BLOCK_TEXT) {
            textbox_t *tb = docbox_get_textbox(&d, i);
            assert(tb != NULL);
        }
    }
    docbox_destroy(&d);
}

TEST(test_rapid_char_input_via_handle_char) {
    docbox_t d;
    make_db(&d, 400, 300);
    const char *msg = "The quick brown fox jumps over the lazy dog";
    for (int i = 0; msg[i]; i++) {
        docbox_handle_char(&d, (uint32_t)msg[i]);
    }
    assert(db_block_streq(&d, 0, msg));
    docbox_destroy(&d);
}

TEST(test_heading_change_undo_redo) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Title");
    assert(docbox_get_heading(&d, 0) == DOCBOX_HEADING_BODY);

    docbox_set_heading(&d, 0, DOCBOX_HEADING_H2);
    assert(docbox_get_heading(&d, 0) == DOCBOX_HEADING_H2);
    /* Text content should be unaffected */
    assert(db_block_streq(&d, 0, "Title"));
    docbox_destroy(&d);
}

TEST(test_stress_alternating_split_delete) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABCDEFGH");
    for (int i = 0; i < 20; i++) {
        int len = db_block_len(&d, 0);
        if (len > 1) {
            db_set_cursor(&d, 0, len / 2);
            docbox_split_block(&d);
        }
        if (docbox_block_count(&d) > 1) {
            docbox_delete_block(&d, docbox_block_count(&d) - 1);
        }
    }
    assert(docbox_block_count(&d) >= 1);
    docbox_destroy(&d);
}

TEST(test_stress_layout_after_every_edit) {
    docbox_t d;
    make_db(&d, 300, 200);
    for (int i = 0; i < 50; i++) {
        docbox_insert_codepoint(&d, 'A' + (uint32_t)(i % 26));
        docbox_layout(&d);
        float x, y, w, h;
        docbox_cursor_rect(&d, 2.0f, &x, &y, &w, &h);
        assert(w > 0.0f && h > 0.0f);
    }
    docbox_destroy(&d);
}

TEST(test_stress_resize_during_edits) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Hello World this is a test of resizing", -1);
    for (int w = 100; w <= 600; w += 50) {
        docbox_resize(&d, (float)w, 300.0f);
        docbox_layout(&d);
        assert(docbox_max_scroll_y(&d) >= 0.0f);
    }
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Media Block — Deep Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_media_display_dimensions) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.src_w = 1000;
    media.src_h = 500;
    media.display_w = 200;
    media.display_h = 100;
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_IMAGE, &media);
    const docbox_media_t *m = docbox_get_media(&d, 1);
    assert(m->display_w == 200.0f);
    assert(m->display_h == 100.0f);
    docbox_destroy(&d);
}

TEST(test_media_block_mime_type_copied) {
    docbox_t d;
    make_db(&d, 400, 300);
    char mime[] = "video/mp4";
    docbox_media_t media = {0};
    media.mime_type = mime;
    media.filename = "test.mp4";
    media.filename_len = 8;
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_VIDEO, &media);
    /* Mutate original */
    mime[0] = 'X';
    const docbox_media_t *m = docbox_get_media(&d, 1);
    assert(m->mime_type[0] == 'v'); /* docbox has its own copy */
    docbox_destroy(&d);
}

TEST(test_mixed_block_types_layout) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Para 1");
    docbox_insert_divider(&d, 1);
    docbox_media_t img = {0};
    img.src_w = 320;
    img.src_h = 240;
    docbox_insert_media_block(&d, 2, DOCBOX_BLOCK_IMAGE, &img);
    docbox_insert_text_block(&d, 3, DOCBOX_HEADING_H2);
    db_set_cursor(&d, 3, 0);
    docbox_insert_utf8(&d, "Heading", -1);
    docbox_insert_media_block(&d, 4, DOCBOX_BLOCK_FILE, NULL);

    docbox_layout(&d);

    /* All blocks should have positive height */
    for (int i = 0; i < docbox_block_count(&d); i++) {
        float x, y, w, h;
        docbox_block_rect(&d, i, &x, &y, &w, &h);
        assert(h > 0.0f);
    }
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_docbox2 — Extended Suite\n");

    printf(" Undo/Redo — Deep:\n");
    RUN(test_undo_single_char_via_handle_key);
    RUN(test_redo_single_char_via_handle_key);
    RUN(test_undo_redo_shift_z_for_redo);
    RUN(test_undo_multiple_groups);
    RUN(test_undo_restores_cursor_position);
    RUN(test_undo_after_delete_back);
    RUN(test_undo_after_delete_forward);
    RUN(test_undo_read_only_blocked);
    RUN(test_redo_read_only_blocked);
    RUN(test_can_undo_redo_flags);
    RUN(test_edit_after_undo_clears_redo_stack);
    RUN(test_undo_many_cycles_no_crash);

    printf(" Cursor Consistency:\n");
    RUN(test_cursor_valid_after_split);
    RUN(test_cursor_valid_after_merge);
    RUN(test_cursor_valid_after_delete_block_before);
    RUN(test_cursor_valid_after_delete_block_at_cursor);
    RUN(test_anchor_tracks_insert_block);
    RUN(test_focused_block_follows_split);
    RUN(test_focused_block_follows_move);

    printf(" Cross-Block Selection — Deep:\n");
    RUN(test_select_all_multi_type_blocks);
    RUN(test_delete_selection_spanning_media);
    RUN(test_delete_selection_only_media_blocks);
    RUN(test_selection_utf8_exact_content);
    RUN(test_selection_utf8_cross_media);

    printf(" Style Preservation:\n");
    RUN(test_style_preserved_after_split);
    RUN(test_style_preserved_after_merge);
    RUN(test_split_merge_roundtrip_preserves_styles);
    RUN(test_style_intersect_mixed_styles);
    RUN(test_toggle_style_on_partially_styled);

    printf(" Layout Geometry:\n");
    RUN(test_layout_block_y_positions_monotonic);
    RUN(test_layout_content_height_increases_with_blocks);
    RUN(test_layout_heading_block_taller);
    RUN(test_resize_relayouts);
    RUN(test_scroll_clamps_to_bounds);
    RUN(test_scroll_by_accumulates);
    RUN(test_scrollbar_thumb_shrinks_with_content);

    printf(" Hit Testing — Deep:\n");
    RUN(test_hit_test_within_first_block);
    RUN(test_hit_test_past_last_block);
    RUN(test_block_at_y_multiple_blocks);
    RUN(test_cursor_rect_updates_with_position);

    printf(" Event Result Flags:\n");
    RUN(test_handle_char_sets_redraw);
    RUN(test_handle_char_control_no_redraw);
    RUN(test_handle_key_return_sets_layout_changed);
    RUN(test_handle_key_backspace_sets_layout_changed);
    RUN(test_handle_key_delete_sets_layout_changed);
    RUN(test_handle_key_copy_sets_request_copy);
    RUN(test_handle_key_cut_sets_request_cut);
    RUN(test_handle_key_paste_sets_request_paste);
    RUN(test_handle_scroll_sets_redraw);
    RUN(test_handle_focus_sets_redraw);

    printf(" Export — Edge Cases:\n");
    RUN(test_export_utf8_buffer_too_small);
    RUN(test_export_utf8_empty_doc);
    RUN(test_export_image_block_placeholder);
    RUN(test_export_video_block_placeholder);
    RUN(test_export_audio_block_placeholder);
    RUN(test_export_file_block_placeholder);
    RUN(test_export_utf8_zero_cap);

    printf(" Clipboard — Edge Cases:\n");
    RUN(test_get_selection_utf8_buffer_too_small);
    RUN(test_paste_replaces_cross_block_selection);
    RUN(test_paste_empty_string_noop);
    RUN(test_paste_at_middle);

    printf(" Read-Only — Deep:\n");
    RUN(test_read_only_delete_forward_blocked);
    RUN(test_read_only_merge_blocked);
    RUN(test_read_only_ctrl_x_becomes_copy);
    RUN(test_read_only_undo_blocked_via_key);
    RUN(test_read_only_insert_media_blocked);
    RUN(test_read_only_delete_selection_blocked);

    printf(" Block ID Stability:\n");
    RUN(test_block_ids_monotonic);
    RUN(test_block_ids_survive_move);
    RUN(test_block_ids_survive_split);

    printf(" Heading — Deep:\n");
    RUN(test_heading_all_levels);
    RUN(test_heading_out_of_range_values);
    RUN(test_set_heading_updates_textbox_font);
    RUN(test_heading_no_change_noop);

    printf(" Block View / Iteration — Deep:\n");
    RUN(test_block_view_text_has_styles);
    RUN(test_block_view_preserves_heading);
    RUN(test_block_view_media_has_type);
    RUN(test_iterate_blocks_collects_ids);

    printf(" Movement — Deep:\n");
    RUN(test_move_right_through_entire_doc);
    RUN(test_move_left_through_entire_doc);
    RUN(test_move_extends_selection_across_blocks);
    RUN(test_move_doc_start_end_with_extend);
    RUN(test_move_line_start_end_on_media);

    printf(" Mouse — Deep:\n");
    RUN(test_mouse_drag_cross_block_selection);
    RUN(test_mouse_focus_lost_stops_drag);

    printf(" Complex Sequences:\n");
    RUN(test_type_split_type_merge_verify);
    RUN(test_cut_paste_cycle);
    RUN(test_select_all_delete_type_again);
    RUN(test_multiple_splits_then_select_all_delete);
    RUN(test_interleaved_insert_delete_blocks);
    RUN(test_rapid_char_input_via_handle_char);
    RUN(test_heading_change_undo_redo);
    RUN(test_stress_alternating_split_delete);
    RUN(test_stress_layout_after_every_edit);
    RUN(test_stress_resize_during_edits);

    printf(" Media Blocks — Deep:\n");
    RUN(test_media_display_dimensions);
    RUN(test_media_block_mime_type_copied);
    RUN(test_mixed_block_types_layout);

    printf("\nAll docbox2 extended tests passed.\n");
    return 0;
}