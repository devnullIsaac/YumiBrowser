/*
    Host-Side Input WASM Bindings
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

#include "input_bindings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B ((InputBindings *)env)

/* ================================================================== */
/*  Memory helpers                                                     */
/* ================================================================== */

static uint8_t *wasm_mem_base(InputBindings *b) {
    return b->memory ? (uint8_t *)wasm_memory_data(b->memory) : NULL;
}
static size_t wasm_mem_size(InputBindings *b) {
    return b->memory ? wasm_memory_data_size(b->memory) : 0;
}

#define ARG_I32(n) (args->data[(n)].of.i32)
#define ARG_F32(n) (args->data[(n)].of.f32)
#define RET_I32(v) do { res->data[0] = \
    (wasm_val_t){.kind = WASM_I32, .of.i32 = (v)}; } while (0)

#define INPUT_MAX_JOYSTICKS  16
#define INPUT_MAX_GAMEPADS   16
#define INPUT_MAX_TOUCHES    32
#define INPUT_MAX_NAME_LEN   256

/* ================================================================== */
/*  Keyboard                                                           */
/* ================================================================== */

static wasm_trap_t *fn_input_get_key(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)B;
    int32_t sc = ARG_I32(0); int nk = 0;
    const bool *state = SDL_GetKeyboardState(&nk);
    RET_I32((state && sc >= 0 && sc < nk && state[sc]) ? 1 : 0);
    return NULL;
}

static wasm_trap_t *fn_input_get_mod_state(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)B; (void)args;
    RET_I32((int32_t)SDL_GetModState());
    return NULL;
}

/* ================================================================== */
/*  Text input (IME)                                                   */
/* ================================================================== */

/**
 * input_start_text_input() -> i32
 * Enables text input events (SDL_EVENT_TEXT_INPUT, SDL_EVENT_TEXT_EDITING).
 * On mobile this shows the on-screen keyboard. On desktop this activates IME.
 * Returns 1 on success.
 */
static wasm_trap_t *fn_input_start_text_input(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_StartTextInput(B->window) ? 1 : 0);
    return NULL;
}

/**
 * input_stop_text_input() -> i32
 * Disables text input events. On mobile this hides the on-screen keyboard.
 * Returns 1 on success.
 */
static wasm_trap_t *fn_input_stop_text_input(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_StopTextInput(B->window) ? 1 : 0);
    return NULL;
}

/**
 * input_text_input_active() -> i32
 * Returns 1 if text input is currently enabled.
 */
static wasm_trap_t *fn_input_text_input_active(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(SDL_TextInputActive(B->window) ? 1 : 0);
    return NULL;
}

/**
 * input_set_text_input_area(x: i32, y: i32, w: i32, h: i32, cursor: i32) -> i32
 * Sets the area where text is being entered, so the IME candidate window
 * can position itself near the cursor without covering the text.
 * Coordinates are window-relative pixels.
 * cursor = offset of the editing cursor relative to x.
 * Returns 1 on success.
 */
static wasm_trap_t *fn_input_set_text_input_area(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Rect rect = {
        .x = ARG_I32(0), .y = ARG_I32(1),
        .w = ARG_I32(2), .h = ARG_I32(3),
    };
    int cursor = ARG_I32(4);
    RET_I32(SDL_SetTextInputArea(B->window, &rect, cursor) ? 1 : 0);
    return NULL;
}

/* ================================================================== */
/*  Mouse — state                                                      */
/* ================================================================== */

static wasm_trap_t *fn_input_get_mouse_state(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    uint8_t *mem = wasm_mem_base(B); size_t msz = wasm_mem_size(B);
    float x = 0, y = 0;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
    if (mem && (size_t)out_ptr + 8 <= msz) {
        memcpy(mem + out_ptr, &x, 4); memcpy(mem + out_ptr + 4, &y, 4);
    }
    RET_I32((int32_t)buttons);
    return NULL;
}

