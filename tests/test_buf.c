/**
 * @file test_buf.c
 * @brief Test suite for buf.h / buf.c — serialization buffer primitives.
 *
 * Tests are structured in sections matching the buf.h API:
 *   1. Writer lifecycle (init, free, growth)
 *   2. Integer round-trips (u8, u16, u32, i64)
 *   3. Raw byte writes and reads
 *   4. String serialization round-trips
 *   5. Reader bounds checking
 *   6. Endian correctness (wire format verification)
 *   7. Edge cases and adversarial inputs
 *
 * All tests use a simple pass/fail macro that prints the failing
 * line and increments a failure counter. The process exits with
 * 0 on all-pass or 1 on any failure (compatible with meson test).
 */

#include "buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <float.h>

/* ── Test harness ──────────────────────────────────────────────── */

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                        \
    do {                                                              \
        g_tests_run++;                                                \
        if (!(cond)) {                                                \
            g_tests_failed++;                                         \
            fprintf(stderr, "  FAIL [%s:%d] %s\n",                   \
                    __FILE__, __LINE__, msg);                         \
        }                                                             \
    } while (0)

#define TEST_SECTION(name)                                            \
    fprintf(stdout, "── %s\n", name)

/* ═══════════════════════════════════════════════════════════════════
 *  1. Writer lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

static void test_buf_init_and_free(void) {
    TEST_SECTION("buf init and free");

    gr_buf_t buf;
    int rc = gr_buf_init(&buf, 256);
    TEST_ASSERT(rc == 0, "gr_buf_init should return 0");
    TEST_ASSERT(buf.data != NULL, "data should be non-NULL after init");
    TEST_ASSERT(buf.len == 0, "len should be 0 after init");
    TEST_ASSERT(buf.cap == 256, "cap should match requested capacity");

    gr_buf_free(&buf);
    TEST_ASSERT(buf.data == NULL, "data should be NULL after free");
    TEST_ASSERT(buf.len == 0, "len should be 0 after free");
    TEST_ASSERT(buf.cap == 0, "cap should be 0 after free");
}

static void test_buf_double_free(void) {
    TEST_SECTION("buf double free safety");

    gr_buf_t buf;
    gr_buf_init(&buf, 64);
    gr_buf_free(&buf);
    /* Second free on zeroed buffer should not crash */
    gr_buf_free(&buf);
    TEST_ASSERT(buf.data == NULL, "data still NULL after double free");
}

static void test_buf_growth(void) {
    TEST_SECTION("buf automatic growth");

    gr_buf_t buf;
    gr_buf_init(&buf, 4); /* tiny initial cap */

    /* Write 256 bytes — forces multiple doublings: 4 → 8 → 16 → ... → 256 */
    uint8_t byte = 0xAB;
    for (int i = 0; i < 256; i++) {
        int rc = gr_buf_write_u8(&buf, byte);
        TEST_ASSERT(rc == 0, "write_u8 should succeed during growth");
    }

    TEST_ASSERT(buf.len == 256, "len should be 256 after 256 writes");
    TEST_ASSERT(buf.cap >= 256, "cap should be at least 256");

    /* Verify all bytes are correct */
    int all_correct = 1;
    for (int i = 0; i < 256; i++) {
        if (buf.data[i] != 0xAB) { all_correct = 0; break; }
    }
    TEST_ASSERT(all_correct, "all 256 bytes should be 0xAB");

    gr_buf_free(&buf);
}

static void test_buf_large_single_write(void) {
    TEST_SECTION("buf large single write exceeding capacity");

    gr_buf_t buf;
    gr_buf_init(&buf, 8);

    /* Write 1024 bytes in one call — capacity must jump past 8 */
    uint8_t block[1024];
    memset(block, 0x55, sizeof(block));
    int rc = gr_buf_write(&buf, block, sizeof(block));
    TEST_ASSERT(rc == 0, "large single write should succeed");
    TEST_ASSERT(buf.len == 1024, "len should be 1024");
    TEST_ASSERT(buf.cap >= 1024, "cap should accommodate the write");
    TEST_ASSERT(memcmp(buf.data, block, 1024) == 0,
                "data should match the source block");

    gr_buf_free(&buf);
}

