/*
    Yumi SDK — SDL3 Subsystem WASM Imports
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
 * @file wasm_sdl.h
 * @brief WebAssembly guest imports for host-side SDL3 subsystem bindings.
 *
 * @details
 * This header declares the host-imported functions that mirror SDL3's
 * Keyboard, Mouse, Joystick, Gamepad, Display, and Audio subsystems.
 * All functions map 1:1 to implementations in `sdl_bindings.c` on the host.
 *
 * ## Adaptations from Native SDL3
 * Since WASM guest modules cannot hold native pointers, all SDL object
 * references are replaced by opaque `uint32_t` handles managed by the host:
 *   - `sdl_joystick_t` — opaque joystick handle
 *   - `sdl_gamepad_t` — opaque gamepad handle
 *   - `sdl_audio_stream_t` — opaque audio stream handle
 *
 * All pointer-returning SDL functions are adapted to write into guest-
 * provided buffers (pointer + max length). For example, `sdl_get_key_name`
 * writes into a guest-provided char buffer instead of returning a const char*.
 *
 * ## Subsystems Covered
 *   - **Display**: Enumeration, bounds, orientation, content scale
 *   - **Keyboard**: State polling, scancode translation, text input/IME
 *   - **Mouse**: Position polling, warping, relative mode, capture
 *   - **Joystick**: Enumeration, axis/button/hat polling, rumble
 *   - **Gamepad**: Enumeration, mapped axis/button polling, trigger rumble
 *   - **Audio**: Device enumeration, stream management, WAV loading, mixing
 *
 * @see wasm_input.h for the input event export callbacks.
 * @see https://wiki.libsdl.org/SDL3/CategoryAPI
 */

#ifndef YUMI_SDL_H
#define YUMI_SDL_H

#include <stdint.h>

/** @cond INTERNAL */
#define IMPORT __attribute__((import_module("env")))
/** @endcond */

/* ================================================================== */
/*  Handle types                                                       */
/* ================================================================== */

/**
 * @brief Opaque handle to an open SDL joystick.
 * @details Obtained via sdl_open_joystick() and released via sdl_close_joystick().
 */
typedef uint32_t sdl_joystick_t;

/**
 * @brief Opaque handle to an open SDL gamepad.
 * @details Obtained via sdl_open_gamepad() and released via sdl_close_gamepad().
 */
typedef uint32_t sdl_gamepad_t;

/**
 * @brief Opaque handle to an SDL audio stream.
 * @details Created via sdl_create_audio_stream() or sdl_open_audio_device_stream().
 */
typedef uint32_t sdl_audio_stream_t;

/* SDL_AudioDeviceID / SDL_JoystickID / SDL_KeyboardID are plain u32
   so we just use uint32_t directly. */

/* ================================================================== */
/*  Keyboard constants                                                 */
/* ================================================================== */

/* --- Key modifier flags (SDL_Keymod) --- */
#define SDL_KMOD_NONE   0x0000u
#define SDL_KMOD_LSHIFT 0x0001u
#define SDL_KMOD_RSHIFT 0x0002u
#define SDL_KMOD_LCTRL  0x0040u
#define SDL_KMOD_RCTRL  0x0080u
#define SDL_KMOD_LALT   0x0100u
#define SDL_KMOD_RALT   0x0200u
#define SDL_KMOD_LGUI   0x0400u
#define SDL_KMOD_RGUI   0x0800u
#define SDL_KMOD_NUM    0x1000u
#define SDL_KMOD_CAPS   0x2000u
#define SDL_KMOD_MODE   0x4000u
#define SDL_KMOD_SCROLL 0x8000u
#define SDL_KMOD_CTRL   (SDL_KMOD_LCTRL  | SDL_KMOD_RCTRL)
#define SDL_KMOD_SHIFT  (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)
#define SDL_KMOD_ALT    (SDL_KMOD_LALT   | SDL_KMOD_RALT)
#define SDL_KMOD_GUI    (SDL_KMOD_LGUI   | SDL_KMOD_RGUI)

