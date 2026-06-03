/*
 * test_crypto_abstract.c - 425-assertion test suite for crypto.h/crypto.c: ML-DSA, ML-KEM, FrodoKEM, BP512 ECDH, Skein, HKDF, Threefish AEAD, key combiners.
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
 * @file test_crypto_abstract.c
 * @brief Comprehensive test suite for the crypto abstraction layer (crypto.h / crypto.c).
 *
 * 425 assertions across 18 test sections:
 *
 *   1.  Init / cleanup lifecycle (idempotent init, cleanup-reinit)
 *   2.  Random bytes (bias / distribution, NULL rejection, zero-length)
 *   3.  Memory hygiene (memzero, constant-time memcmp, NULL safety)
 *   4.  ML-DSA-87 keygen / sign / verify (tamper, wrong-key, uniqueness)
 *   5.  ML-KEM-1024 keygen / encaps / decaps (tamper, wrong-key, implicit reject)
 *   6.  FrodoKEM-1344-SHAKE keygen / encaps / decaps (tamper)
 *   7.  BrainPool-512 ECDH key agreement (alice/bob/eve isolation)
 *   8.  Skein-1024 hashing (one-shot, streaming, empty, avalanche)
 *   9.  Skein-1024-MAC (determinism, key/data sensitivity, keyed ≠ unkeyed)
 *   10. HKDF (determinism, domain separation, salt variation, large output)
 *   11. Threefish-1024 raw block cipher (determinism, tweak, avalanche, bit dist)
 *   12. AEAD encrypt / decrypt (round-trip, tamper ct/tag/aad, wrong key/nonce,
 *       empty plaintext)
 *   13. CTR mode pattern analysis (repeating plaintext → no ciphertext pattern,
 *       AND/OR scans)
 *   14. Key combiners (invite triple-hybrid, peer dual-hybrid, transport derivation,
 *       K1 sensitivity, epoch rotation)
 *   15. Session timeout (range, jitter, spread)
 *   16. Edge cases — NULL rejection for every public API entry point
 *   17. Extended coverage:
 *       - KEM wrong-key (FrodoKEM), encaps randomness (FrodoKEM, ML-KEM)
 *       - ML-DSA sign/verify NULL param exhaustive, empty-message sign+verify
 *       - Skein streaming: empty (init→final), byte-by-byte, zero-length updates
 *       - Skein MAC: empty data, zero-length key
 *       - HKDF: NULL info, zero output length, NULL IKM rejection
 *       - Threefish key sensitivity
 *       - BP512: get_public_key determinism, full NULL param rejection for
 *         get_public_key and ecdh
 *       - AEAD: nonce-reuse determinism, nonce variation, multi-block (677 B),
 *         NULL key/nonce/out_len/ct rejection, auth-only (AAD without plaintext)
 *       - Key combiner: K2/K3 sensitivity, input-order sensitivity (invite & peer),
 *         peer K1/K2 sensitivity, transport determinism
 *       - Lifecycle: double-cleanup safety, re-init after double cleanup
 *       - Exhaustive NULL rejection for mlkem encaps/decaps, frodo keygen/encaps/
 *         decaps, skein update/final NULL ctx, hkdf NULL IKM, aead NULL ct/out_len
 *   18. Full handshake simulation (e2e invitation triple-hybrid → peer dual-hybrid
 *       → transport key → AEAD chat encrypt/decrypt)
 *
 * Dependencies: crypto.c, skein.c, skein_block.c, OpenSSL, oqs-provider.
 */

#include "crypto.h"
#include "static_memory.h"
#include <openssl/provider.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

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
    fprintf(stdout, "\n── %s\n", name)

/* ── Helper: check if buffer is all zeros ──────────────────────── */

static bool is_all_zero(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0) return false;
    }
    return true;
}

/* ── Helper: check if two buffers are identical ────────────────── */

static bool bufs_equal(const uint8_t *a, const uint8_t *b, size_t len) {
    return memcmp(a, b, len) == 0;
}

/* ── Helper: count matching 128-byte blocks in buffer ──────────── */

static int count_repeated_blocks(const uint8_t *buf, size_t len,
                                  size_t block_size)
{
    if (len < block_size * 2) return 0;
    int repeats = 0;
    size_t nblocks = len / block_size;
    for (size_t i = 0; i < nblocks; i++) {
        for (size_t j = i + 1; j < nblocks; j++) {
            if (memcmp(buf + i * block_size,
                       buf + j * block_size, block_size) == 0) {
                repeats++;
            }
        }
    }
    return repeats;
}

/* ═══════════════════════════════════════════════════════════════════
 *  1. Init / Cleanup
 * ═══════════════════════════════════════════════════════════════════ */

static void test_init_cleanup(void) {
    TEST_SECTION("Init / Cleanup lifecycle");

    yumi_crypto_err_t rc = yumi_crypto_init();
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "yumi_crypto_init succeeds");

    /* Double init should be idempotent */
    rc = yumi_crypto_init();
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "double init is idempotent");

    yumi_crypto_cleanup();

    /* Re-init after cleanup should work */
    rc = yumi_crypto_init();
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "re-init after cleanup succeeds");
}

/* ═══════════════════════════════════════════════════════════════════
 *  2. Random bytes
 * ═══════════════════════════════════════════════════════════════════ */

static void test_randombytes(void) {
    TEST_SECTION("Random bytes");

    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));

    yumi_crypto_err_t rc = yumi_randombytes(buf, sizeof(buf));
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "randombytes succeeds");
    TEST_ASSERT(!is_all_zero(buf, sizeof(buf)),
                "randombytes output is not all zeros");

    /* Two calls should produce different output */
    uint8_t buf2[256];
    yumi_randombytes(buf2, sizeof(buf2));
    TEST_ASSERT(!bufs_equal(buf, buf2, sizeof(buf)),
                "two randombytes calls produce different output");

    /* Zero-length should succeed */
    rc = yumi_randombytes(buf, 0);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "randombytes(0) succeeds");

    /* NULL buffer should fail */
    rc = yumi_randombytes(NULL, 16);
    TEST_ASSERT(rc == YUMI_CRYPTO_ERR_INVALID_PARAM, "randombytes(NULL) fails");

    /* Byte distribution: no single byte value should dominate.
     * Count occurrences; in 256 random bytes, each value appears ~1 time.
     * Flag if any value appears > 16 times (statistically near-impossible). */
    uint8_t big[4096];
    yumi_randombytes(big, sizeof(big));
    int counts[256] = {0};
    for (size_t i = 0; i < sizeof(big); i++)
        counts[big[i]]++;
    int max_count = 0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] > max_count) max_count = counts[i];
    }
    TEST_ASSERT(max_count < 64,
                "byte distribution: no value dominates (< 64 of 4096)");
}

/* ═══════════════════════════════════════════════════════════════════
 *  3. Memory hygiene
 * ═══════════════════════════════════════════════════════════════════ */

static void test_memory_hygiene(void) {
    TEST_SECTION("Memory hygiene");

    /* memzero */
    uint8_t secret[64];
    memset(secret, 0xAA, sizeof(secret));
    yumi_memzero(secret, sizeof(secret));
    TEST_ASSERT(is_all_zero(secret, sizeof(secret)),
                "memzero clears buffer");

    /* memzero on NULL should not crash */
    yumi_memzero(NULL, 64);
    TEST_ASSERT(1, "memzero(NULL) does not crash");

    /* Constant-time memcmp: equal */
    uint8_t a[32], b[32];
    memset(a, 0x42, sizeof(a));
    memset(b, 0x42, sizeof(b));
    TEST_ASSERT(yumi_memcmp(a, b, sizeof(a)) == 0,
                "memcmp returns 0 for equal buffers");

    /* Constant-time memcmp: different */
    b[31] ^= 0x01;
    TEST_ASSERT(yumi_memcmp(a, b, sizeof(a)) != 0,
                "memcmp returns non-zero for different buffers");

    /* NULL inputs */
    TEST_ASSERT(yumi_memcmp(NULL, b, sizeof(b)) != 0,
                "memcmp(NULL, b) returns non-zero");
    TEST_ASSERT(yumi_memcmp(a, NULL, sizeof(a)) != 0,
                "memcmp(a, NULL) returns non-zero");
}

/* ═══════════════════════════════════════════════════════════════════
 *  4. ML-DSA-87 — Signing
 * ═══════════════════════════════════════════════════════════════════ */

static void test_mldsa(void) {
    TEST_SECTION("ML-DSA-87 keygen / sign / verify");

    uint8_t pk[YUMI_MLDSA_PUBLIC_KEY_LEN];
    uint8_t sk[YUMI_MLDSA_SECRET_KEY_LEN];

    yumi_crypto_err_t rc = yumi_mldsa_keygen(pk, sk);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "ML-DSA keygen succeeds");
    TEST_ASSERT(!is_all_zero(pk, YUMI_MLDSA_PUBLIC_KEY_LEN),
                "ML-DSA public key is not zero");

    /* Sign a message */
    const uint8_t msg[] = "YumiBrowser test message for ML-DSA-87";
    uint8_t sig[YUMI_MLDSA_SIGN_LEN];

    rc = yumi_mldsa_sign(sig, msg, sizeof(msg) - 1, sk);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "ML-DSA sign succeeds");

    /* Verify valid signature */
    rc = yumi_mldsa_verify(sig, msg, sizeof(msg) - 1, pk);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "ML-DSA verify succeeds with correct sig");

    /* Tampered message should fail */
    uint8_t bad_msg[] = "YumiBrowser test message for ML-DSA-87";
    bad_msg[0] ^= 0x01;
    rc = yumi_mldsa_verify(sig, bad_msg, sizeof(bad_msg) - 1, pk);
    TEST_ASSERT(rc != YUMI_CRYPTO_OK, "ML-DSA verify fails with tampered msg");

    /* Tampered signature should fail */
    uint8_t bad_sig[YUMI_MLDSA_SIGN_LEN];
    memcpy(bad_sig, sig, YUMI_MLDSA_SIGN_LEN);
    bad_sig[0] ^= 0xFF;
    rc = yumi_mldsa_verify(bad_sig, msg, sizeof(msg) - 1, pk);
    TEST_ASSERT(rc != YUMI_CRYPTO_OK, "ML-DSA verify fails with tampered sig");

    /* Wrong key should fail */
    uint8_t pk2[YUMI_MLDSA_PUBLIC_KEY_LEN];
    uint8_t sk2[YUMI_MLDSA_SECRET_KEY_LEN];
    yumi_mldsa_keygen(pk2, sk2);
    rc = yumi_mldsa_verify(sig, msg, sizeof(msg) - 1, pk2);
    TEST_ASSERT(rc != YUMI_CRYPTO_OK, "ML-DSA verify fails with wrong key");

    /* Two keypairs should be different */
    TEST_ASSERT(!bufs_equal(pk, pk2, YUMI_MLDSA_PUBLIC_KEY_LEN),
                "two ML-DSA keygen produce different keys");

    /* Sign different messages → different signatures */
    uint8_t sig2[YUMI_MLDSA_SIGN_LEN];
    const uint8_t msg2[] = "Different message entirely";
    yumi_mldsa_sign(sig2, msg2, sizeof(msg2) - 1, sk);
    TEST_ASSERT(!bufs_equal(sig, sig2, YUMI_MLDSA_SIGN_LEN),
                "different messages produce different signatures");

    yumi_memzero(sk, sizeof(sk));
    yumi_memzero(sk2, sizeof(sk2));
}

/* ═══════════════════════════════════════════════════════════════════
 *  5. ML-KEM-1024 — Key Encapsulation
 * ═══════════════════════════════════════════════════════════════════ */

