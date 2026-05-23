/**
 * @file test_docbox.c
 * @brief Tests for docbox.h — platform-independent multi-element document editor.
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
    return font_size * 0.6f; /* monospace-ish: 60% of font size */
}

/* ── Helpers ──────────────────────────────────────────────────── */

static void make_db(docbox_t *d, float w, float h) {
    docbox_create(d, w, h, 16.0f, 16.0f, 4.0f, dummy_measure, NULL);
}

/** Create a docbox and type some text into the initial block. */
static void make_db_with_text(docbox_t *d, float w, float h,
                              const char *text) {
    make_db(d, w, h);
    docbox_insert_utf8(d, text, -1);
}

/** Check that the text of block @p bi matches @p expected (UTF-8). */
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

/** Get the text length of a block. */
static int db_block_len(docbox_t *d, int bi) {
    textbox_t *tb = docbox_get_textbox(d, bi);
    if (!tb) return 0;
    return textbox_len(tb);
}

/** Place cursor at a specific doc position. */
static void db_set_cursor(docbox_t *d, int block, int offset) {
    docbox_set_cursor(d, (docbox_pos_t){block, offset}, 0);
}

/** Place cursor and extend selection from anchor to pos. */
static void db_select(docbox_t *d, int b1, int o1, int b2, int o2) {
    docbox_set_cursor(d, (docbox_pos_t){b1, o1}, 0);
    docbox_set_cursor(d, (docbox_pos_t){b2, o2}, 1);
}

/* ═══════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_create_destroy) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_create_defaults) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(docbox_block_count(&d) == 1);
    assert(docbox_block_type_at(&d, 0) == DOCBOX_BLOCK_TEXT);
    assert(docbox_has_selection(&d) == 0);
    assert(docbox_can_undo(&d) == 0);
    assert(docbox_can_redo(&d) == 0);
    assert(docbox_focused_block(&d) == 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0 && c.offset == 0);
    docbox_destroy(&d);
}

TEST(test_create_initial_block_empty) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_destroy_cleans_up) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Some text", -1);
    docbox_insert_text_block(&d, 1, DOCBOX_HEADING_H1);
    docbox_destroy(&d);
    /* No crash, no leak (valgrind) */
}

/* ═══════════════════════════════════════════════════════════════
 *  Block Management — Insert
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_insert_text_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    int idx = docbox_insert_text_block(&d, 1, DOCBOX_HEADING_BODY);
    assert(idx == 1);
    assert(docbox_block_count(&d) == 2);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_TEXT);
    docbox_destroy(&d);
}

TEST(test_insert_text_block_at_beginning) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    int idx = docbox_insert_text_block(&d, 0, DOCBOX_HEADING_H1);
    assert(idx == 0);
    assert(docbox_block_count(&d) == 2);
    assert(docbox_block_type_at(&d, 0) == DOCBOX_BLOCK_TEXT);
    /* Original block moved to index 1 */
    assert(db_block_streq(&d, 1, "Hello"));
    docbox_destroy(&d);
}

TEST(test_insert_text_block_heading) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, DOCBOX_HEADING_H3);
    assert(docbox_get_heading(&d, 1) == DOCBOX_HEADING_H3);
    docbox_destroy(&d);
}

TEST(test_insert_media_block_image) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.src_w = 640;
    media.src_h = 480;
    int idx = docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_IMAGE, &media);
    assert(idx == 1);
    assert(docbox_block_count(&d) == 2);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_IMAGE);
    const docbox_media_t *m = docbox_get_media(&d, 1);
    assert(m != NULL);
    assert(m->src_w == 640);
    assert(m->src_h == 480);
    docbox_destroy(&d);
}

TEST(test_insert_media_block_video) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.src_w = 1920;
    media.src_h = 1080;
    int idx = docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_VIDEO, &media);
    assert(idx == 1);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_VIDEO);
    docbox_destroy(&d);
}

TEST(test_insert_media_block_audio) {
    docbox_t d;
    make_db(&d, 400, 300);
    int idx = docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_AUDIO, NULL);
    assert(idx == 1);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_AUDIO);
    docbox_destroy(&d);
}

TEST(test_insert_media_block_file) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.filename = "test.pdf";
    media.filename_len = 8;
    media.file_size = 12345;
    media.mime_type = "application/pdf";
    int idx = docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_FILE, &media);
    assert(idx == 1);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_FILE);
    const docbox_media_t *m = docbox_get_media(&d, 1);
    assert(m != NULL);
    assert(m->file_size == 12345);
    assert(strcmp(m->filename, "test.pdf") == 0);
    assert(strcmp(m->mime_type, "application/pdf") == 0);
    docbox_destroy(&d);
}

TEST(test_insert_divider) {
    docbox_t d;
    make_db(&d, 400, 300);
    int idx = docbox_insert_divider(&d, 1);
    assert(idx == 1);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_DIVIDER);
    assert(docbox_block_count(&d) == 2);
    docbox_destroy(&d);
}

TEST(test_insert_media_block_text_type_rejected) {
    docbox_t d;
    make_db(&d, 400, 300);
    int idx = docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_TEXT, NULL);
    assert(idx == -1);
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_insert_block_clamps_negative_index) {
    docbox_t d;
    make_db(&d, 400, 300);
    int idx = docbox_insert_text_block(&d, -5, DOCBOX_HEADING_BODY);
    assert(idx == 0);
    assert(docbox_block_count(&d) == 2);
    docbox_destroy(&d);
}

TEST(test_insert_block_clamps_large_index) {
    docbox_t d;
    make_db(&d, 400, 300);
    int idx = docbox_insert_text_block(&d, 999, DOCBOX_HEADING_BODY);
    assert(idx == 1); /* clamped to block_count */
    assert(docbox_block_count(&d) == 2);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Block Management — Delete
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_delete_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, DOCBOX_HEADING_BODY);
    assert(docbox_block_count(&d) == 2);
    docbox_delete_block(&d, 1);
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_delete_last_text_block_clears) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    assert(docbox_block_count(&d) == 1);
    docbox_delete_block(&d, 0);
    /* Should keep one empty text block */
    assert(docbox_block_count(&d) == 1);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_delete_media_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    assert(docbox_block_count(&d) == 2);
    docbox_delete_block(&d, 1);
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_delete_block_adjusts_cursor) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, DOCBOX_HEADING_BODY);
    docbox_insert_text_block(&d, 2, DOCBOX_HEADING_BODY);
    db_set_cursor(&d, 2, 0);
    docbox_delete_block(&d, 1);
    /* Cursor was at block 2, after deleting block 1 it should be at block 1 */
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block <= 1);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Block Management — Move
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_move_block_forward) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, DOCBOX_HEADING_BODY);
    docbox_insert_text_block(&d, 2, DOCBOX_HEADING_BODY);
    /* Type in block 0 */
    db_set_cursor(&d, 0, 0);
    docbox_insert_utf8(&d, "AAA", -1);
    /* Type in block 1 */
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "BBB", -1);
    uint32_t id0 = docbox_block_id_at(&d, 0);
    docbox_move_block(&d, 0, 2);
    /* Block that was at 0 should now be at 2 */
    assert(docbox_block_id_at(&d, 2) == id0);
    assert(db_block_streq(&d, 2, "AAA"));
    docbox_destroy(&d);
}

TEST(test_move_block_backward) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, DOCBOX_HEADING_BODY);
    docbox_insert_text_block(&d, 2, DOCBOX_HEADING_BODY);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "CCC", -1);
    uint32_t id2 = docbox_block_id_at(&d, 2);
    docbox_move_block(&d, 2, 0);
    assert(docbox_block_id_at(&d, 0) == id2);
    assert(db_block_streq(&d, 0, "CCC"));
    docbox_destroy(&d);
}

TEST(test_move_block_same_index) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, DOCBOX_HEADING_BODY);
    uint32_t id0 = docbox_block_id_at(&d, 0);
    docbox_move_block(&d, 0, 0);
    assert(docbox_block_id_at(&d, 0) == id0); /* no change */
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Block Management — Accessors
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_block_count) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(docbox_block_count(&d) == 1);
    docbox_insert_text_block(&d, 1, 0);
    assert(docbox_block_count(&d) == 2);
    docbox_insert_divider(&d, 2);
    assert(docbox_block_count(&d) == 3);
    docbox_destroy(&d);
}

TEST(test_block_type_at) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_insert_media_block(&d, 2, DOCBOX_BLOCK_IMAGE, NULL);
    assert(docbox_block_type_at(&d, 0) == DOCBOX_BLOCK_TEXT);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_DIVIDER);
    assert(docbox_block_type_at(&d, 2) == DOCBOX_BLOCK_IMAGE);
    docbox_destroy(&d);
}

TEST(test_block_id_unique) {
    docbox_t d;
    make_db(&d, 400, 300);
    uint32_t id0 = docbox_block_id_at(&d, 0);
    docbox_insert_text_block(&d, 1, 0);
    uint32_t id1 = docbox_block_id_at(&d, 1);
    docbox_insert_divider(&d, 2);
    uint32_t id2 = docbox_block_id_at(&d, 2);
    assert(id0 != id1);
    assert(id1 != id2);
    assert(id0 != id2);
    docbox_destroy(&d);
}

