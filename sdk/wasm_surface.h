/*
    Yumi SDK — 2D/3D GPU Surface Compositing Layer
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
 * @file wasm_surface.h
 * @brief 2D/3D GPU surface compositing layer over WebGPU.
 *
 * @details
 * This header provides a high-level GPU surface API built on top of the
 * low-level WebGPU bindings in `wasm_gpu.h`. It handles:
 *
 *   - **Offscreen surfaces**: Double-buffered textures for intermediate compositing
 *   - **Swapchain surface**: The final display target
 *   - **Blend modes**: 31 blend modes including hardware-blendable (normal,
 *     add, replace) and shader-based (multiply, screen, overlay, etc.)
 *   - **3D compositing**: Depth-based occlusion for mixing 2D UI with 3D content
 *   - **Ping-pong rendering**: Automatic A/B texture swapping for shader blends
 *
 * ## Surface Types
 * | Type | Purpose |
 * |------|---------|
 * | GPU_SURFACE_TYPE_OFFSCREEN | Intermediate render target for layer content |
 * | GPU_SURFACE_TYPE_SWAPCHAIN | Final display output, presented to screen |
 *
 * ## Typical Compositing Flow
 * @code
 *   GPUSurface_init();
 *
 *   GPUSurface bg = GPUSurface_create(800, 600);
 *   GPUSurface ui = GPUSurface_create(800, 600);
 *
 *   GPUSurface_begin_ex(bg, GPU_PASS_CLEAR, GPU_COLOR_BLACK);
 *     // draw 3D scene or background...
 *   GPUSurface_end(bg);
 *
 *   GPUSurface_begin_ex(ui, GPU_PASS_CLEAR, GPU_COLOR_TRANSPARENT);
 *     // draw UI widgets...
 *   GPUSurface_end(ui);
 *
 *   GPUSurface dst = GPUSurface_bind_swapchain();
 *   GPUSurface_clear_color(dst, GPU_COLOR_BLACK);
 *   GPUSurface_compose(bg, dst);                          // bg → screen
 *   GPUSurface_blit(ui, NULL, dst, NULL, GPU_BLEND_NORMAL, 1.0f); // ui overlay
 *   GPUSurface_present(dst);
 * @endcode
 *
 * @see wasm_gpu.h for the underlying WebGPU primitives.
 */

#ifndef GPU_SURFACE_H
#define GPU_SURFACE_H

#include "wasm_gpu.h"
#include <stdint.h>

/* ================================================================== */
/*  GPUSurface — Layer compositing over WGPU with blend modes          */
/*                                                                     */
/*  Architecture:                                                      */
/*    - Every surface owns two textures (A + B) for ping-pong          */
/*    - tex_a is always the authoritative content                      */
/*    - tex_b is scratch for shader-blend passes                       */
/*    - Blit is self-contained: creates encoder, draws, submits        */
/*    - HW blends (normal/add/replace) draw src onto dst.tex_a         */
/*    - Shader blends sample both src.tex_a + dst.tex_a,               */
/*      render into dst.tex_b, then copy the rect back to dst.tex_a    */
/*    - Swapchain surface composes offscreen, present copies to screen */
/*                                                                     */
/*  Usage:                                                             */
/*                                                                     */
/*    GPUSurface_init();                                               */
/*                                                                     */
/*    GPUSurface bg = GPUSurface_create(800, 600);                     */
/*    GPUSurface ui = GPUSurface_create(800, 600);                     */
/*                                                                     */
/*    GPUSurface_begin_ex(bg, GPU_PASS_CLEAR, GPU_COLOR_BLACK);        */
/*      // draw scene...                                               */
/*    GPUSurface_end(bg);                                              */
/*                                                                     */
/*    GPUSurface_begin_ex(ui, GPU_PASS_CLEAR, GPU_COLOR_TRANSPARENT);  */
/*      // draw ui...                                                  */
/*    GPUSurface_end(ui);                                              */
/*                                                                     */
/*    GPUSurface dst = GPUSurface_bind_swapchain();                    */
/*    GPUSurface_clear_color(dst, GPU_COLOR_BLACK);                    */
/*    GPUSurface_compose(bg, dst);                                     */
/*    GPUSurface_blit(ui, NULL, dst, NULL, GPU_BLEND_SCREEN, 0.8f);   */
/*    GPUSurface_present(dst);                                        */
/*                                                                     */
/*  3D compositing (depth-based occlusion):                            */
/*                                                                     */
/*    GPUSurface_begin_ex(dst, GPU_PASS_3D, GPU_COLOR_BLACK);          */
/*      GPUSurface_draw_occluder(dst, &rect, 0.0f);  // near          */
/*      my_backdrop_draw(backdrop, dst);              // at Z=1 (far)  */
/*    GPUSurface_end(dst);                                             */
/*    GPUSurface_compose(ui, dst);                                     */
/*    GPUSurface_present(dst);                                        */
/* ================================================================== */