/* ================================================================== */
/*  Mouse constants                                                    */
/* ================================================================== */

#define SDL_BUTTON_LEFT     1
#define SDL_BUTTON_MIDDLE   2
#define SDL_BUTTON_RIGHT    3
#define SDL_BUTTON_X1       4
#define SDL_BUTTON_X2       5
#define SDL_BUTTON_MASK(X)  (1u << ((X)-1))
#define SDL_BUTTON_LMASK    SDL_BUTTON_MASK(SDL_BUTTON_LEFT)
#define SDL_BUTTON_MMASK    SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)
#define SDL_BUTTON_RMASK    SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)
#define SDL_BUTTON_X1MASK   SDL_BUTTON_MASK(SDL_BUTTON_X1)
#define SDL_BUTTON_X2MASK   SDL_BUTTON_MASK(SDL_BUTTON_X2)

/* ================================================================== */
/*  Joystick constants                                                 */
/* ================================================================== */

#define SDL_JOYSTICK_AXIS_MAX    32767
#define SDL_JOYSTICK_AXIS_MIN   -32768

#define SDL_HAT_CENTERED    0x00u
#define SDL_HAT_UP          0x01u
#define SDL_HAT_RIGHT       0x02u
#define SDL_HAT_DOWN        0x04u
#define SDL_HAT_LEFT        0x08u
#define SDL_HAT_RIGHTUP     (SDL_HAT_RIGHT | SDL_HAT_UP)
#define SDL_HAT_RIGHTDOWN   (SDL_HAT_RIGHT | SDL_HAT_DOWN)
#define SDL_HAT_LEFTUP      (SDL_HAT_LEFT  | SDL_HAT_UP)
#define SDL_HAT_LEFTDOWN    (SDL_HAT_LEFT  | SDL_HAT_DOWN)

/* ================================================================== */
/*  Gamepad enums (stable ABI values matching SDL3)                    */
/* ================================================================== */

/* SDL_GamepadType */
#define SDL_GAMEPAD_TYPE_UNKNOWN                     0
#define SDL_GAMEPAD_TYPE_STANDARD                    1
#define SDL_GAMEPAD_TYPE_XBOX360                     2
#define SDL_GAMEPAD_TYPE_XBOXONE                     3
#define SDL_GAMEPAD_TYPE_PS3                          4
#define SDL_GAMEPAD_TYPE_PS4                          5
#define SDL_GAMEPAD_TYPE_PS5                          6
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO          7
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT  8
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT 9
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR  10
#define SDL_GAMEPAD_TYPE_GAMECUBE                     11

/* SDL_GamepadButton */
#define SDL_GAMEPAD_BUTTON_INVALID          -1
#define SDL_GAMEPAD_BUTTON_SOUTH             0
#define SDL_GAMEPAD_BUTTON_EAST              1
#define SDL_GAMEPAD_BUTTON_WEST              2
#define SDL_GAMEPAD_BUTTON_NORTH             3
#define SDL_GAMEPAD_BUTTON_BACK              4
#define SDL_GAMEPAD_BUTTON_GUIDE             5
#define SDL_GAMEPAD_BUTTON_START             6
#define SDL_GAMEPAD_BUTTON_LEFT_STICK        7
#define SDL_GAMEPAD_BUTTON_RIGHT_STICK       8
#define SDL_GAMEPAD_BUTTON_LEFT_SHOULDER     9
#define SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER   10
#define SDL_GAMEPAD_BUTTON_DPAD_UP          11
#define SDL_GAMEPAD_BUTTON_DPAD_DOWN        12
#define SDL_GAMEPAD_BUTTON_DPAD_LEFT        13
#define SDL_GAMEPAD_BUTTON_DPAD_RIGHT       14
#define SDL_GAMEPAD_BUTTON_MISC1            15
#define SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1    16
#define SDL_GAMEPAD_BUTTON_LEFT_PADDLE1     17
#define SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2    18
#define SDL_GAMEPAD_BUTTON_LEFT_PADDLE2     19
#define SDL_GAMEPAD_BUTTON_TOUCHPAD         20
#define SDL_GAMEPAD_BUTTON_COUNT            27

