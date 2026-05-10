/*
    Yumi Browser Cryptography Abstraction Layer (Implementation)
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
 * @file crypto.c
 * @brief Yumi Browser Cryptography Abstraction Layer — Implementation
 *
 * Wraps OpenSSL + oqs-provider (ML-DSA-87, ML-KEM-1024, FrodoKEM-1344),
 * BrainPool-512 ECDH, and embedded Skein-1024 / Threefish-1024 primitives.
 */

#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/provider.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#include <openssl/err.h>

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>

#include "skein.h"

/* Skein block processing (Threefish lives inside here) */
extern void Skein1024_Process_Block(Skein1024_Ctxt_t *ctx,
    const u08b_t *blkPtr, size_t blkCnt, size_t byteCntAdd);

/* ════════════════════════════════════════════════════════════════
 *  Module state
 * ════════════════════════════════════════════════════════════════ */

static OSSL_PROVIDER *s_default_prov = NULL;
static OSSL_PROVIDER *s_oqs_prov     = NULL;
static bool           s_initialized  = false;

/* ════════════════════════════════════════════════════════════════
 *  Skein / Threefish — Schneier et al. public domain reference
 *  implementation (embedded). See src/crypto/skein*.c
 * ════════════════════════════════════════════════════════════════ */

struct yumi_skein_ctx {
    Skein1024_Ctxt_t skein;
};

/* ──────────────────────────────────────────────────────────────── */

/*
 * Standalone Threefish-1024 encrypt: reuse the Skein block function
 * by wrapping it in a fake Skein1024 context.
 *
 * Skein1024_Process_Block does:
 *   1. key schedule from ctx->X[] + tweak
 *   2. 80 rounds of Threefish
 *   3. feedforward XOR: ctx->X[i] ^= plaintext[i]
 *
 * To get raw Threefish output (no feedforward), we undo step 3.
 */
static void threefish1024_encrypt(
    const uint64_t key[16],
    const uint64_t tweak[2],
    const uint64_t plain[16],
    uint64_t       cipher[16])
{
    Skein1024_Ctxt_t ctx;
    memcpy(ctx.X, key, sizeof(ctx.X));
    ctx.h.T[0] = tweak[0];
    ctx.h.T[1] = tweak[1];

    /* Process one block with byteCntAdd=0 so T[0] isn't modified */
    Skein1024_Process_Block(&ctx, (const uint8_t *)plain, 1, 0);

    /* Undo feedforward: ctx.X[i] = Threefish(plain)[i] ^ plain[i]
     * So: Threefish(plain)[i] = ctx.X[i] ^ plain[i] */
    for (int i = 0; i < 16; i++)
        cipher[i] = ctx.X[i] ^ plain[i];

    OPENSSL_cleanse(&ctx, sizeof(ctx));
}

static void skein1024_hash_internal(
    const uint8_t *data, size_t data_len,
    uint8_t out[YUMI_SKEIN_HASH_LEN])
{
    Skein1024_Ctxt_t ctx;
    Skein1024_Init(&ctx, 1024);             /* 1024-bit output */
    if (data && data_len > 0)
        Skein1024_Update(&ctx, data, data_len);
    Skein1024_Final(&ctx, out);
    OPENSSL_cleanse(&ctx, sizeof(ctx));
}

static void skein1024_mac_internal(
    const uint8_t *key, size_t key_len,
    const uint8_t *data, size_t data_len,
    uint8_t out[YUMI_SKEIN_MAC_LEN])
{
    Skein1024_Ctxt_t ctx;
    /* InitExt with key → Skein-MAC mode (key block processed first) */
    Skein1024_InitExt(&ctx, 1024,
                      SKEIN_CFG_TREE_INFO_SEQUENTIAL,
                      key, key_len);
    if (data && data_len > 0)
        Skein1024_Update(&ctx, data, data_len);
    Skein1024_Final(&ctx, out);
    OPENSSL_cleanse(&ctx, sizeof(ctx));
}

/* ════════════════════════════════════════════════════════════════
 *  BrainPool-512 opaque type
 * ════════════════════════════════════════════════════════════════ */

struct yumi_bp512_keypair {
    EVP_PKEY *pkey;
};

/* ════════════════════════════════════════════════════════════════
 *  Initialization / Cleanup
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_crypto_init(void) {
    if (s_initialized)
        return YUMI_CRYPTO_OK;

    /* Load the default OpenSSL provider */
    s_default_prov = OSSL_PROVIDER_load(NULL, "default");
    if (!s_default_prov)
        return YUMI_CRYPTO_ERR_INIT;

    /* Load the OQS provider for post-quantum algorithms */
    s_oqs_prov = OSSL_PROVIDER_load(NULL, "oqsprovider");
    if (!s_oqs_prov) {
        OSSL_PROVIDER_unload(s_default_prov);
        s_default_prov = NULL;
        return YUMI_CRYPTO_ERR_PROVIDER;
    }

    s_initialized = true;
    return YUMI_CRYPTO_OK;
}

void yumi_crypto_cleanup(void) {
    if (!s_initialized)
        return;

    if (s_oqs_prov) {
        OSSL_PROVIDER_unload(s_oqs_prov);
        s_oqs_prov = NULL;
    }
    if (s_default_prov) {
        OSSL_PROVIDER_unload(s_default_prov);
        s_default_prov = NULL;
    }
    s_initialized = false;
}

/* ════════════════════════════════════════════════════════════════
 *  Random bytes
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_randombytes(uint8_t *buf, size_t len) {
    if (!buf)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    /* RAND_bytes takes int; process in chunks to avoid truncation */
    size_t remaining = len;
    uint8_t *p = buf;
    while (remaining > 0) {
        int chunk = (remaining > (size_t)INT_MAX) ? INT_MAX : (int)remaining;
        if (RAND_bytes(p, chunk) != 1)
            return YUMI_CRYPTO_ERR_RNG;
        p += chunk;
        remaining -= (size_t)chunk;
    }
    return YUMI_CRYPTO_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Memory hygiene
 * ════════════════════════════════════════════════════════════════ */

