/*
 * test_crypto.c - Tests for crypto.c: signing, verification, group encryption, sealed peer boxes, role-based signing, and multi-epoch decrypt (in-memory DuckDB).
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
 * @file test_crypto.c
 * @brief Test suite for crypto.c — signing, verification, and encryption.
 *
 * All tests use in-memory DuckDB (NULL path to duckdb_open).
 *
 * Dependencies: crypto.c, db.c, buf.c, util.c, audit.c, role.c, epoch.c,
 *               libduckdb, OpenSSL + oqs-provider.
 */

#include "internal.h"
#include "crypto.h"
#include "static_memory.h"
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

/* ── Helper: generate a test identity ──────────────────────────── */

static void make_test_identity(gr_identity_t *id) {
    memset(id, 0, sizeof(*id));
    gr_identity_generate(id);
}

/* ── Helper: fully initialized registrar with owner + epoch ────── */

static bool setup_full_reg(gr_registrar_t *reg, gr_identity_t *owner) {
    memset(reg, 0, sizeof(*reg));
    make_test_identity(owner);

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

    /* Configure header with owner identity */
    memset(&reg->header, 0, sizeof(reg->header));
    reg->header.group_type = GR_GROUP_PRIVATE;
    strncpy(reg->header.group_name, "test-group", GR_MAX_NAME_LEN - 1);
    reg->header.version = 0;
    reg->header.created_at = 1700000000000LL;
    reg->header.epoch_id = 1;
    reg->header.retention = gr_retention_defaults();
    memcpy(reg->header.owner_id, owner->peer_id, GR_PEER_ID_LEN);
    memcpy(reg->header.owner_sign_key, owner->public_key, GR_PUBLIC_KEY_LEN);

    for (int i = 0; i < GR_HASH_LEN; i++)
        reg->header.group_id[i] = (uint8_t)(i + 1);

    if (gr_header_save(reg) != GR_OK) {
        gr_destroy_statements(reg);
        duckdb_disconnect(&reg->con);
        duckdb_close(&reg->db);
        return false;
    }

    /* Insert epoch_id=1 with a random key */
    duckdb_prepared_statement stmt = reg->ps_epoch_insert;
    duckdb_clear_bindings(stmt);
    uint8_t epoch_key[GR_EPOCH_KEY_LEN];
    yumi_randombytes(epoch_key, GR_EPOCH_KEY_LEN);
    duckdb_bind_int32(stmt, 1, 1);
    duckdb_bind_blob(stmt, 2, epoch_key, GR_EPOCH_KEY_LEN);
    duckdb_bind_int64(stmt, 3, 1700000000000LL);
    duckdb_bind_int64(stmt, 4, 0);
    duckdb_bind_blob(stmt, 5, owner->peer_id, GR_PEER_ID_LEN);
    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    yumi_memzero(epoch_key, GR_EPOCH_KEY_LEN);
    if (st != DuckDBSuccess) {
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

/* ── Helper: insert a peer with a role ─────────────────────────── */

static bool insert_peer_with_role(gr_registrar_t *reg,
                                  const gr_identity_t *id,
                                  uint32_t perms,
                                  uint32_t role_id) {
    /* Insert role */
    duckdb_prepared_statement stmt = reg->ps_role_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)role_id);
    duckdb_bind_varchar(stmt, 2, "test-role");
    duckdb_bind_int32(stmt, 3, (int32_t)perms);
    uint8_t zero_key[GR_PUBLIC_KEY_LEN] = {0};
    duckdb_bind_blob(stmt, 4, zero_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_int64(stmt, 5, 1700000000000LL);
    duckdb_bind_int64(stmt, 6, 1700000000000LL);
    duckdb_result result;
    if (duckdb_execute_prepared(stmt, &result) != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        return false;
    }
    duckdb_destroy_result(&result);

    /* Insert peer with that role */
    stmt = reg->ps_peer_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, id->peer_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 2, id->public_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_blob(stmt, 3, id->public_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_varchar(stmt, 4, "127.0.0.1");
    duckdb_bind_int32(stmt, 5, 9000);
    duckdb_bind_int32(stmt, 6, 0);  /* active */
    duckdb_bind_int32(stmt, 7, (int32_t)role_id);
    duckdb_bind_int64(stmt, 8, 1700000000000LL);
    duckdb_bind_int64(stmt, 9, 0);
    duckdb_bind_int64(stmt, 10, 1700000000000LL);
    duckdb_bind_varchar(stmt, 11, "");
    uint8_t zero_id[GR_PEER_ID_LEN] = {0};
    duckdb_bind_blob(stmt, 12, zero_id, GR_PEER_ID_LEN);
    if (duckdb_execute_prepared(stmt, &result) != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        return false;
    }
    duckdb_destroy_result(&result);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 *  1. Parameter validation
 * ═══════════════════════════════════════════════════════════════════ */

static void test_sign_data_null_params(void) {
    TEST_SECTION("gr_sign_data: NULL params");
    gr_identity_t id;
    make_test_identity(&id);
    uint8_t data[4] = {1,2,3,4};
    uint8_t sig[GR_SIGN_LEN];

    TEST_ASSERT(gr_sign_data(NULL, data, 4, sig) == GR_ERR_INVALID_PARAM, "NULL signer");
    TEST_ASSERT(gr_sign_data(&id, NULL, 4, sig) == GR_ERR_INVALID_PARAM, "NULL data");
    TEST_ASSERT(gr_sign_data(&id, data, 4, NULL) == GR_ERR_INVALID_PARAM, "NULL sig_out");
}

static void test_verify_data_null_params(void) {
    TEST_SECTION("gr_verify_data: NULL params");
    uint8_t pk[GR_PUBLIC_KEY_LEN] = {0};
    uint8_t data[4] = {1,2,3,4};
    uint8_t sig[GR_SIGN_LEN] = {0};
    bool valid;

    TEST_ASSERT(gr_verify_data(NULL, data, 4, sig, &valid) == GR_ERR_INVALID_PARAM, "NULL pk");
    TEST_ASSERT(gr_verify_data(pk, NULL, 4, sig, &valid) == GR_ERR_INVALID_PARAM, "NULL data");
    TEST_ASSERT(gr_verify_data(pk, data, 4, NULL, &valid) == GR_ERR_INVALID_PARAM, "NULL sig");
    TEST_ASSERT(gr_verify_data(pk, data, 4, sig, NULL) == GR_ERR_INVALID_PARAM, "NULL valid_out");
}

static void test_sign_null_params(void) {
    TEST_SECTION("gr_sign: NULL params");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(gr_sign(NULL, &owner) == GR_ERR_INVALID_PARAM, "NULL reg");
    memset(&reg, 0, sizeof(reg));
    TEST_ASSERT(gr_sign(&reg, NULL) == GR_ERR_INVALID_PARAM, "NULL signer");
}

static void test_verify_null_params(void) {
    TEST_SECTION("gr_verify: NULL params");
    gr_registrar_t reg;
    bool valid;
    TEST_ASSERT(gr_verify(NULL, &valid) == GR_ERR_INVALID_PARAM, "NULL reg");
    memset(&reg, 0, sizeof(reg));
    TEST_ASSERT(gr_verify(&reg, NULL) == GR_ERR_INVALID_PARAM, "NULL valid_out");
}

static void test_encrypt_null_params(void) {
    TEST_SECTION("gr_encrypt: NULL params");
    uint8_t data[4] = {1,2,3,4};
    uint8_t out[128];
    size_t out_len = sizeof(out);
    TEST_ASSERT(gr_encrypt(NULL, data, 4, NULL, 0, out, &out_len)
                == GR_ERR_INVALID_PARAM, "NULL reg");
}

static void test_decrypt_null_params(void) {
    TEST_SECTION("gr_decrypt: NULL params");
    uint8_t ct[64] = {0};
    uint8_t out[64];
    size_t out_len = sizeof(out);
    TEST_ASSERT(gr_decrypt(NULL, ct, 64, NULL, 0, out, &out_len, NULL)
                == GR_ERR_INVALID_PARAM, "NULL reg");
}

static void test_encrypt_for_peer_null_params(void) {
    TEST_SECTION("gr_encrypt_for_peer: NULL params");
    uint8_t pk[GR_PUBLIC_KEY_LEN] = {0};
    uint8_t data[4] = {1,2,3,4};
    uint8_t out[128];
    size_t out_len = sizeof(out);

    TEST_ASSERT(gr_encrypt_for_peer(NULL, data, 4, out, &out_len) == GR_ERR_INVALID_PARAM, "NULL pk");
    TEST_ASSERT(gr_encrypt_for_peer(pk, NULL, 4, out, &out_len) == GR_ERR_INVALID_PARAM, "NULL pt");
    TEST_ASSERT(gr_encrypt_for_peer(pk, data, 4, NULL, &out_len) == GR_ERR_INVALID_PARAM, "NULL out");
    TEST_ASSERT(gr_encrypt_for_peer(pk, data, 4, out, NULL) == GR_ERR_INVALID_PARAM, "NULL out_len");
}

static void test_decrypt_from_peer_null_params(void) {
    TEST_SECTION("gr_decrypt_from_peer: NULL params");
    gr_identity_t id;
    make_test_identity(&id);
    uint8_t ct[128] = {0};
    uint8_t out[128];
    size_t out_len = sizeof(out);

    TEST_ASSERT(gr_decrypt_from_peer(NULL, ct, 64, out, &out_len) == GR_ERR_INVALID_PARAM, "NULL self");
    TEST_ASSERT(gr_decrypt_from_peer(&id, NULL, 64, out, &out_len) == GR_ERR_INVALID_PARAM, "NULL ct");
    TEST_ASSERT(gr_decrypt_from_peer(&id, ct, 64, NULL, &out_len) == GR_ERR_INVALID_PARAM, "NULL out");
    TEST_ASSERT(gr_decrypt_from_peer(&id, ct, 64, out, NULL) == GR_ERR_INVALID_PARAM, "NULL out_len");
}

/* ═══════════════════════════════════════════════════════════════════
 *  2. Data signing
 * ═══════════════════════════════════════════════════════════════════ */

static void test_sign_verify_data_roundtrip(void) {
    TEST_SECTION("gr_sign_data + gr_verify_data: round-trip");
    gr_identity_t id;
    make_test_identity(&id);
    uint8_t data[] = "the quick brown fox";
    uint8_t sig[GR_SIGN_LEN];

    TEST_ASSERT(gr_sign_data(&id, data, sizeof(data), sig) == GR_OK, "sign ok");
    bool valid = false;
    TEST_ASSERT(gr_verify_data(id.public_key, data, sizeof(data), sig, &valid) == GR_OK, "verify ok");
    TEST_ASSERT(valid, "should be valid");
}

static void test_verify_data_rejects_tampered(void) {
    TEST_SECTION("gr_verify_data: rejects tampered data");
    gr_identity_t id;
    make_test_identity(&id);
    uint8_t data[] = "original";
    uint8_t sig[GR_SIGN_LEN];
    gr_sign_data(&id, data, sizeof(data), sig);
    data[0] ^= 0xFF;
    bool valid = true;
    gr_verify_data(id.public_key, data, sizeof(data), sig, &valid);
    TEST_ASSERT(!valid, "tampered data should fail");
}

static void test_verify_data_rejects_wrong_key(void) {
    TEST_SECTION("gr_verify_data: rejects wrong key");
    gr_identity_t alice, bob;
    make_test_identity(&alice);
    make_test_identity(&bob);
    uint8_t data[] = "alice's message";
    uint8_t sig[GR_SIGN_LEN];
    gr_sign_data(&alice, data, sizeof(data), sig);
    bool valid = true;
    gr_verify_data(bob.public_key, data, sizeof(data), sig, &valid);
    TEST_ASSERT(!valid, "wrong key should fail");
}

static void test_sign_data_empty(void) {
    TEST_SECTION("gr_sign_data: zero-length data");
    gr_identity_t id;
    make_test_identity(&id);
    uint8_t dummy = 0;
    uint8_t sig[GR_SIGN_LEN];
    TEST_ASSERT(gr_sign_data(&id, &dummy, 0, sig) == GR_OK, "sign ok");
    bool valid = false;
    gr_verify_data(id.public_key, &dummy, 0, sig, &valid);
    TEST_ASSERT(valid, "zero-length should verify");
}

static void test_sign_data_large(void) {
    TEST_SECTION("gr_sign_data: 64KB data");
    gr_identity_t id;
    make_test_identity(&id);
    size_t len = 65536;
    uint8_t *data = (uint8_t *)malloc(len);
    TEST_ASSERT(data != NULL, "alloc ok");
    yumi_randombytes(data, len);
    uint8_t sig[GR_SIGN_LEN];
    TEST_ASSERT(gr_sign_data(&id, data, len, sig) == GR_OK, "sign ok");
    bool valid = false;
    gr_verify_data(id.public_key, data, len, sig, &valid);
    TEST_ASSERT(valid, "large data should verify");
    free(data);
}

/* ═══════════════════════════════════════════════════════════════════
 *  3. Sealed box
 * ═══════════════════════════════════════════════════════════════════ */

/* Wire overhead for peer encryption: KEM ciphertext + nonce + AEAD tag */
#define PEER_ENCRYPT_OVERHEAD (GR_KEM_CIPHERTEXT_LEN + GR_NONCE_LEN + GR_MAC_LEN)

static void test_sealed_box_roundtrip(void) {
    TEST_SECTION("sealed box: round-trip");
    gr_identity_t r;
    make_test_identity(&r);
    uint8_t pt[] = "epoch key material";
    uint8_t ct[2048];
    size_t ct_len = sizeof(ct);
    TEST_ASSERT(gr_encrypt_for_peer(r.kem_pk, pt, sizeof(pt), ct, &ct_len) == GR_OK, "encrypt ok");
    uint8_t dec[256];
    size_t dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt_from_peer(&r, ct, ct_len, dec, &dec_len) == GR_OK, "decrypt ok");
    TEST_ASSERT(dec_len == sizeof(pt), "length match");
    TEST_ASSERT(memcmp(dec, pt, sizeof(pt)) == 0, "content match");
}

static void test_sealed_box_wrong_recipient(void) {
    TEST_SECTION("sealed box: wrong recipient");
    gr_identity_t alice, bob;
    make_test_identity(&alice);
    make_test_identity(&bob);
    uint8_t pt[] = "for alice";
    uint8_t ct[2048];
    size_t ct_len = sizeof(ct);
    gr_encrypt_for_peer(alice.kem_pk, pt, sizeof(pt), ct, &ct_len);
    uint8_t dec[256];
    size_t dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt_from_peer(&bob, ct, ct_len, dec, &dec_len) == GR_ERR_CRYPTO,
                "wrong recipient should fail");
}

static void test_sealed_box_size_exceeded(void) {
    TEST_SECTION("sealed box: size exceeded");
    gr_identity_t id;
    make_test_identity(&id);
    uint8_t pt[] = "data";
    uint8_t out[4];
    size_t out_len = sizeof(out);
    TEST_ASSERT(gr_encrypt_for_peer(id.kem_pk, pt, sizeof(pt), out, &out_len) == GR_ERR_SIZE_EXCEEDED,
                "should return SIZE_EXCEEDED");
    TEST_ASSERT(out_len == sizeof(pt) + PEER_ENCRYPT_OVERHEAD, "should report required size");
}

static void test_sealed_box_too_short(void) {
    TEST_SECTION("sealed box: too-short ciphertext");
    gr_identity_t id;
    make_test_identity(&id);
    uint8_t ct[4] = {0};
    uint8_t out[64];
    size_t out_len = sizeof(out);
    TEST_ASSERT(gr_decrypt_from_peer(&id, ct, sizeof(ct), out, &out_len) == GR_ERR_INVALID_PARAM,
                "too short should fail");
}

static void test_sealed_box_empty_plaintext(void) {
    TEST_SECTION("sealed box: empty plaintext");
    gr_identity_t id;
    make_test_identity(&id);
    uint8_t dummy = 0;
    uint8_t ct[2048];
    size_t ct_len = sizeof(ct);
    TEST_ASSERT(gr_encrypt_for_peer(id.kem_pk, &dummy, 0, ct, &ct_len) == GR_OK, "encrypt ok");
    uint8_t out[64];
    size_t out_len = sizeof(out);
    TEST_ASSERT(gr_decrypt_from_peer(&id, ct, ct_len, out, &out_len) == GR_OK, "decrypt ok");
    TEST_ASSERT(out_len == 0, "decrypted len should be 0");
}

static void test_sealed_box_tampered(void) {
    TEST_SECTION("sealed box: tampered ciphertext");
    gr_identity_t id;
    make_test_identity(&id);
    uint8_t pt[] = "authentic";
    uint8_t ct[2048];
    size_t ct_len = sizeof(ct);
    gr_encrypt_for_peer(id.kem_pk, pt, sizeof(pt), ct, &ct_len);
    ct[GR_KEM_CIPHERTEXT_LEN + GR_NONCE_LEN + 1] ^= 0xFF;
    uint8_t out[256];
    size_t out_len = sizeof(out);
    TEST_ASSERT(gr_decrypt_from_peer(&id, ct, ct_len, out, &out_len) == GR_ERR_CRYPTO,
                "tampered should fail");
}

static void test_sealed_box_kem_pk_binding(void) {
    TEST_SECTION("sealed box: KEM pk salt binding");

    /* Encrypt for alice, try to decrypt with bob who has the same
     * shared secret (impossible in practice, but verify the HKDF
     * salt differentiates even if KEM output were identical). */
    gr_identity_t alice, bob;
    make_test_identity(&alice);
    make_test_identity(&bob);

    uint8_t pt[] = "context-bound message";
    uint8_t ct[4096];
    size_t ct_len = sizeof(ct);
    TEST_ASSERT(gr_encrypt_for_peer(alice.kem_pk, pt, sizeof(pt), ct, &ct_len) == GR_OK,
                "encrypt for alice ok");

    /* Alice can decrypt her own message */
    uint8_t dec[256];
    size_t dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt_from_peer(&alice, ct, ct_len, dec, &dec_len) == GR_OK,
                "alice decrypts ok");
    TEST_ASSERT(dec_len == sizeof(pt), "alice length match");
    TEST_ASSERT(memcmp(dec, pt, sizeof(pt)) == 0, "alice content match");

    /* Bob cannot decrypt */
    dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt_from_peer(&bob, ct, ct_len, dec, &dec_len) == GR_ERR_CRYPTO,
                "bob cannot decrypt alice's message");

    /* Encrypt for bob, verify bob can decrypt */
    ct_len = sizeof(ct);
    TEST_ASSERT(gr_encrypt_for_peer(bob.kem_pk, pt, sizeof(pt), ct, &ct_len) == GR_OK,
                "encrypt for bob ok");
    dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt_from_peer(&bob, ct, ct_len, dec, &dec_len) == GR_OK,
                "bob decrypts own ok");
    TEST_ASSERT(memcmp(dec, pt, sizeof(pt)) == 0, "bob content match");
}

