/*
    Slang Shader Compiler WASM Bindings (C++ implementation)
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

/*
 * slang_bindings.cpp — Full-parity WASM bindings for the Slang shader compiler.
 *
 * Compiled as C++, exports extern "C" API for the WASM runtime.
 * Each WASM import function has a corresponding entry in the binding table
 * at the bottom of this file.
 */

#include "slang_bindings.h"
#include "slang.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <webgpu.h>

using namespace slang;

/* ================================================================== */
/*  WASM memory helpers                                                */
/* ================================================================== */

#define SB ((SlangBindings *)env)

static uint8_t *sb_mem_base(SlangBindings *sb) {
    return sb->memory ? (uint8_t *)wasm_memory_data(sb->memory) : nullptr;
}
static size_t sb_mem_size(SlangBindings *sb) {
    return sb->memory ? wasm_memory_data_size(sb->memory) : 0;
}
static bool sb_mem_write(SlangBindings *sb, uint32_t ptr, const void *src, size_t len) {
    if ((size_t)ptr + len > sb_mem_size(sb)) return false;
    memcpy(sb_mem_base(sb) + ptr, src, len);
    return true;
}
static const char *sb_mem_cstr(SlangBindings *sb, uint32_t ptr, uint32_t len) {
    if (len == 0) return "";
    if ((size_t)ptr + len > sb_mem_size(sb)) return "";
    return (const char *)(sb_mem_base(sb) + ptr);
}

#define ARG_I32(n) (args->data[(n)].of.i32)
#define RET_I32(v) do { res->data[0] = (wasm_val_t){.kind=WASM_I32,.of={.i32=(v)}}; } while(0)

/* Write a blob handle to WASM memory at the pointer given by the guest.
   If out_ptr == 0, the blob is released immediately. */
static void sb_write_diag_blob(SlangBindings *sb, uint32_t out_ptr, ISlangBlob *blob) {
    if (!blob) {
        if (out_ptr) sb_mem_write(sb, out_ptr, "\0\0\0\0", 4);
        return;
    }
    if (out_ptr == 0) {
        blob->release();
        return;
    }
    uint32_t h = htable_insert(&sb->ht_blob, blob);
    int32_t ih = (int32_t)h;
    sb_mem_write(sb, out_ptr, &ih, 4);
}

/* Reflection handle helpers */
#define INS_REFL(ptr) ((ptr) ? (int32_t)htable_insert(&SB->ht_reflection, (void*)(ptr)) : 0)
#define GET_REFL(h) htable_get(&SB->ht_reflection, (uint32_t)(h))

/* Name helper: if dst_ptr==0, return full length. Else copy min(len,max) and return copied. */
static int32_t sb_copy_name(SlangBindings *sb, const char *name,
                             uint32_t dst_ptr, uint32_t max_len) {
    size_t len = name ? strlen(name) : 0;
    if (dst_ptr == 0) return (int32_t)len;
    uint32_t copy = (uint32_t)len;
    if (copy > max_len) copy = max_len;
    if (copy > 0) sb_mem_write(sb, dst_ptr, name, copy);
    return (int32_t)copy;
}

/* ================================================================== */
/*  Global Session                                                     */
/* ================================================================== */

static wasm_trap_t *fn_slang_create_global_session(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)args;
    IGlobalSession *gs = nullptr;
    SlangResult sr = slang_createGlobalSession(SLANG_API_VERSION, &gs);
    if (SLANG_FAILED(sr) || !gs) {
        fprintf(stderr, "[slang] Failed to create global session (0x%08x)\n", sr);
        RET_I32(0); return nullptr;
    }
    uint32_t h = htable_insert(&SB->ht_global_session, gs);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_gs_destroy(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)res;
    auto *gs = (IGlobalSession *)htable_get(&SB->ht_global_session, (uint32_t)ARG_I32(0));
    if (gs) { gs->release(); htable_remove(&SB->ht_global_session, (uint32_t)ARG_I32(0)); }
    return nullptr;
}

static wasm_trap_t *fn_slang_gs_find_profile(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *gs = (IGlobalSession *)htable_get(&SB->ht_global_session, (uint32_t)ARG_I32(0));
    if (!gs) { RET_I32(0); return nullptr; }
    uint32_t np = (uint32_t)ARG_I32(1), nl = (uint32_t)ARG_I32(2);
    std::string name(sb_mem_cstr(SB, np, nl), nl);
    RET_I32((int32_t)gs->findProfile(name.c_str()));
    return nullptr;
}

static wasm_trap_t *fn_slang_gs_create_session(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *gs = (IGlobalSession *)htable_get(&SB->ht_global_session, (uint32_t)ARG_I32(0));
    if (!gs) { RET_I32(0); return nullptr; }

    int32_t target_format  = ARG_I32(1);
    int32_t profile        = ARG_I32(2);
    int32_t fp_mode        = ARG_I32(3);
    int32_t matrix_layout  = ARG_I32(4);

    TargetDesc target = {};
    target.structureSize = sizeof(TargetDesc);
    target.format        = (SlangCompileTarget)target_format;
    target.profile       = (SlangProfileID)profile;
    target.floatingPointMode = (SlangFloatingPointMode)fp_mode;
    target.flags         = kDefaultTargetFlags;

    SessionDesc desc = {};
    desc.structureSize       = sizeof(SessionDesc);
    desc.targets             = &target;
    desc.targetCount         = 1;
    desc.defaultMatrixLayoutMode = (SlangMatrixLayoutMode)matrix_layout;

    ISession *session = nullptr;
    SlangResult sr = gs->createSession(desc, &session);
    if (SLANG_FAILED(sr) || !session) {
        fprintf(stderr, "[slang] Failed to create session (0x%08x)\n", sr);
        RET_I32(0); return nullptr;
    }
    uint32_t h = htable_insert(&SB->ht_session, session);
    RET_I32((int32_t)h);
    return nullptr;
}

/* ================================================================== */
/*  Session                                                            */
/* ================================================================== */

static wasm_trap_t *fn_slang_session_destroy(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)res;
    auto *s = (ISession *)htable_get(&SB->ht_session, (uint32_t)ARG_I32(0));
    if (s) { s->release(); htable_remove(&SB->ht_session, (uint32_t)ARG_I32(0)); }
    return nullptr;
}

