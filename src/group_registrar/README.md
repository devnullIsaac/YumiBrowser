## Group Registrar

The Group Registrar is a self-contained C library that serves as the **governance layer for peer-to-peer groups** in Yumi Browser. Every peer in a group holds a local replica of its registrar. It answers three questions: *who is in this group, what can they do, and can I trust this copy?*

It is **not** a messaging protocol or networking layer — it is the *state document* that the networking layer synchronizes. Think of it as the constitution of a P2P group: it records membership, permissions, cryptographic identity, infrastructure endpoints, and every mutation ever made, in a signed, hash-chained audit log.

### Quick Start

```c
// ── Create a group and invite a peer ──────────────────────────
yumi_crypto_init();

gr_identity_t owner;
gr_identity_generate(&owner);

gr_registrar_t *reg;
gr_create(&reg, "/path/to/group.db", "My Group",
          GR_GROUP_PRIVATE, &owner);
gr_sign(reg, &owner);

uint8_t *invite_blob;
size_t invite_len;
uint8_t verify_token[GR_HASH_LEN];
gr_invite_create(reg, &owner, 0, &invite_blob, &invite_len, verify_token);
// Send invite_blob to the invitee via any channel.

// ── Join a group via invite ───────────────────────────────────
gr_invite_ticket_t ticket;
gr_invite_parse(invite_blob, invite_len, &ticket);

gr_registrar_t *joined;
gr_open(&joined, "/path/to/joined.db", ticket.group_id);
gr_join_begin(joined, &ticket);

// Chat works immediately. Governance mutations are gated
// until quorum-based verification completes.
```

---

### Architecture

| | |
|-|-|
| **Language** | C11, built with Meson |
| **Storage** | Embedded DuckDB — one file per group (or `:memory:` for tests) |
| **Crypto** | OpenSSL 3.x + oqs-provider (post-quantum primitives), Skein-1024 / Threefish-1024 (embedded) |
| **Handle** | Opaque `gr_registrar_t *` — DB connection, cached header, ~60 prepared statements, join state |
| **API** | 105 public functions, all returning `gr_error_t` (except boolean queries and `void` teardown) |

**21 source files**, single-subsystem-per-file:

| File | Responsibility |
|------|---------------|
| `lifecycle.c` | `gr_create`, `gr_open`, `gr_close` — handle allocation, DB init, teardown |
| `db.c` | Schema (11 tables), header load/save, ~60 prepared statements, column validation, migrations |
| `peer.c` | Peer CRUD: add, kick, ban, leave, address update, observed address, touch, role assignment |
| `role.c` | RBAC: role creation/removal/modification, permission bitmask checks, `gr_has_permission` |
| `audit.c` | Hash-chained, ML-DSA-87-signed audit log — append, list, count, chain verification, fork detection, retention enforcement |
| `crypto.c` | Registrar signing/verification, data signing, AEAD encryption (epoch key), per-peer ML-KEM-1024 key encapsulation |
| `serialize.c` | Full serialization (`GREG` v2), delta sync (`GRDT`), deserialization, merge with conflict detection |
| `epoch.c` | Epoch key rotation, current/historical epoch queries |
| `invite.c` | Invite ticket creation (`GRINV` v3), parsing, invalidation, usage tracking |
| `join.c` | Join verification state machine — quorum-based attestation with dissent callbacks |
| `icon.c` | Group icon storage (image/video), content hashing, static frame fallback |
| `server.c` | Signaling & rebroadcast server registry |
| `webapp.c` | Authorized webapp allowlist |
| `retention.c` | Message/file/registrar size retention policy |
| `identity.c` | ML-DSA-87 + ML-KEM-1024 keypair generation, peer ID derivation (`Skein-1024(pk)` truncated), secure wipe |
| `behavior.c` | Passive behavioral analysis — burst detection, churn, admin abuse scoring, delta anomalies, snapshots |
| `blocklist.c` | DuckDB-backed per-IP blocklist for the bootstrap listener — tracks failed auth attempts, blocks abusive IPs |
| `buf.c` / `buf.h` | Growable write buffer + sequential reader for wire format serialization |
| `util.c` | Hashing, ID comparison, timestamps (ms + ns), error strings |
| `internal.h` | Shared internal declarations, prepared statement handles |

---

### Database Schema (11 tables)

All SELECT queries use **explicit column lists** with positional extraction, validated at startup by `validate_table_columns()`.