/* SDL_GamepadAxis */
#define SDL_GAMEPAD_AXIS_INVALID        -1
#define SDL_GAMEPAD_AXIS_LEFTX           0
#define SDL_GAMEPAD_AXIS_LEFTY           1
#define SDL_GAMEPAD_AXIS_RIGHTX          2
#define SDL_GAMEPAD_AXIS_RIGHTY          3
#define SDL_GAMEPAD_AXIS_LEFT_TRIGGER    4
#define SDL_GAMEPAD_AXIS_RIGHT_TRIGGER   5
#define SDL_GAMEPAD_AXIS_COUNT           6

/* SDL_GamepadButtonLabel */
#define SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN     0
#define SDL_GAMEPAD_BUTTON_LABEL_A           1
#define SDL_GAMEPAD_BUTTON_LABEL_B           2
#define SDL_GAMEPAD_BUTTON_LABEL_X           3
#define SDL_GAMEPAD_BUTTON_LABEL_Y           4
#define SDL_GAMEPAD_BUTTON_LABEL_CROSS       5
#define SDL_GAMEPAD_BUTTON_LABEL_CIRCLE      6
#define SDL_GAMEPAD_BUTTON_LABEL_SQUARE      7
#define SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE    8

/* ================================================================== */
/*  Audio constants                                                    */
/* ================================================================== */

/* SDL_AudioFormat */
#define SDL_AUDIO_UNKNOWN   0x0000u
#define SDL_AUDIO_U8        0x0008u
#define SDL_AUDIO_S8        0x8008u
#define SDL_AUDIO_S16LE     0x8010u
#define SDL_AUDIO_S16BE     0x9010u
#define SDL_AUDIO_S32LE     0x8020u
#define SDL_AUDIO_S32BE     0x9020u
#define SDL_AUDIO_F32LE     0x8120u
#define SDL_AUDIO_F32BE     0x9120u

/* Default playback / recording device IDs */
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK   0xFFFFFFFFu
#define SDL_AUDIO_DEVICE_DEFAULT_RECORDING  0xFFFFFFFEu

/* ================================================================== */
/*  Display imports                                                    */
/* ================================================================== */

/* SDL_DisplayOrientation values */
#define SDL_ORIENTATION_UNKNOWN           0
#define SDL_ORIENTATION_LANDSCAPE         1
#define SDL_ORIENTATION_LANDSCAPE_FLIPPED 2
#define SDL_ORIENTATION_PORTRAIT          3
#define SDL_ORIENTATION_PORTRAIT_FLIPPED  4

/* Writes up to max_count SDL_DisplayID (uint32) into out_ids.
 * Returns actual count of displays. */
IMPORT __attribute__((import_name("sdl_get_displays")))
int sdl_get_displays(uint32_t *out_ids, int max_count);

/* Returns the display ID that the window is currently on. */
IMPORT __attribute__((import_name("sdl_get_display_for_window")))
int sdl_get_display_for_window(void);

/* Writes a NUL-terminated name string into out. Returns bytes written. */
IMPORT __attribute__((import_name("sdl_get_display_name")))
int sdl_get_display_name(uint32_t display_id, char *out, int max_len);

/* Writes display bounds (x, y, w, h as 4 x int32) into out.
 * Returns 1 on success. */
IMPORT __attribute__((import_name("sdl_get_display_bounds")))
int sdl_get_display_bounds(uint32_t display_id, int32_t *out);

/* Writes usable bounds excluding taskbars/docks (x, y, w, h as 4 x int32).
 * Returns 1 on success. */
IMPORT __attribute__((import_name("sdl_get_display_usable_bounds")))
int sdl_get_display_usable_bounds(uint32_t display_id, int32_t *out);

/* Returns the content scale factor (1.0 = 96dpi, 2.0 = HiDPI). */
IMPORT __attribute__((import_name("sdl_get_display_content_scale")))
float sdl_get_display_content_scale(uint32_t display_id);

