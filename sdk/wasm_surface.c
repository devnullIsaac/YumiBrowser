/*
    Yumi SDK — GPU Surface Compositor Implementation
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

#include "wasm_surface.h"
#include <string.h>

#include "shaders/blit_blend.h"
#include "shaders/blit_simple.h"

/* ================================================================== */
/*  Debug logging                                                      */
/* ================================================================== */
#ifdef SURFACE_DEBUG
#include "wasm_log.h"
#define SLOG(s)             LOG(s)
#define SLOG_INT(l, v)      LOG_VAL_INT(l, v)
#define SLOG_FLOAT(l, v)    LOG_VAL_FLOAT(l, v)
#define SLOG_PTR(l, p)      LOG_VAL_INT(l, (int)(uintptr_t)(p))
#else
#define SLOG(s)
#define SLOG_INT(l, v)
#define SLOG_FLOAT(l, v)
#define SLOG_PTR(l, p)
#endif

/* ================================================================== */
/*  Pool                                                               */
/* ================================================================== */

static struct GPUSurface_s g_pool[GPU_SURFACE_MAX];
static int                 g_pool_used[GPU_SURFACE_MAX];

static GPUSurface pool_alloc(void)
{
    for (int i = 0; i < GPU_SURFACE_MAX; i++) {
        if (!g_pool_used[i]) {
            g_pool_used[i] = 1;
            memset(&g_pool[i], 0, sizeof(struct GPUSurface_s));
            SLOG_INT("pool_alloc slot", i);
            return &g_pool[i];
        }
    }
    SLOG("pool_alloc: POOL EXHAUSTED");
    return 0;
}

static void pool_free(GPUSurface s)
{
    int idx = (int)(s - g_pool);
    if (idx >= 0 && idx < GPU_SURFACE_MAX) {
        SLOG_INT("pool_free slot", idx);
        g_pool_used[idx] = 0;
        s->type = GPU_SURFACE_TYPE_INVALID;
    } else {
        SLOG_INT("pool_free: BAD INDEX", idx);
    }
}

/* ================================================================== */
/*  Overallocation policy                                              */
/* ================================================================== */

#define SURFACE_OVERALLOC_FACTOR  1.15f
#define SURFACE_SHRINK_THRESHOLD  0.30f

static uint32_t overalloc_dim(uint32_t d)
{
    uint32_t r = (uint32_t)((float)d * SURFACE_OVERALLOC_FACTOR + 0.5f);
    return r > d ? r : d + 1;
}

/* ================================================================== */
/*  Globals                                                            */
/* ================================================================== */

static gpu_device_t  g_dev;
static gpu_queue_t   g_queue;
static int           g_surface_fmt;
static int           g_inited = 0;

static gpu_buffer_t  g_quad_buf;
static gpu_sampler_t g_sampler;
static gpu_buffer_t  g_blit_ubuf;

static gpu_vertex_attr_t          g_vtx_attr;
static gpu_vertex_buffer_layout_t g_vtx_layout;

static gpu_bind_group_layout_t g_bgl_simple;
static gpu_bind_group_layout_t g_bgl_blend;

static gpu_pipeline_layout_t g_playout_simple;
static gpu_pipeline_layout_t g_playout_blend;

static gpu_render_pipeline_t g_pipe_hw[GPU_BLEND_HW_MAX];
static gpu_render_pipeline_t g_pipe_premul;   /* GPU_BLEND_PREMULTIPLIED */
static gpu_render_pipeline_t g_pipe_blend;
static gpu_render_pipeline_t g_pipe_present;

static gpu_shader_t            g_occluder_vs;
static gpu_shader_t            g_occluder_fs;
static gpu_bind_group_layout_t g_bgl_occluder;
static gpu_pipeline_layout_t   g_playout_occluder;
static gpu_render_pipeline_t   g_pipe_occluder;
static gpu_buffer_t            g_occluder_ubuf;

typedef struct __attribute__((packed, aligned(4))) {
    float rect[4];
    float resolution[2];
    float depth;
    float pad;
} gpu_occluder_uniforms_t;

static const float QUAD_VERTS[] = {
    -1.0f, -1.0f,   1.0f, -1.0f,  -1.0f,  1.0f,
    -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
};

/* ================================================================== */
/*  Internal helpers                                                   */
/* ================================================================== */

static uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

/* ================================================================== */
/*  Validation helper                                                  */
/* ================================================================== */

#ifdef SURFACE_DEBUG
static int surface_slot(GPUSurface s) {
    if (!s) return -1;
    return (int)(s - g_pool);
}

static void validate_surface(GPUSurface s, const char *ctx) {
    if (!s) {
        SLOG("VALIDATE: NULL surface");
        log_write(ctx, (int)str_len(ctx));
        return;
    }
    int slot = surface_slot(s);
    if (slot < 0 || slot >= GPU_SURFACE_MAX) {
        SLOG_INT("VALIDATE: bad slot", slot);
        log_write(ctx, (int)str_len(ctx));
        return;
    }
    if (!g_pool_used[slot]) {
        SLOG_INT("VALIDATE: FREED surface used! slot", slot);
        log_write(ctx, (int)str_len(ctx));
    }
    if (s->type == GPU_SURFACE_TYPE_INVALID) {
        SLOG_INT("VALIDATE: INVALID type surface! slot", slot);
        log_write(ctx, (int)str_len(ctx));
    }
    if (!s->view_a) {
        SLOG_INT("VALIDATE: NULL view_a! slot", slot);
        log_write(ctx, (int)str_len(ctx));
    }
    if (!s->tex_a) {
        SLOG_INT("VALIDATE: NULL tex_a! slot", slot);
        log_write(ctx, (int)str_len(ctx));
    }
}
#define VALIDATE(s, ctx) validate_surface(s, ctx)
#else
#define VALIDATE(s, ctx)
#endif

/* ================================================================== */
/*  Internal: create a texture + view pair (color)                     */
/* ================================================================== */

static void create_tex_pair(uint32_t w, uint32_t h,
                            gpu_texture_t *out_tex,
                            gpu_texture_view_t *out_view)
{
    gpu_texture_desc_t td;
    memset(&td, 0, sizeof(td));
    td.usage     = GPU_TEXTURE_USAGE_RENDER_ATTACHMENT
                 | GPU_TEXTURE_USAGE_TEXTURE_BINDING
                 | GPU_TEXTURE_USAGE_COPY_SRC
                 | GPU_TEXTURE_USAGE_COPY_DST;
    td.dimension = GPU_TEXTURE_DIM_2D;
    td.width     = w;
    td.height    = h;
    td.depth_or_array_layers = 1;
    td.format    = GPU_TEX_FORMAT_RGBA8Unorm;
    td.mip_level_count = 1;
    td.sample_count    = 1;
    *out_tex = wgpu_create_texture(&td);

    gpu_texture_view_desc_t vd;
    memset(&vd, 0, sizeof(vd));
    vd.format            = GPU_TEX_FORMAT_RGBA8Unorm;
    vd.dimension         = GPU_VIEW_DIM_2D;
    vd.base_mip_level    = 0;
    vd.mip_level_count   = 1;
    vd.base_array_layer  = 0;
    vd.array_layer_count = 1;
    *out_view = wgpu_create_texture_view(*out_tex, &vd);

    SLOG_PTR("  tex_pair: tex", *out_tex);
    SLOG_PTR("  tex_pair: view", *out_view);
}

