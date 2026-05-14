/*
 * wgpu_ffmpeg.c
 *
 * Implementation of the zero-copy FFmpeg ↔ WebGPU (Dawn) bridge.
 *
 * Build with exactly ONE platform path active.  The compiler selects
 * automatically via predefined macros.
 */

#include "wgpu_ffmpeg.h"
#include "ffmpeg_loader.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ====================================================================
 * Platform headers
 * ==================================================================== */

#if defined(__linux__) && !defined(__APPLE__)
#  define WFF_LINUX 1
#  include <libavutil/hwcontext_vaapi.h>
#  include <libavutil/hwcontext_drm.h>
#  include <va/va.h>
#  include <va/va_drmcommon.h>
#  include <unistd.h>   /* close() */
#elif defined(__APPLE__)
#  define WFF_MACOS 1
#  include <libavutil/hwcontext_videotoolbox.h>
#  include <CoreVideo/CoreVideo.h>
#  include <IOSurface/IOSurface.h>
#elif defined(_WIN32)
#  define WFF_WINDOWS 1
#  include <libavutil/hwcontext_d3d11va.h>
#  include <d3d11.h>
#  include <dxgi1_2.h>
#else
#  error "Unsupported platform"
#endif

/* ====================================================================
 * Internal bridge structure
 * ==================================================================== */

struct WFFBridge {
    WGPUDevice   device;
    WGPUQueue    queue;
    bool         auto_access;

#if WFF_LINUX
    AVBufferRef *vaapi_device_ref;   /* AVHWDeviceContext (VAAPI) */
#elif WFF_MACOS
    AVBufferRef *vt_device_ref;      /* AVHWDeviceContext (VideoToolbox) */
#elif WFF_WINDOWS
    AVBufferRef *d3d11_device_ref;   /* AVHWDeviceContext (D3D11VA) */
#endif
};

/* ====================================================================
 * Platform data attached to each imported frame
 * ==================================================================== */

#if WFF_LINUX
typedef struct {
    int   fds[4];          /* DMA-BUF file descriptors to close */
    int   fd_count;
} WFFPlatformDataLinux;
#elif WFF_MACOS
typedef struct {
    /* IOSurface ref kept alive while the texture is in use. */
    IOSurfaceRef surface;
} WFFPlatformDataMacOS;
#elif WFF_WINDOWS
typedef struct {
    HANDLE shared_handle;  /* DXGI shared handle (must be closed) */
} WFFPlatformDataWindows;
#endif

/* ====================================================================
 * Utility: pixel layout helpers
 * ==================================================================== */

WFFPixelLayout wff_pixel_layout_from_av(enum AVPixelFormat fmt)
{
    switch (fmt) {
    case AV_PIX_FMT_NV12:   return WFF_PIXEL_LAYOUT_NV12;
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P010BE: return WFF_PIXEL_LAYOUT_P010;
    case AV_PIX_FMT_BGRA:   return WFF_PIXEL_LAYOUT_BGRA;
    case AV_PIX_FMT_RGBA:   return WFF_PIXEL_LAYOUT_RGBA;
    default:                 return WFF_PIXEL_LAYOUT_UNKNOWN;
    }
}

WGPUTextureFormat wff_suggest_texture_format(WFFPixelLayout layout,
                                             uint32_t plane_index)
{
    switch (layout) {
    case WFF_PIXEL_LAYOUT_NV12:
        /* Plane 0 = Y  (R8Unorm),  Plane 1 = UV (RG8Unorm) */
        if (plane_index == 0) return WGPUTextureFormat_R8Unorm;
        if (plane_index == 1) return WGPUTextureFormat_RG8Unorm;
        break;
    case WFF_PIXEL_LAYOUT_P010:
        /* Plane 0 = Y  (R16Unorm), Plane 1 = UV (RG16Unorm) */
        if (plane_index == 0) return WGPUTextureFormat_R16Unorm;
        if (plane_index == 1) return WGPUTextureFormat_RG16Unorm;
        break;
    case WFF_PIXEL_LAYOUT_BGRA:
        if (plane_index == 0) return WGPUTextureFormat_BGRA8Unorm;
        break;
    case WFF_PIXEL_LAYOUT_RGBA:
        if (plane_index == 0) return WGPUTextureFormat_RGBA8Unorm;
        break;
    default:
        break;
    }
    return WGPUTextureFormat_Undefined;
}

