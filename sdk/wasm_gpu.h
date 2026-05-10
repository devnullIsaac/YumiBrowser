/*
    Yumi SDK — WebGPU (Dawn) WASM Imports
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
 * @file wasm_gpu.h
 * @brief WebAssembly guest imports for host-side WebGPU (Dawn) bindings.
 *
 * @details
 * This header provides the complete interface for WASM guest modules to
 * interact with the host's WebGPU implementation (via Dawn). It includes:
 *
 *   - Opaque handle types for all WebGPU objects
 *   - Constants matching the Dawn/webgpu.h enumeration values
 *   - Packed descriptor structs for creating GPU resources
 *   - Host-imported functions for every WebGPU operation
 *
 * ## Design
 * Every function declared here maps 1:1 to a host import in
 * `wgpu_bindings.c`. The host maintains the actual WebGPU objects;
 * guests only hold lightweight 32-bit handles. Handle 0 is universally
 * invalid / null.
 *
 * ## Memory ABI
 * Descriptor structs are `__attribute__((packed, aligned(4)))` to ensure
 * a stable binary layout that both guest (WASM32) and host (native)
 * agree on. Pointer fields in descriptors are WASM32 pointers (4 bytes).
 *
 * ## Typical Render Loop
 * @code
 *   gpu_device_t dev = wgpu_get_device();
 *   gpu_queue_t queue = wgpu_get_queue();
 *
 *   gpu_shader_t vs = wgpu_create_shader_wgsl(dev, vs_code, vs_len);
 *   gpu_shader_t fs = wgpu_create_shader_wgsl(dev, fs_code, fs_len);
 *
 *   gpu_render_pipeline_t pipe = wgpu_create_render_pipeline(
 *       dev, layout, vs, "vs_main", 8, fs, "fs_main", 8,
 *       format, GPU_TOPO_TRIANGLE_LIST, &vtx_layout);
 *
 *   gpu_command_encoder_t enc = wgpu_create_command_encoder(dev);
 *   gpu_render_pass_t pass = wgpu_begin_render_pass(
 *       enc, view, 0.0f, 0.0f, 0.0f, 1.0f);
 *   wgpu_render_pass_set_pipeline(pass, pipe);
 *   wgpu_render_pass_draw(pass, 3, 1, 0, 0);
 *   wgpu_render_pass_end(pass);
 *   gpu_command_buffer_t cmd = wgpu_encoder_finish(enc);
 *   wgpu_queue_submit(queue, &cmd, 1);
 * @endcode
 *
 * @see https://dawn.googlesource.com/dawn
 * @see https://www.w3.org/TR/webgpu/
 */

#ifndef WASM_GPU_H
#define WASM_GPU_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================== */
/*  Opaque handle types                                                */
/* ================================================================== */

/**
 * @brief Opaque handle to a WebGPU device.
 * @details Represents the logical GPU device. Obtained via wgpu_get_device().
 */
typedef uint32_t gpu_device_t;

/**
 * @brief Opaque handle to a WebGPU queue.
 * @details The command submission queue. Obtained via wgpu_get_queue().
 */
typedef uint32_t gpu_queue_t;

/**
 * @brief Opaque handle to a compiled shader module.
 * @details Created from SPIR-V or WGSL source via wgpu_create_shader_spirv()
 *          or wgpu_create_shader_wgsl().
 */
typedef uint32_t gpu_shader_t;

/**
 * @brief Opaque handle to a GPU buffer.
 * @details Stores vertex data, index data, uniform blocks, or storage.
 */
typedef uint32_t gpu_buffer_t;

/**
 * @brief Opaque handle to a pipeline layout.
 * @details Defines the bind group layout slots used by a pipeline.
 */
typedef uint32_t gpu_pipeline_layout_t;

/**
 * @brief Opaque handle to a render pipeline.
 * @details A compiled GPU pipeline for rasterization draw calls.
 */
typedef uint32_t gpu_render_pipeline_t;

/**
 * @brief Opaque handle to a compute pipeline.
 * @details A compiled GPU pipeline for compute shader dispatches.
 */
typedef uint32_t gpu_compute_pipeline_t;

/**
 * @brief Opaque handle to a command encoder.
 * @details Used to record commands (render passes, copies, etc.) into a
 *          command buffer.
 */
typedef uint32_t gpu_command_encoder_t;

/**
 * @brief Opaque handle to an active render pass.
 * @details Obtained from wgpu_begin_render_pass(). Valid only until
 *          wgpu_render_pass_end() is called.
 */
typedef uint32_t gpu_render_pass_t;

/**
 * @brief Opaque handle to an active compute pass.
 * @details Obtained from wgpu_begin_compute_pass(). Valid only until
 *          wgpu_compute_pass_end() is called.
 */
typedef uint32_t gpu_compute_pass_t;

/**
 * @brief Opaque handle to a command buffer.
 * @details A finalized, submit-ready command sequence created by
 *          wgpu_encoder_finish().
 */
typedef uint32_t gpu_command_buffer_t;

/**
 * @brief Opaque handle to a GPU texture.
 * @details A 1D, 2D, 3D, or array texture resource.
 */