static wasm_trap_t *fn_slang_session_load_module(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *s = (ISession *)htable_get(&SB->ht_session, (uint32_t)ARG_I32(0));
    if (!s) { RET_I32(0); return nullptr; }
    uint32_t np = (uint32_t)ARG_I32(1), nl = (uint32_t)ARG_I32(2);
    uint32_t diag_out = (uint32_t)ARG_I32(3);
    std::string name(sb_mem_cstr(SB, np, nl), nl);

    ISlangBlob *diagBlob = nullptr;
    IModule *mod = s->loadModule(name.c_str(), &diagBlob);
    sb_write_diag_blob(SB, diag_out, diagBlob);
    if (!mod) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_component, mod);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_session_load_module_from_source(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *s = (ISession *)htable_get(&SB->ht_session, (uint32_t)ARG_I32(0));
    if (!s) { RET_I32(0); return nullptr; }
    uint32_t mnp = (uint32_t)ARG_I32(1), mnl = (uint32_t)ARG_I32(2);
    uint32_t pp  = (uint32_t)ARG_I32(3), pl  = (uint32_t)ARG_I32(4);
    uint32_t sp  = (uint32_t)ARG_I32(5), sl  = (uint32_t)ARG_I32(6);
    uint32_t diag_out = (uint32_t)ARG_I32(7);

    std::string modName(sb_mem_cstr(SB, mnp, mnl), mnl);
    std::string path(sb_mem_cstr(SB, pp, pl), pl);
    std::string source(sb_mem_cstr(SB, sp, sl), sl);

    ISlangBlob *diagBlob = nullptr;
    IModule *mod = s->loadModuleFromSourceString(
        modName.c_str(), path.c_str(), source.c_str(), &diagBlob);
    sb_write_diag_blob(SB, diag_out, diagBlob);
    if (!mod) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_component, mod);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_session_create_composite(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *s = (ISession *)htable_get(&SB->ht_session, (uint32_t)ARG_I32(0));
    if (!s) { RET_I32(0); return nullptr; }
    uint32_t handles_ptr = (uint32_t)ARG_I32(1);
    int32_t  count       = ARG_I32(2);
    uint32_t diag_out    = (uint32_t)ARG_I32(3);
    if (count <= 0 || count > 64) { RET_I32(0); return nullptr; }

    size_t bytes = (size_t)count * sizeof(int32_t);
    if ((size_t)handles_ptr + bytes > sb_mem_size(SB)) { RET_I32(0); return nullptr; }

    std::vector<IComponentType *> parts;
    const uint8_t *base = sb_mem_base(SB);
    for (int32_t i = 0; i < count; i++) {
        int32_t ch;
        memcpy(&ch, base + handles_ptr + i * 4, 4);
        auto *ct = (IComponentType *)htable_get(&SB->ht_component, (uint32_t)ch);
        if (!ct) { RET_I32(0); return nullptr; }
        parts.push_back(ct);
    }

    ISlangBlob *diagBlob = nullptr;
    IComponentType *composed = nullptr;
    SlangResult sr = s->createCompositeComponentType(
        parts.data(), (SlangInt)parts.size(), &composed, &diagBlob);
    sb_write_diag_blob(SB, diag_out, diagBlob);
    if (SLANG_FAILED(sr) || !composed) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_component, composed);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_session_get_loaded_module_count(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *s = (ISession *)htable_get(&SB->ht_session, (uint32_t)ARG_I32(0));
    RET_I32(s ? (int32_t)s->getLoadedModuleCount() : 0);
    return nullptr;
}

static wasm_trap_t *fn_slang_session_get_loaded_module(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *s = (ISession *)htable_get(&SB->ht_session, (uint32_t)ARG_I32(0));
    if (!s) { RET_I32(0); return nullptr; }
    IModule *mod = s->getLoadedModule(ARG_I32(1));
    if (!mod) { RET_I32(0); return nullptr; }
    mod->addRef();
    uint32_t h = htable_insert(&SB->ht_component, mod);
    RET_I32((int32_t)h);
    return nullptr;
}

/* ================================================================== */
/*  Component Type                                                     */
/* ================================================================== */

static wasm_trap_t *fn_slang_component_destroy(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    auto *ct = (IComponentType *)htable_get(&SB->ht_component, h);
    if (ct) { ct->release(); htable_remove(&SB->ht_component, h); }
    return nullptr;
}

static wasm_trap_t *fn_slang_component_get_layout(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *ct = (IComponentType *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!ct) { RET_I32(0); return nullptr; }
    int32_t target_index = ARG_I32(1);
    uint32_t diag_out = (uint32_t)ARG_I32(2);

    ISlangBlob *diagBlob = nullptr;
    ProgramLayout *layout = ct->getLayout(target_index, &diagBlob);
    sb_write_diag_blob(SB, diag_out, diagBlob);
    RET_I32(INS_REFL(layout));
    return nullptr;
}

static wasm_trap_t *fn_slang_component_get_entry_point_code(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *ct = (IComponentType *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!ct) { RET_I32(0); return nullptr; }
    int32_t ep_index     = ARG_I32(1);
    int32_t target_index = ARG_I32(2);
    uint32_t diag_out    = (uint32_t)ARG_I32(3);

    ISlangBlob *codeBlob = nullptr, *diagBlob = nullptr;
    SlangResult sr = ct->getEntryPointCode(ep_index, target_index, &codeBlob, &diagBlob);
    sb_write_diag_blob(SB, diag_out, diagBlob);
    if (SLANG_FAILED(sr) || !codeBlob) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_blob, codeBlob);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_component_get_target_code(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *ct = (IComponentType *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!ct) { RET_I32(0); return nullptr; }
    int32_t target_index = ARG_I32(1);
    uint32_t diag_out    = (uint32_t)ARG_I32(2);

    ISlangBlob *codeBlob = nullptr, *diagBlob = nullptr;
    SlangResult sr = ct->getTargetCode(target_index, &codeBlob, &diagBlob);
    sb_write_diag_blob(SB, diag_out, diagBlob);
    if (SLANG_FAILED(sr) || !codeBlob) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_blob, codeBlob);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_component_link(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *ct = (IComponentType *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!ct) { RET_I32(0); return nullptr; }
    uint32_t diag_out = (uint32_t)ARG_I32(1);

    ISlangBlob *diagBlob = nullptr;
    IComponentType *linked = nullptr;
    SlangResult sr = ct->link(&linked, &diagBlob);
    sb_write_diag_blob(SB, diag_out, diagBlob);
    if (SLANG_FAILED(sr) || !linked) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_component, linked);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_component_get_specialization_param_count(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *ct = (IComponentType *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    RET_I32(ct ? (int32_t)ct->getSpecializationParamCount() : 0);
    return nullptr;
}

/* ================================================================== */
/*  Module-specific (IModule methods via IComponentType handle)         */
/* ================================================================== */

static wasm_trap_t *fn_slang_module_find_entry_point(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *mod = (IModule *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!mod) { RET_I32(0); return nullptr; }
    uint32_t np = (uint32_t)ARG_I32(1), nl = (uint32_t)ARG_I32(2);
    std::string name(sb_mem_cstr(SB, np, nl), nl);
    IEntryPoint *ep = nullptr;
    SlangResult sr = mod->findEntryPointByName(name.c_str(), &ep);
    if (SLANG_FAILED(sr) || !ep) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_component, ep);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_module_find_and_check_entry_point(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *mod = (IModule *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!mod) { RET_I32(0); return nullptr; }
    uint32_t np = (uint32_t)ARG_I32(1), nl = (uint32_t)ARG_I32(2);
    int32_t stage = ARG_I32(3);
    uint32_t diag_out = (uint32_t)ARG_I32(4);
    std::string name(sb_mem_cstr(SB, np, nl), nl);

    ISlangBlob *diagBlob = nullptr;
    IEntryPoint *ep = nullptr;
    SlangResult sr = mod->findAndCheckEntryPoint(
        name.c_str(), (SlangStage)stage, &ep, &diagBlob);
    sb_write_diag_blob(SB, diag_out, diagBlob);
    if (SLANG_FAILED(sr) || !ep) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_component, ep);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_module_get_defined_entry_point_count(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *mod = (IModule *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    RET_I32(mod ? (int32_t)mod->getDefinedEntryPointCount() : 0);
    return nullptr;
}

static wasm_trap_t *fn_slang_module_get_defined_entry_point(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *mod = (IModule *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!mod) { RET_I32(0); return nullptr; }
    IEntryPoint *ep = nullptr;
    SlangResult sr = mod->getDefinedEntryPoint(ARG_I32(1), &ep);
    if (SLANG_FAILED(sr) || !ep) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_component, ep);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_module_get_name(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *mod = (IModule *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!mod) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, mod->getName(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2)));
    return nullptr;
}

static wasm_trap_t *fn_slang_module_get_file_path(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *mod = (IModule *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!mod) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, mod->getFilePath(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2)));
    return nullptr;
}