static wasm_trap_t *fn_input_get_relative_mouse_state(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    uint8_t *mem = wasm_mem_base(B); size_t msz = wasm_mem_size(B);
    float dx = 0, dy = 0;
    SDL_MouseButtonFlags buttons = SDL_GetRelativeMouseState(&dx, &dy);
    if (mem && (size_t)out_ptr + 8 <= msz) {
        memcpy(mem + out_ptr, &dx, 4); memcpy(mem + out_ptr + 4, &dy, 4);
    }
    RET_I32((int32_t)buttons);
    return NULL;
}

/* ================================================================== */
/*  Mouse — cursor control                                             */
/* ================================================================== */

static wasm_trap_t *fn_input_set_cursor(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)B; int32_t type = ARG_I32(0);
    if (type < 0 || type >= SDL_SYSTEM_CURSOR_COUNT) { RET_I32(0); return NULL; }
    SDL_Cursor *c = SDL_CreateSystemCursor((SDL_SystemCursor)type);
    if (!c) { RET_I32(0); return NULL; }
    RET_I32(SDL_SetCursor(c) ? 1 : 0);
    return NULL;
}

static wasm_trap_t *fn_input_show_cursor(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)B; (void)args; RET_I32(SDL_ShowCursor() ? 1 : 0); return NULL;
}

static wasm_trap_t *fn_input_hide_cursor(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)B; (void)args; RET_I32(SDL_HideCursor() ? 1 : 0); return NULL;
}

static wasm_trap_t *fn_input_warp_mouse(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; SDL_WarpMouseInWindow(B->window, ARG_F32(0), ARG_F32(1)); return NULL;
}

static wasm_trap_t *fn_input_set_relative_mouse(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    RET_I32(SDL_SetWindowRelativeMouseMode(B->window, ARG_I32(0) != 0) ? 1 : 0);
    return NULL;
}

static wasm_trap_t *fn_input_capture_mouse(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)B; RET_I32(SDL_CaptureMouse(ARG_I32(0) != 0) ? 1 : 0); return NULL;
}

/* ================================================================== */
/*  Joystick                                                           */
/* ================================================================== */

static wasm_trap_t *fn_input_get_joysticks(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0); int32_t max = ARG_I32(1);
    uint8_t *mem = wasm_mem_base(B); size_t msz = wasm_mem_size(B);
    if (!mem || max <= 0 || max > INPUT_MAX_JOYSTICKS ||
        (size_t)out_ptr + (size_t)max * 4 > msz) { RET_I32(0); return NULL; }
    int count = 0; SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (!ids) { RET_I32(0); return NULL; }
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++) { uint32_t id = (uint32_t)ids[i]; memcpy(mem + out_ptr + i*4, &id, 4); }
    SDL_free(ids); RET_I32(n); return NULL;
}

static wasm_trap_t *fn_input_open_joystick(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t iid = (uint32_t)ARG_I32(0);
    SDL_Joystick *js = SDL_OpenJoystick((SDL_JoystickID)iid);
    if (!js) { RET_I32(0); return NULL; }
    uint32_t h = htable_insert(&B->joysticks, js);
    printf("[input] Opened joystick %u -> handle %u (%s) [%d axes, %d buttons, %d hats]\n",
           iid, h, SDL_GetJoystickName(js) ? SDL_GetJoystickName(js) : "?",
           SDL_GetNumJoystickAxes(js), SDL_GetNumJoystickButtons(js), SDL_GetNumJoystickHats(js));
    RET_I32((int32_t)h); return NULL;
}

static wasm_trap_t *fn_input_close_joystick(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h = (uint32_t)ARG_I32(0);
    SDL_Joystick *js = htable_get(&B->joysticks, h);
    if (js) { SDL_CloseJoystick(js); htable_remove(&B->joysticks, h); }
    return NULL;
}

static wasm_trap_t *fn_input_joystick_axis(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->joysticks, (uint32_t)ARG_I32(0));
    int32_t ax = ARG_I32(1);
    if (!js || ax < 0 || ax >= SDL_GetNumJoystickAxes(js)) { RET_I32(0); return NULL; }
    RET_I32((int32_t)SDL_GetJoystickAxis(js, ax)); return NULL;
}

