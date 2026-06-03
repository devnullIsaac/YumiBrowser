/*
 * test_db.c - Tests for db.c: schema initialization, header persistence, DB helpers, and prepared statement cache (in-memory DuckDB).
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
 * @file test_db.c
 * @brief Test suite for db.c — schema, header persistence, DB helpers,
 *        and prepared statement cache.
 *
 * All tests use in-memory DuckDB (NULL path to duckdb_open).
 *
 * Dependencies: db.c, buf.c, libduckdb, OpenSSL + oqs-provider.
 */

#include "internal.h"
#include <stdio.h>
#include <string.h>

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

/* ── Helper: create an in-memory registrar handle for testing ──── */

static bool setup_inmemory_reg(gr_registrar_t *reg) {
    memset(reg, 0, sizeof(*reg));

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&reg->db_lock, &attr);
    pthread_mutexattr_destroy(&attr);

    if (duckdb_open(NULL, &reg->db) != DuckDBSuccess)
        return false;
    if (duckdb_connect(reg->db, &reg->con) != DuckDBSuccess) {
        duckdb_close(&reg->db);
        return false;
    }
    return true;
}

static void teardown_reg(gr_registrar_t *reg) {
    if (reg->stmts_ready)
        gr_destroy_statements(reg);
    duckdb_disconnect(&reg->con);
    duckdb_close(&reg->db);
    pthread_mutex_destroy(&reg->db_lock);
    memset(reg, 0, sizeof(*reg));
}

/**
 * @brief Fill a gr_header_t with known test values for round-trip verification.
 */
static void fill_test_header(gr_header_t *h) {
    memset(h, 0, sizeof(*h));

    for (int i = 0; i < GR_HASH_LEN; i++)
        h->group_id[i] = (uint8_t)i;

    h->group_type = GR_GROUP_PRIVATE;
    strncpy(h->group_name, "test-group-alpha", GR_MAX_NAME_LEN - 1);

    h->version = 42;
    h->created_at = 1700000000000LL;
    h->updated_at = 1700000099999LL;
    h->epoch_id = 7;

    h->retention.message_retention_ms = GR_DEFAULT_MESSAGE_RETENTION_MS;
    h->retention.file_retention_ms    = GR_DEFAULT_FILE_RETENTION_MS;
    h->retention.registrar_max_bytes  = GR_DEFAULT_REGISTRAR_MAX_BYTES;

    memset(h->owner_id, 0xAA, GR_PEER_ID_LEN);
    memset(h->owner_sign_key, 0xBB, GR_PUBLIC_KEY_LEN);
    memset(h->signer_id, 0x11, GR_PEER_ID_LEN);
    memset(h->signer_sign_key, 0x22, GR_PUBLIC_KEY_LEN);
    memset(h->signature, 0xCC, GR_SIGN_LEN);
    memset(h->hash, 0xDD, GR_HASH_LEN);
}

/**
 * @brief Compare two headers field by field.
 * @return Name of first mismatched field, or NULL if identical.
 */
