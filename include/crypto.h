/*
 * crypto.h - Yumi cryptography abstraction layer: ML-DSA-87, ML-KEM-1024, FrodoKEM-1344, BrainPool-512 ECDH, Skein-1024 hash/MAC/HKDF, Threefish-1024 AEAD.
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
 * @file crypto.h
 * @brief Yumi Browser Cryptography Abstraction Layer
 *
 * Unified post-quantum cryptography interface built on:
 *   - **ML-DSA-87**      — Digital signatures (FIPS 204, NIST Level 5) via oqs-provider
 *   - **ML-KEM-1024**    — Key encapsulation (post-quantum lattice)    via oqs-provider
 *   - **FrodoKEM-1344**  — Key encapsulation (plain LWE)              via oqs-provider
 *   - **BrainPool-512**  — ECDH key exchange (classical fallback)     via OpenSSL
 *   - **Skein-1024**     — Hashing, MAC, HKDF                        embedded
 *   - **Threefish-1024** — Symmetric encryption, AEAD                 embedded
 *
 * @note Call yumi_crypto_init() once before using any other function.
 *
 * ## Example — Sign and verify a message
 *
 * @code{.c}
 * #include "crypto.h"
 * #include <stdio.h>
 *
 * int main(void) {
 *     yumi_crypto_init();
 *
 *     // Generate an ML-DSA-87 signing keypair
 *     uint8_t pk[YUMI_MLDSA_PUBLIC_KEY_LEN];
 *     uint8_t sk[YUMI_MLDSA_SECRET_KEY_LEN];
 *     yumi_mldsa_keygen(pk, sk);
 *
 *     // Sign a message
 *     const uint8_t msg[] = "Hello, post-quantum world!";
 *     uint8_t sig[YUMI_MLDSA_SIGN_LEN];
 *     yumi_mldsa_sign(sig, msg, sizeof(msg), sk);
 *
 *     // Verify the signature
 *     if (yumi_mldsa_verify(sig, msg, sizeof(msg), pk) == YUMI_CRYPTO_OK)
 *         printf("Signature valid!\n");
 *
 *     // Clean up secret key material
 *     yumi_memzero(sk, sizeof(sk));
 *     yumi_crypto_cleanup();
 *     return 0;
 * }
 * @endcode
 *
 * ## Example — AEAD encrypt / decrypt
 *
 * @code{.c}
 * uint8_t key[YUMI_AEAD_KEY_LEN], nonce[YUMI_AEAD_NONCE_LEN];
 * yumi_randombytes(key, sizeof(key));
 * yumi_randombytes(nonce, sizeof(nonce));
 *
 * const uint8_t plaintext[] = "secret data";
 * uint8_t ct[sizeof(plaintext) + YUMI_AEAD_TAG_LEN];
 * size_t ct_len;
 * yumi_aead_encrypt(ct, &ct_len, plaintext, sizeof(plaintext),
 *                   NULL, 0, nonce, key);
 *
 * uint8_t recovered[sizeof(plaintext)];
 * size_t rec_len;
 * yumi_aead_decrypt(recovered, &rec_len, ct, ct_len,
 *                   NULL, 0, nonce, key);
 * @endcode
 *
 * ## Example — Key encapsulation (ML-KEM-1024)
 *
 * @code{.c}
 * uint8_t pk[YUMI_MLKEM_PUBLIC_KEY_LEN], sk[YUMI_MLKEM_SECRET_KEY_LEN];
 * yumi_mlkem_keygen(pk, sk);
 *
 * // Sender encapsulates a shared secret
 * uint8_t ct[YUMI_MLKEM_CIPHERTEXT_LEN], ss_sender[YUMI_MLKEM_SHARED_SECRET_LEN];
 * yumi_mlkem_encaps(ct, ss_sender, pk);
 *
 * // Receiver decapsulates to get the same shared secret
 * uint8_t ss_receiver[YUMI_MLKEM_SHARED_SECRET_LEN];
 * yumi_mlkem_decaps(ss_receiver, ct, sk);
 * // ss_sender == ss_receiver
 * @endcode
 */

