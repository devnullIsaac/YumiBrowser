/*
    Yumi SDK — Dashboard-Privileged IPC Imports
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

#ifndef WASM_DASHBOARD_H
#define WASM_DASHBOARD_H

/**
 * @file wasm_dashboard.h
 * @brief Guest-side IPC imports reserved for the dashboard webapp.
 *
 * This header carries the privileged surface that only the dashboard
 * webapp itself uses: group enumeration / switching, Group Registrar
 * (GR) proxy verbs, audit log queries, behaviour analysis, and the IP
 * blocklist.  These imports are available to any webapp instance but
 * only make operational sense for the dashboard supervisor running in
 * a user's foreground group.
 *
 * Everything a regular (non-dashboard) webapp needs to talk to the
 * dashboard — clipboard, file dialogs, friends, notifications, shared
 * link buffer, self-identity queries — lives in @c wasm_webapp.h and
 * is deliberately **not** pulled in from here: a dashboard webapp
 * does not consume the webapp→dashboard request surface, and the
 * host runtime refuses to bind those imports for a dashboard slot.
 *
 * Likewise, peer-to-peer networking (@c wasm_network.h) is
 * unavailable to the dashboard — the dashboard orchestrates groups
 * but does not itself participate in peer traffic.  A dashboard
 * webapp that imports @c net_* symbols will fail to instantiate.
 *
 * No guest-export callbacks are declared in this header.  Host → guest
 * callbacks (for example @c on_db_quota_changed, @c on_group_switched,
 * @c on_group_background_changed, @c on_gr_change,
 * @c on_gr_behavior_alert) are documented and stubbed in the template
 * files under @c sdk/templates/.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Shared identifier types                                            */
/* ================================================================== */
/*
 * These three opaque identifiers are shared with wasm_webapp.h and
 * wasm_network.h.  Guarded by the same macros each of those headers
 * uses so multiple inclusions are safe even when a translation unit
 * pulls in two of them.
 */

#ifndef YUMI_PEER_ID_LEN
#define YUMI_PEER_ID_LEN 32
#endif
#ifndef YUMI_GROUP_ID_LEN
#define YUMI_GROUP_ID_LEN 32
#endif
#ifndef YUMI_WEBAPP_ID_LEN
#define YUMI_WEBAPP_ID_LEN 16
#endif

#ifndef YUMI_GROUP_ID_DEFINED
#define YUMI_GROUP_ID_DEFINED
/** Opaque group identifier. */
typedef struct YumiGroupId
{
    uint8_t bytes[YUMI_GROUP_ID_LEN];
} YumiGroupId;
#endif

#ifndef YUMI_WEBAPP_ID_DEFINED
#define YUMI_WEBAPP_ID_DEFINED
/** Opaque webapp identifier — stable across restarts. */
typedef struct YumiWebAppId
{
    uint8_t bytes[YUMI_WEBAPP_ID_LEN];
} YumiWebAppId;
#endif

#ifndef YUMI_PEER_ID_DEFINED
#define YUMI_PEER_ID_DEFINED
/** Peer identity — 32-byte opaque identifier. */
typedef struct YumiPeerId
{
    union
    {
        uint8_t bytes[YUMI_PEER_ID_LEN];
        struct
        {
            uint64_t a;
            uint64_t b;
            uint64_t c;
            uint64_t d;
        };
    };
} YumiPeerId;
#endif

/* ================================================================== */
/*  Group / WebApp management                                          */
/* ================================================================== */
/*
 * These interfaces let the dashboard webapp enumerate and manage
 * *other* webapps and groups that are visible to it.  Four things a
 * caller can do:
 *
 *   1. Discover its own identity (which group it's in, which webapp
 *      it is) via @ref dashboard_self_webapp_id /
 *      @ref dashboard_self_group_id (in wasm_webapp.h), and which
 *      webapps are allowed / currently running in a given group.
 *   2. List all groups the user has joined and switch the dashboard's
 *      foreground group.
 *   3. Open or close the Group Registrar's database access for a
 *      group, and mark a group as "background" (running but not
 *      visible / de-prioritised).
 *   4. Read and write the DuckDB disk quota for *any* webapp inside
 *      a group, not just for itself.
 *
 * Access control: the host decides whether a given caller is
 * permitted to target groups or webapps other than its own.  A call
 * that is rejected returns a negative error code (for int32_t
 * returns) or zeroes out the destination (for write-to-pointer
 * returns) and leaves host state unchanged.
 *
 * All "list" functions follow the two-call convention:
 *   - Pass `out_ptr = NULL` / `out_cap = 0` to query the required
 *     number of entries.
 *   - Allocate, then call again with the sized buffer.
 * The return value is always the true total entry count, even when
 * the buffer was too small.
 */

/** Group flags — returned in YumiGroupInfo.flags. */
#define YUMI_GROUP_FLAG_CONNECTED   (1u << 0)
#define YUMI_GROUP_FLAG_BACKGROUND  (1u << 1)
#define YUMI_GROUP_FLAG_RECOVERY    (1u << 2)
#define YUMI_GROUP_FLAG_DB_OPEN     (1u << 3)

/** WebApp flags — returned in YumiWebAppInfo.flags. */
#define YUMI_WEBAPP_FLAG_ALLOWED    (1u << 0)   /**< Permitted by the group manifest. */
#define YUMI_WEBAPP_FLAG_RUNNING    (1u << 1)   /**< Currently instantiated in a slot. */
#define YUMI_WEBAPP_FLAG_BACKGROUND (1u << 2)   /**< Running but not the focused slot. */
#define YUMI_WEBAPP_FLAG_SELF       (1u << 3)   /**< Same webapp as the caller. */

/** Per-group summary record. */
typedef struct YumiGroupInfo
{
    YumiGroupId id;
    uint32_t    state;          /**< Host-defined GroupState enum value. */
    uint32_t    flags;          /**< YUMI_GROUP_FLAG_* bits. */
    uint32_t    member_count;   /**< Last-known member count. */
    uint32_t    webapp_count;   /**< Webapps currently running in this group. */
    uint8_t     name[64];       /**< UTF-8, NUL-padded. */
} YumiGroupInfo;

/** Per-webapp summary record. */
typedef struct YumiWebAppInfo
{
    YumiWebAppId id;
    uint32_t     flags;          /**< YUMI_WEBAPP_FLAG_* bits. */
    uint32_t     slot_index;     /**< Tiling slot (0xFFFFFFFF if not running). */
    uint64_t     db_size;        /**< Current DuckDB on-disk size (bytes). */
    uint64_t     db_quota;       /**< Current DuckDB disk quota (bytes, 0 => unlimited). */
    uint8_t      name[64];       /**< UTF-8, NUL-padded. */
} YumiWebAppInfo;