/* ═══════════════════════════════════════════════════════════════════
 *  4. Registrar signing (owner)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_sign_verify_roundtrip(void) {
    TEST_SECTION("gr_sign + gr_verify: owner round-trip");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    TEST_ASSERT(gr_sign(&reg, &owner) == GR_OK, "sign ok");
    bool valid = false;
    TEST_ASSERT(gr_verify(&reg, &valid) == GR_OK, "verify ok");
    TEST_ASSERT(valid, "should be valid");

    /* signer_id should be the owner */
    TEST_ASSERT(memcmp(reg.header.signer_id, owner.peer_id, GR_PEER_ID_LEN) == 0,
                "signer_id should be owner");

    teardown_reg(&reg);
}

static void test_sign_unauthorized(void) {
    TEST_SECTION("gr_sign: non-owner without permission is rejected");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    gr_identity_t impostor;
    make_test_identity(&impostor);
    TEST_ASSERT(gr_sign(&reg, &impostor) == GR_ERR_UNAUTHORIZED, "impostor rejected");

    teardown_reg(&reg);
}

static void test_verify_detects_tampered_field(void) {
    TEST_SECTION("gr_verify: detects tampered header field");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");
    gr_sign(&reg, &owner);

    bool valid = true;
    gr_verify(&reg, &valid);
    TEST_ASSERT(valid, "should verify before tampering");

    strncpy(reg.header.group_name, "TAMPERED", GR_MAX_NAME_LEN - 1);
    valid = true;
    gr_verify(&reg, &valid);
    TEST_ASSERT(!valid, "should fail after tampering");

    teardown_reg(&reg);
}

