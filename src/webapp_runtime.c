/*
    Sandboxed WASM Runtime for Individual Webapps (Implementation)
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
 * @file webapp_runtime.c
 * @brief Sandboxed WASM runtime for individual webapps.
 *
 * Refactored from runtime.c.  All bindings are per-instance (no
 * globals).  SDL bindings are removed — webapps use dashboard IPC
 * imports for file dialogs and friend management.
 */

#include "webapp_runtime.h"
#include "dashboard_runtime.h"
#include "wgpu_bindings.h"
#include "duckdb_bindings.h"
#include "log_bindings.h"
#include "font_bindings.h"
#include "text_bindings.h"
#include "input_bindings.h"
#include "media_bindings.h"
#include "clipboard_bindings.h"
#include "slang_bindings.h"
#include "network_bindings.h"
#include "wgpu_ffmpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* Default per-webapp DuckDB quota, in bytes.  The dashboard may
 * override this at any time via webapp_runtime_set_db_quota(). */
#define WEBAPP_DEFAULT_DB_QUOTA_BYTES  ((uint64_t)256 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/*  file I/O                                                           */
/* ------------------------------------------------------------------ */

static uint8_t *load_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)len, f); fclose(f);
    *out_size = (size_t)len; return buf;
}

/* ------------------------------------------------------------------ */
/*  functype builder                                                   */
/* ------------------------------------------------------------------ */

static wasm_functype_t *make_functype(size_t np, wasm_valkind_t p[],
                                      size_t nr, wasm_valkind_t r[]) {
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *ptypes[np];
        for (size_t i = 0; i < np; i++) ptypes[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, ptypes);
    } else { wasm_valtype_vec_new_empty(&params); }
    if (nr > 0) {
        wasm_valtype_t *rtypes[nr];
        for (size_t i = 0; i < nr; i++) rtypes[i] = wasm_valtype_new(r[i]);
        wasm_valtype_vec_new(&results, nr, rtypes);
    } else { wasm_valtype_vec_new_empty(&results); }
    return wasm_functype_new(&params, &results);
}

/* ------------------------------------------------------------------ */
/*  simple host functions (gpu_clear, gpu_rect, gpu_width, gpu_height) */
/* ------------------------------------------------------------------ */

static wasm_trap_t *host_gpu_clear(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; GpuContext *ctx = env;
    gpu_set_clear_color(ctx, args->data[0].of.f32, args->data[1].of.f32,
                        args->data[2].of.f32, args->data[3].of.f32);
    return NULL;
}
static wasm_trap_t *host_gpu_rect(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; GpuContext *ctx = env;
    gpu_push_rect(ctx, args->data[0].of.f32, args->data[1].of.f32,
                  args->data[2].of.f32, args->data[3].of.f32,
                  args->data[4].of.f32, args->data[5].of.f32,
                  args->data[6].of.f32, args->data[7].of.f32);
    return NULL;
}
static wasm_trap_t *host_gpu_width(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; GpuContext *ctx = env;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)ctx->width }; return NULL;
}
static wasm_trap_t *host_gpu_height(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; GpuContext *ctx = env;
    res->data[0] = (wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)ctx->height }; return NULL;
}

typedef struct {
    const char *name; wasm_func_callback_with_env_t cb;
    size_t np; wasm_valkind_t params[8]; size_t nr; wasm_valkind_t results[1];
} HostFn;
static const HostFn HOST_FNS[] = {
    { "gpu_clear",  host_gpu_clear,  4, {WASM_F32,WASM_F32,WASM_F32,WASM_F32}, 0, {0} },
    { "gpu_rect",   host_gpu_rect,   8, {WASM_F32,WASM_F32,WASM_F32,WASM_F32,WASM_F32,WASM_F32,WASM_F32,WASM_F32}, 0, {0} },
    { "gpu_width",  host_gpu_width,  0, {0}, 1, {WASM_I32} },
    { "gpu_height", host_gpu_height, 0, {0}, 1, {WASM_I32} },
};
#define NUM_HOST_FNS (sizeof(HOST_FNS)/sizeof(HOST_FNS[0]))