/** Sentinel "no limit" value for db_quota / set_db_quota. */
#define YUMI_DB_QUOTA_UNLIMITED  ((uint64_t)0)

/* ------------------------------------------------------------------ */
/*  Group enumeration and switching                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief List all groups the local user has joined.
 *
 * @param out_ptr  Pointer to an array of YumiGroupInfo, or NULL.
 * @param out_cap  Capacity of @p out_ptr (in entries).
 * @return         Total number of groups.  If the buffer was too
 *                 small, only `out_cap` entries are written but the
 *                 full count is returned.
 */
__attribute__((import_module("env"), import_name("dashboard_list_groups")))
int32_t dashboard_list_groups(YumiGroupInfo *out_ptr, uint32_t out_cap);

/**
 * @brief Write the currently focused (foreground) group's ID into
 *        @p out.
 *
 * Zeroed if no group is foreground.
 */
__attribute__((import_module("env"), import_name("dashboard_current_group")))
void dashboard_current_group(YumiGroupId *out);

/**
 * @brief Switch the dashboard's foreground group.
 *
 * The previous foreground group is marked BACKGROUND if it was
 * connected.  The target group is brought to the foreground and,
 * if its registrar DB is closed, opened.
 *
 * @param group_id  Target group.
 * @return 0 on success, negative on error (unknown group, access denied).
 */
__attribute__((import_module("env"), import_name("dashboard_switch_group")))
int32_t dashboard_switch_group(const YumiGroupId *group_id);

/* ------------------------------------------------------------------ */
/*  Group Registrar DB access + background mode                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Open the Group Registrar's database for the given group.
 *
 * Connects the registrar (if not already connected), loads the
 * registrar DB, and re-arms all webapp DuckDB bindings that belong
 * to the group.
 *
 * @return 0 on success, negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_group_open")))
int32_t dashboard_group_open(const YumiGroupId *group_id);

/**
 * @brief Close the Group Registrar's database for the given group.
 *
 * Disconnects any running webapps in the group, flushes their
 * DuckDB files, and releases the registrar connection.  The group
 * remains known to the dashboard and can be reopened later.
 *
 * @return 0 on success, negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_group_close")))
int32_t dashboard_group_close(const YumiGroupId *group_id);

/**
 * @brief Toggle the group's background mode.
 *
 * A "background" group keeps its network client running (reconnects,
 * rebroadcast duty, notifications) but its webapps are not
 * composited onto the swapchain and their `frame` export is skipped.
 * Useful when the user is focused on another group but still wants
 * to receive traffic for this one.
 *
 * @param background  Non-zero to mark background, zero to clear.
 * @return 0 on success, negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_group_set_background")))
int32_t dashboard_group_set_background(const YumiGroupId *group_id,
                                       uint32_t background);

/* ------------------------------------------------------------------ */
/*  WebApp discovery                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief List webapps available to a group.
 *
 * Returns both allowed (per the group manifest) and currently
 * running webapps.  Check the YUMI_WEBAPP_FLAG_* bits to tell which
 * is which.  If @p group_id is NULL, lists webapps for the caller's
 * own group.
 *
 * @param group_id   Target group, or NULL for self.
 * @param out_ptr    Destination buffer, or NULL to query length.
 * @param out_cap    Capacity of @p out_ptr (in entries).
 * @return           Total entry count.
 */
__attribute__((import_module("env"), import_name("dashboard_list_webapps")))
int32_t dashboard_list_webapps(const YumiGroupId *group_id,
                               YumiWebAppInfo *out_ptr,
                               uint32_t out_cap);

/* ------------------------------------------------------------------ */
/*  Per-webapp DuckDB quota (targeted)                                 */
/* ------------------------------------------------------------------ */
/*
 * These operate on any webapp inside any group the caller can see.
 * Passing NULL for @p group_id means "the caller's own group"; passing
 * NULL for @p app means "the caller itself".
 *
 * Size / quota returns are signed int64 so the special value -1 can
 * surface errors (unknown target, permission denied).  On success the
 * value fits in a uint64_t.
 */

/**
 * @brief Current on-disk size of the target webapp's DuckDB file.
 * @return Bytes (>=0), or -1 on error.
 */
__attribute__((import_module("env"), import_name("dashboard_webapp_db_size")))
int64_t dashboard_webapp_db_size(const YumiGroupId *group_id,
                                 const YumiWebAppId *app);

/**
 * @brief Current disk quota of the target webapp.
 * @return Bytes (0 => unlimited), or -1 on error.
 */
__attribute__((import_module("env"), import_name("dashboard_webapp_db_quota")))
int64_t dashboard_webapp_db_quota(const YumiGroupId *group_id,
                                  const YumiWebAppId *app);

/**
 * @brief Set the disk quota of the target webapp.
 *
 * The host clamps the request to a policy ceiling before applying.
 * The target webapp is notified via its optional
 * `on_db_quota_changed(uint64_t new_quota)` export.
 *
 * @param quota_bytes  Desired ceiling (YUMI_DB_QUOTA_UNLIMITED for none).
 * @return 0 on success, negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_webapp_set_db_quota")))
int32_t dashboard_webapp_set_db_quota(const YumiGroupId *group_id,
                                      const YumiWebAppId *app,
                                      uint64_t quota_bytes);

/* ================================================================== */
/*  Group Registrar proxy                                              */
/* ================================================================== */
/*
 * The dashboard exposes a surface-level proxy over the underlying
 * Group Registrar (see include/group_registrar.h).  Only user-facing
 * fields are surfaced — secret key material (ML-DSA signing secret
 * keys, ML-KEM secret keys, epoch keys, etc.) is never crossed into
 * guest memory.  Write-side verbs are signed by the dashboard using
 * the caller's *group identity*; the webapp itself never touches the
 * private key.
 *
 * Verbs intentionally omitted:
 *   - identity generate / derive / wipe
 *   - gr_open / gr_close (lifecycle is dashboard-owned)
 *   - epoch_get_current / epoch_get / epoch_list / epoch_count
 *     (the *current* symmetric group key must never leak into a
 *      webapp — only the epoch *rotate* verb is exposed)
 *   - raw sign_data / verify_data / encrypt / decrypt
 *   - peer_add (the group key agreement protocol owns that edge)
 *
 * All verbs take a `group_id` as the first argument; passing NULL
 * selects the caller's own group.  All integer returns use 0 for
 * success and a negative value for error.  All "list" functions use
 * the two-call convention (pass NULL/0 to query the total count,
 * then allocate and call again).
 */