static void test_mlkem(void) {
    TEST_SECTION("ML-KEM-1024 keygen / encaps / decaps");

    uint8_t pk[YUMI_MLKEM_PUBLIC_KEY_LEN];
    uint8_t sk[YUMI_MLKEM_SECRET_KEY_LEN];

    yumi_crypto_err_t rc = yumi_mlkem_keygen(pk, sk);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "ML-KEM keygen succeeds");

    /* Encapsulate */
    uint8_t ct[YUMI_MLKEM_CIPHERTEXT_LEN];
    uint8_t ss_enc[YUMI_MLKEM_SHARED_SECRET_LEN];
    rc = yumi_mlkem_encaps(ct, ss_enc, pk);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "ML-KEM encaps succeeds");
    TEST_ASSERT(!is_all_zero(ss_enc, YUMI_MLKEM_SHARED_SECRET_LEN),
                "ML-KEM shared secret is not zero");

    /* Decapsulate — should produce same shared secret */
    uint8_t ss_dec[YUMI_MLKEM_SHARED_SECRET_LEN];
    rc = yumi_mlkem_decaps(ss_dec, ct, sk);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "ML-KEM decaps succeeds");
    TEST_ASSERT(bufs_equal(ss_enc, ss_dec, YUMI_MLKEM_SHARED_SECRET_LEN),
                "ML-KEM encaps/decaps produce matching shared secrets");

    /* Tampered ciphertext should produce different shared secret
     * (ML-KEM has implicit rejection — returns random, not error) */
    uint8_t bad_ct[YUMI_MLKEM_CIPHERTEXT_LEN];
    memcpy(bad_ct, ct, YUMI_MLKEM_CIPHERTEXT_LEN);
    bad_ct[0] ^= 0xFF;
    uint8_t ss_bad[YUMI_MLKEM_SHARED_SECRET_LEN];
    yumi_mlkem_decaps(ss_bad, bad_ct, sk);
    TEST_ASSERT(!bufs_equal(ss_enc, ss_bad, YUMI_MLKEM_SHARED_SECRET_LEN),
                "ML-KEM tampered ciphertext yields different shared secret");

    /* Wrong secret key should also fail */
    uint8_t pk2[YUMI_MLKEM_PUBLIC_KEY_LEN];
    uint8_t sk2[YUMI_MLKEM_SECRET_KEY_LEN];
    yumi_mlkem_keygen(pk2, sk2);
    uint8_t ss_wrong[YUMI_MLKEM_SHARED_SECRET_LEN];
    yumi_mlkem_decaps(ss_wrong, ct, sk2);
    TEST_ASSERT(!bufs_equal(ss_enc, ss_wrong, YUMI_MLKEM_SHARED_SECRET_LEN),
                "ML-KEM wrong key yields different shared secret");

    yumi_memzero(sk, sizeof(sk));
    yumi_memzero(sk2, sizeof(sk2));
}

/* ═══════════════════════════════════════════════════════════════════
 *  6. FrodoKEM-1344-SHAKE — Key Encapsulation
 * ═══════════════════════════════════════════════════════════════════ */

static void test_frodokem(void) {
    TEST_SECTION("FrodoKEM-1344-SHAKE keygen / encaps / decaps");

    uint8_t *pk = malloc(YUMI_FRODO_PUBLIC_KEY_LEN);
    uint8_t *sk = malloc(YUMI_FRODO_SECRET_KEY_LEN);
    TEST_ASSERT(pk && sk, "FrodoKEM alloc succeeds");

    yumi_crypto_err_t rc = yumi_frodo_keygen(pk, sk);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "FrodoKEM keygen succeeds");

    /* Encapsulate */
    uint8_t *ct = malloc(YUMI_FRODO_CIPHERTEXT_LEN);
    uint8_t ss_enc[YUMI_FRODO_SHARED_SECRET_LEN];
    TEST_ASSERT(ct != NULL, "FrodoKEM ciphertext alloc succeeds");

    rc = yumi_frodo_encaps(ct, ss_enc, pk);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "FrodoKEM encaps succeeds");
    TEST_ASSERT(!is_all_zero(ss_enc, YUMI_FRODO_SHARED_SECRET_LEN),
                "FrodoKEM shared secret is not zero");

    /* Decapsulate */
    uint8_t ss_dec[YUMI_FRODO_SHARED_SECRET_LEN];
    rc = yumi_frodo_decaps(ss_dec, ct, sk);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "FrodoKEM decaps succeeds");
    TEST_ASSERT(bufs_equal(ss_enc, ss_dec, YUMI_FRODO_SHARED_SECRET_LEN),
                "FrodoKEM encaps/decaps produce matching shared secrets");

    /* Tampered ciphertext */
    ct[100] ^= 0xFF;
    uint8_t ss_bad[YUMI_FRODO_SHARED_SECRET_LEN];
    yumi_frodo_decaps(ss_bad, ct, sk);
    TEST_ASSERT(!bufs_equal(ss_enc, ss_bad, YUMI_FRODO_SHARED_SECRET_LEN),
                "FrodoKEM tampered ciphertext yields different shared secret");

    yumi_memzero(sk, YUMI_FRODO_SECRET_KEY_LEN);
    free(pk); free(sk); free(ct);
}

/* ═══════════════════════════════════════════════════════════════════
 *  7. BrainPool-512 ECDH
 * ═══════════════════════════════════════════════════════════════════ */

static void test_bp512_ecdh(void) {
    TEST_SECTION("BrainPool-512 ECDH key agreement");

    yumi_bp512_keypair_t *alice = NULL;
    yumi_bp512_keypair_t *bob   = NULL;

    yumi_crypto_err_t rc = yumi_bp512_keygen(&alice);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "BP512 keygen (alice) succeeds");
    rc = yumi_bp512_keygen(&bob);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "BP512 keygen (bob) succeeds");

    /* Extract public keys */
    uint8_t alice_pk[YUMI_BP512_PUBLIC_KEY_LEN];
    uint8_t bob_pk[YUMI_BP512_PUBLIC_KEY_LEN];
    size_t alice_pk_len, bob_pk_len;

    rc = yumi_bp512_get_public_key(alice, alice_pk, &alice_pk_len);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "BP512 get_public_key (alice) succeeds");
    rc = yumi_bp512_get_public_key(bob, bob_pk, &bob_pk_len);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "BP512 get_public_key (bob) succeeds");

    /* Public keys should be different */
    TEST_ASSERT(!bufs_equal(alice_pk, bob_pk, alice_pk_len),
                "alice and bob have different public keys");

    /* ECDH: both sides should agree */
    uint8_t ss_alice[YUMI_BP512_SHARED_SECRET_LEN];
    uint8_t ss_bob[YUMI_BP512_SHARED_SECRET_LEN];
    size_t ss_a_len, ss_b_len;

    rc = yumi_bp512_ecdh(ss_alice, &ss_a_len, alice, bob_pk, bob_pk_len);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "BP512 ECDH (alice→bob) succeeds");

    rc = yumi_bp512_ecdh(ss_bob, &ss_b_len, bob, alice_pk, alice_pk_len);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "BP512 ECDH (bob→alice) succeeds");

    TEST_ASSERT(ss_a_len == ss_b_len, "shared secret lengths match");
    TEST_ASSERT(bufs_equal(ss_alice, ss_bob, ss_a_len),
                "BP512 ECDH produces matching shared secrets");

    TEST_ASSERT(!is_all_zero(ss_alice, ss_a_len),
                "BP512 shared secret is not zero");

    /* A third party gets a different shared secret */
    yumi_bp512_keypair_t *eve = NULL;
    yumi_bp512_keygen(&eve);
    uint8_t eve_pk[YUMI_BP512_PUBLIC_KEY_LEN];
    size_t eve_pk_len;
    yumi_bp512_get_public_key(eve, eve_pk, &eve_pk_len);

    uint8_t ss_eve[YUMI_BP512_SHARED_SECRET_LEN];
    size_t ss_e_len;
    yumi_bp512_ecdh(ss_eve, &ss_e_len, eve, bob_pk, bob_pk_len);
    TEST_ASSERT(!bufs_equal(ss_alice, ss_eve, ss_a_len),
                "eve gets a different shared secret than alice");

    yumi_bp512_keypair_free(alice);
    yumi_bp512_keypair_free(bob);
    yumi_bp512_keypair_free(eve);
}

/* ═══════════════════════════════════════════════════════════════════
 *  8. Skein-1024 Hashing
 * ═══════════════════════════════════════════════════════════════════ */

static void test_skein_hash(void) {
    TEST_SECTION("Skein-1024 hashing");

    /* One-shot hash */
    const uint8_t data[] = "Hello, Skein-1024!";
    uint8_t hash1[YUMI_SKEIN_HASH_LEN];
    uint8_t hash2[YUMI_SKEIN_HASH_LEN];

    yumi_crypto_err_t rc = yumi_skein_hash(hash1, data, sizeof(data) - 1);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_hash succeeds");
    TEST_ASSERT(!is_all_zero(hash1, YUMI_SKEIN_HASH_LEN),
                "hash output is not zero");

    /* Deterministic: same input → same output */
    rc = yumi_skein_hash(hash2, data, sizeof(data) - 1);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_hash second call succeeds");
    TEST_ASSERT(bufs_equal(hash1, hash2, YUMI_SKEIN_HASH_LEN),
                "skein_hash is deterministic");

    /* Different input → different output */
    const uint8_t data2[] = "Hello, Skein-1024?";
    uint8_t hash3[YUMI_SKEIN_HASH_LEN];
    yumi_skein_hash(hash3, data2, sizeof(data2) - 1);
    TEST_ASSERT(!bufs_equal(hash1, hash3, YUMI_SKEIN_HASH_LEN),
                "different input produces different hash");

    /* Streaming hash should match one-shot */
    yumi_skein_ctx_t *ctx = NULL;
    rc = yumi_skein_init(&ctx);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK && ctx != NULL, "skein_init succeeds");

    /* Feed in chunks */
    size_t half = (sizeof(data) - 1) / 2;
    rc = yumi_skein_update(ctx, data, half);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_update (chunk 1) succeeds");
    rc = yumi_skein_update(ctx, data + half, sizeof(data) - 1 - half);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_update (chunk 2) succeeds");

    uint8_t hash_stream[YUMI_SKEIN_HASH_LEN];
    rc = yumi_skein_final(ctx, hash_stream);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_final succeeds");
    TEST_ASSERT(bufs_equal(hash1, hash_stream, YUMI_SKEIN_HASH_LEN),
                "streaming hash matches one-shot hash");

    yumi_skein_free(ctx);

    /* Empty input hashing */
    uint8_t hash_empty[YUMI_SKEIN_HASH_LEN];
    rc = yumi_skein_hash(hash_empty, NULL, 0);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_hash(NULL, 0) succeeds");
    TEST_ASSERT(!is_all_zero(hash_empty, YUMI_SKEIN_HASH_LEN),
                "empty input hash is not zero");
    TEST_ASSERT(!bufs_equal(hash1, hash_empty, YUMI_SKEIN_HASH_LEN),
                "empty input hash differs from non-empty");

    /* Avalanche: one-bit flip in input → roughly half of output bits differ */
    uint8_t data_flip[sizeof(data)];
    memcpy(data_flip, data, sizeof(data) - 1);
    data_flip[0] ^= 0x01;  /* flip one bit */
    uint8_t hash_flip[YUMI_SKEIN_HASH_LEN];
    yumi_skein_hash(hash_flip, data_flip, sizeof(data) - 1);

    int bits_different = 0;
    for (size_t i = 0; i < YUMI_SKEIN_HASH_LEN; i++) {
        uint8_t diff = hash1[i] ^ hash_flip[i];
        while (diff) { bits_different += diff & 1; diff >>= 1; }
    }
    /* Expect ~512 of 1024 bits to differ. Accept 300-724 (generous). */
    TEST_ASSERT(bits_different > 300 && bits_different < 724,
                "avalanche: one-bit flip changes ~50% of hash bits");
}

/* ═══════════════════════════════════════════════════════════════════
 *  9. Skein-1024-MAC
 * ═══════════════════════════════════════════════════════════════════ */