typedef uint32_t gpu_texture_t;

/**
 * @brief Opaque handle to a texture view.
 * @details A view into a specific subresource range of a texture,
 *          suitable for binding to pipelines or attachment.
 */
typedef uint32_t gpu_texture_view_t;

/**
 * @brief Opaque handle to a sampler.
 * @details Defines texture sampling behavior (filtering, wrapping, LOD).
 */
typedef uint32_t gpu_sampler_t;

/**
 * @brief Opaque handle to a bind group layout.
 * @details Describes the types and visibility of bindings in a bind group.
 */
typedef uint32_t gpu_bind_group_layout_t;

/**
 * @brief Opaque handle to a bind group.
 * @details A concrete set of resource bindings (buffers, textures, samplers)
 *          that can be bound to a pipeline during a draw or dispatch.
 */
typedef uint32_t gpu_bind_group_t;

/**
 * @brief Opaque handle to a query set.
 * @details Used for occlusion queries or timestamp queries.
 */
typedef uint32_t gpu_query_set_t;

/* ================================================================== */
/*  Constants — matching Dawn webgpu.h                                 */
/* ================================================================== */

/**
 * @defgroup GPUBufferUsage Buffer usage flags
 * @brief Bitmask values for gpu_buffer_t usage.
 * @details Combine with bitwise OR. Matches WGPUBufferUsage in Dawn.
 * @{ */
#define GPU_BUFFER_USAGE_MAP_READ  (1 << 0)   /**< Mappable for CPU read. */
#define GPU_BUFFER_USAGE_MAP_WRITE (1 << 1)   /**< Mappable for CPU write. */
#define GPU_BUFFER_USAGE_COPY_SRC  (1 << 2)   /**< Can be source of copy. */
#define GPU_BUFFER_USAGE_COPY_DST  (1 << 3)   /**< Can be destination of copy. */
#define GPU_BUFFER_USAGE_INDEX     (1 << 4)   /**< Used as index buffer. */
#define GPU_BUFFER_USAGE_VERTEX    (1 << 5)   /**< Used as vertex buffer. */
#define GPU_BUFFER_USAGE_UNIFORM   (1 << 6)   /**< Used as uniform buffer. */
#define GPU_BUFFER_USAGE_STORAGE   (1 << 7)   /**< Used as storage buffer. */
#define GPU_BUFFER_USAGE_INDIRECT  (1 << 8)   /**< Used for indirect draws. */
#define GPU_BUFFER_USAGE_QUERY_RESOLVE (1 << 9) /**< Target for query resolve. */
/** @} */

/**
 * @defgroup GPUTextureUsage Texture usage flags
 * @brief Bitmask values for gpu_texture_t usage.
 * @details Combine with bitwise OR. Matches WGPUTextureUsage in Dawn.
 * @{ */
#define GPU_TEXTURE_USAGE_COPY_SRC          (1 << 0)
#define GPU_TEXTURE_USAGE_COPY_DST          (1 << 1)
#define GPU_TEXTURE_USAGE_TEXTURE_BINDING   (1 << 2)
#define GPU_TEXTURE_USAGE_STORAGE_BINDING   (1 << 3)
#define GPU_TEXTURE_USAGE_RENDER_ATTACHMENT (1 << 4)

/* Texture dimension (WGPUTextureDimension) — matches Dawn */
#define GPU_TEXTURE_DIM_1D  1
#define GPU_TEXTURE_DIM_2D  2
#define GPU_TEXTURE_DIM_3D  3

/* Map mode (WGPUMapMode) — bitmask, same in Dawn */
#define GPU_MAP_MODE_READ  1
#define GPU_MAP_MODE_WRITE 2

/* Index format (WGPUIndexFormat) — matches Dawn */
#define GPU_INDEX_FORMAT_UINT16  1
#define GPU_INDEX_FORMAT_UINT32  2

/* Shader stage visibility flags (WGPUShaderStage) — bitmask, same in Dawn */
#define GPU_SHADER_STAGE_VERTEX   (1 << 0)
#define GPU_SHADER_STAGE_FRAGMENT (1 << 1)
#define GPU_SHADER_STAGE_COMPUTE  (1 << 2)

/* Load ops (WGPULoadOp) — Dawn: Load=1, Clear=2 */
#define GPU_LOAD_OP_LOAD  1
#define GPU_LOAD_OP_CLEAR 2

/* Store ops (WGPUStoreOp) — Dawn: Store=1, Discard=2 */
#define GPU_STORE_OP_STORE   1
#define GPU_STORE_OP_DISCARD 2

/* Cull mode (WGPUCullMode) — Dawn: None=1, Front=2, Back=3 */
#define GPU_CULL_NONE  1
#define GPU_CULL_FRONT 2
#define GPU_CULL_BACK  3

/* Front face (WGPUFrontFace) — Dawn: CCW=1, CW=2 */
#define GPU_FRONT_FACE_CCW 1
#define GPU_FRONT_FACE_CW  2