/** Length constants mirrored from group_registrar.h. */
#define YUMI_SERVICE_HASH_LEN   128   /**< Skein-1024 service hash.        */
#define YUMI_VERIFY_TOKEN_LEN   128   /**< Skein-1024 invite token length. */
#define YUMI_GROUP_HASH_LEN     128   /**< Skein-1024 group/id hash.       */
#define YUMI_SIGN_PK_LEN        2592  /**< ML-DSA-87 public signing key.   */
#define YUMI_KEM_PK_LEN         1568  /**< ML-KEM-1024 public key.         */

/** Peer status filter values (match gr_peer_status_t). */
#define YUMI_PEER_STATUS_ACTIVE   0
#define YUMI_PEER_STATUS_KICKED   1
#define YUMI_PEER_STATUS_BANNED   2
#define YUMI_PEER_STATUS_LEFT     3
#define YUMI_PEER_STATUS_ANY      (-1)

/** Server types (match gr_server_type_t). */
#define YUMI_SERVER_SIGNALING     0
#define YUMI_SERVER_REBROADCAST   1

/** Group type (match gr_group_type_t). */
#define YUMI_GROUP_TYPE_PRIVATE   0
#define YUMI_GROUP_TYPE_PUBLIC    1

/** Permission bits (mirror gr_permission_t). */
#define YUMI_PERM_NONE             0u
#define YUMI_PERM_KICK_MEMBER      (1u << 0)
#define YUMI_PERM_BAN_MEMBER       (1u << 1)
#define YUMI_PERM_INVITE_MEMBER    (1u << 2)
#define YUMI_PERM_ADD_WEBAPP       (1u << 3)
#define YUMI_PERM_REMOVE_WEBAPP    (1u << 4)
#define YUMI_PERM_EDIT_ROLES       (1u << 5)
#define YUMI_PERM_EDIT_RETENTION   (1u << 6)
#define YUMI_PERM_EDIT_SERVERS     (1u << 7)
#define YUMI_PERM_ROTATE_EPOCH     (1u << 8)
#define YUMI_PERM_SIGN_REGISTRAR   (1u << 9)
#define YUMI_PERM_SET_GROUP_ICON   (1u << 10)
#define YUMI_PERM_OWNER            0xFFFFFFFFu

/* ── User-facing data records (strict wire layouts) ──────────────── */

/** Public peer record.  Secret keys are never included. */
typedef struct YumiPeerInfo
{
    YumiPeerId peer_id;              /*   0 */
    uint32_t   status;               /*  32  YUMI_PEER_STATUS_* */
    uint32_t   role_id;              /*  36 */
    int64_t    joined_at;            /*  40 */
    int64_t    removed_at;           /*  48 */
    int64_t    last_seen;            /*  56 */
    YumiPeerId removed_by;           /*  64 */
    uint8_t    ip[48];               /*  96 */
    uint16_t   port;                 /* 144 */
    uint8_t    _pad[6];              /* 146 */
    uint8_t    removed_reason[128];  /* 152 */
} YumiPeerInfo;                      /* = 280 */
#define YUMI_PEER_INFO_SIZE          280

/** Role descriptor.  ML-DSA signing key omitted. */
typedef struct YumiRoleInfo
{
    uint32_t role_id;                /*   0 */
    uint32_t permissions;            /*   4 */
    int64_t  created_at;             /*   8 */
    int64_t  modified_at;            /*  16 */
    uint8_t  name[128];              /*  24 */
} YumiRoleInfo;                      /* = 152 */
#define YUMI_ROLE_INFO_SIZE          152

/** Group-registrar webapp manifest entry (NOT slot info). */
typedef struct YumiGroupWebApp
{
    uint8_t    hash[YUMI_SERVICE_HASH_LEN];  /*   0  Skein-1024 service hash */
    uint32_t   version;                      /* 128 */
    uint8_t    _pad[4];                      /* 132 */
    int64_t    added_at;                     /* 136 */
    YumiPeerId added_by;                     /* 144 */
    uint8_t    name[128];                    /* 176 */
} YumiGroupWebApp;                           /* = 304 */
#define YUMI_GROUP_WEBAPP_SIZE       304

/** Server entry.  Only public-key material is carried. */
typedef struct YumiGroupServer
{
    uint32_t type;                              /*    0 */
    uint8_t  _pad[4];                           /*    4 */
    uint8_t  ip[48];                            /*    8 */
    uint16_t port;                              /*   56 */
    uint8_t  _pad2[6];                          /*   58 */
    uint8_t  id_hash[YUMI_GROUP_HASH_LEN];      /*   64  Skein-1024 fingerprint */
    uint8_t  sign_key[YUMI_SIGN_PK_LEN];        /*  192  ML-DSA-87 pk */
    uint8_t  service_hash[YUMI_SERVICE_HASH_LEN];/* 2784 */
    uint8_t  content_kem_pk[YUMI_KEM_PK_LEN];   /* 2912  ML-KEM-1024 pk */
} YumiGroupServer;                              /* = 4480 */
#define YUMI_GROUP_SERVER_SIZE       4480

/** Invite status snapshot. */
typedef struct YumiInviteInfo
{
    uint8_t    verification_token[YUMI_VERIFY_TOKEN_LEN]; /*   0 */
    int64_t    created_at;                                /* 128 */
    int64_t    expires_at;                                /* 136 */
    YumiPeerId created_by;                                /* 144 */
    YumiPeerId used_by;                                   /* 176 */
    uint32_t   invalidated;                               /* 208 */
    uint32_t   used;                                      /* 212 */
} YumiInviteInfo;                                         /* = 216 */
#define YUMI_INVITE_INFO_SIZE        216

/** Parsed invite ticket.  Bootstrap peer list not surfaced — the
 *  dashboard handles the join handshake. */
typedef struct YumiInviteTicketInfo
{
    YumiGroupId group_id;                                 /*   0 */
    uint32_t    group_type;                               /*  32 */
    uint32_t    bootstrap_count;                          /*  36 */
    uint32_t    signaling_count;                          /*  40 */
    uint32_t    _pad;                                     /*  44 */
    int64_t     expires_at;                               /*  48 */
    uint8_t     verification_token[YUMI_VERIFY_TOKEN_LEN];/*  56 */
    uint8_t     group_name[128];                          /* 184 */
} YumiInviteTicketInfo;                                   /* = 312 */
#define YUMI_INVITE_TICKET_SIZE      312

