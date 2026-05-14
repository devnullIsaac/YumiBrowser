/**
 * @file gpu.h
 * @brief WebGPU rendering context for Yumi Browser.
 *
 * Manages a wgpu-native device, surface, and a simple immediate-mode
 * rectangle-drawing pipeline. Used as the base GPU context by all
 * binding layers (WebGL2, WGPU bindings, image/video decode).
 *
 * ## Example
 *
 * @code{.c}
 * #include "gpu.h"
 * #include <SDL3/SDL.h>
 *
 * SDL_Window *win = SDL_CreateWindow("App", 1280, 720, SDL_WINDOW_RESIZABLE);
 * GpuContext gpu;
 * gpu_init(&gpu, win);
 *
 * // Main loop
 * while (running) {
 *     gpu_set_clear_color(&gpu, 0.1f, 0.1f, 0.1f, 1.0f);
 *     if (gpu_frame_begin(&gpu)) {
 *         gpu_push_rect(&gpu, 10, 10, 200, 100, 1, 0, 0, 1);
 *         gpu_frame_end(&gpu);
 *     }
 * }
 * gpu_destroy(&gpu);
 * @endcode
 */

#ifndef GPU_H
#define GPU_H

#include "deps.h"

/** @brief Maximum number of vertices in the immediate-mode batch. */
#define GPU_MAX_VERTICES  (1 << 16)
/** @brief Floats per vertex: x, y, r, g, b, a. */
#define GPU_FLOATS_PER_VTX 6

/**
 * @brief GPU rendering context backed by wgpu-native.
 *
 * Holds the WebGPU instance, device, queue, surface, and a simple
 * coloured-rectangle pipeline with a dynamic vertex buffer.
 */
typedef struct {
    /* Core objects */
    WGPUInstance    instance;
    WGPUAdapter     adapter;
    WGPUDevice      device;
    WGPUQueue       queue;
    WGPUSurface     surface;

    /* Pipeline */
    WGPURenderPipeline pipeline;
    WGPUShaderModule   shader;

    /* Dynamic vertex buffer */
    WGPUBuffer vertex_buffer;
    float      vertices[GPU_MAX_VERTICES * GPU_FLOATS_PER_VTX];
    uint32_t   vertex_count;

    /* Surface state */
    uint32_t          width;
    uint32_t          height;
    WGPUTextureFormat format;
    WGPUSurfaceConfiguration surf_cfg;

    /* Per-frame clear color */
    float clear_color[4];
} GpuContext;

/**
 * @brief Initialize the GPU context from an SDL window.
 *
 * Creates the WebGPU instance, adapter, device, queue, surface,
 * shader module, pipeline, and vertex buffer.
 *
 * @param[out] ctx     Context to initialize.
 * @param[in]  window  SDL3 window providing the surface.
 * @return true on success, false if any GPU setup step fails.
 */
bool gpu_init(GpuContext *ctx, SDL_Window *window);

/**
 * @brief Resize the surface after a window resize event.
 *
 * @param[in,out] ctx  GPU context.
 * @param[in]     w    New width in pixels.
 * @param[in]     h    New height in pixels.
 */
void gpu_resize(GpuContext *ctx, uint32_t w, uint32_t h);

/**
 * @brief Begin a new frame.
 *
 * Acquires the next surface texture and creates a render pass.
 * Call gpu_push_rect() after this to queue geometry.
 *
 * @param[in,out] ctx  GPU context.
 * @return true if frame acquisition succeeded.
 */
bool gpu_frame_begin(GpuContext *ctx);

/**
 * @brief End the current frame and present.
 *
 * Uploads queued vertices, ends the render pass, submits the
 * command buffer, and presents the surface.
 *
 * @param[in,out] ctx  GPU context.
 */
void gpu_frame_end(GpuContext *ctx);

/**
 * @brief Destroy the GPU context and release all WebGPU resources.
 *
 * @param[in,out] ctx  GPU context to destroy.
 */
void gpu_destroy(GpuContext *ctx);

/**
 * @brief Set the per-frame clear colour.
 *
 * @param[in,out] ctx  GPU context.
 * @param[in]     r    Red   (0.0 – 1.0).
 * @param[in]     g    Green (0.0 – 1.0).
 * @param[in]     b    Blue  (0.0 – 1.0).
 * @param[in]     a    Alpha (0.0 – 1.0).
 */