/* Compare function (WGPUCompareFunction) — matches Dawn */
#define GPU_COMPARE_NEVER         1
#define GPU_COMPARE_LESS          2
#define GPU_COMPARE_EQUAL         3
#define GPU_COMPARE_LESS_EQUAL    4
#define GPU_COMPARE_GREATER       5
#define GPU_COMPARE_NOT_EQUAL     6
#define GPU_COMPARE_GREATER_EQUAL 7
#define GPU_COMPARE_ALWAYS        8

/* Stencil operation (WGPUStencilOperation) — Dawn: Keep=1..DecrementWrap=8 */
#define GPU_STENCIL_OP_KEEP            1
#define GPU_STENCIL_OP_ZERO            2
#define GPU_STENCIL_OP_REPLACE         3
#define GPU_STENCIL_OP_INVERT          4
#define GPU_STENCIL_OP_INCREMENT_CLAMP 5
#define GPU_STENCIL_OP_DECREMENT_CLAMP 6
#define GPU_STENCIL_OP_INCREMENT_WRAP  7
#define GPU_STENCIL_OP_DECREMENT_WRAP  8

/* Blend factor (WGPUBlendFactor) — Dawn: Zero=1..OneMinusConstant=13 */
#define GPU_BLEND_ZERO                  1
#define GPU_BLEND_ONE                   2
#define GPU_BLEND_SRC                   3
#define GPU_BLEND_ONE_MINUS_SRC         4
#define GPU_BLEND_SRC_ALPHA             5
#define GPU_BLEND_ONE_MINUS_SRC_ALPHA   6
#define GPU_BLEND_DST                   7
#define GPU_BLEND_ONE_MINUS_DST         8
#define GPU_BLEND_DST_ALPHA             9
#define GPU_BLEND_ONE_MINUS_DST_ALPHA  10
#define GPU_BLEND_SRC_ALPHA_SATURATED  11
#define GPU_BLEND_CONSTANT             12
#define GPU_BLEND_ONE_MINUS_CONSTANT   13

/* Blend operation (WGPUBlendOperation) — Dawn: Add=1..Max=5 */
#define GPU_BLEND_OP_ADD              1
#define GPU_BLEND_OP_SUBTRACT         2
#define GPU_BLEND_OP_REVERSE_SUBTRACT 3
#define GPU_BLEND_OP_MIN              4
#define GPU_BLEND_OP_MAX              5

/* Color write mask — bitmask, same in Dawn */
#define GPU_COLOR_WRITE_RED   (1 << 0)
#define GPU_COLOR_WRITE_GREEN (1 << 1)
#define GPU_COLOR_WRITE_BLUE  (1 << 2)
#define GPU_COLOR_WRITE_ALPHA (1 << 3)
#define GPU_COLOR_WRITE_ALL   0xF

/* Texture view dimension (WGPUTextureViewDimension) — matches Dawn */
#define GPU_VIEW_DIM_1D         1
#define GPU_VIEW_DIM_2D         2
#define GPU_VIEW_DIM_2D_ARRAY   3
#define GPU_VIEW_DIM_CUBE       4
#define GPU_VIEW_DIM_CUBE_ARRAY 5
#define GPU_VIEW_DIM_3D         6

/* Texture sample type — internal ABI, translated by host switch */
/* Host maps: 0->Float, 1->UnfilterableFloat, 2->Depth, 3->Sint, 4->Uint */
#define GPU_SAMPLE_TYPE_FLOAT              0
#define GPU_SAMPLE_TYPE_UNFILTERABLE_FLOAT 1
#define GPU_SAMPLE_TYPE_DEPTH              2
#define GPU_SAMPLE_TYPE_SINT               3
#define GPU_SAMPLE_TYPE_UINT               4

/* Sampler binding type — internal ABI, translated by host switch */
/* Host maps: 1->Filtering, 2->NonFiltering, 3->Comparison */
#define GPU_SAMPLER_TYPE_FILTERING     1
#define GPU_SAMPLER_TYPE_NON_FILTERING 2
#define GPU_SAMPLER_TYPE_COMPARISON    3

/* Storage texture access — internal ABI, translated by host switch */
/* Host maps: 1->WriteOnly, 2->ReadOnly, 3->ReadWrite */
#define GPU_STORAGE_TEX_WRITE_ONLY 1
#define GPU_STORAGE_TEX_READ_ONLY  2
#define GPU_STORAGE_TEX_READ_WRITE 3

/* Address mode (WGPUAddressMode) — matches Dawn */
#define GPU_ADDRESS_CLAMP_TO_EDGE 1
#define GPU_ADDRESS_REPEAT        2
#define GPU_ADDRESS_MIRROR_REPEAT 3

/* Filter mode (WGPUFilterMode) — Dawn: Nearest=1, Linear=2 */
#define GPU_FILTER_NEAREST 1
#define GPU_FILTER_LINEAR  2

/* Mipmap filter mode (WGPUMipmapFilterMode) — Dawn: Nearest=1, Linear=2 */
#define GPU_MIPMAP_NEAREST 1
#define GPU_MIPMAP_LINEAR  2

/* Query type (WGPUQueryType) — Dawn: Occlusion=1, Timestamp=2 */
#define GPU_QUERY_TYPE_OCCLUSION 1
#define GPU_QUERY_TYPE_TIMESTAMP 2