/** Retention policy. */
typedef struct YumiGroupRetention
{
    int64_t message_retention_ms;    /*   0 */
    int64_t file_retention_ms;       /*   8 */
    int64_t registrar_max_bytes;     /*  16 */
} YumiGroupRetention;                /* = 24 */
#define YUMI_GROUP_RETENTION_SIZE    24

/** Group-icon metadata (data / static-frame blobs returned separately). */
typedef struct YumiGroupIconInfo
{
    uint32_t    data_len;                                 /*   0 */
    uint32_t    static_frame_len;                         /*   4 */
    uint16_t    width;                                    /*   8 */
    uint16_t    height;                                   /*  10 */
    uint32_t    is_video;                                 /*  12 */
    int64_t     updated_at;                               /*  16 */
    uint8_t     content_hash[YUMI_GROUP_HASH_LEN];        /*  24 */
    YumiPeerId  updated_by;                               /* 152 */
    uint8_t     mime_type[64];                            /* 184 */
} YumiGroupIconInfo;                                      /* = 248 */
#define YUMI_GROUP_ICON_INFO_SIZE    248

/* ── Peer management ─────────────────────────────────────────────── */

__attribute__((import_module("env"), import_name("dashboard_peer_kick")))
int32_t dashboard_peer_kick(const YumiGroupId *group_id,
                            const YumiPeerId  *peer,
                            const void *reason_ptr, uint32_t reason_len);

__attribute__((import_module("env"), import_name("dashboard_peer_ban")))
int32_t dashboard_peer_ban(const YumiGroupId *group_id,
                           const YumiPeerId  *peer,
                           const void *reason_ptr, uint32_t reason_len);

__attribute__((import_module("env"), import_name("dashboard_peer_touch")))
int32_t dashboard_peer_touch(const YumiGroupId *group_id,
                             const YumiPeerId  *peer);

__attribute__((import_module("env"), import_name("dashboard_peer_set_role")))
int32_t dashboard_peer_set_role(const YumiGroupId *group_id,
                                const YumiPeerId  *peer,
                                uint32_t role_id);

__attribute__((import_module("env"), import_name("dashboard_peer_get")))
int32_t dashboard_peer_get(const YumiGroupId *group_id,
                           const YumiPeerId  *peer,
                           YumiPeerInfo *out);

/** Returns count on success, negative on error. */
__attribute__((import_module("env"), import_name("dashboard_peer_count")))
int32_t dashboard_peer_count(const YumiGroupId *group_id,
                             int32_t status_filter);

/** Two-call listing.  Returns total count (may exceed @p out_cap). */
__attribute__((import_module("env"), import_name("dashboard_peer_list")))
int32_t dashboard_peer_list(const YumiGroupId *group_id,
                            int32_t status_filter,
                            YumiPeerInfo *out, uint32_t out_cap);

/** Returns 1 if authorized, 0 if not, negative on error. */
__attribute__((import_module("env"), import_name("dashboard_peer_is_authorized")))
int32_t dashboard_peer_is_authorized(const YumiGroupId *group_id,
                                     const YumiPeerId  *peer);

/* ── Role management ─────────────────────────────────────────────── */

__attribute__((import_module("env"), import_name("dashboard_role_add")))
int32_t dashboard_role_add(const YumiGroupId *group_id,
                           const void *name_ptr, uint32_t name_len,
                           uint32_t permissions,
                           uint32_t *role_id_out);

__attribute__((import_module("env"), import_name("dashboard_role_remove")))
int32_t dashboard_role_remove(const YumiGroupId *group_id, uint32_t role_id);

__attribute__((import_module("env"), import_name("dashboard_role_set_permissions")))
int32_t dashboard_role_set_permissions(const YumiGroupId *group_id,
                                       uint32_t role_id,
                                       uint32_t permissions);

__attribute__((import_module("env"), import_name("dashboard_role_get")))
int32_t dashboard_role_get(const YumiGroupId *group_id,
                           uint32_t role_id,
                           YumiRoleInfo *out);

__attribute__((import_module("env"), import_name("dashboard_role_list")))
int32_t dashboard_role_list(const YumiGroupId *group_id,
                            YumiRoleInfo *out, uint32_t out_cap);

__attribute__((import_module("env"), import_name("dashboard_role_count")))
int32_t dashboard_role_count(const YumiGroupId *group_id);

/** Returns 1 if the caller holds @p perm, 0 if not, negative on error. */
__attribute__((import_module("env"), import_name("dashboard_has_permission")))
int32_t dashboard_has_permission(const YumiGroupId *group_id, uint32_t perm);

/* ── Group-registrar webapp manifest ─────────────────────────────── */

__attribute__((import_module("env"), import_name("dashboard_group_webapp_add")))
int32_t dashboard_group_webapp_add(const YumiGroupId *group_id,
                                   const YumiGroupWebApp *webapp);

__attribute__((import_module("env"), import_name("dashboard_group_webapp_remove")))
int32_t dashboard_group_webapp_remove(const YumiGroupId *group_id,
                                      const uint8_t hash[YUMI_SERVICE_HASH_LEN]);

__attribute__((import_module("env"), import_name("dashboard_group_webapp_is_authorized")))
int32_t dashboard_group_webapp_is_authorized(const YumiGroupId *group_id,
                                             const uint8_t hash[YUMI_SERVICE_HASH_LEN]);

__attribute__((import_module("env"), import_name("dashboard_group_webapp_list")))
int32_t dashboard_group_webapp_list(const YumiGroupId *group_id,
                                    YumiGroupWebApp *out, uint32_t out_cap);

__attribute__((import_module("env"), import_name("dashboard_group_webapp_count")))
int32_t dashboard_group_webapp_count(const YumiGroupId *group_id);

/* ── Server management ───────────────────────────────────────────── */

__attribute__((import_module("env"), import_name("dashboard_server_add")))
int32_t dashboard_server_add(const YumiGroupId *group_id,
                             const YumiGroupServer *server);

__attribute__((import_module("env"), import_name("dashboard_server_remove")))
int32_t dashboard_server_remove(const YumiGroupId *group_id,
                                const uint8_t id_hash[YUMI_GROUP_HASH_LEN]);

__attribute__((import_module("env"), import_name("dashboard_server_list")))
int32_t dashboard_server_list(const YumiGroupId *group_id,
                              uint32_t type,
                              YumiGroupServer *out, uint32_t out_cap);