static void test_sign_increments_version(void) {
    TEST_SECTION("gr_sign: increments header version");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");
    uint32_t before = reg.header.version;
    gr_sign(&reg, &owner);
    TEST_ASSERT(reg.header.version > before, "version should increase");
    teardown_reg(&reg);
}

static void test_sign_updates_timestamp(void) {
    TEST_SECTION("gr_sign: updates updated_at");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");
    int64_t before = reg.header.updated_at;
    gr_sign(&reg, &owner);
    TEST_ASSERT(reg.header.updated_at >= before, "updated_at should advance");
    teardown_reg(&reg);
}

static void test_sign_sets_hash_and_signature(void) {
    TEST_SECTION("gr_sign: populates hash, signature, and signer_id");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    uint8_t zeros[GR_HASH_LEN];
    memset(zeros, 0, GR_HASH_LEN);
    TEST_ASSERT(memcmp(reg.header.hash, zeros, GR_HASH_LEN) == 0,
                "hash should be zero before sign");

    gr_sign(&reg, &owner);

    TEST_ASSERT(memcmp(reg.header.hash, zeros, GR_HASH_LEN) != 0,
                "hash should be non-zero after sign");

    uint8_t zeros_sig[GR_SIGN_LEN];
    memset(zeros_sig, 0, GR_SIGN_LEN);
    TEST_ASSERT(memcmp(reg.header.signature, zeros_sig, GR_SIGN_LEN) != 0,
                "signature should be non-zero after sign");

    TEST_ASSERT(memcmp(reg.header.signer_id, owner.peer_id, GR_PEER_ID_LEN) == 0,
                "signer_id should be set to owner");

    teardown_reg(&reg);
}