/* ═══════════════════════════════════════════════════════════════════
 *  2. Integer round-trips
 * ═══════════════════════════════════════════════════════════════════ */

static void test_u8_roundtrip(void) {
    TEST_SECTION("u8 round-trip");

    gr_buf_t buf;
    gr_buf_init(&buf, 64);

    uint8_t values[] = { 0, 1, 127, 128, 255 };
    int n = sizeof(values) / sizeof(values[0]);

    for (int i = 0; i < n; i++) {
        gr_buf_write_u8(&buf, values[i]);
    }

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    for (int i = 0; i < n; i++) {
        uint8_t out;
        int rc = gr_read_u8(&r, &out);
        TEST_ASSERT(rc == 0, "read_u8 should succeed");
        TEST_ASSERT(out == values[i], "u8 round-trip value mismatch");
    }

    TEST_ASSERT(r.pos == r.len, "reader should be at end");
    gr_buf_free(&buf);
}

static void test_u16_roundtrip(void) {
    TEST_SECTION("u16 round-trip");

    gr_buf_t buf;
    gr_buf_init(&buf, 64);

    uint16_t values[] = { 0, 1, 255, 256, 0x1234, 0xFFFF };
    int n = sizeof(values) / sizeof(values[0]);

    for (int i = 0; i < n; i++) {
        gr_buf_write_u16(&buf, values[i]);
    }

    TEST_ASSERT(buf.len == (size_t)(n * 2), "each u16 should be 2 bytes");

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    for (int i = 0; i < n; i++) {
        uint16_t out;
        int rc = gr_read_u16(&r, &out);
        TEST_ASSERT(rc == 0, "read_u16 should succeed");
        TEST_ASSERT(out == values[i], "u16 round-trip value mismatch");
    }

    gr_buf_free(&buf);
}

static void test_u32_roundtrip(void) {
    TEST_SECTION("u32 round-trip");

    gr_buf_t buf;
    gr_buf_init(&buf, 128);

    uint32_t values[] = {
        0, 1, 255, 256, 65535, 65536,
        0x12345678, 0xDEADBEEF, 0xFFFFFFFF
    };
    int n = sizeof(values) / sizeof(values[0]);

    for (int i = 0; i < n; i++) {
        gr_buf_write_u32(&buf, values[i]);
    }

    TEST_ASSERT(buf.len == (size_t)(n * 4), "each u32 should be 4 bytes");

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    for (int i = 0; i < n; i++) {
        uint32_t out;
        int rc = gr_read_u32(&r, &out);
        TEST_ASSERT(rc == 0, "read_u32 should succeed");
        TEST_ASSERT(out == values[i], "u32 round-trip value mismatch");
    }

    gr_buf_free(&buf);
}

static void test_i64_roundtrip(void) {
    TEST_SECTION("i64 round-trip");

    gr_buf_t buf;
    gr_buf_init(&buf, 256);

    int64_t values[] = {
        0, 1, -1,
        127, -128,
        32767, -32768,
        2147483647LL, -2147483648LL,
        INT64_MAX, INT64_MIN,
        /* Typical timestamp values (unix ms) */
        1700000000000LL,
        -1700000000000LL,
    };
    int n = sizeof(values) / sizeof(values[0]);

    for (int i = 0; i < n; i++) {
        gr_buf_write_i64(&buf, values[i]);
    }

    TEST_ASSERT(buf.len == (size_t)(n * 8), "each i64 should be 8 bytes");

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    for (int i = 0; i < n; i++) {
        int64_t out;
        int rc = gr_read_i64(&r, &out);
        TEST_ASSERT(rc == 0, "read_i64 should succeed");
        TEST_ASSERT(out == values[i], "i64 round-trip value mismatch");
    }

    gr_buf_free(&buf);
}

/* ═══════════════════════════════════════════════════════════════════
 *  3. Raw byte writes and reads
 * ═══════════════════════════════════════════════════════════════════ */