void yumi_memzero(void *buf, size_t len) {
    if (buf)
        OPENSSL_cleanse(buf, len);
}

int yumi_memcmp(const uint8_t *a, const uint8_t *b, size_t len) {
    if (!a || !b)
        return -1;
    return CRYPTO_memcmp(a, b, len);
}

/* ════════════════════════════════════════════════════════════════
 *  OQS helper — generic EVP_PKEY sign/verify/KEM
 * ════════════════════════════════════════════════════════════════ */

static yumi_crypto_err_t oqs_keygen(const char *alg_name,
                                     uint8_t *pk, size_t pk_len,
                                     uint8_t *sk, size_t sk_len) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, alg_name, NULL);
    if (!ctx)
        return YUMI_CRYPTO_ERR_KEYGEN;

    EVP_PKEY *pkey = NULL;
    yumi_crypto_err_t rc = YUMI_CRYPTO_ERR_KEYGEN;

    if (EVP_PKEY_keygen_init(ctx) <= 0)
        goto done;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0)
        goto done;

    size_t got_pk = pk_len;
    size_t got_sk = sk_len;
    if (EVP_PKEY_get_raw_public_key(pkey, pk, &got_pk) != 1)
        goto done;
    if (EVP_PKEY_get_raw_private_key(pkey, sk, &got_sk) != 1)
        goto done;

    rc = YUMI_CRYPTO_OK;

done:
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    return rc;
}

/* ════════════════════════════════════════════════════════════════
 *  ML-DSA-87 — Post-quantum digital signatures
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_mldsa_keygen(
    uint8_t pk[YUMI_MLDSA_PUBLIC_KEY_LEN],
    uint8_t sk[YUMI_MLDSA_SECRET_KEY_LEN])
{
    if (!pk || !sk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    return oqs_keygen("mldsa87", pk, YUMI_MLDSA_PUBLIC_KEY_LEN,
                      sk, YUMI_MLDSA_SECRET_KEY_LEN);
}

yumi_crypto_err_t yumi_mldsa_sign(
    uint8_t       sig[YUMI_MLDSA_SIGN_LEN],
    const uint8_t *msg, size_t msg_len,
    const uint8_t sk[YUMI_MLDSA_SECRET_KEY_LEN])
{
    if (!sig || !msg || !sk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key_ex(
        NULL, "mldsa87", NULL, sk, YUMI_MLDSA_SECRET_KEY_LEN);
    if (!pkey)
        return YUMI_CRYPTO_ERR_SIGN;

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    yumi_crypto_err_t rc = YUMI_CRYPTO_ERR_SIGN;

    if (!mdctx)
        goto done;
    if (EVP_DigestSignInit(mdctx, NULL, NULL, NULL, pkey) != 1)
        goto done;

    size_t sig_len = YUMI_MLDSA_SIGN_LEN;
    if (EVP_DigestSign(mdctx, sig, &sig_len, msg, msg_len) != 1)
        goto done;

    rc = YUMI_CRYPTO_OK;

done:
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    return rc;
}

yumi_crypto_err_t yumi_mldsa_verify(
    const uint8_t sig[YUMI_MLDSA_SIGN_LEN],
    const uint8_t *msg, size_t msg_len,
    const uint8_t pk[YUMI_MLDSA_PUBLIC_KEY_LEN])
{
    if (!sig || !msg || !pk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key_ex(
        NULL, "mldsa87", NULL, pk, YUMI_MLDSA_PUBLIC_KEY_LEN);
    if (!pkey)
        return YUMI_CRYPTO_ERR_VERIFY;

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    yumi_crypto_err_t rc = YUMI_CRYPTO_ERR_VERIFY;

    if (!mdctx)
        goto done;
    if (EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, pkey) != 1)
        goto done;
    if (EVP_DigestVerify(mdctx, sig, YUMI_MLDSA_SIGN_LEN, msg, msg_len) != 1)
        goto done;

    rc = YUMI_CRYPTO_OK;

done:
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    return rc;
}

/* ════════════════════════════════════════════════════════════════
 *  ML-KEM-1024 — Post-quantum key encapsulation
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_mlkem_keygen(
    uint8_t pk[YUMI_MLKEM_PUBLIC_KEY_LEN],
    uint8_t sk[YUMI_MLKEM_SECRET_KEY_LEN])
{
    if (!pk || !sk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    return oqs_keygen("mlkem1024", pk, YUMI_MLKEM_PUBLIC_KEY_LEN,
                      sk, YUMI_MLKEM_SECRET_KEY_LEN);
}

yumi_crypto_err_t yumi_mlkem_encaps(
    uint8_t ct[YUMI_MLKEM_CIPHERTEXT_LEN],
    uint8_t ss[YUMI_MLKEM_SHARED_SECRET_LEN],
    const uint8_t pk[YUMI_MLKEM_PUBLIC_KEY_LEN])
{
    if (!ct || !ss || !pk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key_ex(
        NULL, "mlkem1024", NULL, pk, YUMI_MLKEM_PUBLIC_KEY_LEN);
    if (!pkey)
        return YUMI_CRYPTO_ERR_ENCAPS;

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
    yumi_crypto_err_t rc = YUMI_CRYPTO_ERR_ENCAPS;

    if (!ctx)
        goto done;
    if (EVP_PKEY_encapsulate_init(ctx, NULL) != 1)
        goto done;

    size_t ct_len = YUMI_MLKEM_CIPHERTEXT_LEN;
    size_t ss_len = YUMI_MLKEM_SHARED_SECRET_LEN;
    if (EVP_PKEY_encapsulate(ctx, ct, &ct_len, ss, &ss_len) != 1)
        goto done;

    rc = YUMI_CRYPTO_OK;

done:
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return rc;
}

yumi_crypto_err_t yumi_mlkem_decaps(
    uint8_t ss[YUMI_MLKEM_SHARED_SECRET_LEN],
    const uint8_t ct[YUMI_MLKEM_CIPHERTEXT_LEN],
    const uint8_t sk[YUMI_MLKEM_SECRET_KEY_LEN])
{
    if (!ss || !ct || !sk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key_ex(
        NULL, "mlkem1024", NULL, sk, YUMI_MLKEM_SECRET_KEY_LEN);
    if (!pkey)
        return YUMI_CRYPTO_ERR_DECAPS;

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
    yumi_crypto_err_t rc = YUMI_CRYPTO_ERR_DECAPS;

    if (!ctx)
        goto done;
    if (EVP_PKEY_decapsulate_init(ctx, NULL) != 1)
        goto done;

    size_t ss_len = YUMI_MLKEM_SHARED_SECRET_LEN;
    if (EVP_PKEY_decapsulate(ctx, ss, &ss_len, ct, YUMI_MLKEM_CIPHERTEXT_LEN) != 1)
        goto done;

    rc = YUMI_CRYPTO_OK;

done:
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return rc;
}

/* ════════════════════════════════════════════════════════════════
 *  FrodoKEM-1344-SHAKE — Plain LWE key encapsulation
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_frodo_keygen(
    uint8_t pk[YUMI_FRODO_PUBLIC_KEY_LEN],
    uint8_t sk[YUMI_FRODO_SECRET_KEY_LEN])
{
    if (!pk || !sk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    return oqs_keygen("frodo1344shake", pk, YUMI_FRODO_PUBLIC_KEY_LEN,
                      sk, YUMI_FRODO_SECRET_KEY_LEN);
}

/* Helper: import raw key bytes into an EVP_PKEY via EVP_PKEY_fromdata.
 * Works with oqs-provider algorithms that don't support
 * EVP_PKEY_new_raw_{public,private}_key_ex. */
