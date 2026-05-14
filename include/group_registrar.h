/**
 * @file group_registrar.h
 * @brief Group Registrar API for Yumi Browser.
 *
 * Manages peer groups with post-quantum cryptographic security:
 * identity, lifecycle, peers, roles, epochs, invitations, join
 * verification, audit logging, delta sync, and behavioral analysis.
 *
 * ════════════════════════════════════════════════════════════════
 *  QUICK START: How to use this API
 * ════════════════════════════════════════════════════════════════
 *
 * ── 1. Creating a group and inviting peers ──────────────────────
 *
 *   yumi_crypto_init();
 *
 *   gr_identity_t owner;
 *   gr_identity_generate(&owner);
 *
 *   gr_registrar_t *reg;
 *   gr_create(&reg, "/path/to/group.db", "My Group",
 *             GR_GROUP_PRIVATE, &owner);
 *   gr_sign(reg, &owner);
 *
 *   // Create invite ticket (lightweight — no full registrar inside)
 *   uint8_t *invite_blob;
 *   size_t invite_len;
 *   uint8_t verify_token[GR_HASH_LEN];
 *   gr_invite_create(reg, &owner, 0, &invite_blob, &invite_len,
 *                    verify_token);
 *
 *   // Send invite_blob to the invitee via any channel.
 *   // It contains bootstrap peers + signaling servers so the
 *   // invitee can locate the group on the network.
 *
 * ── 2. Joining a group via invite ───────────────────────────────
 *
 *   // Parse the received invite blob
 *   gr_invite_ticket_t ticket;
 *   gr_invite_parse(invite_blob, invite_len, &ticket);
 *
 *   // Use ticket.bootstrap_peers and signaling servers to fetch
 *   // the actual registrar from the network — NOT from the inviter.
 *   // The networking layer fetches the serialized blob, writes it
 *   // to a new DuckDB file, then opens it:
 *   gr_registrar_t *reg;
 *   gr_open(&reg, "/path/to/joined.db", ticket.group_id);
 *
 *   // Begin verification — runs in background
 *   gr_error_t err = gr_join_begin(reg, &ticket);
 *   if (err == GR_ERR_JOIN_FAILED) {
 *       // Ticket doesn't match fetched registrar, or inviter
 *       // is not authorized. Do NOT proceed.
 *   }
 *
 *   // Optional: register a callback for peer disagreement
 *   gr_join_set_dissent_callback(reg, my_handler, my_ctx);
 *
 *   // The user is now in the group. Chat works. All incoming
 *   // sync (gr_deserialize, gr_apply_delta) flows through
 *   // normally so the registrar stays current with live changes.
 *   // Only LOCAL governance mutations (gr_peer_kick, gr_role_add,
 *   // gr_sign, etc.) are blocked until verified — but a new
 *   // member has no permissions for those anyway.
 *
 *   // Get the join nonce to share with connecting peers:
 *   uint8_t nonce[GR_JOIN_NONCE_LEN];
 *   gr_join_get_nonce(reg, nonce);
 *
 * ── 3. Attesting a peer (both sides) ────────────────────────────
 *
 *   // EXISTING MEMBER (attester):
 *   // The joiner sends you their nonce during the handshake.
 *   // Sign an attestation binding your view of group ownership
 *   // to that nonce:
 *
 *   gr_header_t my_header;
 *   uint8_t my_sig[GR_SIGN_LEN];
 *   gr_join_export_header_attestation(
 *       my_reg, &my_identity,
 *       joiner_nonce,    // received from the joiner
 *       &my_header,      // your registrar header (output)
 *       my_sig           // Ed25519 signature (output)
 *   );
 *   // Send (my_header, my_identity.peer_id, my_identity.public_key,
 *   //        my_sig) to the joiner.
 *   // Signature is ML-DSA-87 (4627 bytes).
 *
 *   // JOINER (receiving attestation):
 *   gr_error_t err = gr_join_submit_peer_header(
 *       reg, &received_header,
 *       received_peer_id, received_peer_pk,
 *       received_sig
 *   );
 *   // GR_OK              = vote recorded
 *   // GR_ERR_NOT_FOUND   = peer not in registrar
 *   // GR_ERR_SIGNATURE_INVALID = bad sig or wrong nonce
 *   // GR_ERR_JOIN_INVITER_EXCLUDED = inviter can't self-attest
 *
 *   // Evaluate after each submission:
 *   gr_join_verify_result_t result;
 *   gr_join_evaluate(reg, &result);
 *   // VERIFIED = done. FAILED = callback fired. PROVISIONAL = wait.
 *
 *   // Query which peers haven't attested (for gossipsub):
 *   uint8_t pending[64][GR_PEER_ID_LEN];
 *   uint32_t pending_count;
 *   gr_join_list_unattested_peers(reg, (uint8_t *)pending,
 *                                  64, &pending_count);
 *
 * ════════════════════════════════════════════════════════════════
 *
 * All cryptographic operations use the Yumi crypto abstraction layer (crypto.h):
 *   - Peer identity:   ML-DSA-87 signing keypair (NIST Level 5)
 *   - Peer encryption: ML-KEM-1024 key encapsulation
 *   - Group epoch key: Threefish-1024 AEAD symmetric
 *   - Signatures:      ML-DSA-87 (FIPS 204)
 *   - Hashing:         Skein-1024
 *   - Peer ID:         Skein-1024(sign_pk) truncated to 32 bytes
 *   - First connection: Triple-hybrid (ML-KEM + FrodoKEM + BrainPool-512)
 *   - Peer sessions:   Dual-hybrid (ML-KEM + BrainPool-512)
 *
 * Storage: DuckDB. Prerequisites: yumi_crypto_init() before any call.
 */

#ifndef GROUP_REGISTRAR_H
#define GROUP_REGISTRAR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════
 *  Constants
 * ════════════════════════════════════════════════════════════════ */

#define GR_PEER_ID_LEN          32
#define GR_PUBLIC_KEY_LEN       2592   /* ML-DSA-87 signing public key       */
#define GR_SECRET_KEY_LEN       4896   /* ML-DSA-87 signing secret key       */
#define GR_SIGN_LEN             4627   /* ML-DSA-87 signature                */
#define GR_SYMMETRIC_KEY_LEN    128    /* Threefish-1024 symmetric key       */
#define GR_HASH_LEN             128    /* Skein-1024 hash output             */
#define GR_NONCE_LEN            16     /* AEAD nonce (Threefish-1024-CTR)    */
#define GR_MAC_LEN              128    /* Skein-1024 MAC tag                 */
#define GR_MAX_ROLES            50
#define GR_MAX_IP_LEN           46
#define GR_MAX_NAME_LEN         128
#define GR_SERVICE_HASH_LEN     128    /* Skein-1024 hash output             */
#define GR_EPOCH_KEY_LEN        128    /* Threefish-1024 epoch key           */

/* ML-KEM-1024 (key encapsulation) */
#define GR_KEM_PUBLIC_KEY_LEN   1568   /* ML-KEM-1024 public key             */
#define GR_KEM_SECRET_KEY_LEN   3168   /* ML-KEM-1024 secret key             */
#define GR_KEM_CIPHERTEXT_LEN   1568   /* ML-KEM-1024 ciphertext             */

/* Default retention */
#define GR_DEFAULT_MESSAGE_RETENTION_MS  (30LL  * 24 * 60 * 60 * 1000)
#define GR_DEFAULT_FILE_RETENTION_MS     (180LL * 24 * 60 * 60 * 1000)
#define GR_DEFAULT_REGISTRAR_MAX_BYTES   (200LL * 1024 * 1024)
#define GR_RETENTION_FOREVER             0LL

/* Join verification */
#define GR_JOIN_NONCE_LEN               32     /* per-session challenge token     */
#define GR_JOIN_MIN_QUORUM              3      /* floor for attestation quorum    */
#define GR_JOIN_MAX_QUORUM              10     /* cap for attestation quorum      */
#define GR_JOIN_MAX_TRACKED_PEERS       64     /* max peer votes to store         */
#define GR_JOIN_SMALL_GROUP_THRESHOLD   5      /* groups < this skip attestation  */
#define GR_JOIN_ATTESTATION_LEN         2788   /* GR_HASH_LEN+GR_PEER_ID_LEN+GR_PUBLIC_KEY_LEN+4+GR_JOIN_NONCE_LEN */
#define GR_INVITE_MAX_BOOTSTRAP         5      /* max bootstrap peers in invite   */

/* Audit retention */
#define GR_AUDIT_MAX_BYTES              (25LL * 1024 * 1024)
#define GR_AUDIT_EST_ROW_BYTES          500

/* Delta merge */
#define GR_DELTA_MAX_BYTES              (1 * 1024 * 1024)
#define GR_DELTA_ANOMALY_GAP_MS         (30LL * 24 * 60 * 60 * 1000) /* 30 days */

/* Persist-to-disk prompt threshold (50 MB estimated) */
#define GR_PERSIST_PROMPT_BYTES         (50LL * 1024 * 1024)

/* Group icon */
#define GR_GROUP_ICON_MAX_DIM           512
#define GR_GROUP_ICON_MAX_BYTES         (10 * 1024 * 1024)

/* Invite wire format */
#define GR_INVITE_VERSION               3

/* ════════════════════════════════════════════════════════════════
 *  Error codes
 * ════════════════════════════════════════════════════════════════ */