static const HostFn *find_host_fn(const char *name, size_t len) {
    for (size_t i = 0; i < NUM_HOST_FNS; i++)
        if (strlen(HOST_FNS[i].name) == len && memcmp(HOST_FNS[i].name, name, len) == 0)
            return &HOST_FNS[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  sandboxed DuckDB init                                              */
/* ------------------------------------------------------------------ */

static bool duckdb_init_secure(const char *db_path,
                               duckdb_database *db_out,
                               duckdb_connection *conn_out) {
    duckdb_config config;
    if (duckdb_create_config(&config) != DuckDBSuccess) return false;
    duckdb_set_config(config, "enable_external_access", "false");
    duckdb_set_config(config, "allow_community_extensions", "false");
    duckdb_set_config(config, "autoload_known_extensions", "false");
    duckdb_set_config(config, "autoinstall_known_extensions", "false");
    duckdb_set_config(config, "threads", "2");
    duckdb_set_config(config, "memory_limit", "256MB");
    duckdb_set_config(config, "max_temp_directory_size", "256MB");
    char *err = NULL;
    if (duckdb_open_ext(db_path, db_out, config, &err) != DuckDBSuccess) {
        fprintf(stderr, "[duckdb] Open failed: %s\n", err ? err : "?");
        duckdb_free(err); duckdb_destroy_config(&config); return false;
    }
    duckdb_destroy_config(&config);
    if (duckdb_connect(*db_out, conn_out) != DuckDBSuccess) {
        duckdb_close(db_out); return false;
    }
    duckdb_result r;
    duckdb_state st = duckdb_query(*conn_out, "SET lock_configuration = true;", &r);
    duckdb_destroy_result(&r);
    if (st != DuckDBSuccess) {
        duckdb_disconnect(conn_out); duckdb_close(db_out); return false;
    }
    printf("[webapp-duckdb] Secured and locked: %s\n", db_path);
    return true;
}

/* ------------------------------------------------------------------ */
/*  WASM memory helpers                                                */
/* ------------------------------------------------------------------ */

/** Get base pointer to the guest's linear memory. */
static inline uint8_t *wasm_mem(WebAppRuntime *rt) {
    return rt->memory ? (uint8_t *)wasm_memory_data(rt->memory) : NULL;
}

/** Get the size of the guest's linear memory. */
static inline size_t wasm_mem_size(WebAppRuntime *rt) {
    return rt->memory ? wasm_memory_data_size(rt->memory) : 0;
}

/** Bounds-check a guest pointer + length. */
static inline bool wasm_mem_check(WebAppRuntime *rt, uint32_t ptr, uint32_t len) {
    return rt->memory && (size_t)ptr + (size_t)len <= wasm_mem_size(rt);
}

/* ------------------------------------------------------------------ */
/*  Guest export table                                                 */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; size_t name_len; size_t off; } ExportEntry;
#define EXPORT_SLOT(n, f) { n, sizeof(n)-1, offsetof(WebAppRuntime, f) }

static const ExportEntry GUEST_EXPORTS[] = {
    EXPORT_SLOT("init",                fn_init),
    EXPORT_SLOT("frame",               fn_frame),
    EXPORT_SLOT("on_key",              fn_on_key),
    EXPORT_SLOT("on_char",             fn_on_char),
    EXPORT_SLOT("on_mouse_button",     fn_on_mouse_button),
    EXPORT_SLOT("on_mouse_motion",     fn_on_mouse_motion),
    EXPORT_SLOT("on_mouse_wheel",      fn_on_mouse_wheel),
    EXPORT_SLOT("on_touch",            fn_on_touch),
    EXPORT_SLOT("on_joystick_added",   fn_on_joystick_added),
    EXPORT_SLOT("on_joystick_removed", fn_on_joystick_removed),
    EXPORT_SLOT("on_joystick_axis",    fn_on_joystick_axis),
    EXPORT_SLOT("on_joystick_button",  fn_on_joystick_button),
    EXPORT_SLOT("on_joystick_hat",     fn_on_joystick_hat),
    EXPORT_SLOT("on_gamepad_added",    fn_on_gamepad_added),
    EXPORT_SLOT("on_gamepad_removed",  fn_on_gamepad_removed),
    EXPORT_SLOT("on_resize",           fn_on_resize),
    EXPORT_SLOT("on_focus",            fn_on_focus),
    EXPORT_SLOT("on_file_result",      fn_on_file_result),
    EXPORT_SLOT("on_folder_result",    fn_on_folder_result),
    EXPORT_SLOT("on_paste_result",     fn_on_paste_result),
    EXPORT_SLOT("on_friend_list_result", fn_on_friend_list_result),
    EXPORT_SLOT("on_db_quota_changed",  fn_on_db_quota_changed),
    EXPORT_SLOT("on_group_switched",    fn_on_group_switched),
    EXPORT_SLOT("on_group_background_changed", fn_on_group_background_changed),
};

/* Network callbacks live on the NetworkBindings, not on the main
 * runtime — they are wired up separately after module load. */
typedef struct { const char *name; size_t name_len; size_t off; } NetExportEntry;
#define NET_EXPORT_SLOT(n, f) { n, sizeof(n)-1, offsetof(NetworkBindings, f) }
static const NetExportEntry NET_GUEST_EXPORTS[] = {
    NET_EXPORT_SLOT("on_net_recv",                 fn_on_net_recv),
    NET_EXPORT_SLOT("on_net_recv_unreliable",      fn_on_net_recv_unreliable),
    NET_EXPORT_SLOT("on_net_broadcast",            fn_on_net_broadcast),
    NET_EXPORT_SLOT("on_net_broadcast_unreliable", fn_on_net_broadcast_unreliable),
    NET_EXPORT_SLOT("on_net_peer_presence",        fn_on_net_peer_presence),
    NET_EXPORT_SLOT("on_net_shard_list",           fn_on_net_shard_list),
    NET_EXPORT_SLOT("on_net_shard_chunk",          fn_on_net_shard_chunk),
    NET_EXPORT_SLOT("on_net_shard_result",         fn_on_net_shard_result),
};
#define NUM_NET_GUEST_EXPORTS (sizeof(NET_GUEST_EXPORTS)/sizeof(NET_GUEST_EXPORTS[0]))
#define NUM_GUEST_EXPORTS (sizeof(GUEST_EXPORTS)/sizeof(GUEST_EXPORTS[0]))

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

bool webapp_runtime_init(WebAppRuntime *rt,
                         DashboardRuntime *dashboard,
                         GpuContext *gpu,
                         uint32_t group_index,
                         const char *db_path,
                         bool is_dashboard) {
    memset(rt, 0, sizeof(*rt));
    rt->dashboard    = dashboard;
    rt->group_index  = group_index;
    rt->is_dashboard = is_dashboard;
    rt->focused      = false;

    rt->engine = wasm_engine_new();
    if (!rt->engine) return false;
    rt->store = wasm_store_new(rt->engine);
    if (!rt->store) return false;
    printf("[webapp] Wasmer %s ready\n", wasmer_version());

    /* Per-webapp sandboxed DuckDB */
    if (db_path) {
        if (!duckdb_init_secure(db_path, &rt->duckdb_db, &rt->duckdb_conn))
            return false;
        duckdb_bindings_init(&rt->duckdb, rt->duckdb_db, rt->duckdb_conn);
        /* Arm the disk-quota guard.  Dashboard can retune later. */
        duckdb_bindings_set_db_path(&rt->duckdb, db_path);
        duckdb_bindings_set_quota(&rt->duckdb, WEBAPP_DEFAULT_DB_QUOTA_BYTES);
        rt->duckdb_active = true;
    }

    /* WGPU bindings will be fully configured in webapp_runtime_load
       once we know the offscreen texture dimensions */
    wgpu_bindings_init(&rt->wgpu, gpu);

    /* SDL bindings */
    sdl_bindings_init(&rt->sdl, NULL);

    return true;
}

bool webapp_runtime_load(WebAppRuntime *rt,
                         SDL_Window *win,
                         const char *wasm_path) {
    size_t fsz = 0;
    uint8_t *fd = load_file(wasm_path, &fsz);
    if (!fd) {
        fprintf(stderr, "[webapp] Cannot open '%s'\n", wasm_path);
        return false;
    }
    wasm_byte_vec_t bytes = { .size = fsz, .data = (char *)fd };
    rt->module = wasm_module_new(rt->store, &bytes);
    free(fd);
    if (!rt->module) {
        fprintf(stderr, "[webapp] Compile failed\n");
        return false;
    }

    /* ── Init per-instance bindings (NO SDL) ── */
    const char **wgpu_n; wasm_func_t **wgpu_f;
    size_t wgpu_c = wgpu_bindings_get_imports(&rt->wgpu, rt->store, &wgpu_n, &wgpu_f);

    clipboard_bindings_init(&rt->clipboard);
    const char **clip_n; wasm_func_t **clip_f;
    size_t clip_c = clipboard_bindings_get_imports(&rt->clipboard, rt->store, &clip_n, &clip_f);

    const char **ddb_n = NULL; wasm_func_t **ddb_f = NULL; size_t ddb_c = 0;
    if (rt->duckdb_active)
        ddb_c = duckdb_bindings_get_imports(&rt->duckdb, rt->store, &ddb_n, &ddb_f);

    log_bindings_init(&rt->log);
    const char **log_n; wasm_func_t **log_f;
    size_t log_c = log_bindings_get_imports(&rt->log, rt->store, &log_n, &log_f);

    font_bindings_init(&rt->font);
    const char **fnt_n; wasm_func_t **fnt_f;
    size_t fnt_c = font_bindings_get_imports(&rt->font, rt->store, &fnt_n, &fnt_f);

    text_bindings_init(&rt->text);
    text_bindings_set_font_bindings(&rt->text, &rt->font);
    const char **txt_n; wasm_func_t **txt_f;
    size_t txt_c = text_bindings_get_imports(&rt->text, rt->store, &txt_n, &txt_f);

    input_bindings_init(&rt->input, win);
    const char **inp_n; wasm_func_t **inp_f;
    size_t inp_c = input_bindings_get_imports(&rt->input, rt->store, &inp_n, &inp_f);

    /* WFF zero-copy bridge (per-instance) */
    WFFBridgeConfig wff_cfg = {
        .device      = rt->wgpu.gpu->device,
        .queue       = rt->wgpu.gpu->queue,
        .auto_access = true,
    };
    rt->wff_bridge = wff_bridge_create(&wff_cfg);

    media_bindings_init(&rt->media, &rt->wgpu, rt->wff_bridge);
    const char **med_n; wasm_func_t **med_f;
    size_t med_c = media_bindings_get_imports(&rt->media, rt->store, &med_n, &med_f);

    slang_bindings_init(&rt->slang, &rt->wgpu);
    const char **slg_n; wasm_func_t **slg_f;
    size_t slg_c = slang_bindings_get_imports(&rt->slang, rt->store, &slg_n, &slg_f);

    /* SDL bindings (dashboard needs them for picturebox animation) */
    const char **sdl_n; wasm_func_t **sdl_f;
    size_t sdl_c = sdl_bindings_get_imports(&rt->sdl, rt->store, &sdl_n, &sdl_f);

    /* Networking is deliberately withheld from the dashboard slot: the
     * dashboard orchestrates groups but is not itself a peer endpoint. */
    const char **net_n = NULL; wasm_func_t **net_f = NULL; size_t net_c = 0;
    if (!rt->is_dashboard) {
        network_bindings_init(&rt->network);
        net_c = network_bindings_get_imports(&rt->network, rt->store, &net_n, &net_f);
    }

    /* ── Build IPC import funcs ──
     *   - Dashboard slot gets the privileged admin table
     *     (groups, GR proxy, audit, behaviour, blocklist).
     *   - Regular webapps get the webapp→dashboard request table
     *     (clipboard, link, dialogs, friends, notify, self-identity). */
    size_t ipc_count;
    const DashboardIPCEntry *ipc_tbl = rt->is_dashboard
        ? dashboard_only_ipc_table(&ipc_count)
        : webapp_ipc_table(&ipc_count);
    wasm_func_t **ipc_funcs = calloc(ipc_count, sizeof(*ipc_funcs));
    const char  **ipc_names = calloc(ipc_count, sizeof(*ipc_names));
    for (size_t i = 0; i < ipc_count; i++) {
        ipc_names[i] = ipc_tbl[i].name;
        wasm_valkind_t p[10], r[1];
        for (size_t k = 0; k < ipc_tbl[i].np; k++) p[k] = (wasm_valkind_t)ipc_tbl[i].params[k];
        for (size_t k = 0; k < ipc_tbl[i].nr; k++) r[k] = (wasm_valkind_t)ipc_tbl[i].results[k];
        wasm_functype_t *ft = make_functype(ipc_tbl[i].np, p, ipc_tbl[i].nr, r);
        ipc_funcs[i] = wasm_func_new_with_env(rt->store, ft, ipc_tbl[i].cb, rt, NULL);
        wasm_functype_delete(ft);
    }

    /* ── Provider table (no SDL — clipboard + IPC available) ── */
    struct {
        const char **names; wasm_func_t **funcs; size_t count; const char *tag;
    } providers[] = {
        { wgpu_n,     wgpu_f,     wgpu_c,     "wgpu"      },
        { ddb_n,      ddb_f,      ddb_c,      "duckdb"    },
        { log_n,      log_f,      log_c,      "log"       },
        { fnt_n,      fnt_f,      fnt_c,      "font"      },
        { txt_n,      txt_f,      txt_c,      "text"      },
        { inp_n,      inp_f,      inp_c,      "input"     },
        { med_n,      med_f,      med_c,      "media"     },
        { clip_n,     clip_f,     clip_c,      "clipboard" },
        { slg_n,      slg_f,      slg_c,      "slang"     },
        { net_n,      net_f,      net_c,      "network"   },
        { ipc_names,  ipc_funcs,  ipc_count,  "ipc"       },
        { sdl_n,      sdl_f,      sdl_c,      "sdl"       },
    };
    size_t nprov = sizeof(providers) / sizeof(providers[0]);

    /* ── Resolve imports ── */
    wasm_importtype_vec_t imp_types;
    wasm_module_imports(rt->module, &imp_types);
    wasm_extern_t **imp_ext = calloc(imp_types.size, sizeof(wasm_extern_t *));

    for (size_t i = 0; i < imp_types.size; i++) {
        const wasm_name_t *mod  = wasm_importtype_module(imp_types.data[i]);
        const wasm_name_t *name = wasm_importtype_name(imp_types.data[i]);
        bool found = false;

        if (mod->size != 3 || memcmp(mod->data, "env", 3) != 0) {
            fprintf(stderr, "[webapp] Unresolved '%.*s.%.*s'\n",
                    (int)mod->size, mod->data, (int)name->size, name->data);
            free(imp_ext);
            wasm_importtype_vec_delete(&imp_types);
            return false;
        }

        /* Simple host fns (gpu_clear etc.) */
        if (!found) {
            const HostFn *hf = find_host_fn(name->data, name->size);
            if (hf) {
                wasm_functype_t *ft = make_functype(
                    hf->np, (wasm_valkind_t *)hf->params,
                    hf->nr, (wasm_valkind_t *)hf->results);
                imp_ext[i] = wasm_func_as_extern(
                    wasm_func_new_with_env(rt->store, ft, hf->cb, rt->wgpu.gpu, NULL));
                wasm_functype_delete(ft);
                found = true;
                printf("[webapp]   bound env.%.*s (simple)\n",
                       (int)name->size, name->data);
            }
        }

        /* Provider scan */
        for (size_t p = 0; p < nprov && !found; p++) {
            for (size_t j = 0; j < providers[p].count; j++) {
                if (providers[p].names[j] &&
                    strlen(providers[p].names[j]) == name->size &&
                    memcmp(providers[p].names[j], name->data, name->size) == 0) {
                    imp_ext[i] = wasm_func_as_extern(providers[p].funcs[j]);
                    found = true;
                    printf("[webapp]   bound env.%.*s (%s)\n",
                           (int)name->size, name->data, providers[p].tag);
                    break;
                }
            }
        }

        if (!found) {
            fprintf(stderr, "[webapp] Unresolved 'env.%.*s'\n",
                    (int)name->size, name->data);
            free(imp_ext);
            wasm_importtype_vec_delete(&imp_types);
            return false;
        }
    }

    /* ── Instantiate ── */
    wasm_extern_vec_t imp_vec = { .size = imp_types.size, .data = imp_ext };
    wasm_trap_t *trap = NULL;
    rt->instance = wasm_instance_new(rt->store, rt->module, &imp_vec, &trap);
    if (!rt->instance) {
        fprintf(stderr, "[webapp] Instantiation failed\n");
        if (trap) {
            wasm_message_t m; wasm_trap_message(trap, &m);
            fprintf(stderr, "  trap: %.*s\n", (int)m.size, m.data);
            wasm_byte_vec_delete(&m); wasm_trap_delete(trap);
        }
        free(imp_ext);
        wasm_importtype_vec_delete(&imp_types);
        return false;
    }
    wasm_importtype_vec_delete(&imp_types);
    free(imp_ext);
    free(ipc_names);
    free(ipc_funcs);

    /* ── Linear memory — distribute to per-instance bindings ── */
    wasm_extern_vec_t exports;
    wasm_instance_exports(rt->instance, &exports);
    for (size_t i = 0; i < exports.size; i++) {
        wasm_memory_t *mem = wasm_extern_as_memory(exports.data[i]);
        if (mem) {
            rt->memory = mem;
            wgpu_bindings_set_memory(&rt->wgpu, mem);
            clipboard_bindings_set_memory(&rt->clipboard, mem);
            if (rt->duckdb_active) duckdb_bindings_set_memory(&rt->duckdb, mem);
            log_bindings_set_memory(&rt->log, mem);
            font_bindings_set_memory(&rt->font, mem);
            text_bindings_set_memory(&rt->text, mem);
            input_bindings_set_memory(&rt->input, mem);
            media_bindings_set_memory(&rt->media, mem);
            slang_bindings_set_memory(&rt->slang, mem);
            sdl_bindings_set_memory(&rt->sdl, mem);
            if (!rt->is_dashboard)
                network_bindings_set_memory(&rt->network, mem);
            printf("[webapp]   found linear memory\n");
            break;
        }
    }

    /* ── Export discovery ── */
    wasm_exporttype_vec_t exp_types;
    wasm_module_exports(rt->module, &exp_types);
    for (size_t i = 0; i < exp_types.size && i < exports.size; i++) {
        const wasm_name_t *n = wasm_exporttype_name(exp_types.data[i]);
        wasm_func_t *fn = wasm_extern_as_func(exports.data[i]);
        if (!fn) continue;
        for (size_t e = 0; e < NUM_GUEST_EXPORTS; e++) {
            if (n->size == GUEST_EXPORTS[e].name_len &&
                memcmp(n->data, GUEST_EXPORTS[e].name, GUEST_EXPORTS[e].name_len) == 0) {
                *(wasm_func_t **)((char *)rt + GUEST_EXPORTS[e].off) = fn;
                printf("[webapp]   export '%s' bound\n", GUEST_EXPORTS[e].name);
                break;
            }
        }
        if (rt->is_dashboard) continue;
        for (size_t e = 0; e < NUM_NET_GUEST_EXPORTS; e++) {
            if (n->size == NET_GUEST_EXPORTS[e].name_len &&
                memcmp(n->data, NET_GUEST_EXPORTS[e].name,
                       NET_GUEST_EXPORTS[e].name_len) == 0) {
                *(wasm_func_t **)((char *)&rt->network +
                                  NET_GUEST_EXPORTS[e].off) = fn;
                printf("[webapp]   export '%s' bound (network)\n",
                       NET_GUEST_EXPORTS[e].name);
                break;
            }
        }
    }
    wasm_exporttype_vec_delete(&exp_types);

    if (!rt->fn_frame) {
        fprintf(stderr, "[webapp] No 'frame' export\n");
        return false;
    }
    printf("[webapp] Loaded '%s'\n", wasm_path);
    return true;
}

void webapp_runtime_set_viewport(WebAppRuntime *rt, const WebAppViewport *vp) {
    rt->viewport = *vp;
    if (vp->w > 0 && vp->h > 0) {
        gpu_resize_offscreen_texture(rt->wgpu.gpu,
                                     (uint32_t)vp->w, (uint32_t)vp->h,
                                     &rt->offscreen_tex, &rt->offscreen_view);
        /* Point the wgpu bindings at the offscreen surface.
           fn_surface_get_view will return this instead of the swapchain. */
        rt->wgpu.offscreen          = true;
        rt->wgpu.offscreen_texture  = rt->offscreen_tex;
        rt->wgpu.offscreen_view     = rt->offscreen_view;
    }
    /* Notify the guest of the new size only after init() has run.
       Deferring prevents null-deref in guest handlers that allocate
       state (e.g. layout context) inside init(). */
    if (rt->init_called)
        webapp_dispatch_resize(rt, vp->w, vp->h);
}

void webapp_runtime_call_init(WebAppRuntime *rt) {
    if (!rt->fn_init) { rt->init_called = true; return; }
    wasm_val_vec_t a = WASM_EMPTY_VEC, r = WASM_EMPTY_VEC;
    wasm_trap_t *t = wasm_func_call(rt->fn_init, &a, &r);
    if (t) {
        wasm_message_t m; wasm_trap_message(t, &m);
        fprintf(stderr, "[webapp] init trap: %.*s\n", (int)m.size, m.data);
        wasm_byte_vec_delete(&m); wasm_trap_delete(t);
        return;
    }
    rt->init_called = true;
    /* Now that init() has run, send the deferred resize so the guest
       knows its initial viewport size. */
    webapp_dispatch_resize(rt, rt->viewport.w, rt->viewport.h);
}

void webapp_runtime_call_frame(WebAppRuntime *rt) {
    if (!rt->fn_frame) return;
    wasm_val_vec_t a = WASM_EMPTY_VEC, r = WASM_EMPTY_VEC;
    wasm_trap_t *t = wasm_func_call(rt->fn_frame, &a, &r);
    if (t) {
        wasm_message_t m; wasm_trap_message(t, &m);
        fprintf(stderr, "[webapp] frame trap: %.*s\n", (int)m.size, m.data);
        wasm_byte_vec_delete(&m); wasm_trap_delete(t);
    }
}

void webapp_runtime_destroy(WebAppRuntime *rt) {
    /* Per-instance bindings */
    wgpu_bindings_destroy(&rt->wgpu);
    clipboard_bindings_destroy(&rt->clipboard);
    log_bindings_destroy(&rt->log);
    font_bindings_destroy(&rt->font);
    text_bindings_destroy(&rt->text);
    input_bindings_destroy(&rt->input);
    media_bindings_destroy(&rt->media);
    slang_bindings_destroy(&rt->slang);
    if (!rt->is_dashboard)
        network_bindings_destroy(&rt->network);
    if (rt->wff_bridge) { wff_bridge_destroy(rt->wff_bridge); rt->wff_bridge = NULL; }

    /* Per-webapp DuckDB */
    if (rt->duckdb_active) {
        duckdb_bindings_destroy(&rt->duckdb);
        duckdb_disconnect(&rt->duckdb_conn);
        duckdb_close(&rt->duckdb_db);
        rt->duckdb_active = false;
    }

    /* Offscreen texture */
    gpu_destroy_offscreen_texture(&rt->offscreen_tex, &rt->offscreen_view);

    /* Wasmer */
    if (rt->instance) wasm_instance_delete(rt->instance);
    if (rt->module)   wasm_module_delete(rt->module);
    if (rt->store)    wasm_store_delete(rt->store);
    if (rt->engine)   wasm_engine_delete(rt->engine);

    memset(rt, 0, sizeof(*rt));
}

/* ------------------------------------------------------------------ */
/*  Event dispatch                                                     */
/* ------------------------------------------------------------------ */

static void handle_trap(wasm_trap_t *trap, const char *name) {
    if (!trap) return;
    wasm_message_t m; wasm_trap_message(trap, &m);
    fprintf(stderr, "[webapp] %s trap: %.*s\n", name, (int)m.size, m.data);
    wasm_byte_vec_delete(&m); wasm_trap_delete(trap);
}

#define WVAL_I32(v) ((wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)(v) })
#define WVAL_F32(v) ((wasm_val_t){ .kind = WASM_F32, .of.f32 = (float)(v) })

void webapp_dispatch_key(WebAppRuntime *rt, int sc, int kc, int mod, int pressed) {
    if (!rt->fn_on_key) return;
    wasm_val_t a[] = { WVAL_I32(sc), WVAL_I32(kc), WVAL_I32(mod), WVAL_I32(pressed) };
    wasm_val_vec_t args = { .size=4, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_key, &args, &res), "on_key");
}