TEST(test_get_textbox_for_text_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    textbox_t *tb = docbox_get_textbox(&d, 0);
    assert(tb != NULL);
    docbox_destroy(&d);
}

TEST(test_get_textbox_for_media_block_null) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    textbox_t *tb = docbox_get_textbox(&d, 1);
    assert(tb == NULL);
    docbox_destroy(&d);
}

TEST(test_get_media_for_text_block_null) {
    docbox_t d;
    make_db(&d, 400, 300);
    const docbox_media_t *m = docbox_get_media(&d, 0);
    assert(m == NULL);
    docbox_destroy(&d);
}

TEST(test_get_media_for_media_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    const docbox_media_t *m = docbox_get_media(&d, 1);
    assert(m != NULL);
    docbox_destroy(&d);
}

TEST(test_block_accessors_out_of_range) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(docbox_get_textbox(&d, -1) == NULL);
    assert(docbox_get_textbox(&d, 99) == NULL);
    assert(docbox_get_media(&d, -1) == NULL);
    assert(docbox_get_media(&d, 99) == NULL);
    assert(docbox_block_id_at(&d, -1) == 0);
    assert(docbox_block_id_at(&d, 99) == 0);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Heading
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_heading_default) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(docbox_get_heading(&d, 0) == DOCBOX_HEADING_BODY);
    docbox_destroy(&d);
}

TEST(test_set_heading) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_heading(&d, 0, DOCBOX_HEADING_H1);
    assert(docbox_get_heading(&d, 0) == DOCBOX_HEADING_H1);
    docbox_set_heading(&d, 0, DOCBOX_HEADING_H6);
    assert(docbox_get_heading(&d, 0) == DOCBOX_HEADING_H6);
    docbox_destroy(&d);
}

TEST(test_set_heading_clamps) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_heading(&d, 0, -1);
    assert(docbox_get_heading(&d, 0) == DOCBOX_HEADING_BODY);
    docbox_set_heading(&d, 0, 99);
    assert(docbox_get_heading(&d, 0) == DOCBOX_HEADING_H6);
    docbox_destroy(&d);
}

TEST(test_heading_font_size) {
    docbox_t d;
    make_db(&d, 400, 300);
    float body = docbox_heading_font_size(&d, DOCBOX_HEADING_BODY);
    float h1 = docbox_heading_font_size(&d, DOCBOX_HEADING_H1);
    assert(h1 > body);
    float h6 = docbox_heading_font_size(&d, DOCBOX_HEADING_H6);
    assert(h6 < body);
    docbox_destroy(&d);
}

TEST(test_heading_line_height) {
    docbox_t d;
    make_db(&d, 400, 300);
    float body = docbox_heading_line_height(&d, DOCBOX_HEADING_BODY);
    float h1 = docbox_heading_line_height(&d, DOCBOX_HEADING_H1);
    assert(h1 > body);
    docbox_destroy(&d);
}

TEST(test_set_heading_on_media_block_noop) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_set_heading(&d, 1, DOCBOX_HEADING_H2);
    assert(docbox_get_heading(&d, 1) == 0); /* unchanged */
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Document Cursor & Selection
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_get_set_cursor) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 3);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0 && c.offset == 3);
    docbox_destroy(&d);
}

TEST(test_set_cursor_clamps_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_cursor(&d, (docbox_pos_t){99, 0}, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0);
    docbox_destroy(&d);
}

TEST(test_set_cursor_clamps_negative_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_cursor(&d, (docbox_pos_t){-5, 0}, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0);
    docbox_destroy(&d);
}

TEST(test_set_cursor_clamps_offset) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hi");
    docbox_set_cursor(&d, (docbox_pos_t){0, 100}, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.offset == 2);
    docbox_destroy(&d);
}

TEST(test_set_cursor_extend_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 1);
    docbox_set_cursor(&d, (docbox_pos_t){0, 4}, 1);
    assert(docbox_has_selection(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_has_selection_false) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 2);
    assert(docbox_has_selection(&d) == 0);
    docbox_destroy(&d);
}

TEST(test_selection_range_same_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_select(&d, 0, 1, 0, 4);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.block == 0 && s.offset == 1);
    assert(e.block == 0 && e.offset == 4);
    docbox_destroy(&d);
}

TEST(test_selection_range_cross_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_select(&d, 0, 2, 1, 3);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.block == 0 && s.offset == 2);
    assert(e.block == 1 && e.offset == 3);
    docbox_destroy(&d);
}

TEST(test_selection_range_reversed) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_select(&d, 0, 4, 0, 1);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.offset == 1 && e.offset == 4);
    docbox_destroy(&d);
}

TEST(test_select_all) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    docbox_select_all(&d);
    assert(docbox_has_selection(&d) == 1);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.block == 0 && s.offset == 0);
    assert(e.block == 1 && e.offset == 5);
    docbox_destroy(&d);
}

TEST(test_select_all_single_empty) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_select_all(&d);
    assert(docbox_has_selection(&d) == 0);
    docbox_destroy(&d);
}

TEST(test_focused_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(docbox_focused_block(&d) == 0);
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    assert(docbox_focused_block(&d) == 1);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Delete Selection
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_delete_selection_single_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    db_select(&d, 0, 5, 0, 11);
    int r = docbox_delete_selection(&d);
    assert(r == 1);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_delete_selection_no_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 2);
    int r = docbox_delete_selection(&d);
    assert(r == 0);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_delete_selection_cross_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    /* Select from "Hel|lo" to "Wor|ld" */
    db_select(&d, 0, 3, 1, 3);
    int r = docbox_delete_selection(&d);
    assert(r == 1);
    /* Should merge remaining "Hel" + "ld" into one block */
    assert(docbox_block_count(&d) == 1);
    assert(db_block_streq(&d, 0, "Helld"));
    docbox_destroy(&d);
}

TEST(test_delete_selection_entire_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_select(&d, 0, 0, 0, 5);
    docbox_delete_selection(&d);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_delete_selection_media_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_insert_text_block(&d, 2, 0);
    /* Select the divider block fully (offset 0 to 1) */
    db_select(&d, 1, 0, 1, 1);
    docbox_delete_selection(&d);
    /* Divider should be gone */
    assert(docbox_block_count(&d) == 2);
    assert(docbox_block_type_at(&d, 0) == DOCBOX_BLOCK_TEXT);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_TEXT);
    docbox_destroy(&d);
}

TEST(test_delete_selection_multiple_blocks) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AAA");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "BBB", -1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "CCC", -1);
    docbox_insert_text_block(&d, 3, 0);
    db_set_cursor(&d, 3, 0);
    docbox_insert_utf8(&d, "DDD", -1);
    /* Select from block 0 offset 1 to block 3 offset 2: "A|AA BBB CCC DD|D" */
    db_select(&d, 0, 1, 3, 2);
    docbox_delete_selection(&d);
    assert(docbox_block_count(&d) == 1);
    assert(db_block_streq(&d, 0, "AD"));
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Editing — Insert
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_insert_codepoint) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_codepoint(&d, 'A');
    assert(db_block_len(&d, 0) == 1);
    assert(db_block_streq(&d, 0, "A"));
    docbox_destroy(&d);
}

TEST(test_insert_multiple_codepoints) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_codepoint(&d, 'H');
    docbox_insert_codepoint(&d, 'i');
    assert(db_block_streq(&d, 0, "Hi"));
    docbox_destroy(&d);
}

TEST(test_insert_utf8) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Hello", 5);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_insert_utf8_negative_len) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Hello", -1);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_insert_replaces_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    db_select(&d, 0, 5, 0, 11);
    docbox_insert_codepoint(&d, '!');
    assert(db_block_streq(&d, 0, "Hello!"));
    docbox_destroy(&d);
}

TEST(test_insert_on_media_block_noop) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    db_set_cursor(&d, 1, 0);
    docbox_insert_codepoint(&d, 'X');
    /* Should not crash; media block unchanged */
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_DIVIDER);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Editing — Delete Back / Forward
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_delete_back_within_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 5);
    docbox_delete_back(&d);
    assert(db_block_streq(&d, 0, "Hell"));
    docbox_destroy(&d);
}

TEST(test_delete_back_at_block_start_merges) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 1, 0);
    docbox_delete_back(&d);
    /* Should merge blocks */
    assert(docbox_block_count(&d) == 1);
    assert(db_block_streq(&d, 0, "HelloWorld"));
    docbox_destroy(&d);
}

TEST(test_delete_back_at_doc_start) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 0);
    docbox_delete_back(&d);
    assert(db_block_streq(&d, 0, "Hello")); /* no change */
    docbox_destroy(&d);
}

TEST(test_delete_back_on_media_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 1, 0);
    docbox_delete_back(&d);
    /* Divider block should be deleted */
    assert(docbox_block_count(&d) == 2);
    assert(docbox_block_type_at(&d, 0) == DOCBOX_BLOCK_TEXT);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_TEXT);
    docbox_destroy(&d);
}

