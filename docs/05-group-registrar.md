# Group Registrar API Reference

The Group Registrar (`include/group_registrar.h`, `src/group_registrar/`) is the **governance layer for peer-to-peer groups**. It is the state document that the networking layer synchronizes — not a messaging protocol itself.

## Overview

- **Who is in this group?** — Peer membership with ML-DSA-87 cryptographic identity
- **What can they do?** — Role-based permissions (up to 50 roles)
- **Can I trust this copy?** — Signed, hash-chained audit log with join verification
- **Storage**: Each group has its own DuckDB file (or `:memory:`). The opaque `gr_registrar_t *` wraps a DuckDB database, connection, mutex, ~60 cached prepared statements, header cache, and join state.
- **Prerequisite**: Call `yumi_crypto_init()` before any group registrar function.

---

## Error Codes

All functions return `gr_error_t`:

```c
typedef enum {
    GR_OK                        =  0,  /* Success                                    */
    GR_ERR_INVALID_PARAM         = -1,  /* NULL pointer or invalid argument            */
    GR_ERR_NOT_FOUND             = -2,  /* Requested item not found                    */
    GR_ERR_ALREADY_EXISTS        = -3,  /* Item already exists (duplicate)              */
    GR_ERR_UNAUTHORIZED          = -4,  /* Caller lacks required permission             */
    GR_ERR_SIGNATURE_INVALID     = -5,  /* Digital signature verification failed        */
    GR_ERR_CRYPTO                = -6,  /* Generic cryptographic operation failure       */
    GR_ERR_DB                    = -7,  /* Database operation failed                    */
    GR_ERR_SIZE_EXCEEDED         = -8,  /* Size limit exceeded                         */
    GR_ERR_ROLE_LIMIT            = -9,  /* Maximum role count (GR_MAX_ROLES) reached    */
    GR_ERR_EPOCH_MISMATCH        = -10, /* Epoch ID mismatch during decrypt             */
    GR_ERR_SERIALIZATION         = -11, /* Serialization / deserialization error         */
    GR_ERR_OUT_OF_MEMORY         = -12, /* Memory allocation failed                    */
    GR_ERR_INVITE_EXPIRED        = -13, /* Invite ticket has expired                   */
    GR_ERR_INVITE_INVALID        = -14, /* Invite blob is malformed or invalid          */
    GR_ERR_MERGE_CONFLICT        = -15, /* Delta merge conflict                        */
    GR_ERR_JOIN_UNVERIFIED       = -16, /* Operation blocked — join not yet verified    */
    GR_ERR_JOIN_FAILED           = -17, /* Join verification failed (dissent/mismatch)  */
    GR_ERR_JOIN_INVITER_EXCLUDED = -18, /* Inviter cannot self-attest                   */
} gr_error_t;
```

Use `gr_error_str(err)` to get a human-readable string for any error code.

---

## Constants

### Cryptographic Key Sizes

| Constant | Value | Description |
|----------|-------|-------------|
| `GR_PEER_ID_LEN` | 32 | Truncated Skein-1024 hash of signing public key |
| `GR_PUBLIC_KEY_LEN` | 2592 | ML-DSA-87 signing public key |
| `GR_SECRET_KEY_LEN` | 4896 | ML-DSA-87 signing secret key |
| `GR_SIGN_LEN` | 4627 | ML-DSA-87 signature |
| `GR_SYMMETRIC_KEY_LEN` | 128 | Threefish-1024 symmetric key |
| `GR_HASH_LEN` | 128 | Skein-1024 hash output (1024 bits) |
| `GR_NONCE_LEN` | 16 | AEAD nonce (Threefish-1024-CTR) |
| `GR_MAC_LEN` | 128 | Skein-1024 MAC tag |
| `GR_EPOCH_KEY_LEN` | 128 | Threefish-1024 epoch key |
| `GR_KEM_PUBLIC_KEY_LEN` | 1568 | ML-KEM-1024 public key |
| `GR_KEM_SECRET_KEY_LEN` | 3168 | ML-KEM-1024 secret key |
| `GR_KEM_CIPHERTEXT_LEN` | 1568 | ML-KEM-1024 ciphertext |
| `GR_SERVICE_HASH_LEN` | 128 | Skein-1024 hash for webapp content |

### Limits

| Constant | Value | Description |
|----------|-------|-------------|
| `GR_MAX_ROLES` | 50 | Maximum roles per group |
| `GR_MAX_IP_LEN` | 46 | Maximum IP address string length |
| `GR_MAX_NAME_LEN` | 128 | Maximum name string length |

### Retention Defaults

| Constant | Value | Description |
|----------|-------|-------------|
| `GR_DEFAULT_MESSAGE_RETENTION_MS` | 30 days (ms) | Default message retention |
| `GR_DEFAULT_FILE_RETENTION_MS` | 180 days (ms) | Default file retention |
| `GR_DEFAULT_REGISTRAR_MAX_BYTES` | 200 MB | Default max registrar size |
| `GR_RETENTION_FOREVER` | 0 | Retain forever sentinel |

### Join Verification

| Constant | Value | Description |
|----------|-------|-------------|
| `GR_JOIN_NONCE_LEN` | 32 | Per-session challenge token |
| `GR_JOIN_MIN_QUORUM` | 3 | Floor for attestation quorum |
| `GR_JOIN_MAX_QUORUM` | 10 | Cap for attestation quorum |
| `GR_JOIN_MAX_TRACKED_PEERS` | 64 | Max peer votes to store |
| `GR_JOIN_SMALL_GROUP_THRESHOLD` | 5 | Groups below this skip attestation |
| `GR_JOIN_ATTESTATION_LEN` | 2788 | Attestation payload size |
| `GR_INVITE_MAX_BOOTSTRAP` | 5 | Max bootstrap peers in invite |
| `GR_INVITE_VERSION` | 3 | Invite wire format version |

### Audit / Delta / Icon

| Constant | Value | Description |
|----------|-------|-------------|
| `GR_AUDIT_MAX_BYTES` | 25 MB | Max audit log size |
| `GR_AUDIT_EST_ROW_BYTES` | 500 | Estimated bytes per audit row |
| `GR_DELTA_MAX_BYTES` | 1 MB | Max delta blob size |
| `GR_DELTA_ANOMALY_GAP_MS` | 30 days (ms) | Anomaly detection threshold |
| `GR_PERSIST_PROMPT_BYTES` | 50 MB | Persist-to-disk prompt threshold |
| `GR_GROUP_ICON_MAX_DIM` | 512 | Max icon width or height |
| `GR_GROUP_ICON_MAX_BYTES` | 10 MB | Max icon data size |
| `GR_FORK_MAX_BRANCHES` | 8 | Max branches in a fork descriptor |

---

## Enumerations

### `gr_group_type_t`

```c
typedef enum {
    GR_GROUP_PRIVATE = 0,
    GR_GROUP_PUBLIC  = 1,
} gr_group_type_t;
```

### `gr_permission_t`

Bitmask flags. Combine with bitwise OR. Owner has all permissions.

```c
typedef enum {
    GR_PERM_NONE              = 0,
    GR_PERM_KICK_MEMBER       = (1 << 0),
    GR_PERM_BAN_MEMBER        = (1 << 1),
    GR_PERM_INVITE_MEMBER     = (1 << 2),
    GR_PERM_ADD_WEBAPP        = (1 << 3),
    GR_PERM_REMOVE_WEBAPP     = (1 << 4),
    GR_PERM_EDIT_ROLES        = (1 << 5),
    GR_PERM_EDIT_RETENTION    = (1 << 6),
    GR_PERM_EDIT_SERVERS      = (1 << 7),
    GR_PERM_ROTATE_EPOCH      = (1 << 8),
    GR_PERM_SIGN_REGISTRAR    = (1 << 9),
    GR_PERM_SET_GROUP_ICON    = (1 << 10),
    GR_PERM_OWNER             = 0xFFFFFFFF,  /* all permissions */
} gr_permission_t;
```