void webapp_dispatch_char(WebAppRuntime *rt, uint32_t cp) {
    if (!rt->fn_on_char) return;
    wasm_val_t a[] = { WVAL_I32(cp) };
    wasm_val_vec_t args = { .size=1, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_char, &args, &res), "on_char");
}

void webapp_dispatch_mouse_button(WebAppRuntime *rt, int btn, int pressed, float x, float y) {
    if (!rt->fn_on_mouse_button) return;
    wasm_val_t a[] = { WVAL_I32(btn), WVAL_I32(pressed), WVAL_F32(x), WVAL_F32(y) };
    wasm_val_vec_t args = { .size=4, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_mouse_button, &args, &res), "on_mouse_button");
}

void webapp_dispatch_mouse_motion(WebAppRuntime *rt, float x, float y, float dx, float dy, int btns) {
    if (!rt->fn_on_mouse_motion) return;
    wasm_val_t a[] = { WVAL_F32(x), WVAL_F32(y), WVAL_F32(dx), WVAL_F32(dy), WVAL_I32(btns) };
    wasm_val_vec_t args = { .size=5, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_mouse_motion, &args, &res), "on_mouse_motion");
}

void webapp_dispatch_mouse_wheel(WebAppRuntime *rt, float dx, float dy) {
    if (!rt->fn_on_mouse_wheel) return;
    wasm_val_t a[] = { WVAL_F32(dx), WVAL_F32(dy) };
    wasm_val_vec_t args = { .size=2, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_mouse_wheel, &args, &res), "on_mouse_wheel");
}

