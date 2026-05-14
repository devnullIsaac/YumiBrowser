#include "internal.h"

gr_error_t gr_identity_generate(gr_identity_t *out) {
    if (!out) return GR_ERR_INVALID_PARAM;
    memset(out, 0, sizeof(*out));

    /* ML-DSA-87 signing keypair */
    if (yumi_mldsa_keygen(out->public_key, out->secret_key) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;

    /* ML-KEM-1024 key encapsulation keypair */
    if (yumi_mlkem_keygen(out->kem_pk, out->kem_sk) != YUMI_CRYPTO_OK) {
        yumi_memzero(out, sizeof(*out));
        return GR_ERR_CRYPTO;
    }

    /* peer_id = Skein-1024(sign_pk) truncated to GR_PEER_ID_LEN */
    uint8_t full_hash[YUMI_SKEIN_HASH_LEN];
    if (yumi_skein_hash(full_hash, out->public_key, GR_PUBLIC_KEY_LEN) != YUMI_CRYPTO_OK) {
        yumi_memzero(out, sizeof(*out));
        return GR_ERR_CRYPTO;
    }
    memcpy(out->peer_id, full_hash, GR_PEER_ID_LEN);
    yumi_memzero(full_hash, YUMI_SKEIN_HASH_LEN);
    return GR_OK;
}

gr_error_t gr_identity_derive_id(const uint8_t public_key[GR_PUBLIC_KEY_LEN],
                                  uint8_t peer_id_out[GR_PEER_ID_LEN]) {
    if (!public_key || !peer_id_out) return GR_ERR_INVALID_PARAM;
    uint8_t full_hash[YUMI_SKEIN_HASH_LEN];
    if (yumi_skein_hash(full_hash, public_key, GR_PUBLIC_KEY_LEN) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;
    memcpy(peer_id_out, full_hash, GR_PEER_ID_LEN);
    yumi_memzero(full_hash, YUMI_SKEIN_HASH_LEN);
    return GR_OK;
}

void gr_identity_wipe(gr_identity_t *identity) {
    if (identity) {
        yumi_memzero(identity->secret_key, GR_SECRET_KEY_LEN);
        yumi_memzero(identity->kem_sk, GR_KEM_SECRET_KEY_LEN);
    }
}