#ifndef YUMI_CRYPTO_H
#define YUMI_CRYPTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup crypto_errors Error Codes
 * @brief Return codes for all crypto operations.
 * @{
 */
typedef enum {
    YUMI_CRYPTO_OK                =  0,  /**< Success. */
    YUMI_CRYPTO_ERR_INVALID_PARAM = -1,  /**< NULL pointer or invalid argument. */
    YUMI_CRYPTO_ERR_KEYGEN        = -2,  /**< Key generation failed. */
    YUMI_CRYPTO_ERR_SIGN          = -3,  /**< Signing operation failed. */
    YUMI_CRYPTO_ERR_VERIFY        = -4,  /**< Signature verification failed (bad sig). */
    YUMI_CRYPTO_ERR_ENCAPS        = -5,  /**< KEM encapsulation failed. */
    YUMI_CRYPTO_ERR_DECAPS        = -6,  /**< KEM decapsulation failed. */
    YUMI_CRYPTO_ERR_ECDH          = -7,  /**< ECDH key agreement failed. */
    YUMI_CRYPTO_ERR_ENCRYPT       = -8,  /**< AEAD encryption failed. */
    YUMI_CRYPTO_ERR_DECRYPT       = -9,  /**< AEAD decryption / authentication failed. */
    YUMI_CRYPTO_ERR_HASH          = -10, /**< Hashing operation failed. */
    YUMI_CRYPTO_ERR_KDF           = -11, /**< HKDF derivation failed. */
    YUMI_CRYPTO_ERR_RNG           = -12, /**< Random number generation failed. */
    YUMI_CRYPTO_ERR_INIT          = -13, /**< Library initialization failed. */
    YUMI_CRYPTO_ERR_PROVIDER      = -14, /**< oqs-provider load/registration failed. */
    YUMI_CRYPTO_ERR_BUFFER_TOO_SMALL = -15, /**< Output buffer too small. */
} yumi_crypto_err_t;
/** @} */

/* ════════════════════════════════════════════════════════════════
 *  Constants — ML-DSA-87 (signing)
 * ════════════════════════════════════════════════════════════════ */

#define YUMI_MLDSA_PUBLIC_KEY_LEN   2592
#define YUMI_MLDSA_SECRET_KEY_LEN   4896
#define YUMI_MLDSA_SIGN_LEN         4627

/* ════════════════════════════════════════════════════════════════
 *  Constants — ML-KEM-1024 (post-quantum KEM)
 * ════════════════════════════════════════════════════════════════ */

#define YUMI_MLKEM_PUBLIC_KEY_LEN   1568
#define YUMI_MLKEM_SECRET_KEY_LEN   3168
#define YUMI_MLKEM_CIPHERTEXT_LEN   1568
#define YUMI_MLKEM_SHARED_SECRET_LEN 32

/* ════════════════════════════════════════════════════════════════
 *  Constants — FrodoKEM-1344-SHAKE (plain LWE KEM, invitation only)
 * ════════════════════════════════════════════════════════════════ */

#define YUMI_FRODO_PUBLIC_KEY_LEN   21520
#define YUMI_FRODO_SECRET_KEY_LEN   43088
#define YUMI_FRODO_CIPHERTEXT_LEN   21696
#define YUMI_FRODO_SHARED_SECRET_LEN 32

/* ════════════════════════════════════════════════════════════════
 *  Constants — BrainPool-512 ECDH (classical fallback)
 * ════════════════════════════════════════════════════════════════ */

/* Compressed point: 1 (tag) + 64 (x-coordinate) = 65 bytes.
 * Uncompressed: 1 + 64 + 64 = 129 bytes. We store compressed. */
#define YUMI_BP512_PUBLIC_KEY_LEN   129   /* 0x04 || x(64) || y(64) */
#define YUMI_BP512_SECRET_KEY_LEN   64
#define YUMI_BP512_SHARED_SECRET_LEN 64

/* ════════════════════════════════════════════════════════════════
 *  Constants — Skein-1024 (hashing, MAC, HKDF)
 * ════════════════════════════════════════════════════════════════ */

#define YUMI_SKEIN_BLOCK_LEN        128     /* 1024 bits */
#define YUMI_SKEIN_HASH_LEN         128     /* 1024-bit output */
#define YUMI_SKEIN_MAC_LEN          128     /* 1024-bit MAC */

/* ════════════════════════════════════════════════════════════════
 *  Constants — Threefish-1024 (symmetric encryption)
 * ════════════════════════════════════════════════════════════════ */

#define YUMI_THREEFISH_BLOCK_LEN    128     /* 1024 bits */
#define YUMI_THREEFISH_KEY_LEN      128     /* 1024-bit key */
#define YUMI_THREEFISH_TWEAK_LEN    16      /* 128-bit tweak */

/* ════════════════════════════════════════════════════════════════
 *  Constants — AEAD (Threefish-1024-CTR + Skein-1024-MAC)
 * ════════════════════════════════════════════════════════════════ */

#define YUMI_AEAD_KEY_LEN           YUMI_THREEFISH_KEY_LEN  /* 128 */
#define YUMI_AEAD_NONCE_LEN         16      /* 128-bit nonce for CTR */
#define YUMI_AEAD_TAG_LEN           YUMI_SKEIN_MAC_LEN      /* 128 */

/* ════════════════════════════════════════════════════════════════
 *  Constants — HKDF (built on Skein-1024-MAC)
 * ════════════════════════════════════════════════════════════════ */

#define YUMI_HKDF_HASH_LEN          YUMI_SKEIN_HASH_LEN     /* 128 */

/* ════════════════════════════════════════════════════════════════
 *  Constants — Session / transport
 * ════════════════════════════════════════════════════════════════ */

#define YUMI_SESSION_TIMEOUT_BASE   3600    /* 1 hour in seconds */
#define YUMI_SESSION_TIMEOUT_JITTER 300     /* ±5 minutes */
#define YUMI_EPOCH_KEY_LEN          YUMI_THREEFISH_KEY_LEN   /* 128 */
#define YUMI_TRANSPORT_KEY_LEN      YUMI_THREEFISH_KEY_LEN   /* 128 */

/* ════════════════════════════════════════════════════════════════
 *  Opaque types
 * ════════════════════════════════════════════════════════════════ */

/** @brief Opaque BrainPool-512 ECDH keypair (heap-allocated). */
typedef struct yumi_bp512_keypair   yumi_bp512_keypair_t;
/** @brief Opaque Skein-1024 streaming hash context (heap-allocated). */
typedef struct yumi_skein_ctx       yumi_skein_ctx_t;

/* ════════════════════════════════════════════════════════════════
 *  Initialization
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialize the crypto library.
 *
 * Loads OpenSSL, registers oqs-provider, and validates all primitives.
 * Must be called once before any other yumi_crypto function.
 *
 * @return YUMI_CRYPTO_OK on success, or YUMI_CRYPTO_ERR_INIT / YUMI_CRYPTO_ERR_PROVIDER.
 *
 * @code{.c}
 * if (yumi_crypto_init() != YUMI_CRYPTO_OK) {
 *     fprintf(stderr, "Crypto init failed\n");
 *     exit(1);
 * }
 * @endcode
 */
yumi_crypto_err_t yumi_crypto_init(void);

/**
 * @brief Clean up all crypto resources.
 *
 * Call once on application shutdown. No crypto calls are valid after this.
 */
void yumi_crypto_cleanup(void);

/* ════════════════════════════════════════════════════════════════
 *  Random bytes
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Fill a buffer with cryptographically secure random bytes.
 *
 * @param[out] buf  Destination buffer.
 * @param[in]  len  Number of random bytes to generate.
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_RNG on failure.
 *
 * @code{.c}
 * uint8_t nonce[16];
 * yumi_randombytes(nonce, sizeof(nonce));
 * @endcode
 */
yumi_crypto_err_t yumi_randombytes(uint8_t *buf, size_t len);

/* ════════════════════════════════════════════════════════════════
 *  Memory hygiene
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Securely zero memory (not optimized away by the compiler).
 *
 * @param[out] buf  Buffer to zero.
 * @param[in]  len  Number of bytes to zero.
 *
 * @code{.c}
 * uint8_t sk[YUMI_MLDSA_SECRET_KEY_LEN];
 * // ... use sk ...
 * yumi_memzero(sk, sizeof(sk));
 * @endcode
 */
void yumi_memzero(void *buf, size_t len);

/**
 * @brief Constant-time memory comparison (timing-safe).
 *
 * @param[in] a    First buffer.
 * @param[in] b    Second buffer.
 * @param[in] len  Number of bytes to compare.
 * @return 0 if equal, non-zero otherwise.
 */
int  yumi_memcmp(const uint8_t *a, const uint8_t *b, size_t len);

/* ════════════════════════════════════════════════════════════════
 *  ML-DSA-87 — Post-quantum digital signatures (FIPS 204)
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Generate an ML-DSA-87 signing keypair.
 *
 * @param[out] pk  Public key (YUMI_MLDSA_PUBLIC_KEY_LEN bytes).
 * @param[out] sk  Secret key (YUMI_MLDSA_SECRET_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_KEYGEN on failure.
 *
 * @code{.c}
 * uint8_t pk[YUMI_MLDSA_PUBLIC_KEY_LEN], sk[YUMI_MLDSA_SECRET_KEY_LEN];
 * yumi_mldsa_keygen(pk, sk);
 * @endcode
 */
yumi_crypto_err_t yumi_mldsa_keygen(
    uint8_t pk[YUMI_MLDSA_PUBLIC_KEY_LEN],
    uint8_t sk[YUMI_MLDSA_SECRET_KEY_LEN]);

/**
 * @brief Sign a message with ML-DSA-87.
 *
 * @param[out] sig      Signature output (YUMI_MLDSA_SIGN_LEN bytes).
 * @param[in]  msg      Message to sign.
 * @param[in]  msg_len  Length of the message in bytes.
 * @param[in]  sk       Secret key (YUMI_MLDSA_SECRET_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_SIGN on failure.
 *
 * @code{.c}
 * uint8_t sig[YUMI_MLDSA_SIGN_LEN];
 * yumi_mldsa_sign(sig, msg, msg_len, sk);
 * @endcode
 */
yumi_crypto_err_t yumi_mldsa_sign(
    uint8_t       sig[YUMI_MLDSA_SIGN_LEN],
    const uint8_t *msg, size_t msg_len,
    const uint8_t sk[YUMI_MLDSA_SECRET_KEY_LEN]);

/**
 * @brief Verify an ML-DSA-87 signature.
 *
 * @param[in] sig      Signature to verify (YUMI_MLDSA_SIGN_LEN bytes).
 * @param[in] msg      Original message.
 * @param[in] msg_len  Length of the message in bytes.
 * @param[in] pk       Public key (YUMI_MLDSA_PUBLIC_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK if valid, YUMI_CRYPTO_ERR_VERIFY if invalid.
 */
yumi_crypto_err_t yumi_mldsa_verify(
    const uint8_t sig[YUMI_MLDSA_SIGN_LEN],
    const uint8_t *msg, size_t msg_len,
    const uint8_t pk[YUMI_MLDSA_PUBLIC_KEY_LEN]);

/* ════════════════════════════════════════════════════════════════
 *  ML-KEM-1024 — Post-quantum key encapsulation
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Generate an ML-KEM-1024 keypair.
 *
 * @param[out] pk  Public key (YUMI_MLKEM_PUBLIC_KEY_LEN bytes).
 * @param[out] sk  Secret key (YUMI_MLKEM_SECRET_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_KEYGEN on failure.
 */
yumi_crypto_err_t yumi_mlkem_keygen(
    uint8_t pk[YUMI_MLKEM_PUBLIC_KEY_LEN],
    uint8_t sk[YUMI_MLKEM_SECRET_KEY_LEN]);

/**
 * @brief Encapsulate a shared secret using an ML-KEM-1024 public key.
 *
 * The sender calls this with the recipient's public key to produce
 * a ciphertext and a shared secret. Only the recipient can decapsulate.
 *
 * @param[out] ct  Ciphertext (YUMI_MLKEM_CIPHERTEXT_LEN bytes).
 * @param[out] ss  Shared secret (YUMI_MLKEM_SHARED_SECRET_LEN bytes).
 * @param[in]  pk  Recipient's public key (YUMI_MLKEM_PUBLIC_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_ENCAPS on failure.
 *
 * @code{.c}
 * uint8_t ct[YUMI_MLKEM_CIPHERTEXT_LEN], ss[YUMI_MLKEM_SHARED_SECRET_LEN];
 * yumi_mlkem_encaps(ct, ss, recipient_pk);
 * // Send ct to the recipient; use ss as a symmetric key.
 * @endcode
 */
yumi_crypto_err_t yumi_mlkem_encaps(
    uint8_t ct[YUMI_MLKEM_CIPHERTEXT_LEN],
    uint8_t ss[YUMI_MLKEM_SHARED_SECRET_LEN],
    const uint8_t pk[YUMI_MLKEM_PUBLIC_KEY_LEN]);

/**
 * @brief Decapsulate a shared secret from an ML-KEM-1024 ciphertext.
 *
 * @param[out] ss  Shared secret (YUMI_MLKEM_SHARED_SECRET_LEN bytes).
 * @param[in]  ct  Ciphertext from encapsulation (YUMI_MLKEM_CIPHERTEXT_LEN bytes).
 * @param[in]  sk  Recipient's secret key (YUMI_MLKEM_SECRET_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_DECAPS on failure.
 */
yumi_crypto_err_t yumi_mlkem_decaps(
    uint8_t ss[YUMI_MLKEM_SHARED_SECRET_LEN],
    const uint8_t ct[YUMI_MLKEM_CIPHERTEXT_LEN],
    const uint8_t sk[YUMI_MLKEM_SECRET_KEY_LEN]);

/* ════════════════════════════════════════════════════════════════
 *  FrodoKEM-1344-SHAKE — Plain LWE key encapsulation
 *  (invitation handshake only — large ciphertexts, algorithm diversity)
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Generate a FrodoKEM-1344-SHAKE keypair.
 *
 * Used for invitation handshakes where algorithm diversity matters.
 * Key and ciphertext sizes are large (~21 KB each).
 *
 * @param[out] pk  Public key (YUMI_FRODO_PUBLIC_KEY_LEN bytes).
 * @param[out] sk  Secret key (YUMI_FRODO_SECRET_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_KEYGEN on failure.
 */
yumi_crypto_err_t yumi_frodo_keygen(
    uint8_t pk[YUMI_FRODO_PUBLIC_KEY_LEN],
    uint8_t sk[YUMI_FRODO_SECRET_KEY_LEN]);

/**
 * @brief Encapsulate a shared secret using a FrodoKEM-1344 public key.
 *
 * @param[out] ct  Ciphertext (YUMI_FRODO_CIPHERTEXT_LEN bytes).
 * @param[out] ss  Shared secret (YUMI_FRODO_SHARED_SECRET_LEN bytes).
 * @param[in]  pk  Recipient's public key (YUMI_FRODO_PUBLIC_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_ENCAPS on failure.
 */
yumi_crypto_err_t yumi_frodo_encaps(
    uint8_t ct[YUMI_FRODO_CIPHERTEXT_LEN],
    uint8_t ss[YUMI_FRODO_SHARED_SECRET_LEN],
    const uint8_t pk[YUMI_FRODO_PUBLIC_KEY_LEN]);

/**
 * @brief Decapsulate a shared secret from a FrodoKEM-1344 ciphertext.
 *
 * @param[out] ss  Shared secret (YUMI_FRODO_SHARED_SECRET_LEN bytes).
 * @param[in]  ct  Ciphertext (YUMI_FRODO_CIPHERTEXT_LEN bytes).
 * @param[in]  sk  Recipient's secret key (YUMI_FRODO_SECRET_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_DECAPS on failure.
 */
yumi_crypto_err_t yumi_frodo_decaps(
    uint8_t ss[YUMI_FRODO_SHARED_SECRET_LEN],
    const uint8_t ct[YUMI_FRODO_CIPHERTEXT_LEN],
    const uint8_t sk[YUMI_FRODO_SECRET_KEY_LEN]);

/* ════════════════════════════════════════════════════════════════
 *  BrainPool-512 ECDH — Classical elliptic curve key exchange
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Generate a BrainPool-512 ECDH keypair.
 *
 * The keypair is heap-allocated and must be freed with yumi_bp512_keypair_free().
 *
 * @param[out] kp  Pointer to receive the allocated keypair.
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_KEYGEN on failure.
 *
 * @code{.c}
 * yumi_bp512_keypair_t *kp;
 * yumi_bp512_keygen(&kp);
 *
 * uint8_t pk[YUMI_BP512_PUBLIC_KEY_LEN];
 * size_t pk_len;
 * yumi_bp512_get_public_key(kp, pk, &pk_len);
 * // ... share pk with peer ...
 *
 * yumi_bp512_keypair_free(kp);
 * @endcode
 */
yumi_crypto_err_t yumi_bp512_keygen(yumi_bp512_keypair_t **kp);

/**
 * @brief Free a BrainPool-512 keypair.
 *
 * @param[in] kp  Keypair to free (NULL-safe).
 */
void yumi_bp512_keypair_free(yumi_bp512_keypair_t *kp);

/**
 * @brief Extract the public key from a BrainPool-512 keypair.
 *
 * @param[in]  kp      Keypair to extract from.
 * @param[out] pk      Buffer for the uncompressed public key (YUMI_BP512_PUBLIC_KEY_LEN).
 * @param[out] pk_len  Receives the actual public key length.
 * @return YUMI_CRYPTO_OK on success.
 */
yumi_crypto_err_t yumi_bp512_get_public_key(
    const yumi_bp512_keypair_t *kp,
    uint8_t pk[YUMI_BP512_PUBLIC_KEY_LEN],
    size_t *pk_len);

/**
 * @brief Perform BrainPool-512 ECDH key agreement.
 *
 * Derives a shared secret from our keypair and the peer's public key.
 *
 * @param[out] ss          Shared secret output (YUMI_BP512_SHARED_SECRET_LEN).
 * @param[out] ss_len      Receives the actual shared secret length.
 * @param[in]  our_kp      Our keypair.
 * @param[in]  peer_pk     Peer's public key bytes.
 * @param[in]  peer_pk_len Length of the peer's public key.
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_ECDH on failure.
 *
 * @code{.c}
 * uint8_t ss[YUMI_BP512_SHARED_SECRET_LEN];
 * size_t ss_len;
 * yumi_bp512_ecdh(ss, &ss_len, our_kp, peer_pk, peer_pk_len);
 * @endcode
 */
yumi_crypto_err_t yumi_bp512_ecdh(
    uint8_t ss[YUMI_BP512_SHARED_SECRET_LEN],
    size_t *ss_len,
    const yumi_bp512_keypair_t *our_kp,
    const uint8_t *peer_pk, size_t peer_pk_len);

/* ════════════════════════════════════════════════════════════════
 *  Skein-1024 — Hashing
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Compute a one-shot Skein-1024 hash.
 *
 * @param[out] out       Hash output (YUMI_SKEIN_HASH_LEN = 128 bytes).
 * @param[in]  data      Input data.
 * @param[in]  data_len  Length of input data in bytes.
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_HASH on failure.
 *
 * @code{.c}
 * uint8_t hash[YUMI_SKEIN_HASH_LEN];
 * yumi_skein_hash(hash, data, data_len);
 * @endcode
 */
yumi_crypto_err_t yumi_skein_hash(
    uint8_t out[YUMI_SKEIN_HASH_LEN],
    const uint8_t *data, size_t data_len);

/**
 * @brief Initialize a streaming Skein-1024 hash context.
 *
 * Allocates a context on the heap. Free with yumi_skein_free().
 *
 * @param[out] ctx  Receives the allocated context pointer.
 * @return YUMI_CRYPTO_OK on success.
 *
 * @code{.c}
 * yumi_skein_ctx_t *ctx;
 * yumi_skein_init(&ctx);
 * yumi_skein_update(ctx, chunk1, chunk1_len);
 * yumi_skein_update(ctx, chunk2, chunk2_len);
 *
 * uint8_t hash[YUMI_SKEIN_HASH_LEN];
 * yumi_skein_final(ctx, hash);
 * yumi_skein_free(ctx);
 * @endcode
 */
yumi_crypto_err_t yumi_skein_init(yumi_skein_ctx_t **ctx);

/**
 * @brief Feed data into a streaming Skein-1024 hash.
 *
 * @param[in,out] ctx       Hash context from yumi_skein_init().
 * @param[in]     data      Input data chunk.
 * @param[in]     data_len  Length of this chunk.
 * @return YUMI_CRYPTO_OK on success.
 */
yumi_crypto_err_t yumi_skein_update(yumi_skein_ctx_t *ctx,
                                     const uint8_t *data, size_t data_len);

/**
 * @brief Finalize a streaming Skein-1024 hash and produce the digest.
 *
 * @param[in,out] ctx  Hash context (consumed, still must be freed).
 * @param[out]    out  Hash output (YUMI_SKEIN_HASH_LEN bytes).
 * @return YUMI_CRYPTO_OK on success.
 */
yumi_crypto_err_t yumi_skein_final(yumi_skein_ctx_t *ctx,
                                    uint8_t out[YUMI_SKEIN_HASH_LEN]);

/**
 * @brief Free a streaming Skein-1024 hash context.
 *
 * @param[in] ctx  Context to free (NULL-safe).
 */
void              yumi_skein_free(yumi_skein_ctx_t *ctx);

/* ════════════════════════════════════════════════════════════════
 *  Skein-1024-MAC — Message authentication
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Compute a Skein-1024-MAC (keyed hash).
 *
 * @param[out] mac       MAC output (YUMI_SKEIN_MAC_LEN = 128 bytes).
 * @param[in]  key       MAC key.
 * @param[in]  key_len   Length of the key in bytes.
 * @param[in]  data      Input data.
 * @param[in]  data_len  Length of input data in bytes.
 * @return YUMI_CRYPTO_OK on success.
 *
 * @code{.c}
 * uint8_t mac[YUMI_SKEIN_MAC_LEN];
 * yumi_skein_mac(mac, key, key_len, message, message_len);
 * @endcode
 */
yumi_crypto_err_t yumi_skein_mac(
    uint8_t mac[YUMI_SKEIN_MAC_LEN],
    const uint8_t *key, size_t key_len,
    const uint8_t *data, size_t data_len);

/* ════════════════════════════════════════════════════════════════
 *  HKDF — Key derivation (built on Skein-1024-MAC)
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Derive key material using HKDF (extract-then-expand) with Skein-1024.
 *
 * @param[out] okm       Output keying material buffer.
 * @param[in]  okm_len   Desired output length in bytes.
 * @param[in]  ikm       Input keying material.
 * @param[in]  ikm_len   Length of IKM.
 * @param[in]  salt      Optional salt (NULL uses a zero-filled salt).
 * @param[in]  salt_len  Length of salt (ignored if salt is NULL).
 * @param[in]  info      Context/application-specific info string.
 * @param[in]  info_len  Length of info string.
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_KDF on failure.
 *
 * @code{.c}
 * uint8_t derived[64];
 * yumi_hkdf(derived, sizeof(derived),
 *           shared_secret, ss_len,
 *           NULL, 0,            // no salt
 *           (const uint8_t *)"session-key", 11);
 * @endcode
 */
yumi_crypto_err_t yumi_hkdf(
    uint8_t *okm, size_t okm_len,
    const uint8_t *ikm, size_t ikm_len,
    const uint8_t *salt, size_t salt_len,   /* NULL = zero salt */
    const uint8_t *info, size_t info_len);

/* ════════════════════════════════════════════════════════════════
 *  AEAD — Threefish-1024-CTR + Skein-1024-MAC
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Encrypt data with AEAD (Threefish-1024-CTR + Skein-1024-MAC).
 *
 * Produces ciphertext with an appended authentication tag.
 * The output buffer must have room for `plaintext_len + YUMI_AEAD_TAG_LEN`.
 *
 * @param[out] out            Ciphertext + tag output buffer.
 * @param[out] out_len        Receives actual output length.
 * @param[in]  plaintext      Data to encrypt.
 * @param[in]  plaintext_len  Length of plaintext.
 * @param[in]  aad            Additional authenticated data (may be NULL).
 * @param[in]  aad_len        Length of AAD.
 * @param[in]  nonce          Nonce (YUMI_AEAD_NONCE_LEN = 16 bytes). Must be unique per key.
 * @param[in]  key            Encryption key (YUMI_AEAD_KEY_LEN = 128 bytes).
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_ENCRYPT on failure.
 *
 * @code{.c}
 * uint8_t ct[msg_len + YUMI_AEAD_TAG_LEN];
 * size_t ct_len;
 * yumi_aead_encrypt(ct, &ct_len, msg, msg_len, NULL, 0, nonce, key);
 * @endcode
 */
yumi_crypto_err_t yumi_aead_encrypt(
    uint8_t       *out, size_t *out_len,
    const uint8_t *plaintext, size_t plaintext_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t  nonce[YUMI_AEAD_NONCE_LEN],
    const uint8_t  key[YUMI_AEAD_KEY_LEN]);

/**
 * @brief Decrypt AEAD ciphertext and verify the authentication tag.
 *
 * @param[out] out              Plaintext output buffer.
 * @param[out] out_len          Receives actual plaintext length.
 * @param[in]  ciphertext       Ciphertext + tag (from yumi_aead_encrypt).
 * @param[in]  ciphertext_len   Total length including the tag.
 * @param[in]  aad              Additional authenticated data (must match encryption).
 * @param[in]  aad_len          Length of AAD.
 * @param[in]  nonce            Nonce used during encryption.
 * @param[in]  key              Encryption key.
 * @return YUMI_CRYPTO_OK on success, YUMI_CRYPTO_ERR_DECRYPT if tag verification fails.
 */
yumi_crypto_err_t yumi_aead_decrypt(
    uint8_t       *out, size_t *out_len,
    const uint8_t *ciphertext, size_t ciphertext_len,  /* includes tag */
    const uint8_t *aad, size_t aad_len,
    const uint8_t  nonce[YUMI_AEAD_NONCE_LEN],
    const uint8_t  key[YUMI_AEAD_KEY_LEN]);

/* ════════════════════════════════════════════════════════════════
 *  Threefish-1024 — Raw block cipher (for custom constructions)
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Encrypt a single Threefish-1024 block (128 bytes).
 *
 * Low-level primitive; prefer yumi_aead_encrypt() for general use.
 *
 * @param[out] out    Ciphertext block (YUMI_THREEFISH_BLOCK_LEN bytes).
 * @param[in]  in     Plaintext block (YUMI_THREEFISH_BLOCK_LEN bytes).
 * @param[in]  key    Key (YUMI_THREEFISH_KEY_LEN = 128 bytes).
 * @param[in]  tweak  Tweak value (YUMI_THREEFISH_TWEAK_LEN = 16 bytes).
 * @return YUMI_CRYPTO_OK on success.
 */
yumi_crypto_err_t yumi_threefish_encrypt_block(
    uint8_t out[YUMI_THREEFISH_BLOCK_LEN],
    const uint8_t in[YUMI_THREEFISH_BLOCK_LEN],
    const uint8_t key[YUMI_THREEFISH_KEY_LEN],
    const uint8_t tweak[YUMI_THREEFISH_TWEAK_LEN]);

/* ════════════════════════════════════════════════════════════════
 *  Handshake helpers — Key combiners
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Triple-hybrid key combiner for invitation handshakes.
 *
 * Derives a session key from three independent KEM/ECDH shared secrets:
 *   - K1 = ML-KEM-1024 shared secret
 *   - K2 = FrodoKEM-1344 shared secret
 *   - K3 = BrainPool-512 ECDH shared secret
 *
 * @param[out] session_key  Derived session key (YUMI_TRANSPORT_KEY_LEN bytes).
 * @param[in]  k1           ML-KEM shared secret.
 * @param[in]  k1_len       Length of k1.
 * @param[in]  k2           FrodoKEM shared secret.
 * @param[in]  k2_len       Length of k2.
 * @param[in]  k3           BrainPool-512 ECDH shared secret.
 * @param[in]  k3_len       Length of k3.
 * @return YUMI_CRYPTO_OK on success.
 *
 * @code{.c}
 * uint8_t session_key[YUMI_TRANSPORT_KEY_LEN];
 * yumi_combine_invite_keys(session_key,
 *     mlkem_ss, YUMI_MLKEM_SHARED_SECRET_LEN,
 *     frodo_ss, YUMI_FRODO_SHARED_SECRET_LEN,
 *     bp512_ss, bp512_ss_len);
 * @endcode
 */
yumi_crypto_err_t yumi_combine_invite_keys(
    uint8_t session_key[YUMI_TRANSPORT_KEY_LEN],
    const uint8_t *k1, size_t k1_len,
    const uint8_t *k2, size_t k2_len,
    const uint8_t *k3, size_t k3_len);

/**
 * @brief Dual-hybrid key combiner for peer-to-peer handshakes.
 *
 * Derives a temporary key from ML-KEM-1024 + BrainPool-512 shared secrets.
 *
 * @param[out] temp_key  Derived temporary key (YUMI_TRANSPORT_KEY_LEN bytes).
 * @param[in]  k1        ML-KEM shared secret.
 * @param[in]  k1_len    Length of k1.
 * @param[in]  k2        BrainPool-512 ECDH shared secret.
 * @param[in]  k2_len    Length of k2.
 * @return YUMI_CRYPTO_OK on success.
 */
yumi_crypto_err_t yumi_combine_peer_keys(
    uint8_t temp_key[YUMI_TRANSPORT_KEY_LEN],
    const uint8_t *k1, size_t k1_len,
    const uint8_t *k2, size_t k2_len);

/**
 * @brief Derive the final transport key from a temporary key and epoch key.
 *
 * @param[out] transport_key  Final transport key (YUMI_TRANSPORT_KEY_LEN bytes).
 * @param[in]  temp_key       Temporary key from yumi_combine_peer_keys() or
 *                            yumi_combine_invite_keys().
 * @param[in]  epoch_key      Current group epoch key (YUMI_EPOCH_KEY_LEN bytes).
 * @return YUMI_CRYPTO_OK on success.
 *
 * @code{.c}
 * uint8_t transport_key[YUMI_TRANSPORT_KEY_LEN];
 * yumi_derive_transport_key(transport_key, temp_key, epoch_key);
 * // Use transport_key for AEAD encrypt/decrypt on this session.
 * @endcode
 */
yumi_crypto_err_t yumi_derive_transport_key(
    uint8_t transport_key[YUMI_TRANSPORT_KEY_LEN],
    const uint8_t temp_key[YUMI_TRANSPORT_KEY_LEN],
    const uint8_t epoch_key[YUMI_EPOCH_KEY_LEN]);

/* ════════════════════════════════════════════════════════════════
 *  Session lifetime
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Get a session timeout value with random jitter.
 *
 * Returns a timeout in seconds in the range
 * [YUMI_SESSION_TIMEOUT_BASE - JITTER, YUMI_SESSION_TIMEOUT_BASE + JITTER]
 * (approximately 55–65 minutes).
 *
 * @return Timeout in seconds.
 */
uint32_t yumi_session_timeout(void);

/* ════════════════════════════════════════════════════════════════
 *  AEAD — Pre-derived subkeys for high-throughput paths
 *
 *  Avoids re-deriving enc_key and mac_key from the master key on
 *  every packet.  Derive once, then use _keyed() variants.
 *  Wipe with yumi_aead_subkeys_wipe() when the master key changes.
 * ════════════════════════════════════════════════════════════════ */

/* Opaque storage for a pre-keyed Skein-1024 MAC context (avoids
 * re-processing the key block on every packet). */
#define YUMI_AEAD_MAC_CTX_SIZE 320

typedef struct {
    uint8_t enc_key[YUMI_THREEFISH_KEY_LEN]; /* CTR encryption subkey */
    uint8_t mac_key[YUMI_SKEIN_MAC_LEN];     /* Skein-MAC subkey      */
    _Alignas(8) uint8_t _mac_ctx[YUMI_AEAD_MAC_CTX_SIZE]; /* opaque */
} yumi_aead_subkeys_t;

/**
 * @brief Derive AEAD subkeys from a master key (enc_key + mac_key).
 */
yumi_crypto_err_t yumi_aead_derive_subkeys(
    yumi_aead_subkeys_t *out,
    const uint8_t key[YUMI_AEAD_KEY_LEN]);

/**
 * @brief Securely wipe AEAD subkeys.
 */
void yumi_aead_subkeys_wipe(yumi_aead_subkeys_t *sk);

/**
 * @brief AEAD encrypt using pre-derived subkeys (avoids per-packet key derivation).
 */
yumi_crypto_err_t yumi_aead_encrypt_keyed(
    uint8_t       *out, size_t *out_len,
    const uint8_t *plaintext, size_t plaintext_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t  nonce[YUMI_AEAD_NONCE_LEN],
    const yumi_aead_subkeys_t *sk);

/**
 * @brief AEAD decrypt using pre-derived subkeys (avoids per-packet key derivation).
 */
yumi_crypto_err_t yumi_aead_decrypt_keyed(
    uint8_t       *out, size_t *out_len,
    const uint8_t *ciphertext, size_t ciphertext_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t  nonce[YUMI_AEAD_NONCE_LEN],
    const yumi_aead_subkeys_t *sk);

#ifdef __cplusplus
}
#endif

#endif /* YUMI_CRYPTO_H */