/* Step mode (WGPUVertexStepMode) — Dawn: Vertex=1, Instance=2 */
#define GPU_STEP_MODE_VERTEX   1
#define GPU_STEP_MODE_INSTANCE 2

/* ---- Bind group layout entry types ---- */
/* These are internal ABI values used by wgpu_bindings.c switch, NOT Dawn enums */
#define GPU_BIND_TYPE_BUFFER          0
#define GPU_BIND_TYPE_TEXTURE         1
#define GPU_BIND_TYPE_SAMPLER         2
#define GPU_BIND_TYPE_STORAGE_TEXTURE 3

/* ---- Bind group entry types ---- */
/* These are internal ABI values used by wgpu_bindings.c switch, NOT Dawn enums */
#define GPU_ENTRY_BUFFER       0
#define GPU_ENTRY_TEXTURE_VIEW 1
#define GPU_ENTRY_SAMPLER      2

/* ---- Buffer binding type (WGPUBufferBindingType) ---- */
/* Dawn: Uniform=2, Storage=3, ReadOnlyStorage=4 */
#define GPU_BUFFER_TYPE_UNIFORM           0
#define GPU_BUFFER_TYPE_STORAGE           1
#define GPU_BUFFER_TYPE_READ_ONLY_STORAGE 2
/* NOTE: These are internal ABI values. The host wgpu_bindings.c translates
 * 0->WGPUBufferBindingType_Uniform, 1->Storage, 2->ReadOnlyStorage via switch. */

/* ---- Vertex format query (stable ABI) ---- */
#define GPU_FMT_FLOAT32X2  0
#define GPU_FMT_FLOAT32X3  1
#define GPU_FMT_FLOAT32X4  2
#define GPU_FMT_UINT32     3
#define GPU_FMT_FLOAT32    4
#define GPU_FMT_SINT32X2   5
#define GPU_FMT_SINT32X3   6
#define GPU_FMT_SINT32X4   7
#define GPU_FMT_UINT16X2   8
#define GPU_FMT_UINT16X4   9
#define GPU_FMT_UNORM8X2  10
#define GPU_FMT_UNORM8X4  11
#define GPU_FMT_SNORM8X2  12
#define GPU_FMT_SNORM8X4  13
#define GPU_FMT_UINT8X2   14
#define GPU_FMT_UINT8X4   15
#define GPU_FMT_SINT8X2   16
#define GPU_FMT_SINT8X4   17
#define GPU_FMT_UINT32X2  18
#define GPU_FMT_UINT32X3  19
#define GPU_FMT_UINT32X4  20
#define GPU_FMT_SINT32    21
#define GPU_FMT_FLOAT16X2 22
#define GPU_FMT_FLOAT16X4 23
#define GPU_FMT_UNORM16X2 24
#define GPU_FMT_UNORM16X4 25
#define GPU_FMT_SNORM16X2 26
#define GPU_FMT_SNORM16X4 27

/* ---- Topology query (stable ABI) ---- */
#define GPU_TOPO_POINT_LIST     0
#define GPU_TOPO_LINE_LIST      1
#define GPU_TOPO_LINE_STRIP     2
#define GPU_TOPO_TRIANGLE_LIST  3
#define GPU_TOPO_TRIANGLE_STRIP 4

/* ================================================================== */
/*  Packed descriptor structures (linear memory ABI)                   */
/* ================================================================== */

/* ---- Bind group layout entry (32 bytes) ----
 *
 * type 0 (buffer):  field0=buf_type, field1=has_dynamic_offset, field2=min_binding_size
 * type 1 (texture): field0=sample_type, field1=view_dimension, field2=multisampled
 * type 2 (sampler): field0=sampler_type
 * type 3 (storage_tex): field0=access, field1=format, field2=view_dimension
 */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t binding;
    uint32_t visibility;
    uint32_t type;
    uint32_t field0;
    uint32_t field1;
    uint32_t field2;
    uint32_t _pad0;
    uint32_t _pad1;
} gpu_bind_group_layout_entry_t;

/* ---- Bind group entry (20 bytes) ----
 *
 * type 0 (buffer):       handle=buffer_h, field0=offset, field1=size(0=whole)
 * type 1 (texture_view): handle=view_h
 * type 2 (sampler):      handle=sampler_h
 */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t binding;
    uint32_t type;
    uint32_t handle;
    uint32_t field0;
    uint32_t field1;
} gpu_bind_group_entry_t;

