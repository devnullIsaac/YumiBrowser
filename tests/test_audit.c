/*
    Yumi Tests — Group Registrar Audit Log Test Suite
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/**
 * @file test_audit.c
 * @brief Test suite for audit.c — append, list, count, field verification,
 *        version tracking, timestamp filtering, and edge cases.
 *
 * All tests use in-memory DuckDB (NULL path to duckdb_open).
 *
 * Dependencies: audit.c, db.c, buf.c, util.c, libduckdb, OpenSSL + oqs-provider.
 */

#include "internal.h"
#include "crypto.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/* ── Helpers ───────────────────────────────────────────────────── */

static bool setup_full_reg(gr_registrar_t *reg) {
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
    if (!gr_db_init_schema(reg->con)) {
        duckdb_disconnect(&reg->con);
        duckdb_close(&reg->db);
        return false;
    }
    if (gr_prepare_statements(reg) != GR_OK) {
        duckdb_disconnect(&reg->con);
        duckdb_close(&reg->db);
        return false;
    }
    memset(&reg->header, 0, sizeof(reg->header));
    reg->header.version = 0;
    reg->header.created_at = 1700000000000LL;
    if (gr_header_save(reg) != GR_OK) {
        gr_destroy_statements(reg);
        duckdb_disconnect(&reg->con);
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

static void make_test_identity(gr_identity_t *id) {
    memset(id, 0, sizeof(*id));
    gr_identity_generate(id);
}

static bool is_all_zero(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0) return false;
    }
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════════════ */

static void test_audit_list_null_params(void) {
    TEST_SECTION("gr_audit_list: NULL params");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_audit_entry_t e[1];
    uint32_t count = 0;
    TEST_ASSERT(gr_audit_list(NULL, 0, e, 1, &count) == GR_ERR_INVALID_PARAM, "NULL reg");
    TEST_ASSERT(gr_audit_list(&reg, 0, NULL, 1, &count) == GR_ERR_INVALID_PARAM, "NULL out");
    TEST_ASSERT(gr_audit_list(&reg, 0, e, 1, NULL) == GR_ERR_INVALID_PARAM, "NULL count");
    teardown_reg(&reg);
}

static void test_audit_count_null_params(void) {
    TEST_SECTION("gr_audit_count: NULL params");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    uint32_t count = 0;
    TEST_ASSERT(gr_audit_count(NULL, &count) == GR_ERR_INVALID_PARAM, "NULL reg");
    TEST_ASSERT(gr_audit_count(&reg, NULL) == GR_ERR_INVALID_PARAM, "NULL out");
    teardown_reg(&reg);
}

static void test_append_single_and_count(void) {
    TEST_SECTION("gr_audit_append: single append, count=1");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    uint8_t target[GR_PEER_ID_LEN];
    memset(target, 0x42, GR_PEER_ID_LEN);

    TEST_ASSERT(gr_audit_append(&reg, (gr_change_type_t)1, &actor, target, "added") == GR_OK, "append ok");
    uint32_t count = 0;
    gr_audit_count(&reg, &count);
    TEST_ASSERT(count == 1, "count should be 1");
    teardown_reg(&reg);
}

static void test_append_field_roundtrip(void) {
    TEST_SECTION("gr_audit_append: field round-trip");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    uint8_t target[GR_PEER_ID_LEN];
    memset(target, 0xAB, GR_PEER_ID_LEN);

    gr_audit_append(&reg, (gr_change_type_t)3, &actor, target, "role changed");
    gr_audit_entry_t e[1];
    uint32_t count = 0;
    gr_audit_list(&reg, 0, e, 1, &count);
    TEST_ASSERT(count == 1, "should have 1 entry");
    TEST_ASSERT(!is_all_zero(e[0].entry_hash, GR_HASH_LEN), "hash non-zero");
    TEST_ASSERT(e[0].timestamp > 1577836800000LL, "timestamp recent");
    TEST_ASSERT((int)e[0].change_type == 3, "change_type=3");
    TEST_ASSERT(memcmp(e[0].actor_id, actor.peer_id, GR_PEER_ID_LEN) == 0, "actor_id match");

    uint8_t exp_target[GR_PEER_ID_LEN];
    memset(exp_target, 0xAB, GR_PEER_ID_LEN);
    TEST_ASSERT(memcmp(e[0].target_id, exp_target, GR_PEER_ID_LEN) == 0, "target_id match");
    TEST_ASSERT(!is_all_zero(e[0].signature, GR_SIGN_LEN), "sig non-zero");
    TEST_ASSERT(e[0].registrar_version == 1, "version=1");
    TEST_ASSERT(strcmp(e[0].detail, "role changed") == 0, "detail match");

    bool sig_valid = false;
    gr_verify_data(actor.public_key, e[0].entry_hash,
                   GR_HASH_LEN, e[0].signature, &sig_valid);
    TEST_ASSERT(sig_valid, "signature verifies");
    teardown_reg(&reg);
}

static void test_version_incrementing(void) {
    TEST_SECTION("gr_audit_append: version increments");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);

    for (int i = 1; i <= 5; i++) {
        gr_audit_append(&reg, (gr_change_type_t)1, &actor, NULL, "change");
        TEST_ASSERT(reg.header.version == (uint32_t)i, "version should match");
    }
    uint32_t saved = reg.header.version;
    memset(&reg.header, 0, sizeof(reg.header));
    gr_header_load(&reg);
    TEST_ASSERT(reg.header.version == saved, "version persists");
    teardown_reg(&reg);
}

