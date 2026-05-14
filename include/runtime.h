/**
 * @file runtime.h
 * @brief WASM runtime with SDL event dispatch for Yumi Browser.
 *
 * Manages a Wasmer-based WASM runtime that loads guest modules and
 * dispatches SDL input events (keyboard, mouse, touch, joystick,
 * gamepad, window) to exported WASM callbacks.
 *
 * ## Example
 *
 * @code{.c}
 * #include "runtime.h"
 * #include "gpu.h"
 * #include <SDL3/SDL.h>
 *
 * GpuContext gpu;
 * gpu_init(&gpu, window);
 *
 * WasmRuntime rt;
 * runtime_init(&rt, &gpu, "/path/to/group.db");
 * runtime_load(&rt, window, "app.wasm");
 * runtime_call_init(&rt);
 *
 * while (running) {
 *     SDL_Event e;
 *     while (SDL_PollEvent(&e)) {
 *         if (e.type == SDL_EVENT_KEY_DOWN)
 *             runtime_dispatch_key(&rt, e.key.scancode,
 *                 e.key.key, e.key.mod, 1);
 *     }
 *     runtime_call_frame(&rt);
 * }
 *
 * runtime_destroy(&rt);
 * @endcode
 */

#ifndef RUNTIME_H
#define RUNTIME_H

#include "deps.h"
#include "gpu.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief WASM runtime state holding engine, store, module, instance,
 *        and pointers to all exported guest callbacks.
 */
typedef struct {
    GpuContext      *gpu;
    wasm_engine_t   *engine;
    wasm_store_t    *store;
    wasm_module_t   *module;
    wasm_instance_t *instance;

    /* ── Required ── */
    wasm_func_t *fn_frame;

    /* ── Lifecycle ── */
    wasm_func_t *fn_init;

    /* ── Keyboard / Text ── */
    wasm_func_t *fn_on_key;             /* (scancode, keycode, mod, pressed) */
    wasm_func_t *fn_on_char;            /* (codepoint)                       */

    /* ── Mouse ── */
    wasm_func_t *fn_on_mouse_button;    /* (button, pressed, x, y)           */
    wasm_func_t *fn_on_mouse_motion;    /* (x, y, dx, dy, buttons)           */
    wasm_func_t *fn_on_mouse_wheel;     /* (dx, dy)                          */

    /* ── Touch ── */
    wasm_func_t *fn_on_touch;           /* (finger_id, type, x, y, pressure) */

    /* ── Joystick ── */
    wasm_func_t *fn_on_joystick_added;  /* (instance_id)                     */
    wasm_func_t *fn_on_joystick_removed;/* (instance_id)                     */
    wasm_func_t *fn_on_joystick_axis;   /* (instance_id, axis, value)        */
    wasm_func_t *fn_on_joystick_button; /* (instance_id, button, pressed)    */
    wasm_func_t *fn_on_joystick_hat;    /* (instance_id, hat, value)         */

    /* ── Gamepad ── */
    wasm_func_t *fn_on_gamepad_added;   /* (instance_id)                     */
    wasm_func_t *fn_on_gamepad_removed; /* (instance_id)                     */

    /* ── Window ── */
    wasm_func_t *fn_on_resize;          /* (w, h)                            */
    wasm_func_t *fn_on_focus;           /* (gained)                          */
} WasmRuntime;

/**
 * @brief Initialize the WASM engine and store.
 *
 * @param[out] rt       Runtime to initialize.
 * @param[in]  gpu      GPU context for rendering.
 * @param[in]  db_path  Path to the DuckDB database for persistent storage.
 * @return true on success.
 */
bool runtime_init(WasmRuntime *rt, GpuContext *gpu, const char *db_path);

/**
 * @brief Load and instantiate a WASM module from disk.
 *
 * Compiles the module, resolves host imports (GPU, input, font, etc.),
 * and looks up exported guest callbacks (frame, init, event handlers).
 *
 * @param[in,out] rt         Runtime (must be initialized).
 * @param[in]     win        SDL window for input bindings.
 * @param[in]     wasm_path  Path to the .wasm file.
 * @return true on success.
 */
bool runtime_load(WasmRuntime *rt, SDL_Window *win, const char *wasm_path);

/**
 * @brief Call the guest's `init` export (if it exists).
 * @param[in,out] rt  Runtime with a loaded module.
 */
void runtime_call_init(WasmRuntime *rt);

/**
 * @brief Call the guest's `frame` export (runs one frame of the app).
 * @param[in,out] rt  Runtime with a loaded module.
 */
void runtime_call_frame(WasmRuntime *rt);

/**
 * @brief Destroy the runtime and release all WASM resources.
 * @param[in,out] rt  Runtime to destroy.
 */
void runtime_destroy(WasmRuntime *rt);

/* ── Event dispatch — all NULL-safe (no-op if callback not exported) ── */

/**
 * @brief Dispatch a keyboard key event to the guest.
 * @param[in,out] rt        Runtime.
 * @param[in]     scancode  SDL physical scancode.
 * @param[in]     keycode   SDL virtual keycode.
 * @param[in]     mod       Modifier key bitmask.
 * @param[in]     pressed   1 = key down, 0 = key up.
 */