static const char *compare_headers(const gr_header_t *a, const gr_header_t *b) {
    if (memcmp(a->group_id, b->group_id, GR_HASH_LEN) != 0)
        return "group_id";
    if (a->group_type != b->group_type)
        return "group_type";
    if (strcmp(a->group_name, b->group_name) != 0)
        return "group_name";
    if (a->version != b->version)
        return "version";
    if (a->created_at != b->created_at)
        return "created_at";
    if (a->updated_at != b->updated_at)
        return "updated_at";
    if (a->epoch_id != b->epoch_id)
        return "epoch_id";
    if (a->retention.message_retention_ms != b->retention.message_retention_ms)
        return "message_retention_ms";
    if (a->retention.file_retention_ms != b->retention.file_retention_ms)
        return "file_retention_ms";
    if (a->retention.registrar_max_bytes != b->retention.registrar_max_bytes)
        return "registrar_max_bytes";
    if (memcmp(a->owner_id, b->owner_id, GR_PEER_ID_LEN) != 0)
        return "owner_id";
    if (memcmp(a->owner_sign_key, b->owner_sign_key, GR_PUBLIC_KEY_LEN) != 0)
        return "owner_sign_key";
    if (memcmp(a->signer_id, b->signer_id, GR_PEER_ID_LEN) != 0)
        return "signer_id";
    if (memcmp(a->signer_sign_key, b->signer_sign_key, GR_PUBLIC_KEY_LEN) != 0)
        return "signer_sign_key";
    if (memcmp(a->signature, b->signature, GR_SIGN_LEN) != 0)
        return "signature";
    if (memcmp(a->hash, b->hash, GR_HASH_LEN) != 0)
        return "hash";
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  1. DB helpers
 * ═══════════════════════════════════════════════════════════════════ */

static void test_db_exec_success(void) {
    TEST_SECTION("gr_db_exec: valid SQL succeeds");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    TEST_ASSERT(gr_db_exec(reg.con, "SELECT 1"), "SELECT 1 should succeed");
    TEST_ASSERT(gr_db_exec(reg.con, "CREATE TABLE test_exec (id INTEGER)"),
                "CREATE TABLE should succeed");
    TEST_ASSERT(gr_db_exec(reg.con, "DROP TABLE test_exec"),
                "DROP TABLE should succeed");

    teardown_reg(&reg);
}

static void test_db_exec_failure(void) {
    TEST_SECTION("gr_db_exec: invalid SQL fails");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    TEST_ASSERT(!gr_db_exec(reg.con, "THIS IS NOT VALID SQL"),
                "invalid SQL should fail");
    TEST_ASSERT(!gr_db_exec(reg.con, "DROP TABLE nonexistent_table_xyz"),
                "dropping nonexistent table should fail");

    teardown_reg(&reg);
}

static void test_db_vec_get_blob(void) {
    TEST_SECTION("gr_db_vec_get_blob: extract BLOB from chunk vector");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    gr_db_exec(reg.con, "CREATE TABLE blob_test (data BLOB)");
    gr_db_exec(reg.con, "INSERT INTO blob_test VALUES ('\\xDE\\xAD\\xBE\\xEF')");

    duckdb_result result;
    duckdb_query(reg.con, "SELECT data FROM blob_test LIMIT 1", &result);

    duckdb_data_chunk chunk = NULL;
    gr_error_t err = gr_db_fetch_first_chunk(&result, &chunk);
    TEST_ASSERT(err == GR_OK, "first chunk fetch should succeed");

    uint8_t out[4] = {0};
    err = gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 0), 0,
                             out, sizeof(out));
    TEST_ASSERT(err == GR_OK, "blob read should succeed");
    TEST_ASSERT(out[0] == 0xDE && out[1] == 0xAD
             && out[2] == 0xBE && out[3] == 0xEF,
                "blob bytes should match");

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    teardown_reg(&reg);
}

static void test_db_vec_get_blob_length_mismatch(void) {
    TEST_SECTION("gr_db_vec_get_blob: rejects length mismatch");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    gr_db_exec(reg.con, "CREATE TABLE blob_mm (data BLOB)");
    gr_db_exec(reg.con, "INSERT INTO blob_mm VALUES "
               "('\\x01\\x02\\x03\\x04\\x05\\x06\\x07\\x08')");

    duckdb_result result;
    duckdb_query(reg.con, "SELECT data FROM blob_mm LIMIT 1", &result);

    duckdb_data_chunk chunk = NULL;
    TEST_ASSERT(gr_db_fetch_first_chunk(&result, &chunk) == GR_OK,
                "first chunk fetch should succeed");

    uint8_t out[4] = {0};
    gr_error_t err = gr_db_vec_get_blob(
        duckdb_data_chunk_get_vector(chunk, 0), 0, out, 4);
    TEST_ASSERT(err == GR_ERR_DB,
                "fixed-width blob read must fail on length mismatch");

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    teardown_reg(&reg);
}

static void test_db_vec_get_blob_null(void) {
    TEST_SECTION("gr_db_vec_get_blob: NULL blob returns GR_ERR_DB");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    gr_db_exec(reg.con, "CREATE TABLE blob_null (data BLOB)");
    gr_db_exec(reg.con, "INSERT INTO blob_null VALUES (NULL)");

    duckdb_result result;
    duckdb_query(reg.con, "SELECT data FROM blob_null LIMIT 1", &result);

    duckdb_data_chunk chunk = NULL;
    TEST_ASSERT(gr_db_fetch_first_chunk(&result, &chunk) == GR_OK,
                "first chunk fetch should succeed");

    uint8_t out[32];
    memset(out, 0xFF, sizeof(out));
    gr_error_t err = gr_db_vec_get_blob(
        duckdb_data_chunk_get_vector(chunk, 0), 0, out, sizeof(out));
    TEST_ASSERT(err == GR_ERR_DB, "NULL blob should report GR_ERR_DB");
    TEST_ASSERT(out[0] == 0xFF, "buffer should not be modified on failure");

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    teardown_reg(&reg);
}