void webapp_dispatch_touch(WebAppRuntime *rt, int fid, int type, float x, float y, float p) {
    if (!rt->fn_on_touch) return;
    wasm_val_t a[] = { WVAL_I32(fid), WVAL_I32(type), WVAL_F32(x), WVAL_F32(y), WVAL_F32(p) };
    wasm_val_vec_t args = { .size=5, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_touch, &args, &res), "on_touch");
}

void webapp_dispatch_joystick_added(WebAppRuntime *rt, int id) {
    if (!rt->fn_on_joystick_added) return;
    wasm_val_t a[] = { WVAL_I32(id) };
    wasm_val_vec_t args = { .size=1, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_joystick_added, &args, &res), "on_joystick_added");
}

void webapp_dispatch_joystick_removed(WebAppRuntime *rt, int id) {
    if (!rt->fn_on_joystick_removed) return;
    wasm_val_t a[] = { WVAL_I32(id) };
    wasm_val_vec_t args = { .size=1, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_joystick_removed, &args, &res), "on_joystick_removed");
}

void webapp_dispatch_joystick_axis(WebAppRuntime *rt, int id, int axis, int val) {
    if (!rt->fn_on_joystick_axis) return;
    wasm_val_t a[] = { WVAL_I32(id), WVAL_I32(axis), WVAL_I32(val) };
    wasm_val_vec_t args = { .size=3, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_joystick_axis, &args, &res), "on_joystick_axis");
}

