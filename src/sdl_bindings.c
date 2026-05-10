/*
    SDL3 Host WASM Bindings Implementation
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

#include "sdl_bindings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Memory access helpers (same pattern as wgpu_bindings)              */
/* ------------------------------------------------------------------ */

static uint8_t *wasm_mem_base(SdlBindings *b) {
    if (!b->memory) return NULL;
    return (uint8_t *)wasm_memory_data(b->memory);
}

static size_t wasm_mem_size(SdlBindings *b) {
    if (!b->memory) return 0;
    return wasm_memory_data_size(b->memory);
}

static bool mem_read(SdlBindings *b, uint32_t ptr, void *dst, size_t len) {
    if ((size_t)ptr + len > wasm_mem_size(b)) return false;
    memcpy(dst, wasm_mem_base(b) + ptr, len);
    return true;
}

static bool mem_write(SdlBindings *b, uint32_t ptr, const void *src, size_t len) {
    if ((size_t)ptr + len > wasm_mem_size(b)) return false;
    memcpy(wasm_mem_base(b) + ptr, src, len);
    return true;
}


/* Write a NUL-terminated string into guest memory. Returns bytes written
   (including NUL) or 0 on failure. */
static uint32_t mem_write_str(SdlBindings *b, uint32_t ptr, uint32_t max_len,
                               const char *src) {
    if (!src) src = "";
    size_t slen = strlen(src);
    if (slen + 1 > max_len) slen = max_len - 1;
    if (!mem_write(b, ptr, src, slen)) return 0;
    uint8_t nul = 0;
    mem_write(b, ptr + (uint32_t)slen, &nul, 1);
    return (uint32_t)(slen + 1);
}

/* ------------------------------------------------------------------ */
/*  Shortcut macros                                                    */
/* ------------------------------------------------------------------ */

#define B        ((SdlBindings *)env)
#define ARG_I32(n)  (args->data[(n)].of.i32)
#define ARG_I64(n)  (args->data[(n)].of.i64)
#define ARG_F32(n)  (args->data[(n)].of.f32)
#define RET_I32(v)  do { res->data[0] = (wasm_val_t){.kind=WASM_I32, .of.i32=(v)}; } while(0)
#define RET_I64(v)  do { res->data[0] = (wasm_val_t){.kind=WASM_I64, .of.i64=(v)}; } while(0)
#define RET_F32(v)  do { res->data[0] = (wasm_val_t){.kind=WASM_F32, .of.f32=(v)}; } while(0)

/* ================================================================== */
/*  KEYBOARD                                                           */
/* ================================================================== */

/* sdl_has_keyboard() -> i32 (bool) */
static wasm_trap_t *fn_has_keyboard(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_HasKeyboard() ? 1 : 0);
    return NULL;
}

/* sdl_get_keyboard_state(out_ptr, max_keys) -> i32 num_keys
 * Copies the scancode bool array into guest memory at out_ptr.
 * Each element is 1 byte (0 or 1). */
static wasm_trap_t *fn_get_keyboard_state(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr  = (uint32_t)ARG_I32(0);
    uint32_t max_keys = (uint32_t)ARG_I32(1);

    int numkeys = 0;
    const bool *state = SDL_GetKeyboardState(&numkeys);
    if (!state || numkeys <= 0) { RET_I32(0); return NULL; }

    uint32_t to_copy = (uint32_t)numkeys;
    if (to_copy > max_keys) to_copy = max_keys;

    /* bool in SDL3 is C _Bool (1 byte). Write as uint8 array. */
    uint8_t *base = wasm_mem_base(B);
    if ((size_t)out_ptr + to_copy > wasm_mem_size(B)) { RET_I32(0); return NULL; }
    for (uint32_t i = 0; i < to_copy; i++)
        base[out_ptr + i] = state[i] ? 1 : 0;

    RET_I32((int32_t)to_copy);
    return NULL;
}

/* sdl_get_mod_state() -> i32 (SDL_Keymod) */
static wasm_trap_t *fn_get_mod_state(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32((int32_t)SDL_GetModState());
    return NULL;
}

/* sdl_set_mod_state(modstate) */
static wasm_trap_t *fn_set_mod_state(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    SDL_SetModState((SDL_Keymod)(uint16_t)ARG_I32(0));
    return NULL;
}

/* sdl_get_key_from_scancode(scancode, modstate, key_event) -> i32 keycode */
static wasm_trap_t *fn_get_key_from_scancode(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Scancode sc  = (SDL_Scancode)ARG_I32(0);
    SDL_Keymod mod   = (SDL_Keymod)(uint16_t)ARG_I32(1);
    bool key_event   = ARG_I32(2) != 0;
    RET_I32((int32_t)SDL_GetKeyFromScancode(sc, mod, key_event));
    return NULL;
}

/* sdl_get_scancode_from_key(keycode, mod_out_ptr) -> i32 scancode */
static wasm_trap_t *fn_get_scancode_from_key(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Keycode key    = (SDL_Keycode)(uint32_t)ARG_I32(0);
    uint32_t mod_ptr   = (uint32_t)ARG_I32(1);
    SDL_Keymod mod = 0;
    SDL_Scancode sc = SDL_GetScancodeFromKey(key, &mod);
    if (mod_ptr != 0) {
        uint16_t m16 = (uint16_t)mod;
        mem_write(B, mod_ptr, &m16, 2);
    }
    RET_I32((int32_t)sc);
    return NULL;
}

/* sdl_get_key_name(keycode, out_ptr, max_len) -> i32 bytes_written */
static wasm_trap_t *fn_get_key_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Keycode key  = (SDL_Keycode)(uint32_t)ARG_I32(0);
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    uint32_t max_len = (uint32_t)ARG_I32(2);
    const char *name = SDL_GetKeyName(key);
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* sdl_get_scancode_name(scancode, out_ptr, max_len) -> i32 bytes_written */
static wasm_trap_t *fn_get_scancode_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Scancode sc  = (SDL_Scancode)ARG_I32(0);
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    uint32_t max_len = (uint32_t)ARG_I32(2);
    const char *name = SDL_GetScancodeName(sc);
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* sdl_start_text_input() -> i32 (bool success) */
static wasm_trap_t *fn_start_text_input(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_StartTextInput(B->default_window) ? 1 : 0);
    return NULL;
}

/* sdl_stop_text_input() -> i32 (bool success) */
static wasm_trap_t *fn_stop_text_input(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_StopTextInput(B->default_window) ? 1 : 0);
    return NULL;
}

/* sdl_text_input_active() -> i32 (bool) */
static wasm_trap_t *fn_text_input_active(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_TextInputActive(B->default_window) ? 1 : 0);
    return NULL;
}

/* sdl_reset_keyboard() */
static wasm_trap_t *fn_reset_keyboard(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; (void)res;
    SDL_ResetKeyboard();
    return NULL;
}

/* ================================================================== */
/*  MOUSE                                                              */
/* ================================================================== */

/* sdl_has_mouse() -> i32 */
static wasm_trap_t *fn_has_mouse(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_HasMouse() ? 1 : 0);
    return NULL;
}

/* sdl_get_mouse_state(x_ptr, y_ptr) -> i32 button_flags
 * Writes float x,y into guest memory. Pass 0 to ignore. */
static wasm_trap_t *fn_get_mouse_state(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t x_ptr = (uint32_t)ARG_I32(0);
    uint32_t y_ptr = (uint32_t)ARG_I32(1);
    float x = 0, y = 0;
    SDL_MouseButtonFlags flags = SDL_GetMouseState(&x, &y);
    if (x_ptr) mem_write(B, x_ptr, &x, 4);
    if (y_ptr) mem_write(B, y_ptr, &y, 4);
    RET_I32((int32_t)flags);
    return NULL;
}