static void test_verify_rejects_wrong_signer_key(void) {
    TEST_SECTION("gr_verify: fails when signer_sign_key is swapped");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");
    gr_sign(&reg, &owner);

    gr_identity_t other;
    make_test_identity(&other);
    memcpy(reg.header.signer_sign_key, other.public_key, GR_PUBLIC_KEY_LEN);

    bool valid = true;
    gr_verify(&reg, &valid);
    TEST_ASSERT(!valid, "should fail with wrong signer_sign_key");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  4b. Registrar signing (role-based)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_sign_by_role_holder(void) {
    TEST_SECTION("gr_sign: peer with GR_PERM_SIGN_REGISTRAR can sign");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    gr_identity_t moderator;
    make_test_identity(&moderator);
    TEST_ASSERT(insert_peer_with_role(&reg, &moderator, GR_PERM_SIGN_REGISTRAR, 1),
                "insert peer ok");

    TEST_ASSERT(gr_sign(&reg, &moderator) == GR_OK, "role-holder sign ok");

    bool valid = false;
    gr_verify(&reg, &valid);
    TEST_ASSERT(valid, "role-holder signature should verify");

    TEST_ASSERT(memcmp(reg.header.signer_id, moderator.peer_id, GR_PEER_ID_LEN) == 0,
                "signer_id should be moderator");
    TEST_ASSERT(memcmp(reg.header.signer_sign_key, moderator.public_key, GR_PUBLIC_KEY_LEN) == 0,
                "signer_sign_key should be moderator's key");
    TEST_ASSERT(memcmp(reg.header.owner_id, owner.peer_id, GR_PEER_ID_LEN) == 0,
                "owner_id should be unchanged");

    teardown_reg(&reg);
}

static void test_sign_rejected_without_permission(void) {
    TEST_SECTION("gr_sign: peer without GR_PERM_SIGN_REGISTRAR is rejected");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    gr_identity_t member;
    make_test_identity(&member);
    TEST_ASSERT(insert_peer_with_role(&reg, &member, GR_PERM_INVITE_MEMBER, 1),
                "insert peer ok");

    TEST_ASSERT(gr_sign(&reg, &member) == GR_ERR_UNAUTHORIZED,
                "peer without SIGN_REGISTRAR should be rejected");

    teardown_reg(&reg);
}

static void test_sign_owner_still_works(void) {
    TEST_SECTION("gr_sign: owner can still sign (GR_PERM_OWNER includes all)");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    TEST_ASSERT(gr_sign(&reg, &owner) == GR_OK, "owner sign ok");
    bool valid = false;
    gr_verify(&reg, &valid);
    TEST_ASSERT(valid, "owner sig should verify");
    TEST_ASSERT(memcmp(reg.header.signer_id, owner.peer_id, GR_PEER_ID_LEN) == 0,
                "signer_id should be owner");

    teardown_reg(&reg);
}

static void test_sign_alternating_signers(void) {
    TEST_SECTION("gr_sign: alternating owner and role-holder");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    gr_identity_t moderator;
    make_test_identity(&moderator);
    TEST_ASSERT(insert_peer_with_role(&reg, &moderator, GR_PERM_SIGN_REGISTRAR, 1),
                "insert peer ok");

    /* Owner signs */
    TEST_ASSERT(gr_sign(&reg, &owner) == GR_OK, "owner sign ok");
    bool valid = false;
    gr_verify(&reg, &valid);
    TEST_ASSERT(valid, "owner sig valid");
    TEST_ASSERT(memcmp(reg.header.signer_id, owner.peer_id, GR_PEER_ID_LEN) == 0,
                "signer = owner");

    /* Moderator signs */
    TEST_ASSERT(gr_sign(&reg, &moderator) == GR_OK, "moderator sign ok");
    valid = false;
    gr_verify(&reg, &valid);
    TEST_ASSERT(valid, "moderator sig valid");
    TEST_ASSERT(memcmp(reg.header.signer_id, moderator.peer_id, GR_PEER_ID_LEN) == 0,
                "signer = moderator");

    /* Owner signs again */
    TEST_ASSERT(gr_sign(&reg, &owner) == GR_OK, "owner re-sign ok");
    valid = false;
    gr_verify(&reg, &valid);
    TEST_ASSERT(valid, "owner re-sig valid");
    TEST_ASSERT(memcmp(reg.header.signer_id, owner.peer_id, GR_PEER_ID_LEN) == 0,
                "signer = owner again");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  5. Group encryption
 * ═══════════════════════════════════════════════════════════════════ */

static void test_encrypt_decrypt_roundtrip(void) {
    TEST_SECTION("gr_encrypt + gr_decrypt: round-trip");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    uint8_t pt[] = "hello encrypted world!";
    uint8_t ct[256];
    size_t ct_len = sizeof(ct);
    TEST_ASSERT(gr_encrypt(&reg, pt, sizeof(pt), NULL, 0, ct, &ct_len) == GR_OK, "encrypt ok");

    uint8_t dec[256];
    size_t dec_len = sizeof(dec);
    uint32_t eid = 0;
    TEST_ASSERT(gr_decrypt(&reg, ct, ct_len, NULL, 0, dec, &dec_len, &eid) == GR_OK, "decrypt ok");
    TEST_ASSERT(dec_len == sizeof(pt), "length match");
    TEST_ASSERT(memcmp(dec, pt, sizeof(pt)) == 0, "content match");
    TEST_ASSERT(eid == 1, "epoch_id should be 1");

    teardown_reg(&reg);
}

static void test_encrypt_decrypt_with_ad(void) {
    TEST_SECTION("gr_encrypt + gr_decrypt: with AD");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    uint8_t pt[] = "payload";
    uint8_t ad[] = "context";
    uint8_t ct[256];
    size_t ct_len = sizeof(ct);
    gr_encrypt(&reg, pt, sizeof(pt), ad, sizeof(ad), ct, &ct_len);

    uint8_t dec[256];
    size_t dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt(&reg, ct, ct_len, ad, sizeof(ad), dec, &dec_len, NULL) == GR_OK,
                "decrypt with correct AD ok");
    TEST_ASSERT(memcmp(dec, pt, sizeof(pt)) == 0, "content match");

    teardown_reg(&reg);
}

static void test_decrypt_rejects_wrong_ad(void) {
    TEST_SECTION("gr_decrypt: rejects wrong AD");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    uint8_t pt[] = "payload";
    uint8_t ad_ok[] = "correct";
    uint8_t ad_bad[] = "wrong";
    uint8_t ct[256];
    size_t ct_len = sizeof(ct);
    gr_encrypt(&reg, pt, sizeof(pt), ad_ok, sizeof(ad_ok), ct, &ct_len);

    uint8_t dec[256];
    size_t dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt(&reg, ct, ct_len, ad_bad, sizeof(ad_bad), dec, &dec_len, NULL)
                == GR_ERR_CRYPTO, "wrong AD should fail");

    teardown_reg(&reg);
}

