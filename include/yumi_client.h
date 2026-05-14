/*
 * yumi_client.h — Yumi Client: high-level peer group networking
 *
 * Wraps Group Registrar + Secure UDP into a single easy API:
 *
 *   - Auto-sync: registrar delta changes replicate to all connected
 *     peers whenever the local version advances.
 *   - Attestation: join verification runs automatically in the
 *     background after connecting via an invite code.
 *   - Bootstrap: joining a group via invite fetches the registrar
 *     from bootstrap peers, opens it, and starts syncing.
 *   - Messaging: send/broadcast to peers on user channels (2-253).
 *   - Meshnet relay: forward packets to unreachable peers via
 *     intermediate hops with logarithmic TTL and rate limiting.
 *
 * ═══════════════════════════════════════════════════════════════════
 *  QUICK START
 * ═══════════════════════════════════════════════════════════════════
 *
 * ── Creating a group ─────────────────────────────────────────────
 *
 *   yumi_crypto_init();
 *
 *   yumi_client_config_t cfg = {
 *       .db_path    = "/tmp/my_group.db",
 *       .group_name = "My Group",
 *       .on_message = my_recv_handler,
 *       .on_event   = my_event_handler,
 *       .user       = my_ctx,
 *   };
 *
 *   yumi_client_t *client;
 *   yumi_client_create_group(&client, &cfg);
 *
 *   // Create an invite for someone:
 *   uint8_t *invite;
 *   size_t invite_len;
 *   yumi_client_invite(client, 0, &invite, &invite_len);
 *   // hand invite_blob to the invitee …
 *   free(invite);
 *
 * ── Joining a group via invite ───────────────────────────────────
 *
 *   yumi_client_config_t cfg = {
 *       .db_path    = "/tmp/joined.db",
 *       .on_message = my_recv_handler,
 *       .on_event   = my_event_handler,
 *       .user       = my_ctx,
 *   };
 *
 *   yumi_client_t *client;
 *   yumi_client_join(&client, &cfg, invite_blob, invite_len);
 *   // Registrar is fetched, peers connected, attestation running.
 *
 * ── Sending messages ─────────────────────────────────────────────
 *
 *   yumi_client_broadcast(client, data, len);           // all peers
 *   yumi_client_send(client, peer_id, data, len);       // unreliable
 *   yumi_client_send_reliable(client, peer_id, data, len);
 *
 * ── Shutdown ─────────────────────────────────────────────────────
 *
 *   yumi_client_destroy(client);
 *
 * ═══════════════════════════════════════════════════════════════════
 *
 * Channel allocation:
 *   0       Registrar sync protocol (version exchange, delta transfer)
 *   1       Meshnet relay (forward packets for unreachable peers,
 *           logarithmic TTL, 10 KB/s default throttle)
 *   2-253   User data (channel 2 is the default for webapps)
 *   254     Attestation protocol (join nonce, header attestation)
 *   255     Reserved
 *
 * Meshnet relay (channel 1):
 *   Enables communication with peers that can't be directly reached.
 *   TTL = max(2, ceil(log2(active_peers)))  — scales logarithmically.
 *   Default rate limit: 10 KB/s per forwarding peer.
 *   Behavioral analysis flags excessive relay abuse.
 *
 * Duplicate connection detection:
 *   If a second instance of the browser (or any client) attempts to
 *   connect using the same identity, the duplicate handshake is
 *   rejected and YUMI_EVENT_DUPLICATE_PEER fires.
 *
 * Threading model:
 *   - One SUDP worker thread per peer connection.
 *   - One sync worker thread (polls registrar version, auto-connects).
 *   - One bootstrap listener thread (raw UDP, serves registrar to
 *     joiners and handles SUDP port signaling).
 *   - All registrar access serialized by the registrar's db_lock.
 */

#ifndef YUMI_CLIENT_H
#define YUMI_CLIENT_H

#include "group_registrar.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────────────────────────────────────────────── */

#define YUMI_CLIENT_MAX_PEERS       64