/* ====================================================================
 * Bridge lifetime
 * ==================================================================== */

WFFBridge *wff_bridge_create(const WFFBridgeConfig *config)
{
    if (!config || !config->device || !config->queue)
        return NULL;

    WFFBridge *b = (WFFBridge *)calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->device      = config->device;
    b->queue       = config->queue;
    b->auto_access = config->auto_access;
    return b;
}

void wff_bridge_destroy(WFFBridge *bridge)
{
    if (!bridge) return;

#if WFF_LINUX
    av_buffer_unref(&bridge->vaapi_device_ref);
#elif WFF_MACOS
    av_buffer_unref(&bridge->vt_device_ref);
#elif WFF_WINDOWS
    av_buffer_unref(&bridge->d3d11_device_ref);
#endif

    free(bridge);
}

WFFPlatform wff_bridge_platform(const WFFBridge *bridge)
{
    (void)bridge;
#if WFF_LINUX
    return WFF_PLATFORM_LINUX;
#elif WFF_MACOS
    return WFF_PLATFORM_MACOS;
#elif WFF_WINDOWS
    return WFF_PLATFORM_WINDOWS;
#endif
}

/* ====================================================================
 * Hardware decoder initialisation
 * ==================================================================== */

int wff_bridge_init_hw_decoder(WFFBridge *bridge,
                               AVCodecContext *codec_ctx,
                               void *native_device)
{
    int ret;

#if WFF_LINUX
    (void)native_device;
    /*  Create a VAAPI device.  Pass NULL to use the default DRM render
     *  node (/dev/dri/renderD128).  If the caller wants a specific
     *  node they can set the LIBVA_DRIVER_NAME / device path env.     */
    ret = av_hwdevice_ctx_create(&bridge->vaapi_device_ref,
                                 AV_HWDEVICE_TYPE_VAAPI,
                                 NULL, NULL, 0);
    if (ret < 0) return ret;

    codec_ctx->hw_device_ctx = av_buffer_ref(bridge->vaapi_device_ref);
    if (!codec_ctx->hw_device_ctx) return AVERROR(ENOMEM);

#elif WFF_MACOS
    (void)native_device;
    ret = av_hwdevice_ctx_create(&bridge->vt_device_ref,
                                 AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                 NULL, NULL, 0);
    if (ret < 0) return ret;

    codec_ctx->hw_device_ctx = av_buffer_ref(bridge->vt_device_ref);
    if (!codec_ctx->hw_device_ctx) return AVERROR(ENOMEM);

#elif WFF_WINDOWS
    /*  If the caller supplies the Dawn-internal ID3D11Device we can
     *  share the same device context, which avoids an extra copy on
     *  the GPU.  Otherwise FFmpeg makes its own D3D11 device.         */
    if (native_device) {
        AVBufferRef *dev_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!dev_ref) return AVERROR(ENOMEM);

        AVHWDeviceContext     *dev_ctx  = (AVHWDeviceContext *)dev_ref->data;
        AVD3D11VADeviceContext *d3d11hw = (AVD3D11VADeviceContext *)dev_ctx->hwctx;

        /* Take ownership reference on the caller-supplied device. */
        ID3D11Device *d3d_dev = (ID3D11Device *)native_device;
        ID3D11Device_AddRef(d3d_dev);
        d3d11hw->device = d3d_dev;

        ret = av_hwdevice_ctx_init(dev_ref);
        if (ret < 0) {
            av_buffer_unref(&dev_ref);
            return ret;
        }
        bridge->d3d11_device_ref = dev_ref;
    } else {
        ret = av_hwdevice_ctx_create(&bridge->d3d11_device_ref,
                                     AV_HWDEVICE_TYPE_D3D11VA,
                                     NULL, NULL, 0);
        if (ret < 0) return ret;
    }

    codec_ctx->hw_device_ctx = av_buffer_ref(bridge->d3d11_device_ref);
    if (!codec_ctx->hw_device_ctx) return AVERROR(ENOMEM);