static void create_depth(uint32_t w, uint32_t h,
                         gpu_texture_t *out_tex,
                         gpu_texture_view_t *out_view)
{
    gpu_texture_desc_t td;
    memset(&td, 0, sizeof(td));
    td.usage     = GPU_TEXTURE_USAGE_RENDER_ATTACHMENT;
    td.dimension = GPU_TEXTURE_DIM_2D;
    td.width     = w;
    td.height    = h;
    td.depth_or_array_layers = 1;
    td.format    = GPU_SURFACE_DEPTH_FORMAT;
    td.mip_level_count = 1;
    td.sample_count    = 1;
    *out_tex = wgpu_create_texture(&td);

    gpu_texture_view_desc_t vd;
    memset(&vd, 0, sizeof(vd));
    vd.format            = GPU_SURFACE_DEPTH_FORMAT;
    vd.dimension         = GPU_VIEW_DIM_2D;
    vd.base_mip_level    = 0;
    vd.mip_level_count   = 1;
    vd.base_array_layer  = 0;
    vd.array_layer_count = 1;
    *out_view = wgpu_create_texture_view(*out_tex, &vd);
}

/* ================================================================== */
/*  Internal: create a render pipeline with custom blend + entries     */
/* ================================================================== */

static gpu_render_pipeline_t create_blit_pipeline(
    gpu_pipeline_layout_t layout,
    gpu_shader_t vs, const char *vs_entry,
    gpu_shader_t fs, const char *fs_entry,
    int target_fmt,
    int blend_enabled,
    int color_src, int color_dst, int color_op,
    int alpha_src, int alpha_dst, int alpha_op)
{
    if (!vs_entry) vs_entry = "main";
    if (!fs_entry) fs_entry = "main";

    gpu_color_target_t ct;
    memset(&ct, 0, sizeof(ct));
    ct.format           = (uint32_t)target_fmt;
    ct.write_mask       = GPU_COLOR_WRITE_ALL;
    ct.blend_enabled    = (uint32_t)blend_enabled;
    ct.color_src_factor = (uint32_t)color_src;
    ct.color_dst_factor = (uint32_t)color_dst;
    ct.color_operation  = (uint32_t)color_op;
    ct.alpha_src_factor = (uint32_t)alpha_src;
    ct.alpha_dst_factor = (uint32_t)alpha_dst;
    ct.alpha_operation  = (uint32_t)alpha_op;

    uint32_t topo = (uint32_t)wgpu_get_topology(GPU_TOPO_TRIANGLE_LIST);

    gpu_render_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.layout_h            = layout;
    desc.vs_shader_h         = vs;
    desc.vs_entry_ptr        = (uint32_t)(uintptr_t)vs_entry;
    desc.vs_entry_len        = str_len(vs_entry);
    desc.fs_shader_h         = fs;
    desc.fs_entry_ptr        = (uint32_t)(uintptr_t)fs_entry;
    desc.fs_entry_len        = str_len(fs_entry);
    desc.topology            = topo;
    desc.strip_index_format  = 0;
    desc.cull_mode           = GPU_CULL_NONE;
    desc.front_face          = GPU_FRONT_FACE_CCW;
    desc.color_target_count  = 1;
    desc.color_targets_ptr   = (uint32_t)(uintptr_t)&ct;
    desc.has_depth_stencil   = 0;
    desc.depth_stencil_ptr   = 0;
    desc.vertex_buffer_count = 1;
    desc.vertex_buffers_ptr  = (uint32_t)(uintptr_t)&g_vtx_layout;
    desc.sample_count        = 1;
    desc.sample_mask         = 0xFFFFFFFF;
    desc.alpha_to_coverage   = 0;

    return wgpu_create_render_pipeline_desc(&desc);
}

/* ================================================================== */
/*  Public pipeline creation — 2D (no depth)                           */
/* ================================================================== */

gpu_render_pipeline_t GPUSurface_create_pipeline(
    gpu_pipeline_layout_t layout,
    gpu_shader_t vs, const char *vs_entry,
    gpu_shader_t fs, const char *fs_entry,
    int target_fmt,
    int blend_enabled,
    int color_src, int color_dst, int color_op,
    int alpha_src, int alpha_dst, int alpha_op)
{
    return create_blit_pipeline(layout,
        vs, vs_entry, fs, fs_entry,
        target_fmt, blend_enabled,
        color_src, color_dst, color_op,
        alpha_src, alpha_dst, alpha_op);
}

/* ================================================================== */
/*  Public pipeline creation — 3D (with depth)                         */
/* ================================================================== */

gpu_render_pipeline_t GPUSurface_create_pipeline_depth(
    gpu_pipeline_layout_t layout,
    gpu_shader_t vs, const char *vs_entry,
    gpu_shader_t fs, const char *fs_entry,
    int target_fmt,
    int blend_enabled,
    int color_src, int color_dst, int color_op,
    int alpha_src, int alpha_dst, int alpha_op,
    int depth_write,
    int depth_compare)
{
    if (!vs_entry) vs_entry = "main";
    if (!fs_entry) fs_entry = "main";

    gpu_color_target_t ct;
    memset(&ct, 0, sizeof(ct));
    ct.format           = (uint32_t)target_fmt;
    ct.write_mask       = GPU_COLOR_WRITE_ALL;
    ct.blend_enabled    = (uint32_t)blend_enabled;
    ct.color_src_factor = (uint32_t)color_src;
    ct.color_dst_factor = (uint32_t)color_dst;
    ct.color_operation  = (uint32_t)color_op;
    ct.alpha_src_factor = (uint32_t)alpha_src;
    ct.alpha_dst_factor = (uint32_t)alpha_dst;
    ct.alpha_operation  = (uint32_t)alpha_op;

    gpu_depth_stencil_t ds;
    memset(&ds, 0, sizeof(ds));
    ds.format              = GPU_SURFACE_DEPTH_FORMAT;
    ds.depth_write_enabled = depth_write;
    ds.depth_compare       = (uint32_t)depth_compare;

    uint32_t topo = (uint32_t)wgpu_get_topology(GPU_TOPO_TRIANGLE_LIST);

    gpu_render_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.layout_h            = layout;
    desc.vs_shader_h         = vs;
    desc.vs_entry_ptr        = (uint32_t)(uintptr_t)vs_entry;
    desc.vs_entry_len        = str_len(vs_entry);
    desc.fs_shader_h         = fs;
    desc.fs_entry_ptr        = (uint32_t)(uintptr_t)fs_entry;
    desc.fs_entry_len        = str_len(fs_entry);
    desc.topology            = topo;
    desc.strip_index_format  = 0;
    desc.cull_mode           = GPU_CULL_NONE;
    desc.front_face          = GPU_FRONT_FACE_CCW;
    desc.color_target_count  = 1;
    desc.color_targets_ptr   = (uint32_t)(uintptr_t)&ct;
    desc.has_depth_stencil   = 1;
    desc.depth_stencil_ptr   = (uint32_t)(uintptr_t)&ds;
    desc.vertex_buffer_count = 1;
    desc.vertex_buffers_ptr  = (uint32_t)(uintptr_t)&g_vtx_layout;
    desc.sample_count        = 1;
    desc.sample_mask         = 0xFFFFFFFF;
    desc.alpha_to_coverage   = 0;

    return wgpu_create_render_pipeline_desc(&desc);
}

/* ================================================================== */
/*  Internal: occluder initialization                                  */
/* ================================================================== */

static const char k_occluder_vs[] =
    "struct P { rect: vec4f, res: vec2f, depth: f32, pad: f32 }\n"
    "@group(0) @binding(0) var<uniform> p: P;\n"
    "@vertex fn vs_main(@location(0) pos: vec2f)"
    " -> @builtin(position) vec4f {\n"
    "  let uv = pos * 0.5 + 0.5;\n"
    "  let px = p.rect.x + uv.x * p.rect.z;\n"
    "  let py = p.rect.y + (1.0 - uv.y) * p.rect.w;\n"
    "  let cx = px / p.res.x * 2.0 - 1.0;\n"
    "  let cy = 1.0 - py / p.res.y * 2.0;\n"
    "  return vec4f(cx, cy, p.depth, 1.0);\n"
    "}";