/* Returns SDL_DisplayOrientation enum value. */
IMPORT __attribute__((import_name("sdl_get_display_orientation")))
int sdl_get_display_orientation(uint32_t display_id);

/* Returns the natural (unrotated) orientation. */
IMPORT __attribute__((import_name("sdl_get_display_natural_orientation")))
int sdl_get_display_natural_orientation(uint32_t display_id);

/* Writes current display mode info into out pointers.
 * out_w, out_h: pixel dimensions. out_refresh: Hz as float.
 * Any pointer may be 0/NULL to skip. Returns 1 on success. */
IMPORT __attribute__((import_name("sdl_get_display_mode")))
int sdl_get_display_mode(uint32_t display_id,
                         int32_t *out_w, int32_t *out_h,
                         float *out_refresh);

/* Returns pixel density ratio for the window (pixels / points). */
IMPORT __attribute__((import_name("sdl_get_window_pixel_density")))
float sdl_get_window_pixel_density(void);

/* Returns content display scale relative to the window. */
IMPORT __attribute__((import_name("sdl_get_window_display_scale")))
float sdl_get_window_display_scale(void);

/* Guest-side audio spec struct (12 bytes, matches host layout) */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t format;     /* SDL_AudioFormat              */
    int32_t  channels;   /* number of channels           */
    int32_t  freq;       /* sample rate in Hz            */
} sdl_audio_spec_t;

/* ================================================================== */
/*  Keyboard imports                                                   */
/* ================================================================== */

IMPORT __attribute__((import_name("sdl_has_keyboard")))
int sdl_has_keyboard(void);

/* Copies the scancode key-state array (one byte per scancode, 0/1)
 * into `out`. Returns the number of scancodes written. */
IMPORT __attribute__((import_name("sdl_get_keyboard_state")))
int sdl_get_keyboard_state(uint8_t *out, int max_keys);

IMPORT __attribute__((import_name("sdl_get_mod_state")))
int sdl_get_mod_state(void);

IMPORT __attribute__((import_name("sdl_set_mod_state")))
void sdl_set_mod_state(int modstate);

/* Returns SDL_Keycode for the given scancode + modifier state.
 * key_event: 1 to get event-style keycode, 0 for raw translation. */
IMPORT __attribute__((import_name("sdl_get_key_from_scancode")))
int sdl_get_key_from_scancode(int scancode, int modstate, int key_event);

/* Returns SDL_Scancode.  Writes the modifier state to *mod_out if non-NULL. */
IMPORT __attribute__((import_name("sdl_get_scancode_from_key")))
int sdl_get_scancode_from_key(int keycode, uint16_t *mod_out);

/* Writes a NUL-terminated name string into out. Returns bytes written. */
IMPORT __attribute__((import_name("sdl_get_key_name")))
int sdl_get_key_name(int keycode, char *out, int max_len);

IMPORT __attribute__((import_name("sdl_get_scancode_name")))
int sdl_get_scancode_name(int scancode, char *out, int max_len);

IMPORT __attribute__((import_name("sdl_start_text_input")))
int sdl_start_text_input(void);

IMPORT __attribute__((import_name("sdl_stop_text_input")))
int sdl_stop_text_input(void);

IMPORT __attribute__((import_name("sdl_text_input_active")))
int sdl_text_input_active(void);

IMPORT __attribute__((import_name("sdl_reset_keyboard")))
void sdl_reset_keyboard(void);

/* ================================================================== */
/*  Mouse imports                                                      */
/* ================================================================== */

IMPORT __attribute__((import_name("sdl_has_mouse")))
int sdl_has_mouse(void);

/* Returns button-flag bitmask. Writes float x,y into pointers (may be NULL/0). */
IMPORT __attribute__((import_name("sdl_get_mouse_state")))
int sdl_get_mouse_state(float *x, float *y);

IMPORT __attribute__((import_name("sdl_get_global_mouse_state")))
int sdl_get_global_mouse_state(float *x, float *y);

