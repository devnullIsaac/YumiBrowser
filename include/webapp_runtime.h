/*
    Sandboxed WASM Runtime for Individual Webapps
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
 * @file webapp_runtime.h
 * @brief Sandboxed WASM runtime for individual webapps.
 *
 * Each webapp runs in its own Wasmer instance with per-instance
 * bindings.  It renders to an offscreen texture handed out by the
 * DashboardRuntime, which composites it onto the real swapchain.
 *
 * Webapps do NOT have direct access to:
 *   - SDL window / audio devices / joystick creation
 *   - File system
 *
 * Webapps DO have:
 *   - WGPU bindings (offscreen)
 *   - Per-webapp DuckDB (sandboxed)
 *   - Font / text shaping
 *   - Video / image decode
 *   - Logging
 *   - Input (translated coords from dashboard)
 *   - Dashboard IPC imports for clipboard, file dialog, friend, links
 */

#ifndef WEBAPP_RUNTIME_H
#define WEBAPP_RUNTIME_H

#include "deps.h"
#include "gpu.h"
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
#include "sdl_bindings.h"
#include "wgpu_ffmpeg.h"
#include "deps/duckdb/src/include/duckdb.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declaration ─────────────────────────────────────────── */

typedef struct DashboardRuntime DashboardRuntime;

/* ── Intra-group link buffer ─────────────────────────────────────── */

#define WEBAPP_LINK_BUF_SIZE  4096

/* ── Viewport ────────────────────────────────────────────────────── */

typedef struct {
    int x, y, w, h;
} WebAppViewport;

/* ── Per-webapp runtime ──────────────────────────────────────────── */

typedef struct WebAppRuntime {
    /* ── Back-pointer to supervisor ── */
    DashboardRuntime *dashboard;
    uint32_t          group_index;   /**< Index into dashboard's group array. */
    bool              is_dashboard;  /**< True only for the dashboard webapp slot. */

    /* ── Wasmer instance ── */
    wasm_engine_t   *engine;
    wasm_store_t    *store;
    wasm_module_t   *module;
    wasm_instance_t *instance;
    wasm_memory_t   *memory;        /**< Guest linear memory (for IPC read/write). */

    /* ── Guest exports (required) ── */
    wasm_func_t *fn_frame;

    /* ── Guest exports (optional lifecycle) ── */
    wasm_func_t *fn_init;

    /* ── Guest exports (optional input) ── */
    wasm_func_t *fn_on_key;
    wasm_func_t *fn_on_char;
    wasm_func_t *fn_on_mouse_button;
    wasm_func_t *fn_on_mouse_motion;
    wasm_func_t *fn_on_mouse_wheel;
    wasm_func_t *fn_on_touch;
    wasm_func_t *fn_on_joystick_added;
    wasm_func_t *fn_on_joystick_removed;
    wasm_func_t *fn_on_joystick_axis;
    wasm_func_t *fn_on_joystick_button;
    wasm_func_t *fn_on_joystick_hat;
    wasm_func_t *fn_on_gamepad_added;
    wasm_func_t *fn_on_gamepad_removed;

    /* ── Guest exports (optional window) ── */
    wasm_func_t *fn_on_resize;
    wasm_func_t *fn_on_focus;

    /* ── Guest exports (optional IPC results) ── */
    wasm_func_t *fn_on_file_result;         /**< (info_ptr) → DashboardFileInfo */
    wasm_func_t *fn_on_folder_result;       /**< (scan_handle) → folder_scan_t */
    wasm_func_t *fn_on_paste_result;        /**< (info_ptr) → DashboardPasteInfo */
    wasm_func_t *fn_on_friend_list_result;  /**< (peer_ids_ptr, count) */
    wasm_func_t *fn_on_db_quota_changed;    /**< (new_quota_bytes: i64) */
    wasm_func_t *fn_on_group_switched;      /**< (new_group_id_ptr: i32) */
    wasm_func_t *fn_on_group_background_changed; /**< (group_id_ptr: i32, bg: i32) */

    /* ── Lifecycle gate ── */
    bool               init_called;  /**< true after fn_init has executed */

    /* ── Per-instance bindings (not globals) ── */
    WgpuBindings      wgpu;
    ClipboardBindings  clipboard;
    DuckdbBindings     duckdb;
    duckdb_database    duckdb_db;
    duckdb_connection  duckdb_conn;
    bool               duckdb_active;
    LogBindings        log;
    FontBindings       font;
    TextBindings       text;
    InputBindings      input;
    MediaBindings      media;
    SlangBindings      slang;
    NetworkBindings    network;
    SdlBindings        sdl;
    WFFBridge         *wff_bridge;

    /* ── Offscreen rendering surface ── */
    WGPUTexture     offscreen_tex;
    WGPUTextureView offscreen_view;
    WebAppViewport  viewport;
    bool            focused;
} WebAppRuntime;

/* ── Lifecycle ───────────────────────────────────────────────────── */

/**
 * @brief Initialize a webapp runtime.
 *
 * Creates the Wasmer engine/store, opens a sandboxed per-webapp
 * DuckDB, and prepares per-instance bindings.  Does NOT load a
 * module yet — call webapp_runtime_load() after this.
 *
 * @param[out] rt           Runtime to initialize.
 * @param[in]  dashboard    Owning dashboard (for IPC callbacks).
 * @param[in]  gpu          GPU context (shared, owned by dashboard).
 * @param[in]  group_index  Index into dashboard's group array.
 * @param[in]  db_path      Path to per-webapp DuckDB file (sandboxed).
 * @param[in]  is_dashboard True when this runtime hosts the dashboard
 *                          webapp itself.  Grants the privileged
 *                          dashboard IPC surface (group mgmt / GR
 *                          proxy / audit / behavior / blocklist) and
 *                          withholds both the webapp→dashboard request
 *                          surface and peer-to-peer networking.
 * @return true on success.
 */
