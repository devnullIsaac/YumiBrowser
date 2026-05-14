/*
 * wgpu_ffmpeg.h
 *
 * Zero-copy bridge between FFmpeg hardware-decoded frames and WebGPU (Dawn).
 *
 * Supported platforms:
 *   Linux  : VAAPI → DMA-BUF → WGPUSharedTextureMemoryDmaBuf
 *   macOS  : VideoToolbox → IOSurface → WGPUSharedTextureMemoryIOSurface
 *   Windows: D3D11VA → DXGI Shared Handle → WGPUSharedTextureMemoryDXGISharedHandle
 */

#ifndef WGPU_FFMPEG_ZERO_COPY_H
#define WGPU_FFMPEG_ZERO_COPY_H

#include "webgpu.h"

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/frame.h>

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
 * Platform detection
 * ------------------------------------------------------------------ */

typedef enum WFFPlatform {
    WFF_PLATFORM_LINUX   = 0,
    WFF_PLATFORM_MACOS   = 1,
    WFF_PLATFORM_WINDOWS = 2,
} WFFPlatform;

/* --------------------------------------------------------------------
 * Texture format mapping hint
 * ------------------------------------------------------------------ */

typedef enum WFFPixelLayout {
    WFF_PIXEL_LAYOUT_NV12,       /* Y + interleaved UV (most common)   */
    WFF_PIXEL_LAYOUT_P010,       /* 10-bit Y + interleaved UV          */
    WFF_PIXEL_LAYOUT_BGRA,       /* Single plane BGRA                  */
    WFF_PIXEL_LAYOUT_RGBA,       /* Single plane RGBA                  */
    WFF_PIXEL_LAYOUT_UNKNOWN,
} WFFPixelLayout;

/* --------------------------------------------------------------------
 * Imported frame — wraps one AVFrame as a WGPUTexture
 * ------------------------------------------------------------------ */

typedef struct WFFImportedFrame {
    /* The resulting WebGPU texture.  Owned by the shared texture memory;
     * released when you call wff_imported_frame_release().              */
    WGPUTexture                   texture;

    /* Shared texture memory handle used for access arbitration. */
    WGPUSharedTextureMemory       shared_mem;

    /* Shared fence for synchronization (may be NULL on some paths). */
    WGPUSharedFence               fence;

    /* Pixel layout detected from the AVFrame. */
    WFFPixelLayout                pixel_layout;

    /* Dimensions of the imported texture. */
    uint32_t                      width;
    uint32_t                      height;

    /* Number of texture planes (1 for packed, 2–3 for planar YUV). */
    uint32_t                      plane_count;

    /* Platform-specific opaque state (e.g. held fd, HANDLE, etc.).
     * Freed by wff_imported_frame_release(). */
    void                         *platform_data;
} WFFImportedFrame;

/* --------------------------------------------------------------------
 * Bridge context — one per WGPUDevice
 * ------------------------------------------------------------------ */

typedef struct WFFBridgeConfig {
    WGPUDevice   device;
    WGPUQueue    queue;

    /* If true, the bridge will call wgpuSharedTextureMemoryBeginAccess /
     * EndAccess around every import automatically.  Set false if you
     * want to manage access yourself.                                  */
    bool         auto_access;
} WFFBridgeConfig;

typedef struct WFFBridge WFFBridge;

/* --------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------ */

/*  Create a bridge.  Returns NULL on failure. */
WFFBridge *wff_bridge_create(const WFFBridgeConfig *config);

/*  Destroy the bridge and release internal resources. */
void wff_bridge_destroy(WFFBridge *bridge);

/*  Return the platform the bridge was compiled for. */
WFFPlatform wff_bridge_platform(const WFFBridge *bridge);

/*  Populate an AVCodecContext's hw_device_ctx so that FFmpeg will
 *  decode into the correct memory type for zero-copy import.
 *
 *  Call this BEFORE avcodec_open2().
 *
 *  On Linux  : creates a VAAPI device.
 *  On macOS  : creates a VideoToolbox device.
 *  On Windows: creates a D3D11VA device, optionally sharing the
 *              same ID3D11Device that Dawn uses (pass the Dawn
 *              device's native D3D11 device as |native_device|,
 *              or NULL to let FFmpeg create its own).
 *
 *  Returns 0 on success, negative AVERROR on failure.
 */
int wff_bridge_init_hw_decoder(WFFBridge *bridge,
                               AVCodecContext *codec_ctx,
                               void *native_device /* nullable */);

/*  Import one hardware-decoded AVFrame into WebGPU.
 *
 *  The AVFrame MUST come from a hardware decoder whose hw_device_ctx
 *  was set up via wff_bridge_init_hw_decoder() (or is otherwise
 *  compatible).
 *
 *  |out| is filled on success.  The caller owns the imported frame
 *  and must call wff_imported_frame_release() when done.
 *
 *  Returns 0 on success, -1 on failure.
 */
int wff_bridge_import_frame(WFFBridge *bridge,
                            const AVFrame *frame,
                            WFFImportedFrame *out);

/*  End GPU access and release all resources tied to an imported frame.
 *  After this call the WGPUTexture is no longer valid.
 */
void wff_imported_frame_release(WFFBridge *bridge,
                                WFFImportedFrame *frame);

/* --------------------------------------------------------------------
 * Utility helpers
 * ------------------------------------------------------------------ */

/*  Map an AVPixelFormat (the hw_frames_ctx sw_format) to our layout
 *  enum so callers know which WebGPU texture format to expect per
 *  plane.                                                            */
WFFPixelLayout wff_pixel_layout_from_av(enum AVPixelFormat fmt);

/*  Suggest a WGPUTextureFormat for a given plane of a layout.
 *  Returns WGPUTextureFormat_Undefined if unknown.                   */
WGPUTextureFormat wff_suggest_texture_format(WFFPixelLayout layout,
                                             uint32_t plane_index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WGPU_FFMPEG_ZERO_COPY_H */