IMPORT __attribute__((import_name("sdl_get_relative_mouse_state")))
int sdl_get_relative_mouse_state(float *x, float *y);

IMPORT __attribute__((import_name("sdl_warp_mouse_in_window")))
void sdl_warp_mouse_in_window(float x, float y);

IMPORT __attribute__((import_name("sdl_warp_mouse_global")))
int sdl_warp_mouse_global(float x, float y);

IMPORT __attribute__((import_name("sdl_set_relative_mouse_mode")))
int sdl_set_relative_mouse_mode(int enabled);

IMPORT __attribute__((import_name("sdl_get_relative_mouse_mode")))
int sdl_get_relative_mouse_mode(void);

IMPORT __attribute__((import_name("sdl_show_cursor")))
int sdl_show_cursor(void);

IMPORT __attribute__((import_name("sdl_hide_cursor")))
int sdl_hide_cursor(void);

IMPORT __attribute__((import_name("sdl_cursor_visible")))
int sdl_cursor_visible(void);

IMPORT __attribute__((import_name("sdl_capture_mouse")))
int sdl_capture_mouse(int enabled);

/* ================================================================== */
/*  Joystick imports                                                   */
/* ================================================================== */

IMPORT __attribute__((import_name("sdl_has_joystick")))
int sdl_has_joystick(void);

/* Writes up to max_count SDL_JoystickID (uint32) into out_ids.
 * Returns actual count of joysticks. */
IMPORT __attribute__((import_name("sdl_get_joysticks")))
int sdl_get_joysticks(uint32_t *out_ids, int max_count);

IMPORT __attribute__((import_name("sdl_get_joystick_name_for_id")))
int sdl_get_joystick_name_for_id(uint32_t instance_id, char *out, int max_len);

IMPORT __attribute__((import_name("sdl_open_joystick")))
sdl_joystick_t sdl_open_joystick(uint32_t instance_id);

IMPORT __attribute__((import_name("sdl_close_joystick")))
void sdl_close_joystick(sdl_joystick_t js);

IMPORT __attribute__((import_name("sdl_joystick_connected")))
int sdl_joystick_connected(sdl_joystick_t js);

IMPORT __attribute__((import_name("sdl_get_num_joystick_axes")))
int sdl_get_num_joystick_axes(sdl_joystick_t js);

IMPORT __attribute__((import_name("sdl_get_num_joystick_buttons")))
int sdl_get_num_joystick_buttons(sdl_joystick_t js);

IMPORT __attribute__((import_name("sdl_get_num_joystick_hats")))
int sdl_get_num_joystick_hats(sdl_joystick_t js);

/* Returns Sint16 axis value (-32768..32767). */
IMPORT __attribute__((import_name("sdl_get_joystick_axis")))
int sdl_get_joystick_axis(sdl_joystick_t js, int axis);

/* Returns 1 if pressed, 0 otherwise. */
IMPORT __attribute__((import_name("sdl_get_joystick_button")))
int sdl_get_joystick_button(sdl_joystick_t js, int button);

/* Returns hat bitmask (SDL_HAT_*). */
IMPORT __attribute__((import_name("sdl_get_joystick_hat")))
int sdl_get_joystick_hat(sdl_joystick_t js, int hat);

IMPORT __attribute__((import_name("sdl_rumble_joystick")))
int sdl_rumble_joystick(sdl_joystick_t js,
                        int low_frequency_rumble,
                        int high_frequency_rumble,
                        int duration_ms);

IMPORT __attribute__((import_name("sdl_set_joystick_led")))
int sdl_set_joystick_led(sdl_joystick_t js, int r, int g, int b);

IMPORT __attribute__((import_name("sdl_get_joystick_name")))
int sdl_get_joystick_name(sdl_joystick_t js, char *out, int max_len);

IMPORT __attribute__((import_name("sdl_get_joystick_id")))
int sdl_get_joystick_id(sdl_joystick_t js);

IMPORT __attribute__((import_name("sdl_update_joysticks")))
void sdl_update_joysticks(void);

/* ================================================================== */
/*  Gamepad imports                                                    */
/* ================================================================== */