#define GPU_SURFACE_MAX         512
#define GPU_TEX_FORMAT_Undefined 0x00000000
#define GPU_TEX_FORMAT_R8Unorm 0x00000001
#define GPU_TEX_FORMAT_R8Snorm 0x00000002
#define GPU_TEX_FORMAT_R8Uint 0x00000003
#define GPU_TEX_FORMAT_R8Sint 0x00000004
#define GPU_TEX_FORMAT_R16Unorm 0x00000005
#define GPU_TEX_FORMAT_R16Snorm 0x00000006
#define GPU_TEX_FORMAT_R16Uint 0x00000007
#define GPU_TEX_FORMAT_R16Sint 0x00000008
#define GPU_TEX_FORMAT_R16Float 0x00000009
#define GPU_TEX_FORMAT_RG8Unorm 0x0000000A
#define GPU_TEX_FORMAT_RG8Snorm 0x0000000B
#define GPU_TEX_FORMAT_RG8Uint 0x0000000C
#define GPU_TEX_FORMAT_RG8Sint 0x0000000D
#define GPU_TEX_FORMAT_R32Float 0x0000000E
#define GPU_TEX_FORMAT_R32Uint 0x0000000F
#define GPU_TEX_FORMAT_R32Sint 0x00000010
#define GPU_TEX_FORMAT_RG16Unorm 0x00000011
#define GPU_TEX_FORMAT_RG16Snorm 0x00000012
#define GPU_TEX_FORMAT_RG16Uint 0x00000013
#define GPU_TEX_FORMAT_RG16Sint 0x00000014
#define GPU_TEX_FORMAT_RG16Float 0x00000015
#define GPU_TEX_FORMAT_RGBA8Unorm 0x00000016
#define GPU_TEX_FORMAT_RGBA8UnormSrgb 0x00000017
#define GPU_TEX_FORMAT_RGBA8Snorm 0x00000018
#define GPU_TEX_FORMAT_RGBA8Uint 0x00000019
#define GPU_TEX_FORMAT_RGBA8Sint 0x0000001A
#define GPU_TEX_FORMAT_BGRA8Unorm 0x0000001B
#define GPU_TEX_FORMAT_BGRA8UnormSrgb 0x0000001C
#define GPU_TEX_FORMAT_RGB10A2Uint 0x0000001D
#define GPU_TEX_FORMAT_RGB10A2Unorm 0x0000001E
#define GPU_TEX_FORMAT_RG11B10Ufloat 0x0000001F
#define GPU_TEX_FORMAT_RGB9E5Ufloat 0x00000020
#define GPU_TEX_FORMAT_RG32Float 0x00000021
#define GPU_TEX_FORMAT_RG32Uint 0x00000022
#define GPU_TEX_FORMAT_RG32Sint 0x00000023
#define GPU_TEX_FORMAT_RGBA16Unorm 0x00000024
#define GPU_TEX_FORMAT_RGBA16Snorm 0x00000025
#define GPU_TEX_FORMAT_RGBA16Uint 0x00000026
#define GPU_TEX_FORMAT_RGBA16Sint 0x00000027
#define GPU_TEX_FORMAT_RGBA16Float 0x00000028
#define GPU_TEX_FORMAT_RGBA32Float 0x00000029
#define GPU_TEX_FORMAT_RGBA32Uint 0x0000002A
#define GPU_TEX_FORMAT_RGBA32Sint 0x0000002B
#define GPU_TEX_FORMAT_Stencil8 0x0000002C
#define GPU_TEX_FORMAT_Depth16Unorm 0x0000002D
#define GPU_TEX_FORMAT_Depth24Plus 0x0000002E
#define GPU_TEX_FORMAT_Depth24PlusStencil8 0x0000002F
#define GPU_TEX_FORMAT_Depth32Float 0x00000030
#define GPU_TEX_FORMAT_Depth32FloatStencil8 0x00000031
#define GPU_TEX_FORMAT_BC1RGBAUnorm 0x00000032
#define GPU_TEX_FORMAT_BC1RGBAUnormSrgb 0x00000033
#define GPU_TEX_FORMAT_BC2RGBAUnorm 0x00000034
#define GPU_TEX_FORMAT_BC2RGBAUnormSrgb 0x00000035
#define GPU_TEX_FORMAT_BC3RGBAUnorm 0x00000036
#define GPU_TEX_FORMAT_BC3RGBAUnormSrgb 0x00000037
#define GPU_TEX_FORMAT_BC4RUnorm 0x00000038
#define GPU_TEX_FORMAT_BC4RSnorm 0x00000039
#define GPU_TEX_FORMAT_BC5RGUnorm 0x0000003A
#define GPU_TEX_FORMAT_BC5RGSnorm 0x0000003B
#define GPU_TEX_FORMAT_BC6HRGBUfloat 0x0000003C
#define GPU_TEX_FORMAT_BC6HRGBFloat 0x0000003D
#define GPU_TEX_FORMAT_BC7RGBAUnorm 0x0000003E
#define GPU_TEX_FORMAT_BC7RGBAUnormSrgb 0x0000003F
#define GPU_TEX_FORMAT_ETC2RGB8Unorm 0x00000040
#define GPU_TEX_FORMAT_ETC2RGB8UnormSrgb 0x00000041
#define GPU_TEX_FORMAT_ETC2RGB8A1Unorm 0x00000042
#define GPU_TEX_FORMAT_ETC2RGB8A1UnormSrgb 0x00000043
#define GPU_TEX_FORMAT_ETC2RGBA8Unorm 0x00000044
#define GPU_TEX_FORMAT_ETC2RGBA8UnormSrgb 0x00000045
#define GPU_TEX_FORMAT_EACR11Unorm 0x00000046
#define GPU_TEX_FORMAT_EACR11Snorm 0x00000047
#define GPU_TEX_FORMAT_EACRG11Unorm 0x00000048
#define GPU_TEX_FORMAT_EACRG11Snorm 0x00000049
#define GPU_TEX_FORMAT_ASTC4x4Unorm 0x0000004A
#define GPU_TEX_FORMAT_ASTC4x4UnormSrgb 0x0000004B
#define GPU_TEX_FORMAT_ASTC5x4Unorm 0x0000004C
#define GPU_TEX_FORMAT_ASTC5x4UnormSrgb 0x0000004D
#define GPU_TEX_FORMAT_ASTC5x5Unorm 0x0000004E
#define GPU_TEX_FORMAT_ASTC5x5UnormSrgb 0x0000004F
#define GPU_TEX_FORMAT_ASTC6x5Unorm 0x00000050
#define GPU_TEX_FORMAT_ASTC6x5UnormSrgb 0x00000051
#define GPU_TEX_FORMAT_ASTC6x6Unorm 0x00000052
#define GPU_TEX_FORMAT_ASTC6x6UnormSrgb 0x00000053
#define GPU_TEX_FORMAT_ASTC8x5Unorm 0x00000054
#define GPU_TEX_FORMAT_ASTC8x5UnormSrgb 0x00000055
#define GPU_TEX_FORMAT_ASTC8x6Unorm 0x00000056
#define GPU_TEX_FORMAT_ASTC8x6UnormSrgb 0x00000057
#define GPU_TEX_FORMAT_ASTC8x8Unorm 0x00000058
#define GPU_TEX_FORMAT_ASTC8x8UnormSrgb 0x00000059
#define GPU_TEX_FORMAT_ASTC10x5Unorm 0x0000005A
#define GPU_TEX_FORMAT_ASTC10x5UnormSrgb 0x0000005B
#define GPU_TEX_FORMAT_ASTC10x6Unorm 0x0000005C
#define GPU_TEX_FORMAT_ASTC10x6UnormSrgb 0x0000005D
#define GPU_TEX_FORMAT_ASTC10x8Unorm 0x0000005E
#define GPU_TEX_FORMAT_ASTC10x8UnormSrgb 0x0000005F
#define GPU_TEX_FORMAT_ASTC10x10Unorm 0x00000060
#define GPU_TEX_FORMAT_ASTC10x10UnormSrgb 0x00000061
#define GPU_TEX_FORMAT_ASTC12x10Unorm 0x00000062
#define GPU_TEX_FORMAT_ASTC12x10UnormSrgb 0x00000063
#define GPU_TEX_FORMAT_ASTC12x12Unorm 0x00000064
#define GPU_TEX_FORMAT_ASTC12x12UnormSrgb 0x00000065
#define GPU_TEX_FORMAT_R8BG8Biplanar420Unorm 0x00050000
#define GPU_TEX_FORMAT_R10X6BG10X6Biplanar420Unorm 0x00050001
#define GPU_TEX_FORMAT_R8BG8A8Triplanar420Unorm 0x00050002
#define GPU_TEX_FORMAT_R8BG8Biplanar422Unorm 0x00050003
#define GPU_TEX_FORMAT_R8BG8Biplanar444Unorm 0x00050004
#define GPU_TEX_FORMAT_R10X6BG10X6Biplanar422Unorm 0x00050005
#define GPU_TEX_FORMAT_R10X6BG10X6Biplanar444Unorm 0x00050006
#define GPU_TEX_FORMAT_OpaqueYCbCrAndroid 0x00050007
#define GPU_TEX_FORMAT_Force32 0x7FFFFFFF