/* sdl_get_global_mouse_state(x_ptr, y_ptr) -> i32 button_flags */
static wasm_trap_t *fn_get_global_mouse_state(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t x_ptr = (uint32_t)ARG_I32(0);
    uint32_t y_ptr = (uint32_t)ARG_I32(1);
    float x = 0, y = 0;
    SDL_MouseButtonFlags flags = SDL_GetGlobalMouseState(&x, &y);
    if (x_ptr) mem_write(B, x_ptr, &x, 4);
    if (y_ptr) mem_write(B, y_ptr, &y, 4);
    RET_I32((int32_t)flags);
    return NULL;
}

/* sdl_get_relative_mouse_state(x_ptr, y_ptr) -> i32 button_flags */
static wasm_trap_t *fn_get_relative_mouse_state(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t x_ptr = (uint32_t)ARG_I32(0);
    uint32_t y_ptr = (uint32_t)ARG_I32(1);
    float x = 0, y = 0;
    SDL_MouseButtonFlags flags = SDL_GetRelativeMouseState(&x, &y);
    if (x_ptr) mem_write(B, x_ptr, &x, 4);
    if (y_ptr) mem_write(B, y_ptr, &y, 4);
    RET_I32((int32_t)flags);
    return NULL;
}

/* sdl_warp_mouse_in_window(x, y) */
static wasm_trap_t *fn_warp_mouse_in_window(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    float x = ARG_F32(0);
    float y = ARG_F32(1);
    SDL_WarpMouseInWindow(B->default_window, x, y);
    return NULL;
}

/* sdl_warp_mouse_global(x, y) -> i32 (bool) */
static wasm_trap_t *fn_warp_mouse_global(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    float x = ARG_F32(0);
    float y = ARG_F32(1);
    RET_I32(SDL_WarpMouseGlobal(x, y) ? 1 : 0);
    return NULL;
}

/* sdl_set_relative_mouse_mode(enabled) -> i32 (bool) */
static wasm_trap_t *fn_set_relative_mouse_mode(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    bool enabled = ARG_I32(0) != 0;
    RET_I32(SDL_SetWindowRelativeMouseMode(B->default_window, enabled) ? 1 : 0);
    return NULL;
}

/* sdl_get_relative_mouse_mode() -> i32 (bool) */
static wasm_trap_t *fn_get_relative_mouse_mode(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_GetWindowRelativeMouseMode(B->default_window) ? 1 : 0);
    return NULL;
}

/* sdl_show_cursor() -> i32 (bool) */
static wasm_trap_t *fn_show_cursor(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_ShowCursor() ? 1 : 0);
    return NULL;
}

/* sdl_hide_cursor() -> i32 (bool) */
static wasm_trap_t *fn_hide_cursor(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_HideCursor() ? 1 : 0);
    return NULL;
}

/* sdl_cursor_visible() -> i32 (bool) */
static wasm_trap_t *fn_cursor_visible(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_CursorVisible() ? 1 : 0);
    return NULL;
}

/* sdl_capture_mouse(enabled) -> i32 (bool) */
static wasm_trap_t *fn_capture_mouse(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    RET_I32(SDL_CaptureMouse(ARG_I32(0) != 0) ? 1 : 0);
    return NULL;
}

/* ================================================================== */
/*  JOYSTICK                                                           */
/* ================================================================== */

/* sdl_has_joystick() -> i32 */
static wasm_trap_t *fn_has_joystick(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_HasJoystick() ? 1 : 0);
    return NULL;
}

/* sdl_get_joysticks(out_ids_ptr, max_count) -> i32 actual_count
 * Writes SDL_JoystickID (uint32) array into guest memory. */
static wasm_trap_t *fn_get_joysticks(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr  = (uint32_t)ARG_I32(0);
    uint32_t max_cnt  = (uint32_t)ARG_I32(1);
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (!ids) { RET_I32(0); return NULL; }

    uint32_t to_copy = (uint32_t)count;
    if (to_copy > max_cnt) to_copy = max_cnt;
    for (uint32_t i = 0; i < to_copy; i++) {
        uint32_t id = (uint32_t)ids[i];
        mem_write(B, out_ptr + i * 4, &id, 4);
    }
    SDL_free(ids);
    RET_I32((int32_t)to_copy);
    return NULL;
}

/* sdl_get_joystick_name_for_id(instance_id, out_ptr, max_len) -> i32 bytes */
static wasm_trap_t *fn_get_joystick_name_for_id(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_JoystickID id = (SDL_JoystickID)(uint32_t)ARG_I32(0);
    uint32_t out_ptr  = (uint32_t)ARG_I32(1);
    uint32_t max_len  = (uint32_t)ARG_I32(2);
    const char *name  = SDL_GetJoystickNameForID(id);
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* sdl_open_joystick(instance_id) -> handle */
static wasm_trap_t *fn_open_joystick(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_JoystickID id = (SDL_JoystickID)(uint32_t)ARG_I32(0);
    SDL_Joystick *js = SDL_OpenJoystick(id);
    if (!js) { RET_I32(0); return NULL; }
    RET_I32(htable_insert(&B->ht_joystick, js));
    return NULL;
}

/* sdl_close_joystick(handle) */
static wasm_trap_t *fn_close_joystick(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    SDL_Joystick *js = htable_get(&B->ht_joystick, h);
    if (js) {
        SDL_CloseJoystick(js);
        htable_remove(&B->ht_joystick, h);
    }
    return NULL;
}

/* sdl_joystick_connected(handle) -> i32 */
static wasm_trap_t *fn_joystick_connected(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    RET_I32(js && SDL_JoystickConnected(js) ? 1 : 0);
    return NULL;
}

/* sdl_get_num_joystick_axes(handle) -> i32 */
static wasm_trap_t *fn_get_num_joystick_axes(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    RET_I32(js ? SDL_GetNumJoystickAxes(js) : 0);
    return NULL;
}

/* sdl_get_num_joystick_buttons(handle) -> i32 */
static wasm_trap_t *fn_get_num_joystick_buttons(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    RET_I32(js ? SDL_GetNumJoystickButtons(js) : 0);
    return NULL;
}

/* sdl_get_num_joystick_hats(handle) -> i32 */
static wasm_trap_t *fn_get_num_joystick_hats(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    RET_I32(js ? SDL_GetNumJoystickHats(js) : 0);
    return NULL;
}

/* sdl_get_joystick_axis(handle, axis) -> i32 (Sint16) */
static wasm_trap_t *fn_get_joystick_axis(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    int axis = ARG_I32(1);
    RET_I32(js ? (int32_t)SDL_GetJoystickAxis(js, axis) : 0);
    return NULL;
}

/* sdl_get_joystick_button(handle, button) -> i32 (bool) */
static wasm_trap_t *fn_get_joystick_button(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    int button = ARG_I32(1);
    RET_I32(js ? (SDL_GetJoystickButton(js, button) ? 1 : 0) : 0);
    return NULL;
}

/* sdl_get_joystick_hat(handle, hat) -> i32 (Uint8 hat value) */
static wasm_trap_t *fn_get_joystick_hat(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    int hat = ARG_I32(1);
    RET_I32(js ? (int32_t)SDL_GetJoystickHat(js, hat) : 0);
    return NULL;
}

/* sdl_rumble_joystick(handle, low_freq, high_freq, duration_ms) -> i32 */
static wasm_trap_t *fn_rumble_joystick(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    if (!js) { RET_I32(0); return NULL; }
    Uint16 lo  = (Uint16)(uint32_t)ARG_I32(1);
    Uint16 hi  = (Uint16)(uint32_t)ARG_I32(2);
    Uint32 dur = (Uint32)ARG_I32(3);
    RET_I32(SDL_RumbleJoystick(js, lo, hi, dur) ? 1 : 0);
    return NULL;
}

/* sdl_set_joystick_led(handle, r, g, b) -> i32 */
static wasm_trap_t *fn_set_joystick_led(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    if (!js) { RET_I32(0); return NULL; }
    RET_I32(SDL_SetJoystickLED(js,
        (Uint8)ARG_I32(1), (Uint8)ARG_I32(2), (Uint8)ARG_I32(3)) ? 1 : 0);
    return NULL;
}

/* sdl_get_joystick_name(handle, out_ptr, max_len) -> i32 bytes */
static wasm_trap_t *fn_get_joystick_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    uint32_t max_len = (uint32_t)ARG_I32(2);
    const char *name = js ? SDL_GetJoystickName(js) : NULL;
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* sdl_get_joystick_id(handle) -> i32 (instance id) */
static wasm_trap_t *fn_get_joystick_id(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->ht_joystick, (uint32_t)ARG_I32(0));
    RET_I32(js ? (int32_t)SDL_GetJoystickID(js) : 0);
    return NULL;
}

