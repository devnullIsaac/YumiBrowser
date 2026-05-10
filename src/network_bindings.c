/*
    Network WASM Bindings Implementation
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

/**
 * @file network_bindings.c
 * @brief Implementation of the @c net_* WASM import surface.
 *
 * Send paths are stubbed with a -3 "local backpressure" return when
 * no transport is attached, so a guest can safely call them before
 * peer routing is wired in. Inbound dispatch routes reliable and
 * unreliable traffic onto separate guest exports.
 *
 * This module is deliberately independent of dashboard state and the
 * Group Registrar — peer membership is a network concern.
 */

#include "network_bindings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N ((NetworkBindings *)env)

/* ------------------------------------------------------------------ */
/*  Memory helpers                                                     */
/* ------------------------------------------------------------------ */

static uint8_t *nb_mem(NetworkBindings *n) {
    return n->memory ? (uint8_t *)wasm_memory_data(n->memory) : NULL;
}
static size_t nb_mem_size(NetworkBindings *n) {
    return n->memory ? wasm_memory_data_size(n->memory) : 0;
}
static bool nb_mem_check(NetworkBindings *n, uint32_t ptr, uint32_t len) {
    if (!n->memory) return false;
    return (size_t)ptr + (size_t)len <= nb_mem_size(n);
}

/* Maximum payload size accepted on any single send/broadcast call.
 * This caps the guest-visible buffer; the transport layer may further
 * fragment or reject larger messages. */
#define NB_MAX_PAYLOAD  (1u << 20)  /* 1 MiB */

/* Unique tag for each send flavour — the transport will eventually
 * key routing on these. */
typedef enum {
    NB_SEND_RELIABLE           = 0,
    NB_SEND_UNRELIABLE         = 1,
    NB_BROADCAST_RELIABLE      = 2,
    NB_BROADCAST_UNRELIABLE    = 3,
} nb_send_mode_t;

/* Placeholder outbound dispatcher. Returns a signed error code
 * compatible with the SDK contract in @ref sdk/wasm_network.h. */
static int32_t nb_deliver(NetworkBindings *n, nb_send_mode_t mode,
                          const uint8_t *peer_id, uint32_t peer_count,
                          const uint8_t *data, uint32_t len)
{
    (void)mode; (void)peer_id; (void)peer_count; (void)data; (void)len;
    if (!n->transport) {
        /* No transport attached yet — report local backpressure so
         * the guest retries next frame without dropping the payload. */
        return -3;
    }
    /* TODO: forward to yumi_udp_client with the appropriate channel
     * and reliability flag derived from @p mode. */
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Imports                                                            */
/* ------------------------------------------------------------------ */

#define ARG_I32(i) (args->data[(i)].of.i32)
#define RET_I32(v) do { res->data[0] = \
    (wasm_val_t){.kind = WASM_I32, .of.i32 = (int32_t)(v)}; } while (0)
#define RET_I64(v) do { res->data[0] = \
    (wasm_val_t){.kind = WASM_I64, .of.i64 = (int64_t)(v)}; } while (0)

static wasm_trap_t *fn_net_peer_count(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)N; (void)args; RET_I32(0); return NULL;
}

static wasm_trap_t *fn_net_peer_list(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)N;
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    uint32_t out_cap = (uint32_t)ARG_I32(1);
    (void)out_ptr; (void)out_cap;
    RET_I32(0); /* no peers known yet */
    return NULL;
}

/* Shared body for the four send flavours. */
static wasm_trap_t *fn_net_send_common(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res,
    nb_send_mode_t mode)
{
    uint32_t peer_ptr = (uint32_t)ARG_I32(0);
    uint32_t data_ptr = (uint32_t)ARG_I32(1);
    uint32_t len      = (uint32_t)ARG_I32(2);

    if (len == 0 || len > NB_MAX_PAYLOAD) { RET_I32(-2); return NULL; }
    if (!nb_mem_check(N, peer_ptr, YUMI_PEER_ID_LEN) ||
        !nb_mem_check(N, data_ptr, len)) { RET_I32(-1); return NULL; }

    const uint8_t *mem   = nb_mem(N);
    const uint8_t *peer  = mem + peer_ptr;
    const uint8_t *data  = mem + data_ptr;
    RET_I32(nb_deliver(N, mode, peer, 1, data, len));
    return NULL;
}