/* ================================================================== */
/*  Blend modes                                                        */
/* ================================================================== */

typedef enum GPU_BLEND_MODE {
    /* ---- HW blendable ---- */
    GPU_BLEND_NORMAL        = 0,
    GPU_BLEND_REPLACE       = 1,
    GPU_BLEND_ADDITIVE      = 2,

    /* ---- Shader blended (LYGIA functions) ---- */
    GPU_BLEND_MULTIPLY      = 3,
    GPU_BLEND_SCREEN        = 4,
    GPU_BLEND_OVERLAY       = 5,
    GPU_BLEND_DARKEN        = 6,
    GPU_BLEND_LIGHTEN       = 7,
    GPU_BLEND_COLOR_DODGE   = 8,
    GPU_BLEND_COLOR_BURN    = 9,
    GPU_BLEND_HARD_LIGHT    = 10,
    GPU_BLEND_SOFT_LIGHT    = 11,
    GPU_BLEND_DIFFERENCE    = 12,
    GPU_BLEND_EXCLUSION     = 13,
    GPU_BLEND_ADD           = 14,
    GPU_BLEND_SUBTRACT      = 15,
    GPU_BLEND_AVERAGE       = 16,
    GPU_BLEND_NEGATION      = 17,
    GPU_BLEND_LINEAR_BURN   = 18,
    GPU_BLEND_LINEAR_DODGE  = 19,
    GPU_BLEND_LINEAR_LIGHT  = 20,
    GPU_BLEND_VIVID_LIGHT   = 21,
    GPU_BLEND_PIN_LIGHT     = 22,
    GPU_BLEND_HARD_MIX      = 23,
    GPU_BLEND_REFLECT       = 24,
    GPU_BLEND_GLOW          = 25,
    GPU_BLEND_PHOENIX       = 26,
    GPU_BLEND_HUE           = 27,
    GPU_BLEND_SATURATION    = 28,
    GPU_BLEND_COLOR         = 29,
    GPU_BLEND_LUMINOSITY    = 30,

    /* Hardware-blended composition for ALREADY-premultiplied sources.
     *   color: ONE * src + (1 - src.a) * dst
     *   alpha: ONE * src + (1 - src.a) * dst
     * Use this for surface->surface composition: shader draws onto an
     * offscreen surface using GPU_BLEND_NORMAL accumulate the result
     * in premultiplied form, so blitting that surface onward must
     * NOT multiply RGB by alpha a second time.  Compose helpers
     * (GPUSurface_compose / _compose_at) use this mode internally. */
    GPU_BLEND_PREMULTIPLIED = 31,

    GPU_BLEND_COUNT         = 32,
    GPU_BLEND_HW_MAX        = 3   /* indices 0..2 packed in g_pipe_hw[] */
} GPU_BLEND_MODE;