static void test_db_vec_get_blob_alloc(void) {
    TEST_SECTION("gr_db_vec_get_blob_alloc: variable-length BLOB");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    gr_db_exec(reg.con, "CREATE TABLE blob_var (data BLOB)");
    gr_db_exec(reg.con, "INSERT INTO blob_var VALUES "
               "('\\x01\\x02\\x03\\x04\\x05')");

    duckdb_result result;
    duckdb_query(reg.con, "SELECT data FROM blob_var LIMIT 1", &result);

    duckdb_data_chunk chunk = NULL;
    TEST_ASSERT(gr_db_fetch_first_chunk(&result, &chunk) == GR_OK,
                "first chunk fetch should succeed");

    uint8_t *buf = NULL;
    size_t len = 0;
    gr_error_t err = gr_db_vec_get_blob_alloc(
        duckdb_data_chunk_get_vector(chunk, 0), 0, &buf, &len);
    TEST_ASSERT(err == GR_OK, "alloc read should succeed");
    TEST_ASSERT(len == 5, "length should be 5");
    TEST_ASSERT(buf != NULL && buf[0] == 0x01 && buf[4] == 0x05,
                "payload should match");
    free(buf);

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    teardown_reg(&reg);
}

static void test_db_vec_get_str(void) {
    TEST_SECTION("gr_db_vec_get_str: extract VARCHAR from chunk vector");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    gr_db_exec(reg.con, "CREATE TABLE str_test (name VARCHAR)");
    gr_db_exec(reg.con, "INSERT INTO str_test VALUES ('hello world')");

    duckdb_result result;
    duckdb_query(reg.con, "SELECT name FROM str_test LIMIT 1", &result);

    duckdb_data_chunk chunk = NULL;
    TEST_ASSERT(gr_db_fetch_first_chunk(&result, &chunk) == GR_OK,
                "first chunk fetch should succeed");

    char out[64] = {0};
    gr_error_t err = gr_db_vec_get_str(
        duckdb_data_chunk_get_vector(chunk, 0), 0, out, sizeof(out));
    TEST_ASSERT(err == GR_OK, "str read should succeed");
    TEST_ASSERT(strcmp(out, "hello world") == 0, "string should match");

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    teardown_reg(&reg);
}

static void test_db_vec_get_str_too_small(void) {
    TEST_SECTION("gr_db_vec_get_str: fails when buffer too small");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    gr_db_exec(reg.con, "CREATE TABLE str_small (name VARCHAR)");
    gr_db_exec(reg.con, "INSERT INTO str_small VALUES ('abcdefghij')");

    duckdb_result result;
    duckdb_query(reg.con, "SELECT name FROM str_small LIMIT 1", &result);

    duckdb_data_chunk chunk = NULL;
    TEST_ASSERT(gr_db_fetch_first_chunk(&result, &chunk) == GR_OK,
                "first chunk fetch should succeed");

    char out[6] = {0};
    gr_error_t err = gr_db_vec_get_str(
        duckdb_data_chunk_get_vector(chunk, 0), 0, out, sizeof(out));
    TEST_ASSERT(err == GR_ERR_DB,
                "str read into undersized buffer should fail");

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    teardown_reg(&reg);
}

static void test_db_vec_get_str_null(void) {
    TEST_SECTION("gr_db_vec_get_str: NULL VARCHAR produces empty string");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    gr_db_exec(reg.con, "CREATE TABLE str_null (name VARCHAR)");
    gr_db_exec(reg.con, "INSERT INTO str_null VALUES (NULL)");

    duckdb_result result;
    duckdb_query(reg.con, "SELECT name FROM str_null LIMIT 1", &result);

    duckdb_data_chunk chunk = NULL;
    TEST_ASSERT(gr_db_fetch_first_chunk(&result, &chunk) == GR_OK,
                "first chunk fetch should succeed");

    char out[32];
    memset(out, 'X', sizeof(out));
    gr_error_t err = gr_db_vec_get_str(
        duckdb_data_chunk_get_vector(chunk, 0), 0, out, sizeof(out));
    TEST_ASSERT(err == GR_OK, "NULL VARCHAR read should succeed");
    TEST_ASSERT(out[0] == '\0',
                "NULL VARCHAR should produce empty string");

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    teardown_reg(&reg);
}

