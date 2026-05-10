/*
    Group Registrar — Signing, Verification, and Encryption
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
 * @brief Signing, verification, and encryption for the Group Registrar.
 *
 * All operations use the Yumi crypto abstraction layer (crypto.h):
 *   - ML-DSA-87 for signatures
 *   - Skein-1024 for hashing
 *   - Threefish-1024 AEAD for symmetric encryption
 *   - ML-KEM-1024 for peer-scoped key encapsulation
 *
 * gr_sign() rejects signing a registrar in PROVISIONAL state,
 * preventing an unverified registrar from being re-signed.
 */

#include "internal.h"
#include "buf.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Registrar Signing & Verification
 * ═══════════════════════════════════════════════════════════════════ */

static void write_canonical_header(gr_buf_t *buf, const gr_header_t *h) {
    gr_buf_write(buf, h->group_id, GR_HASH_LEN);
    gr_buf_write_u32(buf, (uint32_t)h->group_type);
    gr_buf_write_str(buf, h->group_name, GR_MAX_NAME_LEN);
    gr_buf_write_u32(buf, h->version);
    gr_buf_write_i64(buf, h->created_at);
    gr_buf_write_u32(buf, h->epoch_id);
    gr_buf_write_i64(buf, h->retention.message_retention_ms);
    gr_buf_write_i64(buf, h->retention.file_retention_ms);
    gr_buf_write_i64(buf, h->retention.registrar_max_bytes);
    gr_buf_write(buf, h->owner_id, GR_PEER_ID_LEN);
    gr_buf_write(buf, h->owner_sign_key, GR_PUBLIC_KEY_LEN);
    gr_buf_write(buf, h->signer_id, GR_PEER_ID_LEN);
    gr_buf_write(buf, h->signer_sign_key, GR_PUBLIC_KEY_LEN);
}

