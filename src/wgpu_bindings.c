/*
    WebGPU WASM Bindings Implementation
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

#include "wgpu_bindings.h"
#include <webgpu.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================== */
/*  Memory helpers                                                     */
/* ================================================================== */

static uint8_t *wasm_mem_base(WgpuBindings *b) {
    return b->memory ? (uint8_t *)wasm_memory_data(b->memory) : NULL;
}
static size_t wasm_mem_size(WgpuBindings *b) {
    return b->memory ? wasm_memory_data_size(b->memory) : 0;
}
static bool mem_read(WgpuBindings *b, uint32_t ptr, void *dst, size_t len) {
    if ((size_t)ptr + len > wasm_mem_size(b)) return false;
    memcpy(dst, wasm_mem_base(b) + ptr, len);
    return true;
}
static bool mem_write(WgpuBindings *b, uint32_t ptr, const void *src, size_t len) {
    if ((size_t)ptr + len > wasm_mem_size(b)) return false;
    memcpy(wasm_mem_base(b) + ptr, src, len);
    return true;
}
static float mem_read_f32(WgpuBindings *b, uint32_t ptr) {
    float v = 0; mem_read(b, ptr, &v, 4); return v;
}
static uint32_t mem_read_u32(WgpuBindings *b, uint32_t ptr) {
    uint32_t v = 0; mem_read(b, ptr, &v, 4); return v;
}
static int32_t mem_read_i32(WgpuBindings *b, uint32_t ptr) {
    int32_t v = 0; mem_read(b, ptr, &v, 4); return v;
}
static const char *mem_read_str(WgpuBindings *b, uint32_t ptr, uint32_t len) {
    if (len == 0) return "";
    if ((size_t)ptr + len > wasm_mem_size(b)) return "";
    return (const char *)(wasm_mem_base(b) + ptr);
}
static void *mem_ptr(WgpuBindings *b, uint32_t ptr, uint32_t len) {
    if ((size_t)ptr + len > wasm_mem_size(b)) return NULL;
    return wasm_mem_base(b) + ptr;
}

/* ================================================================== */
/*  Macros                                                             */
/* ================================================================== */

#define B    ((WgpuBindings *)env)
#define GPU  (B->gpu)
#define INST (GPU->instance)
#define DEV  (GPU->device)
#define QUE  (GPU->queue)
#define ARG_I32(n) (args->data[(n)].of.i32)
#define ARG_F32(n) (args->data[(n)].of.f32)
#define RET_I32(v) do { res->data[0] = (wasm_val_t){.kind=WASM_I32,.of.i32=(v)}; } while(0)

/* ================================================================== */
/*  Frame resource management                                          */
/* ================================================================== */

static void release_frame_resources(WgpuBindings *b) {
    /* In offscreen mode, the texture and view are owned by the dashboard
       (via gpu_create_offscreen_texture).  Only remove the handle table
       entries — do NOT release the underlying GPU objects. */
    if (b->offscreen) {
        if (b->frame_view_handle)    { htable_remove(&b->ht_texture_view, b->frame_view_handle); b->frame_view_handle = 0; }
        if (b->frame_texture_handle) { htable_remove(&b->ht_texture, b->frame_texture_handle);   b->frame_texture_handle = 0; }
        b->frame_view    = NULL;
        b->frame_texture = NULL;
        return;
    }
    if (b->frame_view) { wgpuTextureViewRelease(b->frame_view); b->frame_view = NULL; }
    if (b->frame_view_handle) { htable_remove(&b->ht_texture_view, b->frame_view_handle); b->frame_view_handle = 0; }
    if (b->frame_texture) { wgpuTextureRelease(b->frame_texture); b->frame_texture = NULL; }
    if (b->frame_texture_handle) { htable_remove(&b->ht_texture, b->frame_texture_handle); b->frame_texture_handle = 0; }
}

/* ================================================================== */
/*  Device / Queue / Surface info                                      */
/* ================================================================== */

static wasm_trap_t *fn_get_device(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; RET_I32(B->h_device); return NULL;
}
static wasm_trap_t *fn_get_queue(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; RET_I32(B->h_queue); return NULL;
}
static wasm_trap_t *fn_get_surface_format(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; RET_I32((int32_t)GPU->format); return NULL;
}
static wasm_trap_t *fn_get_surface_width(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; RET_I32((int32_t)GPU->width); return NULL;
}
static wasm_trap_t *fn_get_surface_height(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args; RET_I32((int32_t)GPU->height); return NULL;
}

/* Dawn uses wgpuInstanceProcessEvents instead of wgpuDevicePoll */
static wasm_trap_t *fn_device_poll(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    wgpuInstanceProcessEvents(INST);
    RET_I32(1); return NULL;
}

/* ================================================================== */
/*  Enum queries                                                       */
/* ================================================================== */

static wasm_trap_t *fn_get_vertex_format(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    int k = ARG_I32(0);
    WGPUVertexFormat fmt;
    switch (k) {
        case 0: fmt=WGPUVertexFormat_Float32x2; break; case 1: fmt=WGPUVertexFormat_Float32x3; break;
        case 2: fmt=WGPUVertexFormat_Float32x4; break; case 3: fmt=WGPUVertexFormat_Uint32; break;
        case 4: fmt=WGPUVertexFormat_Float32; break;   case 5: fmt=WGPUVertexFormat_Sint32x2; break;
        case 6: fmt=WGPUVertexFormat_Sint32x3; break;  case 7: fmt=WGPUVertexFormat_Sint32x4; break;
        case 8: fmt=WGPUVertexFormat_Uint16x2; break;  case 9: fmt=WGPUVertexFormat_Uint16x4; break;
        case 10: fmt=WGPUVertexFormat_Unorm8x2; break; case 11: fmt=WGPUVertexFormat_Unorm8x4; break;
        case 12: fmt=WGPUVertexFormat_Snorm8x2; break; case 13: fmt=WGPUVertexFormat_Snorm8x4; break;
        case 14: fmt=WGPUVertexFormat_Uint8x2; break;  case 15: fmt=WGPUVertexFormat_Uint8x4; break;
        case 16: fmt=WGPUVertexFormat_Sint8x2; break;  case 17: fmt=WGPUVertexFormat_Sint8x4; break;
        case 18: fmt=WGPUVertexFormat_Uint32x2; break; case 19: fmt=WGPUVertexFormat_Uint32x3; break;
        case 20: fmt=WGPUVertexFormat_Uint32x4; break; case 21: fmt=WGPUVertexFormat_Sint32; break;
        case 22: fmt=WGPUVertexFormat_Float16x2; break;case 23: fmt=WGPUVertexFormat_Float16x4; break;
        case 24: fmt=WGPUVertexFormat_Unorm16x2; break;case 25: fmt=WGPUVertexFormat_Unorm16x4; break;
        case 26: fmt=WGPUVertexFormat_Snorm16x2; break;case 27: fmt=WGPUVertexFormat_Snorm16x4; break;
        default: fmt=WGPUVertexFormat_Float32x2; break;
    }
    RET_I32((int32_t)fmt); return NULL;
}

static wasm_trap_t *fn_get_topology(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    int k = ARG_I32(0);
    WGPUPrimitiveTopology t;
    switch (k) {
        case 0: t=WGPUPrimitiveTopology_PointList; break;
        case 1: t=WGPUPrimitiveTopology_LineList; break;
        case 2: t=WGPUPrimitiveTopology_LineStrip; break;
        case 3: t=WGPUPrimitiveTopology_TriangleList; break;
        case 4: t=WGPUPrimitiveTopology_TriangleStrip; break;
        default: t=WGPUPrimitiveTopology_TriangleList; break;
    }
    RET_I32((int32_t)t); return NULL;
}

/* ================================================================== */
/*  Shader Module                                                      */
/* ================================================================== */

static wasm_trap_t *fn_create_shader_spirv(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    fprintf(stderr, "SPIR-V Shader Module is not supported at this time. Dawn project by Google may not supports this.\n");
    fprintf(stderr, "But if you feels strongly that it is supported, then edit file YumiBrowser/src/wgpu_bindings.c at line %u.\n", __LINE__);
    fprintf(stderr, "You can uncomment the lines below this fprint statements and delete the fprints within fn_create_shader_spirv function.\n");
    fprintf(stderr, "But don't forget to delete the return NULL; just below this fprint line, not the other return NULL, keep that.\n");
    return NULL;
    /* This is not supported in Dawn, so avoid it.
    uint32_t code_ptr = (uint32_t)ARG_I32(1), code_len = (uint32_t)ARG_I32(2);
    uint32_t entry_ptr = (uint32_t)ARG_I32(3), entry_len = (uint32_t)ARG_I32(4);
    if (code_len == 0 || code_len % 4 != 0 || (size_t)code_ptr + code_len > wasm_mem_size(B)) { RET_I32(0); return NULL; }
    const uint32_t *spirv = (const uint32_t *)(wasm_mem_base(B) + code_ptr);
    if (spirv[0] != 0x07230203u) { RET_I32(0); return NULL; }
    const char *label = entry_len > 0 ? mem_read_str(B, entry_ptr, entry_len) : "";
    uint32_t label_len = entry_len;
    WGPUShaderSourceSPIRV src = { .chain={.sType=WGPUSType_ShaderSourceSPIRV}, .codeSize=code_len/4, .code=spirv };
    WGPUShaderModuleDescriptor desc = { .nextInChain=(const WGPUChainedStruct*)&src, .label={.data=label,.length=label_len} };
    WGPUShaderModule sm = wgpuDeviceCreateShaderModule(DEV, &desc);
    RET_I32(sm ? htable_insert(&B->ht_shader, sm) : 0); return NULL;
    */
}

