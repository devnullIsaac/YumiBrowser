/**
 * @file media_bindings.h
 * @brief Unified media (image / video / audio) decode → WebGPU/SDL WASM bindings.
 *
 * One handle type, buffer-only entry point, magic-byte autodetection.
 *
 * Format detection priority on `media_open`:
 *   1. Magic-byte sniff to classify the buffer as IMAGE, CONTAINER (video
 *      or muxed) or AUDIO.
 *   2. FFmpeg AVIO open from the in-memory copy.
 *   3. Stills/animated images (PNG, JPEG, GIF, APNG, WebP, AVIF, HEIC,
 *      JPEG-XL, BMP, TIFF, PSD, EXR, JPEG2000, DPX, ICO, TGA …) are decoded
 *      eagerly to BGRA WebGPU textures, one per frame.
 *   4. Containers with a real video stream stream-decode like the old
 *      video_bindings (HW path through WFFBridge, SW fallback to BGRA upload).
 *   5. Audio-only inputs (MP3, FLAC, Ogg/Opus/Vorbis, WAV, M4A …) demux +
 *      decode + push S16 stereo @ 48 kHz to an SDL audio stream.
 *   6. If FFmpeg refuses the buffer, an optional LibRaw pass tries camera
 *      RAW (CR2, NEF, ARW, DNG, RAF, RW2 …).
 *
 * The host exposes ONE flat WASM import set named `media_*`.
 *
 * ## Example
 * @code{.c}
 * MediaBindings mb;
 * media_bindings_init(&mb, &wgpu, wff_bridge_or_null);
 * media_bindings_set_memory(&mb, wasm_memory);
 *
 * const char **names; wasm_func_t **funcs;
 * size_t n = media_bindings_get_imports(&mb, store, &names, &funcs);
 *
 * // host audio thread:
 * media_bindings_pump_audio(&mb);
 *
 * media_bindings_destroy(&mb);
 * @endcode
 */

#ifndef MEDIA_BINDINGS_H
#define MEDIA_BINDINGS_H

#include "deps.h"
#include "handle_table.h"
#include "wgpu_bindings.h"
#include "wgpu_ffmpeg.h"

/** Media kind reported by `media_kind()`. */
typedef enum {
    MEDIA_KIND_UNKNOWN = 0,
    MEDIA_KIND_IMAGE   = 1,  /**< Still or animated image (eager-decoded). */
    MEDIA_KIND_VIDEO   = 2,  /**< Container with a video stream (streaming). */
    MEDIA_KIND_AUDIO   = 3,  /**< Audio-only container (streaming). */
} MediaKind;

/** Unified per-instance binding state. */
typedef struct {
    WgpuBindings     *wgpu;          /**< WebGPU bindings. */
    WFFBridge        *bridge;         /**< Optional zero-copy HW bridge. */
    HandleTable       ht_media;       /**< handle → MediaContext*. */
    wasm_memory_t    *memory;         /**< Guest WASM linear memory. */
    SDL_AudioDeviceID audio_device;   /**< Shared SDL audio playback device. */

    float postmix_spectrum_decay[12]; /**< Device-wide post-mix spectrum. */
} MediaBindings;

/** Initialise. `bridge` may be NULL (SW-only). */
void   media_bindings_init(MediaBindings *mb,
                           WgpuBindings  *wgpu,
                           WFFBridge     *bridge);

/** Tear down all open media + the audio device. */
void   media_bindings_destroy(MediaBindings *mb);

/** Update the WASM linear memory pointer after instantiation. */
void   media_bindings_set_memory(MediaBindings *mb, wasm_memory_t *mem);

/** Build the import table (function names + wasm_func_t handles). */
size_t media_bindings_get_imports(MediaBindings  *mb,
                                  wasm_store_t   *store,
                                  const char   ***out_names,
                                  wasm_func_t  ***out_funcs);

/**
 * Host-side audio pump. Call from the audio thread / main loop to keep
 * the SDL audio streams of all open MEDIA_KIND_VIDEO / MEDIA_KIND_AUDIO
 * contexts filled.
 */
void   media_bindings_pump_audio(MediaBindings *mb);

#endif /* MEDIA_BINDINGS_H */