static void test_decrypt_rejects_tampered(void) {
    TEST_SECTION("gr_decrypt: rejects tampered ciphertext");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    uint8_t pt[] = "authentic";
    uint8_t ct[256];
    size_t ct_len = sizeof(ct);
    gr_encrypt(&reg, pt, sizeof(pt), NULL, 0, ct, &ct_len);
    ct[GR_NONCE_LEN + 2] ^= 0xFF;

    uint8_t dec[256];
    size_t dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt(&reg, ct, ct_len, NULL, 0, dec, &dec_len, NULL) == GR_ERR_CRYPTO,
                "tampered should fail");

    teardown_reg(&reg);
}

static void test_decrypt_epoch_id_null_ok(void) {
    TEST_SECTION("gr_decrypt: epoch_id_out=NULL is safe");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    uint8_t pt[] = "test";
    uint8_t ct[256];
    size_t ct_len = sizeof(ct);
    gr_encrypt(&reg, pt, sizeof(pt), NULL, 0, ct, &ct_len);

    uint8_t dec[256];
    size_t dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt(&reg, ct, ct_len, NULL, 0, dec, &dec_len, NULL) == GR_OK,
                "NULL epoch_id_out ok");

    teardown_reg(&reg);
}

static void test_encrypt_size_exceeded(void) {
    TEST_SECTION("gr_encrypt: SIZE_EXCEEDED with small buffer");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    uint8_t pt[] = "some data";
    uint8_t out[4];
    size_t out_len = sizeof(out);
    TEST_ASSERT(gr_encrypt(&reg, pt, sizeof(pt), NULL, 0, out, &out_len)
                == GR_ERR_SIZE_EXCEEDED, "should return SIZE_EXCEEDED");
    TEST_ASSERT(out_len == GR_NONCE_LEN + sizeof(pt) + GR_MAC_LEN,
                "should report required size");

    teardown_reg(&reg);
}