static EVP_PKEY *oqs_import_key(const char *alg_name,
                                const uint8_t *key_data, size_t key_len,
                                int is_private)
{
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_from_name(NULL, alg_name, NULL);
    if (!kctx)
        return NULL;

    EVP_PKEY *pkey = NULL;
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    if (!bld) {
        EVP_PKEY_CTX_free(kctx);
        return NULL;
    }

    const char *param_name = is_private ? OSSL_PKEY_PARAM_PRIV_KEY
                                        : OSSL_PKEY_PARAM_PUB_KEY;
    /* Cast away const — OSSL_PARAM_BLD_push_octet_string copies the data */
    OSSL_PARAM_BLD_push_octet_string(bld, param_name,
                                      (void *)key_data, key_len);

    OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
    OSSL_PARAM_BLD_free(bld);
    if (!params) {
        EVP_PKEY_CTX_free(kctx);
        return NULL;
    }

    int selection = is_private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
    if (EVP_PKEY_fromdata_init(kctx) <= 0 ||
        EVP_PKEY_fromdata(kctx, &pkey, selection, params) <= 0) {
        pkey = NULL;
    }

    OSSL_PARAM_free(params);
    EVP_PKEY_CTX_free(kctx);
    return pkey;
}

yumi_crypto_err_t yumi_frodo_encaps(
    uint8_t ct[YUMI_FRODO_CIPHERTEXT_LEN],
    uint8_t ss[YUMI_FRODO_SHARED_SECRET_LEN],
    const uint8_t pk[YUMI_FRODO_PUBLIC_KEY_LEN])
{
    if (!ct || !ss || !pk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    EVP_PKEY *pkey = oqs_import_key("frodo1344shake",
                                     pk, YUMI_FRODO_PUBLIC_KEY_LEN, 0);
    if (!pkey)
        return YUMI_CRYPTO_ERR_ENCAPS;

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
    yumi_crypto_err_t rc = YUMI_CRYPTO_ERR_ENCAPS;

    if (!ctx)
        goto done;
    if (EVP_PKEY_encapsulate_init(ctx, NULL) != 1)
        goto done;

    /* Query actual sizes from the provider */
    size_t ct_len = 0, ss_len = 0;
    if (EVP_PKEY_encapsulate(ctx, NULL, &ct_len, NULL, &ss_len) != 1)
        goto done;
    if (ct_len > YUMI_FRODO_CIPHERTEXT_LEN || ss_len > YUMI_FRODO_SHARED_SECRET_LEN)
        goto done;
    if (EVP_PKEY_encapsulate(ctx, ct, &ct_len, ss, &ss_len) != 1)
        goto done;

    rc = YUMI_CRYPTO_OK;

done:
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return rc;
}

yumi_crypto_err_t yumi_frodo_decaps(
    uint8_t ss[YUMI_FRODO_SHARED_SECRET_LEN],
    const uint8_t ct[YUMI_FRODO_CIPHERTEXT_LEN],
    const uint8_t sk[YUMI_FRODO_SECRET_KEY_LEN])
{
    if (!ss || !ct || !sk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    EVP_PKEY *pkey = oqs_import_key("frodo1344shake",
                                     sk, YUMI_FRODO_SECRET_KEY_LEN, 1);
    if (!pkey)
        return YUMI_CRYPTO_ERR_DECAPS;

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL);
    yumi_crypto_err_t rc = YUMI_CRYPTO_ERR_DECAPS;

    if (!ctx)
        goto done;
    if (EVP_PKEY_decapsulate_init(ctx, NULL) != 1)
        goto done;

    size_t ss_len = YUMI_FRODO_SHARED_SECRET_LEN;
    size_t ct_in_len = YUMI_FRODO_CIPHERTEXT_LEN;
    if (EVP_PKEY_decapsulate(ctx, ss, &ss_len, ct, ct_in_len) != 1)
        goto done;

    rc = YUMI_CRYPTO_OK;

done:
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return rc;
}

/* ════════════════════════════════════════════════════════════════
 *  BrainPool-512 ECDH
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_bp512_keygen(yumi_bp512_keypair_t **kp) {
    if (!kp)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (!pctx)
        return YUMI_CRYPTO_ERR_KEYGEN;

    yumi_crypto_err_t rc = YUMI_CRYPTO_ERR_KEYGEN;
    EVP_PKEY *pkey = NULL;

    if (EVP_PKEY_keygen_init(pctx) <= 0)
        goto done;

    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string(
        OSSL_PKEY_PARAM_GROUP_NAME, "brainpoolP512r1", 0);
    params[1] = OSSL_PARAM_construct_end();

    if (EVP_PKEY_CTX_set_params(pctx, params) <= 0)
        goto done;
    if (EVP_PKEY_keygen(pctx, &pkey) <= 0)
        goto done;

    yumi_bp512_keypair_t *out = calloc(1, sizeof(*out));
    if (!out) {
        EVP_PKEY_free(pkey);
        rc = YUMI_CRYPTO_ERR_KEYGEN;
        goto done;
    }
    out->pkey = pkey;
    pkey = NULL;  /* ownership transferred */
    *kp = out;
    rc = YUMI_CRYPTO_OK;

done:
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(pctx);
    return rc;
}

