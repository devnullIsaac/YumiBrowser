/*
    Yumi SDK — Standalone Webapp Template
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
 * @file webapp_template.c
 * @brief Template for a standalone Yumi Browser webapp (WASM guest).
 *
 * This template includes all available SDK headers and stubs out every
 * export callback the host recognises.  Delete or leave empty the
 * callbacks you don't need — only frame() is required.
 *
 * A "webapp" runs in a standalone WebAppRuntime without dashboard IPC.
 * It uses SDL bindings directly for input, clipboard, and window
 * management.  For apps running inside a dashboard slot, use the
 * dashboard_app_template.c instead.
 *
 * Build with your WASI C toolchain, e.g.:
 *   clang --target=wasm32-wasip1 -O2 -o myapp.wasm webapp_template.c
 */

/* ── SDK headers ──────────────────────────────────────────────────── */

#include "wasm_gpu.h"           /* WebGPU device, buffers, pipelines, textures */
#include "wasm_surface.h"       /* Layer compositing over WGPU */
#include "wasm_sdl.h"           /* SDL3 input, joystick, gamepad, audio */
#include "wasm_font.h"          /* Font loading and glyph rasterisation */
#include "wasm_text.h"          /* Text shaping (HarfBuzz), bidi, line breaks */
#include "wasm_input.h"         /* Keyboard / mouse / gamepad polling */
#include "wasm_clipboard.h"     /* System clipboard access */
#include "wasm_file_types.h"    /* DashboardFileInfo, DashboardFolderEntry, DashboardPasteInfo */
#include "wasm_webapp.h"        /* Webapp → dashboard IPC (clipboard, dialogs, friends, notify) */
#include "wasm_ddb.h"           /* Per-webapp sandboxed DuckDB */
#include "wasm_log.h"           /* Host-side logging */
#include "wasm_media.h"         /* Unified image / video / audio decode */
#include "wasm_network.h"       /* Peer-to-peer messaging and data shards */

#include <stdint.h>
#include <string.h>

/* ================================================================== */
/*  Required export                                                    */
/* ================================================================== */

/**
 * @brief Called every frame by the host runtime.
 *
 * This is the only required export.  Do your rendering and logic here.
 */
__attribute__((export_name("frame")))
void frame(void) {
    /* TODO: render your app */
}

/* ================================================================== */
/*  Optional lifecycle                                                 */
/* ================================================================== */

/**
 * @brief Called once after the WASM module is instantiated.
 *
 * Use this for one-time setup: load fonts, create GPU pipelines,
 * open database tables, etc.
 */
__attribute__((export_name("init")))
void init(void) {
    /* TODO: initialise your app */
}

/* ================================================================== */
/*  Optional input callbacks                                           */
/* ================================================================== */

/**
 * @brief Physical key press / release.
 *
 * @param scancode  SDL scancode.
 * @param keycode   SDL keycode (virtual key).
 * @param mod       Modifier bitmask (shift, ctrl, alt, …).
 * @param pressed   1 = key down, 0 = key up.
 */
__attribute__((export_name("on_key")))
void on_key(int scancode, int keycode, int mod, int pressed) {
    (void)scancode; (void)keycode; (void)mod; (void)pressed;
}

/**
 * @brief Text input (committed codepoint after IME processing).
 *
 * @param codepoint  Unicode codepoint.
 */
__attribute__((export_name("on_char")))
void on_char(uint32_t codepoint) {
    (void)codepoint;
}

/**
 * @brief Mouse button press / release.
 *
 * Coordinates are relative to the webapp's viewport.
 *
 * @param button   Button index (1 = left, 2 = middle, 3 = right).
 * @param pressed  1 = pressed, 0 = released.
 * @param x        X position in viewport pixels.
 * @param y        Y position in viewport pixels.
 */
__attribute__((export_name("on_mouse_button")))
void on_mouse_button(int button, int pressed, float x, float y) {
    (void)button; (void)pressed; (void)x; (void)y;
}

/**
 * @brief Mouse motion.
 *
 * @param x     Absolute X in viewport pixels.
 * @param y     Absolute Y in viewport pixels.
 * @param dx    Delta X since last motion event.
 * @param dy    Delta Y since last motion event.
 * @param btns  Bitmask of currently pressed buttons.
 */
__attribute__((export_name("on_mouse_motion")))
void on_mouse_motion(float x, float y, float dx, float dy, int btns) {
    (void)x; (void)y; (void)dx; (void)dy; (void)btns;
}