#endif

    return 0;
}

/* ====================================================================
 * Frame import — Linux / VAAPI / DMA-BUF
 * ==================================================================== */

#if WFF_LINUX

/*  Helper: map an AVFrame (VAAPI surface) to a DRM PRIME descriptor,
 *  then import each plane via Dawn's SharedTextureMemoryDmaBuf.       */
static int import_frame_linux(WFFBridge *bridge,
                              const AVFrame *frame,
                              WFFImportedFrame *out)
{
    /* Map the VAAPI surface to a DRM frame so we get DMA-BUF fds. */
    AVFrame *drm_frame = av_frame_alloc();
    if (!drm_frame) return -1;

    drm_frame->format = AV_PIX_FMT_DRM_PRIME;
    int ret = av_hwframe_map(drm_frame, frame,
                             AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
    if (ret < 0) {
        av_frame_free(&drm_frame);
        return -1;
    }

    const AVDRMFrameDescriptor *desc =
        (const AVDRMFrameDescriptor *)drm_frame->data[0];
    if (!desc || desc->nb_layers == 0) {
        av_frame_free(&drm_frame);
        return -1;
    }

    /* Determine the SW pixel format from the hw_frames_ctx. */
    enum AVPixelFormat sw_fmt = AV_PIX_FMT_NONE;
    if (frame->hw_frames_ctx) {
        AVHWFramesContext *fctx =
            (AVHWFramesContext *)frame->hw_frames_ctx->data;
        sw_fmt = fctx->sw_format;
    }

    WFFPixelLayout layout = wff_pixel_layout_from_av(sw_fmt);

    /* Build the DMA-BUF plane array for Dawn.
     *
     * Dawn expects one entry per DMA-BUF *memory* plane (i.e. object),
     * not one per logical plane.  For linear NV12 there are typically
     * 2 objects and 2 logical planes so they match, but a tiled modifier
     * often packs both Y and UV into a single object — Dawn then wants
     * only 1 plane entry.
     *
     * Strategy: emit one plane per DRM object, taking offset/stride
     * from the *first* logical plane that references that object.      */
    WGPUSharedTextureMemoryDmaBufPlane planes[4];
    memset(planes, 0, sizeof(planes));

    uint32_t total_planes = (uint32_t)desc->nb_objects;
    if (total_planes > 4) total_planes = 4;

    bool obj_seen[4] = {false};
    for (int li = 0; li < desc->nb_layers; li++) {
        const AVDRMLayerDescriptor *layer = &desc->layers[li];
        for (int pi = 0; pi < layer->nb_planes; pi++) {
            const AVDRMPlaneDescriptor *p = &layer->planes[pi];
            int oi = p->object_index;
            if (oi < 0 || oi >= (int)total_planes) continue;
            if (obj_seen[oi]) continue;          /* first reference wins */
            obj_seen[oi] = true;
            planes[oi].fd     = desc->objects[oi].fd;
            planes[oi].offset = (uint64_t)p->offset;
            planes[oi].stride = (uint32_t)p->pitch;
        }
    }

    /* Determine DRM format & modifier from the first object. */
    uint32_t drm_format   = desc->layers[0].format;
    uint64_t drm_modifier = desc->objects[0].format_modifier;

    WGPUExtent3D size = {
        .width              = (uint32_t)frame->width,
        .height             = (uint32_t)frame->height,
        .depthOrArrayLayers = 1,
    };

    WGPUSharedTextureMemoryDmaBufDescriptor dmabuf_desc = {
        .chain = {
            .next  = NULL,
            .sType = WGPUSType_SharedTextureMemoryDmaBufDescriptor,
        },
        .size        = size,
        .drmFormat   = drm_format,
        .drmModifier = drm_modifier,
        .planeCount  = (size_t)total_planes,
        .planes      = planes,
    };

    WGPUSharedTextureMemoryDescriptor mem_desc = {
        .nextInChain = (WGPUChainedStruct *)&dmabuf_desc,
        .label       = { .data = "ffmpeg-dmabuf", .length = 13 },
    };

    WGPUSharedTextureMemory shared_mem =
        wgpuDeviceImportSharedTextureMemory(bridge->device, &mem_desc);
    if (!shared_mem) {
        av_frame_free(&drm_frame);
        return -1;
    }

    /* Create a texture from the shared memory. */
    WGPUTexture texture =
        wgpuSharedTextureMemoryCreateTexture(shared_mem, NULL);
    if (!texture) {
        wgpuSharedTextureMemoryRelease(shared_mem);
        av_frame_free(&drm_frame);
        return -1;
    }

    /* Begin access if requested. */
        if (bridge->auto_access) {
        /* Vulkan backend requires the initial image layout.
         * DMA-BUF imports arrive as VK_IMAGE_LAYOUT_UNDEFINED (0). */
        WGPUSharedTextureMemoryVkImageLayoutBeginState vk_layout = {
            .chain = {
                .next  = NULL,
                .sType = WGPUSType_SharedTextureMemoryVkImageLayoutBeginState,
            },
            .oldLayout = 0,  /* VK_IMAGE_LAYOUT_UNDEFINED */
            .newLayout = 0,  /* VK_IMAGE_LAYOUT_UNDEFINED */
        };
        WGPUSharedTextureMemoryBeginAccessDescriptor begin_desc = {
            .nextInChain    = (WGPUChainedStruct *)&vk_layout,
            .concurrentRead = WGPU_FALSE,
            .initialized    = WGPU_TRUE,
            .fenceCount     = 0,
            .fences         = NULL,
            .signaledValues = NULL,
        };
        WGPUStatus st = wgpuSharedTextureMemoryBeginAccess(
            shared_mem, texture, &begin_desc);
        if (st != WGPUStatus_Success) {
            wgpuTextureRelease(texture);
            wgpuSharedTextureMemoryRelease(shared_mem);
            av_frame_free(&drm_frame);
            return -1;
        }
    }

    /* Keep DMA-BUF fds alive — dup them so AVFrame can be freed
     * independently.  We close our copies in _release().            */
    WFFPlatformDataLinux *pd = (WFFPlatformDataLinux *)
        calloc(1, sizeof(*pd));
    for (int i = 0; i < desc->nb_objects && i < 4; i++) {
        pd->fds[i] = dup(desc->objects[i].fd);
        pd->fd_count++;
    }

    av_frame_free(&drm_frame);

    out->texture       = texture;
    out->shared_mem    = shared_mem;
    out->fence         = NULL;
    out->pixel_layout  = layout;
    out->width         = (uint32_t)frame->width;
    out->height        = (uint32_t)frame->height;
    out->plane_count   = total_planes;
    out->platform_data = pd;
    return 0;
}

static void release_frame_linux(WFFBridge *bridge, WFFImportedFrame *frame)
{
    if (bridge->auto_access) {
        WGPUSharedTextureMemoryVkImageLayoutEndState vk_end_layout = {
    .chain = { .next = NULL, .sType = WGPUSType_SharedTextureMemoryVkImageLayoutEndState },
    .oldLayout = 0, .newLayout = 0,
};
WGPUSharedTextureMemoryEndAccessState end_state = {
    .nextInChain = (WGPUChainedStruct *)&vk_end_layout,
};
wgpuSharedTextureMemoryEndAccess(
    frame->shared_mem, frame->texture, &end_state);
wgpuSharedTextureMemoryEndAccessStateFreeMembers(end_state);
    }

    wgpuTextureRelease(frame->texture);
    wgpuSharedTextureMemoryRelease(frame->shared_mem);

    WFFPlatformDataLinux *pd = (WFFPlatformDataLinux *)frame->platform_data;
    if (pd) {
        for (int i = 0; i < pd->fd_count; i++) {
            if (pd->fds[i] >= 0) close(pd->fds[i]);
        }
        free(pd);
    }
}

#endif /* WFF_LINUX */

/* ====================================================================
 * Frame import — macOS / VideoToolbox / IOSurface
 * ==================================================================== */

#if WFF_MACOS

static int import_frame_macos(WFFBridge *bridge,
                              const AVFrame *frame,
                              WFFImportedFrame *out)
{
    /* The VideoToolbox frame's data[3] is a CVPixelBufferRef. */
    CVPixelBufferRef pixbuf = (CVPixelBufferRef)frame->data[3];
    if (!pixbuf) return -1;

    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixbuf);
    if (!surface) return -1;

    /* Retain the IOSurface so it stays alive while we use the texture. */
    IOSurfaceIncrementUseCount(surface);

    /* Determine pixel layout. */
    enum AVPixelFormat sw_fmt = AV_PIX_FMT_NONE;
    if (frame->hw_frames_ctx) {
        AVHWFramesContext *fctx =
            (AVHWFramesContext *)frame->hw_frames_ctx->data;
        sw_fmt = fctx->sw_format;
    }
    WFFPixelLayout layout = wff_pixel_layout_from_av(sw_fmt);

    /* Build the IOSurface import descriptor. */
    WGPUSharedTextureMemoryIOSurfaceDescriptor ios_desc = {
        .chain = {
            .next  = NULL,
            .sType = WGPUSType_SharedTextureMemoryIOSurfaceDescriptor,
        },
        .ioSurface           = (void *)surface,
        .allowStorageBinding = WGPU_FALSE,
    };

    WGPUSharedTextureMemoryDescriptor mem_desc = {
        .nextInChain = (WGPUChainedStruct *)&ios_desc,
        .label       = { .data = "ffmpeg-iosurface", .length = 16 },
    };

    WGPUSharedTextureMemory shared_mem =
        wgpuDeviceImportSharedTextureMemory(bridge->device, &mem_desc);
    if (!shared_mem) {
        IOSurfaceDecrementUseCount(surface);
        return -1;
    }

    WGPUTexture texture =
        wgpuSharedTextureMemoryCreateTexture(shared_mem, NULL);
    if (!texture) {
        wgpuSharedTextureMemoryRelease(shared_mem);
        IOSurfaceDecrementUseCount(surface);
        return -1;
    }

    if (bridge->auto_access) {
        WGPUSharedTextureMemoryBeginAccessDescriptor begin_desc = {
            .nextInChain  = NULL,
            .concurrentRead = WGPU_FALSE,
            .initialized  = WGPU_TRUE,
            .fenceCount   = 0,
            .fences       = NULL,
            .signaledValues = NULL,
        };
        WGPUStatus st = wgpuSharedTextureMemoryBeginAccess(
            shared_mem, texture, &begin_desc);
        if (st != WGPUStatus_Success) {
            wgpuTextureRelease(texture);
            wgpuSharedTextureMemoryRelease(shared_mem);
            IOSurfaceDecrementUseCount(surface);
            return -1;
        }
    }

    WFFPlatformDataMacOS *pd = (WFFPlatformDataMacOS *)
        calloc(1, sizeof(*pd));
    pd->surface = surface;

    size_t plane_count = IOSurfaceGetPlaneCount(surface);
    if (plane_count == 0) plane_count = 1; /* non-planar */

    out->texture       = texture;
    out->shared_mem    = shared_mem;
    out->fence         = NULL;
    out->pixel_layout  = layout;
    out->width         = (uint32_t)frame->width;
    out->height        = (uint32_t)frame->height;
    out->plane_count   = (uint32_t)plane_count;
    out->platform_data = pd;
    return 0;
}