/**
 * @defgroup gr_errors Error Codes
 * @brief Return codes for all group registrar operations.
 * @{
 */
typedef enum {
    GR_OK                        =  0,  /**< Success. */
    GR_ERR_INVALID_PARAM         = -1,  /**< NULL pointer or invalid argument. */
    GR_ERR_NOT_FOUND             = -2,  /**< Requested item not found. */
    GR_ERR_ALREADY_EXISTS        = -3,  /**< Item already exists (duplicate). */
    GR_ERR_UNAUTHORIZED          = -4,  /**< Caller lacks required permission. */
    GR_ERR_SIGNATURE_INVALID     = -5,  /**< Digital signature verification failed. */
    GR_ERR_CRYPTO                = -6,  /**< Generic cryptographic operation failure. */
    GR_ERR_DB                    = -7,  /**< Database operation failed. */
    GR_ERR_SIZE_EXCEEDED         = -8,  /**< Size limit exceeded. */
    GR_ERR_ROLE_LIMIT            = -9,  /**< Maximum role count reached. */
    GR_ERR_EPOCH_MISMATCH        = -10, /**< Epoch ID mismatch during decrypt. */
    GR_ERR_SERIALIZATION         = -11, /**< Serialization / deserialization error. */
    GR_ERR_OUT_OF_MEMORY         = -12, /**< Memory allocation failed. */
    GR_ERR_INVITE_EXPIRED        = -13, /**< Invite ticket has expired. */
    GR_ERR_INVITE_INVALID        = -14, /**< Invite blob is malformed or invalid. */
    GR_ERR_MERGE_CONFLICT        = -15, /**< Delta merge conflict. */
    GR_ERR_JOIN_UNVERIFIED       = -16, /**< Operation blocked — join not yet verified. */
    GR_ERR_JOIN_FAILED           = -17, /**< Join verification failed (dissent or mismatch). */
    GR_ERR_JOIN_INVITER_EXCLUDED = -18, /**< Inviter cannot self-attest. */
} gr_error_t;
/** @} */

/* ════════════════════════════════════════════════════════════════
 *  Enums
 * ════════════════════════════════════════════════════════════════ */

typedef enum {
    GR_GROUP_PRIVATE = 0,
    GR_GROUP_PUBLIC  = 1,
} gr_group_type_t;

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
    GR_PERM_OWNER             = 0xFFFFFFFF,
} gr_permission_t;

typedef enum {
    GR_PEER_ACTIVE     = 0,
    GR_PEER_KICKED     = 1,
    GR_PEER_BANNED     = 2,
    GR_PEER_LEFT       = 3,
    GR_PEER_STATUS_ANY = -1,
} gr_peer_status_t;

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

typedef enum {
    GR_SERVER_SIGNALING   = 0,
    GR_SERVER_REBROADCAST = 1,
    GR_SERVER_TYPE_COUNT  = 2,
} gr_server_type_t;

typedef enum {
    GR_SERIALIZE_FULL  = 0,
    GR_SERIALIZE_OWNER = 1,
} gr_serialize_mode_t;

typedef enum {
    GR_JOIN_NONE        = 0,
    GR_JOIN_PROVISIONAL = 1,
    GR_JOIN_VERIFIED    = 2,
    GR_JOIN_FAILED      = 3,
} gr_join_state_t;

typedef enum {
    GR_DELTA_CONTINUE          = 0,
    GR_DELTA_SUSPEND           = 1,
    GR_DELTA_CONTINUE_EXTENDED = 2,
} gr_delta_action_t;

/* ════════════════════════════════════════════════════════════════
 *  Data structures
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  public_key[GR_PUBLIC_KEY_LEN];     /* ML-DSA-87 signing pk */
    uint8_t  secret_key[GR_SECRET_KEY_LEN];     /* ML-DSA-87 signing sk */
    uint8_t  kem_pk[GR_KEM_PUBLIC_KEY_LEN];     /* ML-KEM-1024 pk      */
    uint8_t  kem_sk[GR_KEM_SECRET_KEY_LEN];     /* ML-KEM-1024 sk      */
    uint8_t  peer_id[GR_PEER_ID_LEN];
} gr_identity_t;