static wasm_trap_t *fn_net_send(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    return fn_net_send_common(env, args, res, NB_SEND_RELIABLE);
}
static wasm_trap_t *fn_net_send_unreliable(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    return fn_net_send_common(env, args, res, NB_SEND_UNRELIABLE);
}

static wasm_trap_t *fn_net_send_range_common(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res,
    nb_send_mode_t mode)
{
    uint32_t peers_ptr  = (uint32_t)ARG_I32(0);
    uint32_t peer_count = (uint32_t)ARG_I32(1);
    uint32_t data_ptr   = (uint32_t)ARG_I32(2);
    uint32_t len        = (uint32_t)ARG_I32(3);

    if (len == 0 || len > NB_MAX_PAYLOAD) { RET_I32(-2); return NULL; }
    if (peer_count == 0) { RET_I32(0); return NULL; }
    size_t peers_bytes = (size_t)peer_count * YUMI_PEER_ID_LEN;
    if (peers_bytes / YUMI_PEER_ID_LEN != peer_count) { RET_I32(-2); return NULL; }
    if (!nb_mem_check(N, peers_ptr, (uint32_t)peers_bytes) ||
        !nb_mem_check(N, data_ptr, len)) { RET_I32(-1); return NULL; }

    const uint8_t *mem   = nb_mem(N);
    const uint8_t *peers = mem + peers_ptr;
    const uint8_t *data  = mem + data_ptr;
    RET_I32(nb_deliver(N, mode, peers, peer_count, data, len));
    return NULL;
}
static wasm_trap_t *fn_net_send_range(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    return fn_net_send_range_common(env, args, res, NB_SEND_RELIABLE);
}
static wasm_trap_t *fn_net_send_range_unreliable(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    return fn_net_send_range_common(env, args, res, NB_SEND_UNRELIABLE);
}

static wasm_trap_t *fn_net_broadcast_common(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res,
    nb_send_mode_t mode)
{
    uint32_t data_ptr = (uint32_t)ARG_I32(0);
    uint32_t len      = (uint32_t)ARG_I32(1);

    if (len == 0 || len > NB_MAX_PAYLOAD) { RET_I32(-2); return NULL; }
    if (!nb_mem_check(N, data_ptr, len)) { RET_I32(-1); return NULL; }

    const uint8_t *data = nb_mem(N) + data_ptr;
    RET_I32(nb_deliver(N, mode, NULL, 0, data, len));
    return NULL;
}
static wasm_trap_t *fn_net_broadcast(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    return fn_net_broadcast_common(env, args, res, NB_BROADCAST_RELIABLE);
}
static wasm_trap_t *fn_net_broadcast_unreliable(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    return fn_net_broadcast_common(env, args, res, NB_BROADCAST_UNRELIABLE);
}

static wasm_trap_t *fn_net_shard_upload(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)N; (void)args; RET_I64(0); return NULL;
}
static wasm_trap_t *fn_net_shard_list_request(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)N; (void)args; RET_I32(0); return NULL;
}
static wasm_trap_t *fn_net_shard_request(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)N; (void)args; RET_I32(0); return NULL;
}
static wasm_trap_t *fn_net_shard_cancel(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)N; (void)args; (void)res; return NULL;
}

