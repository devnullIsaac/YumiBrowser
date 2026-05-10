/*
    Yumi SDK — Input Device Polling WASM Imports
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

#ifndef WASM_INPUT_H
#define WASM_INPUT_H

/**
 * @file wasm_input.h
 * @brief WebAssembly guest imports for host-side input device polling.
 *
 * @details
 * This header declares the host-imported functions that allow WASM guest
 * modules to query the state of keyboards, mice, joysticks, gamepads, and
 * touchscreens. It also documents the optional event-export callbacks that
 * the guest may define to receive asynchronous input events.
 *
 * ## Architecture
 * The input system has two complementary halves:
 *
 *   1. **Imports** — Synchronous polling functions the guest calls to read
 *      the current state of input devices. These are useful in a frame loop
 *      where the guest checks `input_get_key()` or `input_gamepad_button()`
 *      every tick.
 *
 *   2. **Event Exports** — Optional asynchronous callbacks the guest defines
 *      with `__attribute__((export_name(...)))`. The host invokes these
 *      whenever the corresponding input event occurs. The host silently skips
 *      any export that is not defined, so guests only need to implement the
 *      callbacks they care about.
 *
 * ## Clipboard Note
 * Clipboard access is intentionally **not** in this header. It lives in
 * `wasm_clipboard.h` so that the dashboard application can mediate access
 * and enforce security policy (preventing silent clipboard reads).
 *
 * ## Scancode vs Keycode
 * The keyboard API uses **scancodes** (physical key positions, layout-
 * independent) for polling and events. The `on_key` event also provides
 * the **keycode** (layout-dependent character) for text-aware processing.
 *
 * ## Typical Frame Loop
 * @code
 *   void game_loop() {
 *       if (input_get_key(INPUT_SCANCODE_SPACE)) {
 *           player_jump();
 *       }
 *       float mouse_xy[2];
 *       input_get_mouse_state(mouse_xy);
 *       aim_at(mouse_xy[0], mouse_xy[1]);
 *   }
 * @endcode
 *
 * @see wasm_clipboard.h for clipboard operations.
 * @see wasm_sdl.h for additional SDL3 constants and display functions.
 */

#include <stdint.h>

/** @cond INTERNAL */
#define IMPORT __attribute__((import_module("env")))
/** @endcond */

/* ================================================================== */
/*  IMPORTS — Keyboard                                                 */
/* ================================================================== */

/**
 * @brief Check whether a specific key is currently pressed.
 *
 * @details
 * Queries the real-time keyboard state for the given scancode.
 * This is a polling API — it returns the instantaneous state at the
 * moment of the call. For event-driven input, implement the `on_key`
 * export callback.
 *
 * @param[in] scancode  SDL scancode (see INPUT_SCANCODE_* constants below).
 * @return 1 if the key is pressed, 0 if released or invalid scancode.
 */
IMPORT __attribute__((import_name("input_get_key"))) int
input_get_key(int scancode);

/**
 * @brief Get the current keyboard modifier state.
 *
 * @details
 * Returns a bitmask of currently-held modifier keys (Shift, Ctrl, Alt, Meta).
 * Use bitwise AND with the modifier constants to test individual modifiers.
 *
 * @return Bitmask of active modifiers.
 */
IMPORT __attribute__((import_name("input_get_mod_state"))) int
input_get_mod_state(void);

/* ================================================================== */
/*  IMPORTS — Text input / IME                                         */
/* ================================================================== */

/**
 * @brief Enable text input events for the window.
 *
 * @details
 * Call this when a text-editing widget gains focus. On mobile platforms
 * this shows the on-screen keyboard; on desktop it activates the Input
 * Method Editor (IME) for composition of complex scripts (CJK, etc.).
 * While active, the host dispatches `on_char` events for committed text
 * and `on_text_editing` events for composition state changes.
 *
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("input_start_text_input"))) int
input_start_text_input(void);

/**
 * @brief Disable text input events.
 *
 * @details
 * Call this when the text field loses focus or is destroyed. On mobile
 * this hides the on-screen keyboard. No further `on_char` or composition
 * events will be delivered until text input is started again.
 *
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("input_stop_text_input"))) int
input_stop_text_input(void);

/**
 * @brief Check whether text input is currently active.
 * @return 1 if active, 0 otherwise.
 */
IMPORT __attribute__((import_name("input_text_input_active"))) int
input_text_input_active(void);

