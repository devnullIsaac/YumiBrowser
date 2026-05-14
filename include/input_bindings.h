/**
 * @file input_bindings.h
 * @brief Host-side input WASM bindings for keyboard, mouse, joystick,
 *        gamepad, touch, and text input (IME).
 *
 * Clipboard is handled separately in clipboard_bindings.h so the
 * dashboard can mediate access independently.
 *
 * ## Example
 *
 * @code{.c}
 * #include "input_bindings.h"
 *
 * InputBindings ib;
 * input_bindings_init(&ib, sdl_window);
 * input_bindings_set_memory(&ib, wasm_memory);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t n = input_bindings_get_imports(&ib, store, &names, &funcs);
 *
 * input_bindings_destroy(&ib);
 * @endcode
 */

#ifndef INPUT_BINDINGS_H
#define INPUT_BINDINGS_H

#include "deps.h"
#include "handle_table.h"
#include <SDL3/SDL.h>
#include <stdint.h>

/**
 * @brief Input binding state.
 */
typedef struct {
    wasm_memory_t *memory;      /**< Guest WASM linear memory. */
    SDL_Window    *window;      /**< SDL window for text input API. */
    HandleTable    joysticks;   /**< Joystick handle table. */
    HandleTable    gamepads;    /**< Gamepad handle table. */
} InputBindings;

/**
 * @brief Initialize input bindings.
 * @param[out] b    Bindings to initialize.
 * @param[in]  win  SDL window for text input / IME.
 */
void   input_bindings_init(InputBindings *b, SDL_Window *win);

/**
 * @brief Destroy input bindings and free handle tables.
 * @param[in,out] b  Bindings to destroy.
 */
void   input_bindings_destroy(InputBindings *b);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] b    Bindings.
 * @param[in]     mem  Guest WASM memory.
 */
void   input_bindings_set_memory(InputBindings *b, wasm_memory_t *mem);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] b          Bindings.
 * @param[in]     store      WASM store.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t input_bindings_get_imports(InputBindings *b, wasm_store_t *store,
                                  const char ***out_names,
                                  wasm_func_t ***out_funcs);

#endif /* INPUT_BINDINGS_H */