| Table | Columns | Purpose |
|-------|---------|---------|
| `gr_header` | 16 | Single-row envelope: group_id, name, type, version, timestamps, epoch_id, retention policy, owner\_id/key, signer\_id/key, signature (ML-DSA-87), hash (Skein-1024) |
| `gr_peers` | 12 | peer_id (PK), kem_pk (ML-KEM-1024), sign_key (ML-DSA-87), ip, port, status, role_id, joined_at, removed_at, last_seen, removed_reason, removed_by |
| `gr_roles` | 6 | role_id (PK), name, permissions (bitmask), sign_key (ML-DSA-87), created_at, modified_at. Max 50 roles |
| `gr_webapps` | 5 | hash (PK), name, version, added_at, added_by |
| `gr_servers` | 8 | id_hash (PK), type (signaling/rebroadcast), ip, port, sign_key (ML-DSA-87), service_hash, content_kem_pk, content_kem_sk |
| `gr_epochs` | 5 | epoch_id (PK), epoch_key (Threefish-1024), created_at, expired_at, created_by |
| `gr_audit_log` | 10 | entry_hash (PK), timestamp, change_type, actor_id, target_id, signature (ML-DSA-87), registrar_version, detail, prev_hash, timestamp_ns |
| `gr_invites` | 7 | verification_token (PK), created_at, expires_at, created_by, invalidated, used, used_by |
| `gr_group_icon` | 10 | Single-row: id, data blob, mime_type, width, height, is_video, static_frame, content_hash, updated_at, updated_by |
| `gr_ip_blocklist` | 5 | ip (PK), fail_count, blocked, blocked_at, last_attempt — bootstrap listener abuse prevention |
| `gr_schema_meta` | 2 | id (PK), schema_version — versioned migration tracking |

The header is persisted via DELETE + INSERT (wrapped in a transaction). Schema migrations are applied sequentially via `gr_db_migrate()`, with version tracking in `gr_schema_meta`.

---

### Cryptographic Design

All crypto goes through the Yumi crypto abstraction layer (`include/crypto.h`, `src/crypto/`), backed by **OpenSSL 3.x** and the **oqs-provider** for post-quantum primitives. No algorithm negotiation, no classical-only fallback paths.

| Primitive | Algorithm | Usage |
|-----------|-----------|-------|
| **Peer identity** | ML-DSA-87 signing keypair (2592-byte pk, 4896-byte sk) + ML-KEM-1024 KEM keypair | Every peer has both; `peer_id = Skein-1024(sign_pk)[0..31]` |
| **Registrar signing** | ML-DSA-87 detached signature (4627 bytes) | Owner/authorized signer signs a canonical header hash |
| **Audit log** | Skein-1024 hash (128 bytes) + ML-DSA-87 detached signature | Each entry hashes `prev_hash ‖ timestamp ‖ change_type ‖ actor_id ‖ target_id ‖ version ‖ detail`, then signs the hash |
| **Group encryption** | Threefish-1024-CTR + Skein-1024-MAC AEAD (128-byte key, 128-byte tag) | Uses the current epoch key; nonce + epoch_id as additional data |
| **Per-peer encryption** | ML-KEM-1024 key encapsulation | Sender encapsulates a shared secret to the peer's `kem_pk`; receiver decapsulates with `kem_sk` |
| **Hashing** | Skein-1024 (128 bytes) | Content hashes, peer IDs, invite tokens, group icon verification |

**Canonical header** for signing: `group_id ‖ group_type ‖ group_name ‖ version ‖ created_at ‖ epoch_id ‖ retention{3 fields} ‖ owner_id ‖ owner_sign_key ‖ signer_id ‖ signer_sign_key`. The Skein-1024 hash of this is ML-DSA-87-signed.

---

### Audit Log

The audit log is the core tamper-evidence mechanism:

- **Hash-chained**: Each entry's hash includes the previous entry's hash (`prev_hash`), forming a chain analogous to a blockchain. The genesis entry uses a zeroed `prev_hash`.
- **Signed**: Each entry's hash is ML-DSA-87-signed by the actor who performed the mutation.
- **22 change types**: From peer joins/kicks to epoch rotations to icon changes and IP blocks (see `gr_change_type_t`).
- **Fork detection**: `gr_audit_list_forks()` identifies hash-chain divergences caused by concurrent offline mutations.
- **Retention enforcement**: `gr_audit_enforce_retention()` prunes entries exceeding the configured byte limit (`GR_AUDIT_MAX_BYTES`, default 25 MB).
- **Chain verification**: `gr_audit_verify_chain()` walks the entire log and reports the first broken link, if any.