void webapp_dispatch_joystick_button(WebAppRuntime *rt, int id, int btn, int pressed) {
    if (!rt->fn_on_joystick_button) return;
    wasm_val_t a[] = { WVAL_I32(id), WVAL_I32(btn), WVAL_I32(pressed) };
    wasm_val_vec_t args = { .size=3, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_joystick_button, &args, &res), "on_joystick_button");
}

void webapp_dispatch_joystick_hat(WebAppRuntime *rt, int id, int hat, int val) {
    if (!rt->fn_on_joystick_hat) return;
    wasm_val_t a[] = { WVAL_I32(id), WVAL_I32(hat), WVAL_I32(val) };
    wasm_val_vec_t args = { .size=3, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_joystick_hat, &args, &res), "on_joystick_hat");
}

void webapp_dispatch_gamepad_added(WebAppRuntime *rt, int id) {
    if (!rt->fn_on_gamepad_added) return;
    wasm_val_t a[] = { WVAL_I32(id) };
    wasm_val_vec_t args = { .size=1, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_gamepad_added, &args, &res), "on_gamepad_added");
}

void webapp_dispatch_gamepad_removed(WebAppRuntime *rt, int id) {
    if (!rt->fn_on_gamepad_removed) return;
    wasm_val_t a[] = { WVAL_I32(id) };
    wasm_val_vec_t args = { .size=1, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_gamepad_removed, &args, &res), "on_gamepad_removed");
}