/* ================================================================== */
/*  Rectangle (for crop / placement)                                   */
/* ================================================================== */

typedef struct {
    int x, y, w, h;
} gpu_rect_t;

/* ================================================================== */
/*  Color type                                                         */
/* ================================================================== */

typedef struct { float r, g, b, a; } gpu_color_t;

#define GPU_COLOR(r_,g_,b_,a_)   ((gpu_color_t){(r_),(g_),(b_),(a_)})
#define GPU_COLOR_BLACK          GPU_COLOR(0,0,0,1)
#define GPU_COLOR_WHITE          GPU_COLOR(1,1,1,1)
#define GPU_COLOR_TRANSPARENT    GPU_COLOR(0,0,0,0)

/* ================================================================== */
/*  Render-pass mode (unified begin)                                   */
/* ================================================================== */

typedef enum gpu_pass_mode {
    GPU_PASS_CLEAR    = 0,   /**< 2D pass, clear color+alpha          */
    GPU_PASS_LOAD     = 1,   /**< 2D pass, preserve existing content  */
    GPU_PASS_3D       = 2,   /**< 3D pass (depth), clear              */
    GPU_PASS_3D_LOAD  = 3,   /**< 3D pass (depth), preserve           */
} gpu_pass_mode_t;

/* ================================================================== */
/*  Pipeline descriptor (struct-based creation)                        */
/* ================================================================== */

typedef struct {
    int enabled;
    int color_src, color_dst, color_op;
    int alpha_src, alpha_dst, alpha_op;
} gpu_blend_desc_t;

typedef struct {
    int write;
    int compare;
} gpu_depth_desc_t;

typedef struct {
    gpu_pipeline_layout_t layout;
    gpu_shader_t          vs;
    const char           *vs_entry;   /**< NULL → "main" */
    gpu_shader_t          fs;
    const char           *fs_entry;   /**< NULL → "main" */
    int                   format;     /**< 0 → offscreen default */
    gpu_blend_desc_t      blend;
    gpu_depth_desc_t      depth;
} gpu_pipeline_desc_t;

/* ================================================================== */
/*  Surface types                                                      */
/* ================================================================== */

typedef enum GPU_SURFACE_TYPE {
    GPU_SURFACE_TYPE_INVALID   = 0,
    GPU_SURFACE_TYPE_OFFSCREEN = 1,
    GPU_SURFACE_TYPE_SWAPCHAIN = 2
} GPU_SURFACE_TYPE;

/* ================================================================== */
/*  Blit uniform layout (must match shader struct exactly)             */
/* ================================================================== */

typedef struct __attribute__((packed, aligned(4))) {
    float    src_uv[4];
    float    dst_size[2];
    float    opacity;
    uint32_t blend_mode;
} gpu_blit_uniforms_t;

/* ================================================================== */
/*  Depth format used for 3D compositing                               */
/* ================================================================== */

#define GPU_SURFACE_DEPTH_FORMAT GPU_TEX_FORMAT_Depth24Plus

/* ================================================================== */
/*  Surface struct                                                     */
/* ================================================================== */

typedef struct GPUSurface_s {
    GPU_SURFACE_TYPE       type;

    gpu_texture_t          tex_a;
    gpu_texture_view_t     view_a;
    gpu_texture_t          tex_b;
    gpu_texture_view_t     view_b;

    gpu_texture_t          depth_tex;
    gpu_texture_view_t     depth_view;

    gpu_texture_view_t     swapchain_view;

    gpu_render_pass_t      renderPass;
    gpu_command_encoder_t  encoder;
    gpu_color_attachment_t colorAttachment;
    gpu_render_pass_desc_t renderPassDesc;

    uint32_t               width;
    uint32_t               height;
    uint32_t               alloc_width;
    uint32_t               alloc_height;
    int                    passActive;
    int                    depth_enabled;
} *GPUSurface;