static void test_updated_at_tracking(void) {
    TEST_SECTION("gr_audit_append: updated_at advances");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    int64_t before = reg.header.updated_at;
    gr_audit_append(&reg, (gr_change_type_t)1, &actor, NULL, "first");
    TEST_ASSERT(reg.header.updated_at > before, "updated_at should advance");
    teardown_reg(&reg);
}

static void test_null_target_id(void) {
    TEST_SECTION("gr_audit_append: NULL target_id -> zeroed");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    gr_audit_append(&reg, (gr_change_type_t)1, &actor, NULL, "no target");
    gr_audit_entry_t e[1];
    uint32_t count = 0;
    gr_audit_list(&reg, 0, e, 1, &count);
    TEST_ASSERT(is_all_zero(e[0].target_id, GR_PEER_ID_LEN), "target should be zero");
    teardown_reg(&reg);
}

static void test_null_detail(void) {
    TEST_SECTION("gr_audit_append: NULL detail -> empty string");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    gr_audit_append(&reg, (gr_change_type_t)1, &actor, NULL, NULL);
    gr_audit_entry_t e[1];
    uint32_t count = 0;
    gr_audit_list(&reg, 0, e, 1, &count);
    TEST_ASSERT(e[0].detail[0] == '\0', "detail should be empty");
    teardown_reg(&reg);
}

static void test_timestamp_filtering(void) {
    TEST_SECTION("gr_audit_list: timestamp filtering");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);

    int64_t ts[3];
    for (int i = 0; i < 3; i++) {
        gr_audit_append(&reg, (gr_change_type_t)(i+1), &actor, NULL, "entry");
        gr_audit_entry_t tmp[3];
        uint32_t n = 0;
        gr_audit_list(&reg, 0, tmp, 3, &n);
        ts[i] = tmp[n-1].timestamp;
        usleep(2000);
    }

    gr_audit_entry_t r[3];
    uint32_t count = 0;
    gr_audit_list(&reg, ts[0], r, 3, &count);
    TEST_ASSERT(count == 2, "2 entries after first ts");
    gr_audit_list(&reg, ts[1], r, 3, &count);
    TEST_ASSERT(count == 1, "1 entry after second ts");
    gr_audit_list(&reg, ts[2], r, 3, &count);
    TEST_ASSERT(count == 0, "0 entries after last ts");

    teardown_reg(&reg);
}

static void test_list_ordering(void) {
    TEST_SECTION("gr_audit_list: ascending order");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    for (int i = 0; i < 5; i++) {
        gr_audit_append(&reg, (gr_change_type_t)1, &actor, NULL, "event");
        usleep(2000);
    }
    gr_audit_entry_t e[5];
    uint32_t count = 0;
    gr_audit_list(&reg, 0, e, 5, &count);
    TEST_ASSERT(count == 5, "should have 5");
    for (uint32_t i = 1; i < count; i++) {
        TEST_ASSERT(e[i].timestamp >= e[i-1].timestamp, "timestamps non-decreasing");
        TEST_ASSERT(e[i].registrar_version > e[i-1].registrar_version, "versions increasing");
    }
    teardown_reg(&reg);
}

static void test_list_max_count(void) {
    TEST_SECTION("gr_audit_list: max_count limits");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    for (int i = 0; i < 5; i++) {
        gr_audit_append(&reg, (gr_change_type_t)1, &actor, NULL, "entry");
        usleep(1000);
    }
    gr_audit_entry_t e[5];
    uint32_t count = 0;
    gr_audit_list(&reg, 0, e, 3, &count);
    TEST_ASSERT(count == 3, "should return 3");
    gr_audit_list(&reg, 0, e, 1, &count);
    TEST_ASSERT(count == 1, "should return 1");
    gr_audit_list(&reg, 0, e, 0, &count);
    TEST_ASSERT(count == 0, "should return 0");
    teardown_reg(&reg);
}

static void test_count_accuracy(void) {
    TEST_SECTION("gr_audit_count: accuracy");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    uint32_t count = 99;
    gr_audit_count(&reg, &count);
    TEST_ASSERT(count == 0, "starts at 0");
    for (int i = 1; i <= 7; i++) {
        gr_audit_append(&reg, (gr_change_type_t)(i%4), &actor, NULL, "action");
        gr_audit_count(&reg, &count);
        TEST_ASSERT(count == (uint32_t)i, "count should match appends");
    }
    teardown_reg(&reg);
}