/**
 * @brief Set the text input area for IME candidate window positioning.
 *
 * @details
 * Provides the host with the screen rectangle of the text being edited
 * so that the IME candidate window can position itself near the cursor
 * without obscuring the text. This is essential for good CJK input
 * experience.
 *
 * @param[in] x       X coordinate of the text area, in window pixels.
 * @param[in] y       Y coordinate of the text area, in window pixels.
 * @param[in] w       Width of the text area, in window pixels.
 * @param[in] h       Height of the text area, in window pixels.
 * @param[in] cursor  Horizontal offset of the editing cursor relative to x.
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("input_set_text_input_area"))) int
input_set_text_input_area(int x, int y, int w, int h, int cursor);

/* ================================================================== */
/*  IMPORTS — Mouse state                                              */
/* ================================================================== */

/**
 * @brief Get the current mouse position and button state.
 *
 * @details
 * Returns the mouse cursor position in window coordinates and the current
 * button bitmask. Writes two floats (x, y) into @p out_xy if non-NULL.
 *
 * @param[out] out_xy  Optional pointer to a 2-element float array for [x, y].
 * @return Bitmask of pressed mouse buttons.
 */
IMPORT __attribute__((import_name("input_get_mouse_state"))) int
input_get_mouse_state(float *out_xy);

/**
 * @brief Get the relative mouse motion since the last call.
 *
 * @details
 * Returns the mouse movement delta (dx, dy) since the previous call to
 * this function. Useful for first-person camera controls. Writes two
 * floats into @p out_dxy if non-NULL.
 *
 * @param[out] out_dxy  Optional pointer to a 2-element float array for [dx, dy].
 * @return Bitmask of pressed mouse buttons.
 */
IMPORT __attribute__((import_name("input_get_relative_mouse_state"))) int
input_get_relative_mouse_state(float *out_dxy);

/* ================================================================== */
/*  IMPORTS — Mouse cursor control                                     */
/* ================================================================== */

/**
 * @brief Set the mouse cursor appearance.
 *
 * @details
 * Changes the system cursor to one of the standard shapes defined in
 * `widget.h` (WIDGET_CURSOR_*). The host maps these to platform-native
 * cursor themes.
 *
 * @param[in] type  Cursor type identifier.
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("input_set_cursor"))) int
input_set_cursor(int type);

/**
 * @brief Show the mouse cursor.
 * @return 1 on success.
 */
IMPORT __attribute__((import_name("input_show_cursor"))) int
input_show_cursor(void);

/**
 * @brief Hide the mouse cursor.
 * @return 1 on success.
 */
IMPORT __attribute__((import_name("input_hide_cursor"))) int
input_hide_cursor(void);

/**
 * @brief Warp the mouse cursor to a specific window position.
 *
 * @details
 * Teleports the mouse cursor to the given window coordinates. This
 * generates a motion event. Useful for centering the cursor in
 * first-person controls.
 *
 * @param[in] x  Target X coordinate in window pixels.
 * @param[in] y  Target Y coordinate in window pixels.
 */
IMPORT __attribute__((import_name("input_warp_mouse"))) void
input_warp_mouse(float x, float y);

/**
 * @brief Enable or disable relative mouse mode.
 *
 * @details
 * In relative mode, the mouse cursor is hidden and mouse motion events
 * are delivered as relative deltas only. The cursor does not leave the
 * window. Ideal for first-person shooters and 3D orbit controls.
 *
 * @param[in] enabled  1 to enable relative mode, 0 to disable.
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("input_set_relative_mouse"))) int
input_set_relative_mouse(int enabled);

/**
 * @brief Capture or release the mouse.
 *
 * @details
 * When captured, the mouse is confined to the window and all motion
 * is reported even when the cursor is outside the window bounds.
 *
 * @param[in] enabled  1 to capture, 0 to release.
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("input_capture_mouse"))) int
input_capture_mouse(int enabled);

/* ================================================================== */
/*  IMPORTS — Joystick (raw)                                           */
/* ================================================================== */

/**
 * @brief Enumerate connected joysticks.
 *
 * @details
 * Writes up to @p max_count joystick instance IDs into @p out_ids.
 * These IDs are stable for the lifetime of the device connection.
 *
 * @param[out] out_ids     Buffer for joystick instance IDs.
 * @param[in]  max_count   Capacity of @p out_ids.
 * @return Actual number of joysticks written.
 */
IMPORT __attribute__((import_name("input_get_joysticks"))) int
input_get_joysticks(uint32_t *out_ids, int max_count);

/**
 * @brief Open a joystick for polling.
 *
 * @param[in] instance_id  Joystick instance ID from input_get_joysticks().
 * @return A positive joystick handle on success, 0 on failure.
 */
IMPORT __attribute__((import_name("input_open_joystick"))) int
input_open_joystick(uint32_t instance_id);

/**
 * @brief Close a joystick and release host resources.
 * @param[in] handle  Valid joystick handle.
 */
IMPORT __attribute__((import_name("input_close_joystick"))) void
input_close_joystick(int handle);