### `gr_peer_status_t`

```c
typedef enum {
    GR_PEER_ACTIVE     = 0,
    GR_PEER_KICKED     = 1,
    GR_PEER_BANNED     = 2,
    GR_PEER_LEFT       = 3,
    GR_PEER_STATUS_ANY = -1,  /* wildcard for list/count filters */
} gr_peer_status_t;
```

### `gr_change_type_t`

Used in audit log entries. 22 mutation types:

```c
typedef enum {
    GR_CHANGE_PEER_ADDED         = 1,
    GR_CHANGE_PEER_REMOVED       = 2,
    GR_CHANGE_PEER_KICKED        = 3,
    GR_CHANGE_PEER_BANNED        = 4,
    GR_CHANGE_PEER_ADDRESS       = 5,
    GR_CHANGE_PEER_ROLE_CHANGED  = 6,
    GR_CHANGE_ROLE_ADDED         = 7,
    GR_CHANGE_ROLE_REMOVED       = 8,
    GR_CHANGE_ROLE_MODIFIED      = 9,
    GR_CHANGE_WEBAPP_ADDED       = 10,
    GR_CHANGE_WEBAPP_REMOVED     = 11,
    GR_CHANGE_SERVER_ADDED       = 12,
    GR_CHANGE_SERVER_REMOVED     = 13,
    GR_CHANGE_EPOCH_ROTATED      = 14,
    GR_CHANGE_RETENTION_SET      = 15,
    GR_CHANGE_REGISTRAR_SIGNED   = 16,
    GR_CHANGE_INVITE_CREATED     = 17,
    GR_CHANGE_INVITE_USED        = 18,
    GR_CHANGE_INVITE_INVALIDATED = 19,
    GR_CHANGE_GROUP_ICON_SET     = 20,
    GR_CHANGE_GROUP_ICON_REMOVED = 21,
    GR_CHANGE_BOOT_IP_BLOCKED    = 22,
} gr_change_type_t;
```

### `gr_server_type_t`

```c
typedef enum {
    GR_SERVER_SIGNALING   = 0,
    GR_SERVER_REBROADCAST = 1,
    GR_SERVER_TYPE_COUNT  = 2,
} gr_server_type_t;
```

### `gr_serialize_mode_t`

```c
typedef enum {
    GR_SERIALIZE_FULL  = 0,  /* all data */
    GR_SERIALIZE_OWNER = 1,  /* owner-only keys */
} gr_serialize_mode_t;
```

### `gr_join_state_t`

```c
typedef enum {
    GR_JOIN_NONE        = 0,  /* not in join verification */
    GR_JOIN_PROVISIONAL = 1,  /* verification in progress */
    GR_JOIN_VERIFIED    = 2,  /* verification passed */
    GR_JOIN_FAILED      = 3,  /* verification failed */
} gr_join_state_t;
```

### `gr_delta_action_t`

Returned from the delta anomaly callback to control behavior:

```c
typedef enum {
    GR_DELTA_CONTINUE          = 0,  /* accept the delta */
    GR_DELTA_SUSPEND           = 1,  /* reject the delta */
    GR_DELTA_CONTINUE_EXTENDED = 2,  /* accept with extended logging */
} gr_delta_action_t;
```

### `gr_behavior_alert_t`

Bitmask flags for behavioral alert callbacks:

```c
typedef enum {
    GR_ALERT_NONE  = 0,
    GR_ALERT_BURST = (1 << 0),  /* actor exceeds actions/min threshold */
    GR_ALERT_ABUSE = (1 << 1),  /* actor destructive ratio too high */
} gr_behavior_alert_t;
```

---

## Data Structures

### `gr_identity_t`

```c
typedef struct {
    uint8_t  public_key[GR_PUBLIC_KEY_LEN];   /* ML-DSA-87 signing public key  */
    uint8_t  secret_key[GR_SECRET_KEY_LEN];   /* ML-DSA-87 signing secret key  */
    uint8_t  kem_pk[GR_KEM_PUBLIC_KEY_LEN];   /* ML-KEM-1024 public key        */
    uint8_t  kem_sk[GR_KEM_SECRET_KEY_LEN];   /* ML-KEM-1024 secret key        */
    uint8_t  peer_id[GR_PEER_ID_LEN];         /* Skein-1024(public_key)[0..31] */
} gr_identity_t;
```

### `gr_peer_t`

```c
typedef struct {
    uint8_t          peer_id[GR_PEER_ID_LEN];
    uint8_t          kem_pk[GR_KEM_PUBLIC_KEY_LEN];   /* ML-KEM-1024 pk for encryption  */
    uint8_t          sign_key[GR_PUBLIC_KEY_LEN];      /* ML-DSA-87 pk for verification  */
    char             ip[GR_MAX_IP_LEN];
    uint16_t         port;
    gr_peer_status_t status;
    uint32_t         role_id;
    int64_t          joined_at;
    int64_t          removed_at;
    int64_t          last_seen;
    char             removed_reason[GR_MAX_NAME_LEN];
    uint8_t          removed_by[GR_PEER_ID_LEN];
} gr_peer_t;
```

### `gr_role_t`

```c
typedef struct {
    uint32_t role_id;
    char     name[GR_MAX_NAME_LEN];
    uint32_t permissions;                      /* bitmask of gr_permission_t */
    uint8_t  sign_key[GR_PUBLIC_KEY_LEN];
    int64_t  created_at;
    int64_t  modified_at;
} gr_role_t;
```

### `gr_webapp_t`

```c
typedef struct {
    uint8_t  hash[GR_SERVICE_HASH_LEN];
    char     name[GR_MAX_NAME_LEN];
    uint32_t version;
    int64_t  added_at;
    uint8_t  added_by[GR_PEER_ID_LEN];
} gr_webapp_t;
```

### `gr_server_t`

```c
typedef struct {
    gr_server_type_t type;
    char     ip[GR_MAX_IP_LEN];
    uint16_t port;
    uint8_t  id_hash[GR_HASH_LEN];
    uint8_t  sign_key[GR_PUBLIC_KEY_LEN];             /* ML-DSA pk for authentication   */
    uint8_t  service_hash[GR_SERVICE_HASH_LEN];
    uint8_t  content_kem_pk[GR_KEM_PUBLIC_KEY_LEN];   /* ML-KEM pk for content encrypt  */
    uint8_t  content_kem_sk[GR_KEM_SECRET_KEY_LEN];   /* ML-KEM sk for content decrypt  */
} gr_server_t;
```

### `gr_epoch_t`

```c
typedef struct {
    uint32_t epoch_id;
    uint8_t  epoch_key[GR_EPOCH_KEY_LEN];     /* 128-byte Threefish-1024 key */
    int64_t  created_at;
    int64_t  expired_at;
    uint8_t  created_by[GR_PEER_ID_LEN];
} gr_epoch_t;
```

### `gr_retention_t`

```c
typedef struct {
    int64_t  message_retention_ms;
    int64_t  file_retention_ms;
    int64_t  registrar_max_bytes;
} gr_retention_t;
```

Default values obtained via `gr_retention_defaults()` (inline helper).

### `gr_header_t`