IMPORT __attribute__((import_name("sdl_has_gamepad")))
int sdl_has_gamepad(void);

/* Writes up to max_count SDL_JoystickID (uint32) into out_ids.
 * Returns actual count of gamepads. */
IMPORT __attribute__((import_name("sdl_get_gamepads")))
int sdl_get_gamepads(uint32_t *out_ids, int max_count);

IMPORT __attribute__((import_name("sdl_is_gamepad")))
int sdl_is_gamepad(uint32_t instance_id);

IMPORT __attribute__((import_name("sdl_open_gamepad")))
sdl_gamepad_t sdl_open_gamepad(uint32_t instance_id);

IMPORT __attribute__((import_name("sdl_close_gamepad")))
void sdl_close_gamepad(sdl_gamepad_t gp);

IMPORT __attribute__((import_name("sdl_gamepad_connected")))
int sdl_gamepad_connected(sdl_gamepad_t gp);

IMPORT __attribute__((import_name("sdl_get_gamepad_name")))
int sdl_get_gamepad_name(sdl_gamepad_t gp, char *out, int max_len);

/* Returns SDL_GamepadType enum value. */
IMPORT __attribute__((import_name("sdl_get_gamepad_type")))
int sdl_get_gamepad_type(sdl_gamepad_t gp);

/* Returns Sint16 axis value. Triggers: 0..32767. Sticks: -32768..32767. */
IMPORT __attribute__((import_name("sdl_get_gamepad_axis")))
int sdl_get_gamepad_axis(sdl_gamepad_t gp, int axis);

/* Returns 1 if pressed, 0 otherwise. */
IMPORT __attribute__((import_name("sdl_get_gamepad_button")))
int sdl_get_gamepad_button(sdl_gamepad_t gp, int button);

IMPORT __attribute__((import_name("sdl_rumble_gamepad")))
int sdl_rumble_gamepad(sdl_gamepad_t gp,
                       int low_frequency_rumble,
                       int high_frequency_rumble,
                       int duration_ms);

IMPORT __attribute__((import_name("sdl_rumble_gamepad_triggers")))
int sdl_rumble_gamepad_triggers(sdl_gamepad_t gp,
                                int left_rumble,
                                int right_rumble,
                                int duration_ms);

IMPORT __attribute__((import_name("sdl_set_gamepad_led")))
int sdl_set_gamepad_led(sdl_gamepad_t gp, int r, int g, int b);

IMPORT __attribute__((import_name("sdl_get_gamepad_id")))
int sdl_get_gamepad_id(sdl_gamepad_t gp);

IMPORT __attribute__((import_name("sdl_gamepad_has_axis")))
int sdl_gamepad_has_axis(sdl_gamepad_t gp, int axis);

IMPORT __attribute__((import_name("sdl_gamepad_has_button")))
int sdl_gamepad_has_button(sdl_gamepad_t gp, int button);

IMPORT __attribute__((import_name("sdl_update_gamepads")))
void sdl_update_gamepads(void);

/* Returns SDL_GamepadButtonLabel enum value. */
IMPORT __attribute__((import_name("sdl_get_gamepad_button_label")))
int sdl_get_gamepad_button_label(sdl_gamepad_t gp, int button);

IMPORT __attribute__((import_name("sdl_get_gamepad_player_index")))
int sdl_get_gamepad_player_index(sdl_gamepad_t gp);

IMPORT __attribute__((import_name("sdl_set_gamepad_player_index")))
int sdl_set_gamepad_player_index(sdl_gamepad_t gp, int player_index);

/* ================================================================== */
/*  Audio imports — drivers & devices                                  */
/* ================================================================== */

IMPORT __attribute__((import_name("sdl_get_num_audio_drivers")))
int sdl_get_num_audio_drivers(void);

IMPORT __attribute__((import_name("sdl_get_audio_driver")))
int sdl_get_audio_driver(int index, char *out, int max_len);

IMPORT __attribute__((import_name("sdl_get_current_audio_driver")))
int sdl_get_current_audio_driver(char *out, int max_len);