static wasm_trap_t *fn_input_joystick_button(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->joysticks, (uint32_t)ARG_I32(0));
    int32_t btn = ARG_I32(1);
    if (!js || btn < 0 || btn >= SDL_GetNumJoystickButtons(js)) { RET_I32(0); return NULL; }
    RET_I32(SDL_GetJoystickButton(js, btn) ? 1 : 0); return NULL;
}

static wasm_trap_t *fn_input_joystick_hat(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->joysticks, (uint32_t)ARG_I32(0));
    int32_t hat = ARG_I32(1);
    if (!js || hat < 0 || hat >= SDL_GetNumJoystickHats(js)) { RET_I32(0); return NULL; }
    RET_I32((int32_t)SDL_GetJoystickHat(js, hat)); return NULL;
}

static wasm_trap_t *fn_input_joystick_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t h = (uint32_t)ARG_I32(0), out = (uint32_t)ARG_I32(1); int32_t ml = ARG_I32(2);
    uint8_t *mem = wasm_mem_base(B); size_t msz = wasm_mem_size(B);
    SDL_Joystick *js = htable_get(&B->joysticks, h);
    if (!js || !mem || ml <= 0 || ml > INPUT_MAX_NAME_LEN || (size_t)out + ml > msz) { RET_I32(0); return NULL; }
    const char *name = SDL_GetJoystickName(js); if (!name) name = "";
    int len = (int)strlen(name); if (len >= ml) len = ml - 1;
    memcpy(mem + out, name, len); mem[out + len] = '\0';
    RET_I32(len); return NULL;
}

static wasm_trap_t *fn_input_joystick_num_axes(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->joysticks, (uint32_t)ARG_I32(0));
    RET_I32(js ? SDL_GetNumJoystickAxes(js) : 0); return NULL;
}

static wasm_trap_t *fn_input_joystick_num_buttons(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->joysticks, (uint32_t)ARG_I32(0));
    RET_I32(js ? SDL_GetNumJoystickButtons(js) : 0); return NULL;
}

static wasm_trap_t *fn_input_joystick_num_hats(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->joysticks, (uint32_t)ARG_I32(0));
    RET_I32(js ? SDL_GetNumJoystickHats(js) : 0); return NULL;
}

static wasm_trap_t *fn_input_joystick_rumble(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Joystick *js = htable_get(&B->joysticks, (uint32_t)ARG_I32(0));
    if (!js) { RET_I32(0); return NULL; }
    RET_I32(SDL_RumbleJoystick(js, (Uint16)((uint32_t)ARG_I32(1)&0xFFFF),
            (Uint16)((uint32_t)ARG_I32(2)&0xFFFF), (uint32_t)ARG_I32(3)) ? 1 : 0);
    return NULL;
}

/* ================================================================== */
/*  Gamepad                                                            */
/* ================================================================== */

static wasm_trap_t *fn_input_get_gamepads(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0); int32_t max = ARG_I32(1);
    uint8_t *mem = wasm_mem_base(B); size_t msz = wasm_mem_size(B);
    if (!mem || max <= 0 || max > INPUT_MAX_GAMEPADS ||
        (size_t)out_ptr + (size_t)max * 4 > msz) { RET_I32(0); return NULL; }
    int count = 0; SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids) { RET_I32(0); return NULL; }
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++) { uint32_t id = (uint32_t)ids[i]; memcpy(mem + out_ptr + i*4, &id, 4); }
    SDL_free(ids); RET_I32(n); return NULL;
}

static wasm_trap_t *fn_input_open_gamepad(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t iid = (uint32_t)ARG_I32(0);
    SDL_Gamepad *gp = SDL_OpenGamepad((SDL_JoystickID)iid);
    if (!gp) { RET_I32(0); return NULL; }
    uint32_t h = htable_insert(&B->gamepads, gp);
    printf("[input] Opened gamepad %u -> handle %u (%s)\n", iid, h,
           SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "?");
    RET_I32((int32_t)h); return NULL;
}

static wasm_trap_t *fn_input_close_gamepad(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h = (uint32_t)ARG_I32(0);
    SDL_Gamepad *gp = htable_get(&B->gamepads, h);
    if (gp) { SDL_CloseGamepad(gp); htable_remove(&B->gamepads, h); }
    return NULL;
}