static void test_detail_truncation(void) {
    TEST_SECTION("gr_audit_append: long detail truncated");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    char long_detail[512];
    memset(long_detail, 'X', sizeof(long_detail) - 1);
    long_detail[sizeof(long_detail) - 1] = '\0';
    TEST_ASSERT(gr_audit_append(&reg, (gr_change_type_t)1, &actor, NULL, long_detail) == GR_OK,
                "append ok");
    gr_audit_entry_t e[1];
    uint32_t count = 0;
    gr_audit_list(&reg, 0, e, 1, &count);
    TEST_ASSERT(strlen(e[0].detail) > 0, "detail non-empty");
    TEST_ASSERT(strlen(e[0].detail) < sizeof(long_detail), "detail shorter than input");
    teardown_reg(&reg);
}

static void test_list_empty(void) {
    TEST_SECTION("gr_audit_list: empty log returns 0");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_audit_entry_t e[4];
    uint32_t count = 99;
    TEST_ASSERT(gr_audit_list(&reg, 0, e, 4, &count) == GR_OK, "list ok");
    TEST_ASSERT(count == 0, "count=0");
    teardown_reg(&reg);
}

static void test_multiple_actors(void) {
    TEST_SECTION("gr_audit_append: distinct actors");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t alice, bob;
    make_test_identity(&alice);
    make_test_identity(&bob);
    gr_audit_append(&reg, (gr_change_type_t)1, &alice, NULL, "alice");
    usleep(1000);
    gr_audit_append(&reg, (gr_change_type_t)1, &bob, NULL, "bob");
    gr_audit_entry_t e[2];
    uint32_t count = 0;
    gr_audit_list(&reg, 0, e, 2, &count);
    TEST_ASSERT(count == 2, "should have 2");
    TEST_ASSERT(memcmp(e[0].actor_id, e[1].actor_id, GR_PEER_ID_LEN) != 0, "actors differ");
    TEST_ASSERT(memcmp(e[0].entry_hash, e[1].entry_hash, GR_HASH_LEN) != 0, "hashes differ");

    bool a_valid = false, b_valid = false;
    gr_verify_data(alice.public_key, e[0].entry_hash, GR_HASH_LEN, e[0].signature, &a_valid);
    gr_verify_data(bob.public_key, e[1].entry_hash, GR_HASH_LEN, e[1].signature, &b_valid);
    TEST_ASSERT(a_valid, "alice sig verifies");
    TEST_ASSERT(b_valid, "bob sig verifies");

    bool cross_valid = true;
    gr_verify_data(bob.public_key, e[0].entry_hash, GR_HASH_LEN, e[0].signature, &cross_valid);
    TEST_ASSERT(!cross_valid, "cross-verify should fail");
    teardown_reg(&reg);
}

static void test_signature_integrity(void) {
    TEST_SECTION("gr_audit_append: signature integrity");
    gr_registrar_t reg;
    TEST_ASSERT(setup_full_reg(&reg), "setup ok");
    gr_identity_t actor;
    make_test_identity(&actor);
    gr_audit_append(&reg, (gr_change_type_t)5, &actor, NULL, "integrity");
    gr_audit_entry_t e[1];
    uint32_t count = 0;
    gr_audit_list(&reg, 0, e, 1, &count);

    bool ok_valid = false;
    gr_verify_data(actor.public_key, e[0].entry_hash, GR_HASH_LEN, e[0].signature, &ok_valid);
    TEST_ASSERT(ok_valid, "sig verifies");

    uint8_t tampered[GR_HASH_LEN];
    memcpy(tampered, e[0].entry_hash, GR_HASH_LEN);
    tampered[0] ^= 0xFF;
    bool bad_valid = true;
    gr_verify_data(actor.public_key, tampered, GR_HASH_LEN, e[0].signature, &bad_valid);
    TEST_ASSERT(!bad_valid, "tampered hash fails");
    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    if (yumi_crypto_init() != YUMI_CRYPTO_OK) {
        fprintf(stderr, "FATAL: yumi_crypto_init() failed\n");
        return 1;
    }

    fprintf(stdout, "═══ audit.c test suite ═══\n\n");

    test_audit_list_null_params();
    test_audit_count_null_params();
    test_append_single_and_count();
    test_append_field_roundtrip();
    test_version_incrementing();
    test_updated_at_tracking();
    test_null_target_id();
    test_null_detail();
    test_timestamp_filtering();
    test_list_ordering();
    test_list_max_count();
    test_count_accuracy();
    test_detail_truncation();
    test_list_empty();
    test_multiple_actors();
    test_signature_integrity();

    fprintf(stdout, "\n═══ Results: %d/%d passed ═══\n",
            g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        fprintf(stderr, "\n%d test(s) FAILED\n", g_tests_failed);
        return 1;
    }

    fprintf(stdout, "All tests passed.\n");
    return 0;
}