/* ================================================================== */
/*  Lifecycle                                                          */
/* ================================================================== */

/**
 * @brief Initialize the GPUSurface subsystem.
 *
 * @details
 * Must be called once before any other GPUSurface function. Sets up
 * internal state, caches the WebGPU device/queue handles, and prepares
 * shared resources (full-screen quad buffer, default sampler).
 */
void       GPUSurface_init(void);

/**
 * @brief Create an offscreen rendering surface.
 *
 * @details
 * Allocates a double-buffered offscreen surface with the given dimensions.
 * The surface owns two RGBA8 textures (A and B) for ping-pong rendering
 * during blend operations.
 *
 * @param[in] width   Surface width in pixels.
 * @param[in] height  Surface height in pixels.
 * @return A valid GPUSurface handle, or NULL on failure.
 */
GPUSurface GPUSurface_create(uint32_t width, uint32_t height);

/**
 * @brief Bind to the swapchain surface (display output).
 *
 * @details
 * Returns a special GPUSurface that represents the current back-buffer
 * of the window swapchain. This surface is ephemeral — it must be used
 * and presented within the same frame. Do not destroy it.
 *
 * @return A swapchain GPUSurface handle.
 */
GPUSurface GPUSurface_bind_swapchain(void);

/**
 * @brief Destroy a surface and release all GPU resources.
 *
 * @details
 * Releases textures, texture views, depth buffers, and any internally
 * allocated command encoders. After this call the handle is invalid.
 *
 * @param[in] surface  The surface to destroy.
 */
void       GPUSurface_destroy(GPUSurface surface);

/**
 * @brief Resize an offscreen surface.
 *
 * @details
 * Reallocates the internal textures to match the new dimensions.
 * Existing content is discarded. This is typically called in response
 * to a window resize event.
 *
 * @param[in] surface  Valid offscreen surface.
 * @param[in] w        New width in pixels.
 * @param[in] h        New height in pixels.
 */
void       GPUSurface_resize(GPUSurface surface, uint32_t w, uint32_t h);

/* ---- Scratch pool (transient per-frame surfaces) ---- */

/**
 * @brief Acquire a transient scratch surface from the pool.
 *
 * @details
 * Returns a recycled offscreen surface from an internal pool, creating
 * a new one if necessary. Scratch surfaces are intended for temporary
 * intermediate results within a single frame (e.g. blur passes).
 *
 * @param[in] width   Desired width in pixels.
 * @param[in] height  Desired height in pixels.
 * @return A scratch GPUSurface handle.
 */
GPUSurface GPUSurface_acquire_scratch(uint32_t width, uint32_t height);

/**
 * @brief Return a scratch surface to the pool.
 *
 * @details
 * Releases the scratch surface back to the internal pool for reuse.
 * The surface handle becomes invalid after this call.
 *
 * @param[in] surface  Scratch surface to release.
 */
void       GPUSurface_release_scratch(GPUSurface surface);

/* ================================================================== */
/*  Clear                                                              */
/* ================================================================== */

/**
 * @brief Clear a surface to a specific RGBA color.
 *
 * @details
 * Fills the entire surface with the given color, overwriting any
 * existing content. This does not begin or end a render pass.
 *
 * @param[in] surface  Target surface.
 * @param[in] r        Red component [0, 1].
 * @param[in] g        Green component [0, 1].
 * @param[in] b        Blue component [0, 1].
 * @param[in] a        Alpha component [0, 1].
 */
void GPUSurface_clear(GPUSurface surface,
                      float r, float g, float b, float a);

/* ================================================================== */
/*  Render pass — 2D (no depth, original API)                          */
/* ================================================================== */

/**
 * @brief Begin a 2D render pass with a clear color.
 *
 * @details
 * Starts recording rendering commands for the surface. The surface
 * content is cleared to the specified RGBA color. All subsequent
 * draw calls target this surface until GPUSurface_end() is called.
 *
 * @param[in] surface  Target surface.
 * @param[in] r        Clear color red component [0, 1].
 * @param[in] g        Clear color green component [0, 1].
 * @param[in] b        Clear color blue component [0, 1].
 * @param[in] a        Clear color alpha component [0, 1].
 */
void GPUSurface_begin(GPUSurface surface,
                      float r, float g, float b, float a);

/**
 * @brief Begin a 2D render pass preserving existing content.
 *
 * @details
 * Like GPUSurface_begin(), but the existing surface content is preserved
 * rather than cleared. New draw calls composite on top.
 *
 * @param[in] surface  Target surface.
 */
void GPUSurface_begin_load(GPUSurface surface);

/**
 * @brief End the current render pass and submit commands.
 *
 * @details
 * Finalizes the render pass, submits the encoded commands to the GPU
 * queue, and releases internal encoder resources. Must be called to
 * make draw calls visible.
 *
 * @param[in] surface  Surface whose render pass is ending.
 */
void GPUSurface_end(GPUSurface surface);

/* ================================================================== */
/*  Render pass — 3D (depth buffer attached)                           */
/* ================================================================== */