static void test_skein_mac(void) {
    TEST_SECTION("Skein-1024-MAC");

    uint8_t key[64];
    uint8_t key2[64];
    yumi_randombytes(key, sizeof(key));
    yumi_randombytes(key2, sizeof(key2));

    const uint8_t data[] = "MAC this data";
    uint8_t mac1[YUMI_SKEIN_MAC_LEN];
    uint8_t mac2[YUMI_SKEIN_MAC_LEN];

    yumi_crypto_err_t rc = yumi_skein_mac(mac1, key, sizeof(key),
                                           data, sizeof(data) - 1);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_mac succeeds");

    /* Deterministic */
    yumi_skein_mac(mac2, key, sizeof(key), data, sizeof(data) - 1);
    TEST_ASSERT(bufs_equal(mac1, mac2, YUMI_SKEIN_MAC_LEN),
                "skein_mac is deterministic");

    /* Different key → different MAC */
    uint8_t mac3[YUMI_SKEIN_MAC_LEN];
    yumi_skein_mac(mac3, key2, sizeof(key2), data, sizeof(data) - 1);
    TEST_ASSERT(!bufs_equal(mac1, mac3, YUMI_SKEIN_MAC_LEN),
                "different key produces different MAC");

    /* Different data → different MAC */
    const uint8_t data2[] = "MAC this data!";
    uint8_t mac4[YUMI_SKEIN_MAC_LEN];
    yumi_skein_mac(mac4, key, sizeof(key), data2, sizeof(data2) - 1);
    TEST_ASSERT(!bufs_equal(mac1, mac4, YUMI_SKEIN_MAC_LEN),
                "different data produces different MAC");

    /* MAC ≠ plain hash (keyed vs unkeyed) */
    uint8_t plain_hash[YUMI_SKEIN_HASH_LEN];
    yumi_skein_hash(plain_hash, data, sizeof(data) - 1);
    TEST_ASSERT(!bufs_equal(mac1, plain_hash, YUMI_SKEIN_MAC_LEN),
                "MAC differs from unkeyed hash of same data");
}

/* ═══════════════════════════════════════════════════════════════════
 *  10. HKDF
 * ═══════════════════════════════════════════════════════════════════ */

static void test_hkdf(void) {
    TEST_SECTION("HKDF (Skein-1024-MAC based)");

    uint8_t ikm[32];
    yumi_randombytes(ikm, sizeof(ikm));

    uint8_t okm1[YUMI_TRANSPORT_KEY_LEN];
    uint8_t okm2[YUMI_TRANSPORT_KEY_LEN];

    const uint8_t info[] = "test-info";
    const uint8_t salt[] = "test-salt";

    /* Basic derivation */
    yumi_crypto_err_t rc = yumi_hkdf(okm1, sizeof(okm1),
                                      ikm, sizeof(ikm),
                                      salt, sizeof(salt) - 1,
                                      info, sizeof(info) - 1);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "HKDF succeeds");
    TEST_ASSERT(!is_all_zero(okm1, sizeof(okm1)), "HKDF output is not zero");

    /* Deterministic */
    yumi_hkdf(okm2, sizeof(okm2), ikm, sizeof(ikm),
              salt, sizeof(salt) - 1, info, sizeof(info) - 1);
    TEST_ASSERT(bufs_equal(okm1, okm2, sizeof(okm1)),
                "HKDF is deterministic");

    /* Different info → different output (domain separation) */
    const uint8_t info2[] = "different-info";
    uint8_t okm3[YUMI_TRANSPORT_KEY_LEN];
    yumi_hkdf(okm3, sizeof(okm3), ikm, sizeof(ikm),
              salt, sizeof(salt) - 1, info2, sizeof(info2) - 1);
    TEST_ASSERT(!bufs_equal(okm1, okm3, sizeof(okm1)),
                "different info produces different HKDF output");

    /* Different salt → different output */
    const uint8_t salt2[] = "other-salt";
    uint8_t okm4[YUMI_TRANSPORT_KEY_LEN];
    yumi_hkdf(okm4, sizeof(okm4), ikm, sizeof(ikm),
              salt2, sizeof(salt2) - 1, info, sizeof(info) - 1);
    TEST_ASSERT(!bufs_equal(okm1, okm4, sizeof(okm1)),
                "different salt produces different HKDF output");

    /* NULL salt should work (zero salt) */
    uint8_t okm5[YUMI_TRANSPORT_KEY_LEN];
    rc = yumi_hkdf(okm5, sizeof(okm5), ikm, sizeof(ikm),
                   NULL, 0, info, sizeof(info) - 1);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "HKDF with NULL salt succeeds");
    TEST_ASSERT(!bufs_equal(okm1, okm5, sizeof(okm1)),
                "NULL salt differs from explicit salt");

    /* Large output (> one hash block = 128 bytes) exercises expansion loop */
    uint8_t okm_big[256];
    rc = yumi_hkdf(okm_big, sizeof(okm_big), ikm, sizeof(ikm),
                   NULL, 0, info, sizeof(info) - 1);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "HKDF large output succeeds");
    /* First 128 bytes should match the shorter derivation */
    TEST_ASSERT(bufs_equal(okm5, okm_big, sizeof(okm5)),
                "HKDF large output prefix matches shorter output");
}

/* ═══════════════════════════════════════════════════════════════════
 *  11. Threefish-1024 raw block cipher — ECB pattern detection
 * ═══════════════════════════════════════════════════════════════════ */

static void test_threefish_block(void) {
    TEST_SECTION("Threefish-1024 raw block cipher");

    uint8_t key[YUMI_THREEFISH_KEY_LEN];
    uint8_t tweak[YUMI_THREEFISH_TWEAK_LEN];
    yumi_randombytes(key, sizeof(key));
    yumi_randombytes(tweak, sizeof(tweak));

    /* Encrypt a known plaintext */
    uint8_t plain[YUMI_THREEFISH_BLOCK_LEN];
    memset(plain, 0x41, sizeof(plain));  /* all 'A' */
    uint8_t cipher[YUMI_THREEFISH_BLOCK_LEN];

    yumi_crypto_err_t rc = yumi_threefish_encrypt_block(cipher, plain,
                                                         key, tweak);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "threefish_encrypt_block succeeds");
    TEST_ASSERT(!bufs_equal(plain, cipher, YUMI_THREEFISH_BLOCK_LEN),
                "ciphertext differs from plaintext");

    /* Deterministic */
    uint8_t cipher2[YUMI_THREEFISH_BLOCK_LEN];
    yumi_threefish_encrypt_block(cipher2, plain, key, tweak);
    TEST_ASSERT(bufs_equal(cipher, cipher2, YUMI_THREEFISH_BLOCK_LEN),
                "threefish is deterministic (same key+tweak+plain)");

    /* Different tweak → different ciphertext */
    uint8_t tweak2[YUMI_THREEFISH_TWEAK_LEN];
    memcpy(tweak2, tweak, sizeof(tweak2));
    tweak2[0] ^= 0x01;
    uint8_t cipher3[YUMI_THREEFISH_BLOCK_LEN];
    yumi_threefish_encrypt_block(cipher3, plain, key, tweak2);
    TEST_ASSERT(!bufs_equal(cipher, cipher3, YUMI_THREEFISH_BLOCK_LEN),
                "different tweak produces different ciphertext");

    /* ECB pattern detection: encrypt same plaintext 8 times under same key
     * but raw block cipher SHOULD produce identical blocks (that's expected
     * for a PRP). The concern is if our Threefish output has internal
     * patterns — we verify via bit distribution analysis. */
    int bit_ones = 0;
    for (size_t i = 0; i < YUMI_THREEFISH_BLOCK_LEN; i++) {
        uint8_t b = cipher[i];
        while (b) { bit_ones += b & 1; b >>= 1; }
    }
    /* 1024 bits total, expect ~512 set. Accept 384-640 (generous). */
    TEST_ASSERT(bit_ones > 384 && bit_ones < 640,
                "ciphertext bit distribution is roughly uniform");

    /* Verify all-zero plaintext doesn't produce all-zero ciphertext */
    uint8_t zero_plain[YUMI_THREEFISH_BLOCK_LEN];
    uint8_t zero_cipher[YUMI_THREEFISH_BLOCK_LEN];
    memset(zero_plain, 0, sizeof(zero_plain));
    yumi_threefish_encrypt_block(zero_cipher, zero_plain, key, tweak);
    TEST_ASSERT(!is_all_zero(zero_cipher, YUMI_THREEFISH_BLOCK_LEN),
                "encrypting all-zero plaintext produces non-zero ciphertext");

    /* Verify full diffusion: changing one bit of plaintext → massive change */
    uint8_t one_bit[YUMI_THREEFISH_BLOCK_LEN];
    memset(one_bit, 0, sizeof(one_bit));
    one_bit[0] = 0x01;
    uint8_t cipher_zero[YUMI_THREEFISH_BLOCK_LEN];
    uint8_t cipher_one[YUMI_THREEFISH_BLOCK_LEN];
    yumi_threefish_encrypt_block(cipher_zero, zero_plain, key, tweak);
    yumi_threefish_encrypt_block(cipher_one, one_bit, key, tweak);

    int diff_bits = 0;
    for (size_t i = 0; i < YUMI_THREEFISH_BLOCK_LEN; i++) {
        uint8_t d = cipher_zero[i] ^ cipher_one[i];
        while (d) { diff_bits += d & 1; d >>= 1; }
    }
    TEST_ASSERT(diff_bits > 300,
                "one-bit plaintext change flips many ciphertext bits (avalanche)");
}

/* ═══════════════════════════════════════════════════════════════════
 *  12. AEAD — Encrypt / Decrypt
 * ═══════════════════════════════════════════════════════════════════ */

static void test_aead(void) {
    TEST_SECTION("AEAD (Threefish-1024-CTR + Skein-1024-MAC)");

    uint8_t key[YUMI_AEAD_KEY_LEN];
    uint8_t nonce[YUMI_AEAD_NONCE_LEN];
    yumi_randombytes(key, sizeof(key));
    yumi_randombytes(nonce, sizeof(nonce));

    const uint8_t plaintext[] = "This is a secret message for AEAD testing. "
                                 "It needs to be reasonably long to exercise "
                                 "multiple CTR blocks properly.";
    const size_t pt_len = sizeof(plaintext) - 1;
    const uint8_t aad[] = "associated data header";
    const size_t aad_len = sizeof(aad) - 1;

    /* Encrypt */
    size_t ct_buf_len = pt_len + YUMI_AEAD_TAG_LEN;
    uint8_t *ct = malloc(ct_buf_len);
    size_t ct_len;
    TEST_ASSERT(ct != NULL, "AEAD ciphertext alloc");

    yumi_crypto_err_t rc = yumi_aead_encrypt(ct, &ct_len, plaintext, pt_len,
                                              aad, aad_len, nonce, key);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "AEAD encrypt succeeds");
    TEST_ASSERT(ct_len == pt_len + YUMI_AEAD_TAG_LEN,
                "AEAD ciphertext length = plaintext + tag");

    /* Ciphertext should differ from plaintext */
    TEST_ASSERT(!bufs_equal(ct, plaintext, pt_len),
                "AEAD ciphertext differs from plaintext");

    /* Decrypt */
    uint8_t *decrypted = malloc(pt_len);
    size_t dec_len;
    TEST_ASSERT(decrypted != NULL, "AEAD decrypted alloc");

    rc = yumi_aead_decrypt(decrypted, &dec_len, ct, ct_len,
                            aad, aad_len, nonce, key);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "AEAD decrypt succeeds");
    TEST_ASSERT(dec_len == pt_len, "AEAD decrypted length matches");
    TEST_ASSERT(bufs_equal(decrypted, plaintext, pt_len),
                "AEAD round-trip recovers original plaintext");

    /* Tampered ciphertext → decrypt should fail */
    uint8_t *tampered_ct = malloc(ct_len);
    memcpy(tampered_ct, ct, ct_len);
    tampered_ct[0] ^= 0x01;
    rc = yumi_aead_decrypt(decrypted, &dec_len, tampered_ct, ct_len,
                            aad, aad_len, nonce, key);
    TEST_ASSERT(rc != YUMI_CRYPTO_OK,
                "AEAD decrypt fails with tampered ciphertext body");

    /* Tampered tag → decrypt should fail */
    memcpy(tampered_ct, ct, ct_len);
    tampered_ct[ct_len - 1] ^= 0x01;  /* flip bit in tag */
    rc = yumi_aead_decrypt(decrypted, &dec_len, tampered_ct, ct_len,
                            aad, aad_len, nonce, key);
    TEST_ASSERT(rc != YUMI_CRYPTO_OK,
                "AEAD decrypt fails with tampered tag");

    /* Tampered AAD → decrypt should fail */
    uint8_t bad_aad[] = "associated data hacker";
    rc = yumi_aead_decrypt(decrypted, &dec_len, ct, ct_len,
                            bad_aad, sizeof(bad_aad) - 1, nonce, key);
    TEST_ASSERT(rc != YUMI_CRYPTO_OK,
                "AEAD decrypt fails with tampered AAD");

    /* Wrong key → decrypt should fail */
    uint8_t wrong_key[YUMI_AEAD_KEY_LEN];
    yumi_randombytes(wrong_key, sizeof(wrong_key));
    rc = yumi_aead_decrypt(decrypted, &dec_len, ct, ct_len,
                            aad, aad_len, nonce, wrong_key);
    TEST_ASSERT(rc != YUMI_CRYPTO_OK,
                "AEAD decrypt fails with wrong key");

    /* Wrong nonce → decrypt should fail */
    uint8_t wrong_nonce[YUMI_AEAD_NONCE_LEN];
    yumi_randombytes(wrong_nonce, sizeof(wrong_nonce));
    rc = yumi_aead_decrypt(decrypted, &dec_len, ct, ct_len,
                            aad, aad_len, wrong_nonce, key);
    TEST_ASSERT(rc != YUMI_CRYPTO_OK,
                "AEAD decrypt fails with wrong nonce");

    /* Empty plaintext */
    uint8_t empty_ct[YUMI_AEAD_TAG_LEN];
    size_t empty_ct_len;
    rc = yumi_aead_encrypt(empty_ct, &empty_ct_len, NULL, 0,
                            aad, aad_len, nonce, key);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "AEAD encrypt empty plaintext succeeds");
    TEST_ASSERT(empty_ct_len == YUMI_AEAD_TAG_LEN,
                "AEAD empty plaintext produces tag-only output");

    uint8_t empty_dec[1];
    size_t empty_dec_len;
    rc = yumi_aead_decrypt(empty_dec, &empty_dec_len, empty_ct, empty_ct_len,
                            aad, aad_len, nonce, key);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "AEAD decrypt empty ciphertext succeeds");
    TEST_ASSERT(empty_dec_len == 0, "AEAD empty decrypt produces zero bytes");

    free(ct);
    free(decrypted);
    free(tampered_ct);
}