static const char k_occluder_fs[] =
    "@fragment fn fs_main() -> @location(0) vec4f {\n"
    "  return vec4f(0.0);\n"
    "}";

static void init_occluder(void)
{
    g_occluder_vs = wgpu_create_shader_wgsl(g_dev, k_occluder_vs,
                                            (int)sizeof(k_occluder_vs) - 1);
    g_occluder_fs = wgpu_create_shader_wgsl(g_dev, k_occluder_fs,
                                            (int)sizeof(k_occluder_fs) - 1);

    gpu_bind_group_layout_entry_t entries[1] = {
        { .binding = 0, .visibility = GPU_SHADER_STAGE_VERTEX,
          .type = GPU_BIND_TYPE_BUFFER,
          .field0 = GPU_BUFFER_TYPE_UNIFORM, .field1 = 0,
          .field2 = sizeof(gpu_occluder_uniforms_t) },
    };
    g_bgl_occluder = wgpu_create_bind_group_layout(g_dev, entries, 1);
    g_playout_occluder = wgpu_create_pipeline_layout(g_dev, &g_bgl_occluder, 1);

    g_occluder_ubuf = wgpu_create_buffer(g_dev,
        GPU_BUFFER_USAGE_UNIFORM | GPU_BUFFER_USAGE_COPY_DST,
        sizeof(gpu_occluder_uniforms_t), 0);

    gpu_color_target_t ct;
    memset(&ct, 0, sizeof(ct));
    ct.format     = GPU_TEX_FORMAT_RGBA8Unorm;
    ct.write_mask = 0;

    gpu_depth_stencil_t ds;
    memset(&ds, 0, sizeof(ds));
    ds.format              = GPU_SURFACE_DEPTH_FORMAT;
    ds.depth_write_enabled = 1;
    ds.depth_compare       = GPU_COMPARE_ALWAYS;

    uint32_t topo = (uint32_t)wgpu_get_topology(GPU_TOPO_TRIANGLE_LIST);

    gpu_render_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.layout_h            = g_playout_occluder;
    desc.vs_shader_h         = g_occluder_vs;
    desc.vs_entry_ptr        = (uint32_t)(uintptr_t)"vs_main";
    desc.vs_entry_len        = 7;
    desc.fs_shader_h         = g_occluder_fs;
    desc.fs_entry_ptr        = (uint32_t)(uintptr_t)"fs_main";
    desc.fs_entry_len        = 7;
    desc.topology            = topo;
    desc.strip_index_format  = 0;
    desc.cull_mode           = GPU_CULL_NONE;
    desc.front_face          = GPU_FRONT_FACE_CCW;
    desc.color_target_count  = 1;
    desc.color_targets_ptr   = (uint32_t)(uintptr_t)&ct;
    desc.has_depth_stencil   = 1;
    desc.depth_stencil_ptr   = (uint32_t)(uintptr_t)&ds;
    desc.vertex_buffer_count = 1;
    desc.vertex_buffers_ptr  = (uint32_t)(uintptr_t)&g_vtx_layout;
    desc.sample_count        = 1;
    desc.sample_mask         = 0xFFFFFFFF;
    desc.alpha_to_coverage   = 0;

    g_pipe_occluder = wgpu_create_render_pipeline_desc(&desc);
}

/* ================================================================== */
/*  Init                                                               */
/* ================================================================== */

void GPUSurface_init(void)
{
    if (g_inited) return;
    g_inited = 1;

    SLOG("GPUSurface_init");

    memset(g_pool, 0, sizeof(g_pool));
    memset(g_pool_used, 0, sizeof(g_pool_used));

    g_dev         = wgpu_get_device();
    g_queue       = wgpu_get_queue();
    g_surface_fmt = wgpu_get_surface_format();

    g_quad_buf = wgpu_create_buffer(g_dev,
        GPU_BUFFER_USAGE_VERTEX | GPU_BUFFER_USAGE_COPY_DST,
        (int)sizeof(QUAD_VERTS), 0);
    wgpu_buffer_write(g_quad_buf, 0, QUAD_VERTS, (int)sizeof(QUAD_VERTS));

    gpu_sampler_desc_t sd;
    memset(&sd, 0, sizeof(sd));
    sd.address_mode_u = GPU_ADDRESS_CLAMP_TO_EDGE;
    sd.address_mode_v = GPU_ADDRESS_CLAMP_TO_EDGE;
    sd.address_mode_w = GPU_ADDRESS_CLAMP_TO_EDGE;
    sd.mag_filter     = GPU_FILTER_LINEAR;
    sd.min_filter     = GPU_FILTER_LINEAR;
    sd.mipmap_filter  = GPU_MIPMAP_NEAREST;
    sd.lod_min_clamp  = 0.0f;
    sd.lod_max_clamp  = 1.0f;
    sd.compare        = 0;
    sd.max_anisotropy = 1;
    g_sampler = wgpu_create_sampler(&sd);

    g_blit_ubuf = wgpu_create_buffer(g_dev,
        GPU_BUFFER_USAGE_UNIFORM | GPU_BUFFER_USAGE_COPY_DST,
        sizeof(gpu_blit_uniforms_t), 0);

    g_vtx_attr.format          = (uint32_t)wgpu_get_vertex_format(GPU_FMT_FLOAT32X2);
    g_vtx_attr.offset          = 0;
    g_vtx_attr.shader_location = 0;

    g_vtx_layout.array_stride    = 8;
    g_vtx_layout.step_mode       = GPU_STEP_MODE_VERTEX;
    g_vtx_layout.attribute_count = 1;
    g_vtx_layout.attributes_ptr  = (uint32_t)(uintptr_t)&g_vtx_attr;

    gpu_bind_group_layout_entry_t simple_entries[3] = {
        { .binding = 0, .visibility = GPU_SHADER_STAGE_VERTEX | GPU_SHADER_STAGE_FRAGMENT,
          .type = GPU_BIND_TYPE_BUFFER,
          .field0 = GPU_BUFFER_TYPE_UNIFORM, .field1 = 0,
          .field2 = sizeof(gpu_blit_uniforms_t) },
        { .binding = 1, .visibility = GPU_SHADER_STAGE_FRAGMENT,
          .type = GPU_BIND_TYPE_TEXTURE,
          .field0 = GPU_SAMPLE_TYPE_FLOAT, .field1 = GPU_VIEW_DIM_2D, .field2 = 0 },
        { .binding = 2, .visibility = GPU_SHADER_STAGE_FRAGMENT,
          .type = GPU_BIND_TYPE_SAMPLER,
          .field0 = GPU_SAMPLER_TYPE_FILTERING },
    };
    g_bgl_simple = wgpu_create_bind_group_layout(g_dev, simple_entries, 3);

    gpu_bind_group_layout_entry_t blend_entries[4] = {
        { .binding = 0, .visibility = GPU_SHADER_STAGE_VERTEX | GPU_SHADER_STAGE_FRAGMENT,
          .type = GPU_BIND_TYPE_BUFFER,
          .field0 = GPU_BUFFER_TYPE_UNIFORM, .field1 = 0,
          .field2 = sizeof(gpu_blit_uniforms_t) },
        { .binding = 1, .visibility = GPU_SHADER_STAGE_FRAGMENT,
          .type = GPU_BIND_TYPE_TEXTURE,
          .field0 = GPU_SAMPLE_TYPE_FLOAT, .field1 = GPU_VIEW_DIM_2D, .field2 = 0 },
        { .binding = 2, .visibility = GPU_SHADER_STAGE_FRAGMENT,
          .type = GPU_BIND_TYPE_TEXTURE,
          .field0 = GPU_SAMPLE_TYPE_FLOAT, .field1 = GPU_VIEW_DIM_2D, .field2 = 0 },
        { .binding = 3, .visibility = GPU_SHADER_STAGE_FRAGMENT,
          .type = GPU_BIND_TYPE_SAMPLER,
          .field0 = GPU_SAMPLER_TYPE_NON_FILTERING },
    };
    g_bgl_blend = wgpu_create_bind_group_layout(g_dev, blend_entries, 4);

    g_playout_simple = wgpu_create_pipeline_layout(g_dev, &g_bgl_simple, 1);
    g_playout_blend  = wgpu_create_pipeline_layout(g_dev, &g_bgl_blend, 1);

    gpu_shader_t vs_simple = wgpu_create_shader_wgsl(g_dev,
        (const char*)blit_simple_vertexMain_wgsl, blit_simple_vertexMain_wgsl_len);
    gpu_shader_t fs_simple = wgpu_create_shader_wgsl(g_dev,
        (const char*)blit_simple_fragmentMain_wgsl, blit_simple_fragmentMain_wgsl_len);
    gpu_shader_t vs_blend = wgpu_create_shader_wgsl(g_dev,
        (const char*)blit_blend_vertexMain_wgsl, blit_blend_vertexMain_wgsl_len);
    gpu_shader_t fs_blend = wgpu_create_shader_wgsl(g_dev,
        (const char*)blit_blend_fragmentMain_wgsl, blit_blend_fragmentMain_wgsl_len);

    g_pipe_hw[GPU_BLEND_NORMAL] = create_blit_pipeline(
        g_playout_simple, vs_simple, "vertexMain", fs_simple, "fragmentMain",
        GPU_TEX_FORMAT_RGBA8Unorm, 1,
        GPU_BLEND_SRC_ALPHA, GPU_BLEND_ONE_MINUS_SRC_ALPHA, GPU_BLEND_OP_ADD,
        GPU_BLEND_ONE,       GPU_BLEND_ONE_MINUS_SRC_ALPHA, GPU_BLEND_OP_ADD);

    g_pipe_hw[GPU_BLEND_REPLACE] = create_blit_pipeline(
        g_playout_simple, vs_simple, "vertexMain", fs_simple, "fragmentMain",
        GPU_TEX_FORMAT_RGBA8Unorm, 0, 0, 0, 0, 0, 0, 0);

    g_pipe_hw[GPU_BLEND_ADDITIVE] = create_blit_pipeline(
        g_playout_simple, vs_simple, "vertexMain", fs_simple, "fragmentMain",
        GPU_TEX_FORMAT_RGBA8Unorm, 1,
        GPU_BLEND_SRC_ALPHA, GPU_BLEND_ONE, GPU_BLEND_OP_ADD,
        GPU_BLEND_ONE,       GPU_BLEND_ONE, GPU_BLEND_OP_ADD);

    /* Premultiplied source over destination: src + (1 - src.a) * dst.
     * Used by GPUSurface_compose / GPUSurface_compose_at so chained
     * surface blits don't multiply text/SDF alpha twice and darken
     * AA fringes ("jagged-text" artefact). */
    g_pipe_premul = create_blit_pipeline(
        g_playout_simple, vs_simple, "vertexMain", fs_simple, "fragmentMain",
        GPU_TEX_FORMAT_RGBA8Unorm, 1,
        GPU_BLEND_ONE, GPU_BLEND_ONE_MINUS_SRC_ALPHA, GPU_BLEND_OP_ADD,
        GPU_BLEND_ONE, GPU_BLEND_ONE_MINUS_SRC_ALPHA, GPU_BLEND_OP_ADD);

    g_pipe_blend = create_blit_pipeline(
        g_playout_blend, vs_blend, "vertexMain", fs_blend, "fragmentMain",
        GPU_TEX_FORMAT_RGBA8Unorm, 0, 0, 0, 0, 0, 0, 0);

    g_pipe_present = create_blit_pipeline(
        g_playout_simple, vs_simple, "vertexMain", fs_simple, "fragmentMain",
        g_surface_fmt, 0, 0, 0, 0, 0, 0, 0);

    init_occluder();
}