/* ---- Texture descriptor (32 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t usage;
    uint32_t dimension;
    uint32_t width;
    uint32_t height;
    uint32_t depth_or_array_layers;
    uint32_t format;
    uint32_t mip_level_count;
    uint32_t sample_count;
} gpu_texture_desc_t;

/* ---- Texture view descriptor (28 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t format;
    uint32_t dimension;
    uint32_t base_mip_level;
    uint32_t mip_level_count;
    uint32_t base_array_layer;
    uint32_t array_layer_count;
    uint32_t _pad;
} gpu_texture_view_desc_t;

/* ---- Queue write texture descriptor (48 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t texture_h;
    uint32_t mip_level;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t origin_z;
    uint32_t data_ptr;
    uint32_t data_len;
    uint32_t bytes_per_row;
    uint32_t rows_per_image;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} gpu_write_texture_desc_t;

/* ---- Sampler descriptor (40 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t address_mode_u;
    uint32_t address_mode_v;
    uint32_t address_mode_w;
    uint32_t mag_filter;
    uint32_t min_filter;
    uint32_t mipmap_filter;
    float    lod_min_clamp;
    float    lod_max_clamp;
    uint32_t compare;
    uint32_t max_anisotropy;
} gpu_sampler_desc_t;

/* ---- Vertex attribute (12 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t format;
    uint32_t offset;
    uint32_t shader_location;
} gpu_vertex_attr_t;

/* ---- Vertex buffer layout (simple, for fn_create_render_pipeline) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t stride;
    uint32_t attribute_count;
    gpu_vertex_attr_t attributes[];
} gpu_vertex_layout_t;

/* ---- Vertex buffer layout (full, for fn_create_render_pipeline_desc) (16 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t array_stride;
    uint32_t step_mode;
    uint32_t attribute_count;
    uint32_t attributes_ptr;
} gpu_vertex_buffer_layout_t;

/* ---- Color target state (36 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t format;
    uint32_t write_mask;
    uint32_t blend_enabled;
    uint32_t color_src_factor;
    uint32_t color_dst_factor;
    uint32_t color_operation;
    uint32_t alpha_src_factor;
    uint32_t alpha_dst_factor;
    uint32_t alpha_operation;
} gpu_color_target_t;

/* ---- Depth stencil state (64 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t format;
    uint32_t depth_write_enabled;
    uint32_t depth_compare;
    uint32_t stencil_front_compare;
    uint32_t stencil_front_fail;
    uint32_t stencil_front_depth_fail;
    uint32_t stencil_front_pass;
    uint32_t stencil_back_compare;
    uint32_t stencil_back_fail;
    uint32_t stencil_back_depth_fail;
    uint32_t stencil_back_pass;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    int32_t  depth_bias;
    float    depth_bias_slope_scale;
    float    depth_bias_clamp;
} gpu_depth_stencil_t;

/* ---- Full render pipeline descriptor (80 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t layout_h;
    uint32_t vs_shader_h;
    uint32_t vs_entry_ptr;
    uint32_t vs_entry_len;
    uint32_t fs_shader_h;
    uint32_t fs_entry_ptr;
    uint32_t fs_entry_len;
    uint32_t topology;
    uint32_t strip_index_format;
    uint32_t cull_mode;
    uint32_t front_face;
    uint32_t color_target_count;
    uint32_t color_targets_ptr;
    uint32_t has_depth_stencil;
    uint32_t depth_stencil_ptr;
    uint32_t vertex_buffer_count;
    uint32_t vertex_buffers_ptr;
    uint32_t sample_count;
    uint32_t sample_mask;
    uint32_t alpha_to_coverage;
} gpu_render_pipeline_desc_t;

/* ---- Render pass color attachment (32 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t view_h;
    uint32_t resolve_target_h;
    uint32_t load_op;
    uint32_t store_op;
    float    clear_r;
    float    clear_g;
    float    clear_b;
    float    clear_a;
} gpu_color_attachment_t;

/* ---- Render pass depth/stencil attachment (28 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t view_h;
    uint32_t depth_load_op;
    uint32_t depth_store_op;
    float    depth_clear_value;
    uint32_t stencil_load_op;
    uint32_t stencil_store_op;
    uint32_t stencil_clear_value;
} gpu_depth_attachment_t;

/* ---- Full render pass descriptor (20 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t color_attachment_count;
    uint32_t color_attachments_ptr;
    uint32_t has_depth_stencil;
    uint32_t depth_stencil_ptr;
    uint32_t occlusion_query_set_h;
} gpu_render_pass_desc_t;

/* ---- Copy buffer to texture descriptor (48 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t buffer_h;
    uint32_t buffer_offset;
    uint32_t bytes_per_row;
    uint32_t rows_per_image;
    uint32_t texture_h;
    uint32_t mip_level;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t origin_z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} gpu_copy_buf_to_tex_t;

/* ---- Copy texture to buffer descriptor (48 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t texture_h;
    uint32_t mip_level;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t origin_z;
    uint32_t buffer_h;
    uint32_t buffer_offset;
    uint32_t bytes_per_row;
    uint32_t rows_per_image;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} gpu_copy_tex_to_buf_t;

/* ---- Copy texture to texture descriptor (52 bytes) ---- */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t src_texture_h;
    uint32_t src_mip_level;
    uint32_t src_origin_x;
    uint32_t src_origin_y;
    uint32_t src_origin_z;
    uint32_t dst_texture_h;
    uint32_t dst_mip_level;
    uint32_t dst_origin_x;
    uint32_t dst_origin_y;
    uint32_t dst_origin_z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} gpu_copy_tex_to_tex_t;

/* ================================================================== */
/*  Imports from host                                                  */
/* ================================================================== */