/* Writes up to max_count SDL_AudioDeviceID (uint32) into out_ids.
 * Returns actual count. */
IMPORT __attribute__((import_name("sdl_get_audio_playback_devices")))
int sdl_get_audio_playback_devices(uint32_t *out_ids, int max_count);

IMPORT __attribute__((import_name("sdl_get_audio_recording_devices")))
int sdl_get_audio_recording_devices(uint32_t *out_ids, int max_count);

IMPORT __attribute__((import_name("sdl_get_audio_device_name")))
int sdl_get_audio_device_name(uint32_t devid, char *out, int max_len);

/* Opens an audio device. Pass format/channels/freq as 0 for defaults.
 * devid: use SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK or _RECORDING for defaults.
 * Returns the opened device ID, or 0 on failure. */
IMPORT __attribute__((import_name("sdl_open_audio_device")))
int sdl_open_audio_device(uint32_t devid, int format, int channels, int freq);

IMPORT __attribute__((import_name("sdl_close_audio_device")))
void sdl_close_audio_device(uint32_t devid);

IMPORT __attribute__((import_name("sdl_pause_audio_device")))
int sdl_pause_audio_device(uint32_t devid);

IMPORT __attribute__((import_name("sdl_resume_audio_device")))
int sdl_resume_audio_device(uint32_t devid);

IMPORT __attribute__((import_name("sdl_audio_device_paused")))
int sdl_audio_device_paused(uint32_t devid);

IMPORT __attribute__((import_name("sdl_set_audio_device_gain")))
int sdl_set_audio_device_gain(uint32_t devid, float gain);

IMPORT __attribute__((import_name("sdl_get_audio_device_gain")))
float sdl_get_audio_device_gain(uint32_t devid);

/* Writes spec (12 bytes) to spec_out, sample_frames (i32) to frames_out.
 * Either pointer may be 0/NULL. Returns 1 on success. */
IMPORT __attribute__((import_name("sdl_get_audio_device_format")))
int sdl_get_audio_device_format(uint32_t devid,
                                 sdl_audio_spec_t *spec_out,
                                 int *frames_out);

/* ================================================================== */
/*  Audio imports — streams                                            */
/* ================================================================== */

/* Creates an audio stream. Returns a handle, or 0 on failure. */
IMPORT __attribute__((import_name("sdl_create_audio_stream")))
sdl_audio_stream_t sdl_create_audio_stream(
    int src_format, int src_channels, int src_freq,
    int dst_format, int dst_channels, int dst_freq);

IMPORT __attribute__((import_name("sdl_destroy_audio_stream")))
void sdl_destroy_audio_stream(sdl_audio_stream_t stream);

IMPORT __attribute__((import_name("sdl_bind_audio_stream")))
int sdl_bind_audio_stream(uint32_t devid, sdl_audio_stream_t stream);

IMPORT __attribute__((import_name("sdl_unbind_audio_stream")))
void sdl_unbind_audio_stream(sdl_audio_stream_t stream);

/* Copies data from guest memory into the stream.
 * Returns 1 on success. */
IMPORT __attribute__((import_name("sdl_put_audio_stream_data")))
int sdl_put_audio_stream_data(sdl_audio_stream_t stream,
                               const void *data, int len);

/* Reads converted data from the stream into guest memory.
 * Returns bytes actually read, or -1 on failure. */
IMPORT __attribute__((import_name("sdl_get_audio_stream_data")))
int sdl_get_audio_stream_data(sdl_audio_stream_t stream,
                               void *buf, int len);

/* Returns bytes of converted data available, or -1 on failure. */
IMPORT __attribute__((import_name("sdl_get_audio_stream_available")))
int sdl_get_audio_stream_available(sdl_audio_stream_t stream);

/* Returns bytes of unconverted data queued, or -1 on failure. */
IMPORT __attribute__((import_name("sdl_get_audio_stream_queued")))
int sdl_get_audio_stream_queued(sdl_audio_stream_t stream);