static void test_raw_bytes_roundtrip(void) {
    TEST_SECTION("raw bytes round-trip");

    gr_buf_t buf;
    gr_buf_init(&buf, 128);

    /* Simulate writing a 32-byte key and a 64-byte signature */
    uint8_t key[32], sig[64];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < 64; i++) sig[i] = (uint8_t)(255 - i);

    gr_buf_write(&buf, key, 32);
    gr_buf_write(&buf, sig, 64);

    TEST_ASSERT(buf.len == 96, "total length should be 32 + 64");

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    uint8_t key_out[32], sig_out[64];
    int rc1 = gr_read_bytes(&r, key_out, 32);
    int rc2 = gr_read_bytes(&r, sig_out, 64);

    TEST_ASSERT(rc1 == 0, "read key should succeed");
    TEST_ASSERT(rc2 == 0, "read sig should succeed");
    TEST_ASSERT(memcmp(key, key_out, 32) == 0, "key should match");
    TEST_ASSERT(memcmp(sig, sig_out, 64) == 0, "sig should match");
    TEST_ASSERT(r.pos == r.len, "reader should be at end");

    gr_buf_free(&buf);
}

static void test_zero_length_write(void) {
    TEST_SECTION("zero-length raw write");

    gr_buf_t buf;
    gr_buf_init(&buf, 64);

    int rc = gr_buf_write(&buf, "hello", 0);
    TEST_ASSERT(rc == 0, "zero-length write should succeed");
    TEST_ASSERT(buf.len == 0, "len should remain 0");

    gr_buf_free(&buf);
}

/* ═══════════════════════════════════════════════════════════════════
 *  4. String serialization round-trips
 * ═══════════════════════════════════════════════════════════════════ */

static void test_string_roundtrip(void) {
    TEST_SECTION("string round-trip: normal strings");

    gr_buf_t buf;
    gr_buf_init(&buf, 512);

    const char *strings[] = {
        "hello",
        "",
        "a",
        "group-name-with-dashes",
        "192.168.1.100",
        "2001:0db8:85a3:0000:0000:8a2e:0370:7334",
    };
    int n = sizeof(strings) / sizeof(strings[0]);

    for (int i = 0; i < n; i++) {
        gr_buf_write_str(&buf, strings[i], 128);
    }

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    for (int i = 0; i < n; i++) {
        char out[128];
        int rc = gr_read_str(&r, out, sizeof(out));
        TEST_ASSERT(rc == 0, "read_str should succeed");
        TEST_ASSERT(strcmp(out, strings[i]) == 0,
                    "string round-trip should match");
    }

    gr_buf_free(&buf);
}

static void test_string_maxlen_clamping(void) {
    TEST_SECTION("string maxlen clamping");

    gr_buf_t buf;
    gr_buf_init(&buf, 128);

    /* Write with maxlen=5 on a longer string — should clamp to 5 bytes */
    const char *long_str = "abcdefghij";
    gr_buf_write_str(&buf, long_str, 5);

    /* Wire format: [u32 len=5] [5 bytes: "abcde"] = 9 bytes total */
    TEST_ASSERT(buf.len == 9, "clamped string should be 4 + 5 = 9 bytes");

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };
    char out[128];
    int rc = gr_read_str(&r, out, sizeof(out));
    TEST_ASSERT(rc == 0, "read clamped string should succeed");
    TEST_ASSERT(strlen(out) == 5, "decoded length should be 5");
    TEST_ASSERT(memcmp(out, "abcde", 5) == 0,
                "clamped content should be first 5 chars");

    gr_buf_free(&buf);
}

static void test_string_empty(void) {
    TEST_SECTION("string round-trip: empty string");

    gr_buf_t buf;
    gr_buf_init(&buf, 64);

    gr_buf_write_str(&buf, "", 128);

    /* Wire format: [u32 len=0] = 4 bytes, no string body */
    TEST_ASSERT(buf.len == 4, "empty string should be 4 bytes (prefix only)");

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };
    char out[64] = "dirty";
    int rc = gr_read_str(&r, out, sizeof(out));
    TEST_ASSERT(rc == 0, "read empty string should succeed");
    TEST_ASSERT(out[0] == '\0', "empty string should be null-terminated");
    TEST_ASSERT(strlen(out) == 0, "empty string length should be 0");

    gr_buf_free(&buf);
}

