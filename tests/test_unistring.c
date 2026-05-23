/**
 * @file test_unistring.c
 * @brief Smoke tests for unistring.h
 */
#define UNISTRING_IMPLEMENTATION
#include "unistring.h"
#include <stdio.h>
#include <assert.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-40s", #name); name(); printf("PASS\n"); } while(0)

/* ── UTF-8 codec ─────────────────────────────────────────────── */

TEST(test_utf8_roundtrip_ascii) {
    char buf[8];
    int n = unistr_utf8_encode('A', buf);
    assert(n == 1 && buf[0] == 'A');
    const char *p = buf;
    uint32_t cp = unistr_utf8_decode(&p);
    assert(cp == 'A');
}

TEST(test_utf8_roundtrip_2byte) {
    char buf[8];
    int n = unistr_utf8_encode(0x00E9, buf); /* é */
    assert(n == 2);
    const char *p = buf;
    uint32_t cp = unistr_utf8_decode(&p);
    assert(cp == 0x00E9);
}

TEST(test_utf8_roundtrip_3byte) {
    char buf[8];
    int n = unistr_utf8_encode(0x4E16, buf); /* 世 */
    assert(n == 3);
    const char *p = buf;
    uint32_t cp = unistr_utf8_decode(&p);
    assert(cp == 0x4E16);
}

TEST(test_utf8_roundtrip_4byte) {
    char buf[8];
    int n = unistr_utf8_encode(0x1F600, buf); /* 😀 */
    assert(n == 4);
    const char *p = buf;
    uint32_t cp = unistr_utf8_decode(&p);
    assert(cp == 0x1F600);
}

TEST(test_utf8_decode_bulk) {
    const char *src = "Hello";
    uint32_t out[16];
    int n = unistr_utf8_decode_bulk(src, 5, out);
    assert(n == 5);
    assert(out[0] == 'H' && out[4] == 'o');
}

TEST(test_utf8_byte_length) {
    uint32_t cps[] = {'A', 0xE9, 0x4E16, 0x1F600};
    int len = unistr_utf8_byte_length(cps, 4);
    assert(len == 1 + 2 + 3 + 4); /* 10 */
}

/* ── Lifecycle & basic access ────────────────────────────────── */

TEST(test_init_destroy) {
    unistr_t s;
    unistr_init(&s);
    assert(unistr_len(&s) == 0);
    assert(unistr_empty(&s));
    unistr_destroy(&s);
}

TEST(test_set_utf8) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "ABC", -1);
    assert(unistr_len(&s) == 3);
    assert(unistr_char_at(&s, 0) == 'A');
    assert(unistr_char_at(&s, 2) == 'C');
    assert(!unistr_empty(&s));
    unistr_destroy(&s);
}

TEST(test_set_codepoints) {
    unistr_t s;
    unistr_init(&s);
    uint32_t cps[] = {'X', 'Y', 'Z'};
    unistr_set_codepoints(&s, cps, 3);
    assert(unistr_len(&s) == 3);
    assert(unistr_char_at(&s, 1) == 'Y');
    unistr_destroy(&s);
}

/* ── Mutation ────────────────────────────────────────────────── */

TEST(test_insert) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "AC", -1);
    uint32_t b = 'B';
    unistr_insert(&s, 1, &b, 1);
    assert(unistr_len(&s) == 3);
    assert(unistr_char_at(&s, 0) == 'A');
    assert(unistr_char_at(&s, 1) == 'B');
    assert(unistr_char_at(&s, 2) == 'C');
    unistr_destroy(&s);
}

TEST(test_insert_utf8) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "Hello!", -1);
    unistr_insert_utf8(&s, 5, " World", -1);
    assert(unistr_len(&s) == 12);
    assert(unistr_char_at(&s, 5) == ' ');
    assert(unistr_char_at(&s, 6) == 'W');
    unistr_destroy(&s);
}

TEST(test_erase) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "ABCDE", -1);
    unistr_erase(&s, 1, 3); /* remove BCD */
    assert(unistr_len(&s) == 2);
    assert(unistr_char_at(&s, 0) == 'A');
    assert(unistr_char_at(&s, 1) == 'E');
    unistr_destroy(&s);
}

