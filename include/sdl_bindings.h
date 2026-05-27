/*
 * sdl_bindings.h - SDL3 host WASM bindings exposing audio, joystick, gamepad, and keyboard state to guest WASM modules via per-resource handle tables.
 * Copyright (C) 2026 DevNullIsaac
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file sdl_bindings.h
 * @brief SDL3 host WASM bindings for Yumi Browser.
 *
 * Exposes SDL audio, joystick, gamepad, and keyboard state to WASM
 * guests via integer handles. Each SDL resource type has its own
 * handle table.
 *
 * ## Example
 *
 * @code{.c}
 * #include "sdl_bindings.h"
 *
 * SdlBindings sb;
 * sdl_bindings_init(&sb, sdl_window);
 * sdl_bindings_set_memory(&sb, wasm_memory);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t n = sdl_bindings_get_imports(&sb, store, &names, &funcs);
 *
 * sdl_bindings_destroy(&sb);
 * @endcode
 */

#ifndef SDL_BINDINGS_H
#define SDL_BINDINGS_H

#include "handle_table.h"
#include <wasm.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SDL binding state with handle tables for audio, joystick, and gamepad objects.
 */
typedef struct SdlBindings {
    wasm_memory_t *memory;               /**< Guest WASM linear memory. */

    HandleTable ht_audio_device;          /**< SDL_AudioDeviceID handles. */
    HandleTable ht_audio_stream;          /**< SDL_AudioStream* handles. */
    HandleTable ht_joystick;              /**< SDL_Joystick* handles. */
    HandleTable ht_gamepad;               /**< SDL_Gamepad* handles. */

    SDL_Window *default_window;           /**< Default SDL window for text input. */
} SdlBindings;

/**
 * @brief Initialize SDL bindings.
 * @param[out] b               Bindings to initialize.
 * @param[in]  default_window  SDL window used as default focus target.
 */
void sdl_bindings_init   (SdlBindings *b, SDL_Window *default_window);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] b    Bindings.
 * @param[in]     mem  Guest WASM memory.
 */
void sdl_bindings_set_memory(SdlBindings *b, wasm_memory_t *mem);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] b          Bindings.
 * @param[in]     store      WASM store.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t sdl_bindings_get_imports(SdlBindings *b,
                                 wasm_store_t *store,
                                 const char ***out_names,
                                 wasm_func_t ***out_funcs);

/**
 * @brief Destroy SDL bindings and free all handle tables.
 * @param[in,out] b  Bindings to destroy.
 */
void sdl_bindings_destroy(SdlBindings *b);

#ifdef __cplusplus
}
#endif

#endif /* SDL_BINDINGS_H */