/* sdl_update_joysticks() */
static wasm_trap_t *fn_update_joysticks(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; (void)res;
    SDL_UpdateJoysticks();
    return NULL;
}

/* ================================================================== */
/*  GAMEPAD                                                            */
/* ================================================================== */

/* sdl_has_gamepad() -> i32 */
static wasm_trap_t *fn_has_gamepad(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_HasGamepad() ? 1 : 0);
    return NULL;
}

/* sdl_get_gamepads(out_ids_ptr, max_count) -> i32 actual_count */
static wasm_trap_t *fn_get_gamepads(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    uint32_t max_cnt = (uint32_t)ARG_I32(1);
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids) { RET_I32(0); return NULL; }

    uint32_t to_copy = (uint32_t)count;
    if (to_copy > max_cnt) to_copy = max_cnt;
    for (uint32_t i = 0; i < to_copy; i++) {
        uint32_t id = (uint32_t)ids[i];
        mem_write(B, out_ptr + i * 4, &id, 4);
    }
    SDL_free(ids);
    RET_I32((int32_t)to_copy);
    return NULL;
}

/* sdl_is_gamepad(instance_id) -> i32 */
static wasm_trap_t *fn_is_gamepad(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    RET_I32(SDL_IsGamepad((SDL_JoystickID)(uint32_t)ARG_I32(0)) ? 1 : 0);
    return NULL;
}

/* sdl_open_gamepad(instance_id) -> handle */
static wasm_trap_t *fn_open_gamepad(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_JoystickID id = (SDL_JoystickID)(uint32_t)ARG_I32(0);
    SDL_Gamepad *gp = SDL_OpenGamepad(id);
    if (!gp) { RET_I32(0); return NULL; }
    RET_I32(htable_insert(&B->ht_gamepad, gp));
    return NULL;
}

/* sdl_close_gamepad(handle) */
static wasm_trap_t *fn_close_gamepad(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, h);
    if (gp) {
        SDL_CloseGamepad(gp);
        htable_remove(&B->ht_gamepad, h);
    }
    return NULL;
}

/* sdl_gamepad_connected(handle) -> i32 */
static wasm_trap_t *fn_gamepad_connected(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    RET_I32(gp && SDL_GamepadConnected(gp) ? 1 : 0);
    return NULL;
}

/* sdl_get_gamepad_name(handle, out_ptr, max_len) -> i32 bytes */
static wasm_trap_t *fn_get_gamepad_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp  = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    uint32_t max_len = (uint32_t)ARG_I32(2);
    const char *name = gp ? SDL_GetGamepadName(gp) : NULL;
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* sdl_get_gamepad_type(handle) -> i32 (SDL_GamepadType) */
static wasm_trap_t *fn_get_gamepad_type(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    RET_I32(gp ? (int32_t)SDL_GetGamepadType(gp) : 0);
    return NULL;
}

/* sdl_get_gamepad_axis(handle, axis) -> i32 (Sint16) */
static wasm_trap_t *fn_get_gamepad_axis(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    SDL_GamepadAxis axis = (SDL_GamepadAxis)ARG_I32(1);
    RET_I32(gp ? (int32_t)SDL_GetGamepadAxis(gp, axis) : 0);
    return NULL;
}

/* sdl_get_gamepad_button(handle, button) -> i32 (bool) */
static wasm_trap_t *fn_get_gamepad_button(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    SDL_GamepadButton btn = (SDL_GamepadButton)ARG_I32(1);
    RET_I32(gp ? (SDL_GetGamepadButton(gp, btn) ? 1 : 0) : 0);
    return NULL;
}

/* sdl_rumble_gamepad(handle, low, high, duration_ms) -> i32 */
static wasm_trap_t *fn_rumble_gamepad(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    if (!gp) { RET_I32(0); return NULL; }
    RET_I32(SDL_RumbleGamepad(gp,
        (Uint16)(uint32_t)ARG_I32(1),
        (Uint16)(uint32_t)ARG_I32(2),
        (Uint32)ARG_I32(3)) ? 1 : 0);
    return NULL;
}

/* sdl_rumble_gamepad_triggers(handle, left, right, duration_ms) -> i32 */
static wasm_trap_t *fn_rumble_gamepad_triggers(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    if (!gp) { RET_I32(0); return NULL; }
    RET_I32(SDL_RumbleGamepadTriggers(gp,
        (Uint16)(uint32_t)ARG_I32(1),
        (Uint16)(uint32_t)ARG_I32(2),
        (Uint32)ARG_I32(3)) ? 1 : 0);
    return NULL;
}

/* sdl_set_gamepad_led(handle, r, g, b) -> i32 */
static wasm_trap_t *fn_set_gamepad_led(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    if (!gp) { RET_I32(0); return NULL; }
    RET_I32(SDL_SetGamepadLED(gp,
        (Uint8)ARG_I32(1), (Uint8)ARG_I32(2), (Uint8)ARG_I32(3)) ? 1 : 0);
    return NULL;
}

/* sdl_get_gamepad_id(handle) -> i32 instance_id */
static wasm_trap_t *fn_get_gamepad_id(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    RET_I32(gp ? (int32_t)SDL_GetGamepadID(gp) : 0);
    return NULL;
}

/* sdl_gamepad_has_axis(handle, axis) -> i32 */
static wasm_trap_t *fn_gamepad_has_axis(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    RET_I32(gp ? (SDL_GamepadHasAxis(gp, (SDL_GamepadAxis)ARG_I32(1)) ? 1 : 0) : 0);
    return NULL;
}

/* sdl_gamepad_has_button(handle, button) -> i32 */
static wasm_trap_t *fn_gamepad_has_button(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    RET_I32(gp ? (SDL_GamepadHasButton(gp, (SDL_GamepadButton)ARG_I32(1)) ? 1 : 0) : 0);
    return NULL;
}

/* sdl_update_gamepads() */
static wasm_trap_t *fn_update_gamepads(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; (void)res;
    SDL_UpdateGamepads();
    return NULL;
}

/* sdl_get_gamepad_button_label(handle, button) -> i32 */
static wasm_trap_t *fn_get_gamepad_button_label(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    if (!gp) { RET_I32(0); return NULL; }
    RET_I32((int32_t)SDL_GetGamepadButtonLabel(gp, (SDL_GamepadButton)ARG_I32(1)));
    return NULL;
}

/* sdl_get_gamepad_player_index(handle) -> i32 */
static wasm_trap_t *fn_get_gamepad_player_index(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    RET_I32(gp ? SDL_GetGamepadPlayerIndex(gp) : -1);
    return NULL;
}

