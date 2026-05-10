/*
    Yumi SDK — Peer-to-Peer Networking WASM Imports
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

#ifndef WASM_NETWORK_H
#define WASM_NETWORK_H

/**
 * @file wasm_network.h
 * @brief WebAssembly guest imports for peer-to-peer networking.
 *
 * @details
 * This header exposes the Yumi peer-to-peer transport to WASM webapps.
 * The webapp never sees IP addresses, ports, or sockets — only
 * **peer IDs**, which are 32-byte opaque identifiers that remain stable
 * across network reconnects, NAT rebindings, and rebroadcaster relays.
 *
 * The runtime handles all routing decisions:
 *
 *   - Direct peer-to-peer sends take the most direct available path
 *     (LAN, ICE-negotiated, or via a rebroadcaster server).
 *   - Broadcasts fan out to every known peer in the current group.
 *     If a rebroadcaster server is allocated for the group the runtime
 *     will forward through it instead of flooding.
 *   - "Data shards" are long-retention blobs handed off to the
 *     rebroadcaster or to peers willing to store them. Shard uploads
 *     are validated against the Group Registrar's quota before being
 *     accepted.
 *
 * ## Two transport categories
 *
 * | Category     | Latency  | Retention          | API |
 * |--------------|----------|--------------------|-----|
 * | Peer message | Low      | Ephemeral (in RAM) | `net_send*`, `net_broadcast` |
 * | Data shard   | Higher   | Group-configured   | `net_shard_*` |
 *
 * Peer messages are for interactive traffic — chat keystrokes, cursor
 * updates, game state. Shards are for anything that should survive the
 * sender going offline — file attachments, signed announcements,
 * snapshots.
 *
 * ## Async model
 *
 * Every receive path is a callback, exported by the guest. The host
 * invokes them between frames, not inside them; you can safely touch
 * guest state from the callbacks without reentrancy worries. Outbound
 * calls (`net_send`, `net_broadcast`, `net_shard_upload`) are
 * non-blocking: they hand the data to the runtime and return.
 *
 * @code
 *   // Send a chat message to everyone in the group.
 *   const char *msg = "hello";
 *   net_broadcast(msg, 5);
 *
 *   // Upload a file for longer retention.
 *   uint64_t sid = net_shard_upload(file_ptr, file_len,
 *                                   NET_SHARD_KIND_BLOB,
 *                                   60 * 60 * 24);   // 1 day TTL
 *
 *   // Later, from another webapp:
 *   uint32_t h = net_shard_request(sid);
 *   // ... on_net_shard_chunk(h, ...) will fire until is_final == 1.
 * @endcode
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Peer ID length in bytes. Matches the host's GR_PEER_ID_LEN. */
#ifndef YUMI_PEER_ID_LEN
#define YUMI_PEER_ID_LEN 32
#endif

/**
 * @brief Opaque 32-byte peer identifier.
 *
 * Peer IDs are stable across reconnects and routing changes. They are
 * the *only* way a webapp may refer to another endpoint — IP and port
 * are never exposed to the guest.
 */
#ifndef YUMI_PEER_ID_DEFINED
#define YUMI_PEER_ID_DEFINED
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
/*  Enums                                                              */
/* ================================================================== */

/**
 * @brief Classification for data shards.
 *
 * The runtime forwards this to the Group Registrar when deciding
 * whether an upload fits within the group's quota policy.
 */
typedef enum net_shard_kind_e
{
    NET_SHARD_KIND_BLOB        = 0, /**< Generic opaque bytes. */
    NET_SHARD_KIND_IMAGE       = 1, /**< Image content. */
    NET_SHARD_KIND_VIDEO       = 2, /**< Video content. */
    NET_SHARD_KIND_AUDIO       = 3, /**< Audio content. */
    NET_SHARD_KIND_DOCUMENT    = 4, /**< Text/document content. */
    NET_SHARD_KIND_APP_PRIVATE = 5, /**< Webapp-defined format. */
} net_shard_kind_t;

/**
 * @brief Status returned to `on_net_shard_result` for a completed request.
 */
typedef enum net_shard_status_e
{
    NET_SHARD_OK          = 0, /**< Delivery complete. */
    NET_SHARD_NOT_FOUND   = 1, /**< No peer or rebroadcaster has this shard. */
    NET_SHARD_REJECTED    = 2, /**< Group Registrar denied the request. */
    NET_SHARD_EXPIRED     = 3, /**< Shard TTL elapsed before retrieval. */
    NET_SHARD_CANCELLED   = 4, /**< Caller cancelled via `net_shard_cancel`. */
    NET_SHARD_IO_ERROR    = 5, /**< Transport failure, partial data possible. */
} net_shard_status_t;

/* ================================================================== */
/*  Outbound: peer list                                                */
/* ================================================================== */