/**
 * @brief Read a joystick axis value.
 *
 * @details
 * Returns the current position of an analog axis. Range is
 * -32768 to 32767, with 0 as the centered position.
 *
 * @param[in] handle  Valid joystick handle.
 * @param[in] axis    Zero-based axis index.
 * @return Axis value, or 0 if invalid.
 */
IMPORT __attribute__((import_name("input_joystick_axis"))) int
input_joystick_axis(int handle, int axis);

/**
 * @brief Read a joystick button state.
 *
 * @param[in] handle  Valid joystick handle.
 * @param[in] button  Zero-based button index.
 * @return 1 if pressed, 0 if released or invalid.
 */
IMPORT __attribute__((import_name("input_joystick_button"))) int
input_joystick_button(int handle, int button);

/**
 * @brief Read a joystick hat (POV) state.
 *
 * @details
 * Returns a bitmask representing the hat direction. See SDL_HAT_*
 * constants in wasm_sdl.h for decoding.
 *
 * @param[in] handle  Valid joystick handle.
 * @param[in] hat     Zero-based hat index.
 * @return Hat bitmask, or 0 if centered/invalid.
 */
IMPORT __attribute__((import_name("input_joystick_hat"))) int
input_joystick_hat(int handle, int hat);

/**
 * @brief Get the name of a joystick.
 *
 * @param[in]  handle   Valid joystick handle.
 * @param[out] out_name Buffer for the NUL-terminated name string.
 * @param[in]  max_len  Capacity of @p out_name in bytes.
 * @return Bytes written (excluding NUL), or 0 on error.
 */
IMPORT __attribute__((import_name("input_joystick_name"))) int
input_joystick_name(int handle, char *out_name, int max_len);

/**
 * @brief Get the number of axes on a joystick.
 * @param[in] handle  Valid joystick handle.
 * @return Number of axes, or 0 on error.
 */
IMPORT __attribute__((import_name("input_joystick_num_axes"))) int
input_joystick_num_axes(int handle);

/**
 * @brief Get the number of buttons on a joystick.
 * @param[in] handle  Valid joystick handle.
 * @return Number of buttons, or 0 on error.
 */
IMPORT __attribute__((import_name("input_joystick_num_buttons"))) int
input_joystick_num_buttons(int handle);

/**
 * @brief Get the number of hats (POV switches) on a joystick.
 * @param[in] handle  Valid joystick handle.
 * @return Number of hats, or 0 on error.
 */
IMPORT __attribute__((import_name("input_joystick_num_hats"))) int
input_joystick_num_hats(int handle);

/**
 * @brief Trigger haptic rumble on a joystick.
 *
 * @details
 * Activates the joystick's force-feedback motors for the specified
 * duration. Not all joysticks support rumble.
 *
 * @param[in] handle      Valid joystick handle.
 * @param[in] low_freq    Low-frequency motor intensity (0-65535).
 * @param[in] high_freq   High-frequency motor intensity (0-65535).
 * @param[in] duration_ms Rumble duration in milliseconds.
 * @return 1 on success, 0 if rumble is unsupported.
 */
IMPORT __attribute__((import_name("input_joystick_rumble"))) int
input_joystick_rumble(int handle, int low_freq, int high_freq, int duration_ms);

/* ================================================================== */
/*  IMPORTS — Gamepad (mapped)                                         */
/* ================================================================== */

/**
 * @brief Enumerate connected gamepads.
 *
 * @details
 * Writes up to @p max_count gamepad instance IDs into @p out_ids.
 * Gamepads are a subset of joysticks that have a standardized button
 * and axis mapping (ABXY, triggers, analog sticks).
 *
 * @param[out] out_ids     Buffer for gamepad instance IDs.
 * @param[in]  max_count   Capacity of @p out_ids.
 * @return Actual number of gamepads written.
 */
IMPORT __attribute__((import_name("input_get_gamepads"))) int
input_get_gamepads(uint32_t *out_ids, int max_count);

/**
 * @brief Open a gamepad for polling.
 *
 * @param[in] instance_id  Gamepad instance ID from input_get_gamepads().
 * @return A positive gamepad handle on success, 0 on failure.
 */
IMPORT __attribute__((import_name("input_open_gamepad"))) int
input_open_gamepad(uint32_t instance_id);

/**
 * @brief Close a gamepad and release host resources.
 * @param[in] handle  Valid gamepad handle.
 */
IMPORT __attribute__((import_name("input_close_gamepad"))) void
input_close_gamepad(int handle);