/**
 * @brief Begin a 3D render pass with depth clearing.
 *
 * @details
 * Starts a render pass with an attached depth buffer. Both color and
 * depth are cleared: color to the given RGBA, depth to 1.0 (far plane).
 * Use this when rendering 3D content that needs depth testing.
 *
 * @param[in] surface  Target surface.
 * @param[in] r        Clear color red component [0, 1].
 * @param[in] g        Clear color green component [0, 1].
 * @param[in] b        Clear color blue component [0, 1].
 * @param[in] a        Clear color alpha component [0, 1].
 */
void GPUSurface_begin_3d(GPUSurface surface,
                         float r, float g, float b, float a);

/**
 * @brief Begin a 3D render pass preserving color and depth.
 *
 * @details
 * Like GPUSurface_begin_3d(), but preserves both existing color and
 * depth buffer contents. Useful for layering multiple 3D passes.
 *
 * @param[in] surface  Target surface.
 */
void GPUSurface_begin_3d_load(GPUSurface surface);

/* ================================================================== */
/*  Drawing into a surface (between begin/end)                         */
/* ================================================================== */

/**
 * @brief Draw with a pipeline and bind group using the full-screen quad.
 *
 * @details
 * Issues a draw call using the internal full-screen quad vertex buffer.
 * The pipeline's vertex shader should expect a simple 2D position attribute.
 * This is the standard path for 2D sprite/rect rendering.
 *
 * @param[in] surface     Active surface (between begin/end).
 * @param[in] pipeline    Compiled render pipeline.
 * @param[in] bind_group  Bind group with textures, samplers, and uniforms.
 */
void GPUSurface_draw_pipeline(
    GPUSurface surface,
    gpu_render_pipeline_t pipeline,
    gpu_bind_group_t bind_group);

/**
 * @brief Draw with explicit vertex buffer and pipeline.
 *
 * @details
 * Issues a draw call using a custom vertex buffer instead of the
 * full-screen quad. Use this for arbitrary geometry (meshes, lines,
 * custom quads).
 *
 * @param[in] surface       Active surface (between begin/end).
 * @param[in] pipeline      Compiled render pipeline.
 * @param[in] bind_group    Bind group with resources.
 * @param[in] vertex_buf    GPU buffer containing vertex data.
 * @param[in] vertex_count  Number of vertices to draw.
 * @param[in] vertex_stride Byte stride between vertices.
 */
void GPUSurface_draw_pipeline_ex(
    GPUSurface surface,
    gpu_render_pipeline_t pipeline,
    gpu_bind_group_t bind_group,
    gpu_buffer_t vertex_buf,
    int vertex_count,
    int vertex_stride);

/* ================================================================== */
/*  Depth-based occlusion                                              */
/* ================================================================== */

/**
 * @brief Stamp a screen-space rectangle into the depth buffer.
 *
 * @details
 * Renders a fullscreen quad with depth write enabled but color write
 * disabled, priming the depth buffer at the specified depth value.
 * This is used for 2D/3D compositing: draw occluders at depth=0
 * (near), then draw 3D content at depth=1 (far). Subsequent 2D draws
 * at depth=0 will naturally occlude the 3D backdrop where they overlap.
 *
 * depth=0 is near (wins depth test), depth=1 is far (loses depth test).
 *
 * Must be called inside a 3D render pass (begin_3d / begin_3d_load).
 *
 * @param[in] surface  Active 3D surface.
 * @param[in] rect     Screen-space rectangle to occlude (NULL = full surface).
 * @param[in] depth    Depth value [0, 1] to write.
 */
void GPUSurface_draw_occluder(GPUSurface surface,
                              const gpu_rect_t *rect, float depth);

/* ================================================================== */
/*  Pipeline creation — 2D (no depth)                                  */
/* ================================================================== */

/**
 * @brief Create a 2D render pipeline.
 *
 * @details
 * Compiles a render pipeline for 2D rasterization with optional alpha
 * blending but no depth testing. For a simpler interface, see
 * GPUSurface_pipeline() which takes a descriptor struct.
 *
 * @param[in] layout       Pipeline layout handle.
 * @param[in] vs           Vertex shader module.
 * @param[in] vs_entry     Vertex shader entry point name (NULL = "main").
 * @param[in] fs           Fragment shader module.
 * @param[in] fs_entry     Fragment shader entry point name (NULL = "main").
 * @param[in] target_fmt   Output color format (0 = default offscreen format).
 * @param[in] blend_enabled 1 to enable alpha blending, 0 for opaque.
 * @param[in] color_src    Color blend source factor (GPU_BLEND_*).
 * @param[in] color_dst    Color blend destination factor.
 * @param[in] color_op     Color blend operation (GPU_BLEND_OP_*).
 * @param[in] alpha_src    Alpha blend source factor.
 * @param[in] alpha_dst    Alpha blend destination factor.
 * @param[in] alpha_op     Alpha blend operation.
 * @return A render pipeline handle, or 0 on failure.
 */
gpu_render_pipeline_t GPUSurface_create_pipeline(
    gpu_pipeline_layout_t layout,
    gpu_shader_t vs, const char *vs_entry,
    gpu_shader_t fs, const char *fs_entry,
    int target_fmt,
    int blend_enabled,
    int color_src, int color_dst, int color_op,
    int alpha_src, int alpha_dst, int alpha_op);