/* ================================================================== */
/*  Create / Destroy                                                   */
/* ================================================================== */

GPUSurface GPUSurface_create(uint32_t width, uint32_t height)
{
    SLOG_INT("GPUSurface_create w", (int)width);
    SLOG_INT("GPUSurface_create h", (int)height);

    GPUSurface s = pool_alloc();
    if (!s) return 0;

    s->type          = GPU_SURFACE_TYPE_OFFSCREEN;
    s->width         = width;
    s->height        = height;
    s->alloc_width   = width;
    s->alloc_height  = height;
    s->passActive    = 0;
    s->depth_enabled = 0;
    s->swapchain_view = 0;

    create_tex_pair(width, height, &s->tex_a, &s->view_a);
    create_tex_pair(width, height, &s->tex_b, &s->view_b);
    create_depth(width, height, &s->depth_tex, &s->depth_view);

    SLOG_PTR("  created view_a", s->view_a);
    return s;
}

GPUSurface GPUSurface_bind_swapchain(void)
{
    SLOG("GPUSurface_bind_swapchain");
    GPUSurface s = pool_alloc();
    if (!s) return 0;

    uint32_t w = (uint32_t)wgpu_get_surface_width();
    uint32_t h = (uint32_t)wgpu_get_surface_height();
    uint32_t aw = overalloc_dim(w);
    uint32_t ah = overalloc_dim(h);

    s->type          = GPU_SURFACE_TYPE_SWAPCHAIN;
    s->width         = w;
    s->height        = h;
    s->alloc_width   = aw;
    s->alloc_height  = ah;
    s->passActive    = 0;
    s->depth_enabled = 0;
    s->swapchain_view = 0;

    create_tex_pair(aw, ah, &s->tex_a, &s->view_a);
    create_tex_pair(aw, ah, &s->tex_b, &s->view_b);
    create_depth(aw, ah, &s->depth_tex, &s->depth_view);

    return s;
}

void GPUSurface_destroy(GPUSurface surface)
{
    if (!surface) return;

    SLOG_INT("GPUSurface_destroy slot", surface_slot(surface));
    SLOG_PTR("  old view_a", surface->view_a);

    if (surface->passActive) {
        SLOG("  WARNING: destroying surface with active pass!");
        GPUSurface_end(surface);
    }

    if (surface->depth_view) wgpu_texture_view_release(surface->depth_view);
    if (surface->depth_tex)  wgpu_texture_destroy(surface->depth_tex);
    if (surface->view_a) wgpu_texture_view_release(surface->view_a);
    if (surface->tex_a)  wgpu_texture_destroy(surface->tex_a);
    if (surface->view_b) wgpu_texture_view_release(surface->view_b);
    if (surface->tex_b)  wgpu_texture_destroy(surface->tex_b);

    pool_free(surface);
}