/* sdl_set_gamepad_player_index(handle, player_index) -> i32 */
static wasm_trap_t *fn_set_gamepad_player_index(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->ht_gamepad, (uint32_t)ARG_I32(0));
    if (!gp) { RET_I32(0); return NULL; }
    RET_I32(SDL_SetGamepadPlayerIndex(gp, ARG_I32(1)) ? 1 : 0);
    return NULL;
}

/* ================================================================== */
/*  AUDIO                                                              */
/* ================================================================== */

/* sdl_get_num_audio_drivers() -> i32 */
static wasm_trap_t *fn_get_num_audio_drivers(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_GetNumAudioDrivers());
    return NULL;
}

/* sdl_get_audio_driver(index, out_ptr, max_len) -> i32 bytes */
static wasm_trap_t *fn_get_audio_driver(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    int index = ARG_I32(0);
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    uint32_t max_len = (uint32_t)ARG_I32(2);
    const char *name = SDL_GetAudioDriver(index);
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* sdl_get_current_audio_driver(out_ptr, max_len) -> i32 bytes */
static wasm_trap_t *fn_get_current_audio_driver(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    uint32_t max_len = (uint32_t)ARG_I32(1);
    const char *name = SDL_GetCurrentAudioDriver();
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* sdl_get_audio_playback_devices(out_ids_ptr, max_count) -> i32 count */
static wasm_trap_t *fn_get_audio_playback_devices(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    uint32_t max_cnt = (uint32_t)ARG_I32(1);
    int count = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&count);
    if (!ids) { RET_I32(0); return NULL; }
    uint32_t to_copy = (uint32_t)count;
    if (to_copy > max_cnt) to_copy = max_cnt;
    for (uint32_t i = 0; i < to_copy; i++) {
        uint32_t id = (uint32_t)ids[i];
        mem_write(B, out_ptr + i * 4, &id, 4);
    }
    SDL_free(ids);
    RET_I32((int32_t)to_copy);
    return NULL;
}

/* sdl_get_audio_recording_devices(out_ids_ptr, max_count) -> i32 count */
static wasm_trap_t *fn_get_audio_recording_devices(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    uint32_t max_cnt = (uint32_t)ARG_I32(1);
    int count = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&count);
    if (!ids) { RET_I32(0); return NULL; }
    uint32_t to_copy = (uint32_t)count;
    if (to_copy > max_cnt) to_copy = max_cnt;
    for (uint32_t i = 0; i < to_copy; i++) {
        uint32_t id = (uint32_t)ids[i];
        mem_write(B, out_ptr + i * 4, &id, 4);
    }
    SDL_free(ids);
    RET_I32((int32_t)to_copy);
    return NULL;
}

/* sdl_get_audio_device_name(devid, out_ptr, max_len) -> i32 bytes */
static wasm_trap_t *fn_get_audio_device_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioDeviceID devid = (SDL_AudioDeviceID)(uint32_t)ARG_I32(0);
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    uint32_t max_len = (uint32_t)ARG_I32(2);
    const char *name = SDL_GetAudioDeviceName(devid);
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* sdl_open_audio_device(devid, format, channels, freq) -> i32 devid
 * Pass devid=0xFFFFFFFF for default playback, 0xFFFFFFFE for default recording.
 * format/channels/freq can be 0 to let SDL choose defaults. */
static wasm_trap_t *fn_open_audio_device(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioDeviceID devid = (SDL_AudioDeviceID)(uint32_t)ARG_I32(0);
    int format   = ARG_I32(1);
    int channels = ARG_I32(2);
    int freq     = ARG_I32(3);

    const SDL_AudioSpec *spec_ptr = NULL;
    SDL_AudioSpec spec = {0};
    if (format || channels || freq) {
        spec.format   = (SDL_AudioFormat)format;
        spec.channels = channels;
        spec.freq     = freq;
        spec_ptr = &spec;
    }

    SDL_AudioDeviceID opened = SDL_OpenAudioDevice(devid, spec_ptr);
    RET_I32((int32_t)opened);
    return NULL;
}

/* sdl_close_audio_device(devid) */
static wasm_trap_t *fn_close_audio_device(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    SDL_CloseAudioDevice((SDL_AudioDeviceID)(uint32_t)ARG_I32(0));
    return NULL;
}

/* sdl_pause_audio_device(devid) -> i32 */
static wasm_trap_t *fn_pause_audio_device(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    RET_I32(SDL_PauseAudioDevice((SDL_AudioDeviceID)(uint32_t)ARG_I32(0)) ? 1 : 0);
    return NULL;
}

/* sdl_resume_audio_device(devid) -> i32 */
static wasm_trap_t *fn_resume_audio_device(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    RET_I32(SDL_ResumeAudioDevice((SDL_AudioDeviceID)(uint32_t)ARG_I32(0)) ? 1 : 0);
    return NULL;
}

/* sdl_audio_device_paused(devid) -> i32 */
static wasm_trap_t *fn_audio_device_paused(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    RET_I32(SDL_AudioDevicePaused((SDL_AudioDeviceID)(uint32_t)ARG_I32(0)) ? 1 : 0);
    return NULL;
}

/* sdl_set_audio_device_gain(devid, gain) -> i32 */
static wasm_trap_t *fn_set_audio_device_gain(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioDeviceID devid = (SDL_AudioDeviceID)(uint32_t)ARG_I32(0);
    float gain = ARG_F32(1);
    RET_I32(SDL_SetAudioDeviceGain(devid, gain) ? 1 : 0);
    return NULL;
}

/* sdl_get_audio_device_gain(devid) -> f32 */
static wasm_trap_t *fn_get_audio_device_gain(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    RET_F32(SDL_GetAudioDeviceGain((SDL_AudioDeviceID)(uint32_t)ARG_I32(0)));
    return NULL;
}

/* sdl_get_audio_device_format(devid, spec_out_ptr, frames_out_ptr) -> i32
 * Writes SDL_AudioSpec (format u16 + channels i32 + freq i32 = 10 bytes
 * but we use a 12 byte layout: u32 format, i32 channels, i32 freq)
 * into guest memory at spec_out_ptr.
 * Writes sample_frames (i32) at frames_out_ptr if non-zero. */
static wasm_trap_t *fn_get_audio_device_format(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioDeviceID devid = (SDL_AudioDeviceID)(uint32_t)ARG_I32(0);
    uint32_t spec_ptr   = (uint32_t)ARG_I32(1);
    uint32_t frames_ptr = (uint32_t)ARG_I32(2);

    SDL_AudioSpec spec = {0};
    int frames = 0;
    bool ok = SDL_GetAudioDeviceFormat(devid, &spec, &frames);
    if (ok && spec_ptr) {
        uint32_t fmt = (uint32_t)spec.format;
        int32_t ch   = spec.channels;
        int32_t fr   = spec.freq;
        mem_write(B, spec_ptr,     &fmt, 4);
        mem_write(B, spec_ptr + 4, &ch,  4);
        mem_write(B, spec_ptr + 8, &fr,  4);
    }
    if (ok && frames_ptr) {
        int32_t f = frames;
        mem_write(B, frames_ptr, &f, 4);
    }
    RET_I32(ok ? 1 : 0);
    return NULL;
}

/* ---- Audio Streams ---- */

/* sdl_create_audio_stream(src_fmt, src_ch, src_freq,
 *                         dst_fmt, dst_ch, dst_freq) -> handle */
static wasm_trap_t *fn_create_audio_stream(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioSpec src = {
        .format   = (SDL_AudioFormat)(uint32_t)ARG_I32(0),
        .channels = ARG_I32(1),
        .freq     = ARG_I32(2),
    };
    SDL_AudioSpec dst = {
        .format   = (SDL_AudioFormat)(uint32_t)ARG_I32(3),
        .channels = ARG_I32(4),
        .freq     = ARG_I32(5),
    };
    SDL_AudioStream *stream = SDL_CreateAudioStream(&src, &dst);
    if (!stream) { RET_I32(0); return NULL; }
    RET_I32(htable_insert(&B->ht_audio_stream, stream));
    return NULL;
}