#define IMPORT __attribute__((import_module("env")))

/* ---- Device / Queue / Surface ---- */

IMPORT __attribute__((import_name("wgpu_get_device")))
gpu_device_t wgpu_get_device(void);

IMPORT __attribute__((import_name("wgpu_get_queue")))
gpu_queue_t wgpu_get_queue(void);

IMPORT __attribute__((import_name("wgpu_get_surface_format")))
int wgpu_get_surface_format(void);

IMPORT __attribute__((import_name("wgpu_get_surface_width")))
int wgpu_get_surface_width(void);

IMPORT __attribute__((import_name("wgpu_get_surface_height")))
int wgpu_get_surface_height(void);

IMPORT __attribute__((import_name("wgpu_device_poll")))
int wgpu_device_poll(int wait);

/* ---- Enum queries ---- */

IMPORT __attribute__((import_name("wgpu_get_vertex_format")))
int wgpu_get_vertex_format(int kind);

IMPORT __attribute__((import_name("wgpu_get_topology")))
int wgpu_get_topology(int kind);

/* ---- Shader ---- */

IMPORT __attribute__((import_name("wgpu_create_shader_spirv")))
gpu_shader_t wgpu_create_shader_spirv(gpu_device_t dev,
    const void *code, int code_len,
    const char *entry, int entry_len);

IMPORT __attribute__((import_name("wgpu_create_shader_wgsl")))
gpu_shader_t wgpu_create_shader_wgsl(gpu_device_t dev,
    const char *code, int code_len);

IMPORT __attribute__((import_name("wgpu_shader_release")))
void wgpu_shader_release(gpu_shader_t shader);

/* ---- Buffer ---- */

IMPORT __attribute__((import_name("wgpu_create_buffer")))
gpu_buffer_t wgpu_create_buffer(gpu_device_t dev, int usage, int size, int mapped);

IMPORT __attribute__((import_name("wgpu_buffer_write")))
void wgpu_buffer_write(gpu_buffer_t buf, int offset, const void *data, int len);

IMPORT __attribute__((import_name("wgpu_buffer_destroy")))
void wgpu_buffer_destroy(gpu_buffer_t buf);

IMPORT __attribute__((import_name("wgpu_buffer_get_size")))
int wgpu_buffer_get_size(gpu_buffer_t buf);

IMPORT __attribute__((import_name("wgpu_buffer_map_sync")))
int wgpu_buffer_map_sync(gpu_buffer_t buf, int mode, int offset, int size);

IMPORT __attribute__((import_name("wgpu_buffer_get_mapped_range")))
void wgpu_buffer_get_mapped_range(gpu_buffer_t buf, int offset, int size, void *dst);

IMPORT __attribute__((import_name("wgpu_buffer_unmap")))
void wgpu_buffer_unmap(gpu_buffer_t buf);

/* ---- Texture ---- */

IMPORT __attribute__((import_name("wgpu_create_texture")))
gpu_texture_t wgpu_create_texture(const gpu_texture_desc_t *desc);

IMPORT __attribute__((import_name("wgpu_create_texture_view")))
gpu_texture_view_t wgpu_create_texture_view(gpu_texture_t tex,
    const gpu_texture_view_desc_t *desc);

IMPORT __attribute__((import_name("wgpu_queue_write_texture")))
void wgpu_queue_write_texture(const gpu_write_texture_desc_t *desc);

IMPORT __attribute__((import_name("wgpu_texture_destroy")))
void wgpu_texture_destroy(gpu_texture_t tex);

IMPORT __attribute__((import_name("wgpu_texture_release")))
void wgpu_texture_release(gpu_texture_t tex);

IMPORT __attribute__((import_name("wgpu_texture_view_release")))
void wgpu_texture_view_release(gpu_texture_view_t view);

/* ---- Sampler ---- */

IMPORT __attribute__((import_name("wgpu_create_sampler")))
gpu_sampler_t wgpu_create_sampler(const gpu_sampler_desc_t *desc);

IMPORT __attribute__((import_name("wgpu_sampler_release")))
void wgpu_sampler_release(gpu_sampler_t sampler);

/* ---- Bind Group Layout / Bind Group ---- */

IMPORT __attribute__((import_name("wgpu_create_bind_group_layout")))
gpu_bind_group_layout_t wgpu_create_bind_group_layout(gpu_device_t dev,
    const gpu_bind_group_layout_entry_t *entries, int entry_count);

IMPORT __attribute__((import_name("wgpu_bind_group_layout_release")))
void wgpu_bind_group_layout_release(gpu_bind_group_layout_t layout);

IMPORT __attribute__((import_name("wgpu_create_bind_group")))
gpu_bind_group_t wgpu_create_bind_group(gpu_device_t dev,
    gpu_bind_group_layout_t layout,
    const gpu_bind_group_entry_t *entries, int entry_count);

IMPORT __attribute__((import_name("wgpu_bind_group_release")))
void wgpu_bind_group_release(gpu_bind_group_t group);

/* ---- Pipeline Layout ---- */

IMPORT __attribute__((import_name("wgpu_create_pipeline_layout_empty")))
gpu_pipeline_layout_t wgpu_create_pipeline_layout_empty(gpu_device_t dev);