void runtime_dispatch_key(WasmRuntime *rt,
                          int scancode, int keycode, int mod, int pressed);

/**
 * @brief Dispatch a text-input codepoint to the guest.
 * @param[in,out] rt         Runtime.
 * @param[in]     codepoint  Unicode codepoint from text input / IME.
 */
void runtime_dispatch_char(WasmRuntime *rt, uint32_t codepoint);

/**
 * @brief Dispatch a mouse button event.
 * @param[in,out] rt       Runtime.
 * @param[in]     button   Mouse button index (1 = left, 2 = middle, 3 = right).
 * @param[in]     pressed  1 = pressed, 0 = released.
 * @param[in]     x        Cursor X in window coordinates.
 * @param[in]     y        Cursor Y in window coordinates.
 */
void runtime_dispatch_mouse_button(WasmRuntime *rt,
                                   int button, int pressed, float x, float y);

/**
 * @brief Dispatch a mouse motion event.
 * @param[in,out] rt       Runtime.
 * @param[in]     x        New cursor X.
 * @param[in]     y        New cursor Y.
 * @param[in]     dx       Delta X since last motion.
 * @param[in]     dy       Delta Y since last motion.
 * @param[in]     buttons  Bitmask of currently held buttons.
 */
void runtime_dispatch_mouse_motion(WasmRuntime *rt,
                                   float x, float y,
                                   float dx, float dy, int buttons);

/**
 * @brief Dispatch a mouse wheel / scroll event.
 * @param[in,out] rt  Runtime.
 * @param[in]     dx  Horizontal scroll amount.
 * @param[in]     dy  Vertical scroll amount.
 */
void runtime_dispatch_mouse_wheel(WasmRuntime *rt, float dx, float dy);

/**
 * @brief Dispatch a touch event (finger down/move/up).
 * @param[in,out] rt         Runtime.
 * @param[in]     finger_id  Unique finger identifier.
 * @param[in]     type       Touch event type (down / move / up).
 * @param[in]     x          Touch X (0.0 – 1.0 normalized).
 * @param[in]     y          Touch Y (0.0 – 1.0 normalized).
 * @param[in]     pressure   Touch pressure (0.0 – 1.0).
 */
void runtime_dispatch_touch(WasmRuntime *rt,
                            int finger_id, int type,
                            float x, float y, float pressure);

/** @brief Dispatch joystick added event.
 *  @param[in,out] rt          Runtime.
 *  @param[in]     instance_id  SDL joystick instance ID. */
void runtime_dispatch_joystick_added(WasmRuntime *rt, int instance_id);

/** @brief Dispatch joystick removed event.
 *  @param[in,out] rt          Runtime.
 *  @param[in]     instance_id  SDL joystick instance ID. */
void runtime_dispatch_joystick_removed(WasmRuntime *rt, int instance_id);

/**
 * @brief Dispatch joystick axis motion.
 * @param[in,out] rt           Runtime.
 * @param[in]     instance_id  SDL joystick instance ID.
 * @param[in]     axis         Axis index.
 * @param[in]     value        Axis value (-32768 to 32767).
 */
void runtime_dispatch_joystick_axis(WasmRuntime *rt,
                                    int instance_id, int axis, int value);

/**
 * @brief Dispatch joystick button event.
 * @param[in,out] rt           Runtime.
 * @param[in]     instance_id  SDL joystick instance ID.
 * @param[in]     button       Button index.
 * @param[in]     pressed      1 = pressed, 0 = released.
 */
void runtime_dispatch_joystick_button(WasmRuntime *rt,
                                      int instance_id, int button, int pressed);

/**
 * @brief Dispatch joystick hat event.
 * @param[in,out] rt           Runtime.
 * @param[in]     instance_id  SDL joystick instance ID.
 * @param[in]     hat          Hat index.
 * @param[in]     value        Hat direction bitmask.
 */
void runtime_dispatch_joystick_hat(WasmRuntime *rt,
                                   int instance_id, int hat, int value);

/** @brief Dispatch gamepad added event.
 *  @param[in,out] rt          Runtime.
 *  @param[in]     instance_id  SDL gamepad instance ID. */
void runtime_dispatch_gamepad_added(WasmRuntime *rt, int instance_id);

/** @brief Dispatch gamepad removed event.
 *  @param[in,out] rt          Runtime.
 *  @param[in]     instance_id  SDL gamepad instance ID. */
void runtime_dispatch_gamepad_removed(WasmRuntime *rt, int instance_id);

/**
 * @brief Dispatch a window resize event.
 * @param[in,out] rt  Runtime.
 * @param[in]     w   New window width in pixels.
 * @param[in]     h   New window height in pixels.
 */
void runtime_dispatch_resize(WasmRuntime *rt, int w, int h);

/**
 * @brief Dispatch a focus gained/lost event.
 * @param[in,out] rt      Runtime.
 * @param[in]     gained  1 = focus gained, 0 = focus lost.
 */
void runtime_dispatch_focus(WasmRuntime *rt, int gained);

/**
 * @brief Pump audio buffers for active video contexts (host-only, no WASM/GPU).
 * @param[in,out] rt  Runtime.
 */
void runtime_pump_audio(WasmRuntime *rt);
#endif /* RUNTIME_H */