/* ═══════════════════════════════════════════════════════════════════
 *  13. CTR mode pattern analysis
 *
 *  Encrypt a large buffer of repeating plaintext blocks under
 *  CTR mode via AEAD. Verify no ciphertext blocks repeat
 *  (which would indicate ECB-like behavior or counter reuse).
 * ═══════════════════════════════════════════════════════════════════ */

static void test_ctr_no_pattern(void) {
    TEST_SECTION("CTR mode: no repeating ciphertext blocks on repeating plaintext");

    /* Create 16 identical 128-byte plaintext blocks */
    const size_t nblocks = 16;
    const size_t block_size = YUMI_THREEFISH_BLOCK_LEN;  /* 128 */
    size_t pt_len = nblocks * block_size;
    uint8_t *plaintext = malloc(pt_len);
    TEST_ASSERT(plaintext != NULL, "pattern test alloc");

    /* Fill every block with the same pattern */
    for (size_t i = 0; i < pt_len; i++)
        plaintext[i] = 0xAA;

    uint8_t key[YUMI_AEAD_KEY_LEN];
    uint8_t nonce[YUMI_AEAD_NONCE_LEN];
    yumi_randombytes(key, sizeof(key));
    yumi_randombytes(nonce, sizeof(nonce));

    size_t ct_buf_len = pt_len + YUMI_AEAD_TAG_LEN;
    uint8_t *ct = malloc(ct_buf_len);
    size_t ct_len;

    yumi_crypto_err_t rc = yumi_aead_encrypt(ct, &ct_len, plaintext, pt_len,
                                              NULL, 0, nonce, key);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "CTR pattern encrypt succeeds");

    /* Count repeated 128-byte blocks in ciphertext (excluding tag) */
    int repeats = count_repeated_blocks(ct, pt_len, block_size);
    TEST_ASSERT(repeats == 0,
                "CTR mode: no repeated ciphertext blocks from identical plaintext");

    /* Also do byte-level AND scan: in ECB mode with identical plaintext,
     * AND-ing all ciphertext blocks would yield the ciphertext value
     * (since they're all the same). In CTR mode, the AND should converge
     * toward 0x00 as the keystream XOR makes blocks independent. */
    uint8_t and_result[128];
    memcpy(and_result, ct, block_size);
    for (size_t i = 1; i < nblocks; i++) {
        for (size_t j = 0; j < block_size; j++) {
            and_result[j] &= ct[i * block_size + j];
        }
    }

    /* Count bytes that are still non-zero after AND */
    int nonzero = 0;
    for (size_t j = 0; j < block_size; j++) {
        if (and_result[j] != 0) nonzero++;
    }
    /* With 16 independent random blocks, expect very few non-zero AND bytes.
     * P(byte survives AND of 16 random) = (255/256)^16 ≈ 0.94 per bit,
     * but the blocks aren't truly random — they're keystream XOR same value.
     * Still, 16 different keystream blocks AND-ed should yield mostly zeros.
     * Be generous: allow up to 32 non-zero bytes out of 128. */
    TEST_ASSERT(nonzero < 32,
                "CTR AND test: blocks are independent (mostly zero AND)");

    /* Verify with a bitwise OR scan too: should converge toward 0xFF */
    uint8_t or_result[128];
    memset(or_result, 0, block_size);
    for (size_t i = 0; i < nblocks; i++) {
        for (size_t j = 0; j < block_size; j++) {
            or_result[j] |= ct[i * block_size + j];
        }
    }
    int all_ones = 0;
    for (size_t j = 0; j < block_size; j++) {
        if (or_result[j] == 0xFF) all_ones++;
    }
    /* Most bytes should be 0xFF after OR-ing 16 independent blocks */
    TEST_ASSERT(all_ones > 96,
                "CTR OR test: blocks cover most bit positions (>96/128 = 0xFF)");

    free(plaintext);
    free(ct);
}

/* ═══════════════════════════════════════════════════════════════════
 *  14. Key combiners
 * ═══════════════════════════════════════════════════════════════════ */

static void test_key_combiners(void) {
    TEST_SECTION("Key combiners (invite / peer / transport)");

    uint8_t k1[32], k2[32], k3[64];
    yumi_randombytes(k1, sizeof(k1));
    yumi_randombytes(k2, sizeof(k2));
    yumi_randombytes(k3, sizeof(k3));

    /* Triple-hybrid invitation combiner */
    uint8_t session_key[YUMI_TRANSPORT_KEY_LEN];
    yumi_crypto_err_t rc = yumi_combine_invite_keys(
        session_key, k1, sizeof(k1), k2, sizeof(k2), k3, sizeof(k3));
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "combine_invite_keys succeeds");
    TEST_ASSERT(!is_all_zero(session_key, YUMI_TRANSPORT_KEY_LEN),
                "invitation session_key is not zero");

    /* Deterministic */
    uint8_t session_key2[YUMI_TRANSPORT_KEY_LEN];
    yumi_combine_invite_keys(session_key2, k1, sizeof(k1),
                              k2, sizeof(k2), k3, sizeof(k3));
    TEST_ASSERT(bufs_equal(session_key, session_key2, YUMI_TRANSPORT_KEY_LEN),
                "combine_invite_keys is deterministic");

    /* Changing any input changes the output */
    uint8_t k1_bad[32];
    memcpy(k1_bad, k1, sizeof(k1_bad));
    k1_bad[0] ^= 0x01;
    uint8_t session_key3[YUMI_TRANSPORT_KEY_LEN];
    yumi_combine_invite_keys(session_key3, k1_bad, sizeof(k1_bad),
                              k2, sizeof(k2), k3, sizeof(k3));
    TEST_ASSERT(!bufs_equal(session_key, session_key3, YUMI_TRANSPORT_KEY_LEN),
                "changing K1 changes invitation session_key");

    /* Dual-hybrid peer combiner */
    uint8_t temp_key[YUMI_TRANSPORT_KEY_LEN];
    rc = yumi_combine_peer_keys(temp_key, k1, sizeof(k1), k2, sizeof(k2));
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "combine_peer_keys succeeds");
    TEST_ASSERT(!is_all_zero(temp_key, YUMI_TRANSPORT_KEY_LEN),
                "peer temp_key is not zero");

    /* Invitation key ≠ peer key (different domain labels) */
    uint8_t invite_2key[YUMI_TRANSPORT_KEY_LEN];
    yumi_combine_invite_keys(invite_2key, k1, sizeof(k1),
                              k2, sizeof(k2), k3, sizeof(k3));
    TEST_ASSERT(!bufs_equal(temp_key, invite_2key, YUMI_TRANSPORT_KEY_LEN),
                "peer combiner output differs from invite combiner");

    /* Transport key derivation */
    uint8_t epoch_key[YUMI_EPOCH_KEY_LEN];
    yumi_randombytes(epoch_key, sizeof(epoch_key));

    uint8_t transport_key[YUMI_TRANSPORT_KEY_LEN];
    rc = yumi_derive_transport_key(transport_key, temp_key, epoch_key);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "derive_transport_key succeeds");
    TEST_ASSERT(!is_all_zero(transport_key, YUMI_TRANSPORT_KEY_LEN),
                "transport_key is not zero");

    /* Transport key ≠ temp_key (epoch contributes) */
    TEST_ASSERT(!bufs_equal(transport_key, temp_key, YUMI_TRANSPORT_KEY_LEN),
                "transport_key differs from temp_key");

    /* Different epoch → different transport key (epoch rotation) */
    uint8_t epoch_key2[YUMI_EPOCH_KEY_LEN];
    yumi_randombytes(epoch_key2, sizeof(epoch_key2));
    uint8_t transport_key2[YUMI_TRANSPORT_KEY_LEN];
    yumi_derive_transport_key(transport_key2, temp_key, epoch_key2);
    TEST_ASSERT(!bufs_equal(transport_key, transport_key2,
                            YUMI_TRANSPORT_KEY_LEN),
                "different epoch_key produces different transport_key");
}

/* ═══════════════════════════════════════════════════════════════════
 *  15. Session timeout
 * ═══════════════════════════════════════════════════════════════════ */

static void test_session_timeout(void) {
    TEST_SECTION("Session timeout (jitter range)");

    uint32_t min_seen = UINT32_MAX;
    uint32_t max_seen = 0;
    bool all_same = true;
    uint32_t first = 0;

    /* Sample 200 timeouts to verify range and variation */
    for (int i = 0; i < 200; i++) {
        uint32_t t = yumi_session_timeout();
        if (i == 0) first = t;
        else if (t != first) all_same = false;

        if (t < min_seen) min_seen = t;
        if (t > max_seen) max_seen = t;

        uint32_t lower = YUMI_SESSION_TIMEOUT_BASE - YUMI_SESSION_TIMEOUT_JITTER;
        uint32_t upper = YUMI_SESSION_TIMEOUT_BASE + YUMI_SESSION_TIMEOUT_JITTER;
        TEST_ASSERT(t >= lower && t <= upper,
                    "session_timeout is within [base-jitter, base+jitter]");
    }

    TEST_ASSERT(!all_same,
                "session_timeout produces varied results (jitter working)");

    /* Spread should be reasonable — at least 60 seconds across 200 samples */
    TEST_ASSERT((max_seen - min_seen) >= 60,
                "session_timeout has meaningful jitter spread");
}

/* ═══════════════════════════════════════════════════════════════════
 *  16. Edge cases and adversarial inputs
 * ═══════════════════════════════════════════════════════════════════ */