/* Channel allocation */
#define YUMI_CLIENT_SYNC_CH         0       /* registrar delta sync        */
#define YUMI_CLIENT_MESH_CH         1       /* meshnet relay               */
#define YUMI_CLIENT_USER_CH         2       /* default user / webapp data  */
#define YUMI_CLIENT_ATTEST_CH       254     /* join attestation            */
#define YUMI_CLIENT_RESERVED_CH     255

/* Meshnet relay */
#define YUMI_MESH_MIN_TTL           2       /* minimum hops                */
#define YUMI_MESH_RATE_BYTES_SEC    10240   /* 10 KB/s default throttle    */
#define YUMI_MESH_RATE_WINDOW_MS    1000    /* token bucket refill window  */

/* Bootstrap wire protocol */
#define YUMI_BOOT_CHUNK_SIZE        1000
#define YUMI_BOOT_TIMEOUT_MS        30000
#define YUMI_BOOT_RETRY_MS          2000

/* ── Client state ────────────────────────────────────────────────── */

typedef enum {
    YUMI_CLIENT_INITIALIZING = 0,
    YUMI_CLIENT_CONNECTING,       /* connecting to peers              */
    YUMI_CLIENT_SYNCING,          /* downloading registrar (join)     */
    YUMI_CLIENT_RUNNING,          /* fully operational                */
    YUMI_CLIENT_CLOSED,
} yumi_client_state_t;

/* ── Events ──────────────────────────────────────────────────────── */

typedef enum {
    YUMI_EVENT_PEER_CONNECTED      = 1,
    YUMI_EVENT_PEER_DISCONNECTED   = 2,
    YUMI_EVENT_SYNC_COMPLETE       = 3,   /* registrar up-to-date      */
    YUMI_EVENT_VERIFIED            = 4,   /* join attestation passed   */
    YUMI_EVENT_VERIFICATION_FAILED = 5,
    YUMI_EVENT_PEER_KICKED         = 6,
    YUMI_EVENT_EPOCH_ROTATED       = 7,
    YUMI_EVENT_DUPLICATE_PEER      = 8,   /* duplicate connection blocked */
    YUMI_EVENT_MESH_ABUSE          = 9,   /* meshnet relay abuse detected */
    YUMI_EVENT_BOOT_BLOCKED        = 10,  /* IP blocked after failed attempts */
} yumi_client_event_t;

/* ── Callbacks ───────────────────────────────────────────────────── */

/**
 * Fired for every user-data message received on channels 2-253.
 * (Channels 0-1 and 254-255 are reserved for internal protocols.)
 */
typedef void (*yumi_client_message_fn)(
    void        *user,
    const uint8_t peer_id[GR_PEER_ID_LEN],
    uint8_t      channel,
    const void  *data,
    uint32_t     len,
    bool         reliable);

/**
 * Fired for lifecycle events (peer connect/disconnect, sync, etc.).
 * @p peer_id may be NULL for non-peer-specific events.
 */
typedef void (*yumi_client_event_fn)(
    void                   *user,
    yumi_client_event_t     event,
    const uint8_t          *peer_id);

/* ── Configuration ───────────────────────────────────────────────── */

typedef struct {
    /* Storage */
    const char          *db_path;       /* DuckDB path (":memory:" ok) */
    const char          *group_name;    /* creation only               */
    gr_group_type_t      group_type;    /* GR_GROUP_PRIVATE / PUBLIC   */

    /* Callbacks */
    yumi_client_message_fn on_message;
    yumi_client_event_fn   on_event;
    void                  *user;

    /* Network (optional) */
    const char *stun_server;            /* NULL = direct mode          */
    uint16_t    stun_port;              /* 0 = default 3478            */
    uint16_t    local_port;             /* 0 = ephemeral               */

    /* STUN fallback list (overrides stun_server when set) */
    const char        **stun_servers;       /* array of hostnames      */
    uint32_t            stun_server_count;  /* entries in the array    */
} yumi_client_config_t;

/* ── Opaque handle ───────────────────────────────────────────────── */

typedef struct yumi_client yumi_client_t;

/* ═══════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * Create a new group.  Caller becomes owner.
 * Starts the bootstrap listener and sync worker automatically.
 *
 * @param[out] out  Receives the allocated client.
 * @param[in]  cfg  Configuration (db_path, group_name required).
 * @return 0 on success, -1 on error.
 */