void GPUSurface_resize(GPUSurface surface, uint32_t w, uint32_t h)
{
    if (!surface) return;
    if (w == surface->width && h == surface->height) return;

    SLOG_INT("GPUSurface_resize slot", surface_slot(surface));
    SLOG_INT("  new w", (int)w);
    SLOG_INT("  new h", (int)h);

    if (surface->passActive) {
        SLOG("  WARNING: resizing surface with active pass!");
        GPUSurface_end(surface);
    }

    surface->width  = w;
    surface->height = h;

    int need_realloc = 0;

    if (w > surface->alloc_width || h > surface->alloc_height)
        need_realloc = 1;

    if (!need_realloc &&
        (float)w < (float)surface->alloc_width  * SURFACE_SHRINK_THRESHOLD &&
        (float)h < (float)surface->alloc_height * SURFACE_SHRINK_THRESHOLD)
        need_realloc = 1;

    if (!need_realloc) {
        SLOG("  resize: no realloc needed (within alloc bounds)");
        return;
    }

    SLOG("  resize: REALLOCATING textures");
    SLOG_PTR("  old view_a", surface->view_a);

    if (surface->depth_view) wgpu_texture_view_release(surface->depth_view);
    if (surface->depth_tex)  wgpu_texture_destroy(surface->depth_tex);
    if (surface->view_b) wgpu_texture_view_release(surface->view_b);
    if (surface->tex_b)  wgpu_texture_destroy(surface->tex_b);
    if (surface->view_a) wgpu_texture_view_release(surface->view_a);
    if (surface->tex_a)  wgpu_texture_destroy(surface->tex_a);

    uint32_t aw = overalloc_dim(w);
    uint32_t ah = overalloc_dim(h);
    surface->alloc_width  = aw;
    surface->alloc_height = ah;

    create_tex_pair(aw, ah, &surface->tex_a, &surface->view_a);
    create_tex_pair(aw, ah, &surface->tex_b, &surface->view_b);
    create_depth(aw, ah, &surface->depth_tex, &surface->depth_view);

    SLOG_PTR("  new view_a", surface->view_a);
}

/* ================================================================== */
/*  Clear                                                              */
/* ================================================================== */

void GPUSurface_clear(GPUSurface surface, float r, float g, float b, float a)
{
    if (!surface || surface->passActive) return;
    VALIDATE(surface, "clear");

    gpu_command_encoder_t enc = wgpu_create_command_encoder(g_dev);
    gpu_render_pass_t pass = wgpu_begin_render_pass(enc, surface->view_a, r, g, b, a);
    wgpu_render_pass_end(pass);
    gpu_command_buffer_t cmd = wgpu_encoder_finish(enc);
    wgpu_queue_submit(g_queue, cmd);
}

/* ================================================================== */
/*  Render pass — 2D (no depth, original API)                          */
/* ================================================================== */

static void begin_pass_internal(GPUSurface surface, int load_op,
                                float r, float g, float b, float a)
{
    if (!surface) {
        SLOG("begin_pass: NULL surface!");
        return;
    }
    if (surface->passActive) {
        SLOG_INT("begin_pass: ALREADY ACTIVE! slot", surface_slot(surface));
        return;
    }
    VALIDATE(surface, "begin_pass");

    surface->encoder = wgpu_create_command_encoder(g_dev);

    surface->colorAttachment.view_h           = surface->view_a;
    surface->colorAttachment.resolve_target_h = 0;
    surface->colorAttachment.load_op          = (uint32_t)load_op;
    surface->colorAttachment.store_op         = GPU_STORE_OP_STORE;
    surface->colorAttachment.clear_r          = r;
    surface->colorAttachment.clear_g          = g;
    surface->colorAttachment.clear_b          = b;
    surface->colorAttachment.clear_a          = a;

    surface->renderPassDesc.color_attachment_count = 1;
    surface->renderPassDesc.color_attachments_ptr  =
        (uint32_t)(uintptr_t)&surface->colorAttachment;
    surface->renderPassDesc.has_depth_stencil      = 0;
    surface->renderPassDesc.depth_stencil_ptr      = 0;
    surface->renderPassDesc.occlusion_query_set_h  = 0;

    surface->renderPass = wgpu_begin_render_pass_desc(
        surface->encoder, &surface->renderPassDesc);
    surface->passActive    = 1;
    surface->depth_enabled = 0;

    if (surface->alloc_width != surface->width ||
        surface->alloc_height != surface->height)
    {
        wgpu_render_pass_set_viewport(surface->renderPass,
            0.0f, 0.0f, (float)surface->width, (float)surface->height,
            0.0f, 1.0f);
        wgpu_render_pass_set_scissor_rect(surface->renderPass,
            0, 0, (int)surface->width, (int)surface->height);
    }
}

void GPUSurface_begin(GPUSurface surface, float r, float g, float b, float a)
{
    begin_pass_internal(surface, GPU_LOAD_OP_CLEAR, r, g, b, a);
}

void GPUSurface_begin_load(GPUSurface surface)
{
    begin_pass_internal(surface, GPU_LOAD_OP_LOAD, 0, 0, 0, 0);
}

/* ================================================================== */
/*  Render pass — 3D (depth buffer attached)                           */
/* ================================================================== */

static void begin_pass_3d_internal(GPUSurface surface, int load_op,
                                   float r, float g, float b, float a)
{
    if (!surface) {
        SLOG("begin_pass_3d: NULL surface!");
        return;
    }
    if (surface->passActive) {
        SLOG_INT("begin_pass_3d: ALREADY ACTIVE! slot", surface_slot(surface));
        return;
    }
    VALIDATE(surface, "begin_pass_3d");

    surface->encoder = wgpu_create_command_encoder(g_dev);

    surface->colorAttachment.view_h           = surface->view_a;
    surface->colorAttachment.resolve_target_h = 0;
    surface->colorAttachment.load_op          = (uint32_t)load_op;
    surface->colorAttachment.store_op         = GPU_STORE_OP_STORE;
    surface->colorAttachment.clear_r          = r;
    surface->colorAttachment.clear_g          = g;
    surface->colorAttachment.clear_b          = b;
    surface->colorAttachment.clear_a          = a;

    gpu_depth_attachment_t dsa;
    memset(&dsa, 0, sizeof(dsa));
    dsa.view_h            = surface->depth_view;
    dsa.depth_load_op     = (load_op == GPU_LOAD_OP_CLEAR)
                                ? GPU_LOAD_OP_CLEAR : GPU_LOAD_OP_LOAD;
    dsa.depth_store_op    = GPU_STORE_OP_STORE;
    dsa.depth_clear_value = 1.0f;

    surface->renderPassDesc.color_attachment_count = 1;
    surface->renderPassDesc.color_attachments_ptr  =
        (uint32_t)(uintptr_t)&surface->colorAttachment;
    surface->renderPassDesc.has_depth_stencil      = 1;
    surface->renderPassDesc.depth_stencil_ptr      =
        (uint32_t)(uintptr_t)&dsa;
    surface->renderPassDesc.occlusion_query_set_h  = 0;

    surface->renderPass = wgpu_begin_render_pass_desc(
        surface->encoder, &surface->renderPassDesc);
    surface->passActive    = 1;
    surface->depth_enabled = 1;

    if (surface->alloc_width != surface->width ||
        surface->alloc_height != surface->height)
    {
        wgpu_render_pass_set_viewport(surface->renderPass,
            0.0f, 0.0f, (float)surface->width, (float)surface->height,
            0.0f, 1.0f);
        wgpu_render_pass_set_scissor_rect(surface->renderPass,
            0, 0, (int)surface->width, (int)surface->height);
    }
}

void GPUSurface_begin_3d(GPUSurface surface, float r, float g, float b, float a)
{
    begin_pass_3d_internal(surface, GPU_LOAD_OP_CLEAR, r, g, b, a);
}

void GPUSurface_begin_3d_load(GPUSurface surface)
{
    begin_pass_3d_internal(surface, GPU_LOAD_OP_LOAD, 0, 0, 0, 0);
}

/* ================================================================== */
/*  End (shared by 2D and 3D)                                          */
/* ================================================================== */