/**
 * @brief Mouse wheel scroll.
 *
 * @param dx  Horizontal scroll amount.
 * @param dy  Vertical scroll amount.
 */
__attribute__((export_name("on_mouse_wheel")))
void on_mouse_wheel(float dx, float dy) {
    (void)dx; (void)dy;
}

/**
 * @brief Touch event (mobile / touchscreen).
 *
 * @param finger_id  Unique finger identifier.
 * @param type       0 = down, 1 = up, 2 = motion.
 * @param x          X position (normalised or pixels, depending on host).
 * @param y          Y position.
 * @param pressure   Contact pressure (0.0–1.0).
 */
__attribute__((export_name("on_touch")))
void on_touch(int finger_id, int type, float x, float y, float pressure) {
    (void)finger_id; (void)type; (void)x; (void)y; (void)pressure;
}

/* ── Joystick ─────────────────────────────────────────────────────── */

__attribute__((export_name("on_joystick_added")))
void on_joystick_added(int id) { (void)id; }

__attribute__((export_name("on_joystick_removed")))
void on_joystick_removed(int id) { (void)id; }

__attribute__((export_name("on_joystick_axis")))
void on_joystick_axis(int id, int axis, int value) {
    (void)id; (void)axis; (void)value;
}

__attribute__((export_name("on_joystick_button")))
void on_joystick_button(int id, int button, int pressed) {
    (void)id; (void)button; (void)pressed;
}

__attribute__((export_name("on_joystick_hat")))
void on_joystick_hat(int id, int hat, int value) {
    (void)id; (void)hat; (void)value;
}

/* ── Gamepad ──────────────────────────────────────────────────────── */

__attribute__((export_name("on_gamepad_added")))
void on_gamepad_added(int id) { (void)id; }

__attribute__((export_name("on_gamepad_removed")))
void on_gamepad_removed(int id) { (void)id; }

/* ================================================================== */
/*  Optional window callbacks                                          */
/* ================================================================== */

/**
 * @brief Viewport was resized.
 *
 * @param width   New width in pixels.
 * @param height  New height in pixels.
 */
__attribute__((export_name("on_resize")))
void on_resize(int width, int height) {
    (void)width; (void)height;
}

/**
 * @brief Focus gained or lost.
 *
 * @param gained  1 = gained focus, 0 = lost focus.
 */
__attribute__((export_name("on_focus")))
void on_focus(int gained) {
    (void)gained;
}

/* ================================================================== */
/*  Optional IPC result callbacks                                      */
/* ================================================================== */

/**
 * @brief Result from a file-open or file-save dialog.
 *
 * @param info  Pointer to a DashboardFileInfo struct in WASM memory.
 *              info->handle is 0 if the user cancelled.
 */
__attribute__((export_name("on_file_result")))
void on_file_result(const DashboardFileInfo *info) {
    (void)info;
}

/**
 * @brief Result from a folder-select dialog.
 *
 * @param scan  Opaque folder scan handle (0 if cancelled).
 */
__attribute__((export_name("on_folder_result")))
void on_folder_result(folder_scan_t scan) {
    (void)scan;
}

/**
 * @brief Result from an asynchronous paste request.
 *
 * @param info  Pointer to a DashboardPasteInfo struct in WASM memory.
 */
__attribute__((export_name("on_paste_result")))
void on_paste_result(const DashboardPasteInfo *info) {
    (void)info;
}

/* ================================================================== */
/*  Optional network callbacks                                         */
/* ------------------------------------------------------------------ */
/*  Invoked by the host runtime when peer traffic arrives.  Payload    */
/*  buffers are host-owned and valid only for the duration of the      */
/*  call — copy any bytes you want to retain beyond return.            */
/* ================================================================== */

/**
 * @brief Reliable unicast message from a peer.
 *
 * Mirrors a sender's @ref net_send call.  Retransmits / in-order
 * delivery relative to that same sender are already handled by the
 * runtime.  No ordering is guaranteed against broadcasts or against
 * messages from other peers.
 *
 * @param peer  Sender's 32-byte peer ID.
 * @param data  Pointer to payload bytes in WASM linear memory.
 * @param len   Payload length in bytes.
 */
__attribute__((export_name("on_net_recv")))
void on_net_recv(const YumiPeerId *peer, const void *data, uint32_t len) {
    (void)peer; (void)data; (void)len;
}