/* WGSL: dev(0), code_ptr(1), code_len(2) */
static wasm_trap_t *fn_create_shader_wgsl(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t code_ptr = (uint32_t)ARG_I32(1), code_len = (uint32_t)ARG_I32(2);
    const char *code = mem_read_str(B, code_ptr, code_len);
    if (!code[0] && code_len > 0) { RET_I32(0); return NULL; }
    WGPUShaderSourceWGSL wgsl = { .chain={.sType=WGPUSType_ShaderSourceWGSL}, .code={.data=code,.length=code_len} };
    WGPUShaderModuleDescriptor desc = { .nextInChain=(WGPUChainedStruct*)&wgsl };
    WGPUShaderModule sm = wgpuDeviceCreateShaderModule(DEV, &desc);
    RET_I32(sm ? htable_insert(&B->ht_shader, sm) : 0); return NULL;
}

static wasm_trap_t *fn_shader_release(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPUShaderModule s=htable_get(&B->ht_shader,h);
    if(s){wgpuShaderModuleRelease(s);htable_remove(&B->ht_shader,h);}
    return NULL;
}

/* ================================================================== */
/*  Buffer                                                             */
/* ================================================================== */

static wasm_trap_t *fn_create_buffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUBufferDescriptor desc = { .usage=(uint32_t)ARG_I32(1), .size=(uint64_t)(uint32_t)ARG_I32(2), .mappedAtCreation=ARG_I32(3)!=0 };
    WGPUBuffer buf = wgpuDeviceCreateBuffer(DEV, &desc);
    RET_I32(htable_insert(&B->ht_buffer, buf)); return NULL;
}

static wasm_trap_t *fn_buffer_write(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(0));
    uint32_t data_ptr=(uint32_t)ARG_I32(2), data_len=(uint32_t)ARG_I32(3);
    if(!buf||(size_t)data_ptr+data_len>wasm_mem_size(B)) return NULL;
    wgpuQueueWriteBuffer(QUE,buf,(uint64_t)(uint32_t)ARG_I32(1),wasm_mem_base(B)+data_ptr,data_len);
    return NULL;
}

static wasm_trap_t *fn_buffer_destroy(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPUBuffer buf=htable_get(&B->ht_buffer,h);
    if(buf){wgpuBufferDestroy(buf);wgpuBufferRelease(buf);htable_remove(&B->ht_buffer,h);}
    return NULL;
}

static wasm_trap_t *fn_buffer_get_size(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(0));
    RET_I32(buf ? (int32_t)wgpuBufferGetSize(buf) : 0); return NULL;
}

static void buffer_map_cb(WGPUMapAsyncStatus status, WGPUStringView msg, void *ud1, void *ud2) {
    (void)status; (void)msg; (void)ud2; *(volatile bool*)ud1 = true;
}
static wasm_trap_t *fn_buffer_map_sync(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(0));
    if(!buf){RET_I32(0);return NULL;}
    uint32_t mode=(uint32_t)ARG_I32(1);
    uint64_t offset=(uint64_t)(uint32_t)ARG_I32(2), size=(uint64_t)(uint32_t)ARG_I32(3);
    volatile bool done=false;
    WGPUBufferMapCallbackInfo cb={.mode=WGPUCallbackMode_AllowProcessEvents,.callback=buffer_map_cb,.userdata1=(void*)&done};
    wgpuBufferMapAsync(buf,mode,offset,size,cb);
    while(!done) wgpuInstanceProcessEvents(INST);
    RET_I32(1); return NULL;
}

static wasm_trap_t *fn_buffer_get_mapped_range(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(0));
    uint64_t offset=(uint64_t)(uint32_t)ARG_I32(1), size=(uint64_t)(uint32_t)ARG_I32(2);
    uint32_t dst_ptr=(uint32_t)ARG_I32(3);
    if(!buf) return NULL;
    const void *data=wgpuBufferGetConstMappedRange(buf,(size_t)offset,(size_t)size);
    if(data) mem_write(B,dst_ptr,data,(size_t)size);
    return NULL;
}

static wasm_trap_t *fn_buffer_unmap(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(0));
    if(buf) wgpuBufferUnmap(buf);
    return NULL;
}

/* ================================================================== */
/*  Texture                                                            */
/* ================================================================== */

static wasm_trap_t *fn_create_texture(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t p=(uint32_t)ARG_I32(0);
    WGPUTextureDescriptor desc={
        .usage=mem_read_u32(B,p),
        .dimension=(WGPUTextureDimension)mem_read_u32(B,p+4),
        .size={mem_read_u32(B,p+8),mem_read_u32(B,p+12),mem_read_u32(B,p+16)},
        .format=(WGPUTextureFormat)mem_read_u32(B,p+20),
        .mipLevelCount=mem_read_u32(B,p+24),
        .sampleCount=mem_read_u32(B,p+28),
    };
    if(desc.mipLevelCount==0) desc.mipLevelCount=1;
    if(desc.sampleCount==0) desc.sampleCount=1;
    WGPUTexture tex=wgpuDeviceCreateTexture(DEV,&desc);
    RET_I32(tex?htable_insert(&B->ht_texture,tex):0); return NULL;
}

static wasm_trap_t *fn_create_texture_view(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUTexture tex=htable_get(&B->ht_texture,(uint32_t)ARG_I32(0));
    uint32_t p=(uint32_t)ARG_I32(1);
    if(!tex){RET_I32(0);return NULL;}
    WGPUTextureView view;
    if(p==0){
        view=wgpuTextureCreateView(tex,NULL);
    } else {
        WGPUTextureViewDescriptor vd={
            .format=(WGPUTextureFormat)mem_read_u32(B,p),
            .dimension=(WGPUTextureViewDimension)mem_read_u32(B,p+4),
            .baseMipLevel=mem_read_u32(B,p+8),
            .mipLevelCount=mem_read_u32(B,p+12),
            .baseArrayLayer=mem_read_u32(B,p+16),
            .arrayLayerCount=mem_read_u32(B,p+20),
        };
        view=wgpuTextureCreateView(tex,&vd);
    }
    RET_I32(view?htable_insert(&B->ht_texture_view,view):0); return NULL;
}

static wasm_trap_t *fn_queue_write_texture(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t p=(uint32_t)ARG_I32(0);
    WGPUTexture tex=htable_get(&B->ht_texture,mem_read_u32(B,p));
    if(!tex) return NULL;
    uint32_t data_ptr=mem_read_u32(B,p+20), data_len=mem_read_u32(B,p+24);
    const void *data=mem_ptr(B,data_ptr,data_len);
    if(!data) return NULL;
    WGPUTexelCopyTextureInfo dst={.texture=tex,.mipLevel=mem_read_u32(B,p+4),
        .origin={mem_read_u32(B,p+8),mem_read_u32(B,p+12),mem_read_u32(B,p+16)}};
    WGPUTexelCopyBufferLayout layout={.bytesPerRow=mem_read_u32(B,p+28),.rowsPerImage=mem_read_u32(B,p+32)};
    WGPUExtent3D extent={mem_read_u32(B,p+36),mem_read_u32(B,p+40),mem_read_u32(B,p+44)};
    wgpuQueueWriteTexture(QUE,&dst,data,data_len,&layout,&extent);
    return NULL;
}

static wasm_trap_t *fn_texture_destroy(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    if(h==B->frame_texture_handle) return NULL;
    WGPUTexture t=htable_get(&B->ht_texture,h);
    if(t){wgpuTextureDestroy(t);wgpuTextureRelease(t);htable_remove(&B->ht_texture,h);}
    return NULL;
}

static wasm_trap_t *fn_texture_release(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    if(h==B->frame_texture_handle) return NULL;
    WGPUTexture t=htable_get(&B->ht_texture,h);
    if(t){wgpuTextureRelease(t);htable_remove(&B->ht_texture,h);}
    return NULL;
}

static wasm_trap_t *fn_texture_view_release(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    if(h==B->frame_view_handle) return NULL;
    WGPUTextureView v=htable_get(&B->ht_texture_view,h);
    if(v){wgpuTextureViewRelease(v);htable_remove(&B->ht_texture_view,h);}
    return NULL;
}

/* ================================================================== */
/*  Sampler                                                            */
/* ================================================================== */

static wasm_trap_t *fn_create_sampler(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t p=(uint32_t)ARG_I32(0);
    uint32_t cmp=mem_read_u32(B,p+32);
    uint32_t aniso=mem_read_u32(B,p+36);
    WGPUSamplerDescriptor desc={
        .addressModeU=(WGPUAddressMode)mem_read_u32(B,p),
        .addressModeV=(WGPUAddressMode)mem_read_u32(B,p+4),
        .addressModeW=(WGPUAddressMode)mem_read_u32(B,p+8),
        .magFilter=(WGPUFilterMode)mem_read_u32(B,p+12),
        .minFilter=(WGPUFilterMode)mem_read_u32(B,p+16),
        .mipmapFilter=(WGPUMipmapFilterMode)mem_read_u32(B,p+20),
        .lodMinClamp=mem_read_f32(B,p+24),
        .lodMaxClamp=mem_read_f32(B,p+28),
        .compare=cmp?(WGPUCompareFunction)cmp:WGPUCompareFunction_Undefined,
        .maxAnisotropy=(uint16_t)(aniso?aniso:1),
    };
    WGPUSampler s=wgpuDeviceCreateSampler(DEV,&desc);
    RET_I32(s?htable_insert(&B->ht_sampler,s):0); return NULL;
}

