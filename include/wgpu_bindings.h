/*
 * wgpu_bindings.h - WebGPU WASM bindings: full WebGPU API exposed to guests via per-object integer handle tables (devices, buffers, textures, pipelines, etc.).
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
 * @file wgpu_bindings.h
 * @brief WebGPU WASM bindings for Yumi Browser.
 *
 * Exposes the full WebGPU API to WASM guests via integer handles. Each
 * WebGPU object type (device, buffer, texture, pipeline, etc.) has its
 * own handle table. The guest calls functions like `wgpu_device_create_buffer`
 * and receives an integer handle back.
 *
 * ## Example
 *
 * @code{.c}
 * #include "wgpu_bindings.h"
 *
 * WgpuBindings wb;
 * wgpu_bindings_init(&wb, &gpu_context);
 * wgpu_bindings_set_memory(&wb, wasm_memory);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t n = wgpu_bindings_get_imports(&wb, store, &names, &funcs);
 * // Register imports into the WASM instance.
 *
 * wgpu_bindings_destroy(&wb);
 * @endcode
 */

#ifndef WGPU_BINDINGS_H
#define WGPU_BINDINGS_H

#include "deps.h"
#include "gpu.h"
#include "handle_table.h"

/**
 * @brief WebGPU binding state with handle tables for every GPU object type.
 */
typedef struct {
    GpuContext *gpu;               /**< Underlying GPU context. */

    HandleTable ht_device;         /**< WGPUDevice handles. */
    HandleTable ht_queue;          /**< WGPUQueue handles. */
    HandleTable ht_buffer;         /**< WGPUBuffer handles. */
    HandleTable ht_texture;        /**< WGPUTexture handles. */
    HandleTable ht_texture_view;   /**< WGPUTextureView handles. */
    HandleTable ht_sampler;        /**< WGPUSampler handles. */
    HandleTable ht_shader;         /**< WGPUShaderModule handles. */
    HandleTable ht_bind_group_layout; /**< WGPUBindGroupLayout handles. */
    HandleTable ht_bind_group;     /**< WGPUBindGroup handles. */
    HandleTable ht_pipeline_layout;/**< WGPUPipelineLayout handles. */
    HandleTable ht_render_pipeline;/**< WGPURenderPipeline handles. */
    HandleTable ht_compute_pipeline;/**< WGPUComputePipeline handles. */
    HandleTable ht_command_encoder;/**< WGPUCommandEncoder handles. */
    HandleTable ht_render_pass;    /**< WGPURenderPassEncoder handles. */
    HandleTable ht_compute_pass;   /**< WGPUComputePassEncoder handles. */
    HandleTable ht_command_buffer; /**< WGPUCommandBuffer handles. */
    HandleTable ht_surface;        /**< WGPUSurface handles. */
    HandleTable ht_query_set;      /**< WGPUQuerySet handles. */

    wasm_memory_t *memory;         /**< Guest WASM linear memory. */

    uint32_t h_device;             /**< Pre-allocated device handle. */
    uint32_t h_queue;              /**< Pre-allocated queue handle. */
    uint32_t h_surface;            /**< Pre-allocated surface handle. */

    WGPUTexture     frame_texture;      /**< Current frame surface texture. */
    WGPUTextureView frame_view;         /**< Current frame texture view. */
    uint32_t        frame_texture_handle; /**< Handle for current frame texture. */
    uint32_t        frame_view_handle;    /**< Handle for current frame view. */

    /** Offscreen mode: webapp renders to an offscreen texture, not the swapchain. */
    bool            offscreen;
    WGPUTexture     offscreen_texture;  /**< Offscreen target (owned by dashboard). */
    WGPUTextureView offscreen_view;     /**< View of the offscreen target. */
} WgpuBindings;

/**
 * @brief Initialize WebGPU bindings from a GPU context.
 *
 * @param[out] b    Bindings to initialize.
 * @param[in]  gpu  GPU context providing device, queue, and surface.
 */
void     wgpu_bindings_init(WgpuBindings *b, GpuContext *gpu);

/**
 * @brief Destroy WebGPU bindings and release all handle tables.
 * @param[in,out] b  Bindings to destroy.
 */
void     wgpu_bindings_destroy(WgpuBindings *b);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] b    Bindings.
 * @param[in]     mem  Guest WASM memory.
 */
void     wgpu_bindings_set_memory(WgpuBindings *b, wasm_memory_t *mem);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] b          Bindings.
 * @param[in]     store      WASM store.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t   wgpu_bindings_get_imports(WgpuBindings *b, wasm_store_t *store,
                                   const char ***out_names,
                                   wasm_func_t ***out_funcs);

#endif