/* sdl_destroy_audio_stream(handle) */
static wasm_trap_t *fn_destroy_audio_stream(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, h);
    if (s) {
        SDL_DestroyAudioStream(s);
        htable_remove(&B->ht_audio_stream, h);
    }
    return NULL;
}

/* sdl_bind_audio_stream(devid, stream_handle) -> i32 */
static wasm_trap_t *fn_bind_audio_stream(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioDeviceID devid = (SDL_AudioDeviceID)(uint32_t)ARG_I32(0);
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(1));
    if (!s) { RET_I32(0); return NULL; }
    RET_I32(SDL_BindAudioStream(devid, s) ? 1 : 0);
    return NULL;
}

/* sdl_unbind_audio_stream(stream_handle) */
static wasm_trap_t *fn_unbind_audio_stream(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    if (s) SDL_UnbindAudioStream(s);
    return NULL;
}

/* sdl_put_audio_stream_data(handle, data_ptr, len) -> i32 */
static wasm_trap_t *fn_put_audio_stream_data(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    uint32_t data_ptr = (uint32_t)ARG_I32(1);
    uint32_t len      = (uint32_t)ARG_I32(2);
    if (!s) { RET_I32(0); return NULL; }

    uint8_t *base = wasm_mem_base(B);
    if ((size_t)data_ptr + len > wasm_mem_size(B)) { RET_I32(0); return NULL; }
    RET_I32(SDL_PutAudioStreamData(s, base + data_ptr, (int)len) ? 1 : 0);
    return NULL;
}

/* sdl_get_audio_stream_data(handle, out_ptr, len) -> i32 bytes_read */
static wasm_trap_t *fn_get_audio_stream_data(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    uint32_t len     = (uint32_t)ARG_I32(2);
    if (!s) { RET_I32(-1); return NULL; }

    uint8_t *base = wasm_mem_base(B);
    if ((size_t)out_ptr + len > wasm_mem_size(B)) { RET_I32(-1); return NULL; }
    RET_I32(SDL_GetAudioStreamData(s, base + out_ptr, (int)len));
    return NULL;
}

/* sdl_get_audio_stream_available(handle) -> i32 bytes */
static wasm_trap_t *fn_get_audio_stream_available(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    RET_I32(s ? SDL_GetAudioStreamAvailable(s) : -1);
    return NULL;
}

/* sdl_get_audio_stream_queued(handle) -> i32 bytes */
static wasm_trap_t *fn_get_audio_stream_queued(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    RET_I32(s ? SDL_GetAudioStreamQueued(s) : -1);
    return NULL;
}

/* sdl_flush_audio_stream(handle) -> i32 */
static wasm_trap_t *fn_flush_audio_stream(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    RET_I32(s ? (SDL_FlushAudioStream(s) ? 1 : 0) : 0);
    return NULL;
}

/* sdl_clear_audio_stream(handle) -> i32 */
static wasm_trap_t *fn_clear_audio_stream(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    RET_I32(s ? (SDL_ClearAudioStream(s) ? 1 : 0) : 0);
    return NULL;
}

/* sdl_set_audio_stream_format(handle,
 *     src_fmt, src_ch, src_freq, dst_fmt, dst_ch, dst_freq) -> i32 */
static wasm_trap_t *fn_set_audio_stream_format(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    if (!s) { RET_I32(0); return NULL; }

    SDL_AudioSpec src = {
        .format   = (SDL_AudioFormat)(uint32_t)ARG_I32(1),
        .channels = ARG_I32(2),
        .freq     = ARG_I32(3),
    };
    SDL_AudioSpec dst = {
        .format   = (SDL_AudioFormat)(uint32_t)ARG_I32(4),
        .channels = ARG_I32(5),
        .freq     = ARG_I32(6),
    };
    /* Pass NULL for either side if all fields are 0 (no change). */
    const SDL_AudioSpec *sp = (src.format || src.channels || src.freq) ? &src : NULL;
    const SDL_AudioSpec *dp = (dst.format || dst.channels || dst.freq) ? &dst : NULL;
    RET_I32(SDL_SetAudioStreamFormat(s, sp, dp) ? 1 : 0);
    return NULL;
}

/* sdl_set_audio_stream_gain(handle, gain) -> i32 */
static wasm_trap_t *fn_set_audio_stream_gain(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    if (!s) { RET_I32(0); return NULL; }
    RET_I32(SDL_SetAudioStreamGain(s, ARG_F32(1)) ? 1 : 0);
    return NULL;
}

/* sdl_get_audio_stream_gain(handle) -> f32 */
static wasm_trap_t *fn_get_audio_stream_gain(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    RET_F32(s ? SDL_GetAudioStreamGain(s) : -1.0f);
    return NULL;
}

/* sdl_set_audio_stream_frequency_ratio(handle, ratio) -> i32 */
static wasm_trap_t *fn_set_audio_stream_freq_ratio(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    if (!s) { RET_I32(0); return NULL; }
    RET_I32(SDL_SetAudioStreamFrequencyRatio(s, ARG_F32(1)) ? 1 : 0);
    return NULL;
}

/* sdl_get_audio_stream_frequency_ratio(handle) -> f32 */
static wasm_trap_t *fn_get_audio_stream_freq_ratio(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    RET_F32(s ? SDL_GetAudioStreamFrequencyRatio(s) : 0.0f);
    return NULL;
}

/* ---- Simplified audio device stream ---- */

/* sdl_open_audio_device_stream(devid, format, channels, freq) -> handle
 * Opens device+stream in one call (device starts paused). */
static wasm_trap_t *fn_open_audio_device_stream(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioDeviceID devid = (SDL_AudioDeviceID)(uint32_t)ARG_I32(0);
    int format   = ARG_I32(1);
    int channels = ARG_I32(2);
    int freq     = ARG_I32(3);

    const SDL_AudioSpec *spec_ptr = NULL;
    SDL_AudioSpec spec = {0};
    if (format || channels || freq) {
        spec.format   = (SDL_AudioFormat)format;
        spec.channels = channels;
        spec.freq     = freq;
        spec_ptr = &spec;
    }

    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(devid, spec_ptr,
                                                         NULL, NULL);
    if (!stream) { RET_I32(0); return NULL; }
    RET_I32(htable_insert(&B->ht_audio_stream, stream));
    return NULL;
}

/* sdl_resume_audio_stream_device(handle) -> i32 */
static wasm_trap_t *fn_resume_audio_stream_device(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    RET_I32(s ? (SDL_ResumeAudioStreamDevice(s) ? 1 : 0) : 0);
    return NULL;
}

/* sdl_pause_audio_stream_device(handle) -> i32 */
static wasm_trap_t *fn_pause_audio_stream_device(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioStream *s = htable_get(&B->ht_audio_stream, (uint32_t)ARG_I32(0));
    RET_I32(s ? (SDL_PauseAudioStreamDevice(s) ? 1 : 0) : 0);
    return NULL;
}

/* ---- WAV loading ---- */

/* sdl_load_wav(path_ptr, path_len, spec_out_ptr, data_out_ptr, len_out_ptr) -> i32
 *
 * Loads a WAV file. Writes:
 *   spec_out_ptr: {u32 format, i32 channels, i32 freq} (12 bytes)
 *   data_out_ptr: audio data copied into guest memory at this location
 *   len_out_ptr:  u32 length in bytes of the audio data
 *
 * The caller must ensure data_out_ptr has enough space.
 * Returns 1 on success, 0 on failure. */