typedef struct {
    uint8_t          peer_id[GR_PEER_ID_LEN];
    uint8_t          kem_pk[GR_KEM_PUBLIC_KEY_LEN];    /* ML-KEM-1024 pk for encryption  */
    uint8_t          sign_key[GR_PUBLIC_KEY_LEN];       /* ML-DSA-87 pk for verification  */
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

typedef struct {
    uint32_t role_id;
    char     name[GR_MAX_NAME_LEN];
    uint32_t permissions;
    uint8_t  sign_key[GR_PUBLIC_KEY_LEN];
    int64_t  created_at;
    int64_t  modified_at;
} gr_role_t;

typedef struct {
    uint8_t  hash[GR_SERVICE_HASH_LEN];
    char     name[GR_MAX_NAME_LEN];
    uint32_t version;
    int64_t  added_at;
    uint8_t  added_by[GR_PEER_ID_LEN];
} gr_webapp_t;

typedef struct {
    gr_server_type_t type;
    char     ip[GR_MAX_IP_LEN];
    uint16_t port;
    uint8_t  id_hash[GR_HASH_LEN];
    uint8_t  sign_key[GR_PUBLIC_KEY_LEN];              /* ML-DSA pk for authentication    */
    uint8_t  service_hash[GR_SERVICE_HASH_LEN];
    uint8_t  content_kem_pk[GR_KEM_PUBLIC_KEY_LEN];    /* ML-KEM pk for content encrypt   */
    uint8_t  content_kem_sk[GR_KEM_SECRET_KEY_LEN];    /* ML-KEM sk for content decrypt   */
} gr_server_t;

typedef struct {
    uint32_t epoch_id;
    uint8_t  epoch_key[GR_EPOCH_KEY_LEN];
    int64_t  created_at;
    int64_t  expired_at;
    uint8_t  created_by[GR_PEER_ID_LEN];
} gr_epoch_t;

typedef struct {
    int64_t  message_retention_ms;
    int64_t  file_retention_ms;
    int64_t  registrar_max_bytes;
} gr_retention_t;

static inline gr_retention_t gr_retention_defaults(void) {
    return (gr_retention_t){
        .message_retention_ms = GR_DEFAULT_MESSAGE_RETENTION_MS,
        .file_retention_ms    = GR_DEFAULT_FILE_RETENTION_MS,
        .registrar_max_bytes  = GR_DEFAULT_REGISTRAR_MAX_BYTES,
    };
}

typedef struct {
    uint8_t          entry_hash[GR_HASH_LEN];
    int64_t          timestamp;
    int64_t          timestamp_ns;       /* nanosecond precision for ordering */
    gr_change_type_t change_type;
    uint8_t          actor_id[GR_PEER_ID_LEN];
    uint8_t          target_id[GR_PEER_ID_LEN];
    uint8_t          signature[GR_SIGN_LEN];
    uint32_t         registrar_version;
    char             detail[256];
    uint8_t          prev_hash[GR_HASH_LEN];
} gr_audit_entry_t;

typedef struct {
    uint8_t  verification_token[GR_HASH_LEN];
    int64_t  created_at;
    int64_t  expires_at;
    uint8_t  created_by[GR_PEER_ID_LEN];
    bool     invalidated;
    bool     used;
    uint8_t  used_by[GR_PEER_ID_LEN];
} gr_invite_info_t;

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

typedef struct {
    uint32_t entries_received;
    uint32_t entries_new;
    uint32_t entries_duplicate;
    uint32_t entries_rejected;
    uint32_t conflicts_resolved;
    uint32_t forks_detected;
    uint32_t new_version;
} gr_merge_result_t;

/* ── Group icon ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  *data;
    size_t    data_len;
    char      mime_type[64];
    uint16_t  width;
    uint16_t  height;
    bool      is_video;
    uint8_t  *static_frame;
    size_t    static_frame_len;
    uint8_t   content_hash[GR_HASH_LEN];
    int64_t   updated_at;
    uint8_t   updated_by[GR_PEER_ID_LEN];
} gr_group_icon_t;

/* ── Join verification ───────────────────────────────────────── */

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

typedef struct gr_join_verify_result gr_join_verify_result_t;

typedef bool (*gr_join_dissent_fn)(
    const gr_join_verify_result_t *result,
    void *user_data
);

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

/* ── Bootstrap / Invite ──────────────────────────────────────── */

typedef struct {
    uint8_t  peer_id[GR_PEER_ID_LEN];
    char     ip[GR_MAX_IP_LEN];
    uint16_t port;
    uint8_t  sign_key[GR_PUBLIC_KEY_LEN];  /* ML-DSA pk for authentication */
} gr_bootstrap_peer_t;

typedef struct {
    uint8_t           group_id[GR_HASH_LEN];
    char              group_name[GR_MAX_NAME_LEN];
    gr_group_type_t   group_type;
    uint8_t           owner_sign_key[GR_PUBLIC_KEY_LEN];
    uint8_t           registrar_hash[GR_HASH_LEN];
    uint8_t           verification_token[GR_HASH_LEN];
    uint8_t           inviter_sign_pk[GR_PUBLIC_KEY_LEN];       /* ML-DSA signing pk   */
    uint8_t           inviter_kem_pk[GR_KEM_PUBLIC_KEY_LEN];    /* ML-KEM-1024 pk      */
    int64_t           expires_at;
    gr_bootstrap_peer_t bootstrap_peers[GR_INVITE_MAX_BOOTSTRAP];
    uint32_t            bootstrap_count;
    uint32_t            signaling_count;
} gr_invite_ticket_t;

/* ── Audit chain verification ────────────────────────────────── */

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

/* ── Audit fork introspection ────────────────────────────────── */

#define GR_FORK_MAX_BRANCHES 8

typedef struct {
    uint8_t          prev_hash[GR_HASH_LEN];
    uint32_t         branch_count;
    gr_audit_entry_t branches[GR_FORK_MAX_BRANCHES];
} gr_audit_fork_t;

/* ── Callbacks ───────────────────────────────────────────────── */

typedef gr_delta_action_t (*gr_delta_anomaly_fn)(
    size_t delta_bytes,
    uint32_t entry_count,
    int64_t time_since_last_update_ms,
    void *user_data
);

typedef bool (*gr_persist_prompt_fn)(
    size_t estimated_size_bytes,
    const char *group_name,
    void *user_data
);

/* ── Behavioral alert (fires per-mutation when thresholds exceeded) ── */

typedef enum {
    GR_ALERT_NONE  = 0,
    GR_ALERT_BURST = (1 << 0),  /* actor exceeds actions/min threshold   */
    GR_ALERT_ABUSE = (1 << 1),  /* actor destructive ratio too high      */
} gr_behavior_alert_t;

/* Forward declarations — full structs defined in Behavioral Analysis section */
typedef struct gr_actor_burst gr_actor_burst_t;
typedef struct gr_admin_score gr_admin_score_t;

typedef void (*gr_behavior_alert_fn)(
    uint32_t alerts,                          /* bitmask of gr_behavior_alert_t */
    const uint8_t actor_id[GR_PEER_ID_LEN],
    gr_change_type_t change_type,             /* the mutation that triggered it */
    const gr_actor_burst_t *burst,            /* actor burst stats              */
    const gr_admin_score_t *admin,            /* actor admin abuse stats        */
    void *user_data
);

typedef struct gr_registrar gr_registrar_t;

/* ════════════════════════════════════════════════════════════════
 *  Identity
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Generate a new peer identity (ML-DSA-87 + ML-KEM-1024 keypairs).
 *
 * @param[out] out  Identity struct to populate.
 * @return GR_OK on success.
 *
 * @code{.c}
 * gr_identity_t me;
 * gr_identity_generate(&me);
 * // me.peer_id is now Skein-1024(me.public_key) truncated to 32 bytes.
 * @endcode
 */
gr_error_t gr_identity_generate(gr_identity_t *out);

/**
 * @brief Derive a peer ID from an ML-DSA-87 public key.
 *
 * Computes `peer_id = Skein-1024(public_key)[0..31]`.
 *
 * @param[in]  public_key   ML-DSA-87 public key (GR_PUBLIC_KEY_LEN bytes).
 * @param[out] peer_id_out  Resulting peer ID (GR_PEER_ID_LEN bytes).
 * @return GR_OK on success.
 */
gr_error_t gr_identity_derive_id(const uint8_t public_key[GR_PUBLIC_KEY_LEN],
                                  uint8_t peer_id_out[GR_PEER_ID_LEN]);

/**
 * @brief Securely wipe all secret key material from an identity.
 * @param[in,out] identity  Identity to wipe (zeroes sk and kem_sk).
 */
void gr_identity_wipe(gr_identity_t *identity);

/* ════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a new group and its registrar database.
 *
 * @param[out] out         Receives the allocated registrar.
 * @param[in]  db_path     Path for the new DuckDB database file.
 * @param[in]  group_name  Human-readable group name.
 * @param[in]  group_type  GR_GROUP_PRIVATE or GR_GROUP_PUBLIC.
 * @param[in]  owner       Owner's identity (becomes the first peer).
 * @return GR_OK on success.
 *
 * @code{.c}
 * gr_registrar_t *reg;
 * gr_create(&reg, "/tmp/group.db", "My Group", GR_GROUP_PRIVATE, &owner);
 * @endcode
 */
gr_error_t gr_create(gr_registrar_t **out, const char *db_path,
                     const char *group_name, gr_group_type_t group_type,
                     const gr_identity_t *owner);

/**
 * @brief Open an existing group registrar database.
 *
 * @param[out] out       Receives the opened registrar.
 * @param[in]  db_path   Path to the existing DuckDB file.
 * @param[in]  group_id  Expected group ID (verified against the DB).
 * @return GR_OK on success, GR_ERR_NOT_FOUND if group_id doesn't match.
 */
gr_error_t gr_open(gr_registrar_t **out, const char *db_path,
                   const uint8_t group_id[GR_HASH_LEN]);

/**
 * @brief Close a registrar and free all associated resources.
 * @param[in,out] reg  Registrar to close (NULL-safe).
 */
void gr_close(gr_registrar_t *reg);

/**
 * @brief Get the current registrar header (metadata snapshot).
 *
 * @param[in]  reg  Registrar.
 * @param[out] out  Header struct to populate.
 * @return GR_OK on success.
 */
gr_error_t gr_get_header(const gr_registrar_t *reg, gr_header_t *out);

/* ════════════════════════════════════════════════════════════════
 *  Peer / Role / WebApp / Server / Epoch / Retention
 * ════════════════════════════════════════════════════════════════ */

/** @name Peer Management
 *  @{ */

/**
 * @brief Add a peer to the group.
 * @param[in,out] reg     Registrar.
 * @param[in]     peer    Peer data (peer_id, sign_key, kem_pk, ip, port).
 * @param[in]     signer  Identity of the admin performing the add.
 * @return GR_OK, GR_ERR_ALREADY_EXISTS, or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_peer_add(gr_registrar_t *reg, const gr_peer_t *peer,
                       const gr_identity_t *signer);

/**
 * @brief Kick a peer from the group (can rejoin if re-invited).
 * @param[in,out] reg      Registrar.
 * @param[in]     peer_id  ID of the peer to kick.
 * @param[in]     reason   Human-readable reason string.
 * @param[in]     signer   Identity with GR_PERM_KICK_MEMBER.
 * @return GR_OK or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_peer_kick(gr_registrar_t *reg,
                        const uint8_t peer_id[GR_PEER_ID_LEN],
                        const char *reason, const gr_identity_t *signer);

/**
 * @brief Ban a peer from the group (permanent, cannot rejoin).
 * @param[in,out] reg      Registrar.
 * @param[in]     peer_id  ID of the peer to ban.
 * @param[in]     reason   Human-readable reason string.
 * @param[in]     signer   Identity with GR_PERM_BAN_MEMBER.
 * @return GR_OK or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_peer_ban(gr_registrar_t *reg,
                        const uint8_t peer_id[GR_PEER_ID_LEN],
                        const char *reason, const gr_identity_t *signer);

/**
 * @brief Voluntarily leave the group.
 * @param[in,out] reg   Registrar.
 * @param[in]     peer  Identity of the leaving peer.
 * @return GR_OK on success.
 */
gr_error_t gr_peer_leave(gr_registrar_t *reg, const gr_identity_t *peer);

/**
 * @brief Update own network address (self-reported).
 * @param[in,out] reg   Registrar.
 * @param[in]     ip    New IP address string.
 * @param[in]     port  New port number.
 * @param[in]     peer  Identity of the peer updating their address.
 * @return GR_OK on success.
 */
gr_error_t gr_peer_update_address(gr_registrar_t *reg, const char *ip,
                                  uint16_t port, const gr_identity_t *peer);

/**
 * @brief Record an observed address for a peer (NAT traversal).
 * @param[in,out] reg       Registrar.
 * @param[in]     peer_id   Peer whose address was observed.
 * @param[in]     ip        Observed IP address.
 * @param[in]     port      Observed port.
 * @param[in]     observer  Identity of the observing peer (for audit trail).
 * @return GR_OK on success.
 */
gr_error_t gr_peer_observed_address(gr_registrar_t *reg,
                                    const uint8_t peer_id[GR_PEER_ID_LEN],
                                    const char *ip, uint16_t port,
                                    const gr_identity_t *observer);

/**
 * @brief Update a peer's last-seen timestamp to now.
 * @param[in,out] reg      Registrar.
 * @param[in]     peer_id  Peer to touch.
 * @return GR_OK on success.
 */
gr_error_t gr_peer_touch(gr_registrar_t *reg,
                         const uint8_t peer_id[GR_PEER_ID_LEN]);

/**
 * @brief Assign a role to a peer.
 * @param[in,out] reg      Registrar.
 * @param[in]     peer_id  Peer to modify.
 * @param[in]     role_id  Role to assign (0 to clear).
 * @param[in]     signer   Identity with GR_PERM_EDIT_ROLES.
 * @return GR_OK, GR_ERR_NOT_FOUND, or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_peer_set_role(gr_registrar_t *reg,
                            const uint8_t peer_id[GR_PEER_ID_LEN],
                            uint32_t role_id, const gr_identity_t *signer);

/**
 * @brief Retrieve a single peer's data by ID.
 * @param[in]  reg      Registrar.
 * @param[in]  peer_id  Peer ID to look up.
 * @param[out] out      Peer data output.
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_peer_get(const gr_registrar_t *reg,
                       const uint8_t peer_id[GR_PEER_ID_LEN], gr_peer_t *out);

/**
 * @brief Count peers matching a status filter.
 * @param[in]  reg            Registrar.
 * @param[in]  status_filter  GR_PEER_ACTIVE, GR_PEER_KICKED, etc. or GR_PEER_STATUS_ANY.
 * @param[out] out            Receives the count.
 * @return GR_OK on success.
 */
gr_error_t gr_peer_count(const gr_registrar_t *reg,
                         gr_peer_status_t status_filter, uint32_t *out);

/**
 * @brief List peers matching a status filter.
 * @param[in]  reg            Registrar.
 * @param[out] out            Array to fill.
 * @param[in]  max_count      Capacity of @p out.
 * @param[out] actual_count   Receives the number written.
 * @param[in]  status_filter  Status filter (GR_PEER_STATUS_ANY for all).
 * @return GR_OK on success.
 */
gr_error_t gr_peer_list(const gr_registrar_t *reg, gr_peer_t *out,
                        uint32_t max_count, uint32_t *actual_count,
                        gr_peer_status_t status_filter);

/**
 * @brief Check if a peer is authorized (active + has a valid role).
 * @param[in] reg      Registrar.
 * @param[in] peer_id  Peer ID to check.
 * @return true if the peer is active and authorized.
 */
bool gr_peer_is_authorized(const gr_registrar_t *reg,
                           const uint8_t peer_id[GR_PEER_ID_LEN]);
/** @} */

/** @name Role Management
 *  @{ */

/**
 * @brief Create a new role.
 * @param[in,out] reg          Registrar.
 * @param[in]     name         Role name.
 * @param[in]     permissions  Permission bitmask (gr_permission_t).
 * @param[in]     signer       Identity with GR_PERM_EDIT_ROLES.
 * @param[out]    role_id_out  Receives the new role's ID.
 * @return GR_OK, GR_ERR_ROLE_LIMIT, or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_role_add(gr_registrar_t *reg, const char *name,
                       uint32_t permissions, const gr_identity_t *signer,
                       uint32_t *role_id_out);

/**
 * @brief Remove a role by ID.
 * @param[in,out] reg      Registrar.
 * @param[in]     role_id  Role to remove.
 * @param[in]     signer   Identity with GR_PERM_EDIT_ROLES.
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_role_remove(gr_registrar_t *reg, uint32_t role_id,
                          const gr_identity_t *signer);

/**
 * @brief Update a role's permission bitmask.
 * @param[in,out] reg              Registrar.
 * @param[in]     role_id          Role to modify.
 * @param[in]     new_permissions  New permission bitmask.
 * @param[in]     signer           Identity with GR_PERM_EDIT_ROLES.
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_role_set_permissions(gr_registrar_t *reg, uint32_t role_id,
                                   uint32_t new_permissions,
                                   const gr_identity_t *signer);

/**
 * @brief Get a role by ID.
 * @param[in]  reg      Registrar.
 * @param[in]  role_id  Role to look up.
 * @param[out] out      Role data output.
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_role_get(const gr_registrar_t *reg, uint32_t role_id,
                       gr_role_t *out);

/**
 * @brief List all roles.
 * @param[in]  reg           Registrar.
 * @param[out] out           Array to fill.
 * @param[in]  max_count     Capacity of @p out.
 * @param[out] actual_count  Receives the number written.
 * @return GR_OK on success.
 */
gr_error_t gr_role_list(const gr_registrar_t *reg, gr_role_t *out,
                        uint32_t max_count, uint32_t *actual_count);

/**
 * @brief Count total roles.
 * @param[in]  reg  Registrar.
 * @param[out] out  Receives the count.
 * @return GR_OK on success.
 */
gr_error_t gr_role_count(const gr_registrar_t *reg, uint32_t *out);

/**
 * @brief Check if an identity has a specific permission.
 * @param[in] reg       Registrar.
 * @param[in] identity  Identity to check.
 * @param[in] perm      Permission flag to test.
 * @return true if the identity has the permission.
 */
bool gr_has_permission(const gr_registrar_t *reg,
                       const gr_identity_t *identity, gr_permission_t perm);
/** @} */

/** @name WebApp Management
 *  @{ */

/**
 * @brief Register a webapp with the group.
 * @param[in,out] reg     Registrar.
 * @param[in]     webapp  Webapp descriptor.
 * @param[in]     signer  Identity with GR_PERM_ADD_WEBAPP.
 * @return GR_OK or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_webapp_add(gr_registrar_t *reg, const gr_webapp_t *webapp,
                         const gr_identity_t *signer);

/**
 * @brief Remove a webapp by its content hash.
 * @param[in,out] reg     Registrar.
 * @param[in]     hash    Webapp content hash.
 * @param[in]     signer  Identity with GR_PERM_REMOVE_WEBAPP.
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_webapp_remove(gr_registrar_t *reg,
                            const uint8_t hash[GR_SERVICE_HASH_LEN],
                            const gr_identity_t *signer);

/**
 * @brief Check if a webapp hash is authorized for use in this group.
 * @param[in] reg   Registrar.
 * @param[in] hash  Webapp content hash to check.
 * @return true if the webapp is registered.
 */
bool gr_webapp_is_authorized(const gr_registrar_t *reg,
                             const uint8_t hash[GR_SERVICE_HASH_LEN]);

/**
 * @brief List all registered webapps.
 * @param[in]  reg           Registrar.
 * @param[out] out           Array to fill.
 * @param[in]  max_count     Capacity of @p out.
 * @param[out] actual_count  Receives the number written.
 * @return GR_OK on success.
 */
gr_error_t gr_webapp_list(const gr_registrar_t *reg, gr_webapp_t *out,
                          uint32_t max_count, uint32_t *actual_count);

/**
 * @brief Count total registered webapps.
 * @param[in]  reg  Registrar.
 * @param[out] out  Receives the count.
 * @return GR_OK on success.
 */
gr_error_t gr_webapp_count(const gr_registrar_t *reg, uint32_t *out);
/** @} */

/** @name Server Management
 *  @{ */

/**
 * @brief Add a signaling or rebroadcast server.
 * @param[in,out] reg     Registrar.
 * @param[in]     server  Server descriptor.
 * @param[in]     signer  Identity with GR_PERM_EDIT_SERVERS.
 * @return GR_OK or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_server_add(gr_registrar_t *reg, const gr_server_t *server,
                         const gr_identity_t *signer);

/**
 * @brief Remove a server by its ID hash.
 * @param[in,out] reg      Registrar.
 * @param[in]     id_hash  Server identity hash.
 * @param[in]     signer   Identity with GR_PERM_EDIT_SERVERS.
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_server_remove(gr_registrar_t *reg,
                            const uint8_t id_hash[GR_HASH_LEN],
                            const gr_identity_t *signer);

/**
 * @brief List servers of a given type.
 * @param[in]  reg           Registrar.
 * @param[in]  type          GR_SERVER_SIGNALING or GR_SERVER_REBROADCAST.
 * @param[out] out           Array to fill.
 * @param[in]  max_count     Capacity of @p out.
 * @param[out] actual_count  Receives the number written.
 * @return GR_OK on success.
 */
gr_error_t gr_server_list(const gr_registrar_t *reg, gr_server_type_t type,
                          gr_server_t *out, uint32_t max_count,
                          uint32_t *actual_count);

/**
 * @brief Count servers of a given type.
 * @param[in]  reg   Registrar.
 * @param[in]  type  Server type.
 * @param[out] out   Receives the count.
 * @return GR_OK on success.
 */
gr_error_t gr_server_count(const gr_registrar_t *reg, gr_server_type_t type,
                           uint32_t *out);
/** @} */

/** @name Epoch Management
 *  @{ */

/**
 * @brief Rotate the epoch key (generates a new symmetric key for the group).
 *
 * Previous epochs remain in the database so old ciphertexts can still
 * be decrypted.
 *
 * @param[in,out] reg     Registrar.
 * @param[in]     signer  Identity with GR_PERM_ROTATE_EPOCH.
 * @return GR_OK or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_epoch_rotate(gr_registrar_t *reg, const gr_identity_t *signer);

/**
 * @brief Get the current (latest) epoch.
 * @param[in]  reg  Registrar.
 * @param[out] out  Epoch data output.
 * @return GR_OK on success.
 */
gr_error_t gr_epoch_get_current(const gr_registrar_t *reg, gr_epoch_t *out);

/**
 * @brief Get a specific epoch by ID.
 * @param[in]  reg       Registrar.
 * @param[in]  epoch_id  Epoch ID to look up.
 * @param[out] out       Epoch data output.
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_epoch_get(const gr_registrar_t *reg, uint32_t epoch_id,
                        gr_epoch_t *out);

/**
 * @brief List all epochs (newest first).
 * @param[in]  reg           Registrar.
 * @param[out] out           Array to fill.
 * @param[in]  max_count     Capacity of @p out.
 * @param[out] actual_count  Receives the number written.
 * @return GR_OK on success.
 */
gr_error_t gr_epoch_list(const gr_registrar_t *reg, gr_epoch_t *out,
                         uint32_t max_count, uint32_t *actual_count);

/**
 * @brief Count total epochs.
 * @param[in]  reg  Registrar.
 * @param[out] out  Receives the count.
 * @return GR_OK on success.
 */
gr_error_t gr_epoch_count(const gr_registrar_t *reg, uint32_t *out);
/** @} */

/** @name Retention Policy
 *  @{ */

/**
 * @brief Set the group's retention policy.
 * @param[in,out] reg     Registrar.
 * @param[in]     policy  New retention settings.
 * @param[in]     signer  Identity with GR_PERM_EDIT_RETENTION.
 * @return GR_OK or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_retention_set(gr_registrar_t *reg, const gr_retention_t *policy,
                            const gr_identity_t *signer);

/**
 * @brief Get the current retention policy.
 * @param[in]  reg  Registrar.
 * @param[out] out  Receives the retention policy.
 * @return GR_OK on success.
 */
gr_error_t gr_retention_get(const gr_registrar_t *reg, gr_retention_t *out);
/** @} */

/* ════════════════════════════════════════════════════════════════
 *  Group icon
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Set or replace the group icon.
 *
 * @param[in,out] reg              Registrar.
 * @param[in]     data             Icon image data (PNG, JPEG, WebP, or video).
 * @param[in]     data_len         Length of @p data in bytes (max GR_GROUP_ICON_MAX_BYTES).
 * @param[in]     mime_type        MIME type string (e.g. "image/png").
 * @param[in]     width            Image width in pixels (max GR_GROUP_ICON_MAX_DIM).
 * @param[in]     height           Image height in pixels (max GR_GROUP_ICON_MAX_DIM).
 * @param[in]     is_video         true if the icon is an animated video.
 * @param[in]     static_frame     Optional static JPEG/PNG frame for video icons (may be NULL).
 * @param[in]     static_frame_len Length of @p static_frame.
 * @param[in]     signer           Identity with GR_PERM_SET_GROUP_ICON.
 * @return GR_OK, GR_ERR_SIZE_EXCEEDED, or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_group_icon_set(gr_registrar_t *reg,
                             const uint8_t *data, size_t data_len,
                             const char *mime_type,
                             uint16_t width, uint16_t height,
                             bool is_video,
                             const uint8_t *static_frame,
                             size_t static_frame_len,
                             const gr_identity_t *signer);

/**
 * @brief Retrieve the group icon (caller must free with gr_group_icon_free()).
 * @param[in]  reg  Registrar.
 * @param[out] out  Icon struct (data is heap-allocated).
 * @return GR_OK or GR_ERR_NOT_FOUND if no icon is set.
 */
gr_error_t gr_group_icon_get(const gr_registrar_t *reg,
                             gr_group_icon_t *out);

/**
 * @brief Remove the group icon.
 * @param[in,out] reg     Registrar.
 * @param[in]     signer  Identity with GR_PERM_SET_GROUP_ICON.
 * @return GR_OK or GR_ERR_UNAUTHORIZED.
 */
gr_error_t gr_group_icon_remove(gr_registrar_t *reg,
                                const gr_identity_t *signer);

/**
 * @brief Compute the content hash of the current group icon.
 * @param[in]  reg       Registrar.
 * @param[out] hash_out  Skein-1024 hash output (GR_HASH_LEN bytes).
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_group_icon_hash(const gr_registrar_t *reg,
                              uint8_t hash_out[GR_HASH_LEN]);

/**
 * @brief Free heap-allocated data inside a gr_group_icon_t.
 * @param[in,out] icon  Icon struct to free (does not free the struct itself).
 */
void gr_group_icon_free(gr_group_icon_t *icon);

/* ════════════════════════════════════════════════════════════════
 *  Signing / Verification / Encryption
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Sign the registrar header (records the signer and signature).
 * @param[in,out] reg     Registrar.
 * @param[in]     signer  Identity with GR_PERM_SIGN_REGISTRAR.
 * @return GR_OK or GR_ERR_UNAUTHORIZED.
 *
 * @code{.c}
 * gr_sign(reg, &owner);
 * // The registrar's header now contains a valid ML-DSA-87 signature.
 * @endcode
 */
gr_error_t gr_sign(gr_registrar_t *reg, const gr_identity_t *signer);

/**
 * @brief Verify the registrar header signature.
 * @param[in]  reg        Registrar.
 * @param[out] valid_out  Receives true if the signature is valid.
 * @return GR_OK on success (check valid_out for the result).
 */
gr_error_t gr_verify(const gr_registrar_t *reg, bool *valid_out);

/**
 * @brief Sign arbitrary data with an identity's ML-DSA-87 key.
 * @param[in]  signer         Signing identity.
 * @param[in]  data           Data to sign.
 * @param[in]  data_len       Length of data.
 * @param[out] signature_out  Signature output (GR_SIGN_LEN bytes).
 * @return GR_OK on success.
 */
gr_error_t gr_sign_data(const gr_identity_t *signer,
                        const uint8_t *data, size_t data_len,
                        uint8_t signature_out[GR_SIGN_LEN]);

/**
 * @brief Verify a signature over arbitrary data.
 * @param[in]  public_key  Signer's ML-DSA-87 public key.
 * @param[in]  data        Original data.
 * @param[in]  data_len    Length of data.
 * @param[in]  signature   Signature to verify (GR_SIGN_LEN bytes).
 * @param[out] valid_out   Receives true if the signature is valid.
 * @return GR_OK on success (check valid_out for the result).
 */
gr_error_t gr_verify_data(const uint8_t public_key[GR_PUBLIC_KEY_LEN],
                          const uint8_t *data, size_t data_len,
                          const uint8_t signature[GR_SIGN_LEN],
                          bool *valid_out);

/**
 * @brief Encrypt data with the current epoch key (group broadcast).
 *
 * Uses Threefish-1024-CTR + Skein-1024-MAC AEAD.
 *
 * @param[in]  reg            Registrar.
 * @param[in]  plaintext      Data to encrypt.
 * @param[in]  plaintext_len  Length of plaintext.
 * @param[in]  ad             Additional authenticated data (may be NULL).
 * @param[in]  ad_len         Length of AAD.
 * @param[out] out            Ciphertext output (needs plaintext_len + overhead).
 * @param[out] out_len        Receives actual output length.
 * @return GR_OK on success.
 */
gr_error_t gr_encrypt(const gr_registrar_t *reg,
                      const uint8_t *plaintext, size_t plaintext_len,
                      const uint8_t *ad, size_t ad_len,
                      uint8_t *out, size_t *out_len);

/**
 * @brief Decrypt data with an epoch key (tries current then previous).
 *
 * @param[in]  reg             Registrar.
 * @param[in]  ciphertext      Data to decrypt (includes nonce + epoch_id + tag).
 * @param[in]  ciphertext_len  Length of ciphertext.
 * @param[in]  ad              Additional authenticated data.
 * @param[in]  ad_len          Length of AAD.
 * @param[out] out             Plaintext output buffer.
 * @param[out] out_len         Receives actual plaintext length.
 * @param[out] epoch_id_out    Receives the epoch ID that decrypted the data (may be NULL).
 * @return GR_OK on success, GR_ERR_EPOCH_MISMATCH if no epoch key works.
 */
gr_error_t gr_decrypt(const gr_registrar_t *reg,
                      const uint8_t *ciphertext, size_t ciphertext_len,
                      const uint8_t *ad, size_t ad_len,
                      uint8_t *out, size_t *out_len,
                      uint32_t *epoch_id_out);

/**
 * @brief Encrypt data for a specific peer using ML-KEM-1024. (one-shot hybrid)
 * @param[in]  peer_kem_pk     Peer's ML-KEM public key.
 * @param[in]  plaintext       Data to encrypt.
 * @param[in]  plaintext_len   Length of plaintext.
 * @param[out] out             Output buffer (KEM ciphertext + AEAD ciphertext).
 * @param[out] out_len         Receives actual output length.
 * @return GR_OK on success.
 */
gr_error_t gr_encrypt_for_peer(const uint8_t peer_kem_pk[GR_KEM_PUBLIC_KEY_LEN],
                               const uint8_t *plaintext, size_t plaintext_len,
                               uint8_t *out, size_t *out_len);

/**
 * @brief Decrypt data from a specific peer using own ML-KEM secret key.
 * @param[in]  self             Own identity (for kem_sk).
 * @param[in]  ciphertext       Data to decrypt.
 * @param[in]  ciphertext_len   Length of ciphertext.
 * @param[out] out              Plaintext output buffer.
 * @param[out] out_len          Receives actual plaintext length.
 * @return GR_OK on success.
 */
gr_error_t gr_decrypt_from_peer(const gr_identity_t *self,
                                const uint8_t *ciphertext, size_t ciphertext_len,
                                uint8_t *out, size_t *out_len);

/* ════════════════════════════════════════════════════════════════
 *  Invite management
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Create an invite ticket for a new peer.
 *
 * Produces a serialized blob containing group metadata, bootstrap peers,
 * and a verification token. The blob can be sent to the invitee over any channel.
 *
 * @param[in,out] reg                    Registrar.
 * @param[in]     inviter                Identity with GR_PERM_INVITE_MEMBER.
 * @param[in]     expiry_timestamp_ms    Expiry time (ms since epoch), 0 for no expiry.
 * @param[out]    out_data               Receives heap-allocated invite blob (free with gr_free()).
 * @param[out]    out_len                Receives blob length.
 * @param[out]    verification_token_out Receives the verification token (GR_HASH_LEN bytes).
 * @return GR_OK or GR_ERR_UNAUTHORIZED.
 *
 * @code{.c}
 * uint8_t *blob;
 * size_t blob_len;
 * uint8_t token[GR_HASH_LEN];
 * gr_invite_create(reg, &me, 0, &blob, &blob_len, token);
 * // Send blob to the invitee; keep token to track/invalidate.
 * gr_free(blob);
 * @endcode
 */
gr_error_t gr_invite_create(gr_registrar_t *reg,
                            const gr_identity_t *inviter,
                            int64_t expiry_timestamp_ms,
                            uint8_t **out_data, size_t *out_len,
                            uint8_t verification_token_out[GR_HASH_LEN]);

/**
 * @brief Invalidate a pending invite by its verification token.
 * @param[in,out] reg                  Registrar.
 * @param[in]     verification_token   Token from gr_invite_create().
 * @param[in]     admin                Identity with GR_PERM_INVITE_MEMBER.
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_invite_invalidate(gr_registrar_t *reg,
                                const uint8_t verification_token[GR_HASH_LEN],
                                const gr_identity_t *admin);

/**
 * @brief Check if an invite is still valid (not expired, not invalidated).
 * @param[in]  reg                  Registrar.
 * @param[in]  verification_token   Token to check.
 * @param[out] valid_out            Receives true if still valid.
 * @return GR_OK on success.
 */
gr_error_t gr_invite_check(const gr_registrar_t *reg,
                           const uint8_t verification_token[GR_HASH_LEN],
                           bool *valid_out);

/**
 * @brief Mark an invite as used by a specific peer.
 * @param[in,out] reg                  Registrar.
 * @param[in]     verification_token   Token of the invite.
 * @param[in]     peer_id              ID of the peer who used the invite.
 * @param[in]     signer               Authorizing identity.
 * @return GR_OK or GR_ERR_NOT_FOUND.
 */
gr_error_t gr_invite_mark_used(gr_registrar_t *reg,
                               const uint8_t verification_token[GR_HASH_LEN],
                               const uint8_t peer_id[GR_PEER_ID_LEN],
                               const gr_identity_t *signer);

/**
 * @brief List all invites (active, used, and invalidated).
 * @param[in]  reg           Registrar.
 * @param[out] out           Array to fill.
 * @param[in]  max_count     Capacity of @p out.
 * @param[out] actual_count  Receives the number written.
 * @return GR_OK on success.
 */
gr_error_t gr_invite_list(const gr_registrar_t *reg,
                          gr_invite_info_t *out, uint32_t max_count,
                          uint32_t *actual_count);

/**
 * @brief Count total invites.
 * @param[in]  reg  Registrar.
 * @param[out] out  Receives the count.
 * @return GR_OK on success.
 */
gr_error_t gr_invite_count(const gr_registrar_t *reg, uint32_t *out);

/**
 * @brief Parse an invite ticket from the wire format blob.
 * @param[in]  invite_data  Raw invite blob from gr_invite_create().
 * @param[in]  invite_len   Length of the blob.
 * @param[out] ticket_out   Parsed ticket struct.
 * @return GR_OK or GR_ERR_INVITE_INVALID.
 */
gr_error_t gr_invite_parse(const uint8_t *invite_data, size_t invite_len,
                           gr_invite_ticket_t *ticket_out);

/* ════════════════════════════════════════════════════════════════
 *  Join verification
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Begin join verification after parsing an invite ticket.
 *
 * Validates the ticket against the opened registrar. If the group is
 * small (< GR_JOIN_SMALL_GROUP_THRESHOLD), verification is skipped.
 *
 * @param[in,out] reg     Registrar (opened from the fetched group DB).
 * @param[in]     ticket  Parsed invite ticket.
 * @return GR_OK on success, GR_ERR_JOIN_FAILED if the ticket doesn't match.
 */
gr_error_t gr_join_begin(gr_registrar_t *reg,
                         const gr_invite_ticket_t *ticket);

/**
 * @brief Register a callback for peer dissent during join verification.
 *
 * Called if any attesting peer disagrees about the group owner.
 *
 * @param[in,out] reg        Registrar.
 * @param[in]     callback   Dissent handler (return true to accept override).
 * @param[in]     user_data  Opaque context passed to the callback.
 * @return GR_OK on success.
 */
gr_error_t gr_join_set_dissent_callback(gr_registrar_t *reg,
                                        gr_join_dissent_fn callback,
                                        void *user_data);

/**
 * @brief Submit a peer's header attestation during join verification.
 *
 * Each connecting peer signs an attestation binding their view of the
 * group ownership to the joiner's nonce.
 *
 * @param[in,out] reg             Registrar.
 * @param[in]     peer_header     Attesting peer's registrar header.
 * @param[in]     peer_id         Attesting peer's ID.
 * @param[in]     peer_pk         Attesting peer's ML-DSA public key.
 * @param[in]     peer_signature  ML-DSA signature over the attestation.
 * @return GR_OK (vote recorded), GR_ERR_NOT_FOUND, GR_ERR_SIGNATURE_INVALID,
 *         or GR_ERR_JOIN_INVITER_EXCLUDED.
 */
gr_error_t gr_join_submit_peer_header(gr_registrar_t *reg,
                                      const gr_header_t *peer_header,
                                      const uint8_t peer_id[GR_PEER_ID_LEN],
                                      const uint8_t peer_pk[GR_PUBLIC_KEY_LEN],
                                      const uint8_t peer_signature[GR_SIGN_LEN]);

/**
 * @brief Evaluate the current join verification state.
 *
 * @param[in,out] reg         Registrar.
 * @param[out]    result_out  Receives the verification result.
 * @return GR_OK on success.
 *
 * @code{.c}
 * gr_join_verify_result_t result;
 * gr_join_evaluate(reg, &result);
 * if (result.state == GR_JOIN_VERIFIED)
 *     printf("Join verified!\n");
 * @endcode
 */
gr_error_t gr_join_evaluate(gr_registrar_t *reg,
                            gr_join_verify_result_t *result_out);

/**
 * @brief Get the current join state without re-evaluating.
 * @param[in]  reg        Registrar.
 * @param[out] state_out  Receives the join state enum.
 * @return GR_OK on success.
 */
gr_error_t gr_join_get_state(const gr_registrar_t *reg,
                             gr_join_state_t *state_out);

/**
 * @brief Get the join nonce to share with connecting peers.
 *
 * Each peer uses this nonce in their attestation so the joiner can
 * verify the attestation is fresh and targeted.
 *
 * @param[in]  reg        Registrar (must be in join verification).
 * @param[out] nonce_out  Receives the nonce (GR_JOIN_NONCE_LEN bytes).
 * @return GR_OK on success.
 */
gr_error_t gr_join_get_nonce(const gr_registrar_t *reg,
                             uint8_t nonce_out[GR_JOIN_NONCE_LEN]);

/**
 * @brief Export a header attestation for a joining peer (existing member side).
 *
 * Signs (header_hash || joiner_nonce || your_peer_id || your_pk) with ML-DSA-87.
 *
 * @param[in]  reg            Registrar.
 * @param[in]  self           Your identity.
 * @param[in]  joiner_nonce   Nonce received from the joiner.
 * @param[out] header_out     Your registrar header (for the joiner).
 * @param[out] signature_out  ML-DSA-87 signature (GR_SIGN_LEN bytes).
 * @return GR_OK on success.
 */
gr_error_t gr_join_export_header_attestation(
    const gr_registrar_t *reg, const gr_identity_t *self,
    const uint8_t joiner_nonce[GR_JOIN_NONCE_LEN],
    gr_header_t *header_out, uint8_t signature_out[GR_SIGN_LEN]);

/**
 * @brief List peers that have not yet submitted attestations.
 *
 * Useful for prompting specific peers via gossipsub.
 *
 * @param[in]  reg           Registrar.
 * @param[out] peer_ids_out  Buffer for peer IDs (each GR_PEER_ID_LEN bytes).
 * @param[in]  max_count     Maximum number of peer IDs to return.
 * @param[out] actual_count  Receives the number written.
 * @return GR_OK on success.
 */
gr_error_t gr_join_list_unattested_peers(
    const gr_registrar_t *reg, uint8_t *peer_ids_out,
    uint32_t max_count, uint32_t *actual_count);

/**
 * @brief Check if the registrar is in a trusted (verified) state.
 * @param[in] reg  Registrar.
 * @return true if join verification passed or was not required.
 */
bool gr_is_trusted(const gr_registrar_t *reg);

/* ════════════════════════════════════════════════════════════════
 *  Audit / Serialization / Utility
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief List audit log entries since a given timestamp.
 * @param[in]  reg              Registrar.
 * @param[in]  since_timestamp  Only return entries after this time (ms since epoch).
 * @param[out] out              Array to fill.
 * @param[in]  max_count        Capacity of @p out.
 * @param[out] actual_count     Receives the number written.
 * @return GR_OK on success.
 */
gr_error_t gr_audit_list(const gr_registrar_t *reg, int64_t since_timestamp,
                         gr_audit_entry_t *out, uint32_t max_count,
                         uint32_t *actual_count);

/**
 * @brief Append a signed, hash-chained audit entry.
 *
 * @param[in,out] reg          Registrar.
 * @param[in]     change_type  The type of mutation being recorded.
 * @param[in]     actor        Identity of the actor (signs the entry).
 * @param[in]     target_id    Target peer ID (may be NULL).
 * @param[in]     detail       Human-readable detail string (may be NULL).
 * @return GR_OK on success.
 */
gr_error_t gr_audit_append(gr_registrar_t *reg, gr_change_type_t change_type,
                           const gr_identity_t *actor,
                           const uint8_t target_id[GR_PEER_ID_LEN],
                           const char *detail);

/**
 * @brief Count total audit log entries.
 * @param[in]  reg  Registrar.
 * @param[out] out  Receives the count.
 * @return GR_OK on success.
 */
gr_error_t gr_audit_count(const gr_registrar_t *reg, uint32_t *out);

/**
 * @brief Verify the integrity of the audit hash chain.
 * @param[in]  reg     Registrar.
 * @param[out] result  Receives verification statistics.
 * @return GR_OK on success.
 */
gr_error_t gr_audit_verify_chain(const gr_registrar_t *reg,
                                 gr_audit_chain_result_t *result);

/**
 * @brief Enforce retention policy by pruning old audit entries.
 * @param[in,out] reg  Registrar.
 * @return GR_OK on success.
 */
gr_error_t gr_audit_enforce_retention(gr_registrar_t *reg);

/**
 * @brief List detected forks in the audit chain.
 * @param[in]  reg           Registrar.
 * @param[out] out           Array of fork descriptors.
 * @param[in]  max_forks     Capacity of @p out.
 * @param[out] actual_count  Receives the number of forks found.
 * @return GR_OK on success.
 */
gr_error_t gr_audit_list_forks(const gr_registrar_t *reg,
                              gr_audit_fork_t *out,
                              uint32_t max_forks,
                              uint32_t *actual_count);

/**
 * @brief Register a callback for delta sync anomaly detection.
 *
 * Called when an incoming delta is suspiciously large or spans too long.
 *
 * @param[in,out] reg        Registrar.
 * @param[in]     callback   Anomaly handler (return action to take).
 * @param[in]     user_data  Opaque context.
 * @return GR_OK on success.
 */
gr_error_t gr_set_delta_anomaly_callback(gr_registrar_t *reg,
                                         gr_delta_anomaly_fn callback,
                                         void *user_data);

/**
 * @brief Register a callback for persist-to-disk prompts.
 *
 * Called when estimated registrar size exceeds GR_PERSIST_PROMPT_BYTES.
 *
 * @param[in,out] reg        Registrar.
 * @param[in]     callback   Prompt handler (return true to persist).
 * @param[in]     user_data  Opaque context.
 * @return GR_OK on success.
 */
gr_error_t gr_set_persist_prompt_callback(gr_registrar_t *reg,
                                          gr_persist_prompt_fn callback,
                                          void *user_data);

/**
 * @brief Serialize the entire registrar to a portable blob.
 *
 * @param[in]  reg       Registrar.
 * @param[in]  mode      GR_SERIALIZE_FULL or GR_SERIALIZE_OWNER (owner-only keys).
 * @param[out] out_data  Receives heap-allocated blob (free with gr_free()).
 * @param[out] out_len   Receives blob length.
 * @return GR_OK on success.
 */
gr_error_t gr_serialize(const gr_registrar_t *reg, gr_serialize_mode_t mode,
                        uint8_t **out_data, size_t *out_len);

/**
 * @brief Deserialize a full registrar blob into an opened registrar.
 * @param[in,out] reg       Registrar to populate.
 * @param[in]     data      Serialized blob.
 * @param[in]     data_len  Length of blob.
 * @return GR_OK or GR_ERR_SERIALIZATION.
 */
gr_error_t gr_deserialize(gr_registrar_t *reg,
                          const uint8_t *data, size_t data_len);

/**
 * @brief Serialize a delta (changes since a given version).
 * @param[in]  reg            Registrar.
 * @param[in]  since_version  Starting version (exclusive).
 * @param[out] out_data       Receives heap-allocated delta blob (free with gr_free()).
 * @param[out] out_len        Receives blob length.
 * @return GR_OK on success.
 */
gr_error_t gr_serialize_delta(const gr_registrar_t *reg,
                              uint32_t since_version,
                              uint8_t **out_data, size_t *out_len);

/**
 * @brief Apply a delta blob to bring the registrar up to date.
 *
 * @param[in,out] reg         Registrar.
 * @param[in]     data        Delta blob.
 * @param[in]     data_len    Length of blob.
 * @param[out]    result_out  Receives merge statistics (may be NULL).
 * @return GR_OK or GR_ERR_MERGE_CONFLICT.
 */
gr_error_t gr_apply_delta(gr_registrar_t *reg, const uint8_t *data,
                          size_t data_len, gr_merge_result_t *result_out);

/**
 * @brief Compute a Skein-1024 hash of arbitrary data.
 * @param[in]  data      Input data.
 * @param[in]  data_len  Length of data.
 * @param[out] out       Hash output (GR_HASH_LEN bytes).
 * @return GR_OK on success.
 */
gr_error_t gr_hash(const uint8_t *data, size_t data_len,
                   uint8_t out[GR_HASH_LEN]);

/**
 * @brief Compare two peer IDs for equality (constant-time).
 * @param[in] a  First peer ID.
 * @param[in] b  Second peer ID.
 * @return true if equal.
 */
bool gr_id_equal(const uint8_t a[GR_PEER_ID_LEN],
                 const uint8_t b[GR_PEER_ID_LEN]);

/**
 * @brief Get the current time in milliseconds since Unix epoch.
 * @return Timestamp in milliseconds.
 */
int64_t gr_timestamp_ms(void);

/**
 * @brief Get the current time in nanoseconds since Unix epoch.
 * @return Timestamp in nanoseconds.
 */
int64_t gr_timestamp_ns(void);

/**
 * @brief Get the current database schema version.
 * @return Schema version number.
 */
uint64_t gr_schema_version(void);

/**
 * @brief Free a heap-allocated buffer returned by gr_serialize/gr_invite_create/etc.
 * @param[in] ptr  Buffer to free (NULL-safe).
 */
void gr_free(void *ptr);

/**
 * @brief Get a human-readable string for an error code.
 * @param[in] err  Error code.
 * @return Static string describing the error.
 */
const char *gr_error_str(gr_error_t err);

/* ════════════════════════════════════════════════════════════════
 *  Behavioral Analysis
 *
 *  Pure-logic, read-only analysis of audit logs, peer history,
 *  admin/mod actions, and sync patterns.  Every function is
 *  passive — it reads existing DB state and returns a result
 *  struct.  No mutations, no callbacks, no side-effects.
 * ════════════════════════════════════════════════════════════════ */

/* ── Per-actor burst detection ───────────────────────────────── */

struct gr_actor_burst {
    uint32_t actions_in_window;
    uint32_t destructive_actions;   /* kicks + bans + removes      */
    uint32_t role_changes;          /* role add/remove/modify       */
    uint32_t invites_created;
    uint32_t epoch_rotations;
    float    actions_per_minute;
    bool     burst_detected;        /* exceeds threshold            */
};

/* ── Group-wide mutation rate ────────────────────────────────── */

typedef struct {
    uint32_t total_mutations;
    uint32_t distinct_actors;
    float    mutations_per_minute;
    float    mutations_per_actor;
    bool     swarm_detected;
} gr_mutation_rate_t;

/* ── Admin / moderator abuse scoring ─────────────────────────── */

struct gr_admin_score {
    uint32_t total_admin_actions;
    uint32_t kicks;
    uint32_t bans;
    uint32_t removes;
    uint32_t role_modifications;
    uint32_t permission_escalations; /* GR_CHANGE_PEER_ROLE_CHANGED  */
    float    destructive_ratio;      /* destructive / total          */
    bool     abuse_suspected;
};

/* ── Peer churn / history ────────────────────────────────────── */

typedef struct {
    uint32_t active_peers;
    uint32_t kicked_peers;
    uint32_t banned_peers;
    uint32_t left_peers;
    uint32_t joined_in_window;
    uint32_t removed_in_window;
    float    churn_rate;             /* (joined+removed) / active    */
    uint32_t stale_count;            /* active but not seen recently */
} gr_peer_churn_t;

/* ── Delta / download anomaly scoring ────────────────────────── */

typedef struct {
    size_t   delta_bytes;
    uint32_t entry_count;
    int64_t  offline_duration_ms;
    float    bytes_per_offline_day;
    float    entries_per_offline_day;
    bool     anomalous;
} gr_delta_score_t;

/* ── Epoch rotation pattern ──────────────────────────────────── */

typedef struct {
    uint32_t rotations_in_window;
    int64_t  avg_epoch_lifetime_ms;
    bool     excessive_rotation;     /* > 1 per hour in window       */
} gr_epoch_pattern_t;

/* ── Network / sync interaction pattern ──────────────────────── */

typedef struct {
    int64_t  last_update_ms;         /* header.updated_at            */
    int64_t  time_since_update_ms;
    size_t   estimated_registrar_bytes;
    uint32_t total_audit_entries;
    float    avg_entry_interval_ms;  /* mean time between entries    */
} gr_network_score_t;

/* ── Behavioral analysis config ──────────────────────────────── */

typedef struct {
    float    burst_actions_per_min;         /* per-actor, default 20  */
    float    swarm_mutations_per_min;       /* group-wide, default 60 */
    float    abuse_destructive_ratio;       /* default 0.6            */
    uint32_t abuse_min_actions;             /* default 10             */
    float    epoch_max_per_hour;            /* default 1.0            */
    float    delta_anomaly_entries_per_day; /* default 100            */
    float    churn_attention_threshold;     /* default 2.0            */
    bool     scale_by_group_size;           /* default true           */
    int64_t  alert_window_ms;               /* per-mutation check window, default 300000 (5 min) */
} gr_behavior_config_t;

static inline gr_behavior_config_t gr_behavior_config_defaults(void) {
    return (gr_behavior_config_t){
        .burst_actions_per_min         = 20.0f,
        .swarm_mutations_per_min       = 60.0f,
        .abuse_destructive_ratio       = 0.6f,
        .abuse_min_actions             = 10,
        .epoch_max_per_hour            = 1.0f,
        .delta_anomaly_entries_per_day = 100.0f,
        .churn_attention_threshold     = 2.0f,
        .scale_by_group_size           = true,
        .alert_window_ms               = 300000, /* 5 minutes */
    };
}

/* ── Full group health snapshot ──────────────────────────────── */

typedef struct {
    gr_peer_churn_t     churn;
    gr_mutation_rate_t  mutation_rate;
    gr_epoch_pattern_t  epoch_pattern;
    gr_network_score_t  network;
    gr_admin_score_t    worst_admin;
    uint8_t             worst_admin_id[GR_PEER_ID_LEN];
    bool                has_worst_admin;
    size_t              estimated_size;
    bool                needs_attention; /* any flag raised          */
} gr_behavior_snapshot_t;

/* ── Behavioral analysis API ─────────────────────────────────── */

/**
 * @brief Detect burst activity for a specific actor within a time window.
 *
 * @param[in]  reg        Registrar.
 * @param[in]  actor_id   Peer ID of the actor to analyze.
 * @param[in]  window_ms  Time window in milliseconds.
 * @param[out] out        Burst detection result.
 * @return GR_OK on success.
 *
 * @code{.c}
 * gr_actor_burst_t burst;
 * gr_behavior_actor_burst(reg, peer_id, 300000, &burst); // 5-minute window
 * if (burst.burst_detected)
 *     printf("Peer exceeded %.1f actions/min\n", burst.actions_per_minute);
 * @endcode
 */
gr_error_t gr_behavior_actor_burst(const gr_registrar_t *reg,
                                   const uint8_t actor_id[GR_PEER_ID_LEN],
                                   int64_t window_ms,
                                   gr_actor_burst_t *out);

/**
 * @brief Measure the group-wide mutation rate within a time window.
 * @param[in]  reg        Registrar.
 * @param[in]  window_ms  Time window in milliseconds.
 * @param[out] out        Mutation rate result.
 * @return GR_OK on success.
 */
gr_error_t gr_behavior_mutation_rate(const gr_registrar_t *reg,
                                     int64_t window_ms,
                                     gr_mutation_rate_t *out);

/**
 * @brief Score an admin/moderator for potential abuse.
 * @param[in]  reg        Registrar.
 * @param[in]  admin_id   Peer ID of the admin to analyze.
 * @param[in]  window_ms  Time window in milliseconds.
 * @param[out] out        Admin abuse score.
 * @return GR_OK on success.
 */
gr_error_t gr_behavior_admin_score(const gr_registrar_t *reg,
                                   const uint8_t admin_id[GR_PEER_ID_LEN],
                                   int64_t window_ms,
                                   gr_admin_score_t *out);

/**
 * @brief Analyze peer churn (join/leave/kick/ban rates).
 * @param[in]  reg        Registrar.
 * @param[in]  window_ms  Time window in milliseconds.
 * @param[out] out        Churn analysis result.
 * @return GR_OK on success.
 */
gr_error_t gr_behavior_peer_churn(const gr_registrar_t *reg,
                                  int64_t window_ms,
                                  gr_peer_churn_t *out);

/**
 * @brief Score an incoming delta for anomalies (size, entry count, time gap).
 * @param[in]  reg          Registrar.
 * @param[in]  delta_bytes  Size of the incoming delta.
 * @param[in]  entry_count  Number of entries in the delta.
 * @param[out] out          Delta anomaly score.
 * @return GR_OK on success.
 */
gr_error_t gr_behavior_delta_score(const gr_registrar_t *reg,
                                   size_t delta_bytes,
                                   uint32_t entry_count,
                                   gr_delta_score_t *out);

/**
 * @brief List peers that haven't been seen recently (stale).
 * @param[in]  reg                  Registrar.
 * @param[in]  stale_threshold_ms   Peers not seen in this many ms are stale.
 * @param[out] peer_ids_out         Buffer for stale peer IDs.
 * @param[in]  max_count            Capacity.
 * @param[out] actual_count         Receives the number written.
 * @return GR_OK on success.
 */
gr_error_t gr_behavior_stale_peers(const gr_registrar_t *reg,
                                   int64_t stale_threshold_ms,
                                   uint8_t *peer_ids_out,
                                   uint32_t max_count,
                                   uint32_t *actual_count);

/**
 * @brief Analyze epoch rotation patterns (too frequent = suspicious).
 * @param[in]  reg        Registrar.
 * @param[in]  window_ms  Time window in milliseconds.
 * @param[out] out        Epoch pattern analysis.
 * @return GR_OK on success.
 */
gr_error_t gr_behavior_epoch_pattern(const gr_registrar_t *reg,
                                     int64_t window_ms,
                                     gr_epoch_pattern_t *out);

/**
 * @brief Compute overall network/sync health metrics.
 * @param[in]  reg  Registrar.
 * @param[out] out  Network health score.
 * @return GR_OK on success.
 */
gr_error_t gr_behavior_network_score(const gr_registrar_t *reg,
                                     gr_network_score_t *out);

/**
 * @brief Take a full behavioral health snapshot of the group.
 *
 * Combines churn, mutation rate, epoch pattern, network score,
 * and worst-admin analysis into a single summary.
 *
 * @param[in]  reg        Registrar.
 * @param[in]  window_ms  Time window for time-based metrics.
 * @param[out] out        Snapshot result.
 * @return GR_OK on success.
 *
 * @code{.c}
 * gr_behavior_snapshot_t snap;
 * gr_behavior_snapshot(reg, 300000, &snap); // 5-minute window
 * if (snap.needs_attention)
 *     printf("Group health needs attention!\n");
 * @endcode
 */
gr_error_t gr_behavior_snapshot(const gr_registrar_t *reg,
                                int64_t window_ms,
                                gr_behavior_snapshot_t *out);

/**
 * @brief Set the behavioral analysis configuration thresholds.
 * @param[in,out] reg     Registrar.
 * @param[in]     config  New configuration.
 * @return GR_OK on success.
 */
gr_error_t gr_behavior_set_config(gr_registrar_t *reg,
                                  const gr_behavior_config_t *config);

/**
 * @brief Get the current behavioral analysis configuration.
 * @param[in]  reg  Registrar.
 * @param[out] out  Receives the current config.
 * @return GR_OK on success.
 */
gr_error_t gr_behavior_get_config(const gr_registrar_t *reg,
                                  gr_behavior_config_t *out);

/**
 * @brief Register a per-mutation behavioral alert callback.
 *
 * Fires on every mutation that triggers burst or abuse detection.
 *
 * @param[in,out] reg        Registrar.
 * @param[in]     callback   Alert handler.
 * @param[in]     user_data  Opaque context.
 * @return GR_OK on success.
 */
gr_error_t gr_set_behavior_alert_callback(gr_registrar_t *reg,
                                          gr_behavior_alert_fn callback,
                                          void *user_data);

/* ════════════════════════════════════════════════════════════════
 *  IP Blocklist
 * ════════════════════════════════════════════════════════════════ */

/**
 * @brief Check if an IP address is currently blocked.
 *
 * Expired blocks (older than @p block_duration_ms) are automatically
 * cleaned up and the IP is reported as not blocked.
 *
 * @param[in]  reg                Registrar.
 * @param[in]  ip                 IP address string (IPv4 or IPv6, max 45 chars).
 * @param[in]  block_duration_ms  Duration of a block in milliseconds.
 * @param[out] blocked_out        Receives true if currently blocked.
 * @return GR_OK on success, GR_ERR_DB on database failure.
 */
gr_error_t gr_blocklist_check(gr_registrar_t *reg, const char *ip,
                              int64_t block_duration_ms, bool *blocked_out);

/**
 * @brief Record a failed authentication attempt from an IP.
 *
 * Increments the fail counter. If it reaches @p max_fails, the IP is
 * blocked and @p just_blocked_out is set to true.
 *
 * @param[in]  reg              Registrar.
 * @param[in]  ip               IP address string.
 * @param[in]  max_fails        Number of failures before blocking.
 * @param[out] just_blocked_out Receives true if this call caused the block.
 * @return GR_OK on success, GR_ERR_DB on database failure.
 */
gr_error_t gr_blocklist_record_fail(gr_registrar_t *reg, const char *ip,
                                    int max_fails, bool *just_blocked_out);

/**
 * @brief Reset the fail counter for an IP (e.g. after successful auth).
 *
 * @param[in,out] reg  Registrar.
 * @param[in]     ip   IP address string.
 * @return GR_OK on success, GR_ERR_DB on database failure.
 */
gr_error_t gr_blocklist_reset(gr_registrar_t *reg, const char *ip);

/**
 * @brief Delete all expired block entries from the database.
 *
 * @param[in,out] reg                Registrar.
 * @param[in]     block_duration_ms  Block duration used to compute the cutoff.
 * @return GR_OK on success, GR_ERR_DB on database failure.
 */
gr_error_t gr_blocklist_cleanup(gr_registrar_t *reg,
                                int64_t block_duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* GROUP_REGISTRAR_H */
