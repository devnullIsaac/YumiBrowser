# Cryptography

The crypto layer (`include/crypto.h`, `src/crypto/`) provides a unified post-quantum cryptography interface.

## Standards Basis

Yumi Browser's cryptography is built on **OpenSSL 3.x** (the same cryptographic library used by Chrome, Firefox, Edge, and a large fraction of the commercial and open-source software ecosystem) together with the **Open Quantum Safe (OQS) project's `oqs-provider`** for post-quantum primitives. The algorithms exposed through these libraries are standardized or standards-track:

- **ML-DSA-87** and **ML-KEM-1024** are NIST-standardized post-quantum algorithms (FIPS 204 and FIPS 203 respectively), selected through the NIST Post-Quantum Cryptography standardization process.
- **FrodoKEM-1344-SHAKE** is a conservative, generic-lattice post-quantum KEM. It was a NIST PQC Round 3 *alternate* candidate and was not selected by NIST for standardization. It is being standardized by ISO/IEC JTC 1/SC 27/WG 2 as a mechanism in a revision of ISO/IEC 18033-2 (asymmetric ciphers), and is recommended by the German Federal Office for Information Security (BSI) for post-quantum confidentiality.
- **BrainPool-P512r1** is an elliptic curve specified by RFC 5639, developed by the Brainpool consortium and widely used within European government and industry deployments.

These are not bespoke primitives. They are the same families of algorithms being deployed across mainstream browsers, operating systems, and security products that are adopting post-quantum cryptography. The Threefish-1024 / Skein-1024 symmetric and hashing layer is the project's own construction choice and is discussed separately in the [Long-Term Stability Commitment](08-stability-commitment.md); a dedicated construction document with test vectors is planned (see [Current Development Focus](23-development-focus.md)).

## Export Control Notice

Yumi Browser contains cryptographic functionality. Download, use, export, re-export, or transfer of this software may be restricted by the laws of your jurisdiction. Compliance with all applicable export control, import control, and sanctions laws is the sole responsibility of the individual user or redistributor.

## Primitives

| Category | Algorithm | Source | Security Level |
|----------|-----------|--------|---------------|
| **Signing** | ML-DSA-87 (FIPS 204) | oqs-provider | NIST Level 5 |
| **KEM (primary)** | ML-KEM-1024 | oqs-provider | Post-quantum lattice |
| **KEM (invitation)** | FrodoKEM-1344-SHAKE | oqs-provider | Plain LWE |
| **ECDH (fallback)** | BrainPool-512 | OpenSSL | Classical |
| **Hashing** | Skein-1024 | Embedded | 1024-bit output |
| **Symmetric** | Threefish-1024 | Embedded | 1024-bit block |
| **AEAD** | Threefish-1024-CTR + Skein-1024-MAC | Embedded | 128-byte key, 128-byte tag |
| **KDF** | HKDF (Skein-1024-MAC) | Embedded | 128-byte output |

## Key sizes

| Constant | Bytes |
|----------|-------|
| `YUMI_MLDSA_PUBLIC_KEY_LEN` | 2592 |
| `YUMI_MLDSA_SECRET_KEY_LEN` | 4896 |
| `YUMI_MLDSA_SIGN_LEN` | 4627 |
| `YUMI_MLKEM_PUBLIC_KEY_LEN` | 1568 |
| `YUMI_MLKEM_SECRET_KEY_LEN` | 3168 |
| `YUMI_MLKEM_CIPHERTEXT_LEN` | 1568 |
| `YUMI_MLKEM_SHARED_SECRET_LEN` | 32 |
| `YUMI_FRODO_PUBLIC_KEY_LEN` | 21520 |
| `YUMI_FRODO_SECRET_KEY_LEN` | 43088 |
| `YUMI_FRODO_CIPHERTEXT_LEN` | 21696 |
| `YUMI_BP512_PUBLIC_KEY_LEN` | 129 |
| `YUMI_BP512_SECRET_KEY_LEN` | 64 |
| `YUMI_AEAD_KEY_LEN` | 128 |
| `YUMI_AEAD_NONCE_LEN` | 16 |
| `YUMI_AEAD_TAG_LEN` | 128 |