TEST(test_delete_back_at_text_start_deletes_prev_media) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "After", -1);
    db_set_cursor(&d, 2, 0);
    docbox_delete_back(&d);
    /* Previous divider should be deleted */
    int found_divider = 0;
    for (int i = 0; i < docbox_block_count(&d); i++)
        if (docbox_block_type_at(&d, i) == DOCBOX_BLOCK_DIVIDER) found_divider = 1;
    assert(!found_divider);
    docbox_destroy(&d);
}

TEST(test_delete_forward_within_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 0);
    docbox_delete_forward(&d);
    assert(db_block_streq(&d, 0, "ello"));
    docbox_destroy(&d);
}

TEST(test_delete_forward_at_block_end_merges) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 0, 5);
    docbox_delete_forward(&d);
    assert(docbox_block_count(&d) == 1);
    assert(db_block_streq(&d, 0, "HelloWorld"));
    docbox_destroy(&d);
}

TEST(test_delete_forward_at_doc_end) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 5);
    docbox_delete_forward(&d);
    assert(db_block_streq(&d, 0, "Hello")); /* no change */
    docbox_destroy(&d);
}

TEST(test_delete_forward_deletes_next_media) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_divider(&d, 1);
    db_set_cursor(&d, 0, 5);
    docbox_delete_forward(&d);
    assert(docbox_block_count(&d) == 1);
    assert(docbox_block_type_at(&d, 0) == DOCBOX_BLOCK_TEXT);
    docbox_destroy(&d);
}

TEST(test_delete_with_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    db_select(&d, 0, 0, 0, 5);
    docbox_delete_back(&d);
    assert(db_block_streq(&d, 0, " World"));
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Editing — Split Block
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_split_block_middle) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "HelloWorld");
    db_set_cursor(&d, 0, 5);
    docbox_split_block(&d);
    assert(docbox_block_count(&d) == 2);
    assert(db_block_streq(&d, 0, "Hello"));
    assert(db_block_streq(&d, 1, "World"));
    docbox_destroy(&d);
}

TEST(test_split_block_at_start) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 0);
    docbox_split_block(&d);
    assert(docbox_block_count(&d) == 2);
    assert(db_block_len(&d, 0) == 0);
    assert(db_block_streq(&d, 1, "Hello"));
    docbox_destroy(&d);
}

TEST(test_split_block_at_end) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 5);
    docbox_split_block(&d);
    assert(docbox_block_count(&d) == 2);
    assert(db_block_streq(&d, 0, "Hello"));
    assert(db_block_len(&d, 1) == 0);
    docbox_destroy(&d);
}

TEST(test_split_block_cursor_moves_to_new) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "HelloWorld");
    db_set_cursor(&d, 0, 5);
    docbox_split_block(&d);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 1);
    assert(c.offset == 0);
    docbox_destroy(&d);
}

TEST(test_split_block_inherits_heading) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_heading(&d, 0, DOCBOX_HEADING_H2);
    docbox_insert_utf8(&d, "HeadingText", -1);
    db_set_cursor(&d, 0, 7);
    docbox_split_block(&d);
    assert(docbox_get_heading(&d, 1) == DOCBOX_HEADING_H2);
    docbox_destroy(&d);
}

TEST(test_split_empty_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_split_block(&d);
    assert(docbox_block_count(&d) == 2);
    assert(db_block_len(&d, 0) == 0);
    assert(db_block_len(&d, 1) == 0);
    docbox_destroy(&d);
}

TEST(test_split_deletes_selection_first) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    db_select(&d, 0, 5, 0, 11);
    docbox_split_block(&d);
    /* Selection " World" deleted, then split at pos 5 */
    assert(docbox_block_count(&d) == 2);
    assert(db_block_streq(&d, 0, "Hello"));
    assert(db_block_len(&d, 1) == 0);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Editing — Merge Blocks
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_merge_blocks) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    docbox_merge_blocks(&d, 0);
    assert(docbox_block_count(&d) == 1);
    assert(db_block_streq(&d, 0, "HelloWorld"));
    docbox_destroy(&d);
}

TEST(test_merge_blocks_cursor_at_seam) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AAA");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "BBB", -1);
    docbox_merge_blocks(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0);
    assert(c.offset == 3); /* at merge seam */
    docbox_destroy(&d);
}

TEST(test_merge_blocks_invalid_index) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_merge_blocks(&d, -1);
    docbox_merge_blocks(&d, 0); /* only 1 block, index+1 doesn't exist */
    assert(docbox_block_count(&d) == 1); /* no crash */
    docbox_destroy(&d);
}

TEST(test_merge_blocks_media_noop) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_merge_blocks(&d, 0); /* block 0 text + block 1 divider → noop */
    assert(docbox_block_count(&d) == 2);
    docbox_destroy(&d);
}

TEST(test_split_then_merge_roundtrip) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "HelloWorld");
    db_set_cursor(&d, 0, 5);
    docbox_split_block(&d);
    assert(docbox_block_count(&d) == 2);
    docbox_merge_blocks(&d, 0);
    assert(docbox_block_count(&d) == 1);
    assert(db_block_streq(&d, 0, "HelloWorld"));
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Style
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_set_current_style) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_current_style(&d, TEXTBOX_STYLE_BOLD);
    assert(docbox_get_current_style(&d) == TEXTBOX_STYLE_BOLD);
    docbox_destroy(&d);
}

TEST(test_toggle_style) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_toggle_style(&d, TEXTBOX_STYLE_ITALIC);
    assert(docbox_get_current_style(&d) == TEXTBOX_STYLE_ITALIC);
    docbox_toggle_style(&d, TEXTBOX_STYLE_ITALIC);
    assert(docbox_get_current_style(&d) == TEXTBOX_STYLE_NONE);
    docbox_destroy(&d);
}

TEST(test_insert_with_style) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_current_style(&d, TEXTBOX_STYLE_BOLD);
    docbox_insert_utf8(&d, "AB", -1);
    textbox_t *tb = docbox_get_textbox(&d, 0);
    assert(textbox_style_at(tb, 0) == TEXTBOX_STYLE_BOLD);
    assert(textbox_style_at(tb, 1) == TEXTBOX_STYLE_BOLD);
    docbox_destroy(&d);
}

TEST(test_apply_style_to_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_select(&d, 0, 1, 0, 4);
    docbox_apply_style_to_selection(&d, TEXTBOX_STYLE_UNDERLINE, TEXTBOX_STYLE_UNDERLINE);
    textbox_t *tb = docbox_get_textbox(&d, 0);
    assert(textbox_style_at(tb, 0) == TEXTBOX_STYLE_NONE);
    assert(textbox_style_at(tb, 1) == TEXTBOX_STYLE_UNDERLINE);
    assert(textbox_style_at(tb, 3) == TEXTBOX_STYLE_UNDERLINE);
    assert(textbox_style_at(tb, 4) == TEXTBOX_STYLE_NONE);
    docbox_destroy(&d);
}

TEST(test_toggle_selection_style) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABCDE");
    db_select(&d, 0, 0, 0, 5);
    docbox_toggle_selection_style(&d, TEXTBOX_STYLE_BOLD);
    textbox_t *tb = docbox_get_textbox(&d, 0);
    for (int i = 0; i < 5; i++)
        assert(textbox_style_at(tb, i) == TEXTBOX_STYLE_BOLD);
    /* Toggle off */
    db_select(&d, 0, 0, 0, 5);
    docbox_toggle_selection_style(&d, TEXTBOX_STYLE_BOLD);
    for (int i = 0; i < 5; i++)
        assert(textbox_style_at(tb, i) == TEXTBOX_STYLE_NONE);
    docbox_destroy(&d);
}

TEST(test_style_union_intersect) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_current_style(&d, TEXTBOX_STYLE_BOLD | TEXTBOX_STYLE_ITALIC);
    docbox_insert_codepoint(&d, 'A');
    docbox_set_current_style(&d, TEXTBOX_STYLE_BOLD);
    docbox_insert_codepoint(&d, 'B');
    db_select(&d, 0, 0, 0, 2);
    uint16_t u = docbox_get_selection_style_union(&d);
    assert(u & TEXTBOX_STYLE_BOLD);
    assert(u & TEXTBOX_STYLE_ITALIC);
    uint16_t inter = docbox_get_selection_style_intersect(&d);
    assert(inter & TEXTBOX_STYLE_BOLD);
    assert(!(inter & TEXTBOX_STYLE_ITALIC));
    docbox_destroy(&d);
}

TEST(test_style_cross_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "AAA");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "BBB", -1);
    /* Select across both blocks */
    db_select(&d, 0, 1, 1, 2);
    docbox_apply_style_to_selection(&d, TEXTBOX_STYLE_ITALIC, TEXTBOX_STYLE_ITALIC);
    textbox_t *tb0 = docbox_get_textbox(&d, 0);
    textbox_t *tb1 = docbox_get_textbox(&d, 1);
    assert(textbox_style_at(tb0, 0) == TEXTBOX_STYLE_NONE);
    assert(textbox_style_at(tb0, 1) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(tb0, 2) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(tb1, 0) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(tb1, 1) == TEXTBOX_STYLE_ITALIC);
    assert(textbox_style_at(tb1, 2) == TEXTBOX_STYLE_NONE);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Layout & Scroll
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_layout) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_layout(&d);
    /* No crash, content_height should be positive */
    assert(docbox_max_scroll_y(&d) >= 0.0f);
    docbox_destroy(&d);
}