static wasm_trap_t *fn_sampler_release(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPUSampler s=htable_get(&B->ht_sampler,h);
    if(s){wgpuSamplerRelease(s);htable_remove(&B->ht_sampler,h);}
    return NULL;
}

/* ================================================================== */
/*  Bind Group Layout                                                  */
/* ================================================================== */

static wasm_trap_t *fn_create_bind_group_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t entries_ptr=(uint32_t)ARG_I32(1), entry_count=(uint32_t)ARG_I32(2);
    if(entry_count>16) entry_count=16;
    WGPUBindGroupLayoutEntry *entries=calloc(entry_count,sizeof(WGPUBindGroupLayoutEntry));
    for(uint32_t i=0;i<entry_count;i++){
        uint32_t base=entries_ptr+i*32;
        entries[i].binding=mem_read_u32(B,base);
        entries[i].visibility=(WGPUShaderStage)mem_read_u32(B,base+4);
        uint32_t type=mem_read_u32(B,base+8);
        switch(type){
            case 0: {
                uint32_t bt=mem_read_u32(B,base+12);
                WGPUBufferBindingType wt;
                switch(bt){case 1:wt=WGPUBufferBindingType_Storage;break;case 2:wt=WGPUBufferBindingType_ReadOnlyStorage;break;default:wt=WGPUBufferBindingType_Uniform;break;}
                entries[i].buffer=(WGPUBufferBindingLayout){.type=wt,.hasDynamicOffset=mem_read_u32(B,base+16)!=0,.minBindingSize=(uint64_t)mem_read_u32(B,base+20)};
                break;
            }
            case 1: {
                uint32_t st=mem_read_u32(B,base+12);
                WGPUTextureSampleType wst;
                switch(st){case 1:wst=WGPUTextureSampleType_UnfilterableFloat;break;case 2:wst=WGPUTextureSampleType_Depth;break;case 3:wst=WGPUTextureSampleType_Sint;break;case 4:wst=WGPUTextureSampleType_Uint;break;default:wst=WGPUTextureSampleType_Float;break;}
                entries[i].texture=(WGPUTextureBindingLayout){.sampleType=wst,.viewDimension=(WGPUTextureViewDimension)mem_read_u32(B,base+16),.multisampled=mem_read_u32(B,base+20)!=0};
                break;
            }
            case 2: {
                uint32_t st=mem_read_u32(B,base+12);
                WGPUSamplerBindingType wst;
                switch(st){case 1:wst=WGPUSamplerBindingType_Filtering;break;case 2:wst=WGPUSamplerBindingType_NonFiltering;break;case 3:wst=WGPUSamplerBindingType_Comparison;break;default:wst=WGPUSamplerBindingType_Filtering;break;}
                entries[i].sampler=(WGPUSamplerBindingLayout){.type=wst};
                break;
            }
            case 3: {
                uint32_t ac=mem_read_u32(B,base+12);
                WGPUStorageTextureAccess wa;
                switch(ac){case 1:wa=WGPUStorageTextureAccess_WriteOnly;break;case 2:wa=WGPUStorageTextureAccess_ReadOnly;break;case 3:wa=WGPUStorageTextureAccess_ReadWrite;break;default:wa=WGPUStorageTextureAccess_WriteOnly;break;}
                entries[i].storageTexture=(WGPUStorageTextureBindingLayout){.access=wa,.format=(WGPUTextureFormat)mem_read_u32(B,base+16),.viewDimension=(WGPUTextureViewDimension)mem_read_u32(B,base+20)};
                break;
            }
        }
    }
    WGPUBindGroupLayoutDescriptor desc={.entryCount=entry_count,.entries=entries};
    WGPUBindGroupLayout bgl=wgpuDeviceCreateBindGroupLayout(DEV,&desc);
    free(entries);
    RET_I32(bgl?htable_insert(&B->ht_bind_group_layout,bgl):0); return NULL;
}

static wasm_trap_t *fn_bind_group_layout_release(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPUBindGroupLayout l=htable_get(&B->ht_bind_group_layout,h);
    if(l){wgpuBindGroupLayoutRelease(l);htable_remove(&B->ht_bind_group_layout,h);}
    return NULL;
}

/* ================================================================== */
/*  Bind Group                                                         */
/* ================================================================== */

static wasm_trap_t *fn_create_bind_group(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUBindGroupLayout bgl=htable_get(&B->ht_bind_group_layout,(uint32_t)ARG_I32(1));
    uint32_t entries_ptr=(uint32_t)ARG_I32(2), entry_count=(uint32_t)ARG_I32(3);
    if(!bgl){RET_I32(0);return NULL;}
    if(entry_count>16) entry_count=16;
    WGPUBindGroupEntry *entries=calloc(entry_count,sizeof(WGPUBindGroupEntry));
    for(uint32_t i=0;i<entry_count;i++){
        uint32_t base=entries_ptr+i*20;
        entries[i].binding=mem_read_u32(B,base);
        uint32_t type=mem_read_u32(B,base+4);
        uint32_t handle=mem_read_u32(B,base+8);
        switch(type){
            case 0: {
                WGPUBuffer buf=htable_get(&B->ht_buffer,handle);
                uint32_t offset=mem_read_u32(B,base+12), size=mem_read_u32(B,base+16);
                entries[i].buffer=buf;
                entries[i].offset=(uint64_t)offset;
                entries[i].size=size?(uint64_t)size:WGPU_WHOLE_SIZE;
                break;
            }
            case 1: entries[i].textureView=htable_get(&B->ht_texture_view,handle); break;
            case 2: entries[i].sampler=htable_get(&B->ht_sampler,handle); break;
        }
    }
    WGPUBindGroupDescriptor desc={.layout=bgl,.entryCount=entry_count,.entries=entries};
    WGPUBindGroup bg=wgpuDeviceCreateBindGroup(DEV,&desc);
    free(entries);
    RET_I32(bg?htable_insert(&B->ht_bind_group,bg):0); return NULL;
}

static wasm_trap_t *fn_bind_group_release(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPUBindGroup g=htable_get(&B->ht_bind_group,h);
    if(g){wgpuBindGroupRelease(g);htable_remove(&B->ht_bind_group,h);}
    return NULL;
}

/* ================================================================== */
/*  Pipeline Layout                                                    */
/* ================================================================== */

static wasm_trap_t *fn_create_pipeline_layout_empty(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    WGPUPipelineLayoutDescriptor desc={.bindGroupLayoutCount=0,.bindGroupLayouts=NULL};
    RET_I32(htable_insert(&B->ht_pipeline_layout,wgpuDeviceCreatePipelineLayout(DEV,&desc)));
    return NULL;
}

static wasm_trap_t *fn_create_pipeline_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t layouts_ptr=(uint32_t)ARG_I32(1), count=(uint32_t)ARG_I32(2);
    if(count>8) count=8;
    WGPUBindGroupLayout bgls[8];
    for(uint32_t i=0;i<count;i++) bgls[i]=htable_get(&B->ht_bind_group_layout,mem_read_u32(B,layouts_ptr+i*4));
    WGPUPipelineLayoutDescriptor desc={.bindGroupLayoutCount=count,.bindGroupLayouts=bgls};
    RET_I32(htable_insert(&B->ht_pipeline_layout,wgpuDeviceCreatePipelineLayout(DEV,&desc)));
    return NULL;
}

static wasm_trap_t *fn_pipeline_layout_release(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPUPipelineLayout l=htable_get(&B->ht_pipeline_layout,h);
    if(l){wgpuPipelineLayoutRelease(l);htable_remove(&B->ht_pipeline_layout,h);}
    return NULL;
}

/* ================================================================== */
/*  Render Pipeline                                                    */
/* ================================================================== */