static void test_string_read_too_small_buffer(void) {
    TEST_SECTION("string read into too-small buffer");

    gr_buf_t buf;
    gr_buf_init(&buf, 64);

    gr_buf_write_str(&buf, "hello world", 128);

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    /* Try to read into a buffer of 5 bytes — "hello world" is 11.
     * slen (11) >= maxlen (5) → should fail. */
    char tiny[5];
    int rc = gr_read_str(&r, tiny, sizeof(tiny));
    TEST_ASSERT(rc == -1, "read_str should fail when buffer is too small");

    gr_buf_free(&buf);
}

/* ═══════════════════════════════════════════════════════════════════
 *  5. Reader bounds checking
 * ═══════════════════════════════════════════════════════════════════ */

static void test_reader_empty(void) {
    TEST_SECTION("reader on empty data");

    uint8_t empty = 0;
    gr_reader_t r = { .data = &empty, .len = 0, .pos = 0 };

    uint8_t  u8;
    uint16_t u16;
    uint32_t u32;
    int64_t  i64;

    TEST_ASSERT(gr_read_u8(&r, &u8)   == -1, "read_u8 on empty should fail");
    TEST_ASSERT(gr_read_u16(&r, &u16) == -1, "read_u16 on empty should fail");
    TEST_ASSERT(gr_read_u32(&r, &u32) == -1, "read_u32 on empty should fail");
    TEST_ASSERT(gr_read_i64(&r, &i64) == -1, "read_i64 on empty should fail");
    TEST_ASSERT(r.pos == 0, "position should not advance on failure");
}

static void test_reader_partial_data(void) {
    TEST_SECTION("reader with insufficient bytes for type");

    /* 3 bytes: enough for u8 and u16, but not u32 */
    uint8_t data[3] = { 0x01, 0x02, 0x03 };
    gr_reader_t r = { .data = data, .len = 3, .pos = 0 };

    uint8_t u8;
    int rc = gr_read_u8(&r, &u8);
    TEST_ASSERT(rc == 0, "read_u8 should succeed with 3 bytes");
    TEST_ASSERT(u8 == 0x01, "u8 value should be 0x01");
    TEST_ASSERT(r.pos == 1, "pos should be 1 after reading u8");

    /* 2 bytes remain: enough for u16 */
    uint16_t u16;
    rc = gr_read_u16(&r, &u16);
    TEST_ASSERT(rc == 0, "read_u16 should succeed with 2 bytes remaining");
    TEST_ASSERT(r.pos == 3, "pos should be 3 after reading u16");

    /* 0 bytes remain: u32 should fail */
    uint32_t u32;
    rc = gr_read_u32(&r, &u32);
    TEST_ASSERT(rc == -1, "read_u32 should fail with 0 bytes remaining");
    TEST_ASSERT(r.pos == 3, "pos should not advance on failure");
}

static void test_reader_exact_fit(void) {
    TEST_SECTION("reader with exactly enough bytes");

    gr_buf_t buf;
    gr_buf_init(&buf, 64);
    gr_buf_write_u32(&buf, 0xCAFEBABE);
    gr_buf_write_u16(&buf, 0x1234);
    /* Total: 6 bytes */

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    uint32_t u32;
    uint16_t u16;
    gr_read_u32(&r, &u32);
    gr_read_u16(&r, &u16);

    TEST_ASSERT(u32 == 0xCAFEBABE, "u32 value should match");
    TEST_ASSERT(u16 == 0x1234, "u16 value should match");
    TEST_ASSERT(r.pos == r.len, "should be exactly at end");

    /* One more byte should fail */
    uint8_t u8;
    int rc = gr_read_u8(&r, &u8);
    TEST_ASSERT(rc == -1, "read past end should fail");

    gr_buf_free(&buf);
}