Every mutating API call in the registrar ends with `gr_audit_append(...)` — there is no mutation without an audit trail.

---

### Permission System (RBAC)

Permissions are a 32-bit bitmask with 11 defined flags:

```
KICK_MEMBER | BAN_MEMBER | INVITE_MEMBER | ADD_WEBAPP | REMOVE_WEBAPP
EDIT_ROLES | EDIT_RETENTION | EDIT_SERVERS | ROTATE_EPOCH | SIGN_REGISTRAR
SET_GROUP_ICON
```

The **owner** has `GR_PERM_OWNER = 0xFFFFFFFF` (all bits set). Every mutating operation checks `gr_has_permission()`, which resolves the peer's role bitmask and tests the required flag. The owner bypasses all checks.

Roles are named permission sets (max 50 per group), each with their own sign_key generated at creation.

---

### Join Verification

When a new peer joins via an invite, they hold a copy of the registrar but **cannot trust it** — it could have been tampered with in transit. Join verification solves this with a quorum-based attestation protocol.

**State machine**: `NONE → PROVISIONAL → VERIFIED | FAILED`

1. **`gr_join_begin()`** — Cross-checks the fetched registrar's `group_id`, `owner_sign_key`, and `registrar_hash` against the invite ticket. Generates a random 32-byte nonce. Enters `PROVISIONAL` state.

2. **Attestation collection** — The joiner connects to existing peers. Each peer signs an attestation: `group_id ‖ owner_id ‖ owner_sign_key ‖ version ‖ nonce` (2788 bytes with ML-DSA-87 keys). Submitted via `gr_join_submit_peer_header()`.

3. **Quorum** — Required attestations = ⌈√(active_peers − 2)⌉, clamped to [3, 10]. Groups < 5 members use `small_group_bypass`. The inviter is explicitly excluded (`GR_ERR_JOIN_INVITER_EXCLUDED`).

4. **Evaluation** — `gr_join_evaluate()` checks quorum. On disagreement, a dissent callback fires so the user can override or reject.

5. **Governance gating** — While `PROVISIONAL`, all governance mutations are blocked. Address updates, peer touch, and incoming sync/deserialization are allowed so the joiner can participate immediately.

---

### Serialization & Sync

Three wire formats:

| Format | Magic | Version | Purpose |
|--------|-------|---------|---------|
| **Full** | `GREG` | 2 | Complete registrar snapshot — header, peers, roles, webapps, servers, epochs, audit entries |
| **Delta** | `GRDT` | — | Incremental sync — only audit entries with `registrar_version > since_version` |
| **Invite** | `GRINV` | 3 | Lightweight ticket — group identification, bootstrap peers, signaling servers, verification token, inviter ML-KEM-1024 public key |

**Full serialization** has two modes:
- `GR_SERIALIZE_FULL` — includes epoch keys (for active sync)
- `GR_SERIALIZE_OWNER` — owner-only backup

**Delta sync** (`gr_apply_delta`):
- Deduplicates by `entry_hash`
- Verifies each entry's Skein-1024 hash (including `prev_hash` chain link) and ML-DSA-87 signature
- Rejects invalid entries
- Reports results via `gr_merge_result_t` (new / duplicate / rejected counts)
- Detects forks (concurrent offline mutations) and reports them via `gr_audit_list_forks()`
- Anomaly callback (`gr_set_delta_anomaly_callback`) fires on suspicious delta patterns (e.g., 30-day offline gap)

`gr_deserialize()` replaces the entire registrar state: clears all tables, inserts from the blob, verifies the header signature.

---

### Invite System

Invites are **lightweight tickets** — they do NOT contain the full registrar.

Wire format (`GRINV` v3):
- **Group identification**: group_id, name, type, owner_sign_key, registrar_hash snapshot
- **Network discovery**: up to 5 bootstrap peers (Fisher-Yates shuffled from active peers, excluding inviter) + signaling servers
- **Security**: verification_token, inviter ML-DSA-87 signing key, inviter ML-KEM-1024 public key, expiry timestamp
- **Integrity**: ML-DSA-87 signature over all preceding bytes

Lifecycle: create → (optionally invalidate) → mark used. Tracked in `gr_invites` for auditability. Private groups require `GR_PERM_INVITE_MEMBER`; public groups allow any member to invite.

---

### Epoch Key Rotation

