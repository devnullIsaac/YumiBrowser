/**
 * @file gles_internal.h
 * @brief Internal helpers for the WebGL2 → WebGPU emulation layer.
 *
 * Provides convenience macros for extracting arguments from WASM
 * callback signatures, memory access helpers, and forward declarations
 * for all `fn_*` callbacks that implement individual GL functions.
 *
 * @note This is a **private** header — not part of the public API.
 *       Only include it from gles_*.c implementation files.
 */

#ifndef GLES_INTERNAL_H
#define GLES_INTERNAL_H

#include "gles_bindings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Macros                                                             */
/* ------------------------------------------------------------------ */

#define CTX  ((WebGL2Context *)env)
#define GPU  (CTX->gpu)
#define DEV  (GPU->device)
#define QUE  (GPU->queue)
#define A_I32(n) (args->data[(n)].of.i32)
#define A_F32(n) (args->data[(n)].of.f32)
#define R_I32(v) do { res->data[0] = (wasm_val_t){.kind=WASM_I32,.of.i32=(v)}; } while(0)
#define R_F32(v) do { res->data[0] = (wasm_val_t){.kind=WASM_F32,.of.f32=(v)}; } while(0)

/* ------------------------------------------------------------------ */
/*  Memory helpers                                                     */
/* ------------------------------------------------------------------ */

static inline uint8_t *wasm_mem_base(WebGL2Context *c) {
    return c->memory ? (uint8_t *)wasm_memory_data(c->memory) : NULL;
}
static inline size_t wasm_mem_size(WebGL2Context *c) {
    return c->memory ? wasm_memory_data_size(c->memory) : 0;
}
static inline const char *mem_str(WebGL2Context *c, uint32_t ptr, uint32_t len) {
    if (!len || (size_t)ptr + len > wasm_mem_size(c)) return "";
    return (const char *)(wasm_mem_base(c) + ptr);
}
static inline const char *mem_cstr(WebGL2Context *c, uint32_t ptr) {
    if (!ptr) return "";
    uint8_t *base = wasm_mem_base(c);
    size_t sz = wasm_mem_size(c);
    if ((size_t)ptr >= sz) return "";
    return (const char *)(base + ptr);
}
static inline void *mem_ptr(WebGL2Context *c, uint32_t ptr, uint32_t len) {
    if ((size_t)ptr + len > wasm_mem_size(c)) return NULL;
    return wasm_mem_base(c) + ptr;
}
static inline uint32_t mem_read_u32(WebGL2Context *c, uint32_t ptr) {
    uint32_t v = 0;
    if ((size_t)ptr + 4 <= wasm_mem_size(c)) memcpy(&v, wasm_mem_base(c) + ptr, 4);
    return v;
}
static inline int32_t mem_read_i32(WebGL2Context *c, uint32_t ptr) {
    int32_t v = 0;
    if ((size_t)ptr + 4 <= wasm_mem_size(c)) memcpy(&v, wasm_mem_base(c) + ptr, 4);
    return v;
}
static inline void mem_write_u32(WebGL2Context *c, uint32_t ptr, uint32_t val) {
    if ((size_t)ptr + 4 <= wasm_mem_size(c)) memcpy(wasm_mem_base(c) + ptr, &val, 4);
}
static inline void mem_write_i32(WebGL2Context *c, uint32_t ptr, int32_t val) {
    if ((size_t)ptr + 4 <= wasm_mem_size(c)) memcpy(wasm_mem_base(c) + ptr, &val, 4);
}
static inline void mem_write_f32(WebGL2Context *c, uint32_t ptr, float val) {
    if ((size_t)ptr + 4 <= wasm_mem_size(c)) memcpy(wasm_mem_base(c) + ptr, &val, 4);
}
static inline float mem_read_f32(WebGL2Context *c, uint32_t ptr) {
    float v = 0;
    if ((size_t)ptr + 4 <= wasm_mem_size(c)) memcpy(&v, wasm_mem_base(c) + ptr, 4);
    return v;
}


/* ------------------------------------------------------------------ */
/*  Shared helpers                                                     */
/* ------------------------------------------------------------------ */

static inline uint32_t get_bound_buffer(WebGL2Context *c, uint32_t target) {
    switch (target) {
        case YGL_ARRAY_BUFFER:         return c->current_array_buffer;
        case YGL_ELEMENT_ARRAY_BUFFER: return c->current_element_buffer;
        case YGL_UNIFORM_BUFFER:       return c->current_uniform_buffer;
        default:                       return 0;
    }
}