IMPORT __attribute__((import_name("wgpu_create_pipeline_layout")))
gpu_pipeline_layout_t wgpu_create_pipeline_layout(gpu_device_t dev,
    const gpu_bind_group_layout_t *layouts, int layout_count);

IMPORT __attribute__((import_name("wgpu_pipeline_layout_release")))
void wgpu_pipeline_layout_release(gpu_pipeline_layout_t layout);

/* ---- Render Pipeline ---- */

IMPORT __attribute__((import_name("wgpu_create_render_pipeline")))
gpu_render_pipeline_t wgpu_create_render_pipeline(gpu_device_t dev,
    gpu_pipeline_layout_t layout,
    gpu_shader_t vs_shader, const char *vs_entry, int vs_len,
    gpu_shader_t fs_shader, const char *fs_entry, int fs_len,
    int format, int topology,
    const gpu_vertex_layout_t *vtx_layout);

IMPORT __attribute__((import_name("wgpu_create_render_pipeline_desc")))
gpu_render_pipeline_t wgpu_create_render_pipeline_desc(
    const gpu_render_pipeline_desc_t *desc);

IMPORT __attribute__((import_name("wgpu_render_pipeline_release")))
void wgpu_render_pipeline_release(gpu_render_pipeline_t pipeline);

IMPORT __attribute__((import_name("wgpu_render_pipeline_get_bind_group_layout")))
gpu_bind_group_layout_t wgpu_render_pipeline_get_bind_group_layout(
    gpu_render_pipeline_t pipeline, int group_index);

/* ---- Compute Pipeline ---- */

IMPORT __attribute__((import_name("wgpu_create_compute_pipeline")))
gpu_compute_pipeline_t wgpu_create_compute_pipeline(
    gpu_pipeline_layout_t layout,
    gpu_shader_t shader, const char *entry, int entry_len);

IMPORT __attribute__((import_name("wgpu_compute_pipeline_release")))
void wgpu_compute_pipeline_release(gpu_compute_pipeline_t pipeline);

IMPORT __attribute__((import_name("wgpu_compute_pipeline_get_bind_group_layout")))
gpu_bind_group_layout_t wgpu_compute_pipeline_get_bind_group_layout(
    gpu_compute_pipeline_t pipeline, int group_index);

/* ---- Command Encoder ---- */

IMPORT __attribute__((import_name("wgpu_create_command_encoder")))
gpu_command_encoder_t wgpu_create_command_encoder(gpu_device_t dev);

IMPORT __attribute__((import_name("wgpu_encoder_finish")))
gpu_command_buffer_t wgpu_encoder_finish(gpu_command_encoder_t enc);

IMPORT __attribute__((import_name("wgpu_encoder_copy_buffer_to_buffer")))
void wgpu_encoder_copy_buffer_to_buffer(gpu_command_encoder_t enc,
    gpu_buffer_t src, int src_offset,
    gpu_buffer_t dst, int dst_offset, int size);

IMPORT __attribute__((import_name("wgpu_encoder_copy_buffer_to_texture")))
void wgpu_encoder_copy_buffer_to_texture(gpu_command_encoder_t enc,
    const gpu_copy_buf_to_tex_t *desc);

IMPORT __attribute__((import_name("wgpu_encoder_copy_texture_to_buffer")))
void wgpu_encoder_copy_texture_to_buffer(gpu_command_encoder_t enc,
    const gpu_copy_tex_to_buf_t *desc);

IMPORT __attribute__((import_name("wgpu_encoder_copy_texture_to_texture")))
void wgpu_encoder_copy_texture_to_texture(gpu_command_encoder_t enc,
    const gpu_copy_tex_to_tex_t *desc);

IMPORT __attribute__((import_name("wgpu_encoder_clear_buffer")))
void wgpu_encoder_clear_buffer(gpu_command_encoder_t enc,
    gpu_buffer_t buf, int offset, int size);

IMPORT __attribute__((import_name("wgpu_encoder_resolve_query_set")))
void wgpu_encoder_resolve_query_set(gpu_command_encoder_t enc,
    gpu_query_set_t query_set, int first_query, int query_count,
    gpu_buffer_t dest_buffer, int dest_offset);

/* ---- Render Pass ---- */

IMPORT __attribute__((import_name("wgpu_begin_render_pass")))
gpu_render_pass_t wgpu_begin_render_pass(gpu_command_encoder_t enc,
    gpu_texture_view_t view, float r, float g, float b, float a);

IMPORT __attribute__((import_name("wgpu_begin_render_pass_desc")))
gpu_render_pass_t wgpu_begin_render_pass_desc(gpu_command_encoder_t enc,
    const gpu_render_pass_desc_t *desc);

IMPORT __attribute__((import_name("wgpu_render_pass_set_pipeline")))
void wgpu_render_pass_set_pipeline(gpu_render_pass_t pass,
    gpu_render_pipeline_t pipeline);

IMPORT __attribute__((import_name("wgpu_render_pass_set_vertex_buffer")))
void wgpu_render_pass_set_vertex_buffer(gpu_render_pass_t pass,
    int slot, gpu_buffer_t buf, int offset, int size);