Epoch keys are 128-byte symmetric keys (Threefish-1024) for group-wide encryption. On rotation:
1. Current epoch expired (timestamp set)
2. New epoch created with fresh random key
3. Both operations in a single DB transaction
4. Historical epochs retained for decrypting old messages

Encryption prepends `nonce ‖ epoch_id_le32` to ciphertext; a 128-byte Skein-1024-MAC tag is appended. Decryption tries the specified epoch_id first, then iterates active epochs (key rotation mid-conversation).

---

### Behavioral Analysis

Pure read-only analysis of audit logs, peer history, and sync patterns. No mutations, no side effects. All thresholds are configurable via `gr_behavior_config_t`.

| Analysis | Function | Output |
|----------|----------|--------|
| Per-actor burst | `gr_behavior_actor_burst()` | Actions/min, destructive count, `burst_detected` flag |
| Group-wide mutation rate | `gr_behavior_mutation_rate()` | Mutations/min, distinct actors, `swarm_detected` flag |
| Admin abuse scoring | `gr_behavior_admin_score()` | Kicks/bans/removes ratio, `abuse_suspected` flag |
| Peer churn | `gr_behavior_peer_churn()` | Active/kicked/banned/left/stale counts, churn rate |
| Delta anomaly | `gr_behavior_delta_score()` | Bytes/entries per offline day, `anomalous` flag |
| Epoch rotation pattern | `gr_behavior_epoch_pattern()` | Rotations in window, avg lifetime, `excessive_rotation` flag |
| Network health | `gr_behavior_network_score()` | Last update age, estimated size, avg entry interval |
| **Full snapshot** | `gr_behavior_snapshot()` | All of the above in one pass, `needs_attention` flag |

Group-size scaling (when enabled) adjusts group-wide thresholds by √(active_peers / 10) so large groups aren't false-flagged. Per-actor thresholds are never scaled.

An alert callback (`gr_set_behavior_alert_callback`) can be registered for real-time notification when any threshold is breached.

---

### Group Icon

Supports static images (PNG, JPEG, WebP) and video loops (MP4, WebM), up to 512×512 at 10 MB. Video icons require a static frame fallback. The icon's Skein-1024 content hash enables efficient sync — peers compare hashes before fetching the full blob. Icons are excluded from the serialized registrar to avoid bloating sync traffic.

---

### Infrastructure Registry

Two server types:
- **Signaling servers** — peer discovery & NAT traversal
- **Rebroadcast servers** — message relay when direct P2P fails

Each server has: id_hash, ip, port, sign_key (ML-DSA-87), service_hash, content_kem_pk, content_kem_sk (ML-KEM-1024). Requires `GR_PERM_EDIT_SERVERS`.

---

### Webapp Authorization

An allowlist of authorized webapps identified by content hash. Enables the browser to verify that a webapp served within a group context is sanctioned by group governance. Requires `GR_PERM_ADD_WEBAPP` / `GR_PERM_REMOVE_WEBAPP`.

---

### Error Handling

18 distinct error codes (`gr_error_t`) covering: parameter validation, not-found, already-exists, authorization, signature invalid, crypto failure, DB error, size exceeded, role limit, epoch mismatch, serialization, OOM, invite expired/invalid, merge conflict, join unverified/failed, and inviter exclusion. `gr_error_str()` maps each to a human-readable string.

---

### Public API (105 functions)