/**
 * @brief Unreliable unicast message from a peer.
 *
 * Mirrors a sender's @ref net_send_unreliable / @ref net_send_range_unreliable
 * call.  Messages on this channel may be dropped, duplicated, or
 * reordered.  Use for high-frequency telemetry where freshness beats
 * completeness.
 *
 * @param peer  Sender's 32-byte peer ID.
 * @param data  Pointer to payload bytes in WASM linear memory.
 * @param len   Payload length in bytes.
 */
__attribute__((export_name("on_net_recv_unreliable")))
void on_net_recv_unreliable(const YumiPeerId *peer,
                            const void *data, uint32_t len) {
    (void)peer; (void)data; (void)len;
}

/**
 * @brief Reliable group broadcast from a peer.
 *
 * Fired when another peer calls @ref net_broadcast.  The @p origin is
 * the original sender (not necessarily the peer that forwarded the
 * packet).
 *
 * @param origin  Origin peer ID.
 * @param data    Payload bytes in WASM linear memory.
 * @param len     Payload length in bytes.
 */
__attribute__((export_name("on_net_broadcast")))
void on_net_broadcast(const YumiPeerId *origin,
                      const void *data, uint32_t len) {
    (void)origin; (void)data; (void)len;
}

/**
 * @brief Unreliable group broadcast from a peer.
 *
 * Fired when another peer calls @ref net_broadcast_unreliable.
 * Delivery is best-effort — may be dropped, duplicated, or reordered.
 *
 * @param origin  Origin peer ID.
 * @param data    Payload bytes in WASM linear memory.
 * @param len     Payload length in bytes.
 */
__attribute__((export_name("on_net_broadcast_unreliable")))
void on_net_broadcast_unreliable(const YumiPeerId *origin,
                                 const void *data, uint32_t len) {
    (void)origin; (void)data; (void)len;
}

/**
 * @brief A peer joined or left the group.
 *
 * @param peer     Peer ID whose presence changed.
 * @param present  Non-zero = peer is now reachable, zero = peer went away.
 */
__attribute__((export_name("on_net_peer_presence")))
void on_net_peer_presence(const YumiPeerId *peer, int32_t present) {
    (void)peer; (void)present;
}

/**
 * @brief Asynchronous reply to @ref net_shard_list_request.
 *
 * May fire multiple times per request as the runtime discovers more
 * shards from peers / rebroadcasters; the final invocation has
 * @p is_final non-zero.
 *
 * @param request_id  Correlates with the @ref net_shard_list_request return value.
 * @param shard_ids   Array of @p count 64-bit shard IDs (host-owned).
 * @param count       Number of shard IDs in this batch.
 * @param is_final    Non-zero on the last batch for this request.
 */
__attribute__((export_name("on_net_shard_list")))
void on_net_shard_list(uint32_t request_id,
                       const uint64_t *shard_ids,
                       uint32_t count,
                       int32_t is_final) {
    (void)request_id; (void)shard_ids; (void)count; (void)is_final;
}

/**
 * @brief Incoming data chunk for an in-flight shard retrieval.
 *
 * Fires zero or more times before the terminal @ref on_net_shard_result.
 * The @p offset is relative to the start of the shard content and
 * increases monotonically for a given @p handle.
 *
 * @param handle    Handle returned by @ref net_shard_request.
 * @param data      Chunk bytes in WASM linear memory (host-owned).
 * @param len       Length of this chunk in bytes.
 * @param offset    Byte offset within the shard.
 * @param is_final  Non-zero on the last chunk.
 */
__attribute__((export_name("on_net_shard_chunk")))
void on_net_shard_chunk(uint32_t handle,
                        const void *data, uint32_t len,
                        uint32_t offset, int32_t is_final) {
    (void)handle; (void)data; (void)len; (void)offset; (void)is_final;
}

/**
 * @brief Terminal status for a shard transfer (upload or retrieval).
 *
 * Always fires exactly once per @p handle, after any @ref on_net_shard_chunk
 * invocations.  Use this to free per-transfer state.
 *
 * @param handle       Handle returned by @ref net_shard_request.
 * @param status       Terminal status (@ref net_shard_status_t).
 * @param total_bytes  Total bytes delivered (0 on failure before any chunks).
 */
__attribute__((export_name("on_net_shard_result")))
void on_net_shard_result(uint32_t handle,
                         net_shard_status_t status,
                         uint64_t total_bytes) {
    (void)handle; (void)status; (void)total_bytes;
}