static wasm_trap_t *fn_create_render_pipeline(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t layout_h=(uint32_t)ARG_I32(1);
    WGPUShaderModule vs=htable_get(&B->ht_shader,(uint32_t)ARG_I32(2));
    uint32_t vs_ep=(uint32_t)ARG_I32(3),vs_el=(uint32_t)ARG_I32(4);
    WGPUShaderModule fs=htable_get(&B->ht_shader,(uint32_t)ARG_I32(5));
    uint32_t fs_ep=(uint32_t)ARG_I32(6),fs_el=(uint32_t)ARG_I32(7);
    WGPUTextureFormat fmt=(WGPUTextureFormat)ARG_I32(8);
    WGPUPrimitiveTopology topo=(WGPUPrimitiveTopology)ARG_I32(9);
    uint32_t vtx_ptr=(uint32_t)ARG_I32(10);
    if(!vs||!fs){RET_I32(0);return NULL;}
    const char *vs_entry=vs_el>0?mem_read_str(B,vs_ep,vs_el):"main";
    const char *fs_entry=fs_el>0?mem_read_str(B,fs_ep,fs_el):"main";
    uint32_t vs_elen=vs_el>0?vs_el:4, fs_elen=fs_el>0?fs_el:4;
    /* Dawn webgpu.h: WGPUVertexBufferLayout has stepMode before arrayStride */
    WGPUVertexBufferLayout vbl={0}; WGPUVertexAttribute *attrs=NULL; uint32_t bc=0;
    if(vtx_ptr){
        uint32_t stride=mem_read_u32(B,vtx_ptr), ac=mem_read_u32(B,vtx_ptr+4);
        if(ac>16) ac=16;
        attrs=calloc(ac,sizeof(WGPUVertexAttribute));
        for(uint32_t i=0;i<ac;i++){
            uint32_t ab=vtx_ptr+8+i*12;
            attrs[i]=(WGPUVertexAttribute){.format=(WGPUVertexFormat)mem_read_u32(B,ab),.offset=(uint64_t)mem_read_u32(B,ab+4),.shaderLocation=mem_read_u32(B,ab+8)};
        }
        vbl=(WGPUVertexBufferLayout){.stepMode=WGPUVertexStepMode_Vertex,.arrayStride=stride,.attributeCount=ac,.attributes=attrs};
        bc=1;
    }
    WGPUBlendState blend={.color={.srcFactor=WGPUBlendFactor_SrcAlpha,.dstFactor=WGPUBlendFactor_OneMinusSrcAlpha,.operation=WGPUBlendOperation_Add},
                          .alpha={.srcFactor=WGPUBlendFactor_One,.dstFactor=WGPUBlendFactor_OneMinusSrcAlpha,.operation=WGPUBlendOperation_Add}};
    WGPUColorTargetState ct={.format=fmt,.blend=&blend,.writeMask=WGPUColorWriteMask_All};
    WGPUFragmentState frag={.module=fs,.entryPoint={.data=fs_entry,.length=fs_elen},.targetCount=1,.targets=&ct};
    WGPUPipelineLayout layout=layout_h?htable_get(&B->ht_pipeline_layout,layout_h):NULL;
    WGPURenderPipelineDescriptor desc={.layout=layout,
        .vertex={.module=vs,.entryPoint={.data=vs_entry,.length=vs_elen},.bufferCount=bc,.buffers=bc?&vbl:NULL},
        .primitive={.topology=topo},.multisample={.count=1,.mask=0xFFFFFFFF},.fragment=&frag};
    WGPURenderPipeline rp=wgpuDeviceCreateRenderPipeline(DEV,&desc);
    free(attrs);
    RET_I32(rp?htable_insert(&B->ht_render_pipeline,rp):0); return NULL;
}

static wasm_trap_t *fn_create_render_pipeline_desc(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t p=(uint32_t)ARG_I32(0);
    uint32_t layout_h=mem_read_u32(B,p);
    WGPUShaderModule vs=htable_get(&B->ht_shader,mem_read_u32(B,p+4));
    uint32_t vs_ep=mem_read_u32(B,p+8),vs_el=mem_read_u32(B,p+12);
    WGPUShaderModule fs=htable_get(&B->ht_shader,mem_read_u32(B,p+16));
    uint32_t fs_ep=mem_read_u32(B,p+20),fs_el=mem_read_u32(B,p+24);
    if(!vs){RET_I32(0);return NULL;}
    const char *vs_entry=vs_el>0?mem_read_str(B,vs_ep,vs_el):"main";
    uint32_t vs_elen=vs_el>0?vs_el:4;

    WGPUPrimitiveState prim={.topology=(WGPUPrimitiveTopology)mem_read_u32(B,p+28),
        .stripIndexFormat=(WGPUIndexFormat)mem_read_u32(B,p+32),
        .frontFace=(WGPUFrontFace)mem_read_u32(B,p+40),
        .cullMode=(WGPUCullMode)mem_read_u32(B,p+36)};

    uint32_t ct_count=mem_read_u32(B,p+44), ct_ptr=mem_read_u32(B,p+48);
    if(ct_count>8) ct_count=8;
    WGPUColorTargetState cts[8]={0}; WGPUBlendState blends[8]={0};
    for(uint32_t i=0;i<ct_count;i++){
        uint32_t cb=ct_ptr+i*36;
        cts[i].format=(WGPUTextureFormat)mem_read_u32(B,cb);
        cts[i].writeMask=(WGPUColorWriteMask)mem_read_u32(B,cb+4);
        if(mem_read_u32(B,cb+8)){
            blends[i]=(WGPUBlendState){
                .color={.srcFactor=(WGPUBlendFactor)mem_read_u32(B,cb+12),.dstFactor=(WGPUBlendFactor)mem_read_u32(B,cb+16),.operation=(WGPUBlendOperation)mem_read_u32(B,cb+20)},
                .alpha={.srcFactor=(WGPUBlendFactor)mem_read_u32(B,cb+24),.dstFactor=(WGPUBlendFactor)mem_read_u32(B,cb+28),.operation=(WGPUBlendOperation)mem_read_u32(B,cb+32)}};
            cts[i].blend=&blends[i];
        } else cts[i].blend=NULL;
    }

    WGPUFragmentState frag={0}; WGPUFragmentState *frag_ptr=NULL;
    if(fs){
        const char *fs_entry=fs_el>0?mem_read_str(B,fs_ep,fs_el):"main";
        uint32_t fs_elen=fs_el>0?fs_el:4;
        frag=(WGPUFragmentState){.module=fs,.entryPoint={.data=fs_entry,.length=fs_elen},.targetCount=ct_count,.targets=cts};
        frag_ptr=&frag;
    }

    /* Dawn: depthWriteEnabled is WGPUOptionalBool */
    WGPUDepthStencilState ds={0}; WGPUDepthStencilState *ds_ptr=NULL;
    if(mem_read_u32(B,p+52)){
        uint32_t dp=mem_read_u32(B,p+56);
        ds=(WGPUDepthStencilState){
            .format=(WGPUTextureFormat)mem_read_u32(B,dp),
            .depthWriteEnabled=mem_read_u32(B,dp+4)?WGPUOptionalBool_True:WGPUOptionalBool_False,
            .depthCompare=(WGPUCompareFunction)mem_read_u32(B,dp+8),
            .stencilFront={.compare=(WGPUCompareFunction)mem_read_u32(B,dp+12),.failOp=(WGPUStencilOperation)mem_read_u32(B,dp+16),.depthFailOp=(WGPUStencilOperation)mem_read_u32(B,dp+20),.passOp=(WGPUStencilOperation)mem_read_u32(B,dp+24)},
            .stencilBack={.compare=(WGPUCompareFunction)mem_read_u32(B,dp+28),.failOp=(WGPUStencilOperation)mem_read_u32(B,dp+32),.depthFailOp=(WGPUStencilOperation)mem_read_u32(B,dp+36),.passOp=(WGPUStencilOperation)mem_read_u32(B,dp+40)},
            .stencilReadMask=mem_read_u32(B,dp+44),.stencilWriteMask=mem_read_u32(B,dp+48),
            .depthBias=mem_read_i32(B,dp+52),.depthBiasSlopeScale=mem_read_f32(B,dp+56),.depthBiasClamp=mem_read_f32(B,dp+60)};
        ds_ptr=&ds;
    }

    /* Dawn: WGPUVertexBufferLayout has stepMode before arrayStride */
    uint32_t vb_count=mem_read_u32(B,p+60), vb_ptr_val=mem_read_u32(B,p+64);
    if(vb_count>16) vb_count=16;
    WGPUVertexBufferLayout vbls[16]={0}; WGPUVertexAttribute all_attrs[256]={0}; uint32_t attr_cursor=0;
    for(uint32_t i=0;i<vb_count;i++){
        uint32_t vb=vb_ptr_val+i*16;
        uint32_t stride=mem_read_u32(B,vb), step=mem_read_u32(B,vb+4);
        uint32_t ac=mem_read_u32(B,vb+8), ap=mem_read_u32(B,vb+12);
        if(ac>16) ac=16;
        for(uint32_t j=0;j<ac&&attr_cursor<256;j++){
            uint32_t ab=ap+j*12;
            all_attrs[attr_cursor++]=(WGPUVertexAttribute){.format=(WGPUVertexFormat)mem_read_u32(B,ab),.offset=(uint64_t)mem_read_u32(B,ab+4),.shaderLocation=mem_read_u32(B,ab+8)};
        }
        vbls[i]=(WGPUVertexBufferLayout){.stepMode=(WGPUVertexStepMode)step,.arrayStride=stride,.attributeCount=ac,.attributes=&all_attrs[attr_cursor-ac]};
    }

    uint32_t sc=mem_read_u32(B,p+68); if(!sc) sc=1;
    uint32_t sm=mem_read_u32(B,p+72); if(!sm) sm=0xFFFFFFFF;

    WGPUPipelineLayout layout=layout_h?htable_get(&B->ht_pipeline_layout,layout_h):NULL;
    WGPURenderPipelineDescriptor desc={.layout=layout,
        .vertex={.module=vs,.entryPoint={.data=vs_entry,.length=vs_elen},.bufferCount=vb_count,.buffers=vb_count?vbls:NULL},
        .primitive=prim,.depthStencil=ds_ptr,.multisample={.count=sc,.mask=sm,.alphaToCoverageEnabled=mem_read_u32(B,p+76)!=0},
        .fragment=frag_ptr};
    WGPURenderPipeline rp=wgpuDeviceCreateRenderPipeline(DEV,&desc);
    RET_I32(rp?htable_insert(&B->ht_render_pipeline,rp):0); return NULL;
}