/**
 * @brief Read a gamepad axis value.
 *
 * @details
 * Gamepad axes use standardized indices (see SDL_GAMEPAD_AXIS_* in
 * wasm_sdl.h). Sticks range -32768..32767; triggers range 0..32767.
 *
 * @param[in] handle  Valid gamepad handle.
 * @param[in] axis    Gamepad axis index.
 * @return Axis value, or 0 if invalid.
 */
IMPORT __attribute__((import_name("input_gamepad_axis"))) int
input_gamepad_axis(int handle, int axis);

/**
 * @brief Read a gamepad button state.
 *
 * @details
 * Gamepad buttons use standardized indices (see SDL_GAMEPAD_BUTTON_*
 * in wasm_sdl.h).
 *
 * @param[in] handle  Valid gamepad handle.
 * @param[in] button  Gamepad button index.
 * @return 1 if pressed, 0 if released or invalid.
 */
IMPORT __attribute__((import_name("input_gamepad_button"))) int
input_gamepad_button(int handle, int button);

/**
 * @brief Trigger haptic rumble on a gamepad.
 *
 * @param[in] handle      Valid gamepad handle.
 * @param[in] low_freq    Low-frequency motor intensity (0-65535).
 * @param[in] high_freq   High-frequency motor intensity (0-65535).
 * @param[in] duration_ms Rumble duration in milliseconds.
 * @return 1 on success, 0 if rumble is unsupported.
 */
IMPORT __attribute__((import_name("input_gamepad_rumble"))) int
input_gamepad_rumble(int handle, int low_freq, int high_freq, int duration_ms);

/**
 * @brief Get the name of a gamepad.
 *
 * @param[in]  handle   Valid gamepad handle.
 * @param[out] out_name Buffer for the NUL-terminated name string.
 * @param[in]  max_len  Capacity of @p out_name in bytes.
 * @return Bytes written (excluding NUL), or 0 on error.
 */
IMPORT __attribute__((import_name("input_gamepad_name"))) int
input_gamepad_name(int handle, char *out_name, int max_len);

/**
 * @brief Get the type of a gamepad.
 *
 * @details
 * Returns an SDL_GAMEPAD_TYPE_* enum value indicating the detected
 * controller family (Xbox, PlayStation, Nintendo Switch, etc.).
 *
 * @param[in] handle  Valid gamepad handle.
 * @return Gamepad type enum value, or 0 if unknown.
 */
IMPORT __attribute__((import_name("input_gamepad_type"))) int
input_gamepad_type(int handle);

/* ================================================================== */
/*  IMPORTS — Touch                                                    */
/* ================================================================== */

/**
 * @brief Get the number of touch devices.
 * @return Number of touch input devices.
 */
IMPORT __attribute__((import_name("input_get_touch_device_count"))) int
input_get_touch_device_count(void);

/**
 * @brief Get information about a specific touch device.
 *
 * @param[in]  index    Zero-based touch device index.
 * @param[out] out_ptr  Pointer to a device info struct in WASM memory.
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("input_get_touch_device_info"))) int
input_get_touch_device_info(int index, void *out_ptr);

/**
 * @brief One active touch finger.
 *
 * @details
 * Represents a single touch contact on a capacitive touchscreen.
 * The id is stable for the lifetime of the touch (from down to up).
 */
typedef struct {
    uint32_t id;       /**< Unique finger identifier. */
    float x;           /**< X position in window coordinates [0, width]. */
    float y;           /**< Y position in window coordinates [0, height]. */
    float pressure;    /**< Normalized pressure [0.0, 1.0], or 0 if unsupported. */
} input_touch_finger_t;

/**
 * @brief Get all currently active touch contacts.
 *
 * @details
 * Polls the current state of all active touches and writes them into
 * @p out_fingers. This is the polling counterpart to the `on_touch`
 * event export.
 *
 * @param[out] out_fingers  Buffer for touch finger records.
 * @param[in]  max_touches  Capacity of @p out_fingers.
 * @return Number of active touches written, or 0 if none.
 */
IMPORT __attribute__((import_name("input_get_touches"))) int
input_get_touches(input_touch_finger_t *out_fingers, int max_touches);

/** @cond INTERNAL */
#undef IMPORT
/** @endcond */

/* ================================================================== */
/*  EVENT EXPORTS — optional guest callbacks                           */
/* ================================================================== */