/**
 * @brief Return the number of peers currently known to this webapp.
 *
 * "Known" means the runtime has an active or recently-active transport
 * to them and they are members of the same group. Peers that are
 * merely reachable via a rebroadcaster still count.
 *
 * @return Number of peers, or 0 if the webapp is not in a group.
 */
__attribute__((import_module("env"), import_name("net_peer_count")))
uint32_t net_peer_count(void);

/**
 * @brief Copy the current peer list into a guest-provided array.
 *
 * Peers are returned in no particular order. The order may change
 * between calls; do not rely on it.
 *
 * @param out_peers  Array of `out_cap` `YumiPeerId` slots to fill.
 * @param out_cap    Capacity of @p out_peers in elements (not bytes).
 * @return           Number of peer IDs actually written. If the return
 *                   value equals @p out_cap the list may have been
 *                   truncated; call `net_peer_count` first or retry
 *                   with a larger buffer.
 */
__attribute__((import_module("env"), import_name("net_peer_list")))
uint32_t net_peer_list(YumiPeerId *out_peers, uint32_t out_cap);

/* ================================================================== */
/*  Outbound: peer messages                                            */
/* ================================================================== */

/**
 * @brief Send a reliable ephemeral message to a single peer.
 *
 * The message is delivered over the most direct transport available
 * with retransmits until acknowledged (or the underlying transport
 * gives up). Ordering is preserved relative to other @c net_send
 * calls to the same peer but *not* against broadcasts — if you need
 * stricter ordering, build it above this call with your own sequence
 * numbers.
 *
 * @param peer  Destination peer ID.
 * @param data  Pointer to payload in WASM linear memory.
 * @param len   Payload length in bytes.
 * @return      0 on accepted-for-send, negative on error
 *              (-1 = unknown peer, -2 = payload too large,
 *               -3 = local backpressure, drop and retry next frame).
 */
__attribute__((import_module("env"), import_name("net_send")))
int32_t net_send(const YumiPeerId *peer, const void *data, uint32_t len);

/**
 * @brief Send an unreliable, fire-and-forget message to a single peer.
 *
 * Semantics match @ref net_send except that the runtime performs
 * **no retransmits** and provides **no ordering guarantees** — packets
 * may be dropped, duplicated, or reordered by the network. Use this
 * for high-frequency telemetry (cursor positions, voice frames,
 * per-frame game state snapshots) where losing the occasional
 * datagram is cheaper than waiting for a retransmit.
 *
 * The recipient sees these through the @ref on_net_recv_unreliable
 * export, distinct from the reliable @ref on_net_recv channel.
 *
 * @param peer  Destination peer ID.
 * @param data  Pointer to payload in WASM linear memory.
 * @param len   Payload length in bytes.
 * @return      0 on accepted-for-send, negative on error
 *              (-1 = unknown peer, -2 = payload too large,
 *               -3 = local backpressure, drop and retry next frame).
 */
__attribute__((import_module("env"), import_name("net_send_unreliable")))
int32_t net_send_unreliable(const YumiPeerId *peer,
                            const void *data, uint32_t len);

/**
 * @brief Send the same message to a contiguous range of peers.
 *
 * Semantically equivalent to calling `net_send` in a loop but cheaper:
 * the runtime reuses a single serialised buffer and can batch the
 * transport calls.
 *
 * @param peers       Array of peer IDs.
 * @param peer_count  Number of peers in @p peers.
 * @param data        Pointer to payload in WASM linear memory.
 * @param len         Payload length in bytes.
 * @return            Number of peers the send was accepted for, or a
 *                    negative error code on total failure.
 */
__attribute__((import_module("env"), import_name("net_send_range")))
int32_t net_send_range(const YumiPeerId *peers, uint32_t peer_count,
                       const void *data, uint32_t len);

/**
 * @brief Unreliable variant of @ref net_send_range.
 *
 * Identical to @ref net_send_range except every recipient receives
 * the message through the unreliable path: no retransmits, no
 * ordering guarantees, delivery is best-effort. Recipients see the
 * payload through @ref on_net_recv_unreliable.
 *
 * @param peers       Array of peer IDs.
 * @param peer_count  Number of peers in @p peers.
 * @param data        Pointer to payload in WASM linear memory.
 * @param len         Payload length in bytes.
 * @return            Number of peers the send was accepted for, or a
 *                    negative error code on total failure.
 */
__attribute__((import_module("env"), import_name("net_send_range_unreliable")))
int32_t net_send_range_unreliable(const YumiPeerId *peers, uint32_t peer_count,
                                  const void *data, uint32_t len);

