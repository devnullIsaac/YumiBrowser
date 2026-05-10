/*
    Yumi SDK — Unified Media Decode WASM Imports
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

#ifndef WASM_MEDIA_H
#define WASM_MEDIA_H

/**
 * @file wasm_media.h
 * @brief Unified WebAssembly guest imports for host-side media decoding.
 *
 * @details
 * This header replaces the older `wasm_image.h` and `wasm_video.h`. It exposes
 * a single, polymorphic media decoder that handles still images, animated
 * images, video streams, and standalone audio tracks. The host classifies
 * incoming buffers by magic bytes and routes them to the appropriate decoder
 * (FFmpeg for video / containers / audio, an eager image path for still and
 * animated frames, optional LibRaw for camera RAW). Each function below maps
 * 1:1 to a `media_*` import implemented in `media_bindings.c` on the host side.
 *
 * ## Buffer-Only Policy
 * There is **no `media_open_file()`**. All media is opened from a byte buffer
 * already resident in WASM linear memory via media_open(). This keeps the
 * host filesystem boundary explicit and lets webapps fetch assets through
 * `wasm_network.h` or embed them as resources.
 *
 * ## Handle Model
 * Open media is referenced by an opaque `media_handle_t` integer. The value 0
 * is reserved and always invalid. After media_close() the handle must not be
 * reused. Pass the handle to media_kind() to determine how to drive it.
 *
 * ## Supported Formats
 * - **Still / animated images:** PNG, JPEG, GIF, BMP, TIFF, PSD, OpenEXR,
 *   DPX, ICO, JPEG XL, JPEG 2000, WebP (RIFF), AVIF / HEIC / HEIF (ISOBMFF),
 *   plus camera RAW when the host is built with LibRaw.
 * - **Video / container streams:** MP4 / MOV / 3GP (ISOBMFF), Matroska / WebM,
 *   AVI (RIFF), FLV, MPEG-TS, and anything else FFmpeg's demuxers accept.
 * - **Standalone audio:** WAV (RIFF), MP3 (ID3 / sync), FLAC, Ogg.
 *
 * The actual set of decoders depends on how the host's FFmpeg was configured.
 *
 * ## Drive Semantics
 * After media_open() succeeds, call media_kind() to learn what to do next:
 *   - `MEDIA_KIND_IMAGE`  — eager: the entire animation is decoded up front.
 *                          Loop with media_frame_count() / media_frame_view().
 *   - `MEDIA_KIND_VIDEO`  — streaming: pump with media_decode_next() and
 *                          fetch the latest frame with media_texture_view().
 *                          Audio (if present) is mixed automatically.
 *   - `MEDIA_KIND_AUDIO`  — streaming with no video. The host pumps audio
 *                          autonomously; the guest only needs to manage
 *                          mute / volume / spectrum.
 *
 * ## Decode Tri-State
 * media_decode_next() follows a tri-state convention:
 *   - `1`  — a new frame was decoded and is ready for display.
 *   - `0`  — end of stream (EOF); no more frames.
 *   - `-1` — an error occurred; the stream may be corrupt or unsupported.
 *
 * ## Audio Pipeline
 * The host owns a single SDL audio device shared across all open media. Per-
 * stream mute and volume are honoured by the mixer. The frequency spectrum
 * returned by media_audio_get_spectrum() is computed device-wide on the
 * post-mix output, so it reflects everything currently audible — its argument
 * is a valid handle for routing only and the result is not per-stream.
 *
 * ## Typical Lifecycle
 * @code
 *   media_handle_t m = media_open(buf, len);
 *   switch (media_kind(m)) {
 *   case MEDIA_KIND_IMAGE:
 *       int n = media_frame_count(m);
 *       for (int i = 0; i < n; ++i) {
 *           gpu_texture_view_t v = media_frame_view(m, i);
 *           // render v for media_frame_duration_ms(m, i) ms …
 *       }
 *       break;
 *   case MEDIA_KIND_VIDEO:
 *       while (media_decode_next(m) == 1) {
 *           gpu_texture_view_t v = media_texture_view(m);
 *           // render v …
 *       }
 *       break;
 *   case MEDIA_KIND_AUDIO:
 *       // host pumps audio; just wait until media_position_ms hits duration
 *       break;
 *   }
 *   media_close(m);
 * @endcode
 *
 * @see wasm_gpu.h for gpu_texture_t and gpu_texture_view_t.
 * @see wasm_network.h for fetching media buffers.
 */

#include "wasm_gpu.h"

/**
 * @brief Opaque handle to a host-managed media decoder instance.
 *
 * @details
 * A positive integer representing an open media object on the host. The
 * value 0 is reserved and always invalid. Every media_* function accepts
 * a `media_handle_t` as its first argument (except media_open()).
 */
