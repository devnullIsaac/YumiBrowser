

## YumiBrowser Crypto Migration — Task List

### 1. Drop libsodium, switch to OpenSSL + embedded primitives
- Replace `crypto_sign_ed25519_*` → ML-DSA-87 via oqs-provider (FIPS 204, NIST Level 5, fully post-quantum)
- Replace `crypto_generichash` (BLAKE2b) → Skein-1024 (embedded reference implementation)
- Replace `sodium_memzero` → `OPENSSL_cleanse`
- Replace `randombytes_buf` → `RAND_bytes`
- Replace `crypto_sign_ed25519_pk_to_curve25519` / `sk_to_curve25519` → remove (no longer deriving X25519 from Ed25519)
- Replace XChaCha20-Poly1305 → Threefish-1024-CTR + Skein-1024-MAC (embedded reference implementation)
- Remove libsodium from meson.build / deps, add OpenSSL linkage
- Port all call sites in group_registrar (identity.c, invite.c, lifecycle.c, util.c, behavior.c)

### 1a. Embed Skein-1024 and Threefish-1024
- Embed Schneier et al. public domain C reference implementation for Skein/Threefish
- Skein-1024: replaces all hashing (peer ID, HKDF, integrity checks)
- Threefish-1024-CTR + Skein-1024-MAC: replaces all symmetric encryption (epoch, transport)
- HKDF built on Skein-1024-MAC (Skein has native MAC mode — no HMAC wrapper needed)
- Optionally register as custom OpenSSL `EVP_MD` / `EVP_CIPHER` providers, or call directly

### 2. Integrate oqs-provider for post-quantum primitives
- Build and configure oqs-provider as an OpenSSL provider
- ML-KEM-1024 (key encapsulation — used in all handshakes):
  - Verify `EVP_KEM` fetch for ML-KEM-1024 works
  - Write wrapper functions: `yumi_mlkem_keygen()`, `yumi_mlkem_encaps()`, `yumi_mlkem_decaps()`
- FrodoKEM-1344-SHAKE (key encapsulation — invitation handshake only, algorithm diversity):
  - Plain LWE (no ring/module structure) — survives breakthroughs against ML-KEM's algebraic structure
  - Larger ciphertexts (~21 KB) — acceptable for one-time invitation handshake
  - Write wrapper functions: `yumi_frodo_keygen()`, `yumi_frodo_encaps()`, `yumi_frodo_decaps()`
- ML-DSA-87 (signing — FIPS 204, NIST Level 5):
  - Public key: 2,592 B, Signature: 4,627 B
  - Fully post-quantum, matches ML-KEM-1024 security level
  - Write wrapper functions: `yumi_mldsa_keygen()`, `yumi_mldsa_sign()`, `yumi_mldsa_verify()`

### 3. Implement BrainPool-512 ECDH
- Ephemeral keypair generation on `brainpoolP512r1` via `EVP_PKEY_keygen`
- ECDH shared secret derivation via `EVP_PKEY_derive`
- Wrap in: `yumi_bp512_keygen()`, `yumi_bp512_ecdh()`

### 5. Build the ephemeral triple-hybrid handshake (invitation only)
- Combine three KEMs from three distinct mathematical families:
  - ML-KEM-1024 (module lattice — NIST standard) → K1
  - FrodoKEM-1344-SHAKE (plain LWE — conservative lattice, different construction) → K2
  - BP512-ECDH (classical ECC — vetted, fallback) → K3
- Combiner: `session_key = HKDF-Skein-1024(K1 ‖ K2 ‖ K3, "yumi-invite-v1")`
- Attacker must break: lattice problems AND plain LWE AND ECDLP — all for same session
- Use session_key to encrypt registrar sync during initial join (Threefish-1024-CTR + Skein-1024-MAC)
- Authenticate handshake transcript with ML-DSA-87 signatures (verified against invite token / registrar)
- Dispose all ephemeral keys after sync completes

### 6. Build the peer-to-peer ephemeral handshake (sustained connections)
- Greet packet: exchange `registrar_version` as `int64_t`
- Ephemeral: ML-KEM-1024 + BP512-ECDH
- `temp_key = HKDF-Skein-1024(K1 ‖ K2, "yumi-peer-v1")`
- If version mismatch: stream delta encrypted under `temp_key` (Threefish-1024-CTR + Skein-1024-MAC)
- Derive `transport_key = HKDF-Skein-1024(temp_key ‖ epoch_key, "yumi-transport-v1")`
- All subsequent traffic encrypted with `transport_key` (Threefish-1024-CTR + Skein-1024-MAC)

### 7. Implement session lifetime and forced re-handshake
- Session timeout: `3600 + random_uniform(-300, +300)` seconds
- On timeout: drop UDP connection, `OPENSSL_cleanse` the `temp_key`
- Reconnect triggers fresh ephemeral handshake

### 8. Implement epoch rotation handling
- Triggers: kick, ban, leave, scheduled timer (configurable, e.g., 7 days)
- On rotation mid-session: re-derive `transport_key = HKDF-Skein-1024(temp_key ‖ new_epoch_key, "yumi-transport-v1")`
- No full re-handshake needed — existing `temp_key` remains valid until session timeout
- Broadcast delta with new epoch key to all connected peers

### 9. Update registrar per-peer storage
- Remove encryption public key from peer record (no longer stored)
- Keep: `peer_id` (128 B, Skein-1024 hash) + `sign_key` (2,592 B, ML-DSA-87 public key)
- Per-peer cost: ~2,720 B → 200 MB soft limit holds ~73K peers before user prompt
- Update DuckDB schema in db.c
- Update `gr_peer_t` struct in group_registrar.h
- Update `GR_PEER_ID_LEN` → 128, `GR_PUBLIC_KEY_LEN` → 2592, `GR_SECRET_KEY_LEN` → 4896, `GR_SIGN_LEN` → 4627
- Update `GR_HASH_LEN` → 128 (Skein-1024 output)

### 10. Signing: ML-DSA-87 (fully post-quantum)
- ML-DSA-87 (FIPS 204, NIST Level 5) — matches ML-KEM-1024 security level
- Public key: 2,592 B, Secret key: 4,896 B, Signature: 4,627 B
- Fully quantum-resistant — closes the authentication gap entirely
- Both key exchange AND signing are now post-quantum
- Registrar size impact: ~2.7 KB per peer (vs. ~185 B with Ed448)
  - 200 MB soft limit → ~73K peers before user prompt
  - Prompt is advisory only — user can OK to grow limit (DuckDB stores on disk, not RAM)
  - 2M peers ≈ 5.4 GB — large but DuckDB handles it fine

### 11. Testing
- Port existing tests (`test_crypto`, `test_join_verify`, `test_audit`, `test_delta`) from libsodium to OpenSSL
- Add handshake round-trip test: full triple-hybrid invitation flow (ML-KEM + FrodoKEM + BP512)
- Add handshake round-trip test: peer-to-peer dual-hybrid flow (ML-KEM + BP512)
- Add epoch rotation mid-session test
- Add session timeout and re-handshake test
- Test Skein-1024 against known test vectors (from Skein specification)
- Test Threefish-1024 against known test vectors (from Skein specification)
- Benchmark Threefish-1024-CTR + Skein-1024-MAC vs AES-256-GCM throughput