void yumi_bp512_keypair_free(yumi_bp512_keypair_t *kp) {
    if (kp) {
        EVP_PKEY_free(kp->pkey);
        yumi_memzero(kp, sizeof(*kp));
        free(kp);
    }
}

yumi_crypto_err_t yumi_bp512_get_public_key(
    const yumi_bp512_keypair_t *kp,
    uint8_t pk[YUMI_BP512_PUBLIC_KEY_LEN],
    size_t *pk_len)
{
    if (!kp || !pk || !pk_len)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    size_t len = YUMI_BP512_PUBLIC_KEY_LEN;
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_octet_string(
        OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, pk, len);
    params[1] = OSSL_PARAM_construct_end();

    if (!EVP_PKEY_get_params(kp->pkey, params))
        return YUMI_CRYPTO_ERR_ECDH;

    *pk_len = params[0].return_size;
    return YUMI_CRYPTO_OK;
}

yumi_crypto_err_t yumi_bp512_ecdh(
    uint8_t ss[YUMI_BP512_SHARED_SECRET_LEN],
    size_t *ss_len,
    const yumi_bp512_keypair_t *our_kp,
    const uint8_t *peer_pk, size_t peer_pk_len)
{
    if (!ss || !ss_len || !our_kp || !peer_pk)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    EVP_PKEY *peer_key = NULL;
    yumi_crypto_err_t rc = YUMI_CRYPTO_ERR_ECDH;

    /* Build the peer key from raw public point */
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    if (!bld)
        goto done;

    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                     "brainpoolP512r1", 0);
    OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                      peer_pk, peer_pk_len);

    OSSL_PARAM *peer_params = OSSL_PARAM_BLD_to_param(bld);
    OSSL_PARAM_BLD_free(bld);
    if (!peer_params)
        goto done;

    EVP_PKEY_CTX *fromdata_ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (!fromdata_ctx) {
        OSSL_PARAM_free(peer_params);
        goto done;
    }

    if (EVP_PKEY_fromdata_init(fromdata_ctx) <= 0 ||
        EVP_PKEY_fromdata(fromdata_ctx, &peer_key,
                          EVP_PKEY_PUBLIC_KEY, peer_params) <= 0) {
        OSSL_PARAM_free(peer_params);
        EVP_PKEY_CTX_free(fromdata_ctx);
        goto done;
    }
    OSSL_PARAM_free(peer_params);
    EVP_PKEY_CTX_free(fromdata_ctx);

    /* Perform ECDH */
    EVP_PKEY_CTX *derive_ctx = EVP_PKEY_CTX_new_from_pkey(NULL, our_kp->pkey, NULL);
    if (!derive_ctx)
        goto done;

    if (EVP_PKEY_derive_init(derive_ctx) <= 0)
        goto done_derive;
    if (EVP_PKEY_derive_set_peer(derive_ctx, peer_key) <= 0)
        goto done_derive;

    size_t len = YUMI_BP512_SHARED_SECRET_LEN;
    if (EVP_PKEY_derive(derive_ctx, ss, &len) <= 0)
        goto done_derive;

    *ss_len = len;
    rc = YUMI_CRYPTO_OK;

done_derive:
    EVP_PKEY_CTX_free(derive_ctx);
done:
    EVP_PKEY_free(peer_key);
    return rc;
}