void GPUSurface_end(GPUSurface surface)
{
    if (!surface) {
        SLOG("GPUSurface_end: NULL surface!");
        return;
    }
    if (!surface->passActive) {
        SLOG_INT("GPUSurface_end: NOT ACTIVE! slot", surface_slot(surface));
        return;
    }

    wgpu_render_pass_end(surface->renderPass);
    gpu_command_buffer_t cmd = wgpu_encoder_finish(surface->encoder);
    wgpu_queue_submit(g_queue, cmd);

    surface->passActive    = 0;
    surface->depth_enabled = 0;
    surface->renderPass    = 0;
    surface->encoder       = 0;
}

/* ================================================================== */
/*  Drawing into a surface                                             */
/* ================================================================== */

void GPUSurface_draw_pipeline(
    GPUSurface surface,
    gpu_render_pipeline_t pipeline,
    gpu_bind_group_t bind_group)
{
    if (!surface || !surface->passActive) {
        SLOG("draw_pipeline: surface NULL or not active!");
        return;
    }

    wgpu_render_pass_set_pipeline(surface->renderPass, pipeline);
    wgpu_render_pass_set_bind_group(surface->renderPass, 0, bind_group, 0, 0);
    wgpu_render_pass_set_vertex_buffer(surface->renderPass, 0,
        g_quad_buf, 0, (int)sizeof(QUAD_VERTS));
    wgpu_render_pass_draw(surface->renderPass, 6, 1, 0, 0);
}

void GPUSurface_draw_pipeline_ex(
    GPUSurface surface,
    gpu_render_pipeline_t pipeline,
    gpu_bind_group_t bind_group,
    gpu_buffer_t vertex_buf,
    int vertex_count,
    int vertex_stride)
{
    if (!surface || !surface->passActive) {
        SLOG("draw_pipeline_ex: surface NULL or not active!");
        return;
    }

    wgpu_render_pass_set_pipeline(surface->renderPass, pipeline);
    wgpu_render_pass_set_bind_group(surface->renderPass, 0, bind_group, 0, 0);
    wgpu_render_pass_set_vertex_buffer(surface->renderPass, 0,
        vertex_buf, 0, vertex_count * vertex_stride);
    wgpu_render_pass_draw(surface->renderPass, vertex_count, 1, 0, 0);
}

/* ================================================================== */
/*  Depth-based occlusion                                              */
/* ================================================================== */

void GPUSurface_draw_occluder(GPUSurface surface,
                              const gpu_rect_t *rect, float depth)
{
    if (!surface || !surface->passActive || !surface->depth_enabled) return;

    gpu_occluder_uniforms_t u;
    u.rect[0]       = (float)rect->x;
    u.rect[1]       = (float)rect->y;
    u.rect[2]       = (float)rect->w;
    u.rect[3]       = (float)rect->h;
    u.resolution[0] = (float)surface->width;
    u.resolution[1] = (float)surface->height;
    u.depth         = depth;
    u.pad           = 0.0f;
    wgpu_buffer_write(g_occluder_ubuf, 0, &u, sizeof(u));

    gpu_bind_group_entry_t ent[1] = {
        { .binding = 0, .type = GPU_ENTRY_BUFFER,
          .handle = g_occluder_ubuf, .field0 = 0,
          .field1 = sizeof(gpu_occluder_uniforms_t) },
    };
    gpu_bind_group_t bg = wgpu_create_bind_group(g_dev, g_bgl_occluder, ent, 1);

    wgpu_render_pass_set_pipeline(surface->renderPass, g_pipe_occluder);
    wgpu_render_pass_set_bind_group(surface->renderPass, 0, bg, 0, 0);
    wgpu_render_pass_set_vertex_buffer(surface->renderPass, 0,
        g_quad_buf, 0, (int)sizeof(QUAD_VERTS));
    wgpu_render_pass_draw(surface->renderPass, 6, 1, 0, 0);
}

/* ================================================================== */
/*  Blit internals                                                     */
/* ================================================================== */

static void compute_uniforms(
    GPUSurface src, const gpu_rect_t *src_rect,
    GPUSurface dst, const gpu_rect_t *dst_rect,
    GPU_BLEND_MODE mode, float opacity,
    gpu_blit_uniforms_t *u)
{
    if (src_rect) {
        u->src_uv[0] = (float)src_rect->x / (float)src->alloc_width;
        u->src_uv[1] = (float)src_rect->y / (float)src->alloc_height;
        u->src_uv[2] = (float)src_rect->w / (float)src->alloc_width;
        u->src_uv[3] = (float)src_rect->h / (float)src->alloc_height;
    } else {
        u->src_uv[0] = 0.0f;
        u->src_uv[1] = 0.0f;
        u->src_uv[2] = (float)src->width / (float)src->alloc_width;
        u->src_uv[3] = (float)src->height / (float)src->alloc_height;
    }

    u->dst_size[0] = (float)dst->width;
    u->dst_size[1] = (float)dst->height;
    u->opacity     = opacity;
    u->blend_mode  = (uint32_t)mode;
}

static void get_dst_rect(GPUSurface dst, const gpu_rect_t *rect,
                         int *x, int *y, int *w, int *h)
{
    if (rect) {
        *x = rect->x;  *y = rect->y;
        *w = rect->w;  *h = rect->h;
    } else {
        *x = 0;  *y = 0;
        *w = (int)dst->width;
        *h = (int)dst->height;
    }
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > (int)dst->width)  *w = (int)dst->width - *x;
    if (*y + *h > (int)dst->height) *h = (int)dst->height - *y;
}

/* ================================================================== */
/*  Blit — HW blend path                                               */
/* ================================================================== */

static void blit_hw(
    GPUSurface src, GPUSurface dst,
    const gpu_blit_uniforms_t *u,
    int dx, int dy, int dw, int dh,
    GPU_BLEND_MODE mode)
{
    VALIDATE(src, "blit_hw src");
    VALIDATE(dst, "blit_hw dst");

    gpu_bind_group_entry_t entries[3] = {
        { .binding = 0, .type = GPU_ENTRY_BUFFER,
          .handle = g_blit_ubuf, .field0 = 0,
          .field1 = sizeof(gpu_blit_uniforms_t) },
        { .binding = 1, .type = GPU_ENTRY_TEXTURE_VIEW,
          .handle = src->view_a, .field0 = 0, .field1 = 0 },
        { .binding = 2, .type = GPU_ENTRY_SAMPLER,
          .handle = g_sampler, .field0 = 0, .field1 = 0 },
    };
    gpu_bind_group_t bg = wgpu_create_bind_group(g_dev, g_bgl_simple, entries, 3);

    gpu_color_attachment_t ca;
    memset(&ca, 0, sizeof(ca));
    ca.view_h  = dst->view_a;
    ca.load_op = GPU_LOAD_OP_LOAD;
    ca.store_op = GPU_STORE_OP_STORE;

    gpu_render_pass_desc_t rpd;
    memset(&rpd, 0, sizeof(rpd));
    rpd.color_attachment_count = 1;
    rpd.color_attachments_ptr  = (uint32_t)(uintptr_t)&ca;

    gpu_command_encoder_t enc = wgpu_create_command_encoder(g_dev);
    gpu_render_pass_t pass = wgpu_begin_render_pass_desc(enc, &rpd);

    wgpu_render_pass_set_viewport(pass,
        (float)dx, (float)dy, (float)dw, (float)dh, 0.0f, 1.0f);
    wgpu_render_pass_set_scissor_rect(pass, dx, dy, dw, dh);

    gpu_render_pipeline_t pipe =
        (mode == GPU_BLEND_PREMULTIPLIED) ? g_pipe_premul
                                          : g_pipe_hw[mode];
    wgpu_render_pass_set_pipeline(pass, pipe);
    wgpu_render_pass_set_bind_group(pass, 0, bg, 0, 0);
    wgpu_render_pass_set_vertex_buffer(pass, 0,
        g_quad_buf, 0, (int)sizeof(QUAD_VERTS));
    wgpu_render_pass_draw(pass, 6, 1, 0, 0);

    wgpu_render_pass_end(pass);
    gpu_command_buffer_t cmd = wgpu_encoder_finish(enc);
    wgpu_queue_submit(g_queue, cmd);
}