static wasm_trap_t *fn_render_pipeline_release(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPURenderPipeline p=htable_get(&B->ht_render_pipeline,h);
    if(p){wgpuRenderPipelineRelease(p);htable_remove(&B->ht_render_pipeline,h);}
    return NULL;
}

static wasm_trap_t *fn_render_pipeline_get_bind_group_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPURenderPipeline p=htable_get(&B->ht_render_pipeline,(uint32_t)ARG_I32(0));
    if(!p){RET_I32(0);return NULL;}
    WGPUBindGroupLayout bgl=wgpuRenderPipelineGetBindGroupLayout(p,(uint32_t)ARG_I32(1));
    RET_I32(bgl?htable_insert(&B->ht_bind_group_layout,bgl):0); return NULL;
}

/* ================================================================== */
/*  Compute Pipeline                                                   */
/* ================================================================== */

static wasm_trap_t *fn_create_compute_pipeline(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUPipelineLayout layout=htable_get(&B->ht_pipeline_layout,(uint32_t)ARG_I32(0));
    WGPUShaderModule shader=htable_get(&B->ht_shader,(uint32_t)ARG_I32(1));
    uint32_t ep=(uint32_t)ARG_I32(2),el=(uint32_t)ARG_I32(3);
    if(!shader){RET_I32(0);return NULL;}
    const char *entry=el>0?mem_read_str(B,ep,el):"main";
    uint32_t elen=el>0?el:4;
    WGPUComputePipelineDescriptor desc={.layout=layout,.compute={.module=shader,.entryPoint={.data=entry,.length=elen}}};
    WGPUComputePipeline cp=wgpuDeviceCreateComputePipeline(DEV,&desc);
    RET_I32(cp?htable_insert(&B->ht_compute_pipeline,cp):0); return NULL;
}

static wasm_trap_t *fn_compute_pipeline_release(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPUComputePipeline p=htable_get(&B->ht_compute_pipeline,h);
    if(p){wgpuComputePipelineRelease(p);htable_remove(&B->ht_compute_pipeline,h);}
    return NULL;
}

static wasm_trap_t *fn_compute_pipeline_get_bind_group_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUComputePipeline p=htable_get(&B->ht_compute_pipeline,(uint32_t)ARG_I32(0));
    if(!p){RET_I32(0);return NULL;}
    WGPUBindGroupLayout bgl=wgpuComputePipelineGetBindGroupLayout(p,(uint32_t)ARG_I32(1));
    RET_I32(bgl?htable_insert(&B->ht_bind_group_layout,bgl):0); return NULL;
}

/* ================================================================== */
/*  Command Encoder                                                    */
/* ================================================================== */

static wasm_trap_t *fn_create_command_encoder(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I32(htable_insert(&B->ht_command_encoder,wgpuDeviceCreateCommandEncoder(DEV,NULL)));
    return NULL;
}

static wasm_trap_t *fn_encoder_finish(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t h=(uint32_t)ARG_I32(0);
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,h);
    if(!enc){RET_I32(0);return NULL;}
    WGPUCommandBuffer cmd=wgpuCommandEncoderFinish(enc,NULL);
    wgpuCommandEncoderRelease(enc); htable_remove(&B->ht_command_encoder,h);
    RET_I32(htable_insert(&B->ht_command_buffer,cmd)); return NULL;
}

static wasm_trap_t *fn_encoder_copy_buffer_to_buffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,(uint32_t)ARG_I32(0));
    WGPUBuffer src=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(1));
    WGPUBuffer dst=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(3));
    if(enc&&src&&dst) wgpuCommandEncoderCopyBufferToBuffer(enc,src,(uint64_t)(uint32_t)ARG_I32(2),dst,(uint64_t)(uint32_t)ARG_I32(4),(uint64_t)(uint32_t)ARG_I32(5));
    return NULL;
}

static wasm_trap_t *fn_encoder_copy_buffer_to_texture(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,(uint32_t)ARG_I32(0));
    uint32_t p=(uint32_t)ARG_I32(1);
    if(!enc) return NULL;
    WGPUBuffer buf=htable_get(&B->ht_buffer,mem_read_u32(B,p));
    WGPUTexture tex=htable_get(&B->ht_texture,mem_read_u32(B,p+16));
    if(!buf||!tex) return NULL;
    WGPUTexelCopyBufferInfo src_info={.layout={.offset=(uint64_t)mem_read_u32(B,p+4),.bytesPerRow=mem_read_u32(B,p+8),.rowsPerImage=mem_read_u32(B,p+12)},.buffer=buf};
    WGPUTexelCopyTextureInfo dst_info={.texture=tex,.mipLevel=mem_read_u32(B,p+20),.origin={mem_read_u32(B,p+24),mem_read_u32(B,p+28),mem_read_u32(B,p+32)}};
    WGPUExtent3D ext={mem_read_u32(B,p+36),mem_read_u32(B,p+40),mem_read_u32(B,p+44)};
    wgpuCommandEncoderCopyBufferToTexture(enc,&src_info,&dst_info,&ext);
    return NULL;
}

static wasm_trap_t *fn_encoder_copy_texture_to_buffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,(uint32_t)ARG_I32(0));
    uint32_t p=(uint32_t)ARG_I32(1);
    if(!enc) return NULL;
    WGPUTexture tex=htable_get(&B->ht_texture,mem_read_u32(B,p));
    WGPUBuffer buf=htable_get(&B->ht_buffer,mem_read_u32(B,p+20));
    if(!tex||!buf) return NULL;
    WGPUTexelCopyTextureInfo src_info={.texture=tex,.mipLevel=mem_read_u32(B,p+4),.origin={mem_read_u32(B,p+8),mem_read_u32(B,p+12),mem_read_u32(B,p+16)}};
    WGPUTexelCopyBufferInfo dst_info={.layout={.offset=(uint64_t)mem_read_u32(B,p+24),.bytesPerRow=mem_read_u32(B,p+28),.rowsPerImage=mem_read_u32(B,p+32)},.buffer=buf};
    WGPUExtent3D ext={mem_read_u32(B,p+36),mem_read_u32(B,p+40),mem_read_u32(B,p+44)};
    wgpuCommandEncoderCopyTextureToBuffer(enc,&src_info,&dst_info,&ext);
    return NULL;
}

static wasm_trap_t *fn_encoder_copy_texture_to_texture(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,(uint32_t)ARG_I32(0));
    uint32_t p=(uint32_t)ARG_I32(1);
    if(!enc) return NULL;
    WGPUTexture st=htable_get(&B->ht_texture,mem_read_u32(B,p));
    WGPUTexture dt=htable_get(&B->ht_texture,mem_read_u32(B,p+20));
    if(!st||!dt) return NULL;
    WGPUTexelCopyTextureInfo src_info={.texture=st,.mipLevel=mem_read_u32(B,p+4),.origin={mem_read_u32(B,p+8),mem_read_u32(B,p+12),mem_read_u32(B,p+16)}};
    WGPUTexelCopyTextureInfo dst_info={.texture=dt,.mipLevel=mem_read_u32(B,p+24),.origin={mem_read_u32(B,p+28),mem_read_u32(B,p+32),mem_read_u32(B,p+36)}};
    WGPUExtent3D ext={mem_read_u32(B,p+40),mem_read_u32(B,p+44),mem_read_u32(B,p+48)};
    wgpuCommandEncoderCopyTextureToTexture(enc,&src_info,&dst_info,&ext);
    return NULL;
}

static wasm_trap_t *fn_encoder_clear_buffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,(uint32_t)ARG_I32(0));
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(1));
    if(enc&&buf) wgpuCommandEncoderClearBuffer(enc,buf,(uint64_t)(uint32_t)ARG_I32(2),(uint64_t)(uint32_t)ARG_I32(3));
    return NULL;
}

/* ================================================================== */
/*  Render Pass                                                        */
/* ================================================================== */

static wasm_trap_t *fn_begin_render_pass(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,(uint32_t)ARG_I32(0));
    WGPUTextureView view=htable_get(&B->ht_texture_view,(uint32_t)ARG_I32(1));
    if(!enc||!view){RET_I32(0);return NULL;}
    WGPURenderPassColorAttachment ca={.view=view,.depthSlice=WGPU_DEPTH_SLICE_UNDEFINED,.loadOp=WGPULoadOp_Clear,.storeOp=WGPUStoreOp_Store,
        .clearValue=(WGPUColor){ARG_F32(2),ARG_F32(3),ARG_F32(4),ARG_F32(5)}};
    WGPURenderPassDescriptor rp={.colorAttachmentCount=1,.colorAttachments=&ca};
    WGPURenderPassEncoder pass=wgpuCommandEncoderBeginRenderPass(enc,&rp);
    RET_I32(htable_insert(&B->ht_render_pass,pass)); return NULL;
}