void webapp_dispatch_resize(WebAppRuntime *rt, int w, int h) {
    if (!rt->fn_on_resize) return;
    wasm_val_t a[] = { WVAL_I32(w), WVAL_I32(h) };
    wasm_val_vec_t args = { .size=2, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_resize, &args, &res), "on_resize");
}

void webapp_dispatch_focus(WebAppRuntime *rt, int gained) {
    if (!rt->fn_on_focus) return;
    wasm_val_t a[] = { WVAL_I32(gained) };
    wasm_val_vec_t args = { .size=1, .data=a }; wasm_val_vec_t res = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_focus, &args, &res), "on_focus");
}

void webapp_runtime_pump_audio(WebAppRuntime *rt) {
    media_bindings_pump_audio(&rt->media);
}

/* ------------------------------------------------------------------ */
/*  DuckDB quota API (dashboard-facing)                                */
/* ------------------------------------------------------------------ */

void webapp_runtime_set_db_quota(WebAppRuntime *rt, uint64_t quota_bytes) {
    if (!rt || !rt->duckdb_active) return;
    duckdb_bindings_set_quota(&rt->duckdb, quota_bytes);
}

uint64_t webapp_runtime_get_db_size(const WebAppRuntime *rt) {
    if (!rt || !rt->duckdb_active) return 0;
    return duckdb_bindings_get_db_size(&rt->duckdb);
}

