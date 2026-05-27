/*
 * network_bindings.h - Host-side WASM bindings for the peer-to-peer wasm_network.h surface: net_peer_*, reliable/unreliable send/broadcast, shard upload/request.
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

#ifndef NETWORK_BINDINGS_H
#define NETWORK_BINDINGS_H

/**
 * @file network_bindings.h
 * @brief Host-side WASM bindings for the @c wasm_network.h SDK surface.
 *
 * Exposes the peer-to-peer networking imports to a guest webapp:
 *   - @c net_peer_count / @c net_peer_list
 *   - @c net_send / @c net_send_range / @c net_broadcast       (reliable)
 *   - @c net_send_unreliable / @c net_send_range_unreliable /
 *     @c net_broadcast_unreliable                              (unreliable)
 *   - @c net_shard_upload / @c net_shard_list_request /
 *     @c net_shard_request / @c net_shard_cancel
 *
 * The reliable / unreliable distinction is exposed end-to-end: the
 * binding stores separate guest-export pointers for the reliable and
 * unreliable receive callbacks so the runtime can deliver traffic
 * onto the correct channel.
 *
 * This module intentionally does not touch the Group Registrar or
 * dashboard IPC — peer routing and membership come from the network
 * subsystem (see @ref include/network/yumi_udp_client.h), not from
 * dashboard state.
 */

#include "deps.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YUMI_PEER_ID_LEN
#define YUMI_PEER_ID_LEN 32
#endif

/* Forward-declared transport handle — bound later when peer routing
 * is wired in. Holding an opaque pointer here keeps the binding
 * independent of the concrete transport implementation. */
typedef struct YumiNetTransport YumiNetTransport;

/**
 * @brief Host-side state for the @c net_* WASM imports.
 *
 * One instance per WebAppRuntime. The transport pointer is optional;
 * when NULL, outbound calls report -3 (local backpressure) so the
 * guest can retry and inbound dispatch is a no-op.
 */
typedef struct {
    wasm_memory_t     *memory;     /**< Guest linear memory (for validation). */
    YumiNetTransport  *transport;  /**< Optional; set by runtime once bound. */

    /* Guest-exported receive callbacks (resolved after instantiation).
     * Any subset may be NULL; missing callbacks drop the corresponding
     * traffic. Reliable and unreliable are strictly separate channels. */
    wasm_func_t *fn_on_net_recv;                 /**< reliable unicast    */
    wasm_func_t *fn_on_net_broadcast;            /**< reliable broadcast  */
    wasm_func_t *fn_on_net_recv_unreliable;      /**< unreliable unicast  */
    wasm_func_t *fn_on_net_broadcast_unreliable; /**< unreliable broadcast*/
    wasm_func_t *fn_on_net_peer_presence;        /**< join/leave          */
    wasm_func_t *fn_on_net_shard_list;
    wasm_func_t *fn_on_net_shard_chunk;
    wasm_func_t *fn_on_net_shard_result;
} NetworkBindings;

/** @brief Zero-initialise the binding state. */
void   network_bindings_init(NetworkBindings *n);

/** @brief Release binding resources. */
void   network_bindings_destroy(NetworkBindings *n);

/** @brief Attach the guest's linear memory. */
void   network_bindings_set_memory(NetworkBindings *n, wasm_memory_t *mem);

/** @brief Attach (or clear) the host transport used for outbound sends. */
void   network_bindings_set_transport(NetworkBindings *n,
                                      YumiNetTransport *transport);

/**
 * @brief Build the import function table for the @c env module.
 *
 * Names and pointers live in static storage owned by the binding;
 * callers must not free them.
 */
size_t network_bindings_get_imports(NetworkBindings *n,
                                    wasm_store_t *store,
                                    const char ***out_names,
                                    wasm_func_t ***out_funcs);

/* ------------------------------------------------------------------ */
/*  Inbound dispatch — host → guest                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Deliver a reliable unicast payload to @c on_net_recv.
 * Safe no-op when the export is absent.
 */
void network_bindings_dispatch_recv(NetworkBindings *n,
                                    const uint8_t peer_id[YUMI_PEER_ID_LEN],
                                    const void *data, uint32_t len);

/**
 * @brief Deliver an unreliable unicast payload to
 *        @c on_net_recv_unreliable.
 */
void network_bindings_dispatch_recv_unreliable(
    NetworkBindings *n,
    const uint8_t peer_id[YUMI_PEER_ID_LEN],
    const void *data, uint32_t len);

/**
 * @brief Deliver a reliable broadcast payload to @c on_net_broadcast.
 */
void network_bindings_dispatch_broadcast(NetworkBindings *n,
                                         const uint8_t peer_id[YUMI_PEER_ID_LEN],
                                         const void *data, uint32_t len);

/**
 * @brief Deliver an unreliable broadcast payload to
 *        @c on_net_broadcast_unreliable.
 */
void network_bindings_dispatch_broadcast_unreliable(
    NetworkBindings *n,
    const uint8_t peer_id[YUMI_PEER_ID_LEN],
    const void *data, uint32_t len);

/** @brief Invoke @c on_net_peer_presence with @p present non-zero
 *         on join, zero on leave. */
void network_bindings_dispatch_peer_presence(
    NetworkBindings *n,
    const uint8_t peer_id[YUMI_PEER_ID_LEN],
    int32_t present);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_BINDINGS_H */