/**
 * @defgroup InputEventExports Optional guest-side input event callbacks
 * @brief Asynchronous callbacks the guest may export to receive input events.
 *
 * @details
 * These callbacks are invoked by the host when the corresponding input
 * event occurs. Define them with `__attribute__((export_name("name")))`.
 * The host detects which exports are present and skips any that are not
 * implemented. Guests only need to export the callbacks they care about.
 *
 * All callbacks run synchronously on the main thread during event
 * processing. They should return quickly to avoid frame drops.
 *
 * ## Keyboard / Text Events
 * @code
 *   __attribute__((export_name("on_key")))
 *   void on_key(int scancode, int keycode, int mod, int pressed) { ... }
 *
 *   __attribute__((export_name("on_char")))
 *   void on_char(int codepoint) { ... }
 * @endcode
 *
 * ## Mouse Events
 * @code
 *   __attribute__((export_name("on_mouse_button")))
 *   void on_mouse_button(int button, int pressed, float x, float y) { ... }
 *
 *   __attribute__((export_name("on_mouse_motion")))
 *   void on_mouse_motion(float x, float y, float dx, float dy, int buttons) { ... }
 *
 *   __attribute__((export_name("on_mouse_wheel")))
 *   void on_mouse_wheel(float dx, float dy) { ... }
 * @endcode
 *
 * ## Touch Events
 * @code
 *   __attribute__((export_name("on_touch")))
 *   void on_touch(int finger_id, int type, float x, float y, float pressure) { ... }
 * @endcode
 * Where `type` is: 0 = down, 1 = up, 2 = motion.
 *
 * ## Joystick Events
 * @code
 *   __attribute__((export_name("on_joystick_added")))
 *   void on_joystick_added(int instance_id) { ... }
 *
 *   __attribute__((export_name("on_joystick_removed")))
 *   void on_joystick_removed(int instance_id) { ... }
 *
 *   __attribute__((export_name("on_joystick_axis")))
 *   void on_joystick_axis(int instance_id, int axis, int value) { ... }
 *
 *   __attribute__((export_name("on_joystick_button")))
 *   void on_joystick_button(int instance_id, int button, int pressed) { ... }
 *
 *   __attribute__((export_name("on_joystick_hat")))
 *   void on_joystick_hat(int instance_id, int hat, int value) { ... }
 * @endcode
 *
 * ## Gamepad Events
 * @code
 *   __attribute__((export_name("on_gamepad_added")))
 *   void on_gamepad_added(int instance_id) { ... }
 *
 *   __attribute__((export_name("on_gamepad_removed")))
 *   void on_gamepad_removed(int instance_id) { ... }
 * @endcode
 *
 * ## Window Events
 * @code
 *   __attribute__((export_name("on_resize")))
 *   void on_resize(int width, int height) { ... }
 *
 *   __attribute__((export_name("on_focus")))
 *   void on_focus(int gained) { ... }
 * @endcode
 *
 * @note Clipboard shortcuts (Ctrl/Cmd+C/V/X/A) are **NOT** intercepted
 * by the host. All keys arrive as `on_key` events. The dashboard app
 * detects shortcuts and calls clipboard_get/clipboard_set itself.
 * @{ */

/* ================================================================== */
/*  Constants                                                          */
/* ================================================================== */