__attribute__((import_module("env"), import_name("dashboard_server_count")))
int32_t dashboard_server_count(const YumiGroupId *group_id, uint32_t type);

/* ── Epoch rotation (only rotation is exposed) ───────────────────── */

__attribute__((import_module("env"), import_name("dashboard_epoch_rotate")))
int32_t dashboard_epoch_rotate(const YumiGroupId *group_id);

/* ── Retention policy ────────────────────────────────────────────── */

__attribute__((import_module("env"), import_name("dashboard_retention_get")))
int32_t dashboard_retention_get(const YumiGroupId *group_id,
                                YumiGroupRetention *out);

__attribute__((import_module("env"), import_name("dashboard_retention_set")))
int32_t dashboard_retention_set(const YumiGroupId *group_id,
                                const YumiGroupRetention *policy);

/* ── Group icon ──────────────────────────────────────────────────── */

__attribute__((import_module("env"), import_name("dashboard_group_icon_set")))
int32_t dashboard_group_icon_set(const YumiGroupId *group_id,
                                 const void *data_ptr, uint32_t data_len,
                                 const void *mime_ptr, uint32_t mime_len,
                                 uint32_t width, uint32_t height,
                                 uint32_t is_video,
                                 const void *static_frame_ptr,
                                 uint32_t static_frame_len);

/**
 * @brief Fetch the group icon into guest-provided buffers.
 *
 * On success:
 *   - `info_out` is filled with the icon metadata (sizes, mime, dims).
 *   - Up to `data_cap` bytes of the main image payload are copied.
 *   - Up to `static_frame_cap` bytes of the optional still frame are copied.
 *
 * Call first with `data_cap = 0` / `static_frame_cap = 0` to read the
 * metadata and required buffer sizes, then call again with sized
 * buffers.  Returns 0 on success, negative on error or not-set.
 */
__attribute__((import_module("env"), import_name("dashboard_group_icon_get")))
int32_t dashboard_group_icon_get(const YumiGroupId *group_id,
                                 YumiGroupIconInfo *info_out,
                                 void *data_out, uint32_t data_cap,
                                 void *static_frame_out, uint32_t static_frame_cap);

__attribute__((import_module("env"), import_name("dashboard_group_icon_remove")))
int32_t dashboard_group_icon_remove(const YumiGroupId *group_id);

__attribute__((import_module("env"), import_name("dashboard_group_icon_hash")))
int32_t dashboard_group_icon_hash(const YumiGroupId *group_id,
                                  uint8_t hash_out[YUMI_GROUP_HASH_LEN]);

/* ── Invite management ───────────────────────────────────────────── */

/**
 * @brief Mint an invite ticket for an outsider.
 *
 * Fills @p verification_token_out with the full token (needed later by
 * @ref dashboard_invite_invalidate / @ref dashboard_invite_check).
 * Writes up to @p blob_cap bytes of the wire blob to @p blob_out.
 *
 * @return Actual blob length on success (may exceed @p blob_cap — call
 *         first with `blob_cap = 0` to size the buffer).  Negative on
 *         error / permission denied.
 */
__attribute__((import_module("env"), import_name("dashboard_invite_create")))
int32_t dashboard_invite_create(const YumiGroupId *group_id,
                                int64_t expiry_ms,
                                void *blob_out, uint32_t blob_cap,
                                uint8_t verification_token_out[YUMI_VERIFY_TOKEN_LEN]);

__attribute__((import_module("env"), import_name("dashboard_invite_invalidate")))
int32_t dashboard_invite_invalidate(const YumiGroupId *group_id,
                                    const uint8_t verification_token[YUMI_VERIFY_TOKEN_LEN]);

/** Returns 1 if valid, 0 if invalid/used/expired, negative on error. */
__attribute__((import_module("env"), import_name("dashboard_invite_check")))
int32_t dashboard_invite_check(const YumiGroupId *group_id,
                               const uint8_t verification_token[YUMI_VERIFY_TOKEN_LEN]);

__attribute__((import_module("env"), import_name("dashboard_invite_list")))
int32_t dashboard_invite_list(const YumiGroupId *group_id,
                              YumiInviteInfo *out, uint32_t out_cap);

__attribute__((import_module("env"), import_name("dashboard_invite_count")))
int32_t dashboard_invite_count(const YumiGroupId *group_id);

/** Parse an invite blob (no registrar lookup; works before join). */
__attribute__((import_module("env"), import_name("dashboard_invite_parse")))
int32_t dashboard_invite_parse(const void *blob_ptr, uint32_t blob_len,
                               YumiInviteTicketInfo *out);

/* ════════════════════════════════════════════════════════════════════
 *  Group Registrar: Audit log, Behavior analysis, IP blocklist
 * ────────────────────────────────────────────────────────────────────
 *  All three surfaces are strictly read-only from the guest.  Signed
 *  audit entries and behavioral thresholds are owned by the host —
 *  webapps can observe but never forge, replay, or reset them.
 *
 *  All "list" functions follow the two-call convention: pass NULL/0
 *  buffers first to retrieve the total count, then allocate and call
 *  again with the real buffer.
 * ════════════════════════════════════════════════════════════════════ */

/** @name Group-registrar change categories
 *
 *  A coarse-grained classification used when filtering audit entries.
 *  The mapping from `change_type` → category is fixed and matches the
 *  `gr_change_type_t` values.
 *  @{ */
#define YUMI_GR_CAT_UNKNOWN    0
#define YUMI_GR_CAT_PEER       1   /**< add/remove/kick/ban/leave/address/role-changed */
#define YUMI_GR_CAT_ROLE       2   /**< role add/remove/modified                      */
#define YUMI_GR_CAT_WEBAPP     3   /**< webapp manifest add/remove                    */
#define YUMI_GR_CAT_SERVER     4   /**< server add/remove                             */
#define YUMI_GR_CAT_EPOCH      5   /**< epoch rotation                                */
#define YUMI_GR_CAT_RETENTION  6   /**< retention policy change                       */
#define YUMI_GR_CAT_REGISTRAR  7   /**< registrar header re-sign                      */
#define YUMI_GR_CAT_INVITE     8   /**< invite create/use/invalidate                  */
#define YUMI_GR_CAT_ICON       9   /**< icon set/removed                              */
#define YUMI_GR_CAT_BLOCKLIST  10  /**< boot-IP blocklist entry recorded              */
/** @} */

/** @name `gr_change_type_t` mirror (use these when inspecting an audit entry).
 *  @{ */