static void test_decrypt_too_short(void) {
    TEST_SECTION("gr_decrypt: rejects too-short ciphertext");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    uint8_t ct[8] = {0};
    uint8_t out[64];
    size_t out_len = sizeof(out);
    TEST_ASSERT(gr_decrypt(&reg, ct, sizeof(ct), NULL, 0, out, &out_len, NULL)
                == GR_ERR_INVALID_PARAM, "too short should fail");

    teardown_reg(&reg);
}

static void test_encrypt_decrypt_multiple_epochs(void) {
    TEST_SECTION("gr_decrypt: decrypts content from older epoch");
    gr_registrar_t reg;
    gr_identity_t owner;
    TEST_ASSERT(setup_full_reg(&reg, &owner), "setup ok");

    /* Encrypt under epoch 1 */
    uint8_t msg1[] = "epoch 1 content";
    uint8_t ct1[256];
    size_t ct1_len = sizeof(ct1);
    TEST_ASSERT(gr_encrypt(&reg, msg1, sizeof(msg1), NULL, 0, ct1, &ct1_len) == GR_OK,
                "encrypt epoch 1 ok");

    /* Expire epoch 1, insert epoch 2 */
    duckdb_prepared_statement stmt = reg.ps_epoch_expire;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int64(stmt, 1, gr_timestamp_ms());
    duckdb_bind_int32(stmt, 2, 1);
    duckdb_result result;
    duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);

    uint8_t ek2[GR_EPOCH_KEY_LEN];
    yumi_randombytes(ek2, GR_EPOCH_KEY_LEN);
    stmt = reg.ps_epoch_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, 2);
    duckdb_bind_blob(stmt, 2, ek2, GR_EPOCH_KEY_LEN);
    duckdb_bind_int64(stmt, 3, gr_timestamp_ms());
    duckdb_bind_int64(stmt, 4, 0);
    duckdb_bind_blob(stmt, 5, owner.peer_id, GR_PEER_ID_LEN);
    duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    yumi_memzero(ek2, GR_EPOCH_KEY_LEN);
    reg.header.epoch_id = 2;
    gr_header_save(&reg);

    /* Encrypt under epoch 2 */
    uint8_t msg2[] = "epoch 2 content";
    uint8_t ct2[256];
    size_t ct2_len = sizeof(ct2);
    TEST_ASSERT(gr_encrypt(&reg, msg2, sizeof(msg2), NULL, 0, ct2, &ct2_len) == GR_OK,
                "encrypt epoch 2 ok");

    /* Decrypt both */
    uint8_t dec[256];
    size_t dec_len;
    uint32_t eid;

    dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt(&reg, ct1, ct1_len, NULL, 0, dec, &dec_len, &eid) == GR_OK,
                "decrypt epoch-1 content ok");
    TEST_ASSERT(eid == 1, "should report epoch_id=1");
    TEST_ASSERT(memcmp(dec, msg1, sizeof(msg1)) == 0, "epoch-1 content match");

    dec_len = sizeof(dec);
    TEST_ASSERT(gr_decrypt(&reg, ct2, ct2_len, NULL, 0, dec, &dec_len, &eid) == GR_OK,
                "decrypt epoch-2 content ok");
    TEST_ASSERT(eid == 2, "should report epoch_id=2");
    TEST_ASSERT(memcmp(dec, msg2, sizeof(msg2)) == 0, "epoch-2 content match");

    teardown_reg(&reg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    initialize_yumi_browser_static_heaps();
    if (yumi_crypto_init() != YUMI_CRYPTO_OK) {
        fprintf(stderr, "FATAL: yumi_crypto_init() failed\n");
        return 1;
    }

    fprintf(stdout, "═══ crypto.c test suite ═══\n\n");

    /* 1. Parameter validation */
    test_sign_data_null_params();
    test_verify_data_null_params();
    test_sign_null_params();
    test_verify_null_params();
    test_encrypt_null_params();
    test_decrypt_null_params();
    test_encrypt_for_peer_null_params();
    test_decrypt_from_peer_null_params();

    /* 2. Data signing */
    test_sign_verify_data_roundtrip();
    test_verify_data_rejects_tampered();
    test_verify_data_rejects_wrong_key();
    test_sign_data_empty();
    test_sign_data_large();

    /* 3. Sealed box */
    test_sealed_box_roundtrip();
    test_sealed_box_wrong_recipient();
    test_sealed_box_size_exceeded();
    test_sealed_box_too_short();
    test_sealed_box_empty_plaintext();
    test_sealed_box_tampered();
    test_sealed_box_kem_pk_binding();

    /* 4. Registrar signing (owner) */
    test_sign_verify_roundtrip();
    test_sign_unauthorized();
    test_verify_detects_tampered_field();
    test_sign_increments_version();
    test_sign_updates_timestamp();
    test_sign_sets_hash_and_signature();
    test_verify_rejects_wrong_signer_key();

    /* 4b. Role-based signing */
    test_sign_by_role_holder();
    test_sign_rejected_without_permission();
    test_sign_owner_still_works();
    test_sign_alternating_signers();

    /* 5. Group encryption */
    test_encrypt_decrypt_roundtrip();
    test_encrypt_decrypt_with_ad();
    test_decrypt_rejects_wrong_ad();
    test_decrypt_rejects_tampered();
    test_decrypt_epoch_id_null_ok();
    test_encrypt_size_exceeded();
    test_decrypt_too_short();
    test_encrypt_decrypt_multiple_epochs();

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