TEST(test_layout_multi_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Line1");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "Line2", -1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "Line3", -1);
    docbox_layout(&d);
    docbox_destroy(&d);
}

TEST(test_layout_with_media) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.src_w = 200;
    media.src_h = 100;
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_IMAGE, &media);
    docbox_layout(&d);
    docbox_destroy(&d);
}

TEST(test_scroll_to) {
    docbox_t d;
    make_db_with_text(&d, 400, 50, "Hello");
    docbox_layout(&d);
    docbox_scroll_to(&d, 0.0f);
    docbox_scroll_to(&d, 100.0f);
    /* No crash */
    docbox_destroy(&d);
}

TEST(test_scroll_by) {
    docbox_t d;
    make_db_with_text(&d, 400, 50, "Hello");
    docbox_layout(&d);
    docbox_scroll_by(&d, 10.0f);
    docbox_scroll_by(&d, -5.0f);
    docbox_destroy(&d);
}

TEST(test_scroll_to_cursor) {
    docbox_t d;
    make_db(&d, 200, 50);
    /* Create many blocks to force scrollable content */
    for (int i = 0; i < 20; i++) {
        int idx = docbox_insert_text_block(&d, docbox_block_count(&d), 0);
        db_set_cursor(&d, idx, 0);
        docbox_insert_utf8(&d, "Line of text", -1);
    }
    db_set_cursor(&d, 19, 0);
    docbox_scroll_to_cursor(&d);
    docbox_destroy(&d);
}

TEST(test_max_scroll_y) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Short");
    float ms = docbox_max_scroll_y(&d);
    assert(ms >= 0.0f);
    docbox_destroy(&d);
}

TEST(test_scrollbar_v) {
    docbox_t d;
    make_db_with_text(&d, 400, 50, "Hello");
    docbox_layout(&d);
    docbox_scrollbar_t sb;
    docbox_get_scrollbar_v(&d, &sb);
    assert(sb.visible >= 0);
    docbox_destroy(&d);
}

TEST(test_set_scrollbar_v) {
    docbox_t d;
    make_db_with_text(&d, 400, 50, "Hello");
    docbox_layout(&d);
    docbox_set_scrollbar_v(&d, 0.5f);
    docbox_destroy(&d);
}

TEST(test_resize) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    docbox_layout(&d);
    docbox_resize(&d, 200, 100);
    docbox_layout(&d);
    /* No crash */
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Hit Testing & Queries
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_hit_test) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_layout(&d);
    docbox_pos_t p = docbox_hit_test(&d, 10.0f, 10.0f);
    assert(p.block >= 0 && p.block < docbox_block_count(&d));
    assert(p.offset >= 0);
    docbox_destroy(&d);
}

TEST(test_hit_test_media) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_media_t media = {0};
    media.src_w = 200;
    media.src_h = 100;
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_IMAGE, &media);
    docbox_layout(&d);
    /* Hit in the media area — offset should be 0 or 1 */
    float bx, by, bw, bh;
    docbox_block_rect(&d, 1, &bx, &by, &bw, &bh);
    docbox_pos_t p = docbox_hit_test(&d, bx + 10, by + 5);
    assert(p.block == 1);
    assert(p.offset == 0 || p.offset == 1);
    docbox_destroy(&d);
}

TEST(test_block_at_y) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    docbox_layout(&d);
    int bi = docbox_block_at_y(&d, 5.0f);
    assert(bi >= 0);
    docbox_destroy(&d);
}

TEST(test_block_rect) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_layout(&d);
    float x, y, w, h;
    docbox_block_rect(&d, 0, &x, &y, &w, &h);
    assert(w > 0.0f);
    assert(h > 0.0f);
    docbox_destroy(&d);
}

TEST(test_block_rect_out_of_range) {
    docbox_t d;
    make_db(&d, 400, 300);
    float x, y, w, h;
    docbox_block_rect(&d, -1, &x, &y, &w, &h);
    assert(w == 0.0f && h == 0.0f);
    docbox_block_rect(&d, 99, &x, &y, &w, &h);
    assert(w == 0.0f && h == 0.0f);
    docbox_destroy(&d);
}

TEST(test_cursor_rect) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 2);
    float x, y, w, h;
    docbox_cursor_rect(&d, 2.0f, &x, &y, &w, &h);
    assert(w > 0.0f);
    assert(h > 0.0f);
    docbox_destroy(&d);
}

TEST(test_cursor_rect_on_media) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    db_set_cursor(&d, 1, 0);
    float x, y, w, h;
    docbox_cursor_rect(&d, 2.0f, &x, &y, &w, &h);
    assert(h > 0.0f);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Movement
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_move_left) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 3);
    docbox_move_left(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.offset == 2);
    docbox_destroy(&d);
}

TEST(test_move_right) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 2);
    docbox_move_right(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.offset == 3);
    docbox_destroy(&d);
}

TEST(test_move_left_cross_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 1, 0);
    docbox_move_left(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0);
    assert(c.offset == 5); /* end of previous block */
    docbox_destroy(&d);
}

TEST(test_move_right_cross_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 0, 5);
    docbox_move_right(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 1);
    assert(c.offset == 0);
    docbox_destroy(&d);
}

TEST(test_move_left_at_doc_start) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 0);
    docbox_move_left(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0 && c.offset == 0);
    docbox_destroy(&d);
}

TEST(test_move_right_at_doc_end) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 5);
    docbox_move_right(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0 && c.offset == 5);
    docbox_destroy(&d);
}

TEST(test_move_left_extend) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 3);
    docbox_move_left(&d, 1);
    docbox_move_left(&d, 1);
    assert(docbox_has_selection(&d) == 1);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.offset == 1 && e.offset == 3);
    docbox_destroy(&d);
}

TEST(test_move_right_extend) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 1);
    docbox_move_right(&d, 1);
    docbox_move_right(&d, 1);
    assert(docbox_has_selection(&d) == 1);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.offset == 1 && e.offset == 3);
    docbox_destroy(&d);
}

TEST(test_move_up_within_block) {
    docbox_t d;
    /* Create a block with long text that wraps */
    make_db(&d, 100, 300);
    docbox_insert_utf8(&d, "This is a long line that will wrap to multiple visual lines", -1);
    docbox_layout(&d);
    db_set_cursor(&d, 0, db_block_len(&d, 0));
    docbox_move_up(&d, 0);
    /* Should still be in block 0 */
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0);
    docbox_destroy(&d);
}

TEST(test_move_up_cross_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 1, 2);
    docbox_move_up(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0);
    docbox_destroy(&d);
}

TEST(test_move_down_cross_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 0, 2);
    docbox_move_down(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 1);
    docbox_destroy(&d);
}

TEST(test_move_line_start) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 3);
    docbox_move_line_start(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.offset == 0);
    docbox_destroy(&d);
}

TEST(test_move_line_end) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 1);
    docbox_move_line_end(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.offset == 5);
    docbox_destroy(&d);
}

TEST(test_move_doc_start) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 1, 3);
    docbox_move_doc_start(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0 && c.offset == 0);
    docbox_destroy(&d);
}

TEST(test_move_doc_end) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 0, 0);
    docbox_move_doc_end(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 1 && c.offset == 5);
    docbox_destroy(&d);
}

TEST(test_move_doc_start_extend) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 3);
    docbox_move_doc_start(&d, 1);
    assert(docbox_has_selection(&d) == 1);
    docbox_pos_t s, e;
    docbox_selection_range(&d, &s, &e);
    assert(s.offset == 0 && e.offset == 3);
    docbox_destroy(&d);
}

TEST(test_move_page_up_down) {
    docbox_t d;
    make_db(&d, 200, 80);
    for (int i = 0; i < 10; i++) {
        int idx = docbox_insert_text_block(&d, docbox_block_count(&d), 0);
        db_set_cursor(&d, idx, 0);
        docbox_insert_utf8(&d, "Line", -1);
    }
    db_set_cursor(&d, 5, 0);
    docbox_move_page_up(&d, 0);
    docbox_move_page_down(&d, 0);
    /* No crash */
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Events — Key
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_char) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_event_result_t r = docbox_handle_char(&d, 'A');
    assert(db_block_streq(&d, 0, "A"));
    assert(r.redraw == 1);
    docbox_destroy(&d);
}

TEST(test_handle_char_multiple) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_char(&d, 'H');
    docbox_handle_char(&d, 'i');
    assert(db_block_streq(&d, 0, "Hi"));
    docbox_destroy(&d);
}

TEST(test_handle_char_control_ignored) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_char(&d, 1); /* SOH */
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_handle_char_tab_allowed) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_char(&d, '\t');
    assert(db_block_len(&d, 0) == 1);
    docbox_destroy(&d);
}