/* ================================================================== */
/*  Pipeline creation — 3D (with depth)                                */
/* ================================================================== */

/**
 * @brief Create a 3D render pipeline with depth testing.
 *
 * @details
 * Compiles a render pipeline with optional alpha blending and depth
 * testing/stenciling. All parameters from GPUSurface_create_pipeline()
 * apply, plus depth-specific state.
 *
 * @param[in] layout        Pipeline layout handle.
 * @param[in] vs            Vertex shader module.
 * @param[in] vs_entry      Vertex shader entry point name.
 * @param[in] fs            Fragment shader module.
 * @param[in] fs_entry      Fragment shader entry point name.
 * @param[in] target_fmt    Output color format.
 * @param[in] blend_enabled 1 to enable alpha blending.
 * @param[in] color_src     Color blend source factor.
 * @param[in] color_dst     Color blend destination factor.
 * @param[in] color_op      Color blend operation.
 * @param[in] alpha_src     Alpha blend source factor.
 * @param[in] alpha_dst     Alpha blend destination factor.
 * @param[in] alpha_op      Alpha blend operation.
 * @param[in] depth_write   1 to enable depth writes.
 * @param[in] depth_compare Depth comparison function (GPU_COMPARE_*).
 * @return A render pipeline handle, or 0 on failure.
 */
gpu_render_pipeline_t GPUSurface_create_pipeline_depth(
    gpu_pipeline_layout_t layout,
    gpu_shader_t vs, const char *vs_entry,
    gpu_shader_t fs, const char *fs_entry,
    int target_fmt,
    int blend_enabled,
    int color_src, int color_dst, int color_op,
    int alpha_src, int alpha_dst, int alpha_op,
    int depth_write,
    int depth_compare);

/* ================================================================== */
/*  Blit / Composite                                                   */
/* ================================================================== */

/**
 * @brief Blit one surface onto another with a blend mode.
 *
 * @details
 * Copies a rectangular region from @p src to @p dst using the specified
 * blend mode and opacity. If @p src_rect is NULL, the entire src surface
 * is used. If @p dst_rect is NULL, the blit fills the entire dst surface.
 *
 * For hardware-blendable modes (NORMAL, ADDITIVE, REPLACE), this uses
 * a single draw call. For shader blend modes, it performs a ping-pong
 * pass internally.
 *
 * @param[in] src        Source surface.
 * @param[in] src_rect   Source crop rectangle, or NULL for full surface.
 * @param[in] dst        Destination surface.
 * @param[in] dst_rect   Destination placement rectangle, or NULL for full surface.
 * @param[in] blend_mode Blend mode to apply.
 * @param[in] opacity    Overall opacity multiplier [0, 1].
 */
void GPUSurface_blit(
    GPUSurface          src,
    const gpu_rect_t   *src_rect,
    GPUSurface          dst,
    const gpu_rect_t   *dst_rect,
    GPU_BLEND_MODE      blend_mode,
    float               opacity);

/* ================================================================== */
/*  Present (swapchain surfaces only)                                  */
/* ================================================================== */

/**
 * @brief Present the swapchain surface to the display.
 *
 * @details
 * Submits all pending GPU work and presents the current swapchain image
 * to the screen. This is the final call in a frame render loop. Only
 * valid on surfaces obtained from GPUSurface_bind_swapchain().
 *
 * @param[in] surface  Swapchain surface.
 */
void GPUSurface_present(GPUSurface surface);

/* ================================================================== */
/*  Accessors                                                          */
/* ================================================================== */

/**
 * @brief Get the shared full-screen quad vertex buffer.
 * @return A GPU buffer handle containing a 4-vertex fullscreen quad.
 */
gpu_buffer_t       GPUSurface_get_quad_buffer(void);

/**
 * @brief Get the shared default texture sampler.
 * @return A GPU sampler handle with linear filtering and clamp-to-edge.
 */
gpu_sampler_t      GPUSurface_get_sampler(void);

/**
 * @brief Get the cached WebGPU device handle.
 * @return The gpu_device_t used by all surfaces.
 */
gpu_device_t       GPUSurface_get_device(void);

/**
 * @brief Get the swapchain surface format.
 * @return The WGPUTextureFormat of the swapchain (e.g. BGRA8Unorm).
 */
int                GPUSurface_get_surface_format(void);

/**
 * @brief Get a surface's logical width.
 * @param[in] surface  Valid surface.
 * @return Width in pixels.
 */
uint32_t           GPUSurface_get_width(GPUSurface surface);

/**
 * @brief Get a surface's logical height.
 * @param[in] surface  Valid surface.
 * @return Height in pixels.
 */
uint32_t           GPUSurface_get_height(GPUSurface surface);

/**
 * @brief Get the current render target texture view.
 * @param[in] surface  Valid surface.
 * @return A gpu_texture_view_t for the surface's current color attachment.
 */
gpu_texture_view_t GPUSurface_get_texture_view(GPUSurface surface);

/**
 * @brief Get the active render pass handle.
 * @param[in] surface  Valid surface with an active render pass.
 * @return The gpu_render_pass_t currently recording commands.
 */