static wasm_trap_t *fn_begin_render_pass_desc(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,(uint32_t)ARG_I32(0));
    uint32_t dp=(uint32_t)ARG_I32(1);
    if(!enc){RET_I32(0);return NULL;}
    uint32_t cc=mem_read_u32(B,dp), cp=mem_read_u32(B,dp+4);
    if(cc>8) cc=8;
    WGPURenderPassColorAttachment cas[8];
    for(uint32_t i=0;i<cc;i++){
        uint32_t cb=cp+i*32;
        WGPUTextureView v=htable_get(&B->ht_texture_view,mem_read_u32(B,cb));
        uint32_t rv=mem_read_u32(B,cb+4);
        cas[i]=(WGPURenderPassColorAttachment){
            .view=v,.resolveTarget=rv?htable_get(&B->ht_texture_view,rv):NULL,
            .depthSlice=WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp=(WGPULoadOp)mem_read_u32(B,cb+8),.storeOp=(WGPUStoreOp)mem_read_u32(B,cb+12),
            .clearValue=(WGPUColor){mem_read_f32(B,cb+16),mem_read_f32(B,cb+20),mem_read_f32(B,cb+24),mem_read_f32(B,cb+28)}};
    }
    WGPURenderPassDepthStencilAttachment dsa={0}; WGPURenderPassDepthStencilAttachment *dsa_ptr=NULL;
    if(mem_read_u32(B,dp+8)){
        uint32_t ddp=mem_read_u32(B,dp+12);
        dsa=(WGPURenderPassDepthStencilAttachment){
            .view=htable_get(&B->ht_texture_view,mem_read_u32(B,ddp)),
            .depthLoadOp=(WGPULoadOp)mem_read_u32(B,ddp+4),.depthStoreOp=(WGPUStoreOp)mem_read_u32(B,ddp+8),
            .depthClearValue=mem_read_f32(B,ddp+12),
            .stencilLoadOp=(WGPULoadOp)mem_read_u32(B,ddp+16),.stencilStoreOp=(WGPUStoreOp)mem_read_u32(B,ddp+20),
            .stencilClearValue=mem_read_u32(B,ddp+24)};
        dsa_ptr=&dsa;
    }
    WGPURenderPassDescriptor rpd={.colorAttachmentCount=cc,.colorAttachments=cas,.depthStencilAttachment=dsa_ptr};
    uint32_t qs_h=mem_read_u32(B,dp+16);
    if(qs_h) rpd.occlusionQuerySet=htable_get(&B->ht_query_set,qs_h);
    WGPURenderPassEncoder pass=wgpuCommandEncoderBeginRenderPass(enc,&rpd);
    RET_I32(pass?htable_insert(&B->ht_render_pass,pass):0); return NULL;
}

static wasm_trap_t *fn_pass_set_pipeline(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    WGPURenderPipeline pip=htable_get(&B->ht_render_pipeline,(uint32_t)ARG_I32(1));
    if(pass&&pip) wgpuRenderPassEncoderSetPipeline(pass,pip);
    return NULL;
}
static wasm_trap_t *fn_pass_set_vertex_buffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(2));
    if(pass&&buf) wgpuRenderPassEncoderSetVertexBuffer(pass,(uint32_t)ARG_I32(1),buf,(uint64_t)(uint32_t)ARG_I32(3),(uint64_t)(uint32_t)ARG_I32(4));
    return NULL;
}
static wasm_trap_t *fn_pass_set_index_buffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(1));
    if(pass&&buf) wgpuRenderPassEncoderSetIndexBuffer(pass,buf,(WGPUIndexFormat)ARG_I32(2),(uint64_t)(uint32_t)ARG_I32(3),(uint64_t)(uint32_t)ARG_I32(4));
    return NULL;
}
static wasm_trap_t *fn_pass_set_bind_group(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    WGPUBindGroup bg=htable_get(&B->ht_bind_group,(uint32_t)ARG_I32(2));
    if(!pass||!bg) return NULL;
    uint32_t dyn_ptr=(uint32_t)ARG_I32(3), dyn_count=(uint32_t)ARG_I32(4);
    const uint32_t *dyn=NULL;
    if(dyn_count>0&&dyn_ptr) dyn=(const uint32_t*)(wasm_mem_base(B)+dyn_ptr);
    wgpuRenderPassEncoderSetBindGroup(pass,(uint32_t)ARG_I32(1),bg,dyn_count,dyn);
    return NULL;
}
static wasm_trap_t *fn_pass_draw(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    if(pass) wgpuRenderPassEncoderDraw(pass,(uint32_t)ARG_I32(1),(uint32_t)ARG_I32(2),(uint32_t)ARG_I32(3),(uint32_t)ARG_I32(4));
    return NULL;
}
static wasm_trap_t *fn_pass_draw_indexed(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    if(pass) wgpuRenderPassEncoderDrawIndexed(pass,(uint32_t)ARG_I32(1),(uint32_t)ARG_I32(2),(uint32_t)ARG_I32(3),(int32_t)ARG_I32(4),(uint32_t)ARG_I32(5));
    return NULL;
}
static wasm_trap_t *fn_pass_draw_indirect(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(1));
    if(pass&&buf) wgpuRenderPassEncoderDrawIndirect(pass,buf,(uint64_t)(uint32_t)ARG_I32(2));
    return NULL;
}
static wasm_trap_t *fn_pass_draw_indexed_indirect(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(1));
    if(pass&&buf) wgpuRenderPassEncoderDrawIndexedIndirect(pass,buf,(uint64_t)(uint32_t)ARG_I32(2));
    return NULL;
}
static wasm_trap_t *fn_pass_set_viewport(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    if(pass) wgpuRenderPassEncoderSetViewport(pass,ARG_F32(1),ARG_F32(2),ARG_F32(3),ARG_F32(4),ARG_F32(5),ARG_F32(6));
    return NULL;
}
static wasm_trap_t *fn_pass_set_scissor_rect(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    if(pass) wgpuRenderPassEncoderSetScissorRect(pass,(uint32_t)ARG_I32(1),(uint32_t)ARG_I32(2),(uint32_t)ARG_I32(3),(uint32_t)ARG_I32(4));
    return NULL;
}
static wasm_trap_t *fn_pass_set_stencil_reference(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    if(pass) wgpuRenderPassEncoderSetStencilReference(pass,(uint32_t)ARG_I32(1));
    return NULL;
}
static wasm_trap_t *fn_pass_set_blend_constant(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    if(pass) wgpuRenderPassEncoderSetBlendConstant(pass,&(WGPUColor){ARG_F32(1),ARG_F32(2),ARG_F32(3),ARG_F32(4)});
    return NULL;
}
static wasm_trap_t *fn_pass_begin_occlusion_query(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    if(pass) wgpuRenderPassEncoderBeginOcclusionQuery(pass,(uint32_t)ARG_I32(1));
    return NULL;
}
static wasm_trap_t *fn_pass_end_occlusion_query(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,(uint32_t)ARG_I32(0));
    if(pass) wgpuRenderPassEncoderEndOcclusionQuery(pass);
    return NULL;
}
static wasm_trap_t *fn_pass_end(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPURenderPassEncoder pass=htable_get(&B->ht_render_pass,h);
    if(pass){wgpuRenderPassEncoderEnd(pass);wgpuRenderPassEncoderRelease(pass);htable_remove(&B->ht_render_pass,h);}
    return NULL;
}

/* ================================================================== */
/*  Compute Pass                                                       */
/* ================================================================== */

static wasm_trap_t *fn_begin_compute_pass(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,(uint32_t)ARG_I32(0));
    if(!enc){RET_I32(0);return NULL;}
    WGPUComputePassDescriptor desc={0};
    WGPUComputePassEncoder pass=wgpuCommandEncoderBeginComputePass(enc,&desc);
    RET_I32(pass?htable_insert(&B->ht_compute_pass,pass):0); return NULL;
}
static wasm_trap_t *fn_compute_pass_set_pipeline(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUComputePassEncoder pass=htable_get(&B->ht_compute_pass,(uint32_t)ARG_I32(0));
    WGPUComputePipeline pip=htable_get(&B->ht_compute_pipeline,(uint32_t)ARG_I32(1));
    if(pass&&pip) wgpuComputePassEncoderSetPipeline(pass,pip);
    return NULL;
}
static wasm_trap_t *fn_compute_pass_set_bind_group(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUComputePassEncoder pass=htable_get(&B->ht_compute_pass,(uint32_t)ARG_I32(0));
    WGPUBindGroup bg=htable_get(&B->ht_bind_group,(uint32_t)ARG_I32(2));
    if(!pass||!bg) return NULL;
    uint32_t dp=(uint32_t)ARG_I32(3),dc=(uint32_t)ARG_I32(4);
    const uint32_t *dyn=(dc>0&&dp)?(const uint32_t*)(wasm_mem_base(B)+dp):NULL;
    wgpuComputePassEncoderSetBindGroup(pass,(uint32_t)ARG_I32(1),bg,dc,dyn);
    return NULL;
}
static wasm_trap_t *fn_compute_pass_dispatch(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUComputePassEncoder pass=htable_get(&B->ht_compute_pass,(uint32_t)ARG_I32(0));
    if(pass) wgpuComputePassEncoderDispatchWorkgroups(pass,(uint32_t)ARG_I32(1),(uint32_t)ARG_I32(2),(uint32_t)ARG_I32(3));
    return NULL;
}
static wasm_trap_t *fn_compute_pass_dispatch_indirect(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUComputePassEncoder pass=htable_get(&B->ht_compute_pass,(uint32_t)ARG_I32(0));
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(1));
    if(pass&&buf) wgpuComputePassEncoderDispatchWorkgroupsIndirect(pass,buf,(uint64_t)(uint32_t)ARG_I32(2));
    return NULL;
}
static wasm_trap_t *fn_compute_pass_end(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPUComputePassEncoder pass=htable_get(&B->ht_compute_pass,h);
    if(pass){wgpuComputePassEncoderEnd(pass);wgpuComputePassEncoderRelease(pass);htable_remove(&B->ht_compute_pass,h);}
    return NULL;
}