## Initialization

```c
yumi_crypto_init();  // loads OpenSSL + oqs-provider
// ... use crypto functions ...
yumi_crypto_cleanup();
```

## API patterns

### Signing (ML-DSA-87)

```c
uint8_t pk[YUMI_MLDSA_PUBLIC_KEY_LEN], sk[YUMI_MLDSA_SECRET_KEY_LEN];
yumi_mldsa_keygen(pk, sk);

uint8_t sig[YUMI_MLDSA_SIGN_LEN];
yumi_mldsa_sign(sig, msg, msg_len, sk);

if (yumi_mldsa_verify(sig, msg, msg_len, pk) == YUMI_CRYPTO_OK)
    // valid
```

### Key Encapsulation (ML-KEM-1024)

```c
uint8_t pk[YUMI_MLKEM_PUBLIC_KEY_LEN], sk[YUMI_MLKEM_SECRET_KEY_LEN];
yumi_mlkem_keygen(pk, sk);

uint8_t ct[YUMI_MLKEM_CIPHERTEXT_LEN], ss[YUMI_MLKEM_SHARED_SECRET_LEN];
yumi_mlkem_encaps(ct, ss, pk);           // sender
yumi_mlkem_decaps(ss_recv, ct, sk);      // receiver
```

### AEAD Encryption (Threefish-1024-CTR + Skein-1024-MAC)

```c
uint8_t key[YUMI_AEAD_KEY_LEN], nonce[YUMI_AEAD_NONCE_LEN];
yumi_randombytes(key, sizeof(key));
yumi_randombytes(nonce, sizeof(nonce));

uint8_t ct[plaintext_len + YUMI_AEAD_TAG_LEN];
size_t ct_len;
yumi_aead_encrypt(ct, &ct_len, pt, pt_len, aad, aad_len, nonce, key);
yumi_aead_decrypt(pt_out, &pt_len, ct, ct_len, aad, aad_len, nonce, key);
```

### ECDH (BrainPool-512, classical fallback)

```c
yumi_bp512_keypair_t *kp = yumi_bp512_keygen();
uint8_t pk[YUMI_BP512_PUBLIC_KEY_LEN];
yumi_bp512_get_public(kp, pk);

uint8_t shared[YUMI_BP512_SHARED_SECRET_LEN];
yumi_bp512_derive(shared, my_kp, their_pk);
```

## Error codes

All functions return `yumi_crypto_err_t`:

| Code | Meaning |
|------|---------|
| `YUMI_CRYPTO_OK` | Success |
| `YUMI_CRYPTO_ERR_INVALID_PARAM` | NULL pointer or invalid argument |
| `YUMI_CRYPTO_ERR_KEYGEN` | Key generation failed |
| `YUMI_CRYPTO_ERR_SIGN` | Signing failed |
| `YUMI_CRYPTO_ERR_VERIFY` | Bad signature |
| `YUMI_CRYPTO_ERR_ENCRYPT` | AEAD encryption failed |
| `YUMI_CRYPTO_ERR_DECRYPT` | AEAD decryption / auth failed |
| `YUMI_CRYPTO_ERR_PROVIDER` | oqs-provider load failed |

## Implementation files — `src/crypto/`

| File | Contents |
|------|----------|
| `crypto.c` | ML-DSA, ML-KEM, FrodoKEM, BrainPool, AEAD, HKDF, randombytes — all via OpenSSL EVP |
| `skein.c`, `skein_block.c` | Skein-1024 hash implementation |
| `skein_debug.c` | Debug tracing for Skein internals |
| `SHA3api_ref.c` | SHA-3 reference (for Skein's init vector generation) |
| `brg_endian.h`, `brg_types.h` | Portability helpers |

## Security notes

- Secret key material is wiped with `yumi_memzero()` after use.
- All functions validate parameters before performing operations.
- The oqs-provider must be loadable at runtime (bundled or system-installed).
- opaque types (`yumi_bp512_keypair_t`, `yumi_skein_ctx_t`) are heap-allocated.
