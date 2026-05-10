/*
    Yumi SDK — Dashboard Webapp Template
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
 * @file dashboard_app_template.c
 * @brief Template for a Yumi Browser dashboard webapp (WASM guest).
 *
 * This template includes all available SDK headers and stubs out every
 * export callback the dashboard host recognises.  Delete or leave empty
 * the callbacks you don't need — only frame() is required.
 *
 * Build with your WASI C toolchain, e.g.:
 *   clang --target=wasm32-wasip1 -O2 -o myapp.wasm dashboard_app_template.c
 */

/* ── SDK headers ──────────────────────────────────────────────────── */

#include "wasm_gpu.h"           /* WebGPU device, buffers, pipelines, textures */
#include "wasm_surface.h"       /* Layer compositing over WGPU */
#include "wasm_font.h"          /* Font loading and glyph rasterisation */
#include "wasm_text.h"          /* Text shaping (HarfBuzz), bidi, line breaks */
#include "wasm_input.h"         /* Keyboard / mouse / gamepad polling */
#include "wasm_clipboard.h"     /* Direct clipboard access (mediated by host) */
#include "wasm_dashboard.h"     /* Dashboard-privileged surface: groups, GR proxy, audit, blocklist */
#include "wasm_ddb.h"           /* Per-webapp sandboxed DuckDB */
#include "wasm_log.h"           /* Host-side logging */
#include "wasm_media.h"         /* Unified image / video / audio decode */

#include <stdint.h>
#include <string.h>

/* ================================================================== */
/*  Required export                                                    */
/* ================================================================== */

/**
 * @brief Called every frame by the dashboard host.
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
 * @brief Viewport was resized by the dashboard tiling layout.
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
 * @param gained  1 = this slot gained focus, 0 = lost focus.
 */
__attribute__((export_name("on_focus")))
void on_focus(int gained) {
    (void)gained;
}

/* ================================================================== */
/*  Optional dashboard surface callbacks                               */
/* ------------------------------------------------------------------ */
/*  These are delivered by the host to the dashboard webapp (and to    */
/*  any running webapp in the affected group, for gr_change and        */
/*  gr_behavior_alert).  Host-owned pointer targets — copy if you      */
/*  need to retain beyond the callback.                                */
/* ================================================================== */

/**
 * @brief The caller's DuckDB disk quota was changed.
 *
 * Fired when the dashboard (or another supervisor webapp) calls
 * @ref dashboard_webapp_set_db_quota targeting this webapp.
 *
 * @param new_quota_bytes  New ceiling in bytes (0 => unlimited).
 */
__attribute__((export_name("on_db_quota_changed")))
void on_db_quota_changed(uint64_t new_quota_bytes) {
    (void)new_quota_bytes;
}

/**
 * @brief The dashboard's foreground group was switched.
 *
 * Delivered to every running webapp in every group after a successful
 * @ref dashboard_switch_group call.
 *
 * @param new_group  The new foreground group (host-owned).
 */
__attribute__((export_name("on_group_switched")))
void on_group_switched(const YumiGroupId *new_group) {
    (void)new_group;
}

/**
 * @brief A group's background mode was toggled.
 *
 * Delivered when @ref dashboard_group_set_background changes a group's
 * background bit.  Webapps in that group should suspend / resume
 * foreground-only work accordingly.
 *
 * @param group       The affected group (host-owned).
 * @param background  Non-zero = now background, zero = now foreground.
 */
__attribute__((export_name("on_group_background_changed")))
void on_group_background_changed(const YumiGroupId *group,
                                 uint32_t background) {
    (void)group; (void)background;
}

/**
 * @brief A Group-Registrar mutation landed.
 *
 * Fired synchronously after every successful GR proxy write (peer
 * kick/ban/role, role edit, webapp manifest, server, epoch rotation,
 * retention, icon, invite, boot-IP block).  Only the webapps running
 * in the affected group receive it; background webapps receive it too.
 *
 * @param category     Coarse classification (@c YUMI_GR_CAT_*).
 * @param change_type  Fine-grained change type (@c YUMI_CHANGE_*).
 * @param group        The affected group (host-owned).
 * @param entry        The audit entry just written (host-owned).
 */
__attribute__((export_name("on_gr_change")))
void on_gr_change(uint32_t category,
                  uint32_t change_type,
                  const YumiGroupId *group,
                  const YumiAuditEntry *entry) {
    (void)category; (void)change_type; (void)group; (void)entry;
}

/**
 * @brief Behavioural threshold crossed during a GR mutation.
 *
 * Fired alongside @ref on_gr_change when the per-mutation behaviour
 * analyser detects a burst (actions/min) or abuse (destructive
 * ratio) condition for the acting peer.
 *
 * @param alerts       Bitmask of @c YUMI_ALERT_* flags.
 * @param change_type  The triggering change type (@c YUMI_CHANGE_*).
 * @param group        The affected group (host-owned).
 * @param actor        The acting peer (host-owned).
 * @param burst        Burst metrics over the alert window (host-owned).
 * @param admin        Abuse-score snapshot for the actor (host-owned).
 */
__attribute__((export_name("on_gr_behavior_alert")))
void on_gr_behavior_alert(uint32_t alerts,
                          uint32_t change_type,
                          const YumiGroupId *group,
                          const YumiPeerId *actor,
                          const YumiActorBurst *burst,
                          const YumiAdminScore *admin) {
    (void)alerts; (void)change_type; (void)group;
    (void)actor; (void)burst; (void)admin;
}