/**
 * @brief Broadcast a message to every peer in the group.
 *
 * The runtime decides whether to flood to every known peer directly
 * or to hand the payload to a rebroadcaster server (if one has been
 * allocated for the group). The webapp does not see and cannot
 * influence this choice.
 *
 * @param data  Pointer to payload in WASM linear memory.
 * @param len   Payload length in bytes.
 * @return      0 on accepted-for-broadcast, negative on error.
 */
__attribute__((import_module("env"), import_name("net_broadcast")))
int32_t net_broadcast(const void *data, uint32_t len);

/**
 * @brief Unreliable broadcast to every peer in the group.
 *
 * Identical to @ref net_broadcast except delivery is best-effort:
 * no retransmits, no ordering guarantees. Recipients see the payload
 * through @ref on_net_broadcast_unreliable.
 *
 * @param data  Pointer to payload in WASM linear memory.
 * @param len   Payload length in bytes.
 * @return      0 on accepted-for-broadcast, negative on error.
 */
__attribute__((import_module("env"), import_name("net_broadcast_unreliable")))
int32_t net_broadcast_unreliable(const void *data, uint32_t len);

/* ================================================================== */
/*  Outbound: data shards                                              */
/* ================================================================== */

/**
 * @brief Upload a data shard for extended retention.
 *
 * A shard is a blob that the runtime will attempt to preserve for up
 * to @p ttl_seconds seconds, either on a rebroadcaster server
 * (preferred) or cooperatively across peers. The Group Registrar is
 * consulted before the upload is accepted — if the group's quota is
 * exhausted or the caller lacks permission this call returns 0.
 *
 * Shards are content-addressed on the host side; the returned shard
 * ID is stable and may be shared with other peers (e.g. embedded in a
 * broadcast message) so they can retrieve the content with
 * `net_shard_request`.
 *
 * @param data          Pointer to shard contents.
 * @param len           Byte length of shard contents.
 * @param kind          Hint for quota/policy classification.
 * @param ttl_seconds   Desired retention in seconds. The runtime may
 *                      shorten this to honour group policy.
 * @return              64-bit shard ID on success, 0 on rejection.
 */
__attribute__((import_module("env"), import_name("net_shard_upload")))
uint64_t net_shard_upload(const void *data, uint32_t len,
                          net_shard_kind_t kind, uint32_t ttl_seconds);

/**
 * @brief Request an enumeration of shards available to this group.
 *
 * Non-blocking. The result is delivered asynchronously through the
 * `on_net_shard_list` export, keyed by the request ID returned here.
 *
 * @return A non-zero request ID used to correlate `on_net_shard_list`
 *         invocations, or 0 on local failure.
 */
__attribute__((import_module("env"), import_name("net_shard_list_request")))
uint32_t net_shard_list_request(void);

/**
 * @brief Begin retrieval of a specific shard by ID.
 *
 * Non-blocking. The runtime locates the shard (on a rebroadcaster or
 * peer), negotiates a transfer, and streams the content back through
 * `on_net_shard_chunk`, finishing with `on_net_shard_result`.
 *
 * @param shard_id  The 64-bit shard identifier returned by a previous
 *                  `net_shard_upload` or discovered via
 *                  `on_net_shard_list`.
 * @return          A non-zero handle ID used to correlate incoming
 *                  chunks with this request, or 0 on local failure
 *                  (unknown shard, policy denial, out of handles).
 */
__attribute__((import_module("env"), import_name("net_shard_request")))
uint32_t net_shard_request(uint64_t shard_id);

/**
 * @brief Cancel an in-flight shard retrieval.
 *
 * A final `on_net_shard_result` with status `NET_SHARD_CANCELLED`
 * will still fire, so cleanup code can live in one place.
 *
 * @param handle  Handle returned by `net_shard_request`.
 */
__attribute__((import_module("env"), import_name("net_shard_cancel")))
void net_shard_cancel(uint32_t handle);

/* ==================================================================
 *  Inbound guest-exported callbacks
 * ------------------------------------------------------------------
 *  The runtime invokes the following exports when the corresponding
 *  events fire.  None of them are required — missing exports are
 *  silently skipped.  See sdk/templates/webapp_template.c and
 *  sdk/templates/dashboard_app_template.c for ready-to-copy stubs
 *  and full per-parameter documentation:
 *
 *    on_net_recv                 (reliable unicast)
 *    on_net_recv_unreliable      (unreliable unicast)
 *    on_net_broadcast            (reliable broadcast)
 *    on_net_broadcast_unreliable (unreliable broadcast)
 *    on_net_peer_presence        (peer join / leave)
 *    on_net_shard_list           (shard enumeration result)
 *    on_net_shard_chunk          (shard transfer chunk)
 *    on_net_shard_result         (shard transfer terminal status)
 *
 *  Payload buffers passed to these callbacks are host-owned and valid
 *  only for the duration of the call — copy anything you need to
 *  retain beyond the callback.
 * ================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* WASM_NETWORK_H */