uint64_t webapp_runtime_get_db_quota(const WebAppRuntime *rt) {
    if (!rt || !rt->duckdb_active) return 0;
    return duckdb_bindings_get_quota(&rt->duckdb);
}

void webapp_runtime_notify_db_quota_changed(WebAppRuntime *rt,
                                            uint64_t new_quota_bytes) {
    if (!rt || !rt->fn_on_db_quota_changed) return;
    wasm_val_t a[] = {
        (wasm_val_t){ .kind = WASM_I64, .of.i64 = (int64_t)new_quota_bytes }
    };
    wasm_val_vec_t args = { .size = 1, .data = a };
    wasm_val_vec_t res  = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_db_quota_changed, &args, &res),
                "on_db_quota_changed");
}

/* Write a payload to a scratch area at the top of guest memory and
 * return the guest pointer to it.  Returns 0 on failure. */
static uint32_t stage_scratch(WebAppRuntime *rt,
                              const void *data, size_t len) {
    size_t mem_sz = wasm_mem_size(rt);
    if (mem_sz < len + 32) return 0;
    uint32_t ptr = (uint32_t)(mem_sz - len - 16);
    ptr &= ~(uint32_t)15;
    memcpy(wasm_mem(rt) + ptr, data, len);
    return ptr;
}