static wasm_trap_t *fn_input_gamepad_axis(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->gamepads, (uint32_t)ARG_I32(0));
    int32_t ax = ARG_I32(1);
    if (!gp || ax < 0 || ax >= SDL_GAMEPAD_AXIS_COUNT) { RET_I32(0); return NULL; }
    RET_I32((int32_t)SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)ax)); return NULL;
}

static wasm_trap_t *fn_input_gamepad_button(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->gamepads, (uint32_t)ARG_I32(0));
    int32_t btn = ARG_I32(1);
    if (!gp || btn < 0 || btn >= SDL_GAMEPAD_BUTTON_COUNT) { RET_I32(0); return NULL; }
    RET_I32(SDL_GetGamepadButton(gp, (SDL_GamepadButton)btn) ? 1 : 0); return NULL;
}

static wasm_trap_t *fn_input_gamepad_rumble(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->gamepads, (uint32_t)ARG_I32(0));
    if (!gp) { RET_I32(0); return NULL; }
    RET_I32(SDL_RumbleGamepad(gp, (Uint16)((uint32_t)ARG_I32(1)&0xFFFF),
            (Uint16)((uint32_t)ARG_I32(2)&0xFFFF), (uint32_t)ARG_I32(3)) ? 1 : 0);
    return NULL;
}

static wasm_trap_t *fn_input_gamepad_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t h = (uint32_t)ARG_I32(0), out = (uint32_t)ARG_I32(1); int32_t ml = ARG_I32(2);
    uint8_t *mem = wasm_mem_base(B); size_t msz = wasm_mem_size(B);
    SDL_Gamepad *gp = htable_get(&B->gamepads, h);
    if (!gp || !mem || ml <= 0 || ml > INPUT_MAX_NAME_LEN || (size_t)out + ml > msz) { RET_I32(0); return NULL; }
    const char *name = SDL_GetGamepadName(gp); if (!name) name = "";
    int len = (int)strlen(name); if (len >= ml) len = ml - 1;
    memcpy(mem + out, name, len); mem[out + len] = '\0';
    RET_I32(len); return NULL;
}

static wasm_trap_t *fn_input_gamepad_type(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    SDL_Gamepad *gp = htable_get(&B->gamepads, (uint32_t)ARG_I32(0));
    RET_I32(gp ? (int32_t)SDL_GetGamepadType(gp) : 0); return NULL;
}

/* ================================================================== */
/*  Touch                                                              */
/* ================================================================== */

static wasm_trap_t *fn_input_get_touch_device_count(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)B; (void)args; int c = 0;
    SDL_TouchID *d = SDL_GetTouchDevices(&c); SDL_free(d);
    RET_I32(c); return NULL;
}

static wasm_trap_t *fn_input_get_touch_device_info(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    int32_t idx = ARG_I32(0); uint32_t out = (uint32_t)ARG_I32(1);
    uint8_t *mem = wasm_mem_base(B); size_t msz = wasm_mem_size(B);
    if (!mem || (size_t)out + 8 > msz) { RET_I32(0); return NULL; }
    int c = 0; SDL_TouchID *d = SDL_GetTouchDevices(&c);
    if (!d || idx < 0 || idx >= c) { SDL_free(d); RET_I32(0); return NULL; }
    SDL_TouchID tid = d[idx]; SDL_free(d);
    int32_t type = (int32_t)SDL_GetTouchDeviceType(tid);
    int fc = 0; SDL_Finger **f = SDL_GetTouchFingers(tid, &fc); SDL_free(f);
    memcpy(mem + out, &type, 4); memcpy(mem + out + 4, &fc, 4);
    RET_I32(1); return NULL;
}