static void test_reader_read_bytes_past_end(void) {
    TEST_SECTION("reader read_bytes past end");

    uint8_t data[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    gr_reader_t r = { .data = data, .len = 4, .pos = 0 };

    uint8_t out[8];
    int rc = gr_read_bytes(&r, out, 8);
    TEST_ASSERT(rc == -1, "reading 8 bytes from 4-byte buffer should fail");
    TEST_ASSERT(r.pos == 0, "pos should not advance on failure");

    /* Read exactly 4 should succeed */
    rc = gr_read_bytes(&r, out, 4);
    TEST_ASSERT(rc == 0, "reading exactly 4 bytes should succeed");
    TEST_ASSERT(memcmp(out, data, 4) == 0, "data should match");
}

/* ═══════════════════════════════════════════════════════════════════
 *  6. Endian correctness (wire format verification)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_wire_format_u16(void) {
    TEST_SECTION("wire format: u16 is little-endian");

    gr_buf_t buf;
    gr_buf_init(&buf, 16);

    gr_buf_write_u16(&buf, 0x0102);

    TEST_ASSERT(buf.len == 2, "u16 should be 2 bytes");
    /* Little-endian: low byte first */
    TEST_ASSERT(buf.data[0] == 0x02, "byte[0] should be 0x02 (low byte)");
    TEST_ASSERT(buf.data[1] == 0x01, "byte[1] should be 0x01 (high byte)");

    gr_buf_free(&buf);
}

static void test_wire_format_u32(void) {
    TEST_SECTION("wire format: u32 is little-endian");

    gr_buf_t buf;
    gr_buf_init(&buf, 16);

    gr_buf_write_u32(&buf, 0x01020304);

    TEST_ASSERT(buf.len == 4, "u32 should be 4 bytes");
    TEST_ASSERT(buf.data[0] == 0x04, "byte[0] should be 0x04 (LSB)");
    TEST_ASSERT(buf.data[1] == 0x03, "byte[1] should be 0x03");
    TEST_ASSERT(buf.data[2] == 0x02, "byte[2] should be 0x02");
    TEST_ASSERT(buf.data[3] == 0x01, "byte[3] should be 0x01 (MSB)");

    gr_buf_free(&buf);
}

static void test_wire_format_i64(void) {
    TEST_SECTION("wire format: i64 is little-endian");

    gr_buf_t buf;
    gr_buf_init(&buf, 16);

    gr_buf_write_i64(&buf, 0x0102030405060708LL);

    TEST_ASSERT(buf.len == 8, "i64 should be 8 bytes");
    TEST_ASSERT(buf.data[0] == 0x08, "byte[0] should be 0x08 (LSB)");
    TEST_ASSERT(buf.data[1] == 0x07, "byte[1] should be 0x07");
    TEST_ASSERT(buf.data[2] == 0x06, "byte[2] should be 0x06");
    TEST_ASSERT(buf.data[3] == 0x05, "byte[3] should be 0x05");
    TEST_ASSERT(buf.data[4] == 0x04, "byte[4] should be 0x04");
    TEST_ASSERT(buf.data[5] == 0x03, "byte[5] should be 0x03");
    TEST_ASSERT(buf.data[6] == 0x02, "byte[6] should be 0x02");
    TEST_ASSERT(buf.data[7] == 0x01, "byte[7] should be 0x01 (MSB)");

    gr_buf_free(&buf);
}

static void test_wire_format_negative_i64(void) {
    TEST_SECTION("wire format: negative i64");

    gr_buf_t buf;
    gr_buf_init(&buf, 16);

    gr_buf_write_i64(&buf, -1LL);

    TEST_ASSERT(buf.len == 8, "i64 should be 8 bytes");
    /* -1 in two's complement is 0xFFFFFFFFFFFFFFFF → all 0xFF in LE */
    int all_ff = 1;
    for (int i = 0; i < 8; i++) {
        if (buf.data[i] != 0xFF) { all_ff = 0; break; }
    }
    TEST_ASSERT(all_ff, "all bytes of -1 should be 0xFF");

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };
    int64_t out;
    gr_read_i64(&r, &out);
    TEST_ASSERT(out == -1LL, "decoded -1 should match");

    gr_buf_free(&buf);
}