/* ================================================================== */
/*  Queue                                                              */
/* ================================================================== */

static wasm_trap_t *fn_queue_submit(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t cmd_h=(uint32_t)ARG_I32(1);
    WGPUCommandBuffer cmd=htable_get(&B->ht_command_buffer,cmd_h);
    if(cmd){wgpuQueueSubmit(QUE,1,&cmd);wgpuCommandBufferRelease(cmd);htable_remove(&B->ht_command_buffer,cmd_h);}
    return NULL;
}

/* ================================================================== */
/*  Surface                                                            */
/* ================================================================== */

static wasm_trap_t *fn_surface_get_view(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    release_frame_resources(B);

    /* Offscreen mode: return the pre-assigned offscreen texture view */
    if (B->offscreen && B->offscreen_texture && B->offscreen_view) {
        B->frame_texture = B->offscreen_texture;
        B->frame_texture_handle = htable_insert(&B->ht_texture, B->offscreen_texture);
        B->frame_view = B->offscreen_view;
        B->frame_view_handle = htable_insert(&B->ht_texture_view, B->offscreen_view);
        RET_I32(B->frame_view_handle); return NULL;
    }

    WGPUSurfaceTexture st;
    wgpuSurfaceGetCurrentTexture(GPU->surface,&st);
    if(st.status!=WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal&&st.status!=WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal){RET_I32(0);return NULL;}
    B->frame_texture=st.texture;
    B->frame_texture_handle=htable_insert(&B->ht_texture,st.texture);
    WGPUTextureView view=wgpuTextureCreateView(st.texture,NULL);
    B->frame_view=view;
    B->frame_view_handle=htable_insert(&B->ht_texture_view,view);
    RET_I32(B->frame_view_handle); return NULL;
}

static wasm_trap_t *fn_surface_present(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;(void)res;

    /* Offscreen mode: no-op, dashboard composites later */
    if (B->offscreen) {
        /* Just clean up frame handles without presenting */
        if (B->frame_view_handle) { htable_remove(&B->ht_texture_view, B->frame_view_handle); B->frame_view_handle = 0; }
        if (B->frame_texture_handle) { htable_remove(&B->ht_texture, B->frame_texture_handle); B->frame_texture_handle = 0; }
        B->frame_texture = NULL;
        B->frame_view = NULL;
        return NULL;
    }

    WGPUStatus status = wgpuSurfacePresent(GPU->surface);
    if (status != WGPUStatus_Success)
    {
        fprintf(stderr, "[wgpu] Failed to present surface! Error code: %u\n", status);
    }
    release_frame_resources(B);
    return NULL;
}

/* ================================================================== */
/*  Query Set                                                          */
/* ================================================================== */

static wasm_trap_t *fn_create_query_set(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    WGPUQuerySetDescriptor desc={.type=(WGPUQueryType)ARG_I32(0),.count=(uint32_t)ARG_I32(1)};
    WGPUQuerySet qs=wgpuDeviceCreateQuerySet(DEV,&desc);
    RET_I32(qs?htable_insert(&B->ht_query_set,qs):0); return NULL;
}

static wasm_trap_t *fn_query_set_destroy(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res; uint32_t h=(uint32_t)ARG_I32(0);
    WGPUQuerySet qs=htable_get(&B->ht_query_set,h);
    if(qs){wgpuQuerySetDestroy(qs);wgpuQuerySetRelease(qs);htable_remove(&B->ht_query_set,h);}
    return NULL;
}

static wasm_trap_t *fn_encoder_resolve_query_set(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    WGPUCommandEncoder enc=htable_get(&B->ht_command_encoder,(uint32_t)ARG_I32(0));
    WGPUQuerySet qs=htable_get(&B->ht_query_set,(uint32_t)ARG_I32(1));
    WGPUBuffer buf=htable_get(&B->ht_buffer,(uint32_t)ARG_I32(4));
    if(enc&&qs&&buf) wgpuCommandEncoderResolveQuerySet(enc,qs,(uint32_t)ARG_I32(2),(uint32_t)ARG_I32(3),buf,(uint64_t)(uint32_t)ARG_I32(5));
    return NULL;
}

/* ================================================================== */
/*  Binding table                                                      */
/* ================================================================== */

typedef struct {
    const char *name;
    wasm_func_callback_with_env_t cb;
    uint32_t np; wasm_valkind_t params[16];
    uint32_t nr; wasm_valkind_t results[1];
} BindingEntry;

#define I WASM_I32
#define F WASM_F32