static wasm_trap_t *fn_input_get_touches(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t out_ptr = (uint32_t)ARG_I32(0); int32_t max = ARG_I32(1);
    uint8_t *mem = wasm_mem_base(B); size_t msz = wasm_mem_size(B);
    if (!mem || max <= 0 || max > INPUT_MAX_TOUCHES ||
        (size_t)out_ptr + (size_t)max * 16 > msz) { RET_I32(0); return NULL; }
    int dc = 0; SDL_TouchID *devs = SDL_GetTouchDevices(&dc);
    if (!devs) { RET_I32(0); return NULL; }
    int total = 0;
    for (int d = 0; d < dc && total < max; d++) {
        int fc = 0; SDL_Finger **fingers = SDL_GetTouchFingers(devs[d], &fc);
        if (!fingers) continue;
        for (int f = 0; f < fc && total < max; f++) {
            SDL_Finger *fin = fingers[f];
            uint32_t fid = (uint32_t)(fin->id & 0xFFFFFFFFu);
            uint8_t *p = mem + out_ptr + (size_t)total * 16;
            memcpy(p+0, &fid, 4); memcpy(p+4, &fin->x, 4);
            memcpy(p+8, &fin->y, 4); memcpy(p+12, &fin->pressure, 4);
            total++;
        }
        SDL_free(fingers);
    }
    SDL_free(devs); RET_I32(total); return NULL;
}

/* ================================================================== */
/*  Binding table                                                      */
/* ================================================================== */

typedef struct {
    const char *name; wasm_func_callback_with_env_t cb;
    uint32_t np; wasm_valkind_t params[8];
    uint32_t nr; wasm_valkind_t results[1];
} InputBindingEntry;

#define I WASM_I32
#define F WASM_F32

static const InputBindingEntry INPUT_BINDINGS[] = {
    /* keyboard (2) */
    {"input_get_key",                   fn_input_get_key,                   1, {I},           1, {I}},
    {"input_get_mod_state",             fn_input_get_mod_state,             0, {0},           1, {I}},
    /* text input / IME (4) */
    {"input_start_text_input",          fn_input_start_text_input,          0, {0},           1, {I}},
    {"input_stop_text_input",           fn_input_stop_text_input,           0, {0},           1, {I}},
    {"input_text_input_active",         fn_input_text_input_active,         0, {0},           1, {I}},
    {"input_set_text_input_area",       fn_input_set_text_input_area,       5, {I,I,I,I,I},   1, {I}},
    /* mouse — state (2) */
    {"input_get_mouse_state",           fn_input_get_mouse_state,           1, {I},           1, {I}},
    {"input_get_relative_mouse_state",  fn_input_get_relative_mouse_state,  1, {I},           1, {I}},
    /* mouse — cursor control (6) */
    {"input_set_cursor",                fn_input_set_cursor,                1, {I},           1, {I}},
    {"input_show_cursor",               fn_input_show_cursor,               0, {0},           1, {I}},
    {"input_hide_cursor",               fn_input_hide_cursor,               0, {0},           1, {I}},
    {"input_warp_mouse",                fn_input_warp_mouse,                2, {F,F},         0, {0}},
    {"input_set_relative_mouse",        fn_input_set_relative_mouse,        1, {I},           1, {I}},
    {"input_capture_mouse",             fn_input_capture_mouse,             1, {I},           1, {I}},
    /* joystick (12) */
    {"input_get_joysticks",             fn_input_get_joysticks,             2, {I,I},         1, {I}},
    {"input_open_joystick",             fn_input_open_joystick,             1, {I},           1, {I}},
    {"input_close_joystick",            fn_input_close_joystick,            1, {I},           0, {0}},
    {"input_joystick_axis",             fn_input_joystick_axis,             2, {I,I},         1, {I}},
    {"input_joystick_button",           fn_input_joystick_button,           2, {I,I},         1, {I}},
    {"input_joystick_hat",              fn_input_joystick_hat,              2, {I,I},         1, {I}},
    {"input_joystick_name",             fn_input_joystick_name,             3, {I,I,I},       1, {I}},
    {"input_joystick_num_axes",         fn_input_joystick_num_axes,         1, {I},           1, {I}},
    {"input_joystick_num_buttons",      fn_input_joystick_num_buttons,      1, {I},           1, {I}},
    {"input_joystick_num_hats",         fn_input_joystick_num_hats,         1, {I},           1, {I}},
    {"input_joystick_rumble",           fn_input_joystick_rumble,           4, {I,I,I,I},     1, {I}},
    /* gamepad (8) */
    {"input_get_gamepads",              fn_input_get_gamepads,              2, {I,I},         1, {I}},
    {"input_open_gamepad",              fn_input_open_gamepad,              1, {I},           1, {I}},
    {"input_close_gamepad",             fn_input_close_gamepad,             1, {I},           0, {0}},
    {"input_gamepad_axis",              fn_input_gamepad_axis,              2, {I,I},         1, {I}},
    {"input_gamepad_button",            fn_input_gamepad_button,            2, {I,I},         1, {I}},
    {"input_gamepad_rumble",            fn_input_gamepad_rumble,            4, {I,I,I,I},     1, {I}},
    {"input_gamepad_name",              fn_input_gamepad_name,              3, {I,I,I},       1, {I}},
    {"input_gamepad_type",              fn_input_gamepad_type,              1, {I},           1, {I}},
    /* touch (3) */
    {"input_get_touch_device_count",    fn_input_get_touch_device_count,    0, {0},           1, {I}},
    {"input_get_touch_device_info",     fn_input_get_touch_device_info,     2, {I,I},         1, {I}},
    {"input_get_touches",               fn_input_get_touches,               2, {I,I},         1, {I}},
};