TEST(test_handle_key_backspace) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 5);
    docbox_handle_key(&d, TEXTBOX_KEY_BACKSPACE, 0);
    assert(db_block_streq(&d, 0, "Hell"));
    docbox_destroy(&d);
}

TEST(test_handle_key_delete) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 0);
    docbox_handle_key(&d, TEXTBOX_KEY_DELETE, 0);
    assert(db_block_streq(&d, 0, "ello"));
    docbox_destroy(&d);
}

TEST(test_handle_key_return_splits) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "HelloWorld");
    db_set_cursor(&d, 0, 5);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_RETURN, 0);
    assert(r.layout_changed == 1);
    assert(docbox_block_count(&d) == 2);
    assert(db_block_streq(&d, 0, "Hello"));
    assert(db_block_streq(&d, 1, "World"));
    docbox_destroy(&d);
}

TEST(test_handle_key_left_right) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 2);
    docbox_handle_key(&d, TEXTBOX_KEY_LEFT, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.offset == 1);
    docbox_handle_key(&d, TEXTBOX_KEY_RIGHT, 0);
    c = docbox_get_cursor(&d);
    assert(c.offset == 2);
    docbox_destroy(&d);
}

TEST(test_handle_key_up_down) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 1, 2);
    docbox_handle_key(&d, TEXTBOX_KEY_UP, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0);
    docbox_handle_key(&d, TEXTBOX_KEY_DOWN, 0);
    c = docbox_get_cursor(&d);
    assert(c.block == 1);
    docbox_destroy(&d);
}

TEST(test_handle_key_home_end) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 3);
    docbox_handle_key(&d, TEXTBOX_KEY_HOME, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.offset == 0);
    docbox_handle_key(&d, TEXTBOX_KEY_END, 0);
    c = docbox_get_cursor(&d);
    assert(c.offset == 5);
    docbox_destroy(&d);
}

TEST(test_handle_key_shift_select) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 2);
    docbox_handle_key(&d, TEXTBOX_KEY_RIGHT, TEXTBOX_MOD_SHIFT);
    docbox_handle_key(&d, TEXTBOX_KEY_RIGHT, TEXTBOX_MOD_SHIFT);
    assert(docbox_has_selection(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_a_select_all) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_A, TEXTBOX_MOD_SHORTCUT);
    assert(r.redraw == 1);
    assert(docbox_has_selection(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_c_copy) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_select_all(&d);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_C, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_copy == 1);
    assert(db_block_streq(&d, 0, "Hello")); /* text unchanged */
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_x_cut) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_select_all(&d);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_X, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_cut == 1);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_v_paste) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_event_result_t r = docbox_handle_key(&d, TEXTBOX_KEY_V, TEXTBOX_MOD_SHORTCUT);
    assert(r.request_paste == 1);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_z_undo) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_char(&d, 'A');
    docbox_undo_break(&d);
    assert(db_block_len(&d, 0) == 1);
    docbox_handle_key(&d, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_y_redo) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_char(&d, 'A');
    docbox_undo_break(&d);
    docbox_handle_key(&d, TEXTBOX_KEY_Z, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_len(&d, 0) == 0);
    docbox_handle_key(&d, TEXTBOX_KEY_Y, TEXTBOX_MOD_SHORTCUT);
    assert(db_block_len(&d, 0) == 1);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_b_bold) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_select(&d, 0, 0, 0, 5);
    docbox_handle_key(&d, TEXTBOX_KEY_B, TEXTBOX_MOD_SHORTCUT);
    textbox_t *tb = docbox_get_textbox(&d, 0);
    assert(textbox_style_at(tb, 0) & TEXTBOX_STYLE_BOLD);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_i_italic) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_select(&d, 0, 0, 0, 5);
    docbox_handle_key(&d, TEXTBOX_KEY_I, TEXTBOX_MOD_SHORTCUT);
    textbox_t *tb = docbox_get_textbox(&d, 0);
    assert(textbox_style_at(tb, 0) & TEXTBOX_STYLE_ITALIC);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_u_underline) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_select(&d, 0, 0, 0, 5);
    docbox_handle_key(&d, TEXTBOX_KEY_U, TEXTBOX_MOD_SHORTCUT);
    textbox_t *tb = docbox_get_textbox(&d, 0);
    assert(textbox_style_at(tb, 0) & TEXTBOX_STYLE_UNDERLINE);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_home_doc_start) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 1, 3);
    docbox_handle_key(&d, TEXTBOX_KEY_HOME, TEXTBOX_MOD_SHORTCUT);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0 && c.offset == 0);
    docbox_destroy(&d);
}

TEST(test_handle_key_ctrl_end_doc_end) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_set_cursor(&d, 0, 0);
    docbox_handle_key(&d, TEXTBOX_KEY_END, TEXTBOX_MOD_SHORTCUT);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 1 && c.offset == 5);
    docbox_destroy(&d);
}

TEST(test_handle_key_page_up_down) {
    docbox_t d;
    make_db(&d, 200, 80);
    for (int i = 0; i < 10; i++) {
        int idx = docbox_insert_text_block(&d, docbox_block_count(&d), 0);
        db_set_cursor(&d, idx, 0);
        docbox_insert_utf8(&d, "Line", -1);
    }
    docbox_handle_key(&d, TEXTBOX_KEY_PAGE_DOWN, 0);
    docbox_handle_key(&d, TEXTBOX_KEY_PAGE_UP, 0);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Events — Mouse
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_mouse_down) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    docbox_layout(&d);
    docbox_event_result_t r = docbox_handle_mouse_down(&d, 20.0f, 10.0f, 0.0f, 0);
    assert(r.redraw == 1);
    docbox_destroy(&d);
}

TEST(test_handle_mouse_drag_select) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    docbox_layout(&d);
    docbox_handle_mouse_down(&d, 10.0f, 10.0f, 0.0f, 0);
    docbox_handle_mouse_drag(&d, 60.0f, 10.0f);
    assert(docbox_has_selection(&d) == 1);
    docbox_handle_mouse_up(&d);
    docbox_destroy(&d);
}

TEST(test_handle_mouse_up) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_event_result_t r = docbox_handle_mouse_up(&d);
    (void)r;
    docbox_destroy(&d);
}

TEST(test_handle_mouse_shift_extend) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    docbox_layout(&d);
    docbox_handle_mouse_down(&d, 10.0f, 10.0f, 0.0f, 0);
    docbox_handle_mouse_up(&d);
    docbox_handle_mouse_down(&d, 60.0f, 10.0f, 0.5f, TEXTBOX_MOD_SHIFT);
    assert(docbox_has_selection(&d) == 1);
    docbox_handle_mouse_up(&d);
    docbox_destroy(&d);
}

TEST(test_handle_mouse_double_click) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    docbox_layout(&d);
    docbox_handle_mouse_down(&d, 20.0f, 10.0f, 0.0f, 0);
    docbox_handle_mouse_up(&d);
    docbox_handle_mouse_down(&d, 20.0f, 10.0f, 0.1f, 0);
    docbox_handle_mouse_up(&d);
    /* Should select a word */
    if (docbox_has_selection(&d)) {
        docbox_pos_t s, e;
        docbox_selection_range(&d, &s, &e);
        assert(e.offset > s.offset);
    }
    docbox_destroy(&d);
}

TEST(test_handle_mouse_triple_click) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    docbox_layout(&d);
    docbox_handle_mouse_down(&d, 20.0f, 10.0f, 0.0f, 0);
    docbox_handle_mouse_up(&d);
    docbox_handle_mouse_down(&d, 20.0f, 10.0f, 0.1f, 0);
    docbox_handle_mouse_up(&d);
    docbox_handle_mouse_down(&d, 20.0f, 10.0f, 0.2f, 0);
    docbox_handle_mouse_up(&d);
    /* Should select entire block */
    if (docbox_has_selection(&d)) {
        docbox_pos_t s, e;
        docbox_selection_range(&d, &s, &e);
        assert(s.offset == 0 && e.offset == 11);
    }
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Events — Scroll & Focus
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_handle_scroll) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_layout(&d);
    docbox_event_result_t r = docbox_handle_scroll(&d, 0.0f, 3.0f);
    assert(r.redraw == 1);
    docbox_destroy(&d);
}

TEST(test_handle_focus_gained) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_event_result_t r = docbox_handle_focus(&d, 1);
    assert(r.redraw == 1);
    docbox_destroy(&d);
}

TEST(test_handle_focus_lost) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_handle_focus(&d, 1);
    docbox_event_result_t r = docbox_handle_focus(&d, 0);
    assert(r.redraw == 1);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Block Iteration
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_get_block_view_text) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_block_view_t v;
    int ok = docbox_get_block_view(&d, 0, &v);
    assert(ok == 1);
    assert(v.index == 0);
    assert(v.type == DOCBOX_BLOCK_TEXT);
    assert(v.text_len == 5);
    assert(v.text_codepoints != NULL);
    assert(v.text_codepoints[0] == 'H');
    assert(v.media == NULL);
    docbox_destroy(&d);
}

TEST(test_get_block_view_media) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_block_view_t v;
    int ok = docbox_get_block_view(&d, 1, &v);
    assert(ok == 1);
    assert(v.type == DOCBOX_BLOCK_DIVIDER);
    assert(v.text_codepoints == NULL);
    assert(v.media != NULL);
    docbox_destroy(&d);
}