/* ================================================================== */
/*  Blit — Shader blend path (ping-pong)                               */
/* ================================================================== */

static void blit_shader(
    GPUSurface src, GPUSurface dst,
    const gpu_blit_uniforms_t *u,
    int dx, int dy, int dw, int dh)
{
    VALIDATE(src, "blit_shader src");
    VALIDATE(dst, "blit_shader dst");

    gpu_bind_group_entry_t entries[4] = {
        { .binding = 0, .type = GPU_ENTRY_BUFFER,
          .handle = g_blit_ubuf, .field0 = 0,
          .field1 = sizeof(gpu_blit_uniforms_t) },
        { .binding = 1, .type = GPU_ENTRY_TEXTURE_VIEW,
          .handle = src->view_a, .field0 = 0, .field1 = 0 },
        { .binding = 2, .type = GPU_ENTRY_TEXTURE_VIEW,
          .handle = dst->view_a, .field0 = 0, .field1 = 0 },
        { .binding = 3, .type = GPU_ENTRY_SAMPLER,
          .handle = g_sampler, .field0 = 0, .field1 = 0 },
    };
    gpu_bind_group_t bg = wgpu_create_bind_group(g_dev, g_bgl_blend, entries, 4);

    gpu_color_attachment_t ca;
    memset(&ca, 0, sizeof(ca));
    ca.view_h   = dst->view_b;
    ca.load_op  = GPU_LOAD_OP_CLEAR;
    ca.store_op = GPU_STORE_OP_STORE;
    ca.clear_r  = 0.0f;
    ca.clear_g  = 0.0f;
    ca.clear_b  = 0.0f;
    ca.clear_a  = 0.0f;

    gpu_render_pass_desc_t rpd;
    memset(&rpd, 0, sizeof(rpd));
    rpd.color_attachment_count = 1;
    rpd.color_attachments_ptr  = (uint32_t)(uintptr_t)&ca;

    gpu_command_encoder_t enc = wgpu_create_command_encoder(g_dev);
    gpu_render_pass_t pass = wgpu_begin_render_pass_desc(enc, &rpd);

    wgpu_render_pass_set_viewport(pass,
        (float)dx, (float)dy, (float)dw, (float)dh, 0.0f, 1.0f);
    wgpu_render_pass_set_scissor_rect(pass, dx, dy, dw, dh);

    wgpu_render_pass_set_pipeline(pass, g_pipe_blend);
    wgpu_render_pass_set_bind_group(pass, 0, bg, 0, 0);
    wgpu_render_pass_set_vertex_buffer(pass, 0,
        g_quad_buf, 0, (int)sizeof(QUAD_VERTS));
    wgpu_render_pass_draw(pass, 6, 1, 0, 0);

    wgpu_render_pass_end(pass);

    gpu_copy_tex_to_tex_t cpy;
    memset(&cpy, 0, sizeof(cpy));
    cpy.src_texture_h  = dst->tex_b;
    cpy.src_mip_level  = 0;
    cpy.src_origin_x   = (uint32_t)dx;
    cpy.src_origin_y   = (uint32_t)dy;
    cpy.src_origin_z   = 0;
    cpy.dst_texture_h  = dst->tex_a;
    cpy.dst_mip_level  = 0;
    cpy.dst_origin_x   = (uint32_t)dx;
    cpy.dst_origin_y   = (uint32_t)dy;
    cpy.dst_origin_z   = 0;
    cpy.width          = (uint32_t)dw;
    cpy.height         = (uint32_t)dh;
    cpy.depth          = 1;

    wgpu_encoder_copy_texture_to_texture(enc, &cpy);

    gpu_command_buffer_t cmd = wgpu_encoder_finish(enc);
    wgpu_queue_submit(g_queue, cmd);
}

/* ================================================================== */
/*  Blit — public API                                                  */
/* ================================================================== */

void GPUSurface_blit(
    GPUSurface          src,
    const gpu_rect_t   *src_rect,
    GPUSurface          dst,
    const gpu_rect_t   *dst_rect,
    GPU_BLEND_MODE      blend_mode,
    float               opacity)
{
    if (!src || !dst) {
        SLOG("blit: NULL src or dst!");
        return;
    }
    if (src->passActive) {
        SLOG_INT("blit: src has ACTIVE PASS! slot", surface_slot(src));
        return;
    }
    if (dst->passActive) {
        SLOG_INT("blit: dst has ACTIVE PASS! slot", surface_slot(dst));
        return;
    }
    if (src == dst) {
        SLOG("blit: src == dst!");
        return;
    }
    if (blend_mode < 0 || blend_mode >= GPU_BLEND_COUNT) {
        SLOG_INT("blit: bad blend_mode", blend_mode);
        return;
    }

    VALIDATE(src, "blit src");
    VALIDATE(dst, "blit dst");

        /* original destination rect (before clipping) */
    int ox, oy, ow, oh;
    if (dst_rect) { ox = dst_rect->x; oy = dst_rect->y; ow = dst_rect->w; oh = dst_rect->h; }
    else          { ox = 0; oy = 0; ow = (int)dst->width; oh = (int)dst->height; }

    int dx, dy, dw, dh;
    get_dst_rect(dst, dst_rect, &dx, &dy, &dw, &dh);
    if (dw <= 0 || dh <= 0) { /* ... */ return; }

    gpu_blit_uniforms_t u;
    compute_uniforms(src, src_rect, dst, dst_rect, blend_mode, opacity, &u);

    /* ---- Adjust source UVs for destination clipping ---- */
    if (ow > 0 && oh > 0) {
        float cx0 = (float)(dx - ox) / (float)ow;   /* fraction clipped on left   */
        float cy0 = (float)(dy - oy) / (float)oh;   /* fraction clipped on top    */
        float cxs = (float)dw / (float)ow;           /* surviving width  fraction  */
        float cys = (float)dh / (float)oh;           /* surviving height fraction  */
        float su = u.src_uv[0], sv = u.src_uv[1];
        float sw = u.src_uv[2], sh = u.src_uv[3];
        u.src_uv[0] = su + cx0 * sw;
        u.src_uv[1] = sv + cy0 * sh;
        u.src_uv[2] = sw * cxs;
        u.src_uv[3] = sh * cys;
    }
    wgpu_buffer_write(g_blit_ubuf, 0, &u, sizeof(u));

    if (blend_mode < GPU_BLEND_HW_MAX ||
        blend_mode == GPU_BLEND_PREMULTIPLIED) {
        blit_hw(src, dst, &u, dx, dy, dw, dh, blend_mode);
    } else {
        blit_shader(src, dst, &u, dx, dy, dw, dh);
    }
}

/* ================================================================== */
/*  Present                                                            */
/* ================================================================== */