typedef int media_handle_t;

/**
 * @name MediaKind
 * @brief Values returned by media_kind() identifying how to drive the handle.
 * @{
 */
#define MEDIA_KIND_UNKNOWN  0  /**< Could not classify or open. */
#define MEDIA_KIND_IMAGE    1  /**< Eager still / animated image. */
#define MEDIA_KIND_VIDEO    2  /**< Streaming video (with optional audio). */
#define MEDIA_KIND_AUDIO    3  /**< Streaming audio with no video track. */
/** @} */

/**
 * @brief Number of frequency bands returned by media_audio_get_spectrum().
 *
 * @details
 * The spectrum is divided into logarithmically-spaced bands across the
 * audible range. This is the exact count the host writes into the output
 * buffer when @p count >= MEDIA_SPECTRUM_BANDS.
 */
#define MEDIA_SPECTRUM_BANDS 12

/** @cond INTERNAL */
#define IMPORT __attribute__((import_module("env")))
/** @endcond */

/* ================================================================== */
/*  Open / Close                                                       */
/* ================================================================== */

/**
 * @brief Open a media object from a memory buffer.
 *
 * @details
 * The host inspects the leading bytes (magic / ftyp / RIFF / EBML / ID3 / …)
 * to classify the buffer, then routes it to the still-image, video /
 * container, or audio path. The buffer is copied internally, so it is safe
 * to free or overwrite the WASM-side memory immediately after this call
 * returns.
 *
 * Buffer-only by design: there is no path-based variant. Read your asset
 * over `wasm_network.h`, embed it as a resource, or load it through the
 * webapp's data channel before calling this function.
 *
 * @param[in] buf_ptr  Pointer to the encoded media bytes in WASM memory.
 * @param[in] buf_len  Total byte length of the buffer.
 * @return A valid media_handle_t on success, or 0 on failure (unknown
 *         format, unsupported codec, or insufficient host resources).
 */
IMPORT __attribute__((import_name("media_open")))
media_handle_t media_open(const void *buf_ptr, int buf_len);

/**
 * @brief Close a media object and release all host resources.
 *
 * @details
 * Destroys the decoder, releases GPU textures, stops any audio belonging
 * to this stream, and frees the snapshotted buffer. The handle is invalid
 * after this call.
 *
 * @param[in] handle  The media handle to close.
 */
IMPORT __attribute__((import_name("media_close")))
void media_close(media_handle_t handle);

/* ================================================================== */
/*  Universal metadata                                                 */
/* ================================================================== */

/**
 * @brief Identify how the host classified the buffer.
 *
 * @param[in] handle  Valid media handle.
 * @return One of #MEDIA_KIND_IMAGE, #MEDIA_KIND_VIDEO, #MEDIA_KIND_AUDIO,
 *         or #MEDIA_KIND_UNKNOWN if the handle is invalid.
 */
IMPORT __attribute__((import_name("media_kind")))
int media_kind(media_handle_t handle);

/**
 * @brief Get the media width in pixels.
 *
 * @details
 * For audio-only handles this returns 0.
 *
 * @param[in] handle  Valid media handle.
 * @return Width in pixels, or 0.
 */
IMPORT __attribute__((import_name("media_width")))
int media_width(media_handle_t handle);

/**
 * @brief Get the media height in pixels.
 *
 * @details
 * For audio-only handles this returns 0.
 *
 * @param[in] handle  Valid media handle.
 * @return Height in pixels, or 0.
 */
IMPORT __attribute__((import_name("media_height")))
int media_height(media_handle_t handle);

/**
 * @brief Check whether the media exposes an alpha channel.
 *
 * @details
 * Returns 1 if the format supports transparency (PNG, GIF, WebP, AVIF,
 * HEIC with alpha, …), even if all pixels happen to be opaque. Returns 0
 * for opaque formats and for video / audio handles.
 *
 * @param[in] handle  Valid media handle.
 * @return 1 if alpha is supported, 0 otherwise.
 */
IMPORT __attribute__((import_name("media_has_alpha")))
int media_has_alpha(media_handle_t handle);

/**
 * @brief Get the total duration of the media.
 *
 * @details
 * For animated images this is the sum of all frame durations. For video
 * and audio streams it is the duration reported by the demuxer. Live
 * streams and unknown durations return 0.
 *
 * @param[in] handle  Valid media handle.
 * @return Duration in milliseconds, or 0.
 */
IMPORT __attribute__((import_name("media_duration_ms")))
int media_duration_ms(media_handle_t handle);

/* ================================================================== */
/*  Image (eager) API                                                  */
/* ================================================================== */