static wasm_trap_t *fn_load_wav(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t path_ptr = (uint32_t)ARG_I32(0);
    uint32_t path_len = (uint32_t)ARG_I32(1);
    uint32_t spec_out = (uint32_t)ARG_I32(2);
    uint32_t data_out = (uint32_t)ARG_I32(3);
    uint32_t data_max = (uint32_t)ARG_I32(4);
    uint32_t len_out  = (uint32_t)ARG_I32(5);

    /* Build NUL-terminated path */
    char pathbuf[1024];
    if (path_len >= sizeof(pathbuf)) { RET_I32(0); return NULL; }
    memcpy(pathbuf, wasm_mem_base(B) + path_ptr, path_len);
    pathbuf[path_len] = '\0';

    SDL_AudioSpec spec = {0};
    Uint8 *audio_buf = NULL;
    Uint32 audio_len = 0;

    if (!SDL_LoadWAV(pathbuf, &spec, &audio_buf, &audio_len)) {
        RET_I32(0);
        return NULL;
    }

    /* Write spec */
    if (spec_out) {
        uint32_t fmt = (uint32_t)spec.format;
        int32_t ch   = spec.channels;
        int32_t fr   = spec.freq;
        mem_write(B, spec_out,     &fmt, 4);
        mem_write(B, spec_out + 4, &ch,  4);
        mem_write(B, spec_out + 8, &fr,  4);
    }

    /* Write audio data */
    uint32_t copy_len = audio_len;
    if (copy_len > data_max) copy_len = data_max;
    if (data_out && copy_len > 0) {
        mem_write(B, data_out, audio_buf, copy_len);
    }

    if (len_out) {
        mem_write(B, len_out, &audio_len, 4);
    }

    SDL_free(audio_buf);
    RET_I32(1);
    return NULL;
}

/* sdl_mix_audio(dst_ptr, src_ptr, format, len, volume) -> i32 */
static wasm_trap_t *fn_mix_audio(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t dst_ptr = (uint32_t)ARG_I32(0);
    uint32_t src_ptr = (uint32_t)ARG_I32(1);
    int32_t  format  = ARG_I32(2);
    uint32_t len     = (uint32_t)ARG_I32(3);
    float    volume  = ARG_F32(4);

    uint8_t *base = wasm_mem_base(B);
    size_t msz = wasm_mem_size(B);
    if ((size_t)dst_ptr + len > msz || (size_t)src_ptr + len > msz) {
        RET_I32(0); return NULL;
    }
    RET_I32(SDL_MixAudio(base + dst_ptr, base + src_ptr,
                          (SDL_AudioFormat)format, (Uint32)len, volume) ? 1 : 0);
    return NULL;
}

/* sdl_get_audio_format_name(format, out_ptr, max_len) -> i32 bytes */
static wasm_trap_t *fn_get_audio_format_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_AudioFormat fmt = (SDL_AudioFormat)(uint32_t)ARG_I32(0);
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    uint32_t max_len = (uint32_t)ARG_I32(2);
    const char *name = SDL_GetAudioFormatName(fmt);
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* ================================================================== */
/*  DISPLAY                                                            */
/* ================================================================== */

/* sdl_get_displays(out_ids_ptr, max_count) -> i32 actual_count */
static wasm_trap_t *fn_get_displays(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    uint32_t max_cnt = (uint32_t)ARG_I32(1);
    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    if (!ids) { RET_I32(0); return NULL; }

    uint32_t to_copy = (uint32_t)count;
    if (to_copy > max_cnt) to_copy = max_cnt;
    for (uint32_t i = 0; i < to_copy; i++) {
        uint32_t id = (uint32_t)ids[i];
        mem_write(B, out_ptr + i * 4, &id, 4);
    }
    SDL_free(ids);
    RET_I32((int32_t)to_copy);
    return NULL;
}

/* sdl_get_display_for_window() -> i32 display_id */
static wasm_trap_t *fn_get_display_for_window(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32((int32_t)SDL_GetDisplayForWindow(B->default_window));
    return NULL;
}

/* sdl_get_display_name(display_id, out_ptr, max_len) -> i32 bytes */
static wasm_trap_t *fn_get_display_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_DisplayID id = (SDL_DisplayID)(uint32_t)ARG_I32(0);
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    uint32_t max_len = (uint32_t)ARG_I32(2);
    const char *name = SDL_GetDisplayName(id);
    RET_I32((int32_t)mem_write_str(B, out_ptr, max_len, name));
    return NULL;
}

/* sdl_get_display_bounds(display_id, out_ptr) -> i32 success
 * Writes 4 x int32 (x, y, w, h) = 16 bytes at out_ptr. */
static wasm_trap_t *fn_get_display_bounds(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_DisplayID id = (SDL_DisplayID)(uint32_t)ARG_I32(0);
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    SDL_Rect rect = {0};
    if (!SDL_GetDisplayBounds(id, &rect)) { RET_I32(0); return NULL; }
    int32_t vals[4] = { rect.x, rect.y, rect.w, rect.h };
    mem_write(B, out_ptr, vals, 16);
    RET_I32(1);
    return NULL;
}

/* sdl_get_display_usable_bounds(display_id, out_ptr) -> i32 success
 * Writes 4 x int32 (x, y, w, h) = 16 bytes at out_ptr. */
static wasm_trap_t *fn_get_display_usable_bounds(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_DisplayID id = (SDL_DisplayID)(uint32_t)ARG_I32(0);
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    SDL_Rect rect = {0};
    if (!SDL_GetDisplayUsableBounds(id, &rect)) { RET_I32(0); return NULL; }
    int32_t vals[4] = { rect.x, rect.y, rect.w, rect.h };
    mem_write(B, out_ptr, vals, 16);
    RET_I32(1);
    return NULL;
}

/* sdl_get_display_content_scale(display_id) -> f32 */
static wasm_trap_t *fn_get_display_content_scale(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_DisplayID id = (SDL_DisplayID)(uint32_t)ARG_I32(0);
    RET_F32(SDL_GetDisplayContentScale(id));
    return NULL;
}

/* sdl_get_display_orientation(display_id) -> i32 */
static wasm_trap_t *fn_get_display_orientation(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_DisplayID id = (SDL_DisplayID)(uint32_t)ARG_I32(0);
    RET_I32((int32_t)SDL_GetCurrentDisplayOrientation(id));
    return NULL;
}

/* sdl_get_display_natural_orientation(display_id) -> i32 */
static wasm_trap_t *fn_get_display_natural_orientation(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_DisplayID id = (SDL_DisplayID)(uint32_t)ARG_I32(0);
    RET_I32((int32_t)SDL_GetNaturalDisplayOrientation(id));
    return NULL;
}

/* sdl_get_display_mode(display_id, out_w, out_h, out_refresh) -> i32 success
 * Writes int32 width/height and float refresh rate. Any ptr may be 0. */
static wasm_trap_t *fn_get_display_mode(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_DisplayID id   = (SDL_DisplayID)(uint32_t)ARG_I32(0);
    uint32_t w_ptr     = (uint32_t)ARG_I32(1);
    uint32_t h_ptr     = (uint32_t)ARG_I32(2);
    uint32_t refresh_ptr = (uint32_t)ARG_I32(3);

    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(id);
    if (!mode) { RET_I32(0); return NULL; }

    if (w_ptr) { int32_t w = mode->w; mem_write(B, w_ptr, &w, 4); }
    if (h_ptr) { int32_t h = mode->h; mem_write(B, h_ptr, &h, 4); }
    if (refresh_ptr) { float r = mode->refresh_rate; mem_write(B, refresh_ptr, &r, 4); }
    RET_I32(1);
    return NULL;
}

/* sdl_get_window_pixel_density() -> f32 */
static wasm_trap_t *fn_get_window_pixel_density(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_F32(SDL_GetWindowPixelDensity(B->default_window));
    return NULL;
}