static const BindingEntry WGPU_BINDINGS[] = {
    /* Device/Queue/Surface info */
    {"wgpu_get_device",         fn_get_device,         0, {0},       1, {I}},
    {"wgpu_get_queue",          fn_get_queue,          0, {0},       1, {I}},
    {"wgpu_get_surface_format", fn_get_surface_format, 0, {0},       1, {I}},
    {"wgpu_get_surface_width",  fn_get_surface_width,  0, {0},       1, {I}},
    {"wgpu_get_surface_height", fn_get_surface_height, 0, {0},       1, {I}},
    {"wgpu_device_poll",        fn_device_poll,        1, {I},       1, {I}},

    /* Enum queries */
    {"wgpu_get_vertex_format",  fn_get_vertex_format,  1, {I},       1, {I}},
    {"wgpu_get_topology",       fn_get_topology,       1, {I},       1, {I}},

    /* Shader */
    {"wgpu_create_shader_spirv", fn_create_shader_spirv, 5, {I,I,I,I,I}, 1, {I}},
    {"wgpu_create_shader_wgsl",  fn_create_shader_wgsl,  3, {I,I,I},     1, {I}},
    {"wgpu_shader_release",      fn_shader_release,      1, {I},          0, {0}},

    /* Buffer */
    {"wgpu_create_buffer",          fn_create_buffer,          4, {I,I,I,I},   1, {I}},
    {"wgpu_buffer_write",           fn_buffer_write,           4, {I,I,I,I},   0, {0}},
    {"wgpu_buffer_destroy",         fn_buffer_destroy,         1, {I},          0, {0}},
    {"wgpu_buffer_get_size",        fn_buffer_get_size,        1, {I},          1, {I}},
    {"wgpu_buffer_map_sync",        fn_buffer_map_sync,        4, {I,I,I,I},   1, {I}},
    {"wgpu_buffer_get_mapped_range",fn_buffer_get_mapped_range,4, {I,I,I,I},   0, {0}},
    {"wgpu_buffer_unmap",           fn_buffer_unmap,           1, {I},          0, {0}},

    /* Texture */
    {"wgpu_create_texture",      fn_create_texture,      1, {I},     1, {I}},
    {"wgpu_create_texture_view", fn_create_texture_view, 2, {I,I},   1, {I}},
    {"wgpu_queue_write_texture", fn_queue_write_texture, 1, {I},     0, {0}},
    {"wgpu_texture_destroy",     fn_texture_destroy,     1, {I},     0, {0}},
    {"wgpu_texture_release",     fn_texture_release,     1, {I},     0, {0}},
    {"wgpu_texture_view_release",fn_texture_view_release,1, {I},     0, {0}},

    /* Sampler */
    {"wgpu_create_sampler",  fn_create_sampler,  1, {I},   1, {I}},
    {"wgpu_sampler_release", fn_sampler_release, 1, {I},   0, {0}},

    /* Bind group layout + bind group */
    {"wgpu_create_bind_group_layout", fn_create_bind_group_layout, 3, {I,I,I}, 1, {I}},
    {"wgpu_bind_group_layout_release",fn_bind_group_layout_release,1, {I},      0, {0}},
    {"wgpu_create_bind_group",        fn_create_bind_group,        4, {I,I,I,I},1, {I}},
    {"wgpu_bind_group_release",       fn_bind_group_release,       1, {I},      0, {0}},

    /* Pipeline layout */
    {"wgpu_create_pipeline_layout_empty", fn_create_pipeline_layout_empty, 1, {I},     1, {I}},
    {"wgpu_create_pipeline_layout",       fn_create_pipeline_layout,       3, {I,I,I}, 1, {I}},
    {"wgpu_pipeline_layout_release",      fn_pipeline_layout_release,      1, {I},     0, {0}},

    /* Render pipeline */
    {"wgpu_create_render_pipeline",     fn_create_render_pipeline,     11, {I,I,I,I,I,I,I,I,I,I,I}, 1, {I}},
    {"wgpu_create_render_pipeline_desc",fn_create_render_pipeline_desc,1,  {I},                      1, {I}},
    {"wgpu_render_pipeline_release",    fn_render_pipeline_release,    1,  {I},                      0, {0}},
    {"wgpu_render_pipeline_get_bind_group_layout", fn_render_pipeline_get_bind_group_layout, 2, {I,I}, 1, {I}},

    /* Compute pipeline */
    {"wgpu_create_compute_pipeline",    fn_create_compute_pipeline,    4, {I,I,I,I}, 1, {I}},
    {"wgpu_compute_pipeline_release",   fn_compute_pipeline_release,   1, {I},       0, {0}},
    {"wgpu_compute_pipeline_get_bind_group_layout", fn_compute_pipeline_get_bind_group_layout, 2, {I,I}, 1, {I}},

    /* Command encoder */
    {"wgpu_create_command_encoder",          fn_create_command_encoder,          1, {I},             1, {I}},
    {"wgpu_encoder_finish",                  fn_encoder_finish,                  1, {I},             1, {I}},
    {"wgpu_encoder_copy_buffer_to_buffer",   fn_encoder_copy_buffer_to_buffer,   6, {I,I,I,I,I,I},  0, {0}},
    {"wgpu_encoder_copy_buffer_to_texture",  fn_encoder_copy_buffer_to_texture,  2, {I,I},          0, {0}},
    {"wgpu_encoder_copy_texture_to_buffer",  fn_encoder_copy_texture_to_buffer,  2, {I,I},          0, {0}},
    {"wgpu_encoder_copy_texture_to_texture", fn_encoder_copy_texture_to_texture, 2, {I,I},          0, {0}},
    {"wgpu_encoder_clear_buffer",            fn_encoder_clear_buffer,            4, {I,I,I,I},      0, {0}},
    {"wgpu_encoder_resolve_query_set",       fn_encoder_resolve_query_set,       6, {I,I,I,I,I,I},  0, {0}},

    /* Render pass */
    {"wgpu_begin_render_pass",                    fn_begin_render_pass,          6, {I,I,F,F,F,F},   1, {I}},
    {"wgpu_begin_render_pass_desc",               fn_begin_render_pass_desc,     2, {I,I},           1, {I}},
    {"wgpu_render_pass_set_pipeline",             fn_pass_set_pipeline,          2, {I,I},           0, {0}},
    {"wgpu_render_pass_set_vertex_buffer",        fn_pass_set_vertex_buffer,     5, {I,I,I,I,I},    0, {0}},
    {"wgpu_render_pass_set_index_buffer",         fn_pass_set_index_buffer,      5, {I,I,I,I,I},    0, {0}},
    {"wgpu_render_pass_set_bind_group",           fn_pass_set_bind_group,        5, {I,I,I,I,I},    0, {0}},
    {"wgpu_render_pass_draw",                     fn_pass_draw,                  5, {I,I,I,I,I},    0, {0}},
    {"wgpu_render_pass_draw_indexed",             fn_pass_draw_indexed,          6, {I,I,I,I,I,I},  0, {0}},
    {"wgpu_render_pass_draw_indirect",            fn_pass_draw_indirect,         3, {I,I,I},        0, {0}},
    {"wgpu_render_pass_draw_indexed_indirect",    fn_pass_draw_indexed_indirect, 3, {I,I,I},        0, {0}},
    {"wgpu_render_pass_set_viewport",             fn_pass_set_viewport,          7, {I,F,F,F,F,F,F},0, {0}},
    {"wgpu_render_pass_set_scissor_rect",         fn_pass_set_scissor_rect,      5, {I,I,I,I,I},   0, {0}},
    {"wgpu_render_pass_set_stencil_reference",    fn_pass_set_stencil_reference, 2, {I,I},          0, {0}},
    {"wgpu_render_pass_set_blend_constant",       fn_pass_set_blend_constant,    5, {I,F,F,F,F},    0, {0}},
    {"wgpu_render_pass_begin_occlusion_query",    fn_pass_begin_occlusion_query, 2, {I,I},          0, {0}},
    {"wgpu_render_pass_end_occlusion_query",      fn_pass_end_occlusion_query,   1, {I},            0, {0}},
    {"wgpu_render_pass_end",                      fn_pass_end,                   1, {I},            0, {0}},

    /* Compute pass */
    {"wgpu_begin_compute_pass",              fn_begin_compute_pass,              1, {I},           1, {I}},
    {"wgpu_compute_pass_set_pipeline",       fn_compute_pass_set_pipeline,       2, {I,I},         0, {0}},
    {"wgpu_compute_pass_set_bind_group",     fn_compute_pass_set_bind_group,     5, {I,I,I,I,I},  0, {0}},
    {"wgpu_compute_pass_dispatch",           fn_compute_pass_dispatch,           4, {I,I,I,I},    0, {0}},
    {"wgpu_compute_pass_dispatch_indirect",  fn_compute_pass_dispatch_indirect,  3, {I,I,I},      0, {0}},
    {"wgpu_compute_pass_end",                fn_compute_pass_end,                1, {I},           0, {0}},

    /* Queue */
    {"wgpu_queue_submit", fn_queue_submit, 2, {I,I}, 0, {0}},

    /* Surface */
    {"wgpu_surface_get_current_texture_view", fn_surface_get_view, 0, {0}, 1, {I}},
    {"wgpu_surface_present",                  fn_surface_present,  0, {0}, 0, {0}},

    /* Query set */
    {"wgpu_create_query_set",  fn_create_query_set,  2, {I,I}, 1, {I}},
    {"wgpu_query_set_destroy", fn_query_set_destroy,  1, {I},   0, {0}},
};

#undef I
#undef F

#define NUM_WGPU_BINDINGS (sizeof(WGPU_BINDINGS)/sizeof(WGPU_BINDINGS[0]))

/* ================================================================== */
/*  functype builder                                                   */
/* ================================================================== */

static wasm_functype_t *make_ft(uint32_t np, const wasm_valkind_t p[], uint32_t nr, const wasm_valkind_t r[]) {
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[16];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else wasm_valtype_vec_new_empty(&params);
    if (nr > 0) {
        wasm_valtype_t *rt[1];
        rt[0] = wasm_valtype_new(r[0]);
        wasm_valtype_vec_new(&results, nr, rt);
    } else wasm_valtype_vec_new_empty(&results);
    return wasm_functype_new(&params, &results);
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void wgpu_bindings_init(WgpuBindings *b, GpuContext *gpu) {
    memset(b, 0, sizeof(*b));
    b->gpu = gpu;
    htable_init(&b->ht_device, 64);
    htable_init(&b->ht_queue, 64);
    htable_init(&b->ht_buffer, 256);
    htable_init(&b->ht_texture, 128);
    htable_init(&b->ht_texture_view, 128);
    htable_init(&b->ht_sampler, 64);
    htable_init(&b->ht_shader, 64);
    htable_init(&b->ht_bind_group_layout, 64);
    htable_init(&b->ht_bind_group, 64);
    htable_init(&b->ht_pipeline_layout, 64);
    htable_init(&b->ht_render_pipeline, 64);
    htable_init(&b->ht_compute_pipeline, 64);
    htable_init(&b->ht_command_encoder, 64);
    htable_init(&b->ht_render_pass, 64);
    htable_init(&b->ht_compute_pass, 64);
    htable_init(&b->ht_command_buffer, 64);
    htable_init(&b->ht_surface, 16);
    htable_init(&b->ht_query_set, 32);
    b->h_device = htable_insert(&b->ht_device, gpu->device);
    b->h_queue = htable_insert(&b->ht_queue, gpu->queue);
    b->h_surface = htable_insert(&b->ht_surface, gpu->surface);
    printf("[wgpu] Initialized WebGPU bindings (Dawn) (%zu imports)\n", NUM_WGPU_BINDINGS);
}

void wgpu_bindings_set_memory(WgpuBindings *b, wasm_memory_t *mem) { b->memory = mem; }

size_t wgpu_bindings_get_imports(WgpuBindings *b, wasm_store_t *store, const char ***out_names, wasm_func_t ***out_funcs) {
    static const char *names[NUM_WGPU_BINDINGS];
    static wasm_func_t *funcs[NUM_WGPU_BINDINGS];
    for (size_t i = 0; i < NUM_WGPU_BINDINGS; i++) {
        names[i] = WGPU_BINDINGS[i].name;
        wasm_functype_t *ft = make_ft(WGPU_BINDINGS[i].np, WGPU_BINDINGS[i].params, WGPU_BINDINGS[i].nr, WGPU_BINDINGS[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft, WGPU_BINDINGS[i].cb, b, NULL);
        wasm_functype_delete(ft);
    }
    *out_names = names;
    *out_funcs = funcs;
    return NUM_WGPU_BINDINGS;
}

void wgpu_bindings_destroy(WgpuBindings *b) {
    release_frame_resources(b);
    htable_destroy(&b->ht_device);
    htable_destroy(&b->ht_queue);
    htable_destroy(&b->ht_buffer);
    htable_destroy(&b->ht_texture);
    htable_destroy(&b->ht_texture_view);
    htable_destroy(&b->ht_sampler);
    htable_destroy(&b->ht_shader);
    htable_destroy(&b->ht_bind_group_layout);
    htable_destroy(&b->ht_bind_group);
    htable_destroy(&b->ht_pipeline_layout);
    htable_destroy(&b->ht_render_pipeline);
    htable_destroy(&b->ht_compute_pipeline);
    htable_destroy(&b->ht_command_encoder);
    htable_destroy(&b->ht_render_pass);
    htable_destroy(&b->ht_compute_pass);
    htable_destroy(&b->ht_command_buffer);
    htable_destroy(&b->ht_surface);
    htable_destroy(&b->ht_query_set);
}