TEST(test_get_block_view_out_of_range) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_block_view_t v;
    assert(docbox_get_block_view(&d, -1, &v) == 0);
    assert(docbox_get_block_view(&d, 99, &v) == 0);
    docbox_destroy(&d);
}

static int count_cb(const docbox_block_view_t *v, void *user) {
    (void)v;
    (*(int *)user)++;
    return 0;
}

TEST(test_iterate_blocks) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, 0);
    docbox_insert_divider(&d, 2);
    int count = 0;
    int visited = docbox_iterate_blocks(&d, count_cb, &count);
    assert(visited == 3);
    assert(count == 3);
    docbox_destroy(&d);
}

static int stop_cb(const docbox_block_view_t *v, void *user) {
    (*(int *)user)++;
    return (*(int *)user) >= 2; /* stop after 2 */
}

TEST(test_iterate_blocks_early_stop) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, 0);
    docbox_insert_text_block(&d, 2, 0);
    docbox_insert_text_block(&d, 3, 0);
    int count = 0;
    int visited = docbox_iterate_blocks(&d, stop_cb, &count);
    assert(visited == 2);
    docbox_destroy(&d);
}

TEST(test_iterate_blocks_null_cb) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, 0);
    int visited = docbox_iterate_blocks(&d, NULL, NULL);
    assert(visited == 2);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Export
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_export_utf8_single_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    char buf[64];
    int n = docbox_export_utf8(&d, buf, sizeof(buf), NULL);
    assert(n == 5);
    assert(memcmp(buf, "Hello", 5) == 0);
    docbox_destroy(&d);
}

TEST(test_export_utf8_multi_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    char buf[64];
    int n = docbox_export_utf8(&d, buf, sizeof(buf), NULL);
    assert(n > 0);
    assert(memcmp(buf, "Hello\nWorld", 11) == 0);
    docbox_destroy(&d);
}

TEST(test_export_utf8_with_media) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_divider(&d, 1);
    char buf[128];
    int n = docbox_export_utf8(&d, buf, sizeof(buf), NULL);
    assert(n > 0);
    /* Should contain "[hr:]" for the divider */
    assert(strstr(buf, "[hr:") != NULL);
    docbox_destroy(&d);
}

TEST(test_export_utf8_custom_separator) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "A");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "B", -1);
    char buf[64];
    int n = docbox_export_utf8(&d, buf, sizeof(buf), " | ");
    assert(n > 0);
    assert(strstr(buf, "A | B") != NULL);
    docbox_destroy(&d);
}

TEST(test_export_utf8_null_dst) {
    docbox_t d;
    make_db(&d, 400, 300);
    int n = docbox_export_utf8(&d, NULL, 0, NULL);
    assert(n == 0);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Clipboard
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_get_selection_utf8) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    db_select(&d, 0, 0, 0, 5);
    char buf[64];
    int n = docbox_get_selection_utf8(&d, buf, sizeof(buf));
    assert(n == 5);
    assert(memcmp(buf, "Hello", 5) == 0);
    docbox_destroy(&d);
}

TEST(test_get_selection_utf8_cross_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    db_select(&d, 0, 3, 1, 2);
    char buf[64];
    int n = docbox_get_selection_utf8(&d, buf, sizeof(buf));
    assert(n > 0);
    /* Should be "lo\nWo" */
    assert(memcmp(buf, "lo\nWo", 5) == 0);
    docbox_destroy(&d);
}

TEST(test_get_selection_utf8_no_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    char buf[64];
    int n = docbox_get_selection_utf8(&d, buf, sizeof(buf));
    assert(n == 0);
    docbox_destroy(&d);
}

TEST(test_paste_utf8) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    db_set_cursor(&d, 0, 5);
    docbox_paste_utf8(&d, " World", 6);
    assert(db_block_streq(&d, 0, "Hello World"));
    docbox_destroy(&d);
}

TEST(test_paste_utf8_negative_len) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_paste_utf8(&d, "Test", -1);
    assert(db_block_streq(&d, 0, "Test"));
    docbox_destroy(&d);
}

TEST(test_paste_utf8_replaces_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    db_select(&d, 0, 5, 0, 11);
    docbox_paste_utf8(&d, "!", 1);
    assert(db_block_streq(&d, 0, "Hello!"));
    docbox_destroy(&d);
}

TEST(test_paste_utf8_null_noop) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_paste_utf8(&d, NULL, 0);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Read-Only
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_read_only_blocks_insert) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    docbox_insert_codepoint(&d, 'X');
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_read_only_blocks_delete) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    db_set_cursor(&d, 0, 5);
    docbox_delete_back(&d);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_read_only_blocks_split) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    db_set_cursor(&d, 0, 3);
    docbox_split_block(&d);
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_read_only_blocks_paste) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    docbox_paste_utf8(&d, " World", 6);
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_read_only_blocks_insert_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_read_only(&d, 1);
    int idx = docbox_insert_text_block(&d, 1, 0);
    assert(idx == -1);
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_read_only_blocks_delete_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_text_block(&d, 1, 0);
    docbox_set_read_only(&d, 1);
    docbox_delete_block(&d, 1);
    assert(docbox_block_count(&d) == 2); /* unchanged */
    docbox_destroy(&d);
}

TEST(test_read_only_allows_selection) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    docbox_select_all(&d);
    assert(docbox_has_selection(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_read_only_allows_movement) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    db_set_cursor(&d, 0, 0);
    docbox_move_right(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.offset == 1);
    docbox_destroy(&d);
}

TEST(test_read_only_toggle) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    docbox_insert_codepoint(&d, 'X');
    assert(db_block_len(&d, 0) == 5);
    docbox_set_read_only(&d, 0);
    db_set_cursor(&d, 0, 5);
    docbox_insert_codepoint(&d, 'X');
    assert(db_block_len(&d, 0) == 6);
    docbox_destroy(&d);
}

TEST(test_read_only_handle_char) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    docbox_handle_char(&d, 'X');
    assert(db_block_streq(&d, 0, "Hello"));
    docbox_destroy(&d);
}

TEST(test_read_only_handle_key_return) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_set_read_only(&d, 1);
    docbox_handle_key(&d, TEXTBOX_KEY_RETURN, 0);
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Undo / Redo
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_undo_insert_text) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Hello", -1);
    docbox_undo_break(&d);
    assert(db_block_len(&d, 0) == 5);
    assert(docbox_can_undo(&d) == 1);
    docbox_undo(&d);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_redo_insert_text) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_utf8(&d, "Hello", -1);
    docbox_undo_break(&d);
    docbox_undo(&d);
    assert(db_block_len(&d, 0) == 0);
    assert(docbox_can_redo(&d) == 1);
    docbox_redo(&d);
    assert(db_block_len(&d, 0) == 5);
    docbox_destroy(&d);
}

TEST(test_undo_on_empty) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(docbox_can_undo(&d) == 0);
    int r = docbox_undo(&d);
    assert(r == 0);
    docbox_destroy(&d);
}

TEST(test_redo_on_empty) {
    docbox_t d;
    make_db(&d, 400, 300);
    assert(docbox_can_redo(&d) == 0);
    int r = docbox_redo(&d);
    assert(r == 0);
    docbox_destroy(&d);
}

TEST(test_undo_break) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_codepoint(&d, 'A');
    docbox_undo_break(&d);
    docbox_insert_codepoint(&d, 'B');
    docbox_undo_break(&d);
    assert(db_block_len(&d, 0) == 2);
    docbox_undo(&d);
    assert(db_block_len(&d, 0) == 1);
    docbox_undo(&d);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_multiple_undo_redo) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_codepoint(&d, 'A');
    docbox_undo_break(&d);
    docbox_insert_codepoint(&d, 'B');
    docbox_undo_break(&d);
    docbox_insert_codepoint(&d, 'C');
    docbox_undo_break(&d);
    assert(db_block_streq(&d, 0, "ABC"));
    docbox_undo(&d);
    assert(db_block_streq(&d, 0, "AB"));
    docbox_undo(&d);
    assert(db_block_streq(&d, 0, "A"));
    docbox_undo(&d);
    assert(db_block_len(&d, 0) == 0);
    docbox_redo(&d);
    assert(db_block_streq(&d, 0, "A"));
    docbox_redo(&d);
    assert(db_block_streq(&d, 0, "AB"));
    docbox_redo(&d);
    assert(db_block_streq(&d, 0, "ABC"));
    docbox_destroy(&d);
}

TEST(test_new_edit_clears_redo) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_codepoint(&d, 'A');
    docbox_undo_break(&d);
    docbox_undo(&d);
    assert(docbox_can_redo(&d) == 1);
    docbox_insert_codepoint(&d, 'B');
    assert(docbox_can_redo(&d) == 0);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Edge Cases
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_empty_doc_operations) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_layout(&d);
    float x, y, w, h;
    docbox_cursor_rect(&d, 2.0f, &x, &y, &w, &h);
    docbox_delete_back(&d);
    docbox_delete_forward(&d);
    assert(docbox_block_count(&d) == 1);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_event_result_none) {
    docbox_event_result_t r = DOCBOX_EVENT_NONE;
    assert(r.redraw == 0);
    assert(r.cursor_moved == 0);
    assert(r.layout_changed == 0);
    assert(r.request_copy == 0);
    assert(r.request_cut == 0);
    assert(r.request_paste == 0);
}