gr_error_t gr_sign(gr_registrar_t *reg, const gr_identity_t *signer) {
    if (!reg || !signer)
        return GR_ERR_INVALID_PARAM;

    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, signer, GR_PERM_SIGN_REGISTRAR))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    gr_error_t err = gr_audit_append(reg, GR_CHANGE_REGISTRAR_SIGNED, signer,
                                     NULL, "registrar signed");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }

    memcpy(reg->header.signer_id, signer->peer_id, GR_PEER_ID_LEN);
    memcpy(reg->header.signer_sign_key, signer->public_key, GR_PUBLIC_KEY_LEN);

    gr_buf_t buf;
    if (gr_buf_init(&buf, 8192) != 0) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_OUT_OF_MEMORY; }

    write_canonical_header(&buf, &reg->header);

    /* Hash with Skein-1024 */
    if (yumi_skein_hash(reg->header.hash, buf.data, buf.len) != YUMI_CRYPTO_OK) {
        gr_buf_free(&buf); gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_CRYPTO;
    }

    /* Sign with ML-DSA-87 */
    if (yumi_mldsa_sign(reg->header.signature, reg->header.hash, GR_HASH_LEN,
                        signer->secret_key) != YUMI_CRYPTO_OK) {
        gr_buf_free(&buf); gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_CRYPTO;
    }

    gr_buf_free(&buf);

    err = gr_header_save(reg);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_verify(const gr_registrar_t *reg, bool *valid_out) {
    if (!reg || !valid_out)
        return GR_ERR_INVALID_PARAM;

    gr_buf_t buf;
    if (gr_buf_init(&buf, 8192) != 0)
        return GR_ERR_OUT_OF_MEMORY;

    write_canonical_header(&buf, &reg->header);

    uint8_t computed[GR_HASH_LEN];
    if (yumi_skein_hash(computed, buf.data, buf.len) != YUMI_CRYPTO_OK) {
        gr_buf_free(&buf);
        return GR_ERR_CRYPTO;
    }
    gr_buf_free(&buf);

    if (yumi_memcmp(computed, reg->header.hash, GR_HASH_LEN) != 0) {
        *valid_out = false;
        return GR_OK;
    }

    yumi_crypto_err_t rc = yumi_mldsa_verify(reg->header.signature,
                                              reg->header.hash, GR_HASH_LEN,
                                              reg->header.signer_sign_key);
    *valid_out = (rc == YUMI_CRYPTO_OK);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Data Signing & Verification
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_sign_data(const gr_identity_t *signer,
                        const uint8_t *data, size_t data_len,
                        uint8_t signature_out[GR_SIGN_LEN]) {
    if (!signer || !data || !signature_out)
        return GR_ERR_INVALID_PARAM;
    if (yumi_mldsa_sign(signature_out, data, data_len,
                        signer->secret_key) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;
    return GR_OK;
}

gr_error_t gr_verify_data(const uint8_t public_key[GR_PUBLIC_KEY_LEN],
                          const uint8_t *data, size_t data_len,
                          const uint8_t signature[GR_SIGN_LEN],
                          bool *valid_out) {
    if (!public_key || !data || !signature || !valid_out)
        return GR_ERR_INVALID_PARAM;
    yumi_crypto_err_t rc = yumi_mldsa_verify(signature, data, data_len,
                                              public_key);
    *valid_out = (rc == YUMI_CRYPTO_OK);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group-Scoped Encryption (Threefish-1024 AEAD with epoch key)
 *
 *  Wire format: nonce(16) || ciphertext || tag(128)
 *
 *  Note: encryption/decryption is allowed during provisional state.
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_encrypt(const gr_registrar_t *reg,
                      const uint8_t *plaintext, size_t plaintext_len,
                      const uint8_t *ad, size_t ad_len,
                      uint8_t *out, size_t *out_len) {
    if (!reg || !plaintext || !out || !out_len)
        return GR_ERR_INVALID_PARAM;

    gr_epoch_t epoch;
    gr_error_t err = gr_epoch_get_current(reg, &epoch);
    if (err != GR_OK)
        return err;

    size_t required = GR_NONCE_LEN + plaintext_len + GR_MAC_LEN;
    if (*out_len < required) {
        *out_len = required;
        yumi_memzero(epoch.epoch_key, GR_EPOCH_KEY_LEN);
        return GR_ERR_SIZE_EXCEEDED;
    }

    /* Generate random nonce */
    if (yumi_randombytes(out, GR_NONCE_LEN) != YUMI_CRYPTO_OK) {
        yumi_memzero(epoch.epoch_key, GR_EPOCH_KEY_LEN);
        return GR_ERR_CRYPTO;
    }

    size_t ct_len = *out_len - GR_NONCE_LEN;
    yumi_crypto_err_t rc = yumi_aead_encrypt(
        out + GR_NONCE_LEN, &ct_len,
        plaintext, plaintext_len,
        ad, ad_len, out, epoch.epoch_key);

    yumi_memzero(epoch.epoch_key, GR_EPOCH_KEY_LEN);
    if (rc != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;
    *out_len = GR_NONCE_LEN + ct_len;
    return GR_OK;
}

gr_error_t gr_decrypt(const gr_registrar_t *reg,
                      const uint8_t *ciphertext, size_t ciphertext_len,
                      const uint8_t *ad, size_t ad_len,
                      uint8_t *out, size_t *out_len,
                      uint32_t *epoch_id_out) {
    if (!reg || !ciphertext || !out || !out_len)
        return GR_ERR_INVALID_PARAM;
    if (ciphertext_len < GR_NONCE_LEN + GR_MAC_LEN)
        return GR_ERR_INVALID_PARAM;

    const uint8_t *nonce = ciphertext;
    const uint8_t *ct = ciphertext + GR_NONCE_LEN;
    size_t ct_len = ciphertext_len - GR_NONCE_LEN;

    uint32_t ecount;
    gr_epoch_count(reg, &ecount);

    gr_epoch_t *epochs = (gr_epoch_t *)calloc(ecount, sizeof(gr_epoch_t));
    if (!epochs)
        return GR_ERR_OUT_OF_MEMORY;

    uint32_t actual;
    gr_error_t err = gr_epoch_list(reg, epochs, ecount, &actual);
    if (err != GR_OK) {
        free(epochs);
        return err;
    }

    /* Trial decrypt with each epoch key (most recent first) */
    for (int i = (int)actual - 1; i >= 0; i--) {
        size_t plen = *out_len;
        yumi_crypto_err_t rc = yumi_aead_decrypt(
            out, &plen,
            ct, ct_len, ad, ad_len,
            nonce, epochs[i].epoch_key);
        if (rc == YUMI_CRYPTO_OK) {
            *out_len = plen;
            if (epoch_id_out)
                *epoch_id_out = epochs[i].epoch_id;
            for (uint32_t j = 0; j < actual; j++)
                yumi_memzero(epochs[j].epoch_key, GR_EPOCH_KEY_LEN);
            free(epochs);
            return GR_OK;
        }
    }

    for (uint32_t j = 0; j < actual; j++)
        yumi_memzero(epochs[j].epoch_key, GR_EPOCH_KEY_LEN);
    free(epochs);
    return GR_ERR_CRYPTO;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Peer-Scoped Encryption (ML-KEM-1024 encapsulation + AEAD)
 *
 *  Wire format: kem_ct(1568) || aead_nonce(16) || aead_ct || aead_tag(128)
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_encrypt_for_peer(const uint8_t peer_kem_pk[GR_KEM_PUBLIC_KEY_LEN],
                               const uint8_t *plaintext, size_t plaintext_len,
                               uint8_t *out, size_t *out_len) {
    if (!peer_kem_pk || !plaintext || !out || !out_len)
        return GR_ERR_INVALID_PARAM;

    size_t required = GR_KEM_CIPHERTEXT_LEN + GR_NONCE_LEN +
                      plaintext_len + GR_MAC_LEN;
    if (*out_len < required) {
        *out_len = required;
        return GR_ERR_SIZE_EXCEEDED;
    }

    /* ML-KEM encapsulation → shared secret */
    uint8_t ss[YUMI_MLKEM_SHARED_SECRET_LEN];
    if (yumi_mlkem_encaps(out, ss, peer_kem_pk) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;

    /* Derive AEAD key from shared secret via HKDF.
     * Use recipient KEM pk as salt to bind the derived key to
     * the intended recipient, preventing cross-context replay. */
    uint8_t aead_key[YUMI_AEAD_KEY_LEN];
    static const uint8_t info[] = "yumi-peer-encrypt-v1";
    if (yumi_hkdf(aead_key, YUMI_AEAD_KEY_LEN,
                  ss, YUMI_MLKEM_SHARED_SECRET_LEN,
                  peer_kem_pk, GR_KEM_PUBLIC_KEY_LEN,
                  info, sizeof(info) - 1) != YUMI_CRYPTO_OK) {
        yumi_memzero(ss, sizeof(ss));
        return GR_ERR_CRYPTO;
    }
    yumi_memzero(ss, sizeof(ss));

    /* Generate nonce */
    uint8_t *nonce_ptr = out + GR_KEM_CIPHERTEXT_LEN;
    if (yumi_randombytes(nonce_ptr, GR_NONCE_LEN) != YUMI_CRYPTO_OK) {
        yumi_memzero(aead_key, sizeof(aead_key));
        return GR_ERR_CRYPTO;
    }

    /* AEAD encrypt */
    size_t ct_len = *out_len - GR_KEM_CIPHERTEXT_LEN - GR_NONCE_LEN;
    yumi_crypto_err_t rc = yumi_aead_encrypt(
        out + GR_KEM_CIPHERTEXT_LEN + GR_NONCE_LEN, &ct_len,
        plaintext, plaintext_len,
        NULL, 0, nonce_ptr, aead_key);

    yumi_memzero(aead_key, sizeof(aead_key));
    if (rc != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;

    *out_len = GR_KEM_CIPHERTEXT_LEN + GR_NONCE_LEN + ct_len;
    return GR_OK;
}

gr_error_t gr_decrypt_from_peer(const gr_identity_t *self,
                                const uint8_t *ciphertext,
                                size_t ciphertext_len,
                                uint8_t *out, size_t *out_len) {
    if (!self || !ciphertext || !out || !out_len)
        return GR_ERR_INVALID_PARAM;

    size_t min_len = GR_KEM_CIPHERTEXT_LEN + GR_NONCE_LEN + GR_MAC_LEN;
    if (ciphertext_len < min_len)
        return GR_ERR_INVALID_PARAM;

    /* ML-KEM decapsulation → shared secret */
    uint8_t ss[YUMI_MLKEM_SHARED_SECRET_LEN];
    if (yumi_mlkem_decaps(ss, ciphertext, self->kem_sk) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;

    /* Derive AEAD key from shared secret via HKDF.
     * Use own KEM pk as salt (recipient = self), matching the
     * peer_kem_pk salt used during encryption. */
    uint8_t aead_key[YUMI_AEAD_KEY_LEN];
    static const uint8_t info[] = "yumi-peer-encrypt-v1";
    if (yumi_hkdf(aead_key, YUMI_AEAD_KEY_LEN,
                  ss, YUMI_MLKEM_SHARED_SECRET_LEN,
                  self->kem_pk, GR_KEM_PUBLIC_KEY_LEN,
                  info, sizeof(info) - 1) != YUMI_CRYPTO_OK) {
        yumi_memzero(ss, sizeof(ss));
        return GR_ERR_CRYPTO;
    }
    yumi_memzero(ss, sizeof(ss));

    const uint8_t *nonce = ciphertext + GR_KEM_CIPHERTEXT_LEN;
    const uint8_t *ct = ciphertext + GR_KEM_CIPHERTEXT_LEN + GR_NONCE_LEN;
    size_t ct_len = ciphertext_len - GR_KEM_CIPHERTEXT_LEN - GR_NONCE_LEN;

    size_t plen = *out_len;
    yumi_crypto_err_t rc = yumi_aead_decrypt(
        out, &plen,
        ct, ct_len, NULL, 0,
        nonce, aead_key);

    yumi_memzero(aead_key, sizeof(aead_key));
    if (rc != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;
    *out_len = plen;
    return GR_OK;
}