TEST(test_replace) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "ABCDE", -1);
    uint32_t repl[] = {'X', 'Y'};
    unistr_replace(&s, 1, 3, repl, 2); /* BCD -> XY */
    assert(unistr_len(&s) == 4);
    assert(unistr_char_at(&s, 0) == 'A');
    assert(unistr_char_at(&s, 1) == 'X');
    assert(unistr_char_at(&s, 2) == 'Y');
    assert(unistr_char_at(&s, 3) == 'E');
    unistr_destroy(&s);
}

TEST(test_clear) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "Hello", -1);
    unistr_clear(&s);
    assert(unistr_len(&s) == 0);
    assert(unistr_empty(&s));
    unistr_destroy(&s);
}

TEST(test_append) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "Hello", -1);
    unistr_append_utf8(&s, " World", -1);
    assert(unistr_len(&s) == 11);
    unistr_destroy(&s);
}

/* ── Export ───────────────────────────────────────────────────── */

TEST(test_to_utf8) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "Hello", -1);
    char buf[64];
    int n = unistr_to_utf8(&s, buf, sizeof(buf));
    assert(n == 5);
    assert(strcmp(buf, "Hello") == 0);
    unistr_destroy(&s);
}

TEST(test_range_to_utf8) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "Hello World", -1);
    char buf[64];
    int n = unistr_range_to_utf8(&s, 6, 5, buf, sizeof(buf));
    assert(n == 5);
    assert(strcmp(buf, "World") == 0);
    unistr_destroy(&s);
}

/* ── Grapheme (default: each char = 1 cluster) ───────────────── */

TEST(test_grapheme_default) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "ABC", -1);
    assert(unistr_grapheme_count(&s) == 3);
    assert(unistr_grapheme_is_boundary(&s, 0));
    assert(unistr_grapheme_is_boundary(&s, 1));
    assert(unistr_grapheme_next(&s, 0) == 1);
    assert(unistr_grapheme_prev(&s, 2) == 1);
    unistr_destroy(&s);
}

/* ── Custom grapheme service ─────────────────────────────────── */

/** Mock: treat pairs as single clusters (AA BB CC...) */
static void mock_grapheme(const uint32_t *cp, int count, uint8_t *out, void *u) {
    (void)cp; (void)u;
    for (int i = 0; i < count; i++)
        out[i] = (i % 2 == 0) ? 1 : 0;
}

TEST(test_grapheme_custom) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "AABBCC", -1);

    unistr_services_t svc = {0};
    svc.grapheme = mock_grapheme;
    unistr_set_services(&s, &svc);

    assert(unistr_grapheme_count(&s) == 3);
    assert(unistr_grapheme_is_boundary(&s, 0));
    assert(!unistr_grapheme_is_boundary(&s, 1));
    assert(unistr_grapheme_is_boundary(&s, 2));
    assert(unistr_grapheme_next(&s, 0) == 2);
    assert(unistr_grapheme_prev(&s, 3) == 2);

    unistr_destroy(&s);
}

/* ── Word boundaries ─────────────────────────────────────────── */

TEST(test_word_bounds) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "hello world", -1);
    int ws, we;
    unistr_word_bounds(&s, 2, &ws, &we);
    assert(ws == 0 && we == 5);
    unistr_word_bounds(&s, 7, &ws, &we);
    assert(ws == 6 && we == 11);
    unistr_destroy(&s);
}

TEST(test_word_next_prev) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "one two three", -1);
    int p = unistr_word_next(&s, 0);
    assert(p == 4); /* start of "two" */
    p = unistr_word_next(&s, 4);
    assert(p == 8); /* start of "three" */
    p = unistr_word_prev(&s, 8);
    assert(p == 4); /* start of "two" */
    p = unistr_word_prev(&s, 4);
    assert(p == 0); /* start of "one" */
    unistr_destroy(&s);
}

TEST(test_is_word_char) {
    assert(unistr_is_word_char('a'));
    assert(unistr_is_word_char('Z'));
    assert(unistr_is_word_char('5'));
    assert(unistr_is_word_char('_'));
    assert(unistr_is_word_char(0xE9));  /* é */
    assert(!unistr_is_word_char(' '));
    assert(!unistr_is_word_char('.'));
}