```c
typedef struct {
    uint8_t          group_id[GR_HASH_LEN];
    gr_group_type_t  group_type;
    char             group_name[GR_MAX_NAME_LEN];
    uint32_t         version;
    int64_t          created_at;
    int64_t          updated_at;
    uint32_t         epoch_id;
    gr_retention_t   retention;
    uint8_t          owner_id[GR_PEER_ID_LEN];
    uint8_t          owner_sign_key[GR_PUBLIC_KEY_LEN];
    uint8_t          signer_id[GR_PEER_ID_LEN];
    uint8_t          signer_sign_key[GR_PUBLIC_KEY_LEN];
    uint8_t          signature[GR_SIGN_LEN];
    uint8_t          hash[GR_HASH_LEN];
} gr_header_t;
```

### `gr_audit_entry_t`

```c
typedef struct {
    uint8_t          entry_hash[GR_HASH_LEN];
    int64_t          timestamp;
    int64_t          timestamp_ns;        /* nanosecond precision for ordering */
    gr_change_type_t change_type;
    uint8_t          actor_id[GR_PEER_ID_LEN];
    uint8_t          target_id[GR_PEER_ID_LEN];
    uint8_t          signature[GR_SIGN_LEN];
    uint32_t         registrar_version;
    char             detail[256];
    uint8_t          prev_hash[GR_HASH_LEN];
} gr_audit_entry_t;
```

### `gr_invite_info_t`

```c
typedef struct {
    uint8_t  verification_token[GR_HASH_LEN];
    int64_t  created_at;
    int64_t  expires_at;
    uint8_t  created_by[GR_PEER_ID_LEN];
    bool     invalidated;
    bool     used;
    uint8_t  used_by[GR_PEER_ID_LEN];
} gr_invite_info_t;
```

### `gr_bootstrap_peer_t`

```c
typedef struct {
    uint8_t  peer_id[GR_PEER_ID_LEN];
    char     ip[GR_MAX_IP_LEN];
    uint16_t port;
    uint8_t  sign_key[GR_PUBLIC_KEY_LEN];  /* ML-DSA pk for authentication */
} gr_bootstrap_peer_t;
```

### `gr_invite_ticket_t`

```c
typedef struct {
    uint8_t             group_id[GR_HASH_LEN];
    char                group_name[GR_MAX_NAME_LEN];
    gr_group_type_t     group_type;
    uint8_t             owner_sign_key[GR_PUBLIC_KEY_LEN];
    uint8_t             registrar_hash[GR_HASH_LEN];
    uint8_t             verification_token[GR_HASH_LEN];
    uint8_t             inviter_sign_pk[GR_PUBLIC_KEY_LEN];      /* ML-DSA signing pk  */
    uint8_t             inviter_kem_pk[GR_KEM_PUBLIC_KEY_LEN];   /* ML-KEM-1024 pk     */
    int64_t             expires_at;
    gr_bootstrap_peer_t bootstrap_peers[GR_INVITE_MAX_BOOTSTRAP];
    uint32_t            bootstrap_count;
    uint32_t            signaling_count;
} gr_invite_ticket_t;
```

### `gr_merge_result_t`

```c
typedef struct {
    uint32_t entries_received;
    uint32_t entries_new;
    uint32_t entries_duplicate;
    uint32_t entries_rejected;
    uint32_t conflicts_resolved;
    uint32_t forks_detected;
    uint32_t new_version;
} gr_merge_result_t;
```

### `gr_group_icon_t`

```c
typedef struct {
    uint8_t  *data;
    size_t    data_len;
    char      mime_type[64];
    uint16_t  width;
    uint16_t  height;
    bool      is_video;
    uint8_t  *static_frame;          /* optional static JPEG/PNG for video icons */
    size_t    static_frame_len;
    uint8_t   content_hash[GR_HASH_LEN];
    int64_t   updated_at;
    uint8_t   updated_by[GR_PEER_ID_LEN];
} gr_group_icon_t;
```

### `gr_join_peer_vote_t`

```c
typedef struct {
    uint8_t  peer_id[GR_PEER_ID_LEN];
    uint8_t  owner_id[GR_PEER_ID_LEN];
    uint8_t  owner_sign_key[GR_PUBLIC_KEY_LEN];
    uint8_t  group_id[GR_HASH_LEN];
    uint8_t  registrar_hash[GR_HASH_LEN];
    uint32_t version;
    int64_t  received_at;
    bool     agrees;
} gr_join_peer_vote_t;
```

### `gr_join_verify_result_t`

```c
typedef struct gr_join_verify_result gr_join_verify_result_t;

struct gr_join_verify_result {
    gr_join_state_t  state;
    int64_t          started_at;
    uint32_t         peers_checked;
    uint32_t         peers_agreed;
    uint32_t         peers_disagreed;
    uint32_t         required_attestations;
    uint8_t          provisional_owner_id[GR_PEER_ID_LEN];
    uint8_t          provisional_owner_key[GR_PUBLIC_KEY_LEN];
    uint8_t          dissent_peer_id[GR_PEER_ID_LEN];
    uint8_t          dissent_owner_id[GR_PEER_ID_LEN];
    uint8_t          dissent_owner_key[GR_PUBLIC_KEY_LEN];
    bool             small_group_bypass;
    bool             user_override;
};
```

### `gr_audit_chain_result_t`

```c
typedef struct {
    uint32_t total_entries;
    uint32_t verified_entries;
    uint32_t invalid_hash;
    uint32_t invalid_signature;
    uint32_t unknown_actor;
    uint32_t forks_detected;
    uint32_t forks_resolved;
    bool     has_genesis;
} gr_audit_chain_result_t;
```

### `gr_audit_fork_t`

```c
typedef struct {
    uint8_t          prev_hash[GR_HASH_LEN];
    uint32_t         branch_count;
    gr_audit_entry_t branches[GR_FORK_MAX_BRANCHES];
} gr_audit_fork_t;
```

### `gr_registrar_t`

Opaque type. Created by `gr_create()` or `gr_open()`, destroyed by `gr_close()`.

```c
typedef struct gr_registrar gr_registrar_t;
```

---

## Callback Typedefs

### `gr_join_dissent_fn`

Called if any attesting peer disagrees about the group owner during join verification. Return `true` to accept the override.

```c
typedef bool (*gr_join_dissent_fn)(
    const gr_join_verify_result_t *result,
    void *user_data
);
```

### `gr_delta_anomaly_fn`

Called when an incoming delta is suspiciously large or spans too long. Return an action to take.

```c
typedef gr_delta_action_t (*gr_delta_anomaly_fn)(
    size_t delta_bytes,
    uint32_t entry_count,
    int64_t time_since_last_update_ms,
    void *user_data
);
```

### `gr_persist_prompt_fn`

Called when estimated registrar size exceeds `GR_PERSIST_PROMPT_BYTES`. Return `true` to persist.

```c
typedef bool (*gr_persist_prompt_fn)(
    size_t estimated_size_bytes,
    const char *group_name,
    void *user_data
);
```

### `gr_behavior_alert_fn`

Fires on every mutation that triggers burst or abuse detection.

```c
typedef void (*gr_behavior_alert_fn)(
    uint32_t alerts,                           /* bitmask of gr_behavior_alert_t */
    const uint8_t actor_id[GR_PEER_ID_LEN],
    gr_change_type_t change_type,              /* the mutation that triggered it */
    const gr_actor_burst_t *burst,             /* actor burst stats */
    const gr_admin_score_t *admin,             /* actor admin abuse stats */
    void *user_data
);
```

---

## Identity API

### `gr_identity_generate`

Generate a new peer identity (ML-DSA-87 + ML-KEM-1024 keypairs). Derives `peer_id = Skein-1024(public_key)[0..31]`.

```c
gr_error_t gr_identity_generate(gr_identity_t *out);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `out` | out | Identity struct to populate |

**Returns**: `GR_OK` on success.

### `gr_identity_derive_id`

Derive a peer ID from an ML-DSA-87 public key. Computes `peer_id = Skein-1024(public_key)[0..31]`.

```c
gr_error_t gr_identity_derive_id(const uint8_t public_key[GR_PUBLIC_KEY_LEN],
                                  uint8_t peer_id_out[GR_PEER_ID_LEN]);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `public_key` | in | ML-DSA-87 public key (2592 bytes) |