void GPUSurface_present(GPUSurface surface)
{
    if (!surface) return;
    if (surface->type != GPU_SURFACE_TYPE_SWAPCHAIN) return;
    if (surface->passActive) GPUSurface_end(surface);

    VALIDATE(surface, "present");

    surface->swapchain_view = wgpu_surface_get_current_texture_view();
    if (!surface->swapchain_view) {
        SLOG("present: failed to get swapchain texture view!");
        return;
    }

    gpu_blit_uniforms_t u;
    u.src_uv[0] = 0.0f;
    u.src_uv[1] = 0.0f;
    u.src_uv[2] = (float)surface->width / (float)surface->alloc_width;
    u.src_uv[3] = (float)surface->height / (float)surface->alloc_height;
    u.dst_size[0] = (float)surface->width;
    u.dst_size[1] = (float)surface->height;
    u.opacity = 1.0f;
    u.blend_mode = GPU_BLEND_REPLACE;
    wgpu_buffer_write(g_blit_ubuf, 0, &u, sizeof(u));

    gpu_bind_group_entry_t entries[3];
    entries[0] = (gpu_bind_group_entry_t){
        .binding = 0, .type = GPU_ENTRY_BUFFER,
        .handle = g_blit_ubuf, .field0 = 0,
        .field1 = sizeof(gpu_blit_uniforms_t)
    };
    entries[1] = (gpu_bind_group_entry_t){
        .binding = 1, .type = GPU_ENTRY_TEXTURE_VIEW,
        .handle = surface->view_a, .field0 = 0, .field1 = 0
    };
    entries[2] = (gpu_bind_group_entry_t){
        .binding = 2, .type = GPU_ENTRY_SAMPLER,
        .handle = g_sampler, .field0 = 0, .field1 = 0
    };

    if (!surface->view_a) return;  // ← defensive null check

    gpu_bind_group_t bg = wgpu_create_bind_group(g_dev, g_bgl_simple, entries, 3);

    gpu_command_encoder_t enc = wgpu_create_command_encoder(g_dev);
    gpu_render_pass_t pass = wgpu_begin_render_pass(
        enc, surface->swapchain_view, 0.0f, 0.0f, 0.0f, 1.0f);

    wgpu_render_pass_set_pipeline(pass, g_pipe_present);
    wgpu_render_pass_set_bind_group(pass, 0, bg, 0, 0);
    wgpu_render_pass_set_vertex_buffer(pass, 0,
        g_quad_buf, 0, (int)sizeof(QUAD_VERTS));
    wgpu_render_pass_draw(pass, 6, 1, 0, 0);

    wgpu_render_pass_end(pass);
    gpu_command_buffer_t cmd = wgpu_encoder_finish(enc);
    wgpu_queue_submit(g_queue, cmd);
    wgpu_surface_present();
    wgpu_texture_view_release(surface->swapchain_view);
    surface->swapchain_view = 0;
}

/* ================================================================== */
/*  Accessors                                                          */
/* ================================================================== */

gpu_buffer_t       GPUSurface_get_quad_buffer(void)    { return g_quad_buf; }
gpu_sampler_t      GPUSurface_get_sampler(void)        { return g_sampler; }
gpu_device_t       GPUSurface_get_device(void)         { return g_dev; }
int                GPUSurface_get_surface_format(void)  { return g_surface_fmt; }

uint32_t GPUSurface_get_width(GPUSurface s)  { return s ? s->width : 0; }
uint32_t GPUSurface_get_height(GPUSurface s) { return s ? s->height : 0; }

gpu_texture_view_t GPUSurface_get_texture_view(GPUSurface s)
{
    return s ? s->view_a : 0;
}

gpu_render_pass_t GPUSurface_get_render_pass(GPUSurface s)
{
    return (s && s->passActive) ? s->renderPass : 0;
}

/* ================================================================== */
/*  Humanized API — convenience functions                              */
/* ================================================================== */

void GPUSurface_begin_ex(GPUSurface surface,
                         gpu_pass_mode_t mode,
                         gpu_color_t c)
{
    switch (mode) {
    case GPU_PASS_CLEAR:
        begin_pass_internal(surface, GPU_LOAD_OP_CLEAR, c.r, c.g, c.b, c.a);
        break;
    case GPU_PASS_LOAD:
        begin_pass_internal(surface, GPU_LOAD_OP_LOAD, 0, 0, 0, 0);
        break;
    case GPU_PASS_3D:
        begin_pass_3d_internal(surface, GPU_LOAD_OP_CLEAR, c.r, c.g, c.b, c.a);
        break;
    case GPU_PASS_3D_LOAD:
        begin_pass_3d_internal(surface, GPU_LOAD_OP_LOAD, 0, 0, 0, 0);
        break;
    }
}

void GPUSurface_clear_color(GPUSurface surface, gpu_color_t c)
{
    GPUSurface_clear(surface, c.r, c.g, c.b, c.a);
}

void GPUSurface_draw(GPUSurface surface,
                     gpu_render_pipeline_t pipeline,
                     gpu_bind_group_t bind_group)
{
    GPUSurface_begin_load(surface);
    GPUSurface_draw_pipeline(surface, pipeline, bind_group);
    GPUSurface_end(surface);
}

void GPUSurface_compose(GPUSurface src, GPUSurface dst)
{
    GPUSurface_blit(src, NULL, dst, NULL, GPU_BLEND_PREMULTIPLIED, 1.0f);
}

void GPUSurface_compose_at(GPUSurface src, GPUSurface dst,
                           gpu_rect_t dst_rect)
{
    GPUSurface_blit(src, NULL, dst, &dst_rect, GPU_BLEND_PREMULTIPLIED, 1.0f);
}

gpu_render_pipeline_t GPUSurface_pipeline(const gpu_pipeline_desc_t *desc)
{
    int fmt = desc->format ? desc->format : GPU_TEX_FORMAT_RGBA8Unorm;
    if (desc->depth.write || desc->depth.compare) {
        return GPUSurface_create_pipeline_depth(
            desc->layout, desc->vs, desc->vs_entry, desc->fs, desc->fs_entry,
            fmt, desc->blend.enabled,
            desc->blend.color_src, desc->blend.color_dst, desc->blend.color_op,
            desc->blend.alpha_src, desc->blend.alpha_dst, desc->blend.alpha_op,
            desc->depth.write, desc->depth.compare);
    }
    return GPUSurface_create_pipeline(
        desc->layout, desc->vs, desc->vs_entry, desc->fs, desc->fs_entry,
        fmt, desc->blend.enabled,
        desc->blend.color_src, desc->blend.color_dst, desc->blend.color_op,
        desc->blend.alpha_src, desc->blend.alpha_dst, desc->blend.alpha_op);
}

/* ================================================================== */
/*  Scratch pool — transient surfaces reused across frames             */
/* ================================================================== */

#define GPU_SCRATCH_POOL_SIZE 8

static GPUSurface g_scratch_pool[GPU_SCRATCH_POOL_SIZE];
static int        g_scratch_in_use[GPU_SCRATCH_POOL_SIZE];

GPUSurface GPUSurface_acquire_scratch(uint32_t width, uint32_t height)
{
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;

    /* Try to find a free slot that already fits */
    for (int i = 0; i < GPU_SCRATCH_POOL_SIZE; i++) {
        if (g_scratch_in_use[i]) continue;
        GPUSurface s = g_scratch_pool[i];
        if (s && s->alloc_width >= width && s->alloc_height >= height) {
            g_scratch_in_use[i] = 1;
            s->width  = width;
            s->height = height;
            return s;
        }
    }

    /* Find a free slot (empty or too small — replace it) */
    for (int i = 0; i < GPU_SCRATCH_POOL_SIZE; i++) {
        if (g_scratch_in_use[i]) continue;
        if (g_scratch_pool[i]) GPUSurface_destroy(g_scratch_pool[i]);
        g_scratch_pool[i] = GPUSurface_create(width, height);
        g_scratch_in_use[i] = 1;
        return g_scratch_pool[i];
    }

    /* All slots busy — fall back to a fresh allocation */
    return GPUSurface_create(width, height);
}

void GPUSurface_release_scratch(GPUSurface surface)
{
    if (!surface) return;
    for (int i = 0; i < GPU_SCRATCH_POOL_SIZE; i++) {
        if (g_scratch_pool[i] == surface) {
            g_scratch_in_use[i] = 0;
            return;
        }
    }
    /* Not from the pool — destroy it */
    GPUSurface_destroy(surface);
}