/* Scancodes (SDL3 SDL_Scancode) */
enum {
  INPUT_SCANCODE_UNKNOWN = 0,
  INPUT_SCANCODE_A = 4,
  INPUT_SCANCODE_B = 5,
  INPUT_SCANCODE_C = 6,
  INPUT_SCANCODE_D = 7,
  INPUT_SCANCODE_E = 8,
  INPUT_SCANCODE_F = 9,
  INPUT_SCANCODE_G = 10,
  INPUT_SCANCODE_H = 11,
  INPUT_SCANCODE_I = 12,
  INPUT_SCANCODE_J = 13,
  INPUT_SCANCODE_K = 14,
  INPUT_SCANCODE_L = 15,
  INPUT_SCANCODE_M = 16,
  INPUT_SCANCODE_N = 17,
  INPUT_SCANCODE_O = 18,
  INPUT_SCANCODE_P = 19,
  INPUT_SCANCODE_Q = 20,
  INPUT_SCANCODE_R = 21,
  INPUT_SCANCODE_S = 22,
  INPUT_SCANCODE_T = 23,
  INPUT_SCANCODE_U = 24,
  INPUT_SCANCODE_V = 25,
  INPUT_SCANCODE_W = 26,
  INPUT_SCANCODE_X = 27,
  INPUT_SCANCODE_Y = 28,
  INPUT_SCANCODE_Z = 29,
  INPUT_SCANCODE_1 = 30,
  INPUT_SCANCODE_2 = 31,
  INPUT_SCANCODE_3 = 32,
  INPUT_SCANCODE_4 = 33,
  INPUT_SCANCODE_5 = 34,
  INPUT_SCANCODE_6 = 35,
  INPUT_SCANCODE_7 = 36,
  INPUT_SCANCODE_8 = 37,
  INPUT_SCANCODE_9 = 38,
  INPUT_SCANCODE_0 = 39,
  INPUT_SCANCODE_RETURN = 40,
  INPUT_SCANCODE_ESCAPE = 41,
  INPUT_SCANCODE_BACKSPACE = 42,
  INPUT_SCANCODE_TAB = 43,
  INPUT_SCANCODE_SPACE = 44,
  INPUT_SCANCODE_MINUS = 45,
  INPUT_SCANCODE_EQUALS = 46,
  INPUT_SCANCODE_LEFTBRACKET = 47,
  INPUT_SCANCODE_RIGHTBRACKET = 48,
  INPUT_SCANCODE_BACKSLASH = 49,
  INPUT_SCANCODE_SEMICOLON = 51,
  INPUT_SCANCODE_APOSTROPHE = 52,
  INPUT_SCANCODE_GRAVE = 53,
  INPUT_SCANCODE_COMMA = 54,
  INPUT_SCANCODE_PERIOD = 55,
  INPUT_SCANCODE_SLASH = 56,
  INPUT_SCANCODE_CAPSLOCK = 57,
  INPUT_SCANCODE_F1 = 58,
  INPUT_SCANCODE_F2 = 59,
  INPUT_SCANCODE_F3 = 60,
  INPUT_SCANCODE_F4 = 61,
  INPUT_SCANCODE_F5 = 62,
  INPUT_SCANCODE_F6 = 63,
  INPUT_SCANCODE_F7 = 64,
  INPUT_SCANCODE_F8 = 65,
  INPUT_SCANCODE_F9 = 66,
  INPUT_SCANCODE_F10 = 67,
  INPUT_SCANCODE_F11 = 68,
  INPUT_SCANCODE_F12 = 69,
  INPUT_SCANCODE_PRINTSCREEN = 70,
  INPUT_SCANCODE_SCROLLLOCK = 71,
  INPUT_SCANCODE_PAUSE = 72,
  INPUT_SCANCODE_INSERT = 73,
  INPUT_SCANCODE_HOME = 74,
  INPUT_SCANCODE_PAGEUP = 75,
  INPUT_SCANCODE_DELETE = 76,
  INPUT_SCANCODE_END = 77,
  INPUT_SCANCODE_PAGEDOWN = 78,
  INPUT_SCANCODE_RIGHT = 79,
  INPUT_SCANCODE_LEFT = 80,
  INPUT_SCANCODE_DOWN = 81,
  INPUT_SCANCODE_UP = 82,
  INPUT_SCANCODE_LCTRL = 224,
  INPUT_SCANCODE_LSHIFT = 225,
  INPUT_SCANCODE_LALT = 226,
  INPUT_SCANCODE_LGUI = 227,
  INPUT_SCANCODE_RCTRL = 228,
  INPUT_SCANCODE_RSHIFT = 229,
  INPUT_SCANCODE_RALT = 230,
  INPUT_SCANCODE_RGUI = 231,
};

/* Keycodes (SDL3 SDLK_*) */
enum {
  INPUT_KEYCODE_UNKNOWN = 0,
  INPUT_KEYCODE_RETURN = 0xdu,
  INPUT_KEYCODE_ESCAPE = 0x1bu,
  INPUT_KEYCODE_BACKSPACE = 0x08u,
  INPUT_KEYCODE_TAB = 0x09u,
  INPUT_KEYCODE_SPACE = 0x20u,
  INPUT_KEYCODE_DELETE = 0x7fu,
  INPUT_KEYCODE_A = 0x61u,
  INPUT_KEYCODE_B = 0x62u,
  INPUT_KEYCODE_C = 0x63u,
  INPUT_KEYCODE_D = 0x64u,
  INPUT_KEYCODE_E = 0x65u,
  INPUT_KEYCODE_F = 0x66u,
  INPUT_KEYCODE_G = 0x67u,
  INPUT_KEYCODE_H = 0x68u,
  INPUT_KEYCODE_I = 0x69u,
  INPUT_KEYCODE_J = 0x6au,
  INPUT_KEYCODE_K = 0x6bu,
  INPUT_KEYCODE_L = 0x6cu,
  INPUT_KEYCODE_M = 0x6du,
  INPUT_KEYCODE_N = 0x6eu,
  INPUT_KEYCODE_O = 0x6fu,
  INPUT_KEYCODE_P = 0x70u,
  INPUT_KEYCODE_Q = 0x71u,
  INPUT_KEYCODE_R = 0x72u,
  INPUT_KEYCODE_S = 0x73u,
  INPUT_KEYCODE_T = 0x74u,
  INPUT_KEYCODE_U = 0x75u,
  INPUT_KEYCODE_V = 0x76u,
  INPUT_KEYCODE_W = 0x77u,
  INPUT_KEYCODE_X = 0x78u,
  INPUT_KEYCODE_Y = 0x79u,
  INPUT_KEYCODE_Z = 0x7au,
  INPUT_KEYCODE_0 = 0x30u,
  INPUT_KEYCODE_1 = 0x31u,
  INPUT_KEYCODE_2 = 0x32u,
  INPUT_KEYCODE_3 = 0x33u,
  INPUT_KEYCODE_4 = 0x34u,
  INPUT_KEYCODE_5 = 0x35u,
  INPUT_KEYCODE_6 = 0x36u,
  INPUT_KEYCODE_7 = 0x37u,
  INPUT_KEYCODE_8 = 0x38u,
  INPUT_KEYCODE_9 = 0x39u,
  INPUT_KEYCODE_RIGHT = 0x4000004fu,
  INPUT_KEYCODE_LEFT = 0x40000050u,
  INPUT_KEYCODE_DOWN = 0x40000051u,
  INPUT_KEYCODE_UP = 0x40000052u,
  INPUT_KEYCODE_HOME = 0x4000004au,
  INPUT_KEYCODE_END = 0x4000004du,
  INPUT_KEYCODE_PAGEUP = 0x4000004bu,
  INPUT_KEYCODE_PAGEDOWN = 0x4000004eu,
  INPUT_KEYCODE_INSERT = 0x40000049u,
  INPUT_KEYCODE_LCTRL = 0x400000e0u,
  INPUT_KEYCODE_LSHIFT = 0x400000e1u,
  INPUT_KEYCODE_LALT = 0x400000e2u,
  INPUT_KEYCODE_LGUI = 0x400000e3u,
  INPUT_KEYCODE_RCTRL = 0x400000e4u,
  INPUT_KEYCODE_RSHIFT = 0x400000e5u,
  INPUT_KEYCODE_RALT = 0x400000e6u,
  INPUT_KEYCODE_RGUI = 0x400000e7u,
};