/* sdl_get_window_display_scale() -> f32 */
static wasm_trap_t *fn_get_window_display_scale(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_F32(SDL_GetWindowDisplayScale(B->default_window));
    return NULL;
}

/* ================================================================== */
/*  PLATFORM                                                           */
/* ================================================================== */

/* sdl_get_platform() -> i32 */
static wasm_trap_t *fn_get_platform(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)env; (void)args;
    const char *p = SDL_GetPlatform();
    int val = 0; /* unknown */
    if      (strcmp(p, "Windows") == 0) val = 1;
    else if (strcmp(p, "macOS")   == 0) val = 2;
    else if (strcmp(p, "Linux")   == 0) val = 3;
    else if (strcmp(p, "iOS")     == 0) val = 4;
    else if (strcmp(p, "Android") == 0) val = 5;
    RET_I32(val);
    return NULL;
}

/* sdl_get_ticks_ms() -> f32   (monotonic milliseconds since SDL init) */
static wasm_trap_t *fn_get_ticks_ms(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)env; (void)args;
    RET_F32((float)SDL_GetTicks());
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Function registry                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char                    *name;
    wasm_func_callback_with_env_t  cb;
    uint32_t np;  wasm_valkind_t   params[10];
    uint32_t nr;  wasm_valkind_t   results[1];
} SdlBindingEntry;

