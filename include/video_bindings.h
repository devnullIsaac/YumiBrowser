/**
 * @file video_bindings.h
 * @brief Zero-copy video decode → WebGPU texture WASM bindings.
 *
 * Opens video by file path or WASM memory buffer, decodes frames via
 * FFmpeg HW acceleration, and imports them as WGPUTextures through the
 * WFF bridge. Falls back to software decode + upload when hardware
 * acceleration is unavailable. Includes audio spectrum analysis for
 * visualizations.
 *
 * ## Example
 *
 * @code{.c}
 * #include "video_bindings.h"
 *
 * VideoBindings vb;
 * video_bindings_init(&vb, &wgpu_bindings, wff_bridge_or_null);
 * video_bindings_set_memory(&vb, wasm_memory);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t n = video_bindings_get_imports(&vb, store, &names, &funcs);
 * // Guest calls video_open, video_decode_frame, video_get_texture, etc.
 *
 * // In the host audio thread, pump audio buffers:
 * video_bindings_pump_audio(&vb);
 *
 * video_bindings_destroy(&vb);
 * @endcode
 */

#ifndef VIDEO_BINDINGS_H
#define VIDEO_BINDINGS_H

#include "deps.h"
#include "handle_table.h"
#include "wgpu_bindings.h"
#include "wgpu_ffmpeg.h"

/**
 * @brief Video binding state.
 */
typedef struct {
    WgpuBindings *wgpu;          /**< WebGPU bindings for texture import. */
    WFFBridge    *bridge;         /**< FFmpeg→WebGPU bridge (may be NULL). */
    HandleTable   ht_video;       /**< Maps handle → VideoContext*. */
    wasm_memory_t *memory;        /**< Guest WASM linear memory. */
    SDL_AudioDeviceID audio_device; /**< SDL audio output device. */

    float postmix_spectrum[12];       /**< Post-mix audio spectrum (12 bands). */
    float postmix_spectrum_decay[12]; /**< Decayed spectrum for smooth visualization. */
} VideoBindings;

/**
 * @brief Initialize video bindings.
 *
 * @param[out] vb      Bindings to initialize.
 * @param[in]  wgpu    WebGPU bindings for texture operations.
 * @param[in]  bridge  WFF bridge for zero-copy HW decode (NULL for SW-only).
 */
void   video_bindings_init(VideoBindings *vb,
                           WgpuBindings  *wgpu,
                           WFFBridge     *bridge /* nullable */);

/**
 * @brief Destroy video bindings and free all video contexts.
 * @param[in,out] vb  Bindings to destroy.
 */
void   video_bindings_destroy(VideoBindings *vb);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] vb   Bindings.
 * @param[in]     mem  Guest WASM memory.
 */
void   video_bindings_set_memory(VideoBindings *vb, wasm_memory_t *mem);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] vb         Bindings.
 * @param[in]     store      WASM store.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t video_bindings_get_imports(VideoBindings  *vb,
                                 wasm_store_t   *store,
                                 const char   ***out_names,
                                 wasm_func_t  ***out_funcs);

/**
 * @brief Pump audio buffers for all active video contexts.
 *
 * Host-only call (no WASM or GPU involved). Call from the audio
 * thread or main loop to keep audio playback smooth.
 *
 * @param[in,out] vb  Bindings with active video contexts.
 */
void video_bindings_pump_audio(VideoBindings *vb);

#endif /* VIDEO_BINDINGS_H */