/* Component byte size for a GL type enum */
static inline uint32_t gl_type_byte_size(uint32_t type) {
    switch (type) {
        case YGL_BYTE: case YGL_UNSIGNED_BYTE: return 1;
        case YGL_SHORT: case YGL_UNSIGNED_SHORT: case YGL_HALF_FLOAT: return 2;
        case YGL_INT: case YGL_UNSIGNED_INT: case YGL_FLOAT: return 4;
        case YGL_UNSIGNED_INT_2_10_10_10_REV: return 4; /* packed */
        default: return 4;
    }
}

/* Tightly-packed byte stride for a vertex attribute */
static inline uint32_t gl_attrib_tight_stride(int size, uint32_t type) {
    if (type == YGL_UNSIGNED_INT_2_10_10_10_REV) return 4;
    return (uint32_t)size * gl_type_byte_size(type);
}

/* ------------------------------------------------------------------ */
/*  Forward declarations for cross-file references                     */
/* ------------------------------------------------------------------ */

/* gles_enums.c */
WGPUPrimitiveTopology  gl_topology(uint32_t mode);
WGPUVertexFormat       gl_attrib_format(int size, uint32_t type, bool norm);
WGPUBlendFactor        gl_blend_factor(uint32_t f);
WGPUBlendOperation     gl_blend_eq(uint32_t e);
WGPUCompareFunction    gl_compare_func(uint32_t f);
WGPUCullMode           gl_cull_mode(uint32_t m);
WGPUFrontFace          gl_front_face(uint32_t f);
WGPUIndexFormat        gl_index_type(uint32_t type);
WGPUTextureFormat      gl_tex_format(uint32_t internal);

WGPUShaderModule   get_blit_shader(WebGL2Context *c);
WGPUSampler        get_blit_sampler(WebGL2Context *c);

/* gles_pipeline.c */
WGPURenderPipeline build_pipeline(WebGL2Context *c, uint32_t mode);
void               ensure_pass(WebGL2Context *c);
void               break_pass(WebGL2Context *c);
void               upload_dirty_ubos(WebGL2Context *c, ProgramObject *po);
void               bind_resources(WebGL2Context *c, ProgramObject *po);

/* FBO helpers (gles_pipeline.c) */
void               begin_render_pass(WebGL2Context *c, WGPULoadOp color_load, WGPULoadOp depth_load);
WGPUTextureView    get_attachment_view(WebGL2Context *c, uint32_t handle, bool is_rb);
WGPUTextureFormat  get_current_color_format(WebGL2Context *c);
WGPUTextureFormat  get_current_depth_format(WebGL2Context *c);

/* Topology emulation (gles_pipeline.c) */
WGPUBuffer         build_fan_indices(WebGL2Context *c, uint32_t first, uint32_t count, uint32_t *out_idx_count);
WGPUBuffer         build_loop_indices(WebGL2Context *c, uint32_t first, uint32_t count, uint32_t *out_idx_count);

/* Blit / mipmap helpers (gles_pipeline.c) */
void               generate_mipmaps_blit(WebGL2Context *c, TextureObject *to);
uint32_t           wgpu_format_bpp(WGPUTextureFormat fmt);
uint32_t           gl_pixel_size(uint32_t format, uint32_t type);

/* gles_draw.c */
void               bind_vao_buffers(WebGL2Context *c);

WGPUBuffer         build_fan_indices_indexed(WebGL2Context *c, const uint8_t *idx_data,
                                              uint32_t count, uint32_t idx_type,
                                              uint32_t *out_idx_count);
WGPUBuffer         build_loop_indices_indexed(WebGL2Context *c, const uint8_t *idx_data,
                                               uint32_t count, uint32_t idx_type,
                                               uint32_t *out_idx_count);
WGPUShaderModule   get_blit_shader(WebGL2Context *c);
WGPUSampler        get_blit_sampler(WebGL2Context *c);
/* ------------------------------------------------------------------ */
/*  All fn_* callbacks                                                 */
/* ------------------------------------------------------------------ */