int yumi_client_create_group(yumi_client_t **out,
                              const yumi_client_config_t *cfg);

/**
 * Open an existing group from a local registrar DB.
 * Auto-connects to all active peers found in the registrar.
 *
 * @param[out] out       Receives the allocated client.
 * @param[in]  cfg       Configuration (db_path required).
 * @param[in]  group_id  Expected group ID (GR_HASH_LEN bytes).
 * @param[in]  identity  Your identity (must already be a member).
 * @return 0 on success, -1 on error.
 */
int yumi_client_open(yumi_client_t **out,
                     const yumi_client_config_t *cfg,
                     const uint8_t group_id[GR_HASH_LEN],
                     const gr_identity_t *identity);

/**
 * Join a group via an invite blob.
 *
 * Automatically: parses invite → connects to bootstrap peer →
 * downloads registrar → begins join verification → starts sync.
 * Blocks until the registrar is fetched (typically 1-5 s).
 * Attestation continues asynchronously; YUMI_EVENT_VERIFIED fires
 * when complete.
 *
 * @param[out] out         Receives the allocated client.
 * @param[in]  cfg         Configuration (db_path required).
 * @param[in]  invite_blob Raw invite blob from yumi_client_invite().
 * @param[in]  invite_len  Length of the invite blob.
 * @return 0 on success, -1 on error.
 */
int yumi_client_join(yumi_client_t **out,
                     const yumi_client_config_t *cfg,
                     const uint8_t *invite_blob, size_t invite_len);

/**
 * Shut down all connections and free resources.
 * Blocks until background threads finish.
 */
void yumi_client_destroy(yumi_client_t *c);

/* ═══════════════════════════════════════════════════════════════════
 *  Messaging
 * ═══════════════════════════════════════════════════════════════════ */

/** Unreliable broadcast to all connected peers (channel 2). */
int yumi_client_broadcast(yumi_client_t *c,
                           const void *data, uint32_t len);

/** Reliable broadcast to all connected peers (channel 2). */
int yumi_client_broadcast_reliable(yumi_client_t *c,
                                    const void *data, uint32_t len);

/** Unreliable send to one peer (channel 2). */
int yumi_client_send(yumi_client_t *c,
                     const uint8_t peer_id[GR_PEER_ID_LEN],
                     const void *data, uint32_t len);

/** Reliable send to one peer (channel 2). */
int yumi_client_send_reliable(yumi_client_t *c,
                               const uint8_t peer_id[GR_PEER_ID_LEN],
                               const void *data, uint32_t len);

/**
 * Send on a specific channel (2-253) to one peer.
 * Channels 0-1 and 254-255 are reserved for internal protocols.
 */
int yumi_client_send_channel(yumi_client_t *c,
                              const uint8_t peer_id[GR_PEER_ID_LEN],
                              uint8_t channel,
                              const void *data, uint32_t len,
                              bool reliable);

/* ═══════════════════════════════════════════════════════════════════
 *  Invitations
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * Create an invite blob for a new peer.
 * Caller must free *out_blob with free().
 *
 * @param[in]  c          Client (must be owner or have INVITE perm).
 * @param[in]  expiry_ms  Expiry timestamp (0 = no expiry).
 * @param[out] out_blob   Receives heap-allocated invite blob.
 * @param[out] out_len    Receives blob length.
 * @return 0 on success, -1 on error.
 */
int yumi_client_invite(yumi_client_t *c, int64_t expiry_ms,
                        uint8_t **out_blob, size_t *out_len);

/* ═══════════════════════════════════════════════════════════════════
 *  Queries
 * ═══════════════════════════════════════════════════════════════════ */

yumi_client_state_t    yumi_client_get_state(const yumi_client_t *c);
gr_registrar_t        *yumi_client_get_registrar(yumi_client_t *c);
const gr_identity_t   *yumi_client_get_identity(const yumi_client_t *c);
uint32_t               yumi_client_connected_peers(const yumi_client_t *c);
bool                   yumi_client_is_verified(const yumi_client_t *c);
uint16_t               yumi_client_get_port(const yumi_client_t *c);

#ifdef __cplusplus
}
#endif

#endif /* YUMI_CLIENT_H */