void webapp_runtime_notify_group_switched(WebAppRuntime *rt,
                                          const uint8_t group_id[32]) {
    if (!rt || !rt->fn_on_group_switched) return;
    uint32_t ptr = stage_scratch(rt, group_id, 32);
    if (!ptr) return;
    wasm_val_t a[] = { (wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)ptr } };
    wasm_val_vec_t args = { .size = 1, .data = a };
    wasm_val_vec_t res  = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_group_switched, &args, &res),
                "on_group_switched");
}

void webapp_runtime_notify_group_background(WebAppRuntime *rt,
                                            const uint8_t group_id[32],
                                            bool background) {
    if (!rt || !rt->fn_on_group_background_changed) return;
    uint32_t ptr = stage_scratch(rt, group_id, 32);
    if (!ptr) return;
    wasm_val_t a[] = {
        (wasm_val_t){ .kind = WASM_I32, .of.i32 = (int32_t)ptr },
        (wasm_val_t){ .kind = WASM_I32, .of.i32 = background ? 1 : 0 },
    };
    wasm_val_vec_t args = { .size = 2, .data = a };
    wasm_val_vec_t res  = WASM_EMPTY_VEC;
    handle_trap(wasm_func_call(rt->fn_on_group_background_changed, &args, &res),
                "on_group_background_changed");
}