/* gles_state.c */
wasm_trap_t *fn_enable(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_disable(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isEnabled(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_clearColor(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_clearDepthf(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_clearStencil(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_clear(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_viewport(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_scissor(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_colorMask(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_depthMask(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_stencilMask(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_blendFunc(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_blendFuncSeparate(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_blendEquation(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_blendEquationSeparate(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_blendColor(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_depthFunc(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_depthRangef(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_cullFace(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_frontFace(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_stencilFunc(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_stencilFuncSeparate(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_stencilOp(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_stencilOpSeparate(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_stencilMaskSeparate(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_lineWidth(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_polygonOffset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_pixelStorei(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_hint(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_sampleCoverage(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getError(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getIntegerv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getBooleanv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getFloatv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getIntegeri_v(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getInteger64v(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getInteger64i_v(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getString(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getStringi(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_flush(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_finish(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_clearBufferfv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_clearBufferiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_clearBufferuiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_clearBufferfi(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_readBuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);

/* gles_buffers.c */
wasm_trap_t *fn_genBuffers(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteBuffers(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindBuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindBufferRange(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindBufferBase(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bufferData(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bufferSubData(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_copyBufferSubData(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_mapBufferRange(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_unmapBuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_flushMappedBufferRange(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_genVertexArrays(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteVertexArrays(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindVertexArray(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_enableVertexAttribArray(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_disableVertexAttribArray(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttribPointer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttribIPointer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttribDivisor(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttrib1f(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttrib2f(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttrib3f(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttrib4f(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttrib4fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttribI4i(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttribI4ui(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttribI4iv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_vertexAttribI4uiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);

/* gles_textures.c */
wasm_trap_t *fn_genTextures(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteTextures(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_activeTexture(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindTexture(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texParameteri(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texParameterf(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texParameterfv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texParameteriv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texImage2D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texSubImage2D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texImage3D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texSubImage3D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texStorage2D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_texStorage3D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_compressedTexImage2D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_compressedTexImage3D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_compressedTexSubImage2D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_compressedTexSubImage3D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_copyTexSubImage2D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_copyTexSubImage3D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_generateMipmap(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_genSamplers(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteSamplers(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindSampler(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_samplerParameteri(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_samplerParameterf(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_samplerParameteriv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_samplerParameterfv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);

/* gles_framebuffers.c */
wasm_trap_t *fn_genRenderbuffers(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteRenderbuffers(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindRenderbuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_renderbufferStorage(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_renderbufferStorageMultisample(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_genFramebuffers(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteFramebuffers(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindFramebuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_framebufferTexture2D(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_framebufferTextureLayer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_framebufferRenderbuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_checkFramebufferStatus(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_drawBuffers(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_blitFramebuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_invalidateFramebuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_invalidateSubFramebuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_readPixels(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);

/* gles_shaders.c */
wasm_trap_t *fn_createShader(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_shaderSource(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_compileShader(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getShaderiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getShaderInfoLog(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteShader(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_createProgram(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_attachShader(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_detachShader(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_linkProgram(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_validateProgram(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindAttribLocation(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_useProgram(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getProgramiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getProgramInfoLog(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteProgram(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getUniformLocation(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getAttribLocation(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getActiveUniform(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getActiveAttrib(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getUniformBlockIndex(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformBlockBinding(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getUniformIndices(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getFragDataLocation(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getShaderSource(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform1i(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform1f(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform2f(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform3f(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform4f(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform1iv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform2iv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform3iv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform4iv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform1fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform2fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform3fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform4fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformMatrix2fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformMatrix3fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformMatrix4fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformMatrix2x3fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformMatrix3x2fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformMatrix2x4fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformMatrix4x2fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformMatrix3x4fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniformMatrix4x3fv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform1ui(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform2ui(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform3ui(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform4ui(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform1uiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform2uiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform3uiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_uniform4uiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);

/* gles_draw.c */
wasm_trap_t *fn_drawArrays(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_drawElements(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_drawArraysInstanced(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_drawElementsInstanced(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_drawRangeElements(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);

/* gles_queries.c */
wasm_trap_t *fn_genQueries(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteQueries(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_beginQuery(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_endQuery(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getQueryiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getQueryObjectuiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isQuery(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_fenceSync(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteSync(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_clientWaitSync(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_waitSync(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getSynciv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isSync(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_beginTransformFeedback(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_endTransformFeedback(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_transformFeedbackVaryings(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getTransformFeedbackVarying(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_bindTransformFeedback(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_genTransformFeedbacks(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_deleteTransformFeedbacks(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_pauseTransformFeedback(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_resumeTransformFeedback(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isTransformFeedback(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isBuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isTexture(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isFramebuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isRenderbuffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isShader(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isProgram(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isSampler(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_isVertexArray(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getBufferParameteriv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getBufferParameteri64v(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getRenderbufferParameteriv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getFramebufferAttachmentParameteriv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getVertexAttribiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getVertexAttribfv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getVertexAttribPointerv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getTexParameteriv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getTexParameterfv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getSamplerParameteriv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getSamplerParameterfv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getUniformfv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getUniformiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getUniformuiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getActiveUniformsiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getActiveUniformBlockiv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getActiveUniformBlockName(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getBufferPointerv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getInternalformativ(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getProgramBinary(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_programBinary(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_programParameteri(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
wasm_trap_t *fn_getBooleani_v(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res);
#endif /* GLES_INTERNAL_H */