/**
 * @brief Get the total number of decoded image frames.
 *
 * @details
 * Valid only when media_kind() == #MEDIA_KIND_IMAGE. For static images
 * this returns 1; for animated GIF / APNG / WebP / AVIF it returns the
 * full frame count. Returns 0 for video and audio handles.
 *
 * @param[in] handle  Valid media handle.
 * @return Frame count, or 0 if not an image.
 */
IMPORT __attribute__((import_name("media_frame_count")))
int media_frame_count(media_handle_t handle);

/**
 * @brief Get the display duration of a specific image frame.
 *
 * @details
 * Valid only when media_kind() == #MEDIA_KIND_IMAGE. Returns how long the
 * given frame should be shown in an animation loop, in milliseconds. For
 * static images this typically returns 0.
 *
 * @param[in] handle       Valid media handle.
 * @param[in] frame_index  Zero-based frame index in [0, media_frame_count()).
 * @return Duration in milliseconds, or 0.0 if unknown / out of range.
 */
IMPORT __attribute__((import_name("media_frame_duration_ms")))
float media_frame_duration_ms(media_handle_t handle, int frame_index);

/**
 * @brief Get a GPU texture view for a specific image frame.
 *
 * @details
 * Valid only when media_kind() == #MEDIA_KIND_IMAGE. The frame is decoded
 * eagerly at media_open() time, so this call is cheap and the returned view
 * remains valid until media_close().
 *
 * @param[in] handle       Valid media handle.
 * @param[in] frame_index  Zero-based frame index in [0, media_frame_count()).
 * @return A gpu_texture_view_t handle, or 0 on error.
 */
IMPORT __attribute__((import_name("media_frame_view")))
gpu_texture_view_t media_frame_view(media_handle_t handle, int frame_index);

/* ================================================================== */
/*  Video / streaming API                                              */
/* ================================================================== */

/**
 * @brief Decode the next streaming frame.
 *
 * @details
 * Valid for #MEDIA_KIND_VIDEO. Advances the internal decoder by one frame.
 * On success the host uploads the new frame to a GPU texture; retrieve it
 * via media_texture_view().
 *
 * @param[in] handle  Valid media handle.
 * @return
 *   - `1`  — frame decoded successfully.
 *   - `0`  — end of stream reached.
 *   - `-1` — decoding error or wrong handle kind.
 */
IMPORT __attribute__((import_name("media_decode_next")))
int media_decode_next(media_handle_t handle);

/**
 * @brief Seek to a specific timestamp.
 *
 * @details
 * Valid for #MEDIA_KIND_VIDEO and #MEDIA_KIND_AUDIO. Repositions the decoder
 * to the closest keyframe at or before the requested millisecond offset.
 * Subsequent calls to media_decode_next() will produce frames starting from
 * the new position.
 *
 * @param[in] handle  Valid media handle.
 * @param[in] ms      Target time in milliseconds from the start of the stream.
 * @return 0 on success, -1 on error.
 */
IMPORT __attribute__((import_name("media_seek")))
int media_seek(media_handle_t handle, int ms);

/**
 * @brief Get the current playback position.
 *
 * @details
 * Returns the timestamp of the most recently decoded frame, in milliseconds.
 * Valid for #MEDIA_KIND_VIDEO and #MEDIA_KIND_AUDIO.
 *
 * @param[in] handle  Valid media handle.
 * @return Current position in milliseconds.
 */
IMPORT __attribute__((import_name("media_position_ms")))
int media_position_ms(media_handle_t handle);

/**
 * @brief Get the GPU texture for the most recently decoded streaming frame.
 *
 * @details
 * Valid for #MEDIA_KIND_VIDEO. Returns the underlying gpu_texture_t backing
 * the current frame. The texture is owned by the host and must not be
 * destroyed by the guest.
 *
 * @param[in] handle  Valid media handle.
 * @return A gpu_texture_t handle, or 0 if no frame has been decoded yet.
 *
 * @see media_texture_view() for a view suitable for binding.
 */
IMPORT __attribute__((import_name("media_texture")))
gpu_texture_t media_texture(media_handle_t handle);

/**
 * @brief Get a GPU texture view for the most recently decoded streaming frame.
 *
 * @details
 * Valid for #MEDIA_KIND_VIDEO. The view can be used directly in bind groups
 * or passed to blit operations. It remains valid until the next call to
 * media_decode_next() or media_close().
 *
 * @param[in] handle  Valid media handle.
 * @return A gpu_texture_view_t handle, or 0 on error.
 */
IMPORT __attribute__((import_name("media_texture_view")))
gpu_texture_view_t media_texture_view(media_handle_t handle);