#define YUMI_CHANGE_PEER_ADDED          1
#define YUMI_CHANGE_PEER_REMOVED        2
#define YUMI_CHANGE_PEER_KICKED         3
#define YUMI_CHANGE_PEER_BANNED         4
#define YUMI_CHANGE_PEER_ADDRESS        5
#define YUMI_CHANGE_PEER_ROLE_CHANGED   6
#define YUMI_CHANGE_ROLE_ADDED          7
#define YUMI_CHANGE_ROLE_REMOVED        8
#define YUMI_CHANGE_ROLE_MODIFIED       9
#define YUMI_CHANGE_WEBAPP_ADDED        10
#define YUMI_CHANGE_WEBAPP_REMOVED      11
#define YUMI_CHANGE_SERVER_ADDED        12
#define YUMI_CHANGE_SERVER_REMOVED      13
#define YUMI_CHANGE_EPOCH_ROTATED       14
#define YUMI_CHANGE_RETENTION_SET       15
#define YUMI_CHANGE_REGISTRAR_SIGNED    16
#define YUMI_CHANGE_INVITE_CREATED      17
#define YUMI_CHANGE_INVITE_USED         18
#define YUMI_CHANGE_INVITE_INVALIDATED  19
#define YUMI_CHANGE_GROUP_ICON_SET      20
#define YUMI_CHANGE_GROUP_ICON_REMOVED  21
#define YUMI_CHANGE_BOOT_IP_BLOCKED     22
/** @} */

/** @name Behavior alert bitmask (matches `gr_behavior_alert_t`).
 *  @{ */
#define YUMI_ALERT_NONE    0u
#define YUMI_ALERT_BURST   (1u << 0)   /**< actor exceeded actions/min threshold */
#define YUMI_ALERT_ABUSE   (1u << 1)   /**< actor destructive ratio too high     */
/** @} */

/* ── Audit log records ───────────────────────────────────────────── */

/** Audit entry as delivered to the guest.  The ML-DSA signature is
 *  **intentionally stripped** — verification is performed host-side
 *  via @ref dashboard_audit_verify_chain. */
typedef struct YumiAuditEntry
{
    uint8_t     entry_hash[YUMI_GROUP_HASH_LEN];  /*   0  Skein-1024 entry hash   */
    uint8_t     prev_hash[YUMI_GROUP_HASH_LEN];   /* 128  Skein-1024 prev hash    */
    int64_t     timestamp_ms;                     /* 256  unix ms                 */
    int64_t     timestamp_ns;                     /* 264  ns since boot (ordering)*/
    uint32_t    change_type;                      /* 272  YUMI_CHANGE_*           */
    uint32_t    registrar_version;                /* 276                          */
    YumiPeerId  actor_id;                         /* 280                          */
    YumiPeerId  target_id;                        /* 312                          */
    uint8_t     detail[256];                      /* 344  UTF-8, NUL-padded       */
} YumiAuditEntry;                                 /* = 600 */
#define YUMI_AUDIT_ENTRY_SIZE 600

/** Audit chain verification summary. */
typedef struct YumiAuditChainResult
{
    uint32_t total_entries;        /*   0 */
    uint32_t verified_entries;     /*   4 */
    uint32_t invalid_hash;         /*   8 */
    uint32_t invalid_signature;    /*  12 */
    uint32_t unknown_actor;        /*  16 */
    uint32_t forks_detected;       /*  20 */
    uint32_t forks_resolved;       /*  24 */
    uint32_t has_genesis;          /*  28  non-zero if a genesis entry exists */
} YumiAuditChainResult;             /* = 32 */
#define YUMI_AUDIT_CHAIN_RESULT_SIZE 32

/** Compact branch descriptor used inside @ref YumiAuditFork. */
typedef struct YumiAuditForkBranch
{
    uint8_t    entry_hash[YUMI_GROUP_HASH_LEN];  /*   0 */
    int64_t    timestamp_ms;                     /* 128 */
    uint32_t   change_type;                      /* 136 */
    uint32_t   _pad;                             /* 140 */
    YumiPeerId actor_id;                         /* 144 */
} YumiAuditForkBranch;                           /* = 176 */
#define YUMI_AUDIT_FORK_BRANCH_SIZE 176

/** Audit-chain fork descriptor (up to 8 competing branches at a point). */
#define YUMI_AUDIT_FORK_MAX_BRANCHES 8
typedef struct YumiAuditFork
{
    uint8_t             prev_hash[YUMI_GROUP_HASH_LEN];     /*    0 */
    uint32_t            branch_count;                       /*  128 */
    uint32_t            _pad;                               /*  132 */
    YumiAuditForkBranch branches[YUMI_AUDIT_FORK_MAX_BRANCHES]; /* 136 */
} YumiAuditFork;                                            /* = 1544 */
#define YUMI_AUDIT_FORK_SIZE 1544

/* ── Behavior analysis records ───────────────────────────────────── */

typedef struct YumiActorBurst
{
    uint32_t actions_in_window;    /*   0 */
    uint32_t destructive_actions;  /*   4  kicks + bans + removes */
    uint32_t role_changes;         /*   8 */
    uint32_t invites_created;      /*  12 */
    uint32_t epoch_rotations;      /*  16 */
    float    actions_per_minute;   /*  20 */
    uint32_t burst_detected;       /*  24  non-zero => above threshold */
    uint32_t _pad;                 /*  28 */
} YumiActorBurst;                  /* = 32 */
#define YUMI_ACTOR_BURST_SIZE 32

typedef struct YumiMutationRate
{
    uint32_t total_mutations;       /*   0 */
    uint32_t distinct_actors;       /*   4 */
    float    mutations_per_minute;  /*   8 */
    float    mutations_per_actor;   /*  12 */
    uint32_t swarm_detected;        /*  16 */
    uint32_t _pad;                  /*  20 */
} YumiMutationRate;                 /* = 24 */
#define YUMI_MUTATION_RATE_SIZE 24

typedef struct YumiAdminScore
{
    uint32_t total_admin_actions;    /*   0 */
    uint32_t kicks;                  /*   4 */
    uint32_t bans;                   /*   8 */
    uint32_t removes;                /*  12 */
    uint32_t role_modifications;     /*  16 */
    uint32_t permission_escalations; /*  20 */
    float    destructive_ratio;      /*  24 */
    uint32_t abuse_suspected;        /*  28 */
} YumiAdminScore;                    /* = 32 */
#define YUMI_ADMIN_SCORE_SIZE 32