gpu_render_pass_t  GPUSurface_get_render_pass(GPUSurface surface);

/**
 * @brief Get the default offscreen surface format.
 * @return Always GPU_TEX_FORMAT_RGBA8Unorm.
 */
static inline int GPUSurface_get_offscreen_format(void) { return GPU_TEX_FORMAT_RGBA8Unorm; }

/* ================================================================== */
/*  Humanized API — convenience functions                              */
/* ================================================================== */

/**
 * @brief Unified begin: replaces begin / begin_load / begin_3d / begin_3d_load.
 *
 * @param[in] surface      Target surface.
 * @param[in] mode         Render pass mode (clear, load, 3D, 3D_load).
 * @param[in] clear_color  Color to clear to (used for *_CLEAR modes).
 */
void GPUSurface_begin_ex(GPUSurface surface,
                         gpu_pass_mode_t mode,
                         gpu_color_t clear_color);

/**
 * @brief Clear with gpu_color_t.
 * @param[in] surface  Target surface.
 * @param[in] color    RGBA clear color.
 */
void GPUSurface_clear_color(GPUSurface surface, gpu_color_t color);

/**
 * @brief One-shot draw: begin_load → draw_pipeline → end.
 *
 * @details
 * Convenience wrapper that begins a load pass, draws the pipeline, and
 * ends the pass in a single call. Useful for simple fullscreen effects.
 *
 * @param[in] surface     Target surface.
 * @param[in] pipeline    Compiled render pipeline.
 * @param[in] bind_group  Bind group with resources.
 */
void GPUSurface_draw(GPUSurface surface,
                     gpu_render_pipeline_t pipeline,
                     gpu_bind_group_t bind_group);

/**
 * @brief Full-surface blit with GPU_BLEND_NORMAL at full opacity.
 *
 * @param[in] src  Source surface.
 * @param[in] dst  Destination surface.
 */
void GPUSurface_compose(GPUSurface src, GPUSurface dst);

/**
 * @brief Blit src into dst at dst_rect, GPU_BLEND_NORMAL at full opacity.
 *
 * @param[in] src       Source surface.
 * @param[in] dst       Destination surface.
 * @param[in] dst_rect  Destination placement rectangle.
 */
void GPUSurface_compose_at(GPUSurface src, GPUSurface dst,
                           gpu_rect_t dst_rect);

/**
 * @brief Create a pipeline from a descriptor struct.
 *
 * @details
 * Simpler alternative to GPUSurface_create_pipeline(). Zero-initialize
 * the descriptor and set only the fields you care about; sensible defaults
 * are applied for everything else.
 *
 * @param[in] desc  Pipeline descriptor. Zero-init for defaults.
 * @return A render pipeline handle, or 0 on failure.
 */
gpu_render_pipeline_t GPUSurface_pipeline(const gpu_pipeline_desc_t *desc);

/* ================================================================== */
/*  Short aliases (opt-in: #define GPU_SHORT_NAMES before include)     */
/* ================================================================== */

#ifdef GPU_SHORT_NAMES

#define surf_init              GPUSurface_init
#define surf_create            GPUSurface_create
#define surf_bind_swapchain    GPUSurface_bind_swapchain
#define surf_destroy           GPUSurface_destroy
#define surf_resize            GPUSurface_resize
#define surf_acquire_scratch   GPUSurface_acquire_scratch
#define surf_release_scratch   GPUSurface_release_scratch

#define surf_clear             GPUSurface_clear
#define surf_clear_color       GPUSurface_clear_color

#define surf_begin             GPUSurface_begin
#define surf_begin_load        GPUSurface_begin_load
#define surf_begin_3d          GPUSurface_begin_3d
#define surf_begin_3d_load     GPUSurface_begin_3d_load
#define surf_begin_ex          GPUSurface_begin_ex
#define surf_end               GPUSurface_end

#define surf_draw_pipeline     GPUSurface_draw_pipeline
#define surf_draw_pipeline_ex  GPUSurface_draw_pipeline_ex
#define surf_draw              GPUSurface_draw
#define surf_draw_occluder     GPUSurface_draw_occluder

#define surf_create_pipeline       GPUSurface_create_pipeline
#define surf_create_pipeline_depth GPUSurface_create_pipeline_depth
#define surf_pipeline              GPUSurface_pipeline

#define surf_blit              GPUSurface_blit
#define surf_compose           GPUSurface_compose
#define surf_compose_at        GPUSurface_compose_at
#define surf_present           GPUSurface_present

#define surf_get_quad_buffer   GPUSurface_get_quad_buffer
#define surf_get_sampler       GPUSurface_get_sampler
#define surf_get_device        GPUSurface_get_device
#define surf_get_surface_format GPUSurface_get_surface_format
#define surf_get_width         GPUSurface_get_width
#define surf_get_height        GPUSurface_get_height
#define surf_get_texture_view  GPUSurface_get_texture_view
#define surf_get_render_pass   GPUSurface_get_render_pass
#define surf_get_offscreen_format GPUSurface_get_offscreen_format

#endif /* GPU_SHORT_NAMES */

#endif /* GPU_SURFACE_H */