/* ════════════════════════════════════════════════════════════════
 *  Skein-1024 — Hashing
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_skein_hash(
    uint8_t out[YUMI_SKEIN_HASH_LEN],
    const uint8_t *data, size_t data_len)
{
    if (!out || (!data && data_len > 0))
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    skein1024_hash_internal(data, data_len, out);
    return YUMI_CRYPTO_OK;
}

yumi_crypto_err_t yumi_skein_init(yumi_skein_ctx_t **ctx) {
    if (!ctx)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    yumi_skein_ctx_t *c = calloc(1, sizeof(*c));
    if (!c)
        return YUMI_CRYPTO_ERR_HASH;
    if (Skein1024_Init(&c->skein, 1024) != SKEIN_SUCCESS) {
        free(c);
        return YUMI_CRYPTO_ERR_HASH;
    }
    *ctx = c;
    return YUMI_CRYPTO_OK;
}

yumi_crypto_err_t yumi_skein_update(yumi_skein_ctx_t *ctx,
                                     const uint8_t *data, size_t data_len)
{
    if (!ctx || (!data && data_len > 0))
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    if (Skein1024_Update(&ctx->skein, data, data_len) != SKEIN_SUCCESS)
        return YUMI_CRYPTO_ERR_HASH;
    return YUMI_CRYPTO_OK;
}

yumi_crypto_err_t yumi_skein_final(yumi_skein_ctx_t *ctx,
                                    uint8_t out[YUMI_SKEIN_HASH_LEN])
{
    if (!ctx || !out)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    if (Skein1024_Final(&ctx->skein, out) != SKEIN_SUCCESS)
        return YUMI_CRYPTO_ERR_HASH;
    return YUMI_CRYPTO_OK;
}

void yumi_skein_free(yumi_skein_ctx_t *ctx) {
    if (ctx) {
        yumi_memzero(ctx, sizeof(*ctx));
        free(ctx);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Skein-1024-MAC
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_skein_mac(
    uint8_t mac[YUMI_SKEIN_MAC_LEN],
    const uint8_t *key, size_t key_len,
    const uint8_t *data, size_t data_len)
{
    if (!mac || !key || (!data && data_len > 0))
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    skein1024_mac_internal(key, key_len, data, data_len, mac);
    return YUMI_CRYPTO_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  HKDF — Built on Skein-1024-MAC
 *
 *  Extract: PRK = Skein-MAC(salt, ikm)
 *  Expand:  T(1) = Skein-MAC(PRK, info || 0x01)
 *           T(n) = Skein-MAC(PRK, T(n-1) || info || n)
 *           OKM  = T(1) || T(2) || ... truncated to okm_len
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_hkdf(
    uint8_t *okm, size_t okm_len,
    const uint8_t *ikm, size_t ikm_len,
    const uint8_t *salt, size_t salt_len,
    const uint8_t *info, size_t info_len)
{
    if (!okm || !ikm)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    /* Extract */
    uint8_t zero_salt[YUMI_SKEIN_HASH_LEN];
    if (!salt) {
        memset(zero_salt, 0, YUMI_SKEIN_HASH_LEN);
        salt = zero_salt;
        salt_len = YUMI_SKEIN_HASH_LEN;
    }

    uint8_t prk[YUMI_SKEIN_MAC_LEN];
    skein1024_mac_internal(salt, salt_len, ikm, ikm_len, prk);

    /* Expand */
    uint8_t t_prev[YUMI_SKEIN_MAC_LEN];
    size_t  t_prev_len = 0;
    size_t  offset = 0;
    uint8_t counter = 1;

    while (offset < okm_len) {
        /* Build: T(n-1) || info || counter */
        size_t input_len = t_prev_len + info_len + 1;
        uint8_t *input = malloc(input_len);
        if (!input) {
            yumi_memzero(prk, sizeof(prk));
            return YUMI_CRYPTO_ERR_KDF;
        }

        size_t pos = 0;
        if (t_prev_len > 0) {
            memcpy(input, t_prev, t_prev_len);
            pos += t_prev_len;
        }
        if (info && info_len > 0) {
            memcpy(input + pos, info, info_len);
            pos += info_len;
        }
        input[pos] = counter;

        uint8_t t_cur[YUMI_SKEIN_MAC_LEN];
        skein1024_mac_internal(prk, YUMI_SKEIN_MAC_LEN, input, input_len, t_cur);

        yumi_memzero(input, input_len);
        free(input);

        size_t to_copy = okm_len - offset;
        if (to_copy > YUMI_SKEIN_MAC_LEN)
            to_copy = YUMI_SKEIN_MAC_LEN;
        memcpy(okm + offset, t_cur, to_copy);

        memcpy(t_prev, t_cur, YUMI_SKEIN_MAC_LEN);
        t_prev_len = YUMI_SKEIN_MAC_LEN;
        yumi_memzero(t_cur, sizeof(t_cur));

        offset += to_copy;
        counter++;
    }

    yumi_memzero(prk, sizeof(prk));
    yumi_memzero(t_prev, sizeof(t_prev));
    return YUMI_CRYPTO_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  AEAD — Threefish-1024-CTR + Skein-1024-MAC
 *
 *  Encrypt-then-MAC:
 *    1. CTR encrypt plaintext with Threefish-1024
 *    2. MAC = Skein-MAC(key, nonce || aad || ciphertext)
 *    3. Output = ciphertext || MAC
 * ════════════════════════════════════════════════════════════════ */

/* Internal: Threefish-1024 in CTR mode */
static yumi_crypto_err_t threefish_ctr(
    uint8_t *out,
    const uint8_t *in, size_t in_len,
    const uint8_t key[YUMI_THREEFISH_KEY_LEN],
    const uint8_t nonce[YUMI_AEAD_NONCE_LEN])
{
    uint64_t key_words[16];
    memcpy(key_words, key, YUMI_THREEFISH_KEY_LEN);

    /* Counter block as uint64_t for direct use with threefish1024_encrypt */
    uint64_t counter[16];
    memset(counter, 0, sizeof(counter));
    memcpy(counter, nonce, YUMI_AEAD_NONCE_LEN);

    uint64_t tweak[2] = {0, 0};
    uint64_t cipher_words[16];
    size_t offset = 0;

    while (offset < in_len) {
        threefish1024_encrypt(key_words, tweak, counter, cipher_words);

        size_t chunk = in_len - offset;
        if (chunk > YUMI_THREEFISH_BLOCK_LEN)
            chunk = YUMI_THREEFISH_BLOCK_LEN;

        /* 64-bit XOR for full words, byte XOR for tail */
        const uint8_t *src = in + offset;
        uint8_t *dst = out + offset;
        const uint8_t *ks = (const uint8_t *)cipher_words;
        size_t full_words = chunk / 8;
        size_t tail_bytes = chunk % 8;

        for (size_t w = 0; w < full_words; w++) {
            uint64_t s;
            memcpy(&s, src + w * 8, 8);
            s ^= cipher_words[w];
            memcpy(dst + w * 8, &s, 8);
        }
        for (size_t i = full_words * 8; i < chunk; i++)
            dst[i] = src[i] ^ ks[i];

        /* Increment counter (last 8 bytes of counter block) */
        counter[15]++;

        offset += chunk;
    }

    yumi_memzero(key_words, sizeof(key_words));
    yumi_memzero(counter, sizeof(counter));
    yumi_memzero(cipher_words, sizeof(cipher_words));
    return YUMI_CRYPTO_OK;
}