static void release_frame_macos(WFFBridge *bridge, WFFImportedFrame *frame)
{
    if (bridge->auto_access) {
        WGPUSharedTextureMemoryEndAccessState end_state =
            WGPU_SHARED_TEXTURE_MEMORY_END_ACCESS_STATE_INIT;
        wgpuSharedTextureMemoryEndAccess(
            frame->shared_mem, frame->texture, &end_state);
        wgpuSharedTextureMemoryEndAccessStateFreeMembers(end_state);
    }

    wgpuTextureDestroy(frame->texture);
    wgpuTextureRelease(frame->texture);
    wgpuSharedTextureMemoryRelease(frame->shared_mem);

    WFFPlatformDataMacOS *pd = (WFFPlatformDataMacOS *)frame->platform_data;
    if (pd) {
        if (pd->surface) IOSurfaceDecrementUseCount(pd->surface);
        free(pd);
    }
}

#endif /* WFF_MACOS */

/* ====================================================================
 * Frame import — Windows / D3D11VA / DXGI Shared Handle
 * ==================================================================== */

#if WFF_WINDOWS

static int import_frame_windows(WFFBridge *bridge,
                                const AVFrame *frame,
                                WFFImportedFrame *out)
{
    /* D3D11VA: data[0] = ID3D11Texture2D*, data[1] = (intptr_t) array index */
    ID3D11Texture2D *src_tex = (ID3D11Texture2D *)frame->data[0];
    if (!src_tex) return -1;

    intptr_t array_index = (intptr_t)frame->data[1];

    /* Determine pixel layout. */
    enum AVPixelFormat sw_fmt = AV_PIX_FMT_NONE;
    if (frame->hw_frames_ctx) {
        AVHWFramesContext *fctx =
            (AVHWFramesContext *)frame->hw_frames_ctx->data;
        sw_fmt = fctx->sw_format;
    }
    WFFPixelLayout layout = wff_pixel_layout_from_av(sw_fmt);

    /* We need a shared DXGI handle.  FFmpeg's texture arrays often
     * share one ID3D11Texture2D across many frames (array slices).
     * Dawn's DXGI import wants a standalone texture, so we must
     * copy the relevant slice into a staging texture with the
     * SHARED_NTHANDLE / SHARED misc flag.
     *
     * If your D3D11 device is shared with Dawn (same device) you
     * could skip this copy and use SharedTextureMemoryD3D11Texture2D
     * directly, but that feature requires the Dawn-specific extension.
     * Here we use the portable DXGI shared-handle path.
     */

    ID3D11Device *device = NULL;
    ID3D11Texture2D_GetDevice(src_tex, &device);
    if (!device) return -1;

    D3D11_TEXTURE2D_DESC src_desc;
    ID3D11Texture2D_GetDesc(src_tex, &src_desc);

    D3D11_TEXTURE2D_DESC staging_desc = {
        .Width          = src_desc.Width,
        .Height         = src_desc.Height,
        .MipLevels      = 1,
        .ArraySize      = 1,
        .Format         = src_desc.Format,
        .SampleDesc     = { .Count = 1, .Quality = 0 },
        .Usage          = D3D11_USAGE_DEFAULT,
        .BindFlags      = D3D11_BIND_SHADER_RESOURCE,
        .CPUAccessFlags = 0,
        .MiscFlags      = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
    };

    ID3D11Texture2D *staging = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(
        device, &staging_desc, NULL, &staging);
    if (FAILED(hr)) {
        ID3D11Device_Release(device);
        return -1;
    }

    /* Copy the single array slice into the staging texture. */
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device_GetImmediateContext(device, &ctx);

    UINT src_subresource = D3D11CalcSubresource(0, (UINT)array_index,
                                                 src_desc.MipLevels);
    ID3D11DeviceContext_CopySubresourceRegion(
        ctx, (ID3D11Resource *)staging, 0, 0, 0, 0,
        (ID3D11Resource *)src_tex, src_subresource, NULL);
    ID3D11DeviceContext_Flush(ctx);
    ID3D11DeviceContext_Release(ctx);

    /* Obtain the DXGI shared handle. */
    IDXGIResource1 *dxgi_res = NULL;
    hr = ID3D11Texture2D_QueryInterface(
        staging, &IID_IDXGIResource1, (void **)&dxgi_res);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(staging);
        ID3D11Device_Release(device);
        return -1;
    }

    HANDLE shared_handle = NULL;
    hr = IDXGIResource1_CreateSharedHandle(
        dxgi_res, NULL, DXGI_SHARED_RESOURCE_READ, NULL, &shared_handle);
    IDXGIResource1_Release(dxgi_res);
    ID3D11Texture2D_Release(staging);
    ID3D11Device_Release(device);

    if (FAILED(hr) || !shared_handle) return -1;

    /* Import into Dawn via SharedTextureMemoryDXGISharedHandle. */
    WGPUSharedTextureMemoryDXGISharedHandleDescriptor dxgi_desc = {
        .chain = {
            .next  = NULL,
            .sType = WGPUSType_SharedTextureMemoryDXGISharedHandleDescriptor,
        },
        .handle         = shared_handle,
        .useKeyedMutex  = WGPU_TRUE,
    };

    WGPUSharedTextureMemoryDescriptor mem_desc = {
        .nextInChain = (WGPUChainedStruct *)&dxgi_desc,
        .label       = { .data = "ffmpeg-dxgi", .length = 11 },
    };

    WGPUSharedTextureMemory shared_mem =
        wgpuDeviceImportSharedTextureMemory(bridge->device, &mem_desc);
    if (!shared_mem) {
        CloseHandle(shared_handle);
        return -1;
    }

    WGPUTexture texture =
        wgpuSharedTextureMemoryCreateTexture(shared_mem, NULL);
    if (!texture) {
        wgpuSharedTextureMemoryRelease(shared_mem);
        CloseHandle(shared_handle);
        return -1;
    }

    if (bridge->auto_access) {
        WGPUSharedTextureMemoryBeginAccessDescriptor begin_desc = {
            .nextInChain  = NULL,
            .concurrentRead = WGPU_FALSE,
            .initialized  = WGPU_TRUE,
            .fenceCount   = 0,
            .fences       = NULL,
            .signaledValues = NULL,
        };
        WGPUStatus st = wgpuSharedTextureMemoryBeginAccess(
            shared_mem, texture, &begin_desc);
        if (st != WGPUStatus_Success) {
            wgpuTextureRelease(texture);
            wgpuSharedTextureMemoryRelease(shared_mem);
            CloseHandle(shared_handle);
            return -1;
        }
    }

    WFFPlatformDataWindows *pd = (WFFPlatformDataWindows *)
        calloc(1, sizeof(*pd));
    pd->shared_handle = shared_handle;

    uint32_t plane_count = (layout == WFF_PIXEL_LAYOUT_NV12 ||
                            layout == WFF_PIXEL_LAYOUT_P010) ? 2 : 1;

    out->texture       = texture;
    out->shared_mem    = shared_mem;
    out->fence         = NULL;
    out->pixel_layout  = layout;
    out->width         = (uint32_t)frame->width;
    out->height        = (uint32_t)frame->height;
    out->plane_count   = plane_count;
    out->platform_data = pd;
    return 0;
}