#undef I
#undef F

#define NUM_INPUT_BINDINGS (sizeof(INPUT_BINDINGS) / sizeof(INPUT_BINDINGS[0]))

static wasm_functype_t *make_ft(uint32_t np, const wasm_valkind_t p[],
                                uint32_t nr, const wasm_valkind_t r[]) {
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[8];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else { wasm_valtype_vec_new_empty(&params); }
    if (nr > 0) {
        wasm_valtype_t *rt[1]; rt[0] = wasm_valtype_new(r[0]);
        wasm_valtype_vec_new(&results, nr, rt);
    } else { wasm_valtype_vec_new_empty(&results); }
    return wasm_functype_new(&params, &results);
}

void input_bindings_init(InputBindings *b, SDL_Window *win) {
    memset(b, 0, sizeof(*b));
    b->window = win;
    htable_init(&b->joysticks, 8);
    htable_init(&b->gamepads, 8);
    printf("[input] Input bindings ready (%zu imports)\n", NUM_INPUT_BINDINGS);
}

void input_bindings_set_memory(InputBindings *b, wasm_memory_t *mem) { b->memory = mem; }

size_t input_bindings_get_imports(InputBindings *b, wasm_store_t *store,
                                  const char ***out_names, wasm_func_t ***out_funcs) {
    static const char *names[NUM_INPUT_BINDINGS];
    static wasm_func_t *funcs[NUM_INPUT_BINDINGS];
    for (size_t i = 0; i < NUM_INPUT_BINDINGS; i++) {
        names[i] = INPUT_BINDINGS[i].name;
        wasm_functype_t *ft = make_ft(INPUT_BINDINGS[i].np, INPUT_BINDINGS[i].params,
                                       INPUT_BINDINGS[i].nr, INPUT_BINDINGS[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft, INPUT_BINDINGS[i].cb, b, NULL);
        wasm_functype_delete(ft);
    }
    *out_names = names; *out_funcs = funcs;
    return NUM_INPUT_BINDINGS;
}

void input_bindings_destroy(InputBindings *b) {
    for (uint32_t i = 1; i <= b->joysticks.capacity; i++) {
        SDL_Joystick *js = htable_get(&b->joysticks, i);
        if (js) { printf("[input] Leaked joystick %u\n", i); SDL_CloseJoystick(js); }
    }
    htable_destroy(&b->joysticks);
    for (uint32_t i = 1; i <= b->gamepads.capacity; i++) {
        SDL_Gamepad *gp = htable_get(&b->gamepads, i);
        if (gp) { printf("[input] Leaked gamepad %u\n", i); SDL_CloseGamepad(gp); }
    }
    htable_destroy(&b->gamepads);
    memset(b, 0, sizeof(*b));
}