/**
 * @brief Query the pixel layout of decoded streaming frames.
 *
 * @details
 * Returns an implementation-defined integer describing the memory layout
 * (RGBA, NV12, I420, …). Useful if a webapp wants to choose between an
 * RGB sampling shader and a YUV sampling shader.
 *
 * @param[in] handle  Valid media handle.
 * @return Pixel layout identifier, or 0 if unknown.
 */
IMPORT __attribute__((import_name("media_pixel_layout")))
int media_pixel_layout(media_handle_t handle);

/**
 * @brief Get the number of planes in the streaming frame.
 *
 * @details
 * Typical values: 1 for RGB / RGBA, 2 for NV12, 3 for I420 / YUV420.
 *
 * @param[in] handle  Valid media handle.
 * @return Number of planes, or 0 on error.
 */
IMPORT __attribute__((import_name("media_plane_count")))
int media_plane_count(media_handle_t handle);

/**
 * @brief Check whether streaming frames are hardware-decoded.
 *
 * @details
 * Hardware decoding means the frames already reside in GPU memory and can
 * be sampled without CPU read-back. Software-decoded frames are uploaded
 * by the host before media_texture_view() returns.
 *
 * @param[in] handle  Valid media handle.
 * @return 1 if hardware-accelerated, 0 if software-decoded, -1 on error.
 */
IMPORT __attribute__((import_name("media_is_hw")))
int media_is_hw(media_handle_t handle);

/**
 * @brief Get the codec format identifier.
 *
 * @details
 * Returns an implementation-defined integer representing the underlying
 * codec (H.264, HEVC, VP9, AV1, AAC, Opus, …). Primarily for diagnostics.
 *
 * @param[in] handle  Valid media handle.
 * @return Codec format identifier, or 0 if unknown.
 */
IMPORT __attribute__((import_name("media_format")))
int media_format(media_handle_t handle);

/* ================================================================== */
/*  Audio control                                                      */
/* ================================================================== */

/**
 * @brief Mute or unmute the audio track of this media.
 *
 * @details
 * Valid for any handle that has an audio track (#MEDIA_KIND_VIDEO with an
 * audio stream, or #MEDIA_KIND_AUDIO).
 *
 * @param[in] handle  Valid media handle.
 * @param[in] muted   1 to mute, 0 to unmute.
 * @return 0 on success, -1 on error.
 */
IMPORT __attribute__((import_name("media_audio_set_muted")))
int media_audio_set_muted(media_handle_t handle, int muted);

/**
 * @brief Check whether this media's audio is currently muted.
 *
 * @param[in] handle  Valid media handle.
 * @return 1 if muted, 0 if not muted, -1 on error.
 */
IMPORT __attribute__((import_name("media_audio_is_muted")))
int media_audio_is_muted(media_handle_t handle);

/**
 * @brief Set the audio playback volume for this media.
 *
 * @details
 * Adjusts the per-stream gain that the host mixer applies before feeding
 * the SDL audio device. Independent of the host's master volume.
 *
 * @param[in] handle  Valid media handle.
 * @param[in] volume  Volume level. 0.0 = silent, 1.0 = full volume.
 *                    Values outside this range may be clamped by the host.
 * @return 0 on success, -1 on error.
 */
IMPORT __attribute__((import_name("media_audio_set_volume")))
int media_audio_set_volume(media_handle_t handle, float volume);

/**
 * @brief Get the current audio playback volume for this media.
 *
 * @param[in] handle  Valid media handle.
 * @return Current volume level in [0.0, 1.0], or -1.0 on error.
 */
IMPORT __attribute__((import_name("media_audio_get_volume")))
float media_audio_get_volume(media_handle_t handle);

/**
 * @brief Retrieve the device-wide audio frequency spectrum.
 *
 * @details
 * The host runs an SDL post-mix tap on the shared audio device and computes
 * a logarithmic-band magnitude spectrum that reflects **all** currently
 * audible streams, not just the one referenced by @p handle. The handle is
 * still required so that the host can validate the call against an open
 * media object.
 *
 * @param[in]  handle   Valid media handle.
 * @param[out] out_ptr  Pointer to a float array in WASM linear memory.
 * @param[in]  count    Number of floats to write. Should be at least
 *                      #MEDIA_SPECTRUM_BANDS; extra slots are left untouched.
 * @return 0 on success, -1 on error.
 */
IMPORT __attribute__((import_name("media_audio_get_spectrum")))
int media_audio_get_spectrum(media_handle_t handle, void *out_ptr, int count);

/** @cond INTERNAL */
#undef IMPORT
/** @endcond */

#endif /* WASM_MEDIA_H */