yumi_crypto_err_t yumi_aead_encrypt(
    uint8_t       *out, size_t *out_len,
    const uint8_t *plaintext, size_t plaintext_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t  nonce[YUMI_AEAD_NONCE_LEN],
    const uint8_t  key[YUMI_AEAD_KEY_LEN])
{
    if (!out || !out_len || !key || !nonce)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    if (!plaintext && plaintext_len > 0)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    /* Derive separate subkeys for encryption and MAC.
     * enc_key = Skein-MAC(key, "yumi-aead-enc")
     * mac_key = Skein-MAC(key, "yumi-aead-mac") */
    uint8_t enc_key[YUMI_THREEFISH_KEY_LEN];
    uint8_t mac_key[YUMI_SKEIN_MAC_LEN];
    static const uint8_t enc_label[] = "yumi-aead-enc";
    static const uint8_t mac_label[] = "yumi-aead-mac";
    skein1024_mac_internal(key, YUMI_AEAD_KEY_LEN,
                           enc_label, sizeof(enc_label) - 1, enc_key);
    skein1024_mac_internal(key, YUMI_AEAD_KEY_LEN,
                           mac_label, sizeof(mac_label) - 1, mac_key);

    /* Step 1: CTR encrypt with enc_key */
    yumi_crypto_err_t rc = threefish_ctr(out, plaintext, plaintext_len,
                                          enc_key, nonce);
    yumi_memzero(enc_key, sizeof(enc_key));
    if (rc != YUMI_CRYPTO_OK) {
        yumi_memzero(mac_key, sizeof(mac_key));
        return rc;
    }

    /* Step 2: MAC over nonce || le64(aad_len) || aad || ciphertext.
     * The 8-byte length prefix prevents ambiguity when AAD is variable. */
    size_t mac_input_len = YUMI_AEAD_NONCE_LEN + 8 + aad_len + plaintext_len;
    uint8_t *mac_input = malloc(mac_input_len);
    if (!mac_input) {
        yumi_memzero(mac_key, sizeof(mac_key));
        return YUMI_CRYPTO_ERR_ENCRYPT;
    }

    size_t pos = 0;
    memcpy(mac_input + pos, nonce, YUMI_AEAD_NONCE_LEN);
    pos += YUMI_AEAD_NONCE_LEN;
    uint64_t aad_len_le = (uint64_t)aad_len;
    memcpy(mac_input + pos, &aad_len_le, 8);
    pos += 8;
    if (aad && aad_len > 0) {
        memcpy(mac_input + pos, aad, aad_len);
        pos += aad_len;
    }
    memcpy(mac_input + pos, out, plaintext_len);

    uint8_t *tag = out + plaintext_len;
    skein1024_mac_internal(mac_key, YUMI_SKEIN_MAC_LEN,
                           mac_input, mac_input_len, tag);

    yumi_memzero(mac_key, sizeof(mac_key));
    yumi_memzero(mac_input, mac_input_len);
    free(mac_input);

    *out_len = plaintext_len + YUMI_AEAD_TAG_LEN;
    return YUMI_CRYPTO_OK;
}