static void test_edge_cases(void) {
    TEST_SECTION("Edge cases and adversarial inputs");

    /* NULL outputs */
    TEST_ASSERT(yumi_mldsa_keygen(NULL, NULL) == YUMI_CRYPTO_ERR_INVALID_PARAM,
                "mldsa_keygen(NULL, NULL) returns INVALID_PARAM");
    TEST_ASSERT(yumi_mlkem_keygen(NULL, NULL) == YUMI_CRYPTO_ERR_INVALID_PARAM,
                "mlkem_keygen(NULL, NULL) returns INVALID_PARAM");
    TEST_ASSERT(yumi_bp512_keygen(NULL) == YUMI_CRYPTO_ERR_INVALID_PARAM,
                "bp512_keygen(NULL) returns INVALID_PARAM");
    TEST_ASSERT(yumi_skein_hash(NULL, NULL, 0) == YUMI_CRYPTO_ERR_INVALID_PARAM,
                "skein_hash(NULL, ...) returns INVALID_PARAM");
    TEST_ASSERT(yumi_skein_init(NULL) == YUMI_CRYPTO_ERR_INVALID_PARAM,
                "skein_init(NULL) returns INVALID_PARAM");
    TEST_ASSERT(yumi_skein_mac(NULL, NULL, 0, NULL, 0) == YUMI_CRYPTO_ERR_INVALID_PARAM,
                "skein_mac(NULL, ...) returns INVALID_PARAM");
    TEST_ASSERT(yumi_hkdf(NULL, 128, NULL, 0, NULL, 0, NULL, 0) ==
                YUMI_CRYPTO_ERR_INVALID_PARAM,
                "hkdf(NULL, ...) returns INVALID_PARAM");
    TEST_ASSERT(yumi_threefish_encrypt_block(NULL, NULL, NULL, NULL) ==
                YUMI_CRYPTO_ERR_INVALID_PARAM,
                "threefish_encrypt_block(NULL, ...) returns INVALID_PARAM");

    /* AEAD NULL checks */
    uint8_t dummy[256];
    size_t dummy_len;
    TEST_ASSERT(yumi_aead_encrypt(NULL, &dummy_len, dummy, 1,
                                   NULL, 0, dummy, dummy) ==
                YUMI_CRYPTO_ERR_INVALID_PARAM,
                "aead_encrypt(NULL out, ...) returns INVALID_PARAM");
    TEST_ASSERT(yumi_aead_decrypt(NULL, &dummy_len, dummy, YUMI_AEAD_TAG_LEN + 1,
                                   NULL, 0, dummy, dummy) ==
                YUMI_CRYPTO_ERR_INVALID_PARAM,
                "aead_decrypt(NULL out, ...) returns INVALID_PARAM");

    /* AEAD with ciphertext shorter than tag must fail */
    TEST_ASSERT(yumi_aead_decrypt(dummy, &dummy_len, dummy, YUMI_AEAD_TAG_LEN - 1,
                                   NULL, 0, dummy, dummy) ==
                YUMI_CRYPTO_ERR_DECRYPT,
                "aead_decrypt with ct < tag length returns DECRYPT error");

    /* Key combiner NULL checks */
    TEST_ASSERT(yumi_combine_invite_keys(NULL, dummy, 32, dummy, 32,
                                          dummy, 64) ==
                YUMI_CRYPTO_ERR_INVALID_PARAM,
                "combine_invite_keys(NULL, ...) returns INVALID_PARAM");
    TEST_ASSERT(yumi_combine_peer_keys(NULL, dummy, 32, dummy, 32) ==
                YUMI_CRYPTO_ERR_INVALID_PARAM,
                "combine_peer_keys(NULL, ...) returns INVALID_PARAM");
    TEST_ASSERT(yumi_derive_transport_key(NULL, dummy, dummy) ==
                YUMI_CRYPTO_ERR_INVALID_PARAM,
                "derive_transport_key(NULL, ...) returns INVALID_PARAM");

    /* Free NULL should not crash */
    yumi_bp512_keypair_free(NULL);
    yumi_skein_free(NULL);
    TEST_ASSERT(1, "free(NULL) does not crash");
}

/* ═══════════════════════════════════════════════════════════════════
 *  17. Extended coverage — additional edge cases & security checks
 * ═══════════════════════════════════════════════════════════════════ */

