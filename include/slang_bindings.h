/*
    Slang Shader Compiler WASM Bindings
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