void gpu_set_clear_color(GpuContext *ctx, float r, float g, float b, float a);

/**
 * @brief Push a solid-colour rectangle into the vertex batch.
 *
 * Coordinates are in pixels from top-left. Must be called between
 * gpu_frame_begin() and gpu_frame_end().
 *
 * @param[in,out] ctx  GPU context.
 * @param[in]     x    Left edge.
 * @param[in]     y    Top edge.
 * @param[in]     w    Width.
 * @param[in]     h    Height.
 * @param[in]     r    Red   (0.0 – 1.0).
 * @param[in]     g    Green (0.0 – 1.0).
 * @param[in]     b    Blue  (0.0 – 1.0).
 * @param[in]     a    Alpha (0.0 – 1.0).
 */
void gpu_push_rect(GpuContext *ctx,
                   float x, float y, float w, float h,
                   float r, float g, float b, float a);

/* ── Offscreen texture management ────────────────────────────────── */

/**
 * @brief Compositing pipeline state for blitting offscreen textures
 *        onto the swapchain as textured quads.
 */
typedef struct {
    WGPURenderPipeline  pipeline;
    WGPUShaderModule    shader;
    WGPUSampler         sampler;
    WGPUBindGroupLayout bind_group_layout;
    WGPUBuffer          vertex_buffer;  /**< Full-screen quad VB. */
    bool                initialized;
} GpuCompositor;

/**
 * @brief Initialize the compositing pipeline (lazy, call before first use).
 *
 * @param[out] comp  Compositor to initialize.
 * @param[in]  ctx   GPU context (provides device and format).
 * @return true on success.
 */
bool gpu_compositor_init(GpuCompositor *comp, GpuContext *ctx);

/**
 * @brief Destroy the compositing pipeline.
 * @param[in,out] comp  Compositor to destroy.
 */
void gpu_compositor_destroy(GpuCompositor *comp);

/**
 * @brief Composite an offscreen texture onto the current render pass.
 *
 * Draws a textured quad at the given viewport rectangle, sampling
 * from the provided offscreen texture view.
 *
 * @param[in] comp       Compositor pipeline.
 * @param[in] ctx        GPU context (for creating bind groups).
 * @param[in] pass       Active render pass encoder.
 * @param[in] tex_view   Offscreen texture view to sample.
 * @param[in] x          Left edge in pixels.
 * @param[in] y          Top edge in pixels.
 * @param[in] w          Width in pixels.
 * @param[in] h          Height in pixels.
 * @param[in] target_w   Full render target width.
 * @param[in] target_h   Full render target height.
 */
void gpu_composite_quad(const GpuCompositor *comp,
                        GpuContext *ctx,
                        WGPURenderPassEncoder pass,
                        WGPUTextureView tex_view,
                        float x, float y, float w, float h,
                        float target_w, float target_h);

/**
 * @brief Create an offscreen render-target texture.
 *
 * Allocates a WGPUTexture with RenderAttachment | TextureBinding |
 * CopySrc usage so webapps can render to it and the dashboard can
 * composite it onto the swapchain.
 *
 * @param[in]  ctx       GPU context (provides device and format).
 * @param[in]  w         Width in pixels.
 * @param[in]  h         Height in pixels.
 * @param[out] out_tex   Receives the new texture.
 * @param[out] out_view  Receives a view of the new texture.
 * @return true on success.
 */
bool gpu_create_offscreen_texture(GpuContext *ctx,
                                  uint32_t w, uint32_t h,
                                  WGPUTexture *out_tex,
                                  WGPUTextureView *out_view);

/**
 * @brief Resize an offscreen texture (destroy + recreate).
 *
 * @param[in]     ctx       GPU context.
 * @param[in]     w         New width.
 * @param[in]     h         New height.
 * @param[in,out] tex       Pointer to texture (updated in place).
 * @param[in,out] view      Pointer to view (updated in place).
 * @return true on success.
 */
bool gpu_resize_offscreen_texture(GpuContext *ctx,
                                  uint32_t w, uint32_t h,
                                  WGPUTexture *tex,
                                  WGPUTextureView *view);

/**
 * @brief Destroy an offscreen texture and its view.
 *
 * @param[in,out] tex   Texture to destroy (set to NULL).
 * @param[in,out] view  View to destroy (set to NULL).
 */
void gpu_destroy_offscreen_texture(WGPUTexture *tex, WGPUTextureView *view);

#endif