TEST(test_media_block_filename_copied) {
    docbox_t d;
    make_db(&d, 400, 300);
    char name[] = "photo.jpg";
    docbox_media_t media = {0};
    media.filename = name;
    media.filename_len = 9;
    docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_IMAGE, &media);
    /* Mutate original — docbox should have its own copy */
    name[0] = 'X';
    const docbox_media_t *m = docbox_get_media(&d, 1);
    assert(m->filename[0] == 'p');
    docbox_destroy(&d);
}

TEST(test_media_block_null_media) {
    docbox_t d;
    make_db(&d, 400, 300);
    int idx = docbox_insert_media_block(&d, 1, DOCBOX_BLOCK_IMAGE, NULL);
    assert(idx == 1);
    const docbox_media_t *m = docbox_get_media(&d, 1);
    assert(m != NULL);
    assert(m->filename == NULL);
    assert(m->mime_type == NULL);
    docbox_destroy(&d);
}

TEST(test_split_block_on_media_noop) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    db_set_cursor(&d, 1, 0);
    docbox_split_block(&d);
    assert(docbox_block_count(&d) == 2); /* no split */
    docbox_destroy(&d);
}

TEST(test_multiple_blocks_mixed_types) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Text1");
    docbox_insert_divider(&d, 1);
    docbox_insert_media_block(&d, 2, DOCBOX_BLOCK_IMAGE, NULL);
    docbox_insert_text_block(&d, 3, DOCBOX_HEADING_H1);
    db_set_cursor(&d, 3, 0);
    docbox_insert_utf8(&d, "Heading", -1);
    docbox_insert_media_block(&d, 4, DOCBOX_BLOCK_FILE, NULL);
    assert(docbox_block_count(&d) == 5);
    assert(docbox_block_type_at(&d, 0) == DOCBOX_BLOCK_TEXT);
    assert(docbox_block_type_at(&d, 1) == DOCBOX_BLOCK_DIVIDER);
    assert(docbox_block_type_at(&d, 2) == DOCBOX_BLOCK_IMAGE);
    assert(docbox_block_type_at(&d, 3) == DOCBOX_BLOCK_TEXT);
    assert(docbox_block_type_at(&d, 4) == DOCBOX_BLOCK_FILE);
    docbox_destroy(&d);
}

TEST(test_heading_scale_affects_textbox) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_set_heading(&d, 0, DOCBOX_HEADING_H1);
    textbox_t *tb = docbox_get_textbox(&d, 0);
    assert(tb != NULL);
    /* H1 scale is 2.0, base is 16.0 → font_size should be 32 */
    assert(NEAR(tb->font_size, 32.0f));
    docbox_destroy(&d);
}

TEST(test_cursor_on_media_block) {
    docbox_t d;
    make_db(&d, 400, 300);
    docbox_insert_divider(&d, 1);
    docbox_insert_text_block(&d, 2, 0);
    /* Place cursor before media (offset 0) */
    db_set_cursor(&d, 1, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 1 && c.offset == 0);
    /* Place cursor after media (offset 1) */
    db_set_cursor(&d, 1, 1);
    c = docbox_get_cursor(&d);
    assert(c.block == 1 && c.offset == 1);
    docbox_destroy(&d);
}

TEST(test_move_across_media_block) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Before");
    docbox_insert_divider(&d, 1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "After", -1);
    /* Move left from start of "After" → should land on/past divider → "Before" end */
    db_set_cursor(&d, 2, 0);
    docbox_move_left(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    /* Should be at divider (offset 1) or end of "Before" depending on implementation */
    assert(c.block <= 1);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Stress Tests
 * ═══════════════════════════════════════════════════════════════ */

TEST(test_stress_insert_delete_codepoints) {
    docbox_t d;
    make_db(&d, 400, 300);
    for (int i = 0; i < 500; i++)
        docbox_insert_codepoint(&d, 'A' + (uint32_t)(i % 26));
    assert(db_block_len(&d, 0) == 500);
    for (int i = 0; i < 500; i++)
        docbox_delete_back(&d);
    assert(db_block_len(&d, 0) == 0);
    docbox_destroy(&d);
}

TEST(test_stress_many_blocks) {
    docbox_t d;
    make_db(&d, 400, 300);
    for (int i = 0; i < 100; i++) {
        docbox_insert_text_block(&d, docbox_block_count(&d), 0);
    }
    assert(docbox_block_count(&d) == 101);
    docbox_layout(&d);
    /* Delete them all from the end */
    while (docbox_block_count(&d) > 1)
        docbox_delete_block(&d, docbox_block_count(&d) - 1);
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

TEST(test_stress_split_merge_cycle) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "ABCDEFGHIJ");
    for (int i = 0; i < 10; i++) {
        db_set_cursor(&d, 0, 5);
        docbox_split_block(&d);
        assert(docbox_block_count(&d) == 2);
        docbox_merge_blocks(&d, 0);
        assert(docbox_block_count(&d) == 1);
        assert(db_block_streq(&d, 0, "ABCDEFGHIJ"));
    }
    docbox_destroy(&d);
}

TEST(test_stress_rapid_layout) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    for (int i = 0; i < 100; i++)
        docbox_layout(&d);
    docbox_destroy(&d);
}

TEST(test_stress_cursor_movement) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello");
    docbox_insert_text_block(&d, 1, 0);
    db_set_cursor(&d, 1, 0);
    docbox_insert_utf8(&d, "World", -1);
    docbox_insert_text_block(&d, 2, 0);
    db_set_cursor(&d, 2, 0);
    docbox_insert_utf8(&d, "Test", -1);
    db_set_cursor(&d, 0, 0);
    for (int i = 0; i < 50; i++)
        docbox_move_right(&d, 0);
    for (int i = 0; i < 50; i++)
        docbox_move_left(&d, 0);
    docbox_pos_t c = docbox_get_cursor(&d);
    assert(c.block == 0 && c.offset == 0);
    docbox_destroy(&d);
}

TEST(test_stress_undo_redo_cycle) {
    docbox_t d;
    make_db(&d, 400, 300);
    for (int i = 0; i < 30; i++) {
        docbox_insert_codepoint(&d, 'A' + (uint32_t)(i % 26));
        docbox_undo_break(&d);
    }
    for (int i = 0; i < 30; i++)
        docbox_undo(&d);
    assert(db_block_len(&d, 0) == 0);
    for (int i = 0; i < 30; i++)
        docbox_redo(&d);
    assert(db_block_len(&d, 0) == 30);
    docbox_destroy(&d);
}

TEST(test_stress_selection_paste) {
    docbox_t d;
    make_db_with_text(&d, 400, 300, "Hello World");
    for (int i = 0; i < 20; i++) {
        docbox_select_all(&d);
        docbox_paste_utf8(&d, "Replaced", -1);
    }
    assert(db_block_streq(&d, 0, "Replaced"));
    docbox_destroy(&d);
}