static const SdlBindingEntry SDL_BINDINGS[] = {
    /* ---- Keyboard ---- */
    {"sdl_has_keyboard",           fn_has_keyboard,           0, {0},                                         1, {WASM_I32}},
    {"sdl_get_keyboard_state",     fn_get_keyboard_state,     2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_mod_state",          fn_get_mod_state,          0, {0},                                         1, {WASM_I32}},
    {"sdl_set_mod_state",          fn_set_mod_state,          1, {WASM_I32},                                  0, {0}},
    {"sdl_get_key_from_scancode",  fn_get_key_from_scancode,  3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_get_scancode_from_key",  fn_get_scancode_from_key,  2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_key_name",           fn_get_key_name,           3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_get_scancode_name",      fn_get_scancode_name,      3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_start_text_input",       fn_start_text_input,       0, {0},                                         1, {WASM_I32}},
    {"sdl_stop_text_input",        fn_stop_text_input,        0, {0},                                         1, {WASM_I32}},
    {"sdl_text_input_active",      fn_text_input_active,      0, {0},                                         1, {WASM_I32}},
    {"sdl_reset_keyboard",         fn_reset_keyboard,         0, {0},                                         0, {0}},

    /* ---- Mouse ---- */
    {"sdl_has_mouse",              fn_has_mouse,              0, {0},                                         1, {WASM_I32}},
    {"sdl_get_mouse_state",        fn_get_mouse_state,        2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_global_mouse_state", fn_get_global_mouse_state, 2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_relative_mouse_state", fn_get_relative_mouse_state, 2, {WASM_I32,WASM_I32},                    1, {WASM_I32}},
    {"sdl_warp_mouse_in_window",   fn_warp_mouse_in_window,   2, {WASM_F32,WASM_F32},                        0, {0}},
    {"sdl_warp_mouse_global",      fn_warp_mouse_global,      2, {WASM_F32,WASM_F32},                        1, {WASM_I32}},
    {"sdl_set_relative_mouse_mode",fn_set_relative_mouse_mode,1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_get_relative_mouse_mode",fn_get_relative_mouse_mode,0, {0},                                         1, {WASM_I32}},
    {"sdl_show_cursor",            fn_show_cursor,            0, {0},                                         1, {WASM_I32}},
    {"sdl_hide_cursor",            fn_hide_cursor,            0, {0},                                         1, {WASM_I32}},
    {"sdl_cursor_visible",         fn_cursor_visible,         0, {0},                                         1, {WASM_I32}},
    {"sdl_capture_mouse",          fn_capture_mouse,          1, {WASM_I32},                                  1, {WASM_I32}},

    /* ---- Joystick ---- */
    {"sdl_has_joystick",           fn_has_joystick,           0, {0},                                         1, {WASM_I32}},
    {"sdl_get_joysticks",          fn_get_joysticks,          2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_joystick_name_for_id", fn_get_joystick_name_for_id, 3, {WASM_I32,WASM_I32,WASM_I32},          1, {WASM_I32}},
    {"sdl_open_joystick",          fn_open_joystick,          1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_close_joystick",         fn_close_joystick,         1, {WASM_I32},                                  0, {0}},
    {"sdl_joystick_connected",     fn_joystick_connected,     1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_get_num_joystick_axes",  fn_get_num_joystick_axes,  1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_get_num_joystick_buttons", fn_get_num_joystick_buttons, 1, {WASM_I32},                              1, {WASM_I32}},
    {"sdl_get_num_joystick_hats",  fn_get_num_joystick_hats,  1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_get_joystick_axis",      fn_get_joystick_axis,      2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_joystick_button",    fn_get_joystick_button,    2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_joystick_hat",       fn_get_joystick_hat,       2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_rumble_joystick",        fn_rumble_joystick,        4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},     1, {WASM_I32}},
    {"sdl_set_joystick_led",       fn_set_joystick_led,       4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},     1, {WASM_I32}},
    {"sdl_get_joystick_name",      fn_get_joystick_name,      3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_get_joystick_id",        fn_get_joystick_id,        1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_update_joysticks",       fn_update_joysticks,       0, {0},                                         0, {0}},

    /* ---- Gamepad ---- */
    {"sdl_has_gamepad",            fn_has_gamepad,            0, {0},                                         1, {WASM_I32}},
    {"sdl_get_gamepads",           fn_get_gamepads,           2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_is_gamepad",             fn_is_gamepad,             1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_open_gamepad",           fn_open_gamepad,           1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_close_gamepad",          fn_close_gamepad,          1, {WASM_I32},                                  0, {0}},
    {"sdl_gamepad_connected",      fn_gamepad_connected,      1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_get_gamepad_name",       fn_get_gamepad_name,       3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_get_gamepad_type",       fn_get_gamepad_type,       1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_get_gamepad_axis",       fn_get_gamepad_axis,       2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_gamepad_button",     fn_get_gamepad_button,     2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_rumble_gamepad",         fn_rumble_gamepad,         4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},     1, {WASM_I32}},
    {"sdl_rumble_gamepad_triggers",fn_rumble_gamepad_triggers,4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},     1, {WASM_I32}},
    {"sdl_set_gamepad_led",        fn_set_gamepad_led,        4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},     1, {WASM_I32}},
    {"sdl_get_gamepad_id",         fn_get_gamepad_id,         1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_gamepad_has_axis",       fn_gamepad_has_axis,       2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_gamepad_has_button",     fn_gamepad_has_button,     2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_update_gamepads",        fn_update_gamepads,        0, {0},                                         0, {0}},
    {"sdl_get_gamepad_button_label", fn_get_gamepad_button_label, 2, {WASM_I32,WASM_I32},                    1, {WASM_I32}},
    {"sdl_get_gamepad_player_index", fn_get_gamepad_player_index, 1, {WASM_I32},                              1, {WASM_I32}},
    {"sdl_set_gamepad_player_index", fn_set_gamepad_player_index, 2, {WASM_I32,WASM_I32},                    1, {WASM_I32}},

    /* ---- Audio: drivers & devices ---- */
    {"sdl_get_num_audio_drivers",  fn_get_num_audio_drivers,  0, {0},                                         1, {WASM_I32}},
    {"sdl_get_audio_driver",       fn_get_audio_driver,       3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_get_current_audio_driver", fn_get_current_audio_driver, 2, {WASM_I32,WASM_I32},                    1, {WASM_I32}},
    {"sdl_get_audio_playback_devices", fn_get_audio_playback_devices, 2, {WASM_I32,WASM_I32},                1, {WASM_I32}},
    {"sdl_get_audio_recording_devices", fn_get_audio_recording_devices, 2, {WASM_I32,WASM_I32},              1, {WASM_I32}},
    {"sdl_get_audio_device_name",  fn_get_audio_device_name,  3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_open_audio_device",      fn_open_audio_device,      4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},     1, {WASM_I32}},
    {"sdl_close_audio_device",     fn_close_audio_device,     1, {WASM_I32},                                  0, {0}},
    {"sdl_pause_audio_device",     fn_pause_audio_device,     1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_resume_audio_device",    fn_resume_audio_device,    1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_audio_device_paused",    fn_audio_device_paused,    1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_set_audio_device_gain",  fn_set_audio_device_gain,  2, {WASM_I32,WASM_F32},                        1, {WASM_I32}},
    {"sdl_get_audio_device_gain",  fn_get_audio_device_gain,  1, {WASM_I32},                                  1, {WASM_F32}},
    {"sdl_get_audio_device_format",fn_get_audio_device_format,3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},

    /* ---- Audio: streams ---- */
    {"sdl_create_audio_stream",    fn_create_audio_stream,    6, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32}, 1, {WASM_I32}},
    {"sdl_destroy_audio_stream",   fn_destroy_audio_stream,   1, {WASM_I32},                                  0, {0}},
    {"sdl_bind_audio_stream",      fn_bind_audio_stream,      2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_unbind_audio_stream",    fn_unbind_audio_stream,    1, {WASM_I32},                                  0, {0}},
    {"sdl_put_audio_stream_data",  fn_put_audio_stream_data,  3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_get_audio_stream_data",  fn_get_audio_stream_data,  3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_get_audio_stream_available", fn_get_audio_stream_available, 1, {WASM_I32},                          1, {WASM_I32}},
    {"sdl_get_audio_stream_queued",fn_get_audio_stream_queued,1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_flush_audio_stream",     fn_flush_audio_stream,     1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_clear_audio_stream",     fn_clear_audio_stream,     1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_set_audio_stream_format",fn_set_audio_stream_format,7, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32}, 1, {WASM_I32}},
    {"sdl_set_audio_stream_gain",  fn_set_audio_stream_gain,  2, {WASM_I32,WASM_F32},                        1, {WASM_I32}},
    {"sdl_get_audio_stream_gain",  fn_get_audio_stream_gain,  1, {WASM_I32},                                  1, {WASM_F32}},
    {"sdl_set_audio_stream_frequency_ratio", fn_set_audio_stream_freq_ratio, 2, {WASM_I32,WASM_F32},         1, {WASM_I32}},
    {"sdl_get_audio_stream_frequency_ratio", fn_get_audio_stream_freq_ratio, 1, {WASM_I32},                   1, {WASM_F32}},

    /* ---- Audio: simplified ---- */
    {"sdl_open_audio_device_stream", fn_open_audio_device_stream, 4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32}, 1, {WASM_I32}},
    {"sdl_resume_audio_stream_device", fn_resume_audio_stream_device, 1, {WASM_I32},                          1, {WASM_I32}},
    {"sdl_pause_audio_stream_device",  fn_pause_audio_stream_device,  1, {WASM_I32},                          1, {WASM_I32}},

    /* ---- Audio: WAV / utility ---- */
    {"sdl_load_wav",               fn_load_wav,               6, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32}, 1, {WASM_I32}},
    {"sdl_mix_audio",              fn_mix_audio,              5, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_F32},          1, {WASM_I32}},
    {"sdl_get_audio_format_name",  fn_get_audio_format_name,  3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},

    /* ---- Display ---- */
    {"sdl_get_displays",                fn_get_displays,                2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_display_for_window",      fn_get_display_for_window,      0, {0},                                         1, {WASM_I32}},
    {"sdl_get_display_name",            fn_get_display_name,            3, {WASM_I32,WASM_I32,WASM_I32},               1, {WASM_I32}},
    {"sdl_get_display_bounds",          fn_get_display_bounds,          2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_display_usable_bounds",   fn_get_display_usable_bounds,   2, {WASM_I32,WASM_I32},                        1, {WASM_I32}},
    {"sdl_get_display_content_scale",   fn_get_display_content_scale,   1, {WASM_I32},                                  1, {WASM_F32}},
    {"sdl_get_display_orientation",     fn_get_display_orientation,     1, {WASM_I32},                                  1, {WASM_I32}},
    {"sdl_get_display_natural_orientation", fn_get_display_natural_orientation, 1, {WASM_I32},                          1, {WASM_I32}},
    {"sdl_get_display_mode",            fn_get_display_mode,            4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},     1, {WASM_I32}},
    {"sdl_get_window_pixel_density",    fn_get_window_pixel_density,    0, {0},                                         1, {WASM_F32}},
    {"sdl_get_window_display_scale",    fn_get_window_display_scale,    0, {0},                                         1, {WASM_F32}},

    /* ---- Platform ---- */
    {"sdl_get_platform",                fn_get_platform,                0, {0},                                         1, {WASM_I32}},
    {"sdl_get_ticks_ms",                fn_get_ticks_ms,                0, {0},                                         1, {WASM_F32}},
};
#define NUM_SDL_BINDINGS (sizeof(SDL_BINDINGS) / sizeof(SDL_BINDINGS[0]))

/* ------------------------------------------------------------------ */
/*  functype builder                                                   */
/* ------------------------------------------------------------------ */

static wasm_functype_t *sdl_make_ft(uint32_t np, const wasm_valkind_t p[],
                                     uint32_t nr, const wasm_valkind_t r[]) {
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[10];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else {
        wasm_valtype_vec_new_empty(&params);
    }
    if (nr > 0) {
        wasm_valtype_t *rt[1];
        for (uint32_t i = 0; i < nr; i++) rt[i] = wasm_valtype_new(r[i]);
        wasm_valtype_vec_new(&results, nr, rt);
    } else {
        wasm_valtype_vec_new_empty(&results);
    }
    return wasm_functype_new(&params, &results);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void sdl_bindings_init(SdlBindings *b, SDL_Window *default_window) {
    memset(b, 0, sizeof(*b));
    b->default_window = default_window;

    htable_init(&b->ht_audio_device,    64);
    htable_init(&b->ht_audio_stream,   128);
    htable_init(&b->ht_joystick,        32);
    htable_init(&b->ht_gamepad,         32);
}

void sdl_bindings_set_memory(SdlBindings *b, wasm_memory_t *mem) {
    b->memory = mem;
}

size_t sdl_bindings_get_imports(SdlBindings *b, wasm_store_t *store,
                                 const char ***out_names,
                                 wasm_func_t ***out_funcs) {
    static const char *names[NUM_SDL_BINDINGS];
    static wasm_func_t *funcs[NUM_SDL_BINDINGS];

    for (size_t i = 0; i < NUM_SDL_BINDINGS; i++) {
        names[i] = SDL_BINDINGS[i].name;
        wasm_functype_t *ft = sdl_make_ft(
            SDL_BINDINGS[i].np, SDL_BINDINGS[i].params,
            SDL_BINDINGS[i].nr, SDL_BINDINGS[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft, SDL_BINDINGS[i].cb,
                                           b, NULL);
        wasm_functype_delete(ft);
    }

    *out_names = names;
    *out_funcs = funcs;
    return NUM_SDL_BINDINGS;
}

void sdl_bindings_destroy(SdlBindings *b) {
    /* Close any still-open gamepads / joysticks */
    /* (In production you'd iterate the htables and close each.) */

    htable_destroy(&b->ht_audio_device);
    htable_destroy(&b->ht_audio_stream);
    htable_destroy(&b->ht_joystick);
    htable_destroy(&b->ht_gamepad);
}