static void test_wire_format_string(void) {
    TEST_SECTION("wire format: length-prefixed string");

    gr_buf_t buf;
    gr_buf_init(&buf, 64);

    gr_buf_write_str(&buf, "Hi", 128);

    /* Expected: [u32 LE: 2] [0x48 0x69] = 6 bytes total */
    TEST_ASSERT(buf.len == 6, "\"Hi\" should be 4 + 2 = 6 bytes");

    /* Length prefix: 2 in LE = 0x02 0x00 0x00 0x00 */
    TEST_ASSERT(buf.data[0] == 0x02, "length byte[0] = 0x02");
    TEST_ASSERT(buf.data[1] == 0x00, "length byte[1] = 0x00");
    TEST_ASSERT(buf.data[2] == 0x00, "length byte[2] = 0x00");
    TEST_ASSERT(buf.data[3] == 0x00, "length byte[3] = 0x00");

    /* String body */
    TEST_ASSERT(buf.data[4] == 'H', "string byte[0] = 'H'");
    TEST_ASSERT(buf.data[5] == 'i', "string byte[1] = 'i'");

    gr_buf_free(&buf);
}

/* ═══════════════════════════════════════════════════════════════════
 *  7. Edge cases and mixed-type sequences
 * ═══════════════════════════════════════════════════════════════════ */

static void test_mixed_types_roundtrip(void) {
    TEST_SECTION("mixed types round-trip (simulates a mini registrar blob)");

    gr_buf_t buf;
    gr_buf_init(&buf, 256);

    /* Simulate a small header-like structure */
    uint8_t  fake_hash[32];
    memset(fake_hash, 0xAA, 32);

    gr_buf_write(&buf, fake_hash, 32);          /* group_id */
    gr_buf_write_u32(&buf, 1);                  /* group_type */
    gr_buf_write_str(&buf, "test group", 128);  /* group_name */
    gr_buf_write_u32(&buf, 42);                 /* version */
    gr_buf_write_i64(&buf, 1700000000000LL);    /* created_at */
    gr_buf_write_u16(&buf, 8080);               /* port */
    gr_buf_write_u8(&buf, 0xFF);                /* flag */

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    uint8_t hash_out[32];
    uint32_t type_out, version_out;
    char name_out[128];
    int64_t ts_out;
    uint16_t port_out;
    uint8_t flag_out;

    TEST_ASSERT(gr_read_bytes(&r, hash_out, 32) == 0, "read hash");
    TEST_ASSERT(memcmp(hash_out, fake_hash, 32) == 0, "hash match");

    TEST_ASSERT(gr_read_u32(&r, &type_out) == 0, "read type");
    TEST_ASSERT(type_out == 1, "type match");

    TEST_ASSERT(gr_read_str(&r, name_out, 128) == 0, "read name");
    TEST_ASSERT(strcmp(name_out, "test group") == 0, "name match");

    TEST_ASSERT(gr_read_u32(&r, &version_out) == 0, "read version");
    TEST_ASSERT(version_out == 42, "version match");

    TEST_ASSERT(gr_read_i64(&r, &ts_out) == 0, "read timestamp");
    TEST_ASSERT(ts_out == 1700000000000LL, "timestamp match");

    TEST_ASSERT(gr_read_u16(&r, &port_out) == 0, "read port");
    TEST_ASSERT(port_out == 8080, "port match");

    TEST_ASSERT(gr_read_u8(&r, &flag_out) == 0, "read flag");
    TEST_ASSERT(flag_out == 0xFF, "flag match");

    TEST_ASSERT(r.pos == r.len, "reader should be at end");

    gr_buf_free(&buf);
}

static void test_i64_boundary_values(void) {
    TEST_SECTION("i64 boundary values (INT64_MIN, INT64_MAX, 0)");

    gr_buf_t buf;
    gr_buf_init(&buf, 64);

    gr_buf_write_i64(&buf, INT64_MIN);
    gr_buf_write_i64(&buf, INT64_MAX);
    gr_buf_write_i64(&buf, 0);

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    int64_t v;
    gr_read_i64(&r, &v);
    TEST_ASSERT(v == INT64_MIN, "INT64_MIN round-trip");
    gr_read_i64(&r, &v);
    TEST_ASSERT(v == INT64_MAX, "INT64_MAX round-trip");
    gr_read_i64(&r, &v);
    TEST_ASSERT(v == 0, "zero round-trip");

    gr_buf_free(&buf);
}