/* Modifier flags (SDL3 SDL_Keymod) */
enum {
  INPUT_KMOD_NONE = 0,
  INPUT_KMOD_LSHIFT = 0x0001,
  INPUT_KMOD_RSHIFT = 0x0002,
  INPUT_KMOD_LCTRL = 0x0040,
  INPUT_KMOD_RCTRL = 0x0080,
  INPUT_KMOD_LALT = 0x0100,
  INPUT_KMOD_RALT = 0x0200,
  INPUT_KMOD_LGUI = 0x0400,
  INPUT_KMOD_RGUI = 0x0800,
  INPUT_KMOD_NUM = 0x1000,
  INPUT_KMOD_CAPS = 0x2000,
  INPUT_KMOD_MODE = 0x4000,
  INPUT_KMOD_SCROLL = 0x8000,
  INPUT_KMOD_CTRL = 0x00C0,
  INPUT_KMOD_SHIFT = 0x0003,
  INPUT_KMOD_ALT = 0x0300,
  INPUT_KMOD_GUI = 0x0C00,
};

/* Mouse buttons */
enum {
  INPUT_MOUSE_BUTTON_LEFT = 1,
  INPUT_MOUSE_BUTTON_MIDDLE = 2,
  INPUT_MOUSE_BUTTON_RIGHT = 3,
  INPUT_MOUSE_BUTTON_X1 = 4,
  INPUT_MOUSE_BUTTON_X2 = 5,
};
#define INPUT_MOUSE_BUTTON_LMASK (1u << 0)
#define INPUT_MOUSE_BUTTON_MMASK (1u << 1)
#define INPUT_MOUSE_BUTTON_RMASK (1u << 2)
#define INPUT_MOUSE_BUTTON_X1MASK (1u << 3)
#define INPUT_MOUSE_BUTTON_X2MASK (1u << 4)

/* System cursors */
enum {
  INPUT_CURSOR_DEFAULT = 0,
  INPUT_CURSOR_TEXT = 1,
  INPUT_CURSOR_WAIT = 2,
  INPUT_CURSOR_CROSSHAIR = 3,
  INPUT_CURSOR_PROGRESS = 4,
  INPUT_CURSOR_NWSE_RESIZE = 5,
  INPUT_CURSOR_NESW_RESIZE = 6,
  INPUT_CURSOR_EW_RESIZE = 7,
  INPUT_CURSOR_NS_RESIZE = 8,
  INPUT_CURSOR_MOVE = 9,
  INPUT_CURSOR_NOT_ALLOWED = 10,
  INPUT_CURSOR_POINTER = 11,
  INPUT_CURSOR_NW_RESIZE = 12,
  INPUT_CURSOR_N_RESIZE = 13,
  INPUT_CURSOR_NE_RESIZE = 14,
  INPUT_CURSOR_E_RESIZE = 15,
  INPUT_CURSOR_SE_RESIZE = 16,
  INPUT_CURSOR_S_RESIZE = 17,
  INPUT_CURSOR_SW_RESIZE = 18,
  INPUT_CURSOR_W_RESIZE = 19,
};