TEST(test_stress_mixed_operations) {
    docbox_t d;
    make_db(&d, 400, 300);
    /* Type, split, type, merge, repeat */
    for (int i = 0; i < 10; i++) {
        db_set_cursor(&d, 0, db_block_len(&d, 0));
        docbox_insert_utf8(&d, "Hello", -1);
        int len = db_block_len(&d, 0);
        db_set_cursor(&d, 0, len / 2);
        docbox_split_block(&d);
        docbox_merge_blocks(&d, 0);
    }
    assert(docbox_block_count(&d) == 1);
    docbox_destroy(&d);
}

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_docbox\n");

    printf(" Lifecycle:\n");
    RUN(test_create_destroy);
    RUN(test_create_defaults);
    RUN(test_create_initial_block_empty);
    RUN(test_destroy_cleans_up);

    printf(" Block Management — Insert:\n");
    RUN(test_insert_text_block);
    RUN(test_insert_text_block_at_beginning);
    RUN(test_insert_text_block_heading);
    RUN(test_insert_media_block_image);
    RUN(test_insert_media_block_video);
    RUN(test_insert_media_block_audio);
    RUN(test_insert_media_block_file);
    RUN(test_insert_divider);
    RUN(test_insert_media_block_text_type_rejected);
    RUN(test_insert_block_clamps_negative_index);
    RUN(test_insert_block_clamps_large_index);

    printf(" Block Management — Delete:\n");
    RUN(test_delete_block);
    RUN(test_delete_last_text_block_clears);
    RUN(test_delete_media_block);
    RUN(test_delete_block_adjusts_cursor);

    printf(" Block Management — Move:\n");
    RUN(test_move_block_forward);
    RUN(test_move_block_backward);
    RUN(test_move_block_same_index);

    printf(" Block Management — Accessors:\n");
    RUN(test_block_count);
    RUN(test_block_type_at);
    RUN(test_block_id_unique);
    RUN(test_get_textbox_for_text_block);
    RUN(test_get_textbox_for_media_block_null);
    RUN(test_get_media_for_text_block_null);
    RUN(test_get_media_for_media_block);
    RUN(test_block_accessors_out_of_range);

    printf(" Heading:\n");
    RUN(test_heading_default);
    RUN(test_set_heading);
    RUN(test_set_heading_clamps);
    RUN(test_heading_font_size);
    RUN(test_heading_line_height);
    RUN(test_set_heading_on_media_block_noop);

    printf(" Document Cursor & Selection:\n");
    RUN(test_get_set_cursor);
    RUN(test_set_cursor_clamps_block);
    RUN(test_set_cursor_clamps_negative_block);
    RUN(test_set_cursor_clamps_offset);
    RUN(test_set_cursor_extend_selection);
    RUN(test_has_selection_false);
    RUN(test_selection_range_same_block);
    RUN(test_selection_range_cross_block);
    RUN(test_selection_range_reversed);
    RUN(test_select_all);
    RUN(test_select_all_single_empty);
    RUN(test_focused_block);

    printf(" Delete Selection:\n");
    RUN(test_delete_selection_single_block);
    RUN(test_delete_selection_no_selection);
    RUN(test_delete_selection_cross_block);
    RUN(test_delete_selection_entire_block);
    RUN(test_delete_selection_media_block);
    RUN(test_delete_selection_multiple_blocks);

    printf(" Editing — Insert:\n");
    RUN(test_insert_codepoint);
    RUN(test_insert_multiple_codepoints);
    RUN(test_insert_utf8);
    RUN(test_insert_utf8_negative_len);
    RUN(test_insert_replaces_selection);
    RUN(test_insert_on_media_block_noop);

    printf(" Editing — Delete Back/Forward:\n");
    RUN(test_delete_back_within_block);
    RUN(test_delete_back_at_block_start_merges);
    RUN(test_delete_back_at_doc_start);
    RUN(test_delete_back_on_media_block);
    RUN(test_delete_back_at_text_start_deletes_prev_media);
    RUN(test_delete_forward_within_block);
    RUN(test_delete_forward_at_block_end_merges);
    RUN(test_delete_forward_at_doc_end);
    RUN(test_delete_forward_deletes_next_media);
    RUN(test_delete_with_selection);

    printf(" Editing — Split Block:\n");
    RUN(test_split_block_middle);
    RUN(test_split_block_at_start);
    RUN(test_split_block_at_end);
    RUN(test_split_block_cursor_moves_to_new);
    RUN(test_split_block_inherits_heading);
    RUN(test_split_empty_block);
    RUN(test_split_deletes_selection_first);

    printf(" Editing — Merge Blocks:\n");
    RUN(test_merge_blocks);
    RUN(test_merge_blocks_cursor_at_seam);
    RUN(test_merge_blocks_invalid_index);
    RUN(test_merge_blocks_media_noop);
    RUN(test_split_then_merge_roundtrip);

    printf(" Style:\n");
    RUN(test_set_current_style);
    RUN(test_toggle_style);
    RUN(test_insert_with_style);
    RUN(test_apply_style_to_selection);
    RUN(test_toggle_selection_style);
    RUN(test_style_union_intersect);
    RUN(test_style_cross_block);

    printf(" Layout & Scroll:\n");
    RUN(test_layout);
    RUN(test_layout_multi_block);
    RUN(test_layout_with_media);
    RUN(test_scroll_to);
    RUN(test_scroll_by);
    RUN(test_scroll_to_cursor);
    RUN(test_max_scroll_y);
    RUN(test_scrollbar_v);
    RUN(test_set_scrollbar_v);
    RUN(test_resize);

    printf(" Hit Testing & Queries:\n");
    RUN(test_hit_test);
    RUN(test_hit_test_media);
    RUN(test_block_at_y);
    RUN(test_block_rect);
    RUN(test_block_rect_out_of_range);
    RUN(test_cursor_rect);
    RUN(test_cursor_rect_on_media);

    printf(" Movement:\n");
    RUN(test_move_left);
    RUN(test_move_right);
    RUN(test_move_left_cross_block);
    RUN(test_move_right_cross_block);
    RUN(test_move_left_at_doc_start);
    RUN(test_move_right_at_doc_end);
    RUN(test_move_left_extend);
    RUN(test_move_right_extend);
    RUN(test_move_up_within_block);
    RUN(test_move_up_cross_block);
    RUN(test_move_down_cross_block);
    RUN(test_move_line_start);
    RUN(test_move_line_end);
    RUN(test_move_doc_start);
    RUN(test_move_doc_end);
    RUN(test_move_doc_start_extend);
    RUN(test_move_page_up_down);

    printf(" Events — Key:\n");
    RUN(test_handle_char);
    RUN(test_handle_char_multiple);
    RUN(test_handle_char_control_ignored);
    RUN(test_handle_char_tab_allowed);
    RUN(test_handle_key_backspace);
    RUN(test_handle_key_delete);
    RUN(test_handle_key_return_splits);
    RUN(test_handle_key_left_right);
    RUN(test_handle_key_up_down);
    RUN(test_handle_key_home_end);
    RUN(test_handle_key_shift_select);
    RUN(test_handle_key_ctrl_a_select_all);
    RUN(test_handle_key_ctrl_c_copy);
    RUN(test_handle_key_ctrl_x_cut);
    RUN(test_handle_key_ctrl_v_paste);
    RUN(test_handle_key_ctrl_z_undo);
    RUN(test_handle_key_ctrl_y_redo);
    RUN(test_handle_key_ctrl_b_bold);
    RUN(test_handle_key_ctrl_i_italic);
    RUN(test_handle_key_ctrl_u_underline);
    RUN(test_handle_key_ctrl_home_doc_start);
    RUN(test_handle_key_ctrl_end_doc_end);
    RUN(test_handle_key_page_up_down);

    printf(" Events — Mouse:\n");
    RUN(test_handle_mouse_down);
    RUN(test_handle_mouse_drag_select);
    RUN(test_handle_mouse_up);
    RUN(test_handle_mouse_shift_extend);
    RUN(test_handle_mouse_double_click);
    RUN(test_handle_mouse_triple_click);

    printf(" Events — Scroll & Focus:\n");
    RUN(test_handle_scroll);
    RUN(test_handle_focus_gained);
    RUN(test_handle_focus_lost);

    printf(" Block Iteration:\n");
    RUN(test_get_block_view_text);
    RUN(test_get_block_view_media);
    RUN(test_get_block_view_out_of_range);
    RUN(test_iterate_blocks);
    RUN(test_iterate_blocks_early_stop);
    RUN(test_iterate_blocks_null_cb);

    printf(" Export:\n");
    RUN(test_export_utf8_single_block);
    RUN(test_export_utf8_multi_block);
    RUN(test_export_utf8_with_media);
    RUN(test_export_utf8_custom_separator);
    RUN(test_export_utf8_null_dst);

    printf(" Clipboard:\n");
    RUN(test_get_selection_utf8);
    RUN(test_get_selection_utf8_cross_block);
    RUN(test_get_selection_utf8_no_selection);
    RUN(test_paste_utf8);
    RUN(test_paste_utf8_negative_len);
    RUN(test_paste_utf8_replaces_selection);
    RUN(test_paste_utf8_null_noop);

    printf(" Read-Only:\n");
    RUN(test_read_only_blocks_insert);
    RUN(test_read_only_blocks_delete);
    RUN(test_read_only_blocks_split);
    RUN(test_read_only_blocks_paste);
    RUN(test_read_only_blocks_insert_block);
    RUN(test_read_only_blocks_delete_block);
    RUN(test_read_only_allows_selection);
    RUN(test_read_only_allows_movement);
    RUN(test_read_only_toggle);
    RUN(test_read_only_handle_char);
    RUN(test_read_only_handle_key_return);

    printf(" Undo / Redo:\n");
    RUN(test_undo_insert_text);
    RUN(test_redo_insert_text);
    RUN(test_undo_on_empty);
    RUN(test_redo_on_empty);
    RUN(test_undo_break);
    RUN(test_multiple_undo_redo);
    RUN(test_new_edit_clears_redo);

    printf(" Edge Cases:\n");
    RUN(test_empty_doc_operations);
    RUN(test_event_result_none);
    RUN(test_media_block_filename_copied);
    RUN(test_media_block_null_media);
    RUN(test_split_block_on_media_noop);
    RUN(test_multiple_blocks_mixed_types);
    RUN(test_heading_scale_affects_textbox);
    RUN(test_cursor_on_media_block);
    RUN(test_move_across_media_block);

    printf(" Stress Tests:\n");
    RUN(test_stress_insert_delete_codepoints);
    RUN(test_stress_many_blocks);
    RUN(test_stress_split_merge_cycle);
    RUN(test_stress_rapid_layout);
    RUN(test_stress_cursor_movement);
    RUN(test_stress_undo_redo_cycle);
    RUN(test_stress_selection_paste);
    RUN(test_stress_mixed_operations);

    printf("\nAll docbox tests passed.\n");
    return 0;
}