bool webapp_runtime_init(WebAppRuntime *rt,
                         DashboardRuntime *dashboard,
                         GpuContext *gpu,
                         uint32_t group_index,
                         const char *db_path,
                         bool is_dashboard);

/**
 * @brief Load and instantiate a WASM module.
 *
 * Compiles the module, resolves the restricted import set (no SDL,
 * no clipboard — replaced by dashboard IPC imports), and discovers
 * guest exports.
 *
 * @param[in,out] rt         Initialized runtime.
 * @param[in]     win        SDL window (for InputBindings text input API only).
 * @param[in]     wasm_path  Path to the .wasm file.
 * @return true on success.
 */
bool webapp_runtime_load(WebAppRuntime *rt,
                         SDL_Window *win,
                         const char *wasm_path);

/**
 * @brief Assign or resize the offscreen texture for this webapp.
 *
 * Called by the dashboard's tiling manager whenever the slot's
 * viewport changes.
 *
 * @param[in,out] rt  Runtime.
 * @param[in]     vp  New viewport (position + size in window coords).
 */
void webapp_runtime_set_viewport(WebAppRuntime *rt,
                                 const WebAppViewport *vp);

/** @brief Call the guest's `init` export (if present). */
void webapp_runtime_call_init(WebAppRuntime *rt);

/** @brief Call the guest's `frame` export. */
void webapp_runtime_call_frame(WebAppRuntime *rt);

/**
 * @brief Set / update this webapp's DuckDB on-disk quota.
 *
 * The dashboard may call this at any time to grow or shrink the
 * sandboxed database's ceiling.  A value of 0 means unlimited.
 * When the quota is reached, further writer statements (INSERT /
 * UPDATE / CREATE / etc.) and appender ops are rejected with a
 * sandbox error; the webapp must remove content or prompt the user
 * to request a larger quota from the dashboard.
 *
 * @param[in,out] rt            Runtime.
 * @param[in]     quota_bytes   New quota in bytes (0 => unlimited).
 */
void webapp_runtime_set_db_quota(WebAppRuntime *rt, uint64_t quota_bytes);

/** @brief Current on-disk usage (main file + WAL), in bytes. */
uint64_t webapp_runtime_get_db_size(const WebAppRuntime *rt);

/** @brief Current quota (bytes; 0 => unlimited). */
uint64_t webapp_runtime_get_db_quota(const WebAppRuntime *rt);

/**
 * @brief Invoke the guest's optional `on_db_quota_changed` export.
 *
 * Called by the dashboard after applying a quota change so the webapp
 * can react (shrink caches, surface UI, etc.).  Safe no-op if the
 * guest does not export the symbol.
 */
void webapp_runtime_notify_db_quota_changed(WebAppRuntime *rt,
                                            uint64_t new_quota_bytes);

/** @brief Invoke `on_group_switched(group_id_ptr)` if exported.
 *         The 32-byte group id is placed in guest memory scratch. */
void webapp_runtime_notify_group_switched(WebAppRuntime *rt,
                                          const uint8_t group_id[32]);

/** @brief Invoke `on_group_background_changed(group_id_ptr, bg)` if exported. */
void webapp_runtime_notify_group_background(WebAppRuntime *rt,
                                            const uint8_t group_id[32],
                                            bool background);

/** @brief Destroy the runtime, per-instance bindings, and DuckDB. */
void webapp_runtime_destroy(WebAppRuntime *rt);

/* ── Event dispatch (coordinates are slot-local) ─────────────────── */

void webapp_dispatch_key(WebAppRuntime *rt,
                         int scancode, int keycode, int mod, int pressed);
void webapp_dispatch_char(WebAppRuntime *rt, uint32_t codepoint);
void webapp_dispatch_mouse_button(WebAppRuntime *rt,
                                  int button, int pressed, float x, float y);
void webapp_dispatch_mouse_motion(WebAppRuntime *rt,
                                  float x, float y,
                                  float dx, float dy, int buttons);
void webapp_dispatch_mouse_wheel(WebAppRuntime *rt, float dx, float dy);
void webapp_dispatch_touch(WebAppRuntime *rt,
                           int finger_id, int type,
                           float x, float y, float pressure);
void webapp_dispatch_joystick_added(WebAppRuntime *rt, int instance_id);
void webapp_dispatch_joystick_removed(WebAppRuntime *rt, int instance_id);
void webapp_dispatch_joystick_axis(WebAppRuntime *rt,
                                   int instance_id, int axis, int value);
void webapp_dispatch_joystick_button(WebAppRuntime *rt,
                                     int instance_id, int button, int pressed);
void webapp_dispatch_joystick_hat(WebAppRuntime *rt,
                                  int instance_id, int hat, int value);
void webapp_dispatch_gamepad_added(WebAppRuntime *rt, int instance_id);
void webapp_dispatch_gamepad_removed(WebAppRuntime *rt, int instance_id);
void webapp_dispatch_resize(WebAppRuntime *rt, int w, int h);
void webapp_dispatch_focus(WebAppRuntime *rt, int gained);

/* ── Audio pump (video playback) ─────────────────────────────────── */

void webapp_runtime_pump_audio(WebAppRuntime *rt);

#ifdef __cplusplus
}
#endif

#endif /* WEBAPP_RUNTIME_H */