/* ------------------------------------------------------------------ */
/*  Binding table                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char                    *name;
    wasm_func_callback_with_env_t  cb;
    uint32_t np; wasm_valkind_t params[4];
    uint32_t nr; wasm_valkind_t results[1];
} NetBindingEntry;

#define I WASM_I32
#define J WASM_I64

static const NetBindingEntry NET_BINDINGS[] = {
    { "net_peer_count",             fn_net_peer_count,            0, {0},       1, {I} },
    { "net_peer_list",              fn_net_peer_list,             2, {I,I},     1, {I} },

    { "net_send",                   fn_net_send,                  3, {I,I,I},   1, {I} },
    { "net_send_unreliable",        fn_net_send_unreliable,       3, {I,I,I},   1, {I} },
    { "net_send_range",             fn_net_send_range,            4, {I,I,I,I}, 1, {I} },
    { "net_send_range_unreliable",  fn_net_send_range_unreliable, 4, {I,I,I,I}, 1, {I} },
    { "net_broadcast",              fn_net_broadcast,             2, {I,I},     1, {I} },
    { "net_broadcast_unreliable",   fn_net_broadcast_unreliable,  2, {I,I},     1, {I} },

    { "net_shard_upload",           fn_net_shard_upload,          4, {I,I,I,I}, 1, {J} },
    { "net_shard_list_request",     fn_net_shard_list_request,    0, {0},       1, {I} },
    { "net_shard_request",          fn_net_shard_request,         1, {J},       1, {I} },
    { "net_shard_cancel",           fn_net_shard_cancel,          1, {I},       0, {0} },
};

#undef I
#undef J

#define NUM_NET_BINDINGS (sizeof(NET_BINDINGS) / sizeof(NET_BINDINGS[0]))

static wasm_functype_t *make_ft(uint32_t np, const wasm_valkind_t p[],
                                uint32_t nr, const wasm_valkind_t r[])
{
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[4];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else { wasm_valtype_vec_new_empty(&params); }
    if (nr > 0) {
        wasm_valtype_t *rt[1];
        rt[0] = wasm_valtype_new(r[0]);
        wasm_valtype_vec_new(&results, nr, rt);
    } else { wasm_valtype_vec_new_empty(&results); }
    return wasm_functype_new(&params, &results);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void network_bindings_init(NetworkBindings *n) {
    memset(n, 0, sizeof(*n));
    printf("[network] Bindings ready (%zu imports)\n", NUM_NET_BINDINGS);
}

void network_bindings_destroy(NetworkBindings *n) {
    memset(n, 0, sizeof(*n));
}

void network_bindings_set_memory(NetworkBindings *n, wasm_memory_t *mem) {
    n->memory = mem;
}

void network_bindings_set_transport(NetworkBindings *n,
                                    YumiNetTransport *transport)
{
    n->transport = transport;
}

size_t network_bindings_get_imports(NetworkBindings *n,
                                    wasm_store_t *store,
                                    const char ***out_names,
                                    wasm_func_t ***out_funcs)
{
    static const char  *names[NUM_NET_BINDINGS];
    static wasm_func_t *funcs[NUM_NET_BINDINGS];
    for (size_t i = 0; i < NUM_NET_BINDINGS; i++) {
        names[i] = NET_BINDINGS[i].name;
        wasm_functype_t *ft = make_ft(
            NET_BINDINGS[i].np, NET_BINDINGS[i].params,
            NET_BINDINGS[i].nr, NET_BINDINGS[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft,
            NET_BINDINGS[i].cb, n, NULL);
        wasm_functype_delete(ft);
    }
    *out_names = names;
    *out_funcs = funcs;
    return NUM_NET_BINDINGS;
}

/* ------------------------------------------------------------------ */
/*  Host → guest dispatch                                              */
/* ------------------------------------------------------------------ */

/* Stage @p data at the top of guest memory (16-byte aligned) after
 * reserving 32 bytes for the peer-id, so both live in guest space at
 * once. Returns 0 on failure. On success *out_peer and *out_data are
 * set to the guest offsets. */
static bool nb_stage(NetworkBindings *n,
                     const uint8_t peer_id[YUMI_PEER_ID_LEN],
                     const void *data, uint32_t len,
                     uint32_t *out_peer, uint32_t *out_data)
{
    uint8_t *mem = nb_mem(n);
    size_t   msz = nb_mem_size(n);
    if (!mem) return false;
    size_t need = (size_t)YUMI_PEER_ID_LEN + (size_t)len + 32;
    if (msz < need) return false;
    uint32_t base = (uint32_t)(msz - need);
    base &= ~(uint32_t)15;
    memcpy(mem + base, peer_id, YUMI_PEER_ID_LEN);
    if (len > 0 && data) memcpy(mem + base + YUMI_PEER_ID_LEN, data, len);
    *out_peer = base;
    *out_data = base + YUMI_PEER_ID_LEN;
    return true;
}