static void test_db_vec_scalars(void) {
    TEST_SECTION("gr_db_vec_get_i32/i64/bool/double: scalar accessors");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    duckdb_result result;
    duckdb_query(reg.con,
                 "SELECT CAST(-42 AS INTEGER), CAST(9000000000 AS BIGINT), "
                 "true, CAST(3.5 AS DOUBLE)",
                 &result);

    duckdb_data_chunk chunk = NULL;
    TEST_ASSERT(gr_db_fetch_first_chunk(&result, &chunk) == GR_OK,
                "first chunk fetch should succeed");

    int32_t i32 = 0;
    int64_t i64 = 0;
    bool    b   = false;
    double  d   = 0.0;
    TEST_ASSERT(gr_db_vec_get_i32(
                    duckdb_data_chunk_get_vector(chunk, 0), 0, &i32) == GR_OK
                && i32 == -42, "i32 should read -42");
    TEST_ASSERT(gr_db_vec_get_i64(
                    duckdb_data_chunk_get_vector(chunk, 1), 0, &i64) == GR_OK
                && i64 == 9000000000LL, "i64 should read 9000000000");
    TEST_ASSERT(gr_db_vec_get_bool(
                    duckdb_data_chunk_get_vector(chunk, 2), 0, &b) == GR_OK
                && b == true, "bool should read true");
    TEST_ASSERT(gr_db_vec_get_double(
                    duckdb_data_chunk_get_vector(chunk, 3), 0, &d) == GR_OK
                && d == 3.5, "double should read 3.5");

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    teardown_reg(&reg);
}