| `peer_id_out` | out | Resulting peer ID (32 bytes) |

### `gr_identity_wipe`

Securely wipe all secret key material from an identity (zeroes `secret_key` and `kem_sk`).

```c
void gr_identity_wipe(gr_identity_t *identity);
```

---

## Lifecycle API

### `gr_create`

Create a new group and its registrar database. The owner becomes the first peer.

```c
gr_error_t gr_create(gr_registrar_t **out, const char *db_path,
                     const char *group_name, gr_group_type_t group_type,
                     const gr_identity_t *owner);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `out` | out | Receives the allocated registrar |
| `db_path` | in | Path for the new DuckDB file |
| `group_name` | in | Human-readable group name |
| `group_type` | in | `GR_GROUP_PRIVATE` or `GR_GROUP_PUBLIC` |
| `owner` | in | Owner's identity (becomes the first peer) |

### `gr_open`

Open an existing group registrar database.

```c
gr_error_t gr_open(gr_registrar_t **out, const char *db_path,
                   const uint8_t group_id[GR_HASH_LEN]);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `out` | out | Receives the opened registrar |
| `db_path` | in | Path to existing DuckDB file |
| `group_id` | in | Expected group ID (verified against DB) |

**Returns**: `GR_OK`, `GR_ERR_NOT_FOUND` if group_id doesn't match.

### `gr_close`

Close a registrar and free all associated resources. NULL-safe.

```c
void gr_close(gr_registrar_t *reg);
```

### `gr_get_header`

Get the current registrar header (metadata snapshot).

```c
gr_error_t gr_get_header(const gr_registrar_t *reg, gr_header_t *out);
```

---

## Peer Management

### `gr_peer_add`

Add a peer to the group.