/* Joystick hats */
#define INPUT_HAT_CENTERED 0x00u
#define INPUT_HAT_UP 0x01u
#define INPUT_HAT_RIGHT 0x02u
#define INPUT_HAT_DOWN 0x04u
#define INPUT_HAT_LEFT 0x08u
#define INPUT_HAT_RIGHTUP (INPUT_HAT_RIGHT | INPUT_HAT_UP)
#define INPUT_HAT_RIGHTDOWN (INPUT_HAT_RIGHT | INPUT_HAT_DOWN)
#define INPUT_HAT_LEFTUP (INPUT_HAT_LEFT | INPUT_HAT_UP)
#define INPUT_HAT_LEFTDOWN (INPUT_HAT_LEFT | INPUT_HAT_DOWN)

#define INPUT_JOYSTICK_AXIS_MAX 32767
#define INPUT_JOYSTICK_AXIS_MIN (-32768)

/* Touch */
enum { INPUT_TOUCH_DOWN = 0, INPUT_TOUCH_UP = 1, INPUT_TOUCH_MOTION = 2 };
enum {
  INPUT_TOUCH_DEVICE_INVALID = -1,
  INPUT_TOUCH_DEVICE_DIRECT = 0,
  INPUT_TOUCH_DEVICE_INDIRECT_ABSOLUTE = 1,
  INPUT_TOUCH_DEVICE_INDIRECT_RELATIVE = 2
};

/* Gamepad buttons */
enum {
  INPUT_GAMEPAD_BUTTON_SOUTH = 0,
  INPUT_GAMEPAD_BUTTON_EAST = 1,
  INPUT_GAMEPAD_BUTTON_WEST = 2,
  INPUT_GAMEPAD_BUTTON_NORTH = 3,
  INPUT_GAMEPAD_BUTTON_BACK = 4,
  INPUT_GAMEPAD_BUTTON_GUIDE = 5,
  INPUT_GAMEPAD_BUTTON_START = 6,
  INPUT_GAMEPAD_BUTTON_LEFT_STICK = 7,
  INPUT_GAMEPAD_BUTTON_RIGHT_STICK = 8,
  INPUT_GAMEPAD_BUTTON_LEFT_SHOULDER = 9,
  INPUT_GAMEPAD_BUTTON_RIGHT_SHOULDER = 10,
  INPUT_GAMEPAD_BUTTON_DPAD_UP = 11,
  INPUT_GAMEPAD_BUTTON_DPAD_DOWN = 12,
  INPUT_GAMEPAD_BUTTON_DPAD_LEFT = 13,
  INPUT_GAMEPAD_BUTTON_DPAD_RIGHT = 14,
  INPUT_GAMEPAD_BUTTON_MISC1 = 15,
  INPUT_GAMEPAD_BUTTON_TOUCHPAD = 20,
  INPUT_GAMEPAD_BUTTON_COUNT = 27,
};

/* Gamepad axes */
enum {
  INPUT_GAMEPAD_AXIS_LEFTX = 0,
  INPUT_GAMEPAD_AXIS_LEFTY = 1,
  INPUT_GAMEPAD_AXIS_RIGHTX = 2,
  INPUT_GAMEPAD_AXIS_RIGHTY = 3,
  INPUT_GAMEPAD_AXIS_LEFT_TRIGGER = 4,
  INPUT_GAMEPAD_AXIS_RIGHT_TRIGGER = 5,
  INPUT_GAMEPAD_AXIS_COUNT = 6,
};

/* Gamepad types */
enum {
  INPUT_GAMEPAD_TYPE_UNKNOWN = 0,
  INPUT_GAMEPAD_TYPE_STANDARD = 1,
  INPUT_GAMEPAD_TYPE_XBOX360 = 2,
  INPUT_GAMEPAD_TYPE_XBOXONE = 3,
  INPUT_GAMEPAD_TYPE_PS3 = 4,
  INPUT_GAMEPAD_TYPE_PS4 = 5,
  INPUT_GAMEPAD_TYPE_PS5 = 6,
  INPUT_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO = 7,
  INPUT_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT = 8,
  INPUT_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT = 9,
  INPUT_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR = 10,
};

/* Helpers */
static inline float input_axis_normalise(int raw) {
  return raw < 0 ? (float)raw / 32768.0f : (float)raw / 32767.0f;
}
static inline int input_mod_active(int st, int flag) {
  return (st & flag) != 0;
}

#endif /* WASM_INPUT_H */