static wasm_trap_t *fn_slang_module_get_module_reflection(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *mod = (IModule *)htable_get(&SB->ht_component, (uint32_t)ARG_I32(0));
    if (!mod) { RET_I32(0); return nullptr; }
    DeclReflection *decl = mod->getModuleReflection();
    RET_I32(INS_REFL(decl));
    return nullptr;
}

/* ================================================================== */
/*  Blob                                                               */
/* ================================================================== */

static wasm_trap_t *fn_slang_blob_get_size(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *b = (ISlangBlob *)htable_get(&SB->ht_blob, (uint32_t)ARG_I32(0));
    RET_I32(b ? (int32_t)b->getBufferSize() : 0);
    return nullptr;
}

static wasm_trap_t *fn_slang_blob_read(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *b = (ISlangBlob *)htable_get(&SB->ht_blob, (uint32_t)ARG_I32(0));
    uint32_t dst = (uint32_t)ARG_I32(1), max_len = (uint32_t)ARG_I32(2);
    if (!b) { RET_I32(0); return nullptr; }
    uint32_t sz = (uint32_t)b->getBufferSize();
    uint32_t copy = sz < max_len ? sz : max_len;
    if (copy > 0) sb_mem_write(SB, dst, b->getBufferPointer(), copy);
    RET_I32((int32_t)copy);
    return nullptr;
}

static wasm_trap_t *fn_slang_blob_destroy(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    auto *b = (ISlangBlob *)htable_get(&SB->ht_blob, h);
    if (b) { b->release(); htable_remove(&SB->ht_blob, h); }
    return nullptr;
}

/* ================================================================== */
/*  Convenience: shader module from blob                               */
/* ================================================================== */

static wasm_trap_t *fn_slang_create_shader_module_from_blob(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *b = (ISlangBlob *)htable_get(&SB->ht_blob, (uint32_t)ARG_I32(0));
    if (!b) { RET_I32(0); return nullptr; }
    uint32_t lp = (uint32_t)ARG_I32(1), ll = (uint32_t)ARG_I32(2);

    const char *code = (const char *)b->getBufferPointer();
    size_t codeSize = b->getBufferSize();

    WGPUShaderSourceWGSL wgslSource = {};
    wgslSource.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgslSource.code.data   = code;
    wgslSource.code.length = codeSize;

    WGPUShaderModuleDescriptor desc = {};
    desc.nextInChain = &wgslSource.chain;
    std::string label;
    if (lp && ll) {
        label.assign(sb_mem_cstr(SB, lp, ll), ll);
        desc.label.data = label.c_str();
        desc.label.length = label.size();
    }

    WGPUShaderModule sm = wgpuDeviceCreateShaderModule(SB->wgpu->gpu->device, &desc);
    if (!sm) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->wgpu->ht_shader, sm);
    RET_I32((int32_t)h);
    return nullptr;
}

/* ================================================================== */
/*  Reflection — ProgramLayout                                         */
/* ================================================================== */