static void nb_invoke3(wasm_func_t *fn,
                       uint32_t a0, uint32_t a1, uint32_t a2,
                       const char *tag)
{
    if (!fn) return;
    wasm_val_t a[] = {
        (wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)a0 },
        (wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)a1 },
        (wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)a2 },
    };
    wasm_val_vec_t args = { .size = 3, .data = a };
    wasm_val_vec_t r    = WASM_EMPTY_VEC;
    wasm_trap_t *trap = wasm_func_call(fn, &args, &r);
    if (trap) {
        wasm_message_t m; wasm_trap_message(trap, &m);
        fprintf(stderr, "[network] %s trap: %.*s\n",
                tag, (int)m.size, m.data);
        wasm_byte_vec_delete(&m); wasm_trap_delete(trap);
    }
}

static void nb_dispatch_payload(NetworkBindings *n, wasm_func_t *fn,
                                const uint8_t peer_id[YUMI_PEER_ID_LEN],
                                const void *data, uint32_t len,
                                const char *tag)
{
    if (!n || !fn) return;
    uint32_t peer_off, data_off;
    if (!nb_stage(n, peer_id, data, len, &peer_off, &data_off)) return;
    nb_invoke3(fn, peer_off, data_off, len, tag);
}

void network_bindings_dispatch_recv(NetworkBindings *n,
                                    const uint8_t peer_id[YUMI_PEER_ID_LEN],
                                    const void *data, uint32_t len)
{
    nb_dispatch_payload(n, n->fn_on_net_recv, peer_id, data, len, "on_net_recv");
}

void network_bindings_dispatch_recv_unreliable(NetworkBindings *n,
    const uint8_t peer_id[YUMI_PEER_ID_LEN],
    const void *data, uint32_t len)
{
    nb_dispatch_payload(n, n->fn_on_net_recv_unreliable, peer_id, data, len,
                        "on_net_recv_unreliable");
}

void network_bindings_dispatch_broadcast(NetworkBindings *n,
    const uint8_t peer_id[YUMI_PEER_ID_LEN],
    const void *data, uint32_t len)
{
    nb_dispatch_payload(n, n->fn_on_net_broadcast, peer_id, data, len,
                        "on_net_broadcast");
}

void network_bindings_dispatch_broadcast_unreliable(NetworkBindings *n,
    const uint8_t peer_id[YUMI_PEER_ID_LEN],
    const void *data, uint32_t len)
{
    nb_dispatch_payload(n, n->fn_on_net_broadcast_unreliable, peer_id, data, len,
                        "on_net_broadcast_unreliable");
}

void network_bindings_dispatch_peer_presence(NetworkBindings *n,
    const uint8_t peer_id[YUMI_PEER_ID_LEN],
    int32_t present)
{
    if (!n || !n->fn_on_net_peer_presence) return;
    uint8_t *mem = nb_mem(n);
    size_t   msz = nb_mem_size(n);
    if (!mem || msz < YUMI_PEER_ID_LEN + 16) return;
    uint32_t off = (uint32_t)(msz - YUMI_PEER_ID_LEN - 16);
    off &= ~(uint32_t)15;
    memcpy(mem + off, peer_id, YUMI_PEER_ID_LEN);

    wasm_val_t a[] = {
        (wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)off },
        (wasm_val_t){ .kind = WASM_I32, .of.i32 = present },
    };
    wasm_val_vec_t args = { .size = 2, .data = a };
    wasm_val_vec_t r    = WASM_EMPTY_VEC;
    wasm_trap_t *trap = wasm_func_call(n->fn_on_net_peer_presence, &args, &r);
    if (trap) {
        wasm_message_t m; wasm_trap_message(trap, &m);
        fprintf(stderr, "[network] on_net_peer_presence trap: %.*s\n",
                (int)m.size, m.data);
        wasm_byte_vec_delete(&m); wasm_trap_delete(trap);
    }
}