yumi_crypto_err_t yumi_aead_decrypt(
    uint8_t       *out, size_t *out_len,
    const uint8_t *ciphertext, size_t ciphertext_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t  nonce[YUMI_AEAD_NONCE_LEN],
    const uint8_t  key[YUMI_AEAD_KEY_LEN])
{
    if (!out || !out_len || !ciphertext || !key || !nonce)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    if (ciphertext_len < YUMI_AEAD_TAG_LEN)
        return YUMI_CRYPTO_ERR_DECRYPT;

    size_t ct_only_len = ciphertext_len - YUMI_AEAD_TAG_LEN;
    const uint8_t *received_tag = ciphertext + ct_only_len;

    /* Derive separate subkeys for encryption and MAC */
    uint8_t enc_key[YUMI_THREEFISH_KEY_LEN];
    uint8_t mac_key[YUMI_SKEIN_MAC_LEN];
    static const uint8_t enc_label[] = "yumi-aead-enc";
    static const uint8_t mac_label[] = "yumi-aead-mac";
    skein1024_mac_internal(key, YUMI_AEAD_KEY_LEN,
                           enc_label, sizeof(enc_label) - 1, enc_key);
    skein1024_mac_internal(key, YUMI_AEAD_KEY_LEN,
                           mac_label, sizeof(mac_label) - 1, mac_key);

    /* Verify MAC first (encrypt-then-MAC: verify before decrypt).
     * MAC input: nonce || le64(aad_len) || aad || ciphertext. */
    size_t mac_input_len = YUMI_AEAD_NONCE_LEN + 8 + aad_len + ct_only_len;
    uint8_t *mac_input = malloc(mac_input_len);
    if (!mac_input) {
        yumi_memzero(enc_key, sizeof(enc_key));
        yumi_memzero(mac_key, sizeof(mac_key));
        return YUMI_CRYPTO_ERR_DECRYPT;
    }

    size_t pos = 0;
    memcpy(mac_input + pos, nonce, YUMI_AEAD_NONCE_LEN);
    pos += YUMI_AEAD_NONCE_LEN;
    uint64_t aad_len_le = (uint64_t)aad_len;
    memcpy(mac_input + pos, &aad_len_le, 8);
    pos += 8;
    if (aad && aad_len > 0) {
        memcpy(mac_input + pos, aad, aad_len);
        pos += aad_len;
    }
    memcpy(mac_input + pos, ciphertext, ct_only_len);

    uint8_t computed_tag[YUMI_AEAD_TAG_LEN];
    skein1024_mac_internal(mac_key, YUMI_SKEIN_MAC_LEN,
                           mac_input, mac_input_len, computed_tag);

    yumi_memzero(mac_input, mac_input_len);
    free(mac_input);
    yumi_memzero(mac_key, sizeof(mac_key));

    /* Constant-time comparison */
    if (CRYPTO_memcmp(computed_tag, received_tag, YUMI_AEAD_TAG_LEN) != 0) {
        yumi_memzero(computed_tag, sizeof(computed_tag));
        yumi_memzero(enc_key, sizeof(enc_key));
        return YUMI_CRYPTO_ERR_DECRYPT;
    }
    yumi_memzero(computed_tag, sizeof(computed_tag));

    /* CTR decrypt (same operation as encrypt) */
    yumi_crypto_err_t rc = threefish_ctr(out, ciphertext, ct_only_len,
                                          enc_key, nonce);
    yumi_memzero(enc_key, sizeof(enc_key));
    if (rc != YUMI_CRYPTO_OK)
        return rc;

    *out_len = ct_only_len;
    return YUMI_CRYPTO_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  AEAD — Pre-derived subkeys for high-throughput paths
 * ════════════════════════════════════════════════════════════════ */

_Static_assert(sizeof(Skein1024_Ctxt_t) <= YUMI_AEAD_MAC_CTX_SIZE,
               "YUMI_AEAD_MAC_CTX_SIZE too small for Skein1024_Ctxt_t");

yumi_crypto_err_t yumi_aead_derive_subkeys(
    yumi_aead_subkeys_t *out,
    const uint8_t key[YUMI_AEAD_KEY_LEN])
{
    if (!out || !key)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    static const uint8_t enc_label[] = "yumi-aead-enc";
    static const uint8_t mac_label[] = "yumi-aead-mac";
    skein1024_mac_internal(key, YUMI_AEAD_KEY_LEN,
                           enc_label, sizeof(enc_label) - 1, out->enc_key);
    skein1024_mac_internal(key, YUMI_AEAD_KEY_LEN,
                           mac_label, sizeof(mac_label) - 1, out->mac_key);

    /* Cache a pre-keyed Skein-MAC context so the per-packet MAC
     * skips the expensive key-block processing (saves 2 Threefish
     * operations per MAC call). */
    Skein1024_Ctxt_t *cached = (Skein1024_Ctxt_t *)out->_mac_ctx;
    Skein1024_InitExt(cached, 1024,
                      SKEIN_CFG_TREE_INFO_SEQUENTIAL,
                      out->mac_key, YUMI_SKEIN_MAC_LEN);

    return YUMI_CRYPTO_OK;
}

void yumi_aead_subkeys_wipe(yumi_aead_subkeys_t *sk)
{
    if (sk) yumi_memzero(sk, sizeof(*sk));
}

yumi_crypto_err_t yumi_aead_encrypt_keyed(
    uint8_t       *out, size_t *out_len,
    const uint8_t *plaintext, size_t plaintext_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t  nonce[YUMI_AEAD_NONCE_LEN],
    const yumi_aead_subkeys_t *sk)
{
    if (!out || !out_len || !sk || !nonce)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    if (!plaintext && plaintext_len > 0)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    /* CTR encrypt with pre-derived enc_key */
    yumi_crypto_err_t rc = threefish_ctr(out, plaintext, plaintext_len,
                                          sk->enc_key, nonce);
    if (rc != YUMI_CRYPTO_OK)
        return rc;

    /* Streaming MAC: nonce || le64(aad_len) || aad || ct.
     * Length prefix prevents AAD/ct boundary ambiguity. */
    Skein1024_Ctxt_t ctx;
    memcpy(&ctx, sk->_mac_ctx, sizeof(ctx));
    Skein1024_Update(&ctx, nonce, YUMI_AEAD_NONCE_LEN);
    uint64_t aad_len_le = (uint64_t)aad_len;
    Skein1024_Update(&ctx, (const uint8_t *)&aad_len_le, 8);
    if (aad && aad_len > 0)
        Skein1024_Update(&ctx, aad, aad_len);
    if (plaintext_len > 0)
        Skein1024_Update(&ctx, out, plaintext_len);

    uint8_t *tag = out + plaintext_len;
    Skein1024_Final(&ctx, tag);
    OPENSSL_cleanse(&ctx, sizeof(ctx));

    *out_len = plaintext_len + YUMI_AEAD_TAG_LEN;
    return YUMI_CRYPTO_OK;
}

yumi_crypto_err_t yumi_aead_decrypt_keyed(
    uint8_t       *out, size_t *out_len,
    const uint8_t *ciphertext, size_t ciphertext_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t  nonce[YUMI_AEAD_NONCE_LEN],
    const yumi_aead_subkeys_t *sk)
{
    if (!out || !out_len || !ciphertext || !sk || !nonce)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;
    if (ciphertext_len < YUMI_AEAD_TAG_LEN)
        return YUMI_CRYPTO_ERR_DECRYPT;

    size_t ct_only_len = ciphertext_len - YUMI_AEAD_TAG_LEN;
    const uint8_t *received_tag = ciphertext + ct_only_len;

    /* Streaming MAC verify: nonce || le64(aad_len) || aad || ct */
    Skein1024_Ctxt_t ctx;
    memcpy(&ctx, sk->_mac_ctx, sizeof(ctx));
    Skein1024_Update(&ctx, nonce, YUMI_AEAD_NONCE_LEN);
    uint64_t aad_len_le = (uint64_t)aad_len;
    Skein1024_Update(&ctx, (const uint8_t *)&aad_len_le, 8);
    if (aad && aad_len > 0)
        Skein1024_Update(&ctx, aad, aad_len);
    if (ct_only_len > 0)
        Skein1024_Update(&ctx, ciphertext, ct_only_len);

    uint8_t computed_tag[YUMI_AEAD_TAG_LEN];
    Skein1024_Final(&ctx, computed_tag);
    OPENSSL_cleanse(&ctx, sizeof(ctx));

    if (CRYPTO_memcmp(computed_tag, received_tag, YUMI_AEAD_TAG_LEN) != 0) {
        yumi_memzero(computed_tag, sizeof(computed_tag));
        return YUMI_CRYPTO_ERR_DECRYPT;
    }
    yumi_memzero(computed_tag, sizeof(computed_tag));

    /* CTR decrypt */
    yumi_crypto_err_t rc = threefish_ctr(out, ciphertext, ct_only_len,
                                          sk->enc_key, nonce);
    if (rc != YUMI_CRYPTO_OK)
        return rc;

    *out_len = ct_only_len;
    return YUMI_CRYPTO_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Threefish-1024 — Raw block cipher
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_threefish_encrypt_block(
    uint8_t out[YUMI_THREEFISH_BLOCK_LEN],
    const uint8_t in[YUMI_THREEFISH_BLOCK_LEN],
    const uint8_t key[YUMI_THREEFISH_KEY_LEN],
    const uint8_t tweak[YUMI_THREEFISH_TWEAK_LEN])
{
    if (!out || !in || !key || !tweak)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    uint64_t k[16], t[2], p[16], c[16];
    memcpy(k, key, YUMI_THREEFISH_KEY_LEN);
    memcpy(t, tweak, YUMI_THREEFISH_TWEAK_LEN);
    memcpy(p, in, YUMI_THREEFISH_BLOCK_LEN);

    threefish1024_encrypt(k, t, p, c);
    memcpy(out, c, YUMI_THREEFISH_BLOCK_LEN);

    yumi_memzero(k, sizeof(k));
    yumi_memzero(p, sizeof(p));
    yumi_memzero(c, sizeof(c));
    return YUMI_CRYPTO_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Handshake helpers — Key combiners
 * ════════════════════════════════════════════════════════════════ */

yumi_crypto_err_t yumi_combine_invite_keys(
    uint8_t session_key[YUMI_TRANSPORT_KEY_LEN],
    const uint8_t *k1, size_t k1_len,
    const uint8_t *k2, size_t k2_len,
    const uint8_t *k3, size_t k3_len)
{
    if (!session_key || !k1 || !k2 || !k3)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    /* Concatenate: K1 || K2 || K3 */
    size_t ikm_len = k1_len + k2_len + k3_len;
    uint8_t *ikm = malloc(ikm_len);
    if (!ikm)
        return YUMI_CRYPTO_ERR_KDF;

    size_t pos = 0;
    memcpy(ikm + pos, k1, k1_len); pos += k1_len;
    memcpy(ikm + pos, k2, k2_len); pos += k2_len;
    memcpy(ikm + pos, k3, k3_len);

    static const uint8_t info[] = "yumi-invite-v1";
    yumi_crypto_err_t rc = yumi_hkdf(session_key, YUMI_TRANSPORT_KEY_LEN,
                                      ikm, ikm_len,
                                      NULL, 0,
                                      info, sizeof(info) - 1);

    yumi_memzero(ikm, ikm_len);
    free(ikm);
    return rc;
}

yumi_crypto_err_t yumi_combine_peer_keys(
    uint8_t temp_key[YUMI_TRANSPORT_KEY_LEN],
    const uint8_t *k1, size_t k1_len,
    const uint8_t *k2, size_t k2_len)
{
    if (!temp_key || !k1 || !k2)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    /* Concatenate: K1 || K2 */
    size_t ikm_len = k1_len + k2_len;
    uint8_t *ikm = malloc(ikm_len);
    if (!ikm)
        return YUMI_CRYPTO_ERR_KDF;

    memcpy(ikm, k1, k1_len);
    memcpy(ikm + k1_len, k2, k2_len);

    static const uint8_t info[] = "yumi-peer-v1";
    yumi_crypto_err_t rc = yumi_hkdf(temp_key, YUMI_TRANSPORT_KEY_LEN,
                                      ikm, ikm_len,
                                      NULL, 0,
                                      info, sizeof(info) - 1);

    yumi_memzero(ikm, ikm_len);
    free(ikm);
    return rc;
}

yumi_crypto_err_t yumi_derive_transport_key(
    uint8_t transport_key[YUMI_TRANSPORT_KEY_LEN],
    const uint8_t temp_key[YUMI_TRANSPORT_KEY_LEN],
    const uint8_t epoch_key[YUMI_EPOCH_KEY_LEN])
{
    if (!transport_key || !temp_key || !epoch_key)
        return YUMI_CRYPTO_ERR_INVALID_PARAM;

    /* Concatenate: temp_key || epoch_key */
    uint8_t ikm[YUMI_TRANSPORT_KEY_LEN + YUMI_EPOCH_KEY_LEN];
    memcpy(ikm, temp_key, YUMI_TRANSPORT_KEY_LEN);
    memcpy(ikm + YUMI_TRANSPORT_KEY_LEN, epoch_key, YUMI_EPOCH_KEY_LEN);

    static const uint8_t info[] = "yumi-transport-v1";
    yumi_crypto_err_t rc = yumi_hkdf(transport_key, YUMI_TRANSPORT_KEY_LEN,
                                      ikm, sizeof(ikm),
                                      NULL, 0,
                                      info, sizeof(info) - 1);

    yumi_memzero(ikm, sizeof(ikm));
    return rc;
}

/* ════════════════════════════════════════════════════════════════
 *  Session lifetime
 * ════════════════════════════════════════════════════════════════ */

uint32_t yumi_session_timeout(void) {
    uint32_t jitter;
    if (yumi_randombytes((uint8_t *)&jitter, sizeof(jitter)) != YUMI_CRYPTO_OK)
        return YUMI_SESSION_TIMEOUT_BASE;

    /* Map to range [-JITTER, +JITTER] */
    int32_t offset = (int32_t)(jitter % (2 * YUMI_SESSION_TIMEOUT_JITTER + 1))
                     - (int32_t)YUMI_SESSION_TIMEOUT_JITTER;

    return (uint32_t)((int32_t)YUMI_SESSION_TIMEOUT_BASE + offset);
}
