/**
 * @file slang_bindings.h
 * @brief Slang shader compiler WASM bindings for Yumi Browser.
 *
 * Full-parity bindings exposing the Slang C API to WASM guests via
 * integer handles. Covers global sessions, compilation sessions,
 * modules, entry points, linking, code generation, and the complete
 * reflection API (types, layouts, variables, bindings, entry points,
 * functions, generics, declarations, and user attributes).
 */

#ifndef SLANG_BINDINGS_H
#define SLANG_BINDINGS_H

#include "deps.h"
#include "handle_table.h"
#include "wgpu_bindings.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Slang binding state.
 *
 * Handle tables:
 *  - ht_global_session: IGlobalSession* (COM, release on destroy)
 *  - ht_session:        ISession*       (COM, release on destroy)
 *  - ht_component:      IComponentType* (COM, release on destroy)
 *                        Covers IModule, IEntryPoint, composed, linked.
 *  - ht_blob:           ISlangBlob*     (COM, release on destroy)
 *  - ht_reflection:     void*           (interior pointers, no release)
 *                        All reflection types (ProgramLayout, TypeReflection,
 *                        TypeLayoutReflection, VariableReflection, etc.)
 */
typedef struct {
    WgpuBindings  *wgpu;
    HandleTable    ht_global_session;
    HandleTable    ht_session;
    HandleTable    ht_component;
    HandleTable    ht_blob;
    HandleTable    ht_reflection;
    wasm_memory_t *memory;
} SlangBindings;

void   slang_bindings_init(SlangBindings *sb, WgpuBindings *wgpu);
void   slang_bindings_destroy(SlangBindings *sb);
void   slang_bindings_set_memory(SlangBindings *sb, wasm_memory_t *mem);
size_t slang_bindings_get_imports(SlangBindings  *sb,
                                  wasm_store_t   *store,
                                  const char   ***out_names,
                                  wasm_func_t  ***out_funcs);

#ifdef __cplusplus
}
#endif

#endif /* SLANG_BINDINGS_H */