```c
gr_error_t gr_peer_add(gr_registrar_t *reg, const gr_peer_t *peer,
                       const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `reg` | in/out | Registrar |
| `peer` | in | Peer data (peer_id, sign_key, kem_pk, ip, port) |
| `signer` | in | Identity of the admin performing the add |

**Returns**: `GR_OK`, `GR_ERR_ALREADY_EXISTS`, or `GR_ERR_UNAUTHORIZED`.

### `gr_peer_kick`

Kick a peer from the group (can rejoin if re-invited).

```c
gr_error_t gr_peer_kick(gr_registrar_t *reg,
                        const uint8_t peer_id[GR_PEER_ID_LEN],
                        const char *reason, const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `peer_id` | in | ID of the peer to kick |
| `reason` | in | Human-readable reason string |
| `signer` | in | Identity with `GR_PERM_KICK_MEMBER` |

### `gr_peer_ban`

Ban a peer from the group (permanent, cannot rejoin).

```c
gr_error_t gr_peer_ban(gr_registrar_t *reg,
                        const uint8_t peer_id[GR_PEER_ID_LEN],
                        const char *reason, const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `peer_id` | in | ID of the peer to ban |
| `reason` | in | Human-readable reason string |
| `signer` | in | Identity with `GR_PERM_BAN_MEMBER` |

### `gr_peer_leave`

Voluntarily leave the group.

```c
gr_error_t gr_peer_leave(gr_registrar_t *reg, const gr_identity_t *peer);
```

### `gr_peer_update_address`

Update own network address (self-reported).

```c
gr_error_t gr_peer_update_address(gr_registrar_t *reg, const char *ip,
                                  uint16_t port, const gr_identity_t *peer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `ip` | in | New IP address string |
| `port` | in | New port number |
| `peer` | in | Identity of the peer updating their address |

### `gr_peer_observed_address`

Record an observed address for a peer (NAT traversal).

```c
gr_error_t gr_peer_observed_address(gr_registrar_t *reg,
                                    const uint8_t peer_id[GR_PEER_ID_LEN],
                                    const char *ip, uint16_t port,
                                    const gr_identity_t *observer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `peer_id` | in | Peer whose address was observed |
| `ip` | in | Observed IP address |
| `port` | in | Observed port |
| `observer` | in | Identity of the observing peer (for audit trail) |

### `gr_peer_touch`

Update a peer's last-seen timestamp to now.

```c
gr_error_t gr_peer_touch(gr_registrar_t *reg,
                         const uint8_t peer_id[GR_PEER_ID_LEN]);
```

### `gr_peer_set_role`

Assign a role to a peer.

```c
gr_error_t gr_peer_set_role(gr_registrar_t *reg,
                            const uint8_t peer_id[GR_PEER_ID_LEN],
                            uint32_t role_id, const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `peer_id` | in | Peer to modify |
| `role_id` | in | Role to assign (0 to clear) |
| `signer` | in | Identity with `GR_PERM_EDIT_ROLES` |

**Returns**: `GR_OK`, `GR_ERR_NOT_FOUND`, or `GR_ERR_UNAUTHORIZED`.

### `gr_peer_get`

Retrieve a single peer's data by ID.

```c
gr_error_t gr_peer_get(const gr_registrar_t *reg,
                       const uint8_t peer_id[GR_PEER_ID_LEN], gr_peer_t *out);
```

**Returns**: `GR_OK` or `GR_ERR_NOT_FOUND`.

### `gr_peer_count`

Count peers matching a status filter.

```c
gr_error_t gr_peer_count(const gr_registrar_t *reg,
                         gr_peer_status_t status_filter, uint32_t *out);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `status_filter` | in | `GR_PEER_ACTIVE`, `GR_PEER_KICKED`, etc., or `GR_PEER_STATUS_ANY` |
| `out` | out | Receives the count |

### `gr_peer_list`

List peers matching a status filter.

```c
gr_error_t gr_peer_list(const gr_registrar_t *reg, gr_peer_t *out,
                        uint32_t max_count, uint32_t *actual_count,
                        gr_peer_status_t status_filter);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `out` | out | Array to fill |
| `max_count` | in | Capacity of `out` |
| `actual_count` | out | Receives the number written |
| `status_filter` | in | Status filter (`GR_PEER_STATUS_ANY` for all) |

### `gr_peer_is_authorized`

Check if a peer is authorized (active + has a valid role).

```c
bool gr_peer_is_authorized(const gr_registrar_t *reg,
                           const uint8_t peer_id[GR_PEER_ID_LEN]);
```

Returns `true` if the peer is active and authorized.

---

## Role Management

### `gr_role_add`

Create a new role.

```c
gr_error_t gr_role_add(gr_registrar_t *reg, const char *name,
                       uint32_t permissions, const gr_identity_t *signer,
                       uint32_t *role_id_out);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `name` | in | Role name |
| `permissions` | in | Permission bitmask (`gr_permission_t` values OR'd together) |
| `signer` | in | Identity with `GR_PERM_EDIT_ROLES` |
| `role_id_out` | out | Receives the new role's ID |

**Returns**: `GR_OK`, `GR_ERR_ROLE_LIMIT`, or `GR_ERR_UNAUTHORIZED`.

### `gr_role_remove`

Remove a role by ID.

```c
gr_error_t gr_role_remove(gr_registrar_t *reg, uint32_t role_id,
                          const gr_identity_t *signer);
```

**Returns**: `GR_OK` or `GR_ERR_NOT_FOUND`.

### `gr_role_set_permissions`

Update a role's permission bitmask.

```c
gr_error_t gr_role_set_permissions(gr_registrar_t *reg, uint32_t role_id,
                                   uint32_t new_permissions,
                                   const gr_identity_t *signer);
```

**Returns**: `GR_OK` or `GR_ERR_NOT_FOUND`.

### `gr_role_get`

Get a role by ID.

```c
gr_error_t gr_role_get(const gr_registrar_t *reg, uint32_t role_id,
                       gr_role_t *out);
```

**Returns**: `GR_OK` or `GR_ERR_NOT_FOUND`.

### `gr_role_list`

List all roles.

```c
gr_error_t gr_role_list(const gr_registrar_t *reg, gr_role_t *out,
                        uint32_t max_count, uint32_t *actual_count);
```

### `gr_role_count`

Count total roles.

```c
gr_error_t gr_role_count(const gr_registrar_t *reg, uint32_t *out);
```

### `gr_has_permission`

Check if an identity has a specific permission.

```c
bool gr_has_permission(const gr_registrar_t *reg,
                       const gr_identity_t *identity, gr_permission_t perm);
```

Returns `true` if the identity has the permission.

---

## Webapp Management

### `gr_webapp_add`

Register a webapp with the group.

```c
gr_error_t gr_webapp_add(gr_registrar_t *reg, const gr_webapp_t *webapp,
                         const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `webapp` | in | Webapp descriptor |
| `signer` | in | Identity with `GR_PERM_ADD_WEBAPP` |

### `gr_webapp_remove`

Remove a webapp by its content hash.

```c
gr_error_t gr_webapp_remove(gr_registrar_t *reg,
                            const uint8_t hash[GR_SERVICE_HASH_LEN],
                            const gr_identity_t *signer);
```

### `gr_webapp_is_authorized`

Check if a webapp hash is authorized for use in this group.

```c
bool gr_webapp_is_authorized(const gr_registrar_t *reg,
                             const uint8_t hash[GR_SERVICE_HASH_LEN]);
```

### `gr_webapp_list`

List all registered webapps.

```c
gr_error_t gr_webapp_list(const gr_registrar_t *reg, gr_webapp_t *out,
                          uint32_t max_count, uint32_t *actual_count);
```

### `gr_webapp_count`

Count total registered webapps.

```c
gr_error_t gr_webapp_count(const gr_registrar_t *reg, uint32_t *out);
```

---

## Server Management

### `gr_server_add`

Add a signaling or rebroadcast server.

```c
gr_error_t gr_server_add(gr_registrar_t *reg, const gr_server_t *server,
                         const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `server` | in | Server descriptor |
| `signer` | in | Identity with `GR_PERM_EDIT_SERVERS` |

### `gr_server_remove`

Remove a server by its ID hash.

```c
gr_error_t gr_server_remove(gr_registrar_t *reg,
                            const uint8_t id_hash[GR_HASH_LEN],
                            const gr_identity_t *signer);
```

### `gr_server_list`

List servers of a given type.

```c
gr_error_t gr_server_list(const gr_registrar_t *reg, gr_server_type_t type,
                          gr_server_t *out, uint32_t max_count,
                          uint32_t *actual_count);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `type` | in | `GR_SERVER_SIGNALING` or `GR_SERVER_REBROADCAST` |

### `gr_server_count`

Count servers of a given type.

```c
gr_error_t gr_server_count(const gr_registrar_t *reg, gr_server_type_t type,
                           uint32_t *out);
```

---

## Epoch Key Management

Epoch keys are Threefish-1024 symmetric keys (128 bytes). When a peer is kicked or banned, the epoch rotates so the removed peer can't decrypt future traffic. Previous epochs remain in the database so old ciphertexts can still be decrypted.

### `gr_epoch_rotate`

Rotate the epoch key (generates a new symmetric key for the group).

```c
gr_error_t gr_epoch_rotate(gr_registrar_t *reg, const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `signer` | in | Identity with `GR_PERM_ROTATE_EPOCH` |

### `gr_epoch_get_current`

Get the current (latest) epoch.

```c
gr_error_t gr_epoch_get_current(const gr_registrar_t *reg, gr_epoch_t *out);
```

### `gr_epoch_get`

Get a specific epoch by ID.

```c
gr_error_t gr_epoch_get(const gr_registrar_t *reg, uint32_t epoch_id,
                        gr_epoch_t *out);
```

**Returns**: `GR_OK` or `GR_ERR_NOT_FOUND`.

### `gr_epoch_list`

List all epochs (newest first).

```c
gr_error_t gr_epoch_list(const gr_registrar_t *reg, gr_epoch_t *out,
                         uint32_t max_count, uint32_t *actual_count);
```

### `gr_epoch_count`

Count total epochs.

```c
gr_error_t gr_epoch_count(const gr_registrar_t *reg, uint32_t *out);
```

---

## Retention Policy

### `gr_retention_set`

Set the group's retention policy.

```c
gr_error_t gr_retention_set(gr_registrar_t *reg, const gr_retention_t *policy,
                            const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `policy` | in | New retention settings |
| `signer` | in | Identity with `GR_PERM_EDIT_RETENTION` |

### `gr_retention_get`

Get the current retention policy.

```c
gr_error_t gr_retention_get(const gr_registrar_t *reg, gr_retention_t *out);
```

### `gr_retention_defaults`

Inline helper returning default retention values.

```c
static inline gr_retention_t gr_retention_defaults(void);
```

---

## Group Icon

### `gr_group_icon_set`

Set or replace the group icon.

```c
gr_error_t gr_group_icon_set(gr_registrar_t *reg,
                             const uint8_t *data, size_t data_len,
                             const char *mime_type,
                             uint16_t width, uint16_t height,
                             bool is_video,
                             const uint8_t *static_frame,
                             size_t static_frame_len,
                             const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `data` | in | Icon image data (PNG, JPEG, WebP, or video) |
| `data_len` | in | Length in bytes (max `GR_GROUP_ICON_MAX_BYTES`) |
| `mime_type` | in | MIME type string (e.g. `"image/png"`) |
| `width` / `height` | in | Dimensions (max `GR_GROUP_ICON_MAX_DIM`) |
| `is_video` | in | `true` if the icon is an animated video |
| `static_frame` | in | Optional static JPEG/PNG frame for video icons (may be NULL) |
| `static_frame_len` | in | Length of `static_frame` |
| `signer` | in | Identity with `GR_PERM_SET_GROUP_ICON` |

**Returns**: `GR_OK`, `GR_ERR_SIZE_EXCEEDED`, or `GR_ERR_UNAUTHORIZED`.

### `gr_group_icon_get`

Retrieve the group icon. Caller must free with `gr_group_icon_free()`.

```c
gr_error_t gr_group_icon_get(const gr_registrar_t *reg,
                             gr_group_icon_t *out);
```

**Returns**: `GR_OK` or `GR_ERR_NOT_FOUND` if no icon is set.

### `gr_group_icon_remove`

Remove the group icon.

```c
gr_error_t gr_group_icon_remove(gr_registrar_t *reg,
                                const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `signer` | in | Identity with `GR_PERM_SET_GROUP_ICON` |

### `gr_group_icon_hash`

Compute the content hash of the current group icon (Skein-1024).

```c
gr_error_t gr_group_icon_hash(const gr_registrar_t *reg,
                              uint8_t hash_out[GR_HASH_LEN]);
```

**Returns**: `GR_OK` or `GR_ERR_NOT_FOUND`.

### `gr_group_icon_free`

Free heap-allocated data inside a `gr_group_icon_t` (does not free the struct itself).

```c
void gr_group_icon_free(gr_group_icon_t *icon);
```

---

## Signing / Verification / Encryption

### `gr_sign`

Sign the registrar header (records the signer and signature).

```c
gr_error_t gr_sign(gr_registrar_t *reg, const gr_identity_t *signer);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `signer` | in | Identity with `GR_PERM_SIGN_REGISTRAR` |

### `gr_verify`

Verify the registrar header signature.

```c
gr_error_t gr_verify(const gr_registrar_t *reg, bool *valid_out);
```

### `gr_sign_data`

Sign arbitrary data with an identity's ML-DSA-87 key.

```c
gr_error_t gr_sign_data(const gr_identity_t *signer,
                        const uint8_t *data, size_t data_len,
                        uint8_t signature_out[GR_SIGN_LEN]);
```

### `gr_verify_data`

Verify a signature over arbitrary data.

```c
gr_error_t gr_verify_data(const uint8_t public_key[GR_PUBLIC_KEY_LEN],
                          const uint8_t *data, size_t data_len,
                          const uint8_t signature[GR_SIGN_LEN],
                          bool *valid_out);
```

### `gr_encrypt`

Encrypt data with the current epoch key. Uses Threefish-1024-CTR + Skein-1024-MAC AEAD.

```c
gr_error_t gr_encrypt(const gr_registrar_t *reg,
                      const uint8_t *plaintext, size_t plaintext_len,
                      const uint8_t *ad, size_t ad_len,
                      uint8_t *out, size_t *out_len);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `plaintext` | in | Data to encrypt |
| `ad` | in | Additional authenticated data (may be NULL) |
| `ad_len` | in | Length of AAD |
| `out` | out | Ciphertext output (needs `plaintext_len` + overhead) |
| `out_len` | out | Receives actual output length |

### `gr_decrypt`

Decrypt data with an epoch key (tries current then previous).

```c
gr_error_t gr_decrypt(const gr_registrar_t *reg,
                      const uint8_t *ciphertext, size_t ciphertext_len,
                      const uint8_t *ad, size_t ad_len,
                      uint8_t *out, size_t *out_len,
                      uint32_t *epoch_id_out);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `ciphertext` | in | Data to decrypt (includes nonce + epoch_id + tag) |
| `ad` | in | Additional authenticated data |
| `epoch_id_out` | out | Receives the epoch ID that decrypted the data (may be NULL) |

**Returns**: `GR_ERR_EPOCH_MISMATCH` if no epoch key works.

### `gr_encrypt_for_peer`

Encrypt data for a specific peer using ML-KEM-1024 (one-shot hybrid).

```c
gr_error_t gr_encrypt_for_peer(const uint8_t peer_kem_pk[GR_KEM_PUBLIC_KEY_LEN],
                               const uint8_t *plaintext, size_t plaintext_len,
                               uint8_t *out, size_t *out_len);
```

### `gr_decrypt_from_peer`

Decrypt data from a specific peer using own ML-KEM secret key.

```c
gr_error_t gr_decrypt_from_peer(const gr_identity_t *self,
                                const uint8_t *ciphertext, size_t ciphertext_len,
                                uint8_t *out, size_t *out_len);
```

---

## Invite Management

### `gr_invite_create`

Create an invite ticket. Produces a serialized blob containing group metadata, bootstrap peers, and a verification token.

```c
gr_error_t gr_invite_create(gr_registrar_t *reg,
                            const gr_identity_t *inviter,
                            int64_t expiry_timestamp_ms,
                            uint8_t **out_data, size_t *out_len,
                            uint8_t verification_token_out[GR_HASH_LEN]);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `inviter` | in | Identity with `GR_PERM_INVITE_MEMBER` |
| `expiry_timestamp_ms` | in | Expiry time (ms since epoch), 0 = no expiry |
| `out_data` | out | Heap-allocated invite blob (free with `gr_free()`) |
| `out_len` | out | Receives blob length |
| `verification_token_out` | out | Token for tracking/invalidation |

### `gr_invite_invalidate`

Invalidate a pending invite by its verification token.

```c
gr_error_t gr_invite_invalidate(gr_registrar_t *reg,
                                const uint8_t verification_token[GR_HASH_LEN],
                                const gr_identity_t *admin);
```

**Returns**: `GR_OK` or `GR_ERR_NOT_FOUND`.

### `gr_invite_check`

Check if an invite is still valid (not expired, not invalidated).

```c
gr_error_t gr_invite_check(const gr_registrar_t *reg,
                           const uint8_t verification_token[GR_HASH_LEN],
                           bool *valid_out);
```

### `gr_invite_mark_used`

Mark an invite as used by a specific peer.

```c
gr_error_t gr_invite_mark_used(gr_registrar_t *reg,
                               const uint8_t verification_token[GR_HASH_LEN],
                               const uint8_t peer_id[GR_PEER_ID_LEN],
                               const gr_identity_t *signer);
```

**Returns**: `GR_OK` or `GR_ERR_NOT_FOUND`.

### `gr_invite_list`

List all invites (active, used, and invalidated).

```c
gr_error_t gr_invite_list(const gr_registrar_t *reg,
                          gr_invite_info_t *out, uint32_t max_count,
                          uint32_t *actual_count);
```

### `gr_invite_count`

Count total invites.

```c
gr_error_t gr_invite_count(const gr_registrar_t *reg, uint32_t *out);
```

### `gr_invite_parse`

Parse an invite ticket from the wire format blob.

```c
gr_error_t gr_invite_parse(const uint8_t *invite_data, size_t invite_len,
                           gr_invite_ticket_t *ticket_out);
```

**Returns**: `GR_OK` or `GR_ERR_INVITE_INVALID`.

---

## Join Verification

Quorum-based attestation protocol. The joiner generates a nonce, existing peers sign attestations binding their view of group ownership to that nonce. Groups smaller than `GR_JOIN_SMALL_GROUP_THRESHOLD` (5) skip attestation.

### `gr_join_begin`

Begin join verification after parsing an invite ticket.

```c
gr_error_t gr_join_begin(gr_registrar_t *reg,
                         const gr_invite_ticket_t *ticket);
```

**Returns**: `GR_OK`, `GR_ERR_JOIN_FAILED` if the ticket doesn't match.

### `gr_join_set_dissent_callback`

Register a callback for peer dissent during join verification. Called if any attesting peer disagrees about the group owner.

```c
gr_error_t gr_join_set_dissent_callback(gr_registrar_t *reg,
                                        gr_join_dissent_fn callback,
                                        void *user_data);
```

### `gr_join_submit_peer_header`

Submit a peer's header attestation during join verification.

```c
gr_error_t gr_join_submit_peer_header(gr_registrar_t *reg,
                                      const gr_header_t *peer_header,
                                      const uint8_t peer_id[GR_PEER_ID_LEN],
                                      const uint8_t peer_pk[GR_PUBLIC_KEY_LEN],
                                      const uint8_t peer_signature[GR_SIGN_LEN]);
```

**Returns**: `GR_OK` (vote recorded), `GR_ERR_NOT_FOUND`, `GR_ERR_SIGNATURE_INVALID`, or `GR_ERR_JOIN_INVITER_EXCLUDED`.

### `gr_join_evaluate`

Evaluate the current join verification state.

```c
gr_error_t gr_join_evaluate(gr_registrar_t *reg,
                            gr_join_verify_result_t *result_out);
```

### `gr_join_get_state`

Get the current join state without re-evaluating.

```c
gr_error_t gr_join_get_state(const gr_registrar_t *reg,
                             gr_join_state_t *state_out);
```

### `gr_join_get_nonce`

Get the join nonce to share with connecting peers.

```c
gr_error_t gr_join_get_nonce(const gr_registrar_t *reg,
                             uint8_t nonce_out[GR_JOIN_NONCE_LEN]);
```

### `gr_join_export_header_attestation`

Export a header attestation for a joining peer (existing member side). Signs `(header_hash || joiner_nonce || peer_id || pk)` with ML-DSA-87.

```c
gr_error_t gr_join_export_header_attestation(
    const gr_registrar_t *reg, const gr_identity_t *self,
    const uint8_t joiner_nonce[GR_JOIN_NONCE_LEN],
    gr_header_t *header_out, uint8_t signature_out[GR_SIGN_LEN]);
```

### `gr_join_list_unattested_peers`

List peers that have not yet submitted attestations. `peer_ids_out` is a flat buffer of `GR_PEER_ID_LEN`-byte IDs.

```c
gr_error_t gr_join_list_unattested_peers(
    const gr_registrar_t *reg, uint8_t *peer_ids_out,
    uint32_t max_count, uint32_t *actual_count);
```

### `gr_is_trusted`

Check if the registrar is in a trusted (verified) state.

```c
bool gr_is_trusted(const gr_registrar_t *reg);
```

Returns `true` if join verification passed or was not required.

---

## Audit Log

Every mutation is hash-chained with ML-DSA-87 signature. Max 25 MB (`GR_AUDIT_MAX_BYTES`). 22 change types (`gr_change_type_t`).

### `gr_audit_list`

List audit log entries since a given timestamp.

```c
gr_error_t gr_audit_list(const gr_registrar_t *reg, int64_t since_timestamp,
                         gr_audit_entry_t *out, uint32_t max_count,
                         uint32_t *actual_count);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `since_timestamp` | in | Only return entries after this time (ms since epoch) |

### `gr_audit_append`

Append a signed, hash-chained audit entry.

```c
gr_error_t gr_audit_append(gr_registrar_t *reg, gr_change_type_t change_type,
                           const gr_identity_t *actor,
                           const uint8_t target_id[GR_PEER_ID_LEN],
                           const char *detail);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `change_type` | in | The type of mutation being recorded |
| `actor` | in | Identity of the actor (signs the entry) |
| `target_id` | in | Target peer ID (may be NULL) |
| `detail` | in | Human-readable detail string (may be NULL) |

### `gr_audit_count`

Count total audit log entries.

```c
gr_error_t gr_audit_count(const gr_registrar_t *reg, uint32_t *out);
```

### `gr_audit_verify_chain`

Verify the integrity of the audit hash chain.

```c
gr_error_t gr_audit_verify_chain(const gr_registrar_t *reg,
                                 gr_audit_chain_result_t *result);
```

### `gr_audit_enforce_retention`

Enforce retention policy by pruning old audit entries.

```c
gr_error_t gr_audit_enforce_retention(gr_registrar_t *reg);
```

### `gr_audit_list_forks`

List detected forks in the audit chain.

```c
gr_error_t gr_audit_list_forks(const gr_registrar_t *reg,
                              gr_audit_fork_t *out,
                              uint32_t max_forks,
                              uint32_t *actual_count);
```

---

## Serialization & Delta Sync

### `gr_serialize`

Serialize the entire registrar to a portable blob.

```c
gr_error_t gr_serialize(const gr_registrar_t *reg, gr_serialize_mode_t mode,
                        uint8_t **out_data, size_t *out_len);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `mode` | in | `GR_SERIALIZE_FULL` or `GR_SERIALIZE_OWNER` (owner-only keys) |
| `out_data` | out | Heap-allocated blob (free with `gr_free()`) |

### `gr_deserialize`

Deserialize a full registrar blob into an opened registrar.

```c
gr_error_t gr_deserialize(gr_registrar_t *reg,
                          const uint8_t *data, size_t data_len);
```

**Returns**: `GR_OK` or `GR_ERR_SERIALIZATION`.

### `gr_serialize_delta`

Serialize a delta (changes since a given version).

```c
gr_error_t gr_serialize_delta(const gr_registrar_t *reg,
                              uint32_t since_version,
                              uint8_t **out_data, size_t *out_len);
```

### `gr_apply_delta`

Apply a delta blob to bring the registrar up to date.

```c
gr_error_t gr_apply_delta(gr_registrar_t *reg, const uint8_t *data,
                          size_t data_len, gr_merge_result_t *result_out);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `result_out` | out | Receives merge statistics (may be NULL) |

**Returns**: `GR_OK` or `GR_ERR_MERGE_CONFLICT`.

---

## Callbacks

### `gr_set_delta_anomaly_callback`

Register a callback for delta sync anomaly detection. Called when an incoming delta is suspiciously large or spans too long.

```c
gr_error_t gr_set_delta_anomaly_callback(gr_registrar_t *reg,
                                         gr_delta_anomaly_fn callback,
                                         void *user_data);
```

### `gr_set_persist_prompt_callback`

Register a callback for persist-to-disk prompts. Called when estimated registrar size exceeds `GR_PERSIST_PROMPT_BYTES`.

```c
gr_error_t gr_set_persist_prompt_callback(gr_registrar_t *reg,
                                          gr_persist_prompt_fn callback,
                                          void *user_data);
```

### `gr_set_behavior_alert_callback`

Register a per-mutation behavioral alert callback. Fires on every mutation that triggers burst or abuse detection.

```c
gr_error_t gr_set_behavior_alert_callback(gr_registrar_t *reg,
                                          gr_behavior_alert_fn callback,
                                          void *user_data);
```

---

## Utility Functions

### `gr_hash`

Compute a Skein-1024 hash of arbitrary data.

```c
gr_error_t gr_hash(const uint8_t *data, size_t data_len,
                   uint8_t out[GR_HASH_LEN]);
```

### `gr_id_equal`

Compare two peer IDs for equality (constant-time).

```c
bool gr_id_equal(const uint8_t a[GR_PEER_ID_LEN],
                 const uint8_t b[GR_PEER_ID_LEN]);
```

### `gr_timestamp_ms`

Get the current time in milliseconds since Unix epoch.

```c
int64_t gr_timestamp_ms(void);
```

### `gr_timestamp_ns`

Get the current time in nanoseconds since Unix epoch.

```c
int64_t gr_timestamp_ns(void);
```

### `gr_schema_version`

Get the current database schema version.

```c
uint64_t gr_schema_version(void);
```

### `gr_free`

Free a heap-allocated buffer returned by `gr_serialize`, `gr_invite_create`, etc. NULL-safe.

```c
void gr_free(void *ptr);
```

### `gr_error_str`

Get a human-readable string for an error code.

```c
const char *gr_error_str(gr_error_t err);
```

---

## Behavioral Analysis

Pure-logic, read-only analysis of audit logs, peer history, admin/mod actions, and sync patterns. Every function is passive — reads existing DB state and returns a result struct. No mutations, no callbacks, no side-effects.

### Result Structures

#### `gr_actor_burst_t`

```c
struct gr_actor_burst {
    uint32_t actions_in_window;
    uint32_t destructive_actions;    /* kicks + bans + removes */
    uint32_t role_changes;           /* role add/remove/modify */
    uint32_t invites_created;
    uint32_t epoch_rotations;
    float    actions_per_minute;
    bool     burst_detected;         /* exceeds threshold */
};
```

#### `gr_mutation_rate_t`

```c
typedef struct {
    uint32_t total_mutations;
    uint32_t distinct_actors;
    float    mutations_per_minute;
    float    mutations_per_actor;
    bool     swarm_detected;
} gr_mutation_rate_t;
```

#### `gr_admin_score_t`

```c
struct gr_admin_score {
    uint32_t total_admin_actions;
    uint32_t kicks;
    uint32_t bans;
    uint32_t removes;
    uint32_t role_modifications;
    uint32_t permission_escalations; /* GR_CHANGE_PEER_ROLE_CHANGED */
    float    destructive_ratio;      /* destructive / total */
    bool     abuse_suspected;
};
```

#### `gr_peer_churn_t`

```c
typedef struct {
    uint32_t active_peers;
    uint32_t kicked_peers;
    uint32_t banned_peers;
    uint32_t left_peers;
    uint32_t joined_in_window;
    uint32_t removed_in_window;
    float    churn_rate;             /* (joined+removed) / active */
    uint32_t stale_count;            /* active but not seen recently */
} gr_peer_churn_t;
```

#### `gr_delta_score_t`

```c
typedef struct {
    size_t   delta_bytes;
    uint32_t entry_count;
    int64_t  offline_duration_ms;
    float    bytes_per_offline_day;
    float    entries_per_offline_day;
    bool     anomalous;
} gr_delta_score_t;
```

#### `gr_epoch_pattern_t`

```c
typedef struct {
    uint32_t rotations_in_window;
    int64_t  avg_epoch_lifetime_ms;
    bool     excessive_rotation;     /* > 1 per hour in window */
} gr_epoch_pattern_t;
```

#### `gr_network_score_t`

```c
typedef struct {
    int64_t  last_update_ms;         /* header.updated_at */
    int64_t  time_since_update_ms;
    size_t   estimated_registrar_bytes;
    uint32_t total_audit_entries;
    float    avg_entry_interval_ms;  /* mean time between entries */
} gr_network_score_t;
```

#### `gr_behavior_snapshot_t`

```c
typedef struct {
    gr_peer_churn_t     churn;
    gr_mutation_rate_t  mutation_rate;
    gr_epoch_pattern_t  epoch_pattern;
    gr_network_score_t  network;
    gr_admin_score_t    worst_admin;
    uint8_t             worst_admin_id[GR_PEER_ID_LEN];
    bool                has_worst_admin;
    size_t              estimated_size;
    bool                needs_attention;  /* any flag raised */
} gr_behavior_snapshot_t;
```

### Configuration

```c
typedef struct {
    float    burst_actions_per_min;         /* default 20 */
    float    swarm_mutations_per_min;       /* default 60 */
    float    abuse_destructive_ratio;       /* default 0.6 */
    uint32_t abuse_min_actions;             /* default 10 */
    float    epoch_max_per_hour;            /* default 1.0 */
    float    delta_anomaly_entries_per_day; /* default 100 */
    float    churn_attention_threshold;     /* default 2.0 */
    bool     scale_by_group_size;           /* default true */
    int64_t  alert_window_ms;              /* default 300000 (5 min) */
} gr_behavior_config_t;
```

Use `gr_behavior_config_defaults()` for the default configuration (inline helper).

### `gr_behavior_actor_burst`

Detect burst activity for a specific actor within a time window.

```c
gr_error_t gr_behavior_actor_burst(const gr_registrar_t *reg,
                                   const uint8_t actor_id[GR_PEER_ID_LEN],
                                   int64_t window_ms,
                                   gr_actor_burst_t *out);
```

### `gr_behavior_mutation_rate`

Measure the group-wide mutation rate within a time window.

```c
gr_error_t gr_behavior_mutation_rate(const gr_registrar_t *reg,
                                     int64_t window_ms,
                                     gr_mutation_rate_t *out);
```

### `gr_behavior_admin_score`

Score an admin/moderator for potential abuse.

```c
gr_error_t gr_behavior_admin_score(const gr_registrar_t *reg,
                                   const uint8_t admin_id[GR_PEER_ID_LEN],
                                   int64_t window_ms,
                                   gr_admin_score_t *out);
```

### `gr_behavior_peer_churn`

Analyze peer churn (join/leave/kick/ban rates).

```c
gr_error_t gr_behavior_peer_churn(const gr_registrar_t *reg,
                                  int64_t window_ms,
                                  gr_peer_churn_t *out);
```

### `gr_behavior_delta_score`

Score an incoming delta for anomalies (size, entry count, time gap).

```c
gr_error_t gr_behavior_delta_score(const gr_registrar_t *reg,
                                   size_t delta_bytes,
                                   uint32_t entry_count,
                                   gr_delta_score_t *out);
```

### `gr_behavior_stale_peers`

List peers that haven't been seen recently (stale).

```c
gr_error_t gr_behavior_stale_peers(const gr_registrar_t *reg,
                                   int64_t stale_threshold_ms,
                                   uint8_t *peer_ids_out,
                                   uint32_t max_count,
                                   uint32_t *actual_count);
```

### `gr_behavior_epoch_pattern`

Analyze epoch rotation patterns (too frequent = suspicious).

```c
gr_error_t gr_behavior_epoch_pattern(const gr_registrar_t *reg,
                                     int64_t window_ms,
                                     gr_epoch_pattern_t *out);
```

### `gr_behavior_network_score`

Compute overall network/sync health metrics.

```c
gr_error_t gr_behavior_network_score(const gr_registrar_t *reg,
                                     gr_network_score_t *out);
```

### `gr_behavior_snapshot`

Take a full behavioral health snapshot of the group. Combines churn, mutation rate, epoch pattern, network score, and worst-admin analysis.

```c
gr_error_t gr_behavior_snapshot(const gr_registrar_t *reg,
                                int64_t window_ms,
                                gr_behavior_snapshot_t *out);
```

### `gr_behavior_set_config`

Set the behavioral analysis configuration thresholds.

```c
gr_error_t gr_behavior_set_config(gr_registrar_t *reg,
                                  const gr_behavior_config_t *config);
```

### `gr_behavior_get_config`

Get the current behavioral analysis configuration.

```c
gr_error_t gr_behavior_get_config(const gr_registrar_t *reg,
                                  gr_behavior_config_t *out);
```

---

## IP Blocklist

### `gr_blocklist_check`

Check if an IP address is currently blocked. Expired blocks are automatically cleaned up.

```c
gr_error_t gr_blocklist_check(gr_registrar_t *reg, const char *ip,
                              int64_t block_duration_ms, bool *blocked_out);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `ip` | in | IP address string (IPv4 or IPv6, max 45 chars) |
| `block_duration_ms` | in | Duration of a block in milliseconds |
| `blocked_out` | out | Receives `true` if currently blocked |

**Returns**: `GR_OK`, `GR_ERR_DB`.

### `gr_blocklist_record_fail`

Record a failed authentication attempt from an IP. Blocks if it reaches `max_fails`.

```c
gr_error_t gr_blocklist_record_fail(gr_registrar_t *reg, const char *ip,
                                    int max_fails, bool *just_blocked_out);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `max_fails` | in | Number of failures before blocking |
| `just_blocked_out` | out | Receives `true` if this call caused the block |

### `gr_blocklist_reset`

Reset the fail counter for an IP (e.g. after successful auth).

```c
gr_error_t gr_blocklist_reset(gr_registrar_t *reg, const char *ip);
```

### `gr_blocklist_cleanup`

Delete all expired block entries from the database.

```c
gr_error_t gr_blocklist_cleanup(gr_registrar_t *reg,
                                int64_t block_duration_ms);
```

---

## Implementation Files — `src/group_registrar/`

| File | Responsibility |
|------|---------------|
| `identity.c` | Key generation, peer ID derivation |
| `lifecycle.c` | Create, open, close, destroy |
| `peer.c` | Add, kick, ban, list peers |
| `role.c` | Role CRUD, permission management |
| `epoch.c` | Epoch key rotation |
| `invite.c` | Invite creation/parsing |
| `join.c` | Join verification, attestation protocol |
| `audit.c` | Audit log queries, hash-chain verification |
| `serialize.c` | Full and delta serialization/deserialization |
| `db.c` | Schema creation, prepared statement cache |
| `server.c` | Signaling/rebroadcast server management |
| `icon.c` | Group icon storage |
| `retention.c` | Message/file retention policy |
| `blocklist.c` | IP blocklist for bootstrap |
| `behavior.c` | Behavioral analysis |
| `crypto.c` | Internal crypto helpers |
| `buf.c` / `buf.h` | Binary buffer utilities |
| `webapp.c` | Per-group webapp registration |
| `internal.h` | Shared private header |