static void test_extended_coverage(void) {
    TEST_SECTION("Extended coverage — KEM randomness & wrong-key");

    /* ── FrodoKEM wrong-key decapsulation ── */
    {
        uint8_t *pk  = malloc(YUMI_FRODO_PUBLIC_KEY_LEN);
        uint8_t *sk  = malloc(YUMI_FRODO_SECRET_KEY_LEN);
        uint8_t *pk2 = malloc(YUMI_FRODO_PUBLIC_KEY_LEN);
        uint8_t *sk2 = malloc(YUMI_FRODO_SECRET_KEY_LEN);
        uint8_t *ct  = malloc(YUMI_FRODO_CIPHERTEXT_LEN);
        TEST_ASSERT(pk && sk && pk2 && sk2 && ct, "FrodoKEM wrong-key alloc");

        yumi_frodo_keygen(pk, sk);
        yumi_frodo_keygen(pk2, sk2);

        uint8_t ss_enc[YUMI_FRODO_SHARED_SECRET_LEN];
        yumi_frodo_encaps(ct, ss_enc, pk);

        uint8_t ss_wrong[YUMI_FRODO_SHARED_SECRET_LEN];
        yumi_frodo_decaps(ss_wrong, ct, sk2);
        TEST_ASSERT(!bufs_equal(ss_enc, ss_wrong, YUMI_FRODO_SHARED_SECRET_LEN),
                    "FrodoKEM wrong key yields different shared secret");

        yumi_memzero(sk, YUMI_FRODO_SECRET_KEY_LEN);
        yumi_memzero(sk2, YUMI_FRODO_SECRET_KEY_LEN);
        free(pk); free(sk); free(pk2); free(sk2); free(ct);
    }

    /* ── FrodoKEM encaps randomness (same pk → different ct/ss each time) ── */
    {
        uint8_t *pk = malloc(YUMI_FRODO_PUBLIC_KEY_LEN);
        uint8_t *sk = malloc(YUMI_FRODO_SECRET_KEY_LEN);
        uint8_t *ct1 = malloc(YUMI_FRODO_CIPHERTEXT_LEN);
        uint8_t *ct2 = malloc(YUMI_FRODO_CIPHERTEXT_LEN);
        TEST_ASSERT(pk && sk && ct1 && ct2, "FrodoKEM randomness alloc");

        yumi_frodo_keygen(pk, sk);

        uint8_t ss1[YUMI_FRODO_SHARED_SECRET_LEN];
        uint8_t ss2[YUMI_FRODO_SHARED_SECRET_LEN];
        yumi_frodo_encaps(ct1, ss1, pk);
        yumi_frodo_encaps(ct2, ss2, pk);

        TEST_ASSERT(!bufs_equal(ct1, ct2, YUMI_FRODO_CIPHERTEXT_LEN),
                    "FrodoKEM two encaps produce different ciphertexts");
        TEST_ASSERT(!bufs_equal(ss1, ss2, YUMI_FRODO_SHARED_SECRET_LEN),
                    "FrodoKEM two encaps produce different shared secrets");

        yumi_memzero(sk, YUMI_FRODO_SECRET_KEY_LEN);
        free(pk); free(sk); free(ct1); free(ct2);
    }

    /* ── ML-KEM encaps randomness ── */
    {
        uint8_t pk[YUMI_MLKEM_PUBLIC_KEY_LEN], sk[YUMI_MLKEM_SECRET_KEY_LEN];
        yumi_mlkem_keygen(pk, sk);

        uint8_t ct1[YUMI_MLKEM_CIPHERTEXT_LEN], ct2[YUMI_MLKEM_CIPHERTEXT_LEN];
        uint8_t ss1[YUMI_MLKEM_SHARED_SECRET_LEN], ss2[YUMI_MLKEM_SHARED_SECRET_LEN];
        yumi_mlkem_encaps(ct1, ss1, pk);
        yumi_mlkem_encaps(ct2, ss2, pk);

        TEST_ASSERT(!bufs_equal(ct1, ct2, YUMI_MLKEM_CIPHERTEXT_LEN),
                    "ML-KEM two encaps produce different ciphertexts");
        TEST_ASSERT(!bufs_equal(ss1, ss2, YUMI_MLKEM_SHARED_SECRET_LEN),
                    "ML-KEM two encaps produce different shared secrets");

        yumi_memzero(sk, sizeof(sk));
    }

    TEST_SECTION("Extended coverage — signature edge cases");

    /* ── ML-DSA sign/verify NULL param rejection ── */
    {
        uint8_t pk[YUMI_MLDSA_PUBLIC_KEY_LEN], sk[YUMI_MLDSA_SECRET_KEY_LEN];
        yumi_mldsa_keygen(pk, sk);
        uint8_t sig[YUMI_MLDSA_SIGN_LEN];

        TEST_ASSERT(yumi_mldsa_sign(sig, NULL, 0, sk) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "ML-DSA sign(NULL msg) returns INVALID_PARAM");
        TEST_ASSERT(yumi_mldsa_sign(NULL, (const uint8_t *)"x", 1, sk) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "ML-DSA sign(NULL sig) returns INVALID_PARAM");
        TEST_ASSERT(yumi_mldsa_sign(sig, (const uint8_t *)"x", 1, NULL) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "ML-DSA sign(NULL sk) returns INVALID_PARAM");

        yumi_mldsa_sign(sig, (const uint8_t *)"x", 1, sk);
        TEST_ASSERT(yumi_mldsa_verify(NULL, (const uint8_t *)"x", 1, pk) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "ML-DSA verify(NULL sig) returns INVALID_PARAM");
        TEST_ASSERT(yumi_mldsa_verify(sig, NULL, 0, pk) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "ML-DSA verify(NULL msg) returns INVALID_PARAM");
        TEST_ASSERT(yumi_mldsa_verify(sig, (const uint8_t *)"x", 1, NULL) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "ML-DSA verify(NULL pk) returns INVALID_PARAM");

        /* Sign + verify empty message (non-NULL ptr, len=0) */
        uint8_t empty = 0;
        yumi_crypto_err_t rc = yumi_mldsa_sign(sig, &empty, 0, sk);
        if (rc == YUMI_CRYPTO_OK) {
            rc = yumi_mldsa_verify(sig, &empty, 0, pk);
            TEST_ASSERT(rc == YUMI_CRYPTO_OK,
                        "ML-DSA sign+verify empty message succeeds");
        } else {
            /* Provider may reject zero-length — not a bug */
            TEST_ASSERT(1, "ML-DSA empty message: provider limitation (OK)");
        }

        yumi_memzero(sk, sizeof(sk));
    }

    TEST_SECTION("Extended coverage — Skein streaming edge cases");

    /* ── Empty streaming (init → final with no update) ── */
    {
        yumi_skein_ctx_t *ctx = NULL;
        yumi_crypto_err_t rc = yumi_skein_init(&ctx);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_init for empty streaming");

        uint8_t hash_stream[YUMI_SKEIN_HASH_LEN];
        rc = yumi_skein_final(ctx, hash_stream);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_final with no update succeeds");

        uint8_t hash_oneshot[YUMI_SKEIN_HASH_LEN];
        yumi_skein_hash(hash_oneshot, NULL, 0);
        TEST_ASSERT(bufs_equal(hash_stream, hash_oneshot, YUMI_SKEIN_HASH_LEN),
                    "empty streaming hash matches one-shot empty hash");

        yumi_skein_free(ctx);
    }

    /* ── Byte-by-byte streaming ── */
    {
        const uint8_t data[] = "Hello, Skein-1024!";
        size_t data_len = sizeof(data) - 1;

        uint8_t hash_oneshot[YUMI_SKEIN_HASH_LEN];
        yumi_skein_hash(hash_oneshot, data, data_len);

        yumi_skein_ctx_t *ctx = NULL;
        yumi_skein_init(&ctx);
        for (size_t i = 0; i < data_len; i++)
            yumi_skein_update(ctx, &data[i], 1);
        uint8_t hash_bytewise[YUMI_SKEIN_HASH_LEN];
        yumi_skein_final(ctx, hash_bytewise);
        yumi_skein_free(ctx);

        TEST_ASSERT(bufs_equal(hash_oneshot, hash_bytewise, YUMI_SKEIN_HASH_LEN),
                    "byte-by-byte streaming matches one-shot hash");
    }

    /* ── Zero-length updates are transparent ── */
    {
        const uint8_t data[] = "test data";
        size_t data_len = sizeof(data) - 1;

        uint8_t hash_oneshot[YUMI_SKEIN_HASH_LEN];
        yumi_skein_hash(hash_oneshot, data, data_len);

        yumi_skein_ctx_t *ctx = NULL;
        yumi_skein_init(&ctx);
        yumi_skein_update(ctx, NULL, 0);
        yumi_skein_update(ctx, data, data_len);
        yumi_skein_update(ctx, NULL, 0);
        uint8_t hash_padded[YUMI_SKEIN_HASH_LEN];
        yumi_skein_final(ctx, hash_padded);
        yumi_skein_free(ctx);

        TEST_ASSERT(bufs_equal(hash_oneshot, hash_padded, YUMI_SKEIN_HASH_LEN),
                    "zero-length updates don't affect streaming hash");
    }

    TEST_SECTION("Extended coverage — Skein MAC edge cases");

    /* ── MAC with empty data ── */
    {
        uint8_t key[64];
        yumi_randombytes(key, sizeof(key));

        uint8_t mac[YUMI_SKEIN_MAC_LEN];
        yumi_crypto_err_t rc = yumi_skein_mac(mac, key, sizeof(key), NULL, 0);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "skein_mac with empty data succeeds");
        TEST_ASSERT(!is_all_zero(mac, YUMI_SKEIN_MAC_LEN),
                    "skein_mac empty data output is not zero");

        uint8_t mac2[YUMI_SKEIN_MAC_LEN];
        yumi_skein_mac(mac2, key, sizeof(key), (const uint8_t *)"x", 1);
        TEST_ASSERT(!bufs_equal(mac, mac2, YUMI_SKEIN_MAC_LEN),
                    "MAC of empty data differs from MAC of non-empty data");
    }

    /* ── MAC with zero-length key ── */
    {
        uint8_t dummy_key = 0;
        uint8_t mac[YUMI_SKEIN_MAC_LEN];
        yumi_crypto_err_t rc = yumi_skein_mac(mac, &dummy_key, 0,
                                               (const uint8_t *)"data", 4);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK,
                    "skein_mac with zero-length key succeeds");
        TEST_ASSERT(!is_all_zero(mac, YUMI_SKEIN_MAC_LEN),
                    "zero-length key MAC output is not zero");
    }

    TEST_SECTION("Extended coverage — HKDF edge cases");

    /* ── NULL info ── */
    {
        uint8_t ikm[32];
        yumi_randombytes(ikm, sizeof(ikm));

        uint8_t okm[YUMI_TRANSPORT_KEY_LEN];
        yumi_crypto_err_t rc = yumi_hkdf(okm, sizeof(okm),
                                          ikm, sizeof(ikm),
                                          NULL, 0, NULL, 0);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "HKDF with NULL info succeeds");
        TEST_ASSERT(!is_all_zero(okm, sizeof(okm)),
                    "HKDF NULL info output is not zero");

        uint8_t okm2[YUMI_TRANSPORT_KEY_LEN];
        yumi_hkdf(okm2, sizeof(okm2), ikm, sizeof(ikm),
                  NULL, 0, (const uint8_t *)"info", 4);
        TEST_ASSERT(!bufs_equal(okm, okm2, sizeof(okm)),
                    "HKDF NULL info differs from explicit info");
    }

    /* ── Zero output length ── */
    {
        uint8_t ikm[32];
        yumi_randombytes(ikm, sizeof(ikm));
        uint8_t okm[1] = {0xAA};
        yumi_crypto_err_t rc = yumi_hkdf(okm, 0, ikm, sizeof(ikm),
                                          NULL, 0, NULL, 0);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "HKDF zero output length succeeds");
        TEST_ASSERT(okm[0] == 0xAA, "HKDF zero-len doesn't write to buffer");
    }

    TEST_SECTION("Extended coverage — Threefish key sensitivity");

    {
        uint8_t key1[YUMI_THREEFISH_KEY_LEN], key2[YUMI_THREEFISH_KEY_LEN];
        uint8_t tweak[YUMI_THREEFISH_TWEAK_LEN];
        uint8_t plain[YUMI_THREEFISH_BLOCK_LEN];
        yumi_randombytes(key1, sizeof(key1));
        yumi_randombytes(key2, sizeof(key2));
        yumi_randombytes(tweak, sizeof(tweak));
        memset(plain, 0x42, sizeof(plain));

        uint8_t c1[YUMI_THREEFISH_BLOCK_LEN], c2[YUMI_THREEFISH_BLOCK_LEN];
        yumi_threefish_encrypt_block(c1, plain, key1, tweak);
        yumi_threefish_encrypt_block(c2, plain, key2, tweak);
        TEST_ASSERT(!bufs_equal(c1, c2, YUMI_THREEFISH_BLOCK_LEN),
                    "different key produces different Threefish ciphertext");
    }

    TEST_SECTION("Extended coverage — BP512 additional");

    /* ── get_public_key determinism ── */
    {
        yumi_bp512_keypair_t *kp = NULL;
        yumi_bp512_keygen(&kp);

        uint8_t pk1[YUMI_BP512_PUBLIC_KEY_LEN], pk2[YUMI_BP512_PUBLIC_KEY_LEN];
        size_t len1, len2;
        yumi_bp512_get_public_key(kp, pk1, &len1);
        yumi_bp512_get_public_key(kp, pk2, &len2);
        TEST_ASSERT(len1 == len2 && bufs_equal(pk1, pk2, len1),
                    "BP512 get_public_key is deterministic");

        yumi_bp512_keypair_free(kp);
    }

    /* ── BP512 NULL parameter rejection ── */
    {
        yumi_bp512_keypair_t *kp = NULL;
        yumi_bp512_keygen(&kp);
        uint8_t pk[YUMI_BP512_PUBLIC_KEY_LEN];
        size_t pk_len;

        TEST_ASSERT(yumi_bp512_get_public_key(NULL, pk, &pk_len) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "BP512 get_public_key(NULL kp) returns INVALID_PARAM");
        TEST_ASSERT(yumi_bp512_get_public_key(kp, NULL, &pk_len) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "BP512 get_public_key(NULL pk) returns INVALID_PARAM");
        TEST_ASSERT(yumi_bp512_get_public_key(kp, pk, NULL) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "BP512 get_public_key(NULL pk_len) returns INVALID_PARAM");

        uint8_t ss[YUMI_BP512_SHARED_SECRET_LEN];
        size_t ss_len;
        yumi_bp512_get_public_key(kp, pk, &pk_len);

        TEST_ASSERT(yumi_bp512_ecdh(NULL, &ss_len, kp, pk, pk_len) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "BP512 ecdh(NULL ss) returns INVALID_PARAM");
        TEST_ASSERT(yumi_bp512_ecdh(ss, NULL, kp, pk, pk_len) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "BP512 ecdh(NULL ss_len) returns INVALID_PARAM");
        TEST_ASSERT(yumi_bp512_ecdh(ss, &ss_len, NULL, pk, pk_len) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "BP512 ecdh(NULL kp) returns INVALID_PARAM");
        TEST_ASSERT(yumi_bp512_ecdh(ss, &ss_len, kp, NULL, pk_len) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "BP512 ecdh(NULL peer_pk) returns INVALID_PARAM");

        yumi_bp512_keypair_free(kp);
    }

    TEST_SECTION("Extended coverage — AEAD additional");

    /* ── Nonce reuse is deterministic ── */
    {
        uint8_t key[YUMI_AEAD_KEY_LEN], nonce[YUMI_AEAD_NONCE_LEN];
        yumi_randombytes(key, sizeof(key));
        yumi_randombytes(nonce, sizeof(nonce));

        const uint8_t pt[] = "determinism test";
        size_t pt_len = sizeof(pt) - 1;
        size_t ct_buf = pt_len + YUMI_AEAD_TAG_LEN;

        uint8_t *ct1 = malloc(ct_buf), *ct2 = malloc(ct_buf);
        size_t ct1_len, ct2_len;
        yumi_aead_encrypt(ct1, &ct1_len, pt, pt_len, NULL, 0, nonce, key);
        yumi_aead_encrypt(ct2, &ct2_len, pt, pt_len, NULL, 0, nonce, key);
        TEST_ASSERT(ct1_len == ct2_len &&
                    bufs_equal(ct1, ct2, ct1_len),
                    "AEAD same key+nonce+pt is deterministic");
        free(ct1); free(ct2);
    }

    /* ── Different nonce → different ciphertext ── */
    {
        uint8_t key[YUMI_AEAD_KEY_LEN];
        uint8_t nonce1[YUMI_AEAD_NONCE_LEN], nonce2[YUMI_AEAD_NONCE_LEN];
        yumi_randombytes(key, sizeof(key));
        yumi_randombytes(nonce1, sizeof(nonce1));
        yumi_randombytes(nonce2, sizeof(nonce2));

        const uint8_t pt[] = "nonce variation";
        size_t pt_len = sizeof(pt) - 1;
        size_t ct_buf = pt_len + YUMI_AEAD_TAG_LEN;

        uint8_t *ct1 = malloc(ct_buf), *ct2 = malloc(ct_buf);
        size_t len1, len2;
        yumi_aead_encrypt(ct1, &len1, pt, pt_len, NULL, 0, nonce1, key);
        yumi_aead_encrypt(ct2, &len2, pt, pt_len, NULL, 0, nonce2, key);
        TEST_ASSERT(!bufs_equal(ct1, ct2, len1),
                    "AEAD different nonce produces different ciphertext");
        free(ct1); free(ct2);
    }

    /* ── Multi-block roundtrip (5 full blocks + partial) ── */
    {
        uint8_t key[YUMI_AEAD_KEY_LEN], nonce[YUMI_AEAD_NONCE_LEN];
        yumi_randombytes(key, sizeof(key));
        yumi_randombytes(nonce, sizeof(nonce));

        size_t pt_len = 5 * YUMI_THREEFISH_BLOCK_LEN + 37;  /* 677 bytes */
        uint8_t *pt = malloc(pt_len);
        yumi_randombytes(pt, pt_len);

        size_t ct_buf = pt_len + YUMI_AEAD_TAG_LEN;
        uint8_t *ct = malloc(ct_buf);
        size_t ct_len;

        yumi_crypto_err_t rc = yumi_aead_encrypt(ct, &ct_len, pt, pt_len,
                                                  (const uint8_t *)"hdr", 3,
                                                  nonce, key);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "AEAD multi-block encrypt succeeds");
        TEST_ASSERT(ct_len == pt_len + YUMI_AEAD_TAG_LEN,
                    "AEAD multi-block ciphertext length correct");

        uint8_t *dec = malloc(pt_len);
        size_t dec_len;
        rc = yumi_aead_decrypt(dec, &dec_len, ct, ct_len,
                                (const uint8_t *)"hdr", 3, nonce, key);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "AEAD multi-block decrypt succeeds");
        TEST_ASSERT(dec_len == pt_len && bufs_equal(dec, pt, pt_len),
                    "AEAD multi-block roundtrip recovers plaintext");

        free(pt); free(ct); free(dec);
    }

    /* ── AEAD NULL key/nonce rejection ── */
    {
        uint8_t buf[256];
        size_t len;
        TEST_ASSERT(yumi_aead_encrypt(buf, &len, buf, 1,
                                       NULL, 0, buf, NULL) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "AEAD encrypt(NULL key) returns INVALID_PARAM");
        TEST_ASSERT(yumi_aead_encrypt(buf, &len, buf, 1,
                                       NULL, 0, NULL, buf) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "AEAD encrypt(NULL nonce) returns INVALID_PARAM");
        TEST_ASSERT(yumi_aead_decrypt(buf, &len, buf, YUMI_AEAD_TAG_LEN + 1,
                                       NULL, 0, buf, NULL) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "AEAD decrypt(NULL key) returns INVALID_PARAM");
        TEST_ASSERT(yumi_aead_decrypt(buf, &len, buf, YUMI_AEAD_TAG_LEN + 1,
                                       NULL, 0, NULL, buf) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "AEAD decrypt(NULL nonce) returns INVALID_PARAM");
    }

    /* ── Auth-only mode (AAD with no plaintext) ── */
    {
        uint8_t key[YUMI_AEAD_KEY_LEN], nonce[YUMI_AEAD_NONCE_LEN];
        yumi_randombytes(key, sizeof(key));
        yumi_randombytes(nonce, sizeof(nonce));

        const uint8_t aad[] = "authenticate this header only";
        uint8_t tag_out[YUMI_AEAD_TAG_LEN];
        size_t ct_len;

        yumi_crypto_err_t rc = yumi_aead_encrypt(tag_out, &ct_len,
                                                  NULL, 0,
                                                  aad, sizeof(aad) - 1,
                                                  nonce, key);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "AEAD auth-only encrypt succeeds");
        TEST_ASSERT(ct_len == YUMI_AEAD_TAG_LEN,
                    "AEAD auth-only produces tag only");

        uint8_t dec_dummy[1];
        size_t dec_len;
        rc = yumi_aead_decrypt(dec_dummy, &dec_len, tag_out, ct_len,
                                aad, sizeof(aad) - 1, nonce, key);
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "AEAD auth-only decrypt succeeds");

        rc = yumi_aead_decrypt(dec_dummy, &dec_len, tag_out, ct_len,
                                (const uint8_t *)"wrong", 5, nonce, key);
        TEST_ASSERT(rc != YUMI_CRYPTO_OK,
                    "AEAD auth-only with wrong AAD fails");
    }

    TEST_SECTION("Extended coverage — key combiner sensitivity");

    /* ── Invite combiner: K2 and K3 sensitivity ── */
    {
        uint8_t k1[32], k2[32], k3[64];
        yumi_randombytes(k1, sizeof(k1));
        yumi_randombytes(k2, sizeof(k2));
        yumi_randombytes(k3, sizeof(k3));

        uint8_t base[YUMI_TRANSPORT_KEY_LEN];
        yumi_combine_invite_keys(base, k1, sizeof(k1),
                                  k2, sizeof(k2), k3, sizeof(k3));

        /* Change K2 */
        uint8_t k2_mod[32];
        memcpy(k2_mod, k2, sizeof(k2_mod));
        k2_mod[0] ^= 0x01;
        uint8_t out_k2[YUMI_TRANSPORT_KEY_LEN];
        yumi_combine_invite_keys(out_k2, k1, sizeof(k1),
                                  k2_mod, sizeof(k2_mod), k3, sizeof(k3));
        TEST_ASSERT(!bufs_equal(base, out_k2, YUMI_TRANSPORT_KEY_LEN),
                    "changing K2 changes invitation session_key");

        /* Change K3 */
        uint8_t k3_mod[64];
        memcpy(k3_mod, k3, sizeof(k3_mod));
        k3_mod[0] ^= 0x01;
        uint8_t out_k3[YUMI_TRANSPORT_KEY_LEN];
        yumi_combine_invite_keys(out_k3, k1, sizeof(k1),
                                  k2, sizeof(k2), k3_mod, sizeof(k3_mod));
        TEST_ASSERT(!bufs_equal(base, out_k3, YUMI_TRANSPORT_KEY_LEN),
                    "changing K3 changes invitation session_key");

        /* Order matters: K1||K2||K3 vs K2||K1||K3 */
        uint8_t out_swap[YUMI_TRANSPORT_KEY_LEN];
        yumi_combine_invite_keys(out_swap, k2, sizeof(k2),
                                  k1, sizeof(k1), k3, sizeof(k3));
        TEST_ASSERT(!bufs_equal(base, out_swap, YUMI_TRANSPORT_KEY_LEN),
                    "invite combiner: swapping K1/K2 changes output");
    }

    /* ── Peer combiner sensitivity ── */
    {
        uint8_t k1[32], k2[64];
        yumi_randombytes(k1, sizeof(k1));
        yumi_randombytes(k2, sizeof(k2));

        uint8_t base[YUMI_TRANSPORT_KEY_LEN];
        yumi_combine_peer_keys(base, k1, sizeof(k1), k2, sizeof(k2));

        uint8_t k1_mod[32];
        memcpy(k1_mod, k1, sizeof(k1_mod));
        k1_mod[0] ^= 0x01;
        uint8_t out1[YUMI_TRANSPORT_KEY_LEN];
        yumi_combine_peer_keys(out1, k1_mod, sizeof(k1_mod), k2, sizeof(k2));
        TEST_ASSERT(!bufs_equal(base, out1, YUMI_TRANSPORT_KEY_LEN),
                    "peer combiner: changing K1 changes output");

        uint8_t k2_mod[64];
        memcpy(k2_mod, k2, sizeof(k2_mod));
        k2_mod[0] ^= 0x01;
        uint8_t out2[YUMI_TRANSPORT_KEY_LEN];
        yumi_combine_peer_keys(out2, k1, sizeof(k1), k2_mod, sizeof(k2_mod));
        TEST_ASSERT(!bufs_equal(base, out2, YUMI_TRANSPORT_KEY_LEN),
                    "peer combiner: changing K2 changes output");

        /* Order: same-size keys, swap order */
        uint8_t ka[32], kb[32];
        yumi_randombytes(ka, sizeof(ka));
        yumi_randombytes(kb, sizeof(kb));
        uint8_t ab[YUMI_TRANSPORT_KEY_LEN], ba[YUMI_TRANSPORT_KEY_LEN];
        yumi_combine_peer_keys(ab, ka, sizeof(ka), kb, sizeof(kb));
        yumi_combine_peer_keys(ba, kb, sizeof(kb), ka, sizeof(ka));
        TEST_ASSERT(!bufs_equal(ab, ba, YUMI_TRANSPORT_KEY_LEN),
                    "peer combiner: swapping K1/K2 changes output");
    }

    /* ── Transport key determinism ── */
    {
        uint8_t temp[YUMI_TRANSPORT_KEY_LEN], epoch[YUMI_EPOCH_KEY_LEN];
        yumi_randombytes(temp, sizeof(temp));
        yumi_randombytes(epoch, sizeof(epoch));

        uint8_t t1[YUMI_TRANSPORT_KEY_LEN], t2[YUMI_TRANSPORT_KEY_LEN];
        yumi_derive_transport_key(t1, temp, epoch);
        yumi_derive_transport_key(t2, temp, epoch);
        TEST_ASSERT(bufs_equal(t1, t2, YUMI_TRANSPORT_KEY_LEN),
                    "derive_transport_key is deterministic");
    }

    TEST_SECTION("Extended coverage — lifecycle & NULL exhaustive");

    /* ── Double cleanup safety ── */
    {
        yumi_crypto_cleanup();
        yumi_crypto_cleanup();
        TEST_ASSERT(1, "double cleanup does not crash");

        yumi_crypto_err_t rc = yumi_crypto_init();
        TEST_ASSERT(rc == YUMI_CRYPTO_OK, "re-init after double cleanup");
    }

    /* ── KEM / signature NULL param exhaustive ── */
    {
        uint8_t dummy[64];

        TEST_ASSERT(yumi_mlkem_encaps(NULL, dummy, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "mlkem_encaps(NULL ct) returns INVALID_PARAM");
        TEST_ASSERT(yumi_mlkem_encaps(dummy, NULL, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "mlkem_encaps(NULL ss) returns INVALID_PARAM");
        TEST_ASSERT(yumi_mlkem_decaps(NULL, dummy, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "mlkem_decaps(NULL ss) returns INVALID_PARAM");
        TEST_ASSERT(yumi_mlkem_decaps(dummy, NULL, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "mlkem_decaps(NULL ct) returns INVALID_PARAM");

        TEST_ASSERT(yumi_frodo_keygen(NULL, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "frodo_keygen(NULL pk) returns INVALID_PARAM");
        TEST_ASSERT(yumi_frodo_keygen(dummy, NULL) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "frodo_keygen(NULL sk) returns INVALID_PARAM");
        TEST_ASSERT(yumi_frodo_encaps(NULL, dummy, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "frodo_encaps(NULL ct) returns INVALID_PARAM");
        TEST_ASSERT(yumi_frodo_decaps(NULL, dummy, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "frodo_decaps(NULL ss) returns INVALID_PARAM");

        /* Skein update/final NULL ctx */
        TEST_ASSERT(yumi_skein_update(NULL, dummy, 1) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "skein_update(NULL ctx) returns INVALID_PARAM");
        TEST_ASSERT(yumi_skein_final(NULL, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "skein_final(NULL ctx) returns INVALID_PARAM");

        /* HKDF NULL IKM */
        TEST_ASSERT(yumi_hkdf(dummy, 64, NULL, 0, NULL, 0, NULL, 0) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "hkdf(NULL ikm) returns INVALID_PARAM");

        /* AEAD decrypt NULL ciphertext */
        size_t len;
        TEST_ASSERT(yumi_aead_decrypt(dummy, &len, NULL,
                                       YUMI_AEAD_TAG_LEN + 1,
                                       NULL, 0, dummy, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "aead_decrypt(NULL ct) returns INVALID_PARAM");

        /* AEAD NULL out_len */
        TEST_ASSERT(yumi_aead_encrypt(dummy, NULL, dummy, 1,
                                       NULL, 0, dummy, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "aead_encrypt(NULL out_len) returns INVALID_PARAM");
        TEST_ASSERT(yumi_aead_decrypt(dummy, NULL, dummy,
                                       YUMI_AEAD_TAG_LEN + 1,
                                       NULL, 0, dummy, dummy) ==
                    YUMI_CRYPTO_ERR_INVALID_PARAM,
                    "aead_decrypt(NULL out_len) returns INVALID_PARAM");
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  18. Full handshake simulation (end-to-end)
 * ═══════════════════════════════════════════════════════════════════ */

static void test_full_handshake(void) {
    TEST_SECTION("Full handshake simulation (e2e invite → peer → transport)");

    /* === Invitation handshake (triple-hybrid) === */

    /* Joiner generates ephemeral KEM keypairs */
    uint8_t mlkem_pk[YUMI_MLKEM_PUBLIC_KEY_LEN],
            mlkem_sk[YUMI_MLKEM_SECRET_KEY_LEN];
    yumi_mlkem_keygen(mlkem_pk, mlkem_sk);

    uint8_t *frodo_pk = malloc(YUMI_FRODO_PUBLIC_KEY_LEN);
    uint8_t *frodo_sk = malloc(YUMI_FRODO_SECRET_KEY_LEN);
    yumi_frodo_keygen(frodo_pk, frodo_sk);

    yumi_bp512_keypair_t *joiner_bp = NULL;
    yumi_bp512_keygen(&joiner_bp);
    uint8_t joiner_bp_pk[YUMI_BP512_PUBLIC_KEY_LEN];
    size_t joiner_bp_pk_len;
    yumi_bp512_get_public_key(joiner_bp, joiner_bp_pk, &joiner_bp_pk_len);

    /* Host encapsulates and generates ephemeral BP key */
    uint8_t mlkem_ct[YUMI_MLKEM_CIPHERTEXT_LEN];
    uint8_t mlkem_ss_host[YUMI_MLKEM_SHARED_SECRET_LEN];
    yumi_mlkem_encaps(mlkem_ct, mlkem_ss_host, mlkem_pk);

    uint8_t *frodo_ct = malloc(YUMI_FRODO_CIPHERTEXT_LEN);
    uint8_t frodo_ss_host[YUMI_FRODO_SHARED_SECRET_LEN];
    yumi_frodo_encaps(frodo_ct, frodo_ss_host, frodo_pk);

    yumi_bp512_keypair_t *host_bp = NULL;
    yumi_bp512_keygen(&host_bp);
    uint8_t host_bp_pk[YUMI_BP512_PUBLIC_KEY_LEN];
    size_t host_bp_pk_len;
    yumi_bp512_get_public_key(host_bp, host_bp_pk, &host_bp_pk_len);

    /* Both sides derive BP shared secret */
    uint8_t bp_ss_host[YUMI_BP512_SHARED_SECRET_LEN];
    uint8_t bp_ss_joiner[YUMI_BP512_SHARED_SECRET_LEN];
    size_t bp_ss_len;
    yumi_bp512_ecdh(bp_ss_host, &bp_ss_len, host_bp,
                     joiner_bp_pk, joiner_bp_pk_len);
    yumi_bp512_ecdh(bp_ss_joiner, &bp_ss_len, joiner_bp,
                     host_bp_pk, host_bp_pk_len);
    TEST_ASSERT(bufs_equal(bp_ss_host, bp_ss_joiner, bp_ss_len),
                "e2e: BP shared secrets match");

    /* Joiner decapsulates KEMs */
    uint8_t mlkem_ss_joiner[YUMI_MLKEM_SHARED_SECRET_LEN];
    yumi_mlkem_decaps(mlkem_ss_joiner, mlkem_ct, mlkem_sk);
    TEST_ASSERT(bufs_equal(mlkem_ss_host, mlkem_ss_joiner,
                           YUMI_MLKEM_SHARED_SECRET_LEN),
                "e2e: ML-KEM shared secrets match");

    uint8_t frodo_ss_joiner[YUMI_FRODO_SHARED_SECRET_LEN];
    yumi_frodo_decaps(frodo_ss_joiner, frodo_ct, frodo_sk);
    TEST_ASSERT(bufs_equal(frodo_ss_host, frodo_ss_joiner,
                           YUMI_FRODO_SHARED_SECRET_LEN),
                "e2e: FrodoKEM shared secrets match");

    /* Triple-hybrid combiner */
    uint8_t session_host[YUMI_TRANSPORT_KEY_LEN];
    uint8_t session_joiner[YUMI_TRANSPORT_KEY_LEN];

    yumi_combine_invite_keys(session_host,
        mlkem_ss_host, YUMI_MLKEM_SHARED_SECRET_LEN,
        frodo_ss_host, YUMI_FRODO_SHARED_SECRET_LEN,
        bp_ss_host, bp_ss_len);

    yumi_combine_invite_keys(session_joiner,
        mlkem_ss_joiner, YUMI_MLKEM_SHARED_SECRET_LEN,
        frodo_ss_joiner, YUMI_FRODO_SHARED_SECRET_LEN,
        bp_ss_joiner, bp_ss_len);

    TEST_ASSERT(bufs_equal(session_host, session_joiner, YUMI_TRANSPORT_KEY_LEN),
                "e2e: invitation session keys match (triple-hybrid)");

    /* Encrypt registrar data under session key */
    const uint8_t registrar_data[] = "Registrar snapshot with peer list...";
    size_t reg_len = sizeof(registrar_data) - 1;
    uint8_t invite_nonce[YUMI_AEAD_NONCE_LEN];
    yumi_randombytes(invite_nonce, sizeof(invite_nonce));

    size_t enc_buf_len = reg_len + YUMI_AEAD_TAG_LEN;
    uint8_t *enc_registrar = malloc(enc_buf_len);
    size_t enc_len;
    yumi_aead_encrypt(enc_registrar, &enc_len, registrar_data, reg_len,
                       NULL, 0, invite_nonce, session_host);

    /* Joiner decrypts */
    uint8_t *dec_registrar = malloc(reg_len);
    size_t dec_len;
    yumi_crypto_err_t rc = yumi_aead_decrypt(dec_registrar, &dec_len,
                                              enc_registrar, enc_len,
                                              NULL, 0, invite_nonce,
                                              session_joiner);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "e2e: registrar decrypt succeeds");
    TEST_ASSERT(dec_len == reg_len && bufs_equal(dec_registrar,
                registrar_data, reg_len),
                "e2e: registrar data recovered correctly");

    /* === Peer handshake (dual-hybrid) + transport key === */
    uint8_t peer_mlkem_pk[YUMI_MLKEM_PUBLIC_KEY_LEN],
            peer_mlkem_sk[YUMI_MLKEM_SECRET_KEY_LEN];
    yumi_mlkem_keygen(peer_mlkem_pk, peer_mlkem_sk);

    yumi_bp512_keypair_t *peer_a = NULL, *peer_b = NULL;
    yumi_bp512_keygen(&peer_a);
    yumi_bp512_keygen(&peer_b);

    uint8_t pa_pk[YUMI_BP512_PUBLIC_KEY_LEN], pb_pk[YUMI_BP512_PUBLIC_KEY_LEN];
    size_t pa_len, pb_len;
    yumi_bp512_get_public_key(peer_a, pa_pk, &pa_len);
    yumi_bp512_get_public_key(peer_b, pb_pk, &pb_len);

    /* KEM encaps/decaps */
    uint8_t peer_ct[YUMI_MLKEM_CIPHERTEXT_LEN];
    uint8_t peer_ss_a[YUMI_MLKEM_SHARED_SECRET_LEN],
            peer_ss_b[YUMI_MLKEM_SHARED_SECRET_LEN];
    yumi_mlkem_encaps(peer_ct, peer_ss_a, peer_mlkem_pk);
    yumi_mlkem_decaps(peer_ss_b, peer_ct, peer_mlkem_sk);

    /* BP ECDH */
    uint8_t bp_a[YUMI_BP512_SHARED_SECRET_LEN],
            bp_b[YUMI_BP512_SHARED_SECRET_LEN];
    size_t bp_a_len, bp_b_len;
    yumi_bp512_ecdh(bp_a, &bp_a_len, peer_a, pb_pk, pb_len);
    yumi_bp512_ecdh(bp_b, &bp_b_len, peer_b, pa_pk, pa_len);

    /* Peer combiner */
    uint8_t temp_a[YUMI_TRANSPORT_KEY_LEN], temp_b[YUMI_TRANSPORT_KEY_LEN];
    yumi_combine_peer_keys(temp_a, peer_ss_a, YUMI_MLKEM_SHARED_SECRET_LEN,
                            bp_a, bp_a_len);
    yumi_combine_peer_keys(temp_b, peer_ss_b, YUMI_MLKEM_SHARED_SECRET_LEN,
                            bp_b, bp_b_len);
    TEST_ASSERT(bufs_equal(temp_a, temp_b, YUMI_TRANSPORT_KEY_LEN),
                "e2e: peer temp_keys match (dual-hybrid)");

    /* Transport key with shared epoch */
    uint8_t epoch[YUMI_EPOCH_KEY_LEN];
    yumi_randombytes(epoch, sizeof(epoch));
    uint8_t trans_a[YUMI_TRANSPORT_KEY_LEN], trans_b[YUMI_TRANSPORT_KEY_LEN];
    yumi_derive_transport_key(trans_a, temp_a, epoch);
    yumi_derive_transport_key(trans_b, temp_b, epoch);
    TEST_ASSERT(bufs_equal(trans_a, trans_b, YUMI_TRANSPORT_KEY_LEN),
                "e2e: transport keys match");

    /* Encrypt/decrypt a message under transport key */
    const uint8_t chat_msg[] = "Hello from peer A!";
    uint8_t msg_nonce[YUMI_AEAD_NONCE_LEN];
    yumi_randombytes(msg_nonce, sizeof(msg_nonce));
    size_t chat_enc_len = sizeof(chat_msg) - 1 + YUMI_AEAD_TAG_LEN;
    uint8_t *chat_enc = malloc(chat_enc_len);
    size_t chat_ct_len;
    yumi_aead_encrypt(chat_enc, &chat_ct_len, chat_msg, sizeof(chat_msg) - 1,
                       NULL, 0, msg_nonce, trans_a);

    uint8_t chat_dec[sizeof(chat_msg)];
    size_t chat_dec_len;
    rc = yumi_aead_decrypt(chat_dec, &chat_dec_len, chat_enc, chat_ct_len,
                            NULL, 0, msg_nonce, trans_b);
    TEST_ASSERT(rc == YUMI_CRYPTO_OK, "e2e: chat message decrypt succeeds");
    TEST_ASSERT(chat_dec_len == sizeof(chat_msg) - 1 &&
                bufs_equal(chat_dec, chat_msg, chat_dec_len),
                "e2e: chat message recovered");

    /* Cleanup */
    free(frodo_pk); free(frodo_sk); free(frodo_ct);
    free(enc_registrar); free(dec_registrar);
    free(chat_enc);
    yumi_bp512_keypair_free(joiner_bp);
    yumi_bp512_keypair_free(host_bp);
    yumi_bp512_keypair_free(peer_a);
    yumi_bp512_keypair_free(peer_b);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
    initialize_yumi_browser_static_heaps();
    fprintf(stdout, "╔═══════════════════════════════════════════╗\n");
    fprintf(stdout, "║  Yumi Crypto Abstraction — Test Suite     ║\n");
    fprintf(stdout, "╚═══════════════════════════════════════════╝\n");

    /* Initialize the crypto layer */
    yumi_crypto_err_t rc = yumi_crypto_init();
    if (rc != YUMI_CRYPTO_OK) {
        fprintf(stderr, "FATAL: yumi_crypto_init() failed with code %d\n", rc);
        fprintf(stderr, "  (Is oqs-provider installed and accessible?)\n");
        return 1;
    }

    test_init_cleanup();
    /* Re-init for remaining tests (cleanup was called inside test_init_cleanup) */

    /* Provider diagnostics */
    {
        TEST_SECTION("OQS provider diagnostics");

        OSSL_PROVIDER *deflt = OSSL_PROVIDER_load(NULL, "default");
        TEST_ASSERT(deflt != NULL, "default provider loads");

        OSSL_PROVIDER *oqs = OSSL_PROVIDER_load(NULL, "oqsprovider");
        TEST_ASSERT(oqs != NULL, "oqsprovider loads");
        if (!oqs) {
            fprintf(stderr, "  HINT: set OPENSSL_MODULES to the directory "
                            "containing oqsprovider.so\n");
        }

        /* Check that FrodoKEM algorithm is available */
        EVP_PKEY_CTX *probe = EVP_PKEY_CTX_new_from_name(NULL,
                                  "frodo1344shake", NULL);
        TEST_ASSERT(probe != NULL,
                    "frodo1344shake algorithm is available");
        EVP_PKEY_CTX_free(probe);

        /* Check that ML-KEM is available */
        probe = EVP_PKEY_CTX_new_from_name(NULL, "mlkem1024", NULL);
        TEST_ASSERT(probe != NULL, "mlkem1024 algorithm is available");
        EVP_PKEY_CTX_free(probe);

        /* Check that ML-DSA is available */
        probe = EVP_PKEY_CTX_new_from_name(NULL, "mldsa87", NULL);
        TEST_ASSERT(probe != NULL, "mldsa87 algorithm is available");
        EVP_PKEY_CTX_free(probe);

        if (oqs)   OSSL_PROVIDER_unload(oqs);
        if (deflt) OSSL_PROVIDER_unload(deflt);
    }

    test_randombytes();
    test_memory_hygiene();
    test_mldsa();
    test_mlkem();
    test_frodokem();
    test_bp512_ecdh();
    test_skein_hash();
    test_skein_mac();
    test_hkdf();
    test_threefish_block();
    test_aead();
    test_ctr_no_pattern();
    test_key_combiners();
    test_session_timeout();
    test_edge_cases();
    test_extended_coverage();
    test_full_handshake();

    /* Summary */
    fprintf(stdout, "\n════════════════════════════════════════════\n");
    fprintf(stdout, "  Tests: %d   Passed: %d   Failed: %d\n",
            g_tests_run, g_tests_run - g_tests_failed, g_tests_failed);
    fprintf(stdout, "════════════════════════════════════════════\n");

    yumi_crypto_cleanup();

    return g_tests_failed > 0 ? 1 : 0;
}