static wasm_trap_t *fn_slang_refl_get_parameter_count(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    RET_I32(r ? (int32_t)r->getParameterCount() : 0);
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_get_parameter_by_index(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    if (!r) { RET_I32(0); return nullptr; }
    auto *vl = r->getParameterByIndex((unsigned)ARG_I32(1));
    RET_I32(INS_REFL(vl));
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_get_entry_point_count(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    RET_I32(r ? (int32_t)r->getEntryPointCount() : 0);
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_get_entry_point_by_index(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    if (!r) { RET_I32(0); return nullptr; }
    auto *ep = r->getEntryPointByIndex((SlangUInt)ARG_I32(1));
    RET_I32(INS_REFL(ep));
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_find_type_by_name(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    if (!r) { RET_I32(0); return nullptr; }
    uint32_t np = (uint32_t)ARG_I32(1), nl = (uint32_t)ARG_I32(2);
    std::string name(sb_mem_cstr(SB, np, nl), nl);
    auto *t = r->findTypeByName(name.c_str());
    RET_I32(INS_REFL(t));
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_get_type_layout(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(1));
    if (!r || !t) { RET_I32(0); return nullptr; }
    auto rules = (LayoutRules)ARG_I32(2);
    auto *tl = r->getTypeLayout(t, rules);
    RET_I32(INS_REFL(tl));
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_find_entry_point_by_name(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    if (!r) { RET_I32(0); return nullptr; }
    uint32_t np = (uint32_t)ARG_I32(1), nl = (uint32_t)ARG_I32(2);
    std::string name(sb_mem_cstr(SB, np, nl), nl);
    auto *ep = r->findEntryPointByName(name.c_str());
    RET_I32(INS_REFL(ep));
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_get_global_constant_buffer_binding(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    RET_I32(r ? (int32_t)r->getGlobalConstantBufferBinding() : 0);
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_get_global_constant_buffer_size(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    RET_I32(r ? (int32_t)r->getGlobalConstantBufferSize() : 0);
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_get_global_params_type_layout(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    if (!r) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(r->getGlobalParamsTypeLayout()));
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_get_global_params_var_layout(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    if (!r) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(r->getGlobalParamsVarLayout()));
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_find_function_by_name(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    if (!r) { RET_I32(0); return nullptr; }
    uint32_t np = (uint32_t)ARG_I32(1), nl = (uint32_t)ARG_I32(2);
    std::string name(sb_mem_cstr(SB, np, nl), nl);
    auto *fn = r->findFunctionByName(name.c_str());
    RET_I32(INS_REFL(fn));
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_to_json(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    if (!r) { RET_I32(0); return nullptr; }
    ISlangBlob *blob = nullptr;
    SlangResult sr = r->toJson(&blob);
    if (SLANG_FAILED(sr) || !blob) { RET_I32(0); return nullptr; }
    uint32_t h = htable_insert(&SB->ht_blob, blob);
    RET_I32((int32_t)h);
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_get_hashed_string_count(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    RET_I32(r ? (int32_t)r->getHashedStringCount() : 0);
    return nullptr;
}

static wasm_trap_t *fn_slang_refl_is_sub_type(
    void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    auto *r = (ProgramLayout *)GET_REFL(ARG_I32(0));
    auto *sub = (TypeReflection *)GET_REFL(ARG_I32(1));
    auto *sup = (TypeReflection *)GET_REFL(ARG_I32(2));
    RET_I32((r && sub && sup) ? (r->isSubType(sub, sup) ? 1 : 0) : 0);
    return nullptr;
}

/* ================================================================== */
/*  Reflection — TypeReflection                                        */
/* ================================================================== */

static wasm_trap_t *fn_slang_type_get_kind(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    RET_I32(t ? (int32_t)t->getKind() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_field_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    RET_I32(t ? (int32_t)t->getFieldCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_field_by_index(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    if (!t) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(t->getFieldByIndex((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_element_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    RET_I32(t ? (int32_t)t->getElementCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_element_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    if (!t) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(t->getElementType())); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_row_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    RET_I32(t ? (int32_t)t->getRowCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_column_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    RET_I32(t ? (int32_t)t->getColumnCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_scalar_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    RET_I32(t ? (int32_t)t->getScalarType() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_resource_result_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    if (!t) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(t->getResourceResultType())); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_resource_shape(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    RET_I32(t ? (int32_t)t->getResourceShape() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_resource_access(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    RET_I32(t ? (int32_t)t->getResourceAccess() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_name(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    if (!t) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, t->getName(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2))); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_user_attribute_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    RET_I32(t ? (int32_t)t->getUserAttributeCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_user_attribute(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    if (!t) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(t->getUserAttributeByIndex((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_type_get_generic_container(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *t = (TypeReflection *)GET_REFL(ARG_I32(0));
    if (!t) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(t->getGenericContainer())); return nullptr;
}

/* ================================================================== */
/*  Reflection — TypeLayoutReflection                                  */
/* ================================================================== */

static wasm_trap_t *fn_slang_type_layout_get_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!tl) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(tl->getType())); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_kind(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getKind() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_size(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getSize((SlangParameterCategory)ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_stride(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getStride((SlangParameterCategory)ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_alignment(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getAlignment((SlangParameterCategory)ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_field_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getFieldCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_field_by_index(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!tl) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(tl->getFieldByIndex((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_find_field_index_by_name(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!tl) { RET_I32(-1); return nullptr; }
    uint32_t np = (uint32_t)ARG_I32(1), nl = (uint32_t)ARG_I32(2);
    std::string name(sb_mem_cstr(SB, np, nl), nl);
    RET_I32((int32_t)tl->findFieldIndexByName(name.c_str(), name.c_str() + name.size())); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_element_stride(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getElementStride((SlangParameterCategory)ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_element_type_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!tl) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(tl->getElementTypeLayout())); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_element_var_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!tl) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(tl->getElementVarLayout())); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_parameter_category(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getParameterCategory() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_category_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getCategoryCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_category_by_index(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getCategoryByIndex((unsigned)ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_binding_range_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getBindingRangeCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_binding_range_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getBindingRangeType(ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_binding_range_binding_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getBindingRangeBindingCount(ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_binding_range_index_offset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    /* Not available in this Slang version — commented out in slang.h */
    (void)env; (void)args;
    RET_I32(0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_binding_range_space_offset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    /* Not available in this Slang version — commented out in slang.h */
    (void)env; (void)args;
    RET_I32(0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_binding_range_leaf_type_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!tl) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(tl->getBindingRangeLeafTypeLayout(ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_binding_range_leaf_variable(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!tl) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(tl->getBindingRangeLeafVariable(ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_field_binding_range_offset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getFieldBindingRangeOffset(ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_descriptor_set_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getDescriptorSetCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_descriptor_set_space_offset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getDescriptorSetSpaceOffset(ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_desc_set_range_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getDescriptorSetDescriptorRangeCount(ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_desc_set_range_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getDescriptorSetDescriptorRangeType(ARG_I32(1), ARG_I32(2)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_desc_set_range_desc_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getDescriptorSetDescriptorRangeDescriptorCount(ARG_I32(1), ARG_I32(2)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_desc_set_range_category(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getDescriptorSetDescriptorRangeCategory(ARG_I32(1), ARG_I32(2)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_sub_object_range_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getSubObjectRangeCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_sub_object_range_binding_range_index(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getSubObjectRangeBindingRangeIndex(ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_matrix_layout_mode(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(tl ? (int32_t)tl->getMatrixLayoutMode() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_type_layout_get_container_var_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *tl = (TypeLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!tl) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(tl->getContainerVarLayout())); return nullptr;
}

/* ================================================================== */
/*  Reflection — VariableReflection                                    */
/* ================================================================== */

static wasm_trap_t *fn_slang_var_get_name(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *v = (VariableReflection *)GET_REFL(ARG_I32(0));
    if (!v) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, v->getName(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2))); return nullptr;
}
static wasm_trap_t *fn_slang_var_get_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *v = (VariableReflection *)GET_REFL(ARG_I32(0));
    if (!v) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(v->getType())); return nullptr;
}
static wasm_trap_t *fn_slang_var_has_default_value(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *v = (VariableReflection *)GET_REFL(ARG_I32(0));
    RET_I32(v ? (v->hasDefaultValue() ? 1 : 0) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_var_get_user_attribute_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *v = (VariableReflection *)GET_REFL(ARG_I32(0));
    RET_I32(v ? (int32_t)v->getUserAttributeCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_var_get_user_attribute(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *v = (VariableReflection *)GET_REFL(ARG_I32(0));
    if (!v) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(v->getUserAttributeByIndex((unsigned)ARG_I32(1)))); return nullptr;
}

/* ================================================================== */
/*  Reflection — VariableLayoutReflection                              */
/* ================================================================== */

static wasm_trap_t *fn_slang_var_layout_get_variable(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *vl = (VariableLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!vl) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(vl->getVariable())); return nullptr;
}
static wasm_trap_t *fn_slang_var_layout_get_type_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *vl = (VariableLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!vl) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(vl->getTypeLayout())); return nullptr;
}
static wasm_trap_t *fn_slang_var_layout_get_offset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *vl = (VariableLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(vl ? (int32_t)vl->getOffset((SlangParameterCategory)ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_var_layout_get_binding_index(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *vl = (VariableLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(vl ? (int32_t)vl->getBindingIndex() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_var_layout_get_binding_space(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *vl = (VariableLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(vl ? (int32_t)vl->getBindingSpace() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_var_layout_get_space(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *vl = (VariableLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(vl ? (int32_t)vl->getBindingSpace((SlangParameterCategory)ARG_I32(1)) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_var_layout_get_semantic_name(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *vl = (VariableLayoutReflection *)GET_REFL(ARG_I32(0));
    if (!vl) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, vl->getSemanticName(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2))); return nullptr;
}
static wasm_trap_t *fn_slang_var_layout_get_semantic_index(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *vl = (VariableLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(vl ? (int32_t)vl->getSemanticIndex() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_var_layout_get_stage(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *vl = (VariableLayoutReflection *)GET_REFL(ARG_I32(0));
    RET_I32(vl ? (int32_t)vl->getStage() : 0); return nullptr;
}

/* ================================================================== */
/*  Reflection — EntryPointReflection                                  */
/* ================================================================== */

static wasm_trap_t *fn_slang_ep_layout_get_name(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    if (!ep) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, ep->getName(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2))); return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_get_name_override(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    if (!ep) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, ep->getNameOverride(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2))); return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_get_stage(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    RET_I32(ep ? (int32_t)ep->getStage() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_get_parameter_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    RET_I32(ep ? (int32_t)ep->getParameterCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_get_parameter_by_index(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    if (!ep) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(ep->getParameterByIndex((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_get_compute_thread_group_size(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    if (!ep || !out_ptr) return nullptr;
    SlangUInt sz[3] = {};
    ep->getComputeThreadGroupSize(3, sz);
    int32_t isz[3] = { (int32_t)sz[0], (int32_t)sz[1], (int32_t)sz[2] };
    sb_mem_write(SB, out_ptr, isz, sizeof(isz));
    return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_get_compute_wave_size(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    uint32_t out_ptr = (uint32_t)ARG_I32(1);
    if (!ep || !out_ptr) return nullptr;
    SlangUInt sz[3] = {};
    ep->getComputeWaveSize(sz);
    int32_t isz[3] = { (int32_t)sz[0], (int32_t)sz[1], (int32_t)sz[2] };
    sb_mem_write(SB, out_ptr, isz, sizeof(isz));
    return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_uses_any_sample_rate_input(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    RET_I32(ep ? (ep->usesAnySampleRateInput() ? 1 : 0) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_has_default_constant_buffer(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    RET_I32(ep ? (ep->hasDefaultConstantBuffer() ? 1 : 0) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_get_result_var_layout(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    if (!ep) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(ep->getResultVarLayout())); return nullptr;
}
static wasm_trap_t *fn_slang_ep_layout_get_function(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *ep = (EntryPointReflection *)GET_REFL(ARG_I32(0));
    if (!ep) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(ep->getFunction())); return nullptr;
}

/* ================================================================== */
/*  Reflection — FunctionReflection                                    */
/* ================================================================== */

static wasm_trap_t *fn_slang_func_get_name(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    if (!f) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, f->getName(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2))); return nullptr;
}
static wasm_trap_t *fn_slang_func_get_result_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    if (!f) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(f->getReturnType())); return nullptr;
}
static wasm_trap_t *fn_slang_func_get_parameter_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    RET_I32(f ? (int32_t)f->getParameterCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_func_get_parameter(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    if (!f) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(f->getParameterByIndex((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_func_get_user_attribute_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    RET_I32(f ? (int32_t)f->getUserAttributeCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_func_get_user_attribute(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    if (!f) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(f->getUserAttributeByIndex((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_func_is_overloaded(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    RET_I32(f ? (f->isOverloaded() ? 1 : 0) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_func_get_overload_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    RET_I32(f ? (int32_t)f->getOverloadCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_func_get_overload(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    if (!f) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(f->getOverload((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_func_get_generic_container(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *f = (FunctionReflection *)GET_REFL(ARG_I32(0));
    if (!f) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(f->getGenericContainer())); return nullptr;
}

/* ================================================================== */
/*  Reflection — GenericReflection                                     */
/* ================================================================== */

static wasm_trap_t *fn_slang_generic_as_decl(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    if (!g) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(g->asDecl())); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_name(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    if (!g) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, g->getName(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2))); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_type_parameter_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    RET_I32(g ? (int32_t)g->getTypeParameterCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_type_parameter(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    if (!g) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(g->getTypeParameter((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_value_parameter_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    RET_I32(g ? (int32_t)g->getValueParameterCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_value_parameter(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    if (!g) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(g->getValueParameter((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_type_param_constraint_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    if (!g) { RET_I32(0); return nullptr; }
    auto *tp = (VariableReflection *)GET_REFL(ARG_I32(1));
    RET_I32(tp ? (int32_t)g->getTypeParameterConstraintCount(tp) : 0); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_type_param_constraint_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    if (!g) { RET_I32(0); return nullptr; }
    auto *tp = (VariableReflection *)GET_REFL(ARG_I32(1));
    if (!tp) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(g->getTypeParameterConstraintType(tp, (unsigned)ARG_I32(2)))); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_inner_decl(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    if (!g) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(g->getInnerDecl())); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_inner_kind(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    RET_I32(g ? (int32_t)g->getInnerKind() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_generic_get_outer_generic_container(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *g = (GenericReflection *)GET_REFL(ARG_I32(0));
    if (!g) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(g->getOuterGenericContainer())); return nullptr;
}

/* ================================================================== */
/*  Reflection — DeclReflection                                        */
/* ================================================================== */

static wasm_trap_t *fn_slang_decl_get_name(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *d = (DeclReflection *)GET_REFL(ARG_I32(0));
    if (!d) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, d->getName(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2))); return nullptr;
}
static wasm_trap_t *fn_slang_decl_get_kind(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *d = (DeclReflection *)GET_REFL(ARG_I32(0));
    RET_I32(d ? (int32_t)d->getKind() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_decl_get_children_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *d = (DeclReflection *)GET_REFL(ARG_I32(0));
    RET_I32(d ? (int32_t)d->getChildrenCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_decl_get_child(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *d = (DeclReflection *)GET_REFL(ARG_I32(0));
    if (!d) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(d->getChild((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_decl_get_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *d = (DeclReflection *)GET_REFL(ARG_I32(0));
    if (!d) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(d->getType())); return nullptr;
}
static wasm_trap_t *fn_slang_decl_cast_to_variable(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *d = (DeclReflection *)GET_REFL(ARG_I32(0));
    if (!d) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(d->asVariable())); return nullptr;
}
static wasm_trap_t *fn_slang_decl_cast_to_function(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *d = (DeclReflection *)GET_REFL(ARG_I32(0));
    if (!d) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(d->asFunction())); return nullptr;
}
static wasm_trap_t *fn_slang_decl_cast_to_generic(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *d = (DeclReflection *)GET_REFL(ARG_I32(0));
    if (!d) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(d->asGeneric())); return nullptr;
}
static wasm_trap_t *fn_slang_decl_get_parent(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *d = (DeclReflection *)GET_REFL(ARG_I32(0));
    if (!d) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(d->getParent())); return nullptr;
}

/* ================================================================== */
/*  Reflection — UserAttribute                                         */
/* ================================================================== */

static wasm_trap_t *fn_slang_attr_get_name(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *a = (UserAttribute *)GET_REFL(ARG_I32(0));
    if (!a) { RET_I32(0); return nullptr; }
    RET_I32(sb_copy_name(SB, a->getName(), (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2))); return nullptr;
}
static wasm_trap_t *fn_slang_attr_get_argument_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *a = (UserAttribute *)GET_REFL(ARG_I32(0));
    RET_I32(a ? (int32_t)a->getArgumentCount() : 0); return nullptr;
}
static wasm_trap_t *fn_slang_attr_get_argument_type(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *a = (UserAttribute *)GET_REFL(ARG_I32(0));
    if (!a) { RET_I32(0); return nullptr; }
    RET_I32(INS_REFL(a->getArgumentType((unsigned)ARG_I32(1)))); return nullptr;
}
static wasm_trap_t *fn_slang_attr_get_argument_value_int(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *a = (UserAttribute *)GET_REFL(ARG_I32(0));
    if (!a) { RET_I32(-1); return nullptr; }
    uint32_t out_ptr = (uint32_t)ARG_I32(2);
    int val = 0;
    SlangResult sr = a->getArgumentValueInt((unsigned)ARG_I32(1), &val);
    if (SLANG_SUCCEEDED(sr) && out_ptr)
        sb_mem_write(SB, out_ptr, &val, sizeof(val));
    RET_I32((int32_t)sr);
    return nullptr;
}
static wasm_trap_t *fn_slang_attr_get_argument_value_float(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *a = (UserAttribute *)GET_REFL(ARG_I32(0));
    if (!a) { RET_I32(-1); return nullptr; }
    uint32_t out_ptr = (uint32_t)ARG_I32(2);
    float val = 0;
    SlangResult sr = a->getArgumentValueFloat((unsigned)ARG_I32(1), &val);
    if (SLANG_SUCCEEDED(sr) && out_ptr)
        sb_mem_write(SB, out_ptr, &val, sizeof(val));
    RET_I32((int32_t)sr);
    return nullptr;
}
static wasm_trap_t *fn_slang_attr_get_argument_value_string(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    auto *a = (UserAttribute *)GET_REFL(ARG_I32(0));
    if (!a) { RET_I32(0); return nullptr; }
    size_t outLen = 0;
    const char *str = a->getArgumentValueString((unsigned)ARG_I32(1), &outLen);
    uint32_t dst = (uint32_t)ARG_I32(2), max_len = (uint32_t)ARG_I32(3);
    if (dst == 0) { RET_I32((int32_t)outLen); return nullptr; }
    uint32_t copy = (uint32_t)outLen;
    if (copy > max_len) copy = max_len;
    if (copy > 0 && str) sb_mem_write(SB, dst, str, copy);
    RET_I32((int32_t)copy);
    return nullptr;
}

/* ================================================================== */
/*  Binding table                                                      */
/* ================================================================== */

typedef struct {
    const char *name;
    wasm_func_callback_with_env_t cb;
    uint32_t np; wasm_valkind_t params[10];
    uint32_t nr; wasm_valkind_t results[1];
} SBindingEntry;

#define I WASM_I32

static const SBindingEntry SLANG_BINDING_TABLE[] = {
    /* ---- Global Session ---- */
    {"slang_create_global_session",     fn_slang_create_global_session,    0, {0},               1, {I}},
    {"slang_gs_destroy",                fn_slang_gs_destroy,               1, {I},               0, {0}},
    {"slang_gs_find_profile",           fn_slang_gs_find_profile,          3, {I,I,I},           1, {I}},
    {"slang_gs_create_session",         fn_slang_gs_create_session,        5, {I,I,I,I,I},       1, {I}},

    /* ---- Session ---- */
    {"slang_session_destroy",           fn_slang_session_destroy,          1, {I},               0, {0}},
    {"slang_session_load_module",       fn_slang_session_load_module,      4, {I,I,I,I},         1, {I}},
    {"slang_session_load_module_from_source", fn_slang_session_load_module_from_source, 8, {I,I,I,I,I,I,I,I}, 1, {I}},
    {"slang_session_create_composite",  fn_slang_session_create_composite, 4, {I,I,I,I},         1, {I}},
    {"slang_session_get_loaded_module_count", fn_slang_session_get_loaded_module_count, 1, {I},   1, {I}},
    {"slang_session_get_loaded_module", fn_slang_session_get_loaded_module, 2, {I,I},            1, {I}},

    /* ---- Component Type ---- */
    {"slang_component_destroy",         fn_slang_component_destroy,        1, {I},               0, {0}},
    {"slang_component_get_layout",      fn_slang_component_get_layout,     3, {I,I,I},           1, {I}},
    {"slang_component_get_entry_point_code", fn_slang_component_get_entry_point_code, 4, {I,I,I,I}, 1, {I}},
    {"slang_component_get_target_code", fn_slang_component_get_target_code, 3, {I,I,I},          1, {I}},
    {"slang_component_link",            fn_slang_component_link,           2, {I,I},             1, {I}},
    {"slang_component_get_specialization_param_count", fn_slang_component_get_specialization_param_count, 1, {I}, 1, {I}},

    /* ---- Module ---- */
    {"slang_module_find_entry_point",   fn_slang_module_find_entry_point,  3, {I,I,I},           1, {I}},
    {"slang_module_find_and_check_entry_point", fn_slang_module_find_and_check_entry_point, 5, {I,I,I,I,I}, 1, {I}},
    {"slang_module_get_defined_entry_point_count", fn_slang_module_get_defined_entry_point_count, 1, {I}, 1, {I}},
    {"slang_module_get_defined_entry_point", fn_slang_module_get_defined_entry_point, 2, {I,I},  1, {I}},
    {"slang_module_get_name",           fn_slang_module_get_name,          3, {I,I,I},           1, {I}},
    {"slang_module_get_file_path",      fn_slang_module_get_file_path,     3, {I,I,I},           1, {I}},
    {"slang_module_get_module_reflection", fn_slang_module_get_module_reflection, 1, {I},        1, {I}},

    /* ---- Blob ---- */
    {"slang_blob_get_size",             fn_slang_blob_get_size,            1, {I},               1, {I}},
    {"slang_blob_read",                 fn_slang_blob_read,                3, {I,I,I},           1, {I}},
    {"slang_blob_destroy",              fn_slang_blob_destroy,             1, {I},               0, {0}},

    /* ---- Convenience ---- */
    {"slang_create_shader_module_from_blob", fn_slang_create_shader_module_from_blob, 3, {I,I,I}, 1, {I}},

    /* ---- Reflection: ProgramLayout ---- */
    {"slang_refl_get_parameter_count",  fn_slang_refl_get_parameter_count, 1, {I},               1, {I}},
    {"slang_refl_get_parameter_by_index", fn_slang_refl_get_parameter_by_index, 2, {I,I},        1, {I}},
    {"slang_refl_get_entry_point_count", fn_slang_refl_get_entry_point_count, 1, {I},            1, {I}},
    {"slang_refl_get_entry_point_by_index", fn_slang_refl_get_entry_point_by_index, 2, {I,I},    1, {I}},
    {"slang_refl_find_type_by_name",    fn_slang_refl_find_type_by_name,   3, {I,I,I},           1, {I}},
    {"slang_refl_get_type_layout",      fn_slang_refl_get_type_layout,     3, {I,I,I},           1, {I}},
    {"slang_refl_find_entry_point_by_name", fn_slang_refl_find_entry_point_by_name, 3, {I,I,I},  1, {I}},
    {"slang_refl_get_global_constant_buffer_binding", fn_slang_refl_get_global_constant_buffer_binding, 1, {I}, 1, {I}},
    {"slang_refl_get_global_constant_buffer_size", fn_slang_refl_get_global_constant_buffer_size, 1, {I}, 1, {I}},
    {"slang_refl_get_global_params_type_layout", fn_slang_refl_get_global_params_type_layout, 1, {I}, 1, {I}},
    {"slang_refl_get_global_params_var_layout", fn_slang_refl_get_global_params_var_layout, 1, {I}, 1, {I}},
    {"slang_refl_find_function_by_name", fn_slang_refl_find_function_by_name, 3, {I,I,I},        1, {I}},
    {"slang_refl_to_json",              fn_slang_refl_to_json,             1, {I},               1, {I}},
    {"slang_refl_get_hashed_string_count", fn_slang_refl_get_hashed_string_count, 1, {I},        1, {I}},
    {"slang_refl_is_sub_type",          fn_slang_refl_is_sub_type,         3, {I,I,I},           1, {I}},

    /* ---- Reflection: TypeReflection ---- */
    {"slang_type_get_kind",             fn_slang_type_get_kind,            1, {I},               1, {I}},
    {"slang_type_get_field_count",      fn_slang_type_get_field_count,     1, {I},               1, {I}},
    {"slang_type_get_field_by_index",   fn_slang_type_get_field_by_index,  2, {I,I},             1, {I}},
    {"slang_type_get_element_count",    fn_slang_type_get_element_count,   1, {I},               1, {I}},
    {"slang_type_get_element_type",     fn_slang_type_get_element_type,    1, {I},               1, {I}},
    {"slang_type_get_row_count",        fn_slang_type_get_row_count,       1, {I},               1, {I}},
    {"slang_type_get_column_count",     fn_slang_type_get_column_count,    1, {I},               1, {I}},
    {"slang_type_get_scalar_type",      fn_slang_type_get_scalar_type,     1, {I},               1, {I}},
    {"slang_type_get_resource_result_type", fn_slang_type_get_resource_result_type, 1, {I},      1, {I}},
    {"slang_type_get_resource_shape",   fn_slang_type_get_resource_shape,  1, {I},               1, {I}},
    {"slang_type_get_resource_access",  fn_slang_type_get_resource_access, 1, {I},               1, {I}},
    {"slang_type_get_name",             fn_slang_type_get_name,            3, {I,I,I},           1, {I}},
    {"slang_type_get_user_attribute_count", fn_slang_type_get_user_attribute_count, 1, {I},      1, {I}},
    {"slang_type_get_user_attribute",   fn_slang_type_get_user_attribute,  2, {I,I},             1, {I}},
    {"slang_type_get_generic_container", fn_slang_type_get_generic_container, 1, {I},            1, {I}},

    /* ---- Reflection: TypeLayoutReflection ---- */
    {"slang_type_layout_get_type",      fn_slang_type_layout_get_type,     1, {I},               1, {I}},
    {"slang_type_layout_get_kind",      fn_slang_type_layout_get_kind,     1, {I},               1, {I}},
    {"slang_type_layout_get_size",      fn_slang_type_layout_get_size,     2, {I,I},             1, {I}},
    {"slang_type_layout_get_stride",    fn_slang_type_layout_get_stride,   2, {I,I},             1, {I}},
    {"slang_type_layout_get_alignment", fn_slang_type_layout_get_alignment, 2, {I,I},            1, {I}},
    {"slang_type_layout_get_field_count", fn_slang_type_layout_get_field_count, 1, {I},          1, {I}},
    {"slang_type_layout_get_field_by_index", fn_slang_type_layout_get_field_by_index, 2, {I,I},  1, {I}},
    {"slang_type_layout_find_field_index_by_name", fn_slang_type_layout_find_field_index_by_name, 3, {I,I,I}, 1, {I}},
    {"slang_type_layout_get_element_stride", fn_slang_type_layout_get_element_stride, 2, {I,I},  1, {I}},
    {"slang_type_layout_get_element_type_layout", fn_slang_type_layout_get_element_type_layout, 1, {I}, 1, {I}},
    {"slang_type_layout_get_element_var_layout", fn_slang_type_layout_get_element_var_layout, 1, {I}, 1, {I}},
    {"slang_type_layout_get_parameter_category", fn_slang_type_layout_get_parameter_category, 1, {I}, 1, {I}},
    {"slang_type_layout_get_category_count", fn_slang_type_layout_get_category_count, 1, {I},    1, {I}},
    {"slang_type_layout_get_category_by_index", fn_slang_type_layout_get_category_by_index, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_binding_range_count", fn_slang_type_layout_get_binding_range_count, 1, {I}, 1, {I}},
    {"slang_type_layout_get_binding_range_type", fn_slang_type_layout_get_binding_range_type, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_binding_range_binding_count", fn_slang_type_layout_get_binding_range_binding_count, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_binding_range_index_offset", fn_slang_type_layout_get_binding_range_index_offset, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_binding_range_space_offset", fn_slang_type_layout_get_binding_range_space_offset, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_binding_range_leaf_type_layout", fn_slang_type_layout_get_binding_range_leaf_type_layout, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_binding_range_leaf_variable", fn_slang_type_layout_get_binding_range_leaf_variable, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_field_binding_range_offset", fn_slang_type_layout_get_field_binding_range_offset, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_descriptor_set_count", fn_slang_type_layout_get_descriptor_set_count, 1, {I}, 1, {I}},
    {"slang_type_layout_get_descriptor_set_space_offset", fn_slang_type_layout_get_descriptor_set_space_offset, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_desc_set_range_count", fn_slang_type_layout_get_desc_set_range_count, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_desc_set_range_type", fn_slang_type_layout_get_desc_set_range_type, 3, {I,I,I}, 1, {I}},
    {"slang_type_layout_get_desc_set_range_desc_count", fn_slang_type_layout_get_desc_set_range_desc_count, 3, {I,I,I}, 1, {I}},
    {"slang_type_layout_get_desc_set_range_category", fn_slang_type_layout_get_desc_set_range_category, 3, {I,I,I}, 1, {I}},
    {"slang_type_layout_get_sub_object_range_count", fn_slang_type_layout_get_sub_object_range_count, 1, {I}, 1, {I}},
    {"slang_type_layout_get_sub_object_range_binding_range_index", fn_slang_type_layout_get_sub_object_range_binding_range_index, 2, {I,I}, 1, {I}},
    {"slang_type_layout_get_matrix_layout_mode", fn_slang_type_layout_get_matrix_layout_mode, 1, {I}, 1, {I}},
    {"slang_type_layout_get_container_var_layout", fn_slang_type_layout_get_container_var_layout, 1, {I}, 1, {I}},

    /* ---- Reflection: VariableReflection ---- */
    {"slang_var_get_name",              fn_slang_var_get_name,             3, {I,I,I},           1, {I}},
    {"slang_var_get_type",              fn_slang_var_get_type,             1, {I},               1, {I}},
    {"slang_var_has_default_value",     fn_slang_var_has_default_value,    1, {I},               1, {I}},
    {"slang_var_get_user_attribute_count", fn_slang_var_get_user_attribute_count, 1, {I},        1, {I}},
    {"slang_var_get_user_attribute",    fn_slang_var_get_user_attribute,   2, {I,I},             1, {I}},

    /* ---- Reflection: VariableLayoutReflection ---- */
    {"slang_var_layout_get_variable",   fn_slang_var_layout_get_variable,  1, {I},               1, {I}},
    {"slang_var_layout_get_type_layout", fn_slang_var_layout_get_type_layout, 1, {I},            1, {I}},
    {"slang_var_layout_get_offset",     fn_slang_var_layout_get_offset,    2, {I,I},             1, {I}},
    {"slang_var_layout_get_binding_index", fn_slang_var_layout_get_binding_index, 1, {I},        1, {I}},
    {"slang_var_layout_get_binding_space", fn_slang_var_layout_get_binding_space, 1, {I},        1, {I}},
    {"slang_var_layout_get_space",      fn_slang_var_layout_get_space,     2, {I,I},             1, {I}},
    {"slang_var_layout_get_semantic_name", fn_slang_var_layout_get_semantic_name, 3, {I,I,I},    1, {I}},
    {"slang_var_layout_get_semantic_index", fn_slang_var_layout_get_semantic_index, 1, {I},      1, {I}},
    {"slang_var_layout_get_stage",      fn_slang_var_layout_get_stage,     1, {I},               1, {I}},

    /* ---- Reflection: EntryPointReflection ---- */
    {"slang_ep_layout_get_name",        fn_slang_ep_layout_get_name,       3, {I,I,I},           1, {I}},
    {"slang_ep_layout_get_name_override", fn_slang_ep_layout_get_name_override, 3, {I,I,I},     1, {I}},
    {"slang_ep_layout_get_stage",       fn_slang_ep_layout_get_stage,      1, {I},               1, {I}},
    {"slang_ep_layout_get_parameter_count", fn_slang_ep_layout_get_parameter_count, 1, {I},      1, {I}},
    {"slang_ep_layout_get_parameter_by_index", fn_slang_ep_layout_get_parameter_by_index, 2, {I,I}, 1, {I}},
    {"slang_ep_layout_get_compute_thread_group_size", fn_slang_ep_layout_get_compute_thread_group_size, 2, {I,I}, 0, {0}},
    {"slang_ep_layout_get_compute_wave_size", fn_slang_ep_layout_get_compute_wave_size, 2, {I,I}, 0, {0}},
    {"slang_ep_layout_uses_any_sample_rate_input", fn_slang_ep_layout_uses_any_sample_rate_input, 1, {I}, 1, {I}},
    {"slang_ep_layout_has_default_constant_buffer", fn_slang_ep_layout_has_default_constant_buffer, 1, {I}, 1, {I}},
    {"slang_ep_layout_get_result_var_layout", fn_slang_ep_layout_get_result_var_layout, 1, {I},  1, {I}},
    {"slang_ep_layout_get_function",    fn_slang_ep_layout_get_function,   1, {I},               1, {I}},

    /* ---- Reflection: FunctionReflection ---- */
    {"slang_func_get_name",             fn_slang_func_get_name,            3, {I,I,I},           1, {I}},
    {"slang_func_get_result_type",      fn_slang_func_get_result_type,     1, {I},               1, {I}},
    {"slang_func_get_parameter_count",  fn_slang_func_get_parameter_count, 1, {I},               1, {I}},
    {"slang_func_get_parameter",        fn_slang_func_get_parameter,       2, {I,I},             1, {I}},
    {"slang_func_get_user_attribute_count", fn_slang_func_get_user_attribute_count, 1, {I},      1, {I}},
    {"slang_func_get_user_attribute",   fn_slang_func_get_user_attribute,  2, {I,I},             1, {I}},
    {"slang_func_is_overloaded",        fn_slang_func_is_overloaded,       1, {I},               1, {I}},
    {"slang_func_get_overload_count",   fn_slang_func_get_overload_count,  1, {I},               1, {I}},
    {"slang_func_get_overload",         fn_slang_func_get_overload,        2, {I,I},             1, {I}},
    {"slang_func_get_generic_container", fn_slang_func_get_generic_container, 1, {I},            1, {I}},

    /* ---- Reflection: GenericReflection ---- */
    {"slang_generic_as_decl",           fn_slang_generic_as_decl,          1, {I},               1, {I}},
    {"slang_generic_get_name",          fn_slang_generic_get_name,         3, {I,I,I},           1, {I}},
    {"slang_generic_get_type_parameter_count", fn_slang_generic_get_type_parameter_count, 1, {I}, 1, {I}},
    {"slang_generic_get_type_parameter", fn_slang_generic_get_type_parameter, 2, {I,I},          1, {I}},
    {"slang_generic_get_value_parameter_count", fn_slang_generic_get_value_parameter_count, 1, {I}, 1, {I}},
    {"slang_generic_get_value_parameter", fn_slang_generic_get_value_parameter, 2, {I,I},        1, {I}},
    {"slang_generic_get_type_param_constraint_count", fn_slang_generic_get_type_param_constraint_count, 2, {I,I}, 1, {I}},
    {"slang_generic_get_type_param_constraint_type", fn_slang_generic_get_type_param_constraint_type, 3, {I,I,I}, 1, {I}},
    {"slang_generic_get_inner_decl",    fn_slang_generic_get_inner_decl,   1, {I},               1, {I}},
    {"slang_generic_get_inner_kind",    fn_slang_generic_get_inner_kind,   1, {I},               1, {I}},
    {"slang_generic_get_outer_generic_container", fn_slang_generic_get_outer_generic_container, 1, {I}, 1, {I}},

    /* ---- Reflection: DeclReflection ---- */
    {"slang_decl_get_name",             fn_slang_decl_get_name,            3, {I,I,I},           1, {I}},
    {"slang_decl_get_kind",             fn_slang_decl_get_kind,            1, {I},               1, {I}},
    {"slang_decl_get_children_count",   fn_slang_decl_get_children_count,  1, {I},               1, {I}},
    {"slang_decl_get_child",            fn_slang_decl_get_child,           2, {I,I},             1, {I}},
    {"slang_decl_get_type",             fn_slang_decl_get_type,            1, {I},               1, {I}},
    {"slang_decl_cast_to_variable",     fn_slang_decl_cast_to_variable,    1, {I},               1, {I}},
    {"slang_decl_cast_to_function",     fn_slang_decl_cast_to_function,    1, {I},               1, {I}},
    {"slang_decl_cast_to_generic",      fn_slang_decl_cast_to_generic,     1, {I},               1, {I}},
    {"slang_decl_get_parent",           fn_slang_decl_get_parent,          1, {I},               1, {I}},

    /* ---- Reflection: UserAttribute ---- */
    {"slang_attr_get_name",             fn_slang_attr_get_name,            3, {I,I,I},           1, {I}},
    {"slang_attr_get_argument_count",   fn_slang_attr_get_argument_count,  1, {I},               1, {I}},
    {"slang_attr_get_argument_type",    fn_slang_attr_get_argument_type,   2, {I,I},             1, {I}},
    {"slang_attr_get_argument_value_int", fn_slang_attr_get_argument_value_int, 3, {I,I,I},      1, {I}},
    {"slang_attr_get_argument_value_float", fn_slang_attr_get_argument_value_float, 3, {I,I,I},  1, {I}},
    {"slang_attr_get_argument_value_string", fn_slang_attr_get_argument_value_string, 4, {I,I,I,I}, 1, {I}},
};

#undef I

#define NUM_SLANG_BINDINGS (sizeof(SLANG_BINDING_TABLE)/sizeof(SLANG_BINDING_TABLE[0]))

/* ================================================================== */
/*  Functype builder                                                   */
/* ================================================================== */

static wasm_functype_t *smake_ft(uint32_t np, const wasm_valkind_t p[],
                                 uint32_t nr, const wasm_valkind_t r[])
{
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[10];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else wasm_valtype_vec_new_empty(&params);
    if (nr > 0) {
        wasm_valtype_t *rt[1] = { wasm_valtype_new(r[0]) };
        wasm_valtype_vec_new(&results, nr, rt);
    } else wasm_valtype_vec_new_empty(&results);
    return wasm_functype_new(&params, &results);
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

extern "C" void slang_bindings_init(SlangBindings *sb, WgpuBindings *wgpu)
{
    memset(sb, 0, sizeof(*sb));
    sb->wgpu = wgpu;
    htable_init(&sb->ht_global_session, 4);
    htable_init(&sb->ht_session, 4);
    htable_init(&sb->ht_component, 16);
    htable_init(&sb->ht_blob, 16);
    htable_init(&sb->ht_reflection, 64);
    printf("[slang] Initialized slang bindings (%zu imports)\n", NUM_SLANG_BINDINGS);
}

extern "C" void slang_bindings_destroy(SlangBindings *sb)
{
    /* Reflection handles are interior pointers — no release needed. */
    htable_destroy(&sb->ht_reflection);

    /* Blobs are standalone ref-counted objects — safe to release. */
    for (uint32_t h = 1; h <= sb->ht_blob.capacity; h++) {
        auto *b = (ISlangBlob *)htable_get(&sb->ht_blob, h);
        if (b) b->release();
    }
    htable_destroy(&sb->ht_blob);

    /* Components (IModule, IEntryPoint, linked programs) are *owned* by
       their ISession.  Releasing a session cascades to its children, so
       we must NOT release components individually — that would be a
       double-free if the session is still alive.  Just drop the table. */
    htable_destroy(&sb->ht_component);

    /* Sessions are owned by their IGlobalSession.  Releasing the global
       session cascades to all child sessions (and transitively to all
       modules).  Just drop the session table without releasing. */
    htable_destroy(&sb->ht_session);

    /* Release top-level global sessions — this cascades everything. */
    for (uint32_t h = 1; h <= sb->ht_global_session.capacity; h++) {
        auto *gs = (IGlobalSession *)htable_get(&sb->ht_global_session, h);
        if (gs) gs->release();
    }
    htable_destroy(&sb->ht_global_session);
}

extern "C" void slang_bindings_set_memory(SlangBindings *sb, wasm_memory_t *mem)
{
    sb->memory = mem;
}

extern "C" size_t slang_bindings_get_imports(SlangBindings  *sb,
                                              wasm_store_t   *store,
                                              const char   ***out_names,
                                              wasm_func_t  ***out_funcs)
{
    static const char *names[NUM_SLANG_BINDINGS];
    static wasm_func_t *funcs[NUM_SLANG_BINDINGS];
    for (size_t i = 0; i < NUM_SLANG_BINDINGS; i++) {
        names[i] = SLANG_BINDING_TABLE[i].name;
        wasm_functype_t *ft = smake_ft(SLANG_BINDING_TABLE[i].np,
                                       SLANG_BINDING_TABLE[i].params,
                                       SLANG_BINDING_TABLE[i].nr,
                                       SLANG_BINDING_TABLE[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft,
                                          SLANG_BINDING_TABLE[i].cb, sb, nullptr);
        wasm_functype_delete(ft);
    }
    *out_names = names;
    *out_funcs = funcs;
    return NUM_SLANG_BINDINGS;
}