IMPORT __attribute__((import_name("sdl_flush_audio_stream")))
int sdl_flush_audio_stream(sdl_audio_stream_t stream);

IMPORT __attribute__((import_name("sdl_clear_audio_stream")))
int sdl_clear_audio_stream(sdl_audio_stream_t stream);

/* Change format on either side. Pass 0 for all three fields on a side
 * to leave that side unchanged. */
IMPORT __attribute__((import_name("sdl_set_audio_stream_format")))
int sdl_set_audio_stream_format(sdl_audio_stream_t stream,
    int src_format, int src_channels, int src_freq,
    int dst_format, int dst_channels, int dst_freq);

IMPORT __attribute__((import_name("sdl_set_audio_stream_gain")))
int sdl_set_audio_stream_gain(sdl_audio_stream_t stream, float gain);

IMPORT __attribute__((import_name("sdl_get_audio_stream_gain")))
float sdl_get_audio_stream_gain(sdl_audio_stream_t stream);

/* ratio > 1.0 = faster/higher pitch, < 1.0 = slower/lower pitch. */
IMPORT __attribute__((import_name("sdl_set_audio_stream_frequency_ratio")))
int sdl_set_audio_stream_frequency_ratio(sdl_audio_stream_t stream,
                                          float ratio);

IMPORT __attribute__((import_name("sdl_get_audio_stream_frequency_ratio")))
float sdl_get_audio_stream_frequency_ratio(sdl_audio_stream_t stream);

/* ================================================================== */
/*  Audio imports — simplified device+stream                           */
/* ================================================================== */

/* Opens a device and creates a bound stream in one call.
 * The device starts PAUSED — call sdl_resume_audio_stream_device().
 * Destroying the stream also closes the device.
 * Pass format/channels/freq as 0 to let SDL choose. */
IMPORT __attribute__((import_name("sdl_open_audio_device_stream")))
sdl_audio_stream_t sdl_open_audio_device_stream(
    uint32_t devid, int format, int channels, int freq);

IMPORT __attribute__((import_name("sdl_resume_audio_stream_device")))
int sdl_resume_audio_stream_device(sdl_audio_stream_t stream);

IMPORT __attribute__((import_name("sdl_pause_audio_stream_device")))
int sdl_pause_audio_stream_device(sdl_audio_stream_t stream);

/* ================================================================== */
/*  Audio imports — WAV loading & utilities                            */
/* ================================================================== */

/* Loads a WAV file from the given path.
 * Writes the spec to spec_out (12 bytes), audio data to data_out
 * (up to data_max bytes), and actual byte length to len_out.
 * Returns 1 on success, 0 on failure. */
IMPORT __attribute__((import_name("sdl_load_wav")))
int sdl_load_wav(const char *path, int path_len,
                  sdl_audio_spec_t *spec_out,
                  void *data_out, int data_max,
                  uint32_t *len_out);

/* Mixes src into dst with the given volume (0.0-1.0).
 * Both buffers must be the same format and length. */
IMPORT __attribute__((import_name("sdl_mix_audio")))
int sdl_mix_audio(void *dst, const void *src,
                   int format, int len, float volume);

IMPORT __attribute__((import_name("sdl_get_audio_format_name")))
int sdl_get_audio_format_name(int format, char *out, int max_len);


/* ================================================================== */
/*  Platform identification                                            */
/* ================================================================== */

#define SDL_PLATFORM_UNKNOWN  0
#define SDL_PLATFORM_WINDOWS  1
#define SDL_PLATFORM_MACOS    2
#define SDL_PLATFORM_LINUX    3
#define SDL_PLATFORM_IOS      4
#define SDL_PLATFORM_ANDROID  5

/* Returns one of the SDL_PLATFORM_* constants. */
IMPORT __attribute__((import_name("sdl_get_platform")))
int sdl_get_platform(void);

/* Returns monotonic time in milliseconds since application start. */
IMPORT __attribute__((import_name("sdl_get_ticks_ms")))
float sdl_get_ticks_ms(void);

#undef IMPORT

#endif /* YUMI_SDL_H */