static void test_db_fetch_scalar_helpers(void) {
    TEST_SECTION("gr_db_fetch_i64_scalar / gr_db_fetch_i32_scalar");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    /* Populated result */
    {
        duckdb_result r;
        duckdb_query(reg.con, "SELECT CAST(1234567890123 AS BIGINT)", &r);
        int64_t v = 0;
        TEST_ASSERT(gr_db_fetch_i64_scalar(&r, &v) == GR_OK
                    && v == 1234567890123LL,
                    "i64 scalar should read value");
        duckdb_destroy_result(&r);
    }
    {
        duckdb_result r;
        duckdb_query(reg.con, "SELECT CAST(-7 AS INTEGER)", &r);
        int32_t v = 0;
        TEST_ASSERT(gr_db_fetch_i32_scalar(&r, &v) == GR_OK && v == -7,
                    "i32 scalar should read value");
        duckdb_destroy_result(&r);
    }

    /* Empty result => GR_ERR_NOT_FOUND */
    {
        duckdb_result r;
        duckdb_query(reg.con,
                     "SELECT CAST(1 AS BIGINT) WHERE 1 = 0", &r);
        int64_t v = 0;
        TEST_ASSERT(gr_db_fetch_i64_scalar(&r, &v) == GR_ERR_NOT_FOUND,
                    "empty result should return NOT_FOUND");
        duckdb_destroy_result(&r);
    }

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  2. Schema initialization
 * ═══════════════════════════════════════════════════════════════════ */

static void test_schema_init(void) {
    TEST_SECTION("gr_db_init_schema: creates all 9 tables");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    TEST_ASSERT(gr_db_init_schema(reg.con), "schema init should succeed");

    static const char *table_queries[] = {
        "SELECT COUNT(*) FROM gr_header",
        "SELECT COUNT(*) FROM gr_peers",
        "SELECT COUNT(*) FROM gr_roles",
        "SELECT COUNT(*) FROM gr_webapps",
        "SELECT COUNT(*) FROM gr_servers",
        "SELECT COUNT(*) FROM gr_epochs",
        "SELECT COUNT(*) FROM gr_audit_log",
        "SELECT COUNT(*) FROM gr_invites",
        "SELECT COUNT(*) FROM gr_group_icon",
        NULL
    };
    for (int i = 0; table_queries[i] != NULL; i++) {
        duckdb_result result;
        duckdb_state st = duckdb_query(reg.con, table_queries[i], &result);
        TEST_ASSERT(st == DuckDBSuccess, "table should be queryable");
        duckdb_destroy_result(&result);
    }

    teardown_reg(&reg);
}

static void test_schema_init_idempotent(void) {
    TEST_SECTION("gr_db_init_schema: idempotent (calling twice is safe)");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    TEST_ASSERT(gr_db_init_schema(reg.con), "first schema init should succeed");
    gr_db_exec(reg.con, "INSERT INTO gr_roles VALUES (1, 'admin', 255, NULL, 1000, 1000)");
    TEST_ASSERT(gr_db_init_schema(reg.con), "second schema init should succeed");

    duckdb_result result;
    duckdb_query(reg.con, "SELECT COUNT(*) FROM gr_roles", &result);
    int64_t count = 0;
    gr_db_fetch_i64_scalar(&result, &count);
    duckdb_destroy_result(&result);
    TEST_ASSERT(count == 1, "data should survive idempotent schema init");

    teardown_reg(&reg);
}

static void test_schema_column_counts(void) {
    TEST_SECTION("schema: verify column counts for each table");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    struct { const char *table; idx_t expected_cols; } checks[] = {
        { "gr_header",    16 },
        { "gr_peers",     12 },
        { "gr_roles",      6 },
        { "gr_webapps",    7 },
        { "gr_servers",    8 },
        { "gr_epochs",     5 },
        { "gr_audit_log",  10 },
        { "gr_invites",    7 },
        { "gr_group_icon", 10 },
    };
    int n = sizeof(checks) / sizeof(checks[0]);

    for (int i = 0; i < n; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "SELECT * FROM %s LIMIT 0", checks[i].table);
        duckdb_result result;
        duckdb_state st = duckdb_query(reg.con, sql, &result);
        TEST_ASSERT(st == DuckDBSuccess, "query should succeed");
        idx_t col_count = duckdb_column_count(&result);
        if (col_count != checks[i].expected_cols) {
            fprintf(stderr, "    %s: expected %llu cols, got %llu\n",
                    checks[i].table,
                    (unsigned long long)checks[i].expected_cols,
                    (unsigned long long)col_count);
        }
        TEST_ASSERT(col_count == checks[i].expected_cols,
                    "column count should match schema definition");
        duckdb_destroy_result(&result);
    }

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  3. Header save/load round-trip (fallback path)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_header_roundtrip_fallback(void) {
    TEST_SECTION("header save/load round-trip (fallback path)");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);
    TEST_ASSERT(!reg.stmts_ready, "stmts should not be ready yet");

    fill_test_header(&reg.header);
    gr_error_t err = gr_header_save(&reg);
    TEST_ASSERT(err == GR_OK, "header save should succeed");

    gr_header_t saved = reg.header;
    memset(&reg.header, 0, sizeof(reg.header));
    err = gr_header_load(&reg);
    TEST_ASSERT(err == GR_OK, "header load should succeed");

    const char *mismatch = compare_headers(&saved, &reg.header);
    if (mismatch) fprintf(stderr, "    first mismatch: %s\n", mismatch);
    TEST_ASSERT(mismatch == NULL, "all header fields should round-trip");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  4. Header save/load round-trip (cached path)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_header_roundtrip_cached(void) {
    TEST_SECTION("header save/load round-trip (cached path)");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    gr_error_t err = gr_prepare_statements(&reg);
    TEST_ASSERT(err == GR_OK, "prepare statements should succeed");
    TEST_ASSERT(reg.stmts_ready, "stmts should be ready");

    fill_test_header(&reg.header);
    err = gr_header_save(&reg);
    TEST_ASSERT(err == GR_OK, "cached header save should succeed");

    gr_header_t saved = reg.header;
    memset(&reg.header, 0, sizeof(reg.header));
    err = gr_header_load(&reg);
    TEST_ASSERT(err == GR_OK, "header load should succeed");

    const char *mismatch = compare_headers(&saved, &reg.header);
    if (mismatch) fprintf(stderr, "    first mismatch: %s\n", mismatch);
    TEST_ASSERT(mismatch == NULL, "all header fields should round-trip (cached)");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  5. Header field-by-field verification
 * ═══════════════════════════════════════════════════════════════════ */

static void test_header_field_isolation(void) {
    TEST_SECTION("header: each field stored/loaded independently");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    fill_test_header(&reg.header);
    gr_header_save(&reg);

    memset(&reg.header, 0, sizeof(reg.header));
    gr_header_load(&reg);
    gr_header_t *h = &reg.header;

    /* Col 0: group_id */
    {
        int ok = 1;
        for (int i = 0; i < GR_HASH_LEN; i++) {
            if (h->group_id[i] != (uint8_t)i) { ok = 0; break; }
        }
        TEST_ASSERT(ok, "col 0: group_id bytes should be 0..31");
    }

    /* Col 1: group_type */
    TEST_ASSERT(h->group_type == GR_GROUP_PRIVATE,
                "col 1: group_type should be PRIVATE (0)");

    /* Col 2: group_name */
    TEST_ASSERT(strcmp(h->group_name, "test-group-alpha") == 0,
                "col 2: group_name should match");

    /* Col 3: version */
    TEST_ASSERT(h->version == 42, "col 3: version should be 42");

    /* Col 4: created_at */
    TEST_ASSERT(h->created_at == 1700000000000LL,
                "col 4: created_at should match");

    /* Col 5: updated_at */
    TEST_ASSERT(h->updated_at == 1700000099999LL,
                "col 5: updated_at should match");

    /* Col 6: epoch_id */
    TEST_ASSERT(h->epoch_id == 7, "col 6: epoch_id should be 7");

    /* Col 7: message_retention_ms */
    TEST_ASSERT(h->retention.message_retention_ms == GR_DEFAULT_MESSAGE_RETENTION_MS,
                "col 7: message_retention_ms should match default");

    /* Col 8: file_retention_ms */
    TEST_ASSERT(h->retention.file_retention_ms == GR_DEFAULT_FILE_RETENTION_MS,
                "col 8: file_retention_ms should match default");

    /* Col 9: registrar_max_bytes */
    TEST_ASSERT(h->retention.registrar_max_bytes == GR_DEFAULT_REGISTRAR_MAX_BYTES,
                "col 9: registrar_max_bytes should match default");

    /* Col 10: owner_id */
    {
        uint8_t expected[GR_PEER_ID_LEN];
        memset(expected, 0xAA, GR_PEER_ID_LEN);
        TEST_ASSERT(memcmp(h->owner_id, expected, GR_PEER_ID_LEN) == 0,
                    "col 10: owner_id should be all 0xAA");
    }

    /* Col 11: owner_sign_key */
    {
        uint8_t expected[GR_PUBLIC_KEY_LEN];
        memset(expected, 0xBB, GR_PUBLIC_KEY_LEN);
        TEST_ASSERT(memcmp(h->owner_sign_key, expected, GR_PUBLIC_KEY_LEN) == 0,
                    "col 11: owner_sign_key should be all 0xBB");
    }

    /* Col 12: signer_id */
    {
        uint8_t expected[GR_PEER_ID_LEN];
        memset(expected, 0x11, GR_PEER_ID_LEN);
        TEST_ASSERT(memcmp(h->signer_id, expected, GR_PEER_ID_LEN) == 0,
                    "col 12: signer_id should be all 0x11");
    }

    /* Col 13: signer_sign_key */
    {
        uint8_t expected[GR_PUBLIC_KEY_LEN];
        memset(expected, 0x22, GR_PUBLIC_KEY_LEN);
        TEST_ASSERT(memcmp(h->signer_sign_key, expected, GR_PUBLIC_KEY_LEN) == 0,
                    "col 13: signer_sign_key should be all 0x22");
    }

    /* Col 14: signature */
    {
        uint8_t expected[GR_SIGN_LEN];
        memset(expected, 0xCC, GR_SIGN_LEN);
        TEST_ASSERT(memcmp(h->signature, expected, GR_SIGN_LEN) == 0,
                    "col 14: signature should be all 0xCC");
    }

    /* Col 15: hash */
    {
        uint8_t expected[GR_HASH_LEN];
        memset(expected, 0xDD, GR_HASH_LEN);
        TEST_ASSERT(memcmp(h->hash, expected, GR_HASH_LEN) == 0,
                    "col 15: hash should be all 0xDD");
    }

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  6. Prepared statement lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

static void test_prepare_statements(void) {
    TEST_SECTION("gr_prepare_statements: all statements prepare successfully");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    gr_error_t err = gr_prepare_statements(&reg);
    TEST_ASSERT(err == GR_OK, "all statements should prepare");
    TEST_ASSERT(reg.stmts_ready, "stmts_ready should be true");

    teardown_reg(&reg);
}

static void test_prepare_without_schema_fails(void) {
    TEST_SECTION("gr_prepare_statements: fails without schema");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");

    gr_error_t err = gr_prepare_statements(&reg);
    TEST_ASSERT(err == GR_ERR_DB, "prepare should fail without schema");

    reg.stmts_ready = true;
    teardown_reg(&reg);
}

static void test_destroy_statements(void) {
    TEST_SECTION("gr_destroy_statements: cleans up and resets flag");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);
    gr_prepare_statements(&reg);

    TEST_ASSERT(reg.stmts_ready, "stmts should be ready before destroy");
    gr_destroy_statements(&reg);
    TEST_ASSERT(!reg.stmts_ready, "stmts_ready should be false after destroy");

    teardown_reg(&reg);
}

static void test_destroy_statements_safe_on_not_ready(void) {
    TEST_SECTION("gr_destroy_statements: safe when stmts_ready=false");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    TEST_ASSERT(!reg.stmts_ready, "stmts should not be ready");
    gr_destroy_statements(&reg);
    TEST_ASSERT(!reg.stmts_ready, "still not ready after no-op destroy");
    teardown_reg(&reg);
}

static void test_destroy_statements_null_safe(void) {
    TEST_SECTION("gr_destroy_statements: safe with NULL pointer");
    gr_destroy_statements(NULL);
    TEST_ASSERT(true, "NULL destroy should not crash");
}

static void test_prepare_destroy_cycle(void) {
    TEST_SECTION("prepare -> destroy -> prepare -> destroy cycle");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    gr_error_t err = gr_prepare_statements(&reg);
    TEST_ASSERT(err == GR_OK, "first prepare should succeed");
    gr_destroy_statements(&reg);
    TEST_ASSERT(!reg.stmts_ready, "should be not-ready after first destroy");

    err = gr_prepare_statements(&reg);
    TEST_ASSERT(err == GR_OK, "second prepare should succeed");
    TEST_ASSERT(reg.stmts_ready, "should be ready after second prepare");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  7. Header load on empty table
 * ═══════════════════════════════════════════════════════════════════ */

static void test_header_load_empty(void) {
    TEST_SECTION("gr_header_load: returns GR_ERR_NOT_FOUND on empty table");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    gr_error_t err = gr_header_load(&reg);
    TEST_ASSERT(err == GR_ERR_NOT_FOUND, "should return NOT_FOUND");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  8. Multiple save/load cycles (overwrite correctness)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_header_overwrite(void) {
    TEST_SECTION("header save overwrites previous data (always 1 row)");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    fill_test_header(&reg.header);
    reg.header.version = 1;
    strncpy(reg.header.group_name, "first", GR_MAX_NAME_LEN - 1);
    gr_header_save(&reg);

    reg.header.version = 99;
    strncpy(reg.header.group_name, "second", GR_MAX_NAME_LEN - 1);
    memset(reg.header.hash, 0xEE, GR_HASH_LEN);
    gr_header_save(&reg);

    duckdb_result result;
    duckdb_query(reg.con, "SELECT COUNT(*) FROM gr_header", &result);
    int64_t count = 0;
    gr_db_fetch_i64_scalar(&result, &count);
    duckdb_destroy_result(&result);
    TEST_ASSERT(count == 1, "should always have exactly 1 row");

    memset(&reg.header, 0, sizeof(reg.header));
    gr_header_load(&reg);
    TEST_ASSERT(reg.header.version == 99, "version should be 99");
    TEST_ASSERT(strcmp(reg.header.group_name, "second") == 0, "name should be 'second'");

    uint8_t expected_hash[GR_HASH_LEN];
    memset(expected_hash, 0xEE, GR_HASH_LEN);
    TEST_ASSERT(memcmp(reg.header.hash, expected_hash, GR_HASH_LEN) == 0,
                "hash should be all 0xEE");

    teardown_reg(&reg);
}

static void test_header_dirty_flag(void) {
    TEST_SECTION("header_dirty flag behavior");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    reg.header_dirty = true;
    fill_test_header(&reg.header);
    gr_header_save(&reg);
    TEST_ASSERT(!reg.header_dirty, "header_dirty should be false after save");

    reg.header_dirty = true;
    gr_header_load(&reg);
    TEST_ASSERT(!reg.header_dirty, "header_dirty should be false after load");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  9. Header edge cases
 * ═══════════════════════════════════════════════════════════════════ */

static void test_header_empty_name(void) {
    TEST_SECTION("header: empty group_name round-trips");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    fill_test_header(&reg.header);
    reg.header.group_name[0] = '\0';
    gr_header_save(&reg);

    memset(&reg.header, 0xFF, sizeof(reg.header));
    gr_header_load(&reg);
    TEST_ASSERT(reg.header.group_name[0] == '\0', "empty group_name should round-trip");

    teardown_reg(&reg);
}

static void test_header_max_length_name(void) {
    TEST_SECTION("header: maximum length group_name round-trips");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    fill_test_header(&reg.header);
    memset(reg.header.group_name, 'A', GR_MAX_NAME_LEN - 1);
    reg.header.group_name[GR_MAX_NAME_LEN - 1] = '\0';
    gr_header_save(&reg);

    char saved_name[GR_MAX_NAME_LEN];
    memcpy(saved_name, reg.header.group_name, GR_MAX_NAME_LEN);

    memset(&reg.header, 0, sizeof(reg.header));
    gr_header_load(&reg);
    TEST_ASSERT(strcmp(reg.header.group_name, saved_name) == 0,
                "max-length name should round-trip");
    TEST_ASSERT(strlen(reg.header.group_name) == GR_MAX_NAME_LEN - 1,
                "loaded name length should be GR_MAX_NAME_LEN - 1");

    teardown_reg(&reg);
}

static void test_header_zero_retention(void) {
    TEST_SECTION("header: zero retention values (FOREVER) round-trip");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    fill_test_header(&reg.header);
    reg.header.retention.message_retention_ms = GR_RETENTION_FOREVER;
    reg.header.retention.file_retention_ms    = GR_RETENTION_FOREVER;
    reg.header.retention.registrar_max_bytes  = 0;
    gr_header_save(&reg);

    memset(&reg.header, 0xFF, sizeof(reg.header));
    gr_header_load(&reg);
    TEST_ASSERT(reg.header.retention.message_retention_ms == 0,
                "message_retention=FOREVER should round-trip as 0");
    TEST_ASSERT(reg.header.retention.file_retention_ms == 0,
                "file_retention=FOREVER should round-trip as 0");
    TEST_ASSERT(reg.header.retention.registrar_max_bytes == 0,
                "registrar_max_bytes=0 should round-trip");

    teardown_reg(&reg);
}

static void test_header_negative_timestamps(void) {
    TEST_SECTION("header: negative timestamps round-trip");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    fill_test_header(&reg.header);
    reg.header.created_at = -1000000LL;
    reg.header.updated_at = -1LL;
    gr_header_save(&reg);

    memset(&reg.header, 0, sizeof(reg.header));
    gr_header_load(&reg);
    TEST_ASSERT(reg.header.created_at == -1000000LL, "negative created_at should round-trip");
    TEST_ASSERT(reg.header.updated_at == -1LL, "negative updated_at should round-trip");

    teardown_reg(&reg);
}

static void test_header_public_group_type(void) {
    TEST_SECTION("header: GR_GROUP_PUBLIC (1) round-trips");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);

    fill_test_header(&reg.header);
    reg.header.group_type = GR_GROUP_PUBLIC;
    gr_header_save(&reg);

    memset(&reg.header, 0, sizeof(reg.header));
    gr_header_load(&reg);
    TEST_ASSERT(reg.header.group_type == GR_GROUP_PUBLIC,
                "group_type PUBLIC should round-trip");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  10. Cached statement reuse
 * ═══════════════════════════════════════════════════════════════════ */

static void test_cached_save_multiple_times(void) {
    TEST_SECTION("cached header save: reusable across multiple saves");
    gr_registrar_t reg;
    TEST_ASSERT(setup_inmemory_reg(&reg), "setup should succeed");
    gr_db_init_schema(reg.con);
    gr_prepare_statements(&reg);

    for (int i = 0; i < 10; i++) {
        fill_test_header(&reg.header);
        reg.header.version = (uint32_t)(100 + i);
        reg.header.epoch_id = (uint32_t)i;
        char name[GR_MAX_NAME_LEN];
        snprintf(name, sizeof(name), "group-%d", i);
        strncpy(reg.header.group_name, name, GR_MAX_NAME_LEN - 1);

        gr_error_t err = gr_header_save(&reg);
        TEST_ASSERT(err == GR_OK, "cached save should succeed each time");
    }

    memset(&reg.header, 0, sizeof(reg.header));
    gr_header_load(&reg);
    TEST_ASSERT(reg.header.version == 109, "version should be 109 (last)");
    TEST_ASSERT(reg.header.epoch_id == 9, "epoch_id should be 9 (last)");
    TEST_ASSERT(strcmp(reg.header.group_name, "group-9") == 0, "name should be 'group-9'");

    duckdb_result result;
    duckdb_query(reg.con, "SELECT COUNT(*) FROM gr_header", &result);
    int64_t count = 0;
    gr_db_fetch_i64_scalar(&result, &count);
    duckdb_destroy_result(&result);
    TEST_ASSERT(count == 1, "should still be exactly 1 row");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    fprintf(stdout, "═══ db.c test suite ═══\n\n");

    /* 1. DB helpers */
    test_db_exec_success();
    test_db_exec_failure();
    test_db_vec_get_blob();
    test_db_vec_get_blob_length_mismatch();
    test_db_vec_get_blob_null();
    test_db_vec_get_blob_alloc();
    test_db_vec_get_str();
    test_db_vec_get_str_too_small();
    test_db_vec_get_str_null();
    test_db_vec_scalars();
    test_db_fetch_scalar_helpers();

    /* 2. Schema */
    test_schema_init();
    test_schema_init_idempotent();
    test_schema_column_counts();

    /* 3. Header round-trip (fallback) */
    test_header_roundtrip_fallback();

    /* 4. Header round-trip (cached) */
    test_header_roundtrip_cached();

    /* 5. Header field isolation */
    test_header_field_isolation();

    /* 6. Prepared statement lifecycle */
    test_prepare_statements();
    test_prepare_without_schema_fails();
    test_destroy_statements();
    test_destroy_statements_safe_on_not_ready();
    test_destroy_statements_null_safe();
    test_prepare_destroy_cycle();

    /* 7. Header load empty */
    test_header_load_empty();

    /* 8. Overwrite */
    test_header_overwrite();
    test_header_dirty_flag();

    /* 9. Edge cases */
    test_header_empty_name();
    test_header_max_length_name();
    test_header_zero_retention();
    test_header_negative_timestamps();
    test_header_public_group_type();

    /* 10. Cached reuse */
    test_cached_save_multiple_times();

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