typedef struct YumiPeerChurn
{
    uint32_t active_peers;        /*   0 */
    uint32_t kicked_peers;        /*   4 */
    uint32_t banned_peers;        /*   8 */
    uint32_t left_peers;          /*  12 */
    uint32_t joined_in_window;    /*  16 */
    uint32_t removed_in_window;   /*  20 */
    float    churn_rate;          /*  24 */
    uint32_t stale_count;         /*  28 */
} YumiPeerChurn;                  /* = 32 */
#define YUMI_PEER_CHURN_SIZE 32

typedef struct YumiDeltaScore
{
    uint64_t delta_bytes;              /*   0 */
    uint32_t entry_count;              /*   8 */
    uint32_t _pad;                     /*  12 */
    int64_t  offline_duration_ms;      /*  16 */
    float    bytes_per_offline_day;    /*  24 */
    float    entries_per_offline_day;  /*  28 */
    uint32_t anomalous;                /*  32 */
    uint32_t _pad2;                    /*  36 */
} YumiDeltaScore;                      /* = 40 */
#define YUMI_DELTA_SCORE_SIZE 40

typedef struct YumiEpochPattern
{
    uint32_t rotations_in_window;   /*   0 */
    uint32_t _pad;                  /*   4 */
    int64_t  avg_epoch_lifetime_ms; /*   8 */
    uint32_t excessive_rotation;    /*  16 */
    uint32_t _pad2;                 /*  20 */
} YumiEpochPattern;                 /* = 24 */
#define YUMI_EPOCH_PATTERN_SIZE 24

typedef struct YumiNetworkScore
{
    int64_t  last_update_ms;            /*   0 */
    int64_t  time_since_update_ms;      /*   8 */
    uint64_t estimated_registrar_bytes; /*  16 */
    uint32_t total_audit_entries;       /*  24 */
    float    avg_entry_interval_ms;     /*  28 */
} YumiNetworkScore;                     /* = 32 */
#define YUMI_NETWORK_SCORE_SIZE 32

/** Full group-health snapshot (@ref dashboard_behavior_snapshot). */
typedef struct YumiBehaviorSnapshot
{
    YumiPeerChurn     churn;             /*    0 */
    YumiMutationRate  mutation_rate;     /*   32 */
    YumiEpochPattern  epoch_pattern;     /*   56 */
    YumiNetworkScore  network;           /*   80 */
    YumiAdminScore    worst_admin;       /*  112 */
    YumiPeerId        worst_admin_id;    /*  144 */
    uint32_t          has_worst_admin;   /*  176 */
    uint32_t          _pad;              /*  180 */
    uint64_t          estimated_size;    /*  184 */
    uint32_t          needs_attention;   /*  192 */
    uint32_t          _pad2;             /*  196 */
} YumiBehaviorSnapshot;                  /* = 200 */
#define YUMI_BEHAVIOR_SNAPSHOT_SIZE 200

/** Behavior analysis configuration thresholds. */
typedef struct YumiBehaviorConfig
{
    float    burst_actions_per_min;         /*   0 */
    float    swarm_mutations_per_min;       /*   4 */
    float    abuse_destructive_ratio;       /*   8 */
    uint32_t abuse_min_actions;             /*  12 */
    float    epoch_max_per_hour;            /*  16 */
    float    delta_anomaly_entries_per_day; /*  20 */
    float    churn_attention_threshold;     /*  24 */
    uint32_t scale_by_group_size;           /*  28 */
    int64_t  alert_window_ms;               /*  32 */
} YumiBehaviorConfig;                       /* = 40 */
#define YUMI_BEHAVIOR_CONFIG_SIZE 40

/* ── IP blocklist record ─────────────────────────────────────────── */

typedef struct YumiBlocklistEntry
{
    uint8_t  ip[48];         /*   0  NUL-terminated IPv4/IPv6 string  */
    uint32_t fail_count;     /*  48 */
    uint32_t is_blocked;     /*  52  non-zero if currently blocked    */
    int64_t  blocked_at_ms;  /*  56  ms since epoch (0 if never)      */
    int64_t  first_seen_ms;  /*  64                                   */
} YumiBlocklistEntry;        /* = 72 */
#define YUMI_BLOCKLIST_ENTRY_SIZE 72

/* ── Audit API ───────────────────────────────────────────────────── */

/**
 * @brief List audit entries newer than a timestamp (two-call convention).
 *
 * @param group_id         Target group (NULL = caller's group).
 * @param since_ms         Unix-ms cutoff (0 = all).
 * @param out              Output array (may be NULL to size the buffer).
 * @param out_cap          Capacity of @p out in entries.
 * @return Total number of entries available, or negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_audit_list")))
int32_t dashboard_audit_list(const YumiGroupId *group_id,
                             int64_t since_ms,
                             YumiAuditEntry *out, uint32_t out_cap);

/** @return Total audit-log entry count, or negative on error. */
__attribute__((import_module("env"), import_name("dashboard_audit_count")))
int32_t dashboard_audit_count(const YumiGroupId *group_id);

/**
 * @brief Verify the hash/signature chain over the entire audit log.
 * Heavyweight — intended for diagnostics or scheduled sanity checks.
 * @return 0 on success (populates @p out), negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_audit_verify_chain")))
int32_t dashboard_audit_verify_chain(const YumiGroupId *group_id,
                                     YumiAuditChainResult *out);

/**
 * @brief List detected audit-chain forks (competing branches at one prev_hash).
 * Two-call convention.
 * @return Total fork count (may exceed @p out_cap), or negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_audit_list_forks")))
int32_t dashboard_audit_list_forks(const YumiGroupId *group_id,
                                   YumiAuditFork *out, uint32_t out_cap);

/** Classify a `change_type` (0..22) into a category (`YUMI_GR_CAT_*`).
 *  Pure guest helper — no host call. */