/* ── Line breaks ─────────────────────────────────────────────── */

TEST(test_line_break_default) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "hello world\nbye", -1);
    assert(unistr_line_break_at(&s, 5) == UNISTR_BREAK_SOFT);  /* space */
    assert(unistr_line_break_at(&s, 11) == UNISTR_BREAK_HARD); /* \n */
    assert(unistr_line_break_at(&s, 0) == UNISTR_BREAK_NONE);  /* h */

    int kind;
    int pos = unistr_line_break_next(&s, 0, &kind);
    assert(pos == 5 && kind == UNISTR_BREAK_SOFT);
    unistr_destroy(&s);
}

/* ── BiDi (default = all LTR) ────────────────────────────────── */

TEST(test_bidi_default_ltr) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "Hello", -1);
    assert(unistr_bidi_base_dir(&s) == 0);
    assert(unistr_bidi_run_count(&s) == 1);
    assert(unistr_bidi_level_at(&s, 0) == 0);
    assert(!unistr_bidi_is_rtl(&s, 0));

    int order[8];
    int n = unistr_bidi_visual_order(&s, 0, 5, order, 8);
    assert(n == 5);
    for (int i = 0; i < 5; i++) assert(order[i] == i);
    unistr_destroy(&s);
}

/* ── Cache invalidation ──────────────────────────────────────── */

TEST(test_cache_invalidation) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "AB", -1);

    /* Force grapheme analysis */
    assert(unistr_grapheme_count(&s) == 2);
    /* Mutate → cache should go stale */
    unistr_append_utf8(&s, "C", -1);
    assert(s.gc_gen != s.gen); /* stale */

    /* Re-query → should re-analyze */
    assert(unistr_grapheme_count(&s) == 3);
    assert(s.gc_gen == s.gen); /* fresh */

    unistr_destroy(&s);
}

/* ── Capacity ────────────────────────────────────────────────── */

TEST(test_reserve_shrink) {
    unistr_t s;
    unistr_init(&s);
    unistr_set_utf8(&s, "Hi", -1);
    unistr_reserve(&s, 1024);
    assert(s.cap >= 1024);
    unistr_shrink_to_fit(&s);
    assert(s.cap == 2);
    unistr_destroy(&s);
}

/* ── Larger string stress ────────────────────────────────────── */

TEST(test_large_insert) {
    unistr_t s;
    unistr_init(&s);
    for (int i = 0; i < 10000; i++)
        unistr_insert_char(&s, s.len, (uint32_t)('A' + (i % 26)));
    assert(unistr_len(&s) == 10000);
    assert(unistr_char_at(&s, 0) == 'A');
    assert(unistr_char_at(&s, 25) == 'Z');
    assert(unistr_grapheme_count(&s) == 10000);
    unistr_destroy(&s);
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("unistring.h tests:\n");

    /* UTF-8 codec */
    RUN(test_utf8_roundtrip_ascii);
    RUN(test_utf8_roundtrip_2byte);
    RUN(test_utf8_roundtrip_3byte);
    RUN(test_utf8_roundtrip_4byte);
    RUN(test_utf8_decode_bulk);
    RUN(test_utf8_byte_length);

    /* Lifecycle */
    RUN(test_init_destroy);
    RUN(test_set_utf8);
    RUN(test_set_codepoints);

    /* Mutation */
    RUN(test_insert);
    RUN(test_insert_utf8);
    RUN(test_erase);
    RUN(test_replace);
    RUN(test_clear);
    RUN(test_append);

    /* Export */
    RUN(test_to_utf8);
    RUN(test_range_to_utf8);

    /* Grapheme */
    RUN(test_grapheme_default);
    RUN(test_grapheme_custom);

    /* Word */
    RUN(test_word_bounds);
    RUN(test_word_next_prev);
    RUN(test_is_word_char);

    /* Line break */
    RUN(test_line_break_default);

    /* BiDi */
    RUN(test_bidi_default_ltr);

    /* Cache */
    RUN(test_cache_invalidation);

    /* Capacity */
    RUN(test_reserve_shrink);

    /* Stress */
    RUN(test_large_insert);

    printf("\nAll tests passed.\n");
    return 0;
}