static void release_frame_windows(WFFBridge *bridge, WFFImportedFrame *frame)
{
    if (bridge->auto_access) {
        WGPUSharedTextureMemoryEndAccessState end_state =
            WGPU_SHARED_TEXTURE_MEMORY_END_ACCESS_STATE_INIT;
        wgpuSharedTextureMemoryEndAccess(
            frame->shared_mem, frame->texture, &end_state);
        wgpuSharedTextureMemoryEndAccessStateFreeMembers(end_state);
    }

    wgpuTextureDestroy(frame->texture);
    wgpuTextureRelease(frame->texture);
    wgpuSharedTextureMemoryRelease(frame->shared_mem);

    WFFPlatformDataWindows *pd =
        (WFFPlatformDataWindows *)frame->platform_data;
    if (pd) {
        if (pd->shared_handle) CloseHandle(pd->shared_handle);
        free(pd);
    }
}

#endif /* WFF_WINDOWS */

/* ====================================================================
 * Public dispatch
 * ==================================================================== */

int wff_bridge_import_frame(WFFBridge *bridge,
                            const AVFrame *frame,
                            WFFImportedFrame *out)
{
    if (!bridge || !frame || !out) return -1;
    memset(out, 0, sizeof(*out));

#if WFF_LINUX
    return import_frame_linux(bridge, frame, out);
#elif WFF_MACOS
    return import_frame_macos(bridge, frame, out);
#elif WFF_WINDOWS
    return import_frame_windows(bridge, frame, out);
#endif
}

void wff_imported_frame_release(WFFBridge *bridge,
                                WFFImportedFrame *frame)
{
    if (!bridge || !frame || !frame->texture) return;

#if WFF_LINUX
    release_frame_linux(bridge, frame);
#elif WFF_MACOS
    release_frame_macos(bridge, frame);
#elif WFF_WINDOWS
    release_frame_windows(bridge, frame);
#endif

    memset(frame, 0, sizeof(*frame));
}