static inline uint32_t yumi_gr_category(uint32_t change_type) {
    switch (change_type) {
    case YUMI_CHANGE_PEER_ADDED: case YUMI_CHANGE_PEER_REMOVED:
    case YUMI_CHANGE_PEER_KICKED: case YUMI_CHANGE_PEER_BANNED:
    case YUMI_CHANGE_PEER_ADDRESS: case YUMI_CHANGE_PEER_ROLE_CHANGED:
        return YUMI_GR_CAT_PEER;
    case YUMI_CHANGE_ROLE_ADDED: case YUMI_CHANGE_ROLE_REMOVED:
    case YUMI_CHANGE_ROLE_MODIFIED:
        return YUMI_GR_CAT_ROLE;
    case YUMI_CHANGE_WEBAPP_ADDED: case YUMI_CHANGE_WEBAPP_REMOVED:
        return YUMI_GR_CAT_WEBAPP;
    case YUMI_CHANGE_SERVER_ADDED: case YUMI_CHANGE_SERVER_REMOVED:
        return YUMI_GR_CAT_SERVER;
    case YUMI_CHANGE_EPOCH_ROTATED:
        return YUMI_GR_CAT_EPOCH;
    case YUMI_CHANGE_RETENTION_SET:
        return YUMI_GR_CAT_RETENTION;
    case YUMI_CHANGE_REGISTRAR_SIGNED:
        return YUMI_GR_CAT_REGISTRAR;
    case YUMI_CHANGE_INVITE_CREATED: case YUMI_CHANGE_INVITE_USED:
    case YUMI_CHANGE_INVITE_INVALIDATED:
        return YUMI_GR_CAT_INVITE;
    case YUMI_CHANGE_GROUP_ICON_SET: case YUMI_CHANGE_GROUP_ICON_REMOVED:
        return YUMI_GR_CAT_ICON;
    case YUMI_CHANGE_BOOT_IP_BLOCKED:
        return YUMI_GR_CAT_BLOCKLIST;
    default:
        return YUMI_GR_CAT_UNKNOWN;
    }
}

/* ── Behavior API (all read-only except `set_config`) ────────────── */

/** @brief Burst activity for a specific actor over @p window_ms. */
__attribute__((import_module("env"), import_name("dashboard_behavior_actor_burst")))
int32_t dashboard_behavior_actor_burst(const YumiGroupId *group_id,
                                       const YumiPeerId  *actor,
                                       int64_t window_ms,
                                       YumiActorBurst *out);

/** @brief Group-wide mutation rate over @p window_ms. */
__attribute__((import_module("env"), import_name("dashboard_behavior_mutation_rate")))
int32_t dashboard_behavior_mutation_rate(const YumiGroupId *group_id,
                                         int64_t window_ms,
                                         YumiMutationRate *out);

/** @brief Admin/moderator abuse scoring for one actor over @p window_ms. */
__attribute__((import_module("env"), import_name("dashboard_behavior_admin_score")))
int32_t dashboard_behavior_admin_score(const YumiGroupId *group_id,
                                       const YumiPeerId  *admin,
                                       int64_t window_ms,
                                       YumiAdminScore *out);

/** @brief Peer churn (joins / leaves / kicks / bans) over @p window_ms. */
__attribute__((import_module("env"), import_name("dashboard_behavior_peer_churn")))
int32_t dashboard_behavior_peer_churn(const YumiGroupId *group_id,
                                      int64_t window_ms,
                                      YumiPeerChurn *out);

/** @brief Score an incoming delta for anomalies. */
__attribute__((import_module("env"), import_name("dashboard_behavior_delta_score")))
int32_t dashboard_behavior_delta_score(const YumiGroupId *group_id,
                                       uint64_t delta_bytes,
                                       uint32_t entry_count,
                                       YumiDeltaScore *out);

/**
 * @brief List peers not seen within @p stale_threshold_ms.
 * Two-call convention.  Writes up to @p max_count × @ref YUMI_PEER_ID_LEN bytes.
 * @return Number of stale peers written (may be less than total), negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_behavior_stale_peers")))
int32_t dashboard_behavior_stale_peers(const YumiGroupId *group_id,
                                       int64_t stale_threshold_ms,
                                       YumiPeerId *out, uint32_t max_count);

/** @brief Epoch-rotation cadence analysis over @p window_ms. */
__attribute__((import_module("env"), import_name("dashboard_behavior_epoch_pattern")))
int32_t dashboard_behavior_epoch_pattern(const YumiGroupId *group_id,
                                         int64_t window_ms,
                                         YumiEpochPattern *out);

/** @brief Overall network / sync health score. */
__attribute__((import_module("env"), import_name("dashboard_behavior_network_score")))
int32_t dashboard_behavior_network_score(const YumiGroupId *group_id,
                                         YumiNetworkScore *out);

/** @brief Composite group-health snapshot (churn + mutations + epoch + net + worst admin). */
__attribute__((import_module("env"), import_name("dashboard_behavior_snapshot")))
int32_t dashboard_behavior_snapshot(const YumiGroupId *group_id,
                                    int64_t window_ms,
                                    YumiBehaviorSnapshot *out);

/** @brief Read current behavior-analysis thresholds. */
__attribute__((import_module("env"), import_name("dashboard_behavior_get_config")))
int32_t dashboard_behavior_get_config(const YumiGroupId *group_id,
                                      YumiBehaviorConfig *out);

/**
 * @brief Update behavior-analysis thresholds (requires moderator permission).
 *
 * Not signed into the audit log — thresholds are local detection knobs, not
 * group-consensus state.
 */
__attribute__((import_module("env"), import_name("dashboard_behavior_set_config")))
int32_t dashboard_behavior_set_config(const YumiGroupId *group_id,
                                      const YumiBehaviorConfig *config);

/* ── IP blocklist API ────────────────────────────────────────────── */

/**
 * @brief Check whether a bootstrap/connect IP is currently blocked.
 *
 * Expired blocks (older than @p block_duration_ms) are auto-cleaned.
 *
 * @return 1 if blocked, 0 if not, negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_blocklist_check")))
int32_t dashboard_blocklist_check(const YumiGroupId *group_id,
                                  const void *ip_ptr, uint32_t ip_len,
                                  int64_t block_duration_ms);

/**
 * @brief Record an authentication failure for @p ip.
 *
 * When the running fail count reaches @p max_fails the IP is locked
 * and a @ref YUMI_CHANGE_BOOT_IP_BLOCKED entry is written to the
 * audit log.
 *
 * @return 1 if this call caused the block, 0 otherwise, negative on error.
 */
__attribute__((import_module("env"), import_name("dashboard_blocklist_record_fail")))
int32_t dashboard_blocklist_record_fail(const YumiGroupId *group_id,
                                        const void *ip_ptr, uint32_t ip_len,
                                        int32_t max_fails);

/** @brief Clear the fail counter for @p ip (e.g. after a successful auth). */
__attribute__((import_module("env"), import_name("dashboard_blocklist_reset")))
int32_t dashboard_blocklist_reset(const YumiGroupId *group_id,
                                  const void *ip_ptr, uint32_t ip_len);

/** @brief Delete expired block entries from the backing store. */
__attribute__((import_module("env"), import_name("dashboard_blocklist_cleanup")))
int32_t dashboard_blocklist_cleanup(const YumiGroupId *group_id,
                                    int64_t block_duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* WASM_DASHBOARD_H */