| Subsystem | Count | Key Functions |
|-----------|-------|---------------|
| Identity | 3 | `gr_identity_generate`, `gr_identity_derive_id`, `gr_identity_wipe` |
| Lifecycle | 4 | `gr_create`, `gr_open`, `gr_close`, `gr_get_header` |
| Peers | 12 | `gr_peer_add`, `gr_peer_kick`, `gr_peer_ban`, `gr_peer_leave`, `gr_peer_update_address`, `gr_peer_observed_address`, `gr_peer_touch`, `gr_peer_set_role`, `gr_peer_get`, `gr_peer_count`, `gr_peer_list`, `gr_peer_is_authorized` |
| Roles | 7 | `gr_role_add`, `gr_role_remove`, `gr_role_set_permissions`, `gr_role_get`, `gr_role_list`, `gr_role_count`, `gr_has_permission` |
| Webapps | 5 | `gr_webapp_add`, `gr_webapp_remove`, `gr_webapp_is_authorized`, `gr_webapp_list`, `gr_webapp_count` |
| Servers | 4 | `gr_server_add`, `gr_server_remove`, `gr_server_list`, `gr_server_count` |
| Epochs | 5 | `gr_epoch_rotate`, `gr_epoch_get_current`, `gr_epoch_get`, `gr_epoch_list`, `gr_epoch_count` |
| Retention | 2 | `gr_retention_set`, `gr_retention_get` |
| Icon | 5 | `gr_group_icon_set`, `gr_group_icon_get`, `gr_group_icon_remove`, `gr_group_icon_hash`, `gr_group_icon_free` |
| Signing & Crypto | 8 | `gr_sign`, `gr_verify`, `gr_sign_data`, `gr_verify_data`, `gr_encrypt`, `gr_decrypt`, `gr_encrypt_for_peer`, `gr_decrypt_from_peer` |
| Invites | 7 | `gr_invite_create`, `gr_invite_invalidate`, `gr_invite_check`, `gr_invite_mark_used`, `gr_invite_list`, `gr_invite_count`, `gr_invite_parse` |
| Join Verification | 9 | `gr_join_begin`, `gr_join_set_dissent_callback`, `gr_join_submit_peer_header`, `gr_join_evaluate`, `gr_join_get_state`, `gr_join_get_nonce`, `gr_join_export_header_attestation`, `gr_join_list_unattested_peers`, `gr_is_trusted` |
| Audit & Serialization | 18 | `gr_audit_list`, `gr_audit_count`, `gr_audit_verify_chain`, `gr_audit_enforce_retention`, `gr_audit_list_forks`, `gr_serialize`, `gr_deserialize`, `gr_serialize_delta`, `gr_apply_delta`, `gr_set_delta_anomaly_callback`, `gr_set_persist_prompt_callback`, `gr_hash`, `gr_id_equal`, `gr_timestamp_ms`, `gr_timestamp_ns`, `gr_schema_version`, `gr_free`, `gr_error_str` |
| Behavioral Analysis | 12 | `gr_behavior_actor_burst`, `gr_behavior_mutation_rate`, `gr_behavior_admin_score`, `gr_behavior_peer_churn`, `gr_behavior_delta_score`, `gr_behavior_stale_peers`, `gr_behavior_epoch_pattern`, `gr_behavior_network_score`, `gr_behavior_snapshot`, `gr_behavior_set_config`, `gr_behavior_get_config`, `gr_set_behavior_alert_callback` |
| Blocklist | 4 | `gr_blocklist_check`, `gr_blocklist_record_fail`, `gr_blocklist_reset`, `gr_blocklist_cleanup` |

---

### Testing

6 dedicated test suites in `tests/`:

| Test | Coverage |
|------|----------|
| `test_audit.c` | Audit log append, hash chain integrity, retention enforcement |
| `test_db.c` | Schema creation, column validation, prepared statements, migrations |
| `test_join_verify.c` | Full join verification flow, quorum, dissent, small group bypass |
| `test_delta.c` | Full/delta serialization, LWW merge (both directions), fork detection (3 tests), fork introspection (4 tests), fork-aware retention, anomaly callbacks, schema migration (4 tests), concurrent partition (2-admin + 3-way), bidirectional sync, edge cases — **46 tests, 172 assertions, 23 sections** |
| `test_crypto.c` | Signing, verification, encryption/decryption, ML-KEM encapsulation, key derivation |
| `test_buf.c` | Buffer growth, sequential read/write, overflow handling |

---

### Design Principles

1. **Every peer holds a complete replica** — no central server. The registrar is the shared state document.
2. **Every mutation is audited** — hash-chained, signed audit log makes tampering detectable.
3. **Trust is established through quorum** — new joiners verify against multiple independent peers, not the inviter.
4. **Governance is permission-gated** — RBAC with bitmask system, owner as superuser.
5. **Crypto is non-negotiable** — ML-DSA-87 identities, Skein-1024 hashing, Threefish-1024-CTR + Skein-1024-MAC encryption, ML-KEM-1024 key encapsulation, all via OpenSSL 3.x + oqs-provider. No algorithm negotiation.
6. **Prepared statement caching** — ~60 statements prepared once at open, reused throughout the handle's lifetime, destroyed on close.
7. **Provisional state gating** — new joiners communicate immediately but cannot alter governance until verified.
8. **Fork-aware sync** — concurrent offline mutations are detected, reported, and merged via LWW with full introspection.
9. **Behavioral analysis** — passive, read-only anomaly detection with configurable thresholds and group-size scaling.