static void test_many_strings_sequential(void) {
    TEST_SECTION("many strings written and read sequentially");

    gr_buf_t buf;
    gr_buf_init(&buf, 4096);

    /* Write 100 numbered strings */
    char tmp[64];
    for (int i = 0; i < 100; i++) {
        snprintf(tmp, sizeof(tmp), "string-%03d", i);
        gr_buf_write_str(&buf, tmp, sizeof(tmp));
    }

    gr_reader_t r = { .data = buf.data, .len = buf.len, .pos = 0 };

    int all_ok = 1;
    for (int i = 0; i < 100; i++) {
        char expected[64], actual[64];
        snprintf(expected, sizeof(expected), "string-%03d", i);
        if (gr_read_str(&r, actual, sizeof(actual)) != 0) {
            all_ok = 0; break;
        }
        if (strcmp(actual, expected) != 0) {
            all_ok = 0; break;
        }
    }
    TEST_ASSERT(all_ok, "all 100 strings should round-trip correctly");
    TEST_ASSERT(r.pos == r.len, "reader should be at end");

    gr_buf_free(&buf);
}

static void test_reader_does_not_advance_on_failure(void) {
    TEST_SECTION("reader position stable on failure");

    uint8_t data[2] = { 0xAA, 0xBB };
    gr_reader_t r = { .data = data, .len = 2, .pos = 0 };

    /* Try to read 4 bytes — should fail, pos stays at 0 */
    uint32_t u32;
    int rc = gr_read_u32(&r, &u32);
    TEST_ASSERT(rc == -1, "read_u32 from 2 bytes should fail");
    TEST_ASSERT(r.pos == 0, "pos should be 0 after failed read");

    /* Now read 2 bytes — should succeed */
    uint16_t u16;
    rc = gr_read_u16(&r, &u16);
    TEST_ASSERT(rc == 0, "read_u16 from 2 bytes should succeed");
    TEST_ASSERT(r.pos == 2, "pos should be 2 after successful read");

    /* Try again — should fail, pos stays at 2 */
    uint8_t u8;
    rc = gr_read_u8(&r, &u8);
    TEST_ASSERT(rc == -1, "read past end should fail");
    TEST_ASSERT(r.pos == 2, "pos should stay at 2");
}

static void test_endian_helpers_identity(void) {
    TEST_SECTION("endian helpers: htole → letoh is identity");

    /* For all types, converting to LE and back should be identity */
    uint16_t v16 = 0xABCD;
    TEST_ASSERT(gr_le16toh(gr_htole16(v16)) == v16,
                "u16 htole→letoh identity");

    uint32_t v32 = 0x12345678;
    TEST_ASSERT(gr_le32toh(gr_htole32(v32)) == v32,
                "u32 htole→letoh identity");

    uint64_t v64 = 0x0102030405060708ULL;
    TEST_ASSERT(gr_le64toh(gr_htole64(v64)) == v64,
                "u64 htole→letoh identity");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    fprintf(stdout, "═══ buf.h / buf.c test suite ═══\n\n");

    /* 1. Writer lifecycle */
    test_buf_init_and_free();
    test_buf_double_free();
    test_buf_growth();
    test_buf_large_single_write();

    /* 2. Integer round-trips */
    test_u8_roundtrip();
    test_u16_roundtrip();
    test_u32_roundtrip();
    test_i64_roundtrip();

    /* 3. Raw bytes */
    test_raw_bytes_roundtrip();
    test_zero_length_write();

    /* 4. Strings */
    test_string_roundtrip();
    test_string_maxlen_clamping();
    test_string_empty();
    test_string_read_too_small_buffer();

    /* 5. Reader bounds */
    test_reader_empty();
    test_reader_partial_data();
    test_reader_exact_fit();
    test_reader_read_bytes_past_end();

    /* 6. Endian / wire format */
    test_wire_format_u16();
    test_wire_format_u32();
    test_wire_format_i64();
    test_wire_format_negative_i64();
    test_wire_format_string();

    /* 7. Edge cases */
    test_mixed_types_roundtrip();
    test_i64_boundary_values();
    test_many_strings_sequential();
    test_reader_does_not_advance_on_failure();
    test_endian_helpers_identity();

    /* Summary */
    fprintf(stdout, "\n═══ Results: %d/%d passed ═══\n",
            g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        fprintf(stderr, "\n%d test(s) FAILED\n", g_tests_failed);
        return 1;
    }

    fprintf(stdout, "All tests passed.\n");
    return 0;
}