IMPORT __attribute__((import_name("wgpu_render_pass_set_index_buffer")))
void wgpu_render_pass_set_index_buffer(gpu_render_pass_t pass,
    gpu_buffer_t buf, int format, int offset, int size);

IMPORT __attribute__((import_name("wgpu_render_pass_set_bind_group")))
void wgpu_render_pass_set_bind_group(gpu_render_pass_t pass,
    int group_index, gpu_bind_group_t group,
    const uint32_t *dynamic_offsets, int dynamic_offset_count);

IMPORT __attribute__((import_name("wgpu_render_pass_draw")))
void wgpu_render_pass_draw(gpu_render_pass_t pass,
    int vertex_count, int instance_count,
    int first_vertex, int first_instance);

IMPORT __attribute__((import_name("wgpu_render_pass_draw_indexed")))
void wgpu_render_pass_draw_indexed(gpu_render_pass_t pass,
    int index_count, int instance_count,
    int first_index, int vertex_offset, int first_instance);

IMPORT __attribute__((import_name("wgpu_render_pass_draw_indirect")))
void wgpu_render_pass_draw_indirect(gpu_render_pass_t pass,
    gpu_buffer_t indirect_buf, int indirect_offset);

IMPORT __attribute__((import_name("wgpu_render_pass_draw_indexed_indirect")))
void wgpu_render_pass_draw_indexed_indirect(gpu_render_pass_t pass,
    gpu_buffer_t indirect_buf, int indirect_offset);

IMPORT __attribute__((import_name("wgpu_render_pass_set_viewport")))
void wgpu_render_pass_set_viewport(gpu_render_pass_t pass,
    float x, float y, float w, float h, float min_depth, float max_depth);

IMPORT __attribute__((import_name("wgpu_render_pass_set_scissor_rect")))
void wgpu_render_pass_set_scissor_rect(gpu_render_pass_t pass,
    int x, int y, int w, int h);

IMPORT __attribute__((import_name("wgpu_render_pass_set_stencil_reference")))
void wgpu_render_pass_set_stencil_reference(gpu_render_pass_t pass, int ref);

IMPORT __attribute__((import_name("wgpu_render_pass_set_blend_constant")))
void wgpu_render_pass_set_blend_constant(gpu_render_pass_t pass,
    float r, float g, float b, float a);

IMPORT __attribute__((import_name("wgpu_render_pass_begin_occlusion_query")))
void wgpu_render_pass_begin_occlusion_query(gpu_render_pass_t pass, int query_index);

IMPORT __attribute__((import_name("wgpu_render_pass_end_occlusion_query")))
void wgpu_render_pass_end_occlusion_query(gpu_render_pass_t pass);

IMPORT __attribute__((import_name("wgpu_render_pass_end")))
void wgpu_render_pass_end(gpu_render_pass_t pass);

/* ---- Compute Pass ---- */

IMPORT __attribute__((import_name("wgpu_begin_compute_pass")))
gpu_compute_pass_t wgpu_begin_compute_pass(gpu_command_encoder_t enc);

IMPORT __attribute__((import_name("wgpu_compute_pass_set_pipeline")))
void wgpu_compute_pass_set_pipeline(gpu_compute_pass_t pass,
    gpu_compute_pipeline_t pipeline);

IMPORT __attribute__((import_name("wgpu_compute_pass_set_bind_group")))
void wgpu_compute_pass_set_bind_group(gpu_compute_pass_t pass,
    int group_index, gpu_bind_group_t group,
    const uint32_t *dynamic_offsets, int dynamic_offset_count);

IMPORT __attribute__((import_name("wgpu_compute_pass_dispatch")))
void wgpu_compute_pass_dispatch(gpu_compute_pass_t pass, int x, int y, int z);

IMPORT __attribute__((import_name("wgpu_compute_pass_dispatch_indirect")))
void wgpu_compute_pass_dispatch_indirect(gpu_compute_pass_t pass,
    gpu_buffer_t indirect_buf, int indirect_offset);

IMPORT __attribute__((import_name("wgpu_compute_pass_end")))
void wgpu_compute_pass_end(gpu_compute_pass_t pass);

/* ---- Queue ---- */

IMPORT __attribute__((import_name("wgpu_queue_submit")))
void wgpu_queue_submit(gpu_queue_t queue, gpu_command_buffer_t cmd);

/* ---- Surface ---- */

IMPORT __attribute__((import_name("wgpu_surface_get_current_texture_view")))
gpu_texture_view_t wgpu_surface_get_current_texture_view(void);

IMPORT __attribute__((import_name("wgpu_surface_present")))
void wgpu_surface_present(void);

/* ---- Query Set ---- */

IMPORT __attribute__((import_name("wgpu_create_query_set")))
gpu_query_set_t wgpu_create_query_set(int type, int count);

IMPORT __attribute__((import_name("wgpu_query_set_destroy")))
void wgpu_query_set_destroy(gpu_query_set_t query_set);

#undef IMPORT

#endif /* WASM_GPU_H */
