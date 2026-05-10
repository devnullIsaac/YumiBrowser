/*
    Yumi SDK — Slang Shader Compiler WASM Imports
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
 * @file wasm_slang.h
 * @brief Full-parity WebAssembly guest header for the Slang shader compiler.
 *
 * @details
 * This header declares host-imported functions that provide complete access to
 * the Slang shading language compiler from WASM guest modules. Every function
 * maps 1:1 to a host import in `slang_bindings.cpp`.
 *
 * ## Handle Model
 * All Slang objects are referenced by opaque 32-bit integer handles. Handle 0
 * is universally invalid / null. Guests must explicitly destroy handles to
 * release host resources.
 *
 * ## Typical Workflow
 * @code
 *   slang_global_session_t gs = slang_create_global_session();
 *   slang_session_t session = slang_gs_create_session(gs, SLANG_WGSL, ...);
 *   slang_component_t module = slang_session_load_module_from_source(session, ...);
 *   slang_component_t ep = slang_module_find_and_check_entry_point(module, ...);
 *   slang_component_t composite = slang_session_create_composite(session, &ep, 1, ...);
 *   slang_component_t linked = slang_component_link(composite);
 *   slang_blob_t blob = slang_component_get_entry_point_code(linked, 0, 0);
 *   slang_reflection_t refl = slang_component_get_layout(linked, 0);
 *   // query reflection, create gpu_shader_t from blob ...
 *   slang_blob_destroy(blob);
 *   slang_component_destroy(linked);
 *   slang_session_destroy(session);
 *   slang_gs_destroy(gs);
 * @endcode
 *
 * ## Name Functions Convention
 * For functions that return names (slang_module_get_name, etc.):
 *   - Pass `(handle, dst=0, max_len=0)` to query length only.
 *   - Pass `(handle, dst, max_len)` to copy into WASM memory.
 *   - Return value: bytes written, or total length when `dst==0`.
 *   - Names are **not** NUL-terminated unless `max_len` exceeds actual length.
 *
 * @see https://github.com/shader-slang/slang
 * @see wasm_gpu.h for gpu_shader_t and related GPU types.
 */

#ifndef WASM_SLANG_H
#define WASM_SLANG_H

#include "wasm_gpu.h"
#include <stdint.h>

/* ================================================================== */
/*  Opaque handle types                                                */
/* ================================================================== */

typedef int32_t slang_global_session_t;     /* IGlobalSession              */
typedef int32_t slang_session_t;            /* ISession                    */
typedef int32_t slang_component_t;          /* IComponentType (module/ep/composed/linked) */
typedef int32_t slang_blob_t;              /* ISlangBlob                  */
typedef int32_t slang_reflection_t;        /* ProgramLayout / ShaderReflection */
typedef int32_t slang_type_t;              /* TypeReflection              */
typedef int32_t slang_type_layout_t;       /* TypeLayoutReflection        */
typedef int32_t slang_variable_t;          /* VariableReflection          */
typedef int32_t slang_variable_layout_t;   /* VariableLayoutReflection    */
typedef int32_t slang_ep_layout_t;         /* EntryPointReflection        */
typedef int32_t slang_func_reflection_t;   /* FunctionReflection          */
typedef int32_t slang_generic_reflection_t;/* GenericReflection           */
typedef int32_t slang_decl_reflection_t;   /* DeclReflection              */
typedef int32_t slang_user_attribute_t;    /* UserAttribute               */

/* ================================================================== */
/*  Compile target (SlangCompileTarget)                                */
/* ================================================================== */

#define SLANG_TARGET_UNKNOWN                0
#define SLANG_TARGET_NONE                   1
#define SLANG_GLSL                          2
#define SLANG_HLSL                          5
#define SLANG_SPIRV                         6
#define SLANG_SPIRV_ASM                     7
#define SLANG_DXBC                          8
#define SLANG_DXBC_ASM                      9
#define SLANG_DXIL                         10
#define SLANG_DXIL_ASM                     11
#define SLANG_C_SOURCE                     12
#define SLANG_CPP_SOURCE                   13
#define SLANG_HOST_EXECUTABLE              14
#define SLANG_SHADER_SHARED_LIBRARY        15
#define SLANG_SHADER_HOST_CALLABLE         16
#define SLANG_CUDA_SOURCE                  17
#define SLANG_PTX                          18
#define SLANG_CUDA_OBJECT_CODE             19
#define SLANG_OBJECT_CODE                  20
#define SLANG_HOST_CPP_SOURCE              21
#define SLANG_HOST_HOST_CALLABLE           22
#define SLANG_CPP_PYTORCH_BINDING          23
#define SLANG_METAL                        24
#define SLANG_METAL_LIB                    25
#define SLANG_METAL_LIB_ASM               26
#define SLANG_HOST_SHARED_LIBRARY          27
#define SLANG_WGSL                         28
#define SLANG_WGSL_SPIRV_ASM              29
#define SLANG_WGSL_SPIRV                  30

/* ================================================================== */
/*  Shader stage (SlangStage)                                          */
/* ================================================================== */

#define SLANG_STAGE_NONE            0
#define SLANG_STAGE_VERTEX          1
#define SLANG_STAGE_HULL            2
#define SLANG_STAGE_DOMAIN          3
#define SLANG_STAGE_GEOMETRY        4
#define SLANG_STAGE_FRAGMENT        5
#define SLANG_STAGE_COMPUTE         6
#define SLANG_STAGE_RAY_GENERATION  7
#define SLANG_STAGE_INTERSECTION    8
#define SLANG_STAGE_ANY_HIT         9
#define SLANG_STAGE_CLOSEST_HIT    10
#define SLANG_STAGE_MISS           11
#define SLANG_STAGE_CALLABLE       12
#define SLANG_STAGE_MESH           13
#define SLANG_STAGE_AMPLIFICATION  14
#define SLANG_STAGE_PIXEL          SLANG_STAGE_FRAGMENT

/* ================================================================== */
/*  Source language (SlangSourceLanguage)                               */
/* ================================================================== */

#define SLANG_SOURCE_LANGUAGE_UNKNOWN   0
#define SLANG_SOURCE_LANGUAGE_SLANG     1
#define SLANG_SOURCE_LANGUAGE_HLSL      2
#define SLANG_SOURCE_LANGUAGE_GLSL      3
#define SLANG_SOURCE_LANGUAGE_C         4
#define SLANG_SOURCE_LANGUAGE_CPP       5
#define SLANG_SOURCE_LANGUAGE_CUDA      6
#define SLANG_SOURCE_LANGUAGE_SPIRV     7
#define SLANG_SOURCE_LANGUAGE_METAL     8
#define SLANG_SOURCE_LANGUAGE_WGSL      9

/* ================================================================== */
/*  Severity (SlangSeverity)                                           */
/* ================================================================== */

#define SLANG_SEVERITY_DISABLED  0
#define SLANG_SEVERITY_NOTE      1
#define SLANG_SEVERITY_WARNING   2
#define SLANG_SEVERITY_ERROR     3
#define SLANG_SEVERITY_FATAL     4
#define SLANG_SEVERITY_INTERNAL  5

/* ================================================================== */
/*  Floating-point mode (SlangFloatingPointMode)                       */
/* ================================================================== */

#define SLANG_FLOATING_POINT_MODE_DEFAULT  0
#define SLANG_FLOATING_POINT_MODE_FAST     1
#define SLANG_FLOATING_POINT_MODE_PRECISE  2

/* ================================================================== */
/*  Optimization level (SlangOptimizationLevel)                        */
/* ================================================================== */

#define SLANG_OPTIMIZATION_LEVEL_NONE     0
#define SLANG_OPTIMIZATION_LEVEL_DEFAULT  1
#define SLANG_OPTIMIZATION_LEVEL_HIGH     2
#define SLANG_OPTIMIZATION_LEVEL_MAXIMAL  3

/* ================================================================== */
/*  Matrix layout (SlangMatrixLayoutMode)                              */
/* ================================================================== */

#define SLANG_MATRIX_LAYOUT_MODE_UNKNOWN      0
#define SLANG_MATRIX_LAYOUT_ROW_MAJOR         1
#define SLANG_MATRIX_LAYOUT_COLUMN_MAJOR      2

/* ================================================================== */
/*  Type kind (SlangTypeKind)                                          */
/* ================================================================== */

#define SLANG_TYPE_KIND_NONE                    0
#define SLANG_TYPE_KIND_STRUCT                  1
#define SLANG_TYPE_KIND_ARRAY                   2
#define SLANG_TYPE_KIND_MATRIX                  3
#define SLANG_TYPE_KIND_VECTOR                  4
#define SLANG_TYPE_KIND_SCALAR                  5
#define SLANG_TYPE_KIND_CONSTANT_BUFFER         6
#define SLANG_TYPE_KIND_RESOURCE                7
#define SLANG_TYPE_KIND_SAMPLER_STATE           8
#define SLANG_TYPE_KIND_TEXTURE_BUFFER          9
#define SLANG_TYPE_KIND_SHADER_STORAGE_BUFFER  10
#define SLANG_TYPE_KIND_PARAMETER_BLOCK        11
#define SLANG_TYPE_KIND_GENERIC_TYPE_PARAMETER 12
#define SLANG_TYPE_KIND_INTERFACE              13
#define SLANG_TYPE_KIND_OUTPUT_STREAM          14
#define SLANG_TYPE_KIND_MESH_OUTPUT            15
#define SLANG_TYPE_KIND_SPECIALIZED            16
#define SLANG_TYPE_KIND_FEEDBACK               17
#define SLANG_TYPE_KIND_POINTER                18
#define SLANG_TYPE_KIND_DYNAMIC_RESOURCE       19
#define SLANG_TYPE_KIND_ENUM                   20

/* ================================================================== */
/*  Scalar type (SlangScalarType)                                      */
/* ================================================================== */

#define SLANG_SCALAR_TYPE_NONE     0
#define SLANG_SCALAR_TYPE_VOID     1
#define SLANG_SCALAR_TYPE_BOOL     2
#define SLANG_SCALAR_TYPE_INT32    3
#define SLANG_SCALAR_TYPE_UINT32   4
#define SLANG_SCALAR_TYPE_INT64    5
#define SLANG_SCALAR_TYPE_UINT64   6
#define SLANG_SCALAR_TYPE_FLOAT16  7
#define SLANG_SCALAR_TYPE_FLOAT32  8
#define SLANG_SCALAR_TYPE_FLOAT64  9
#define SLANG_SCALAR_TYPE_INT8    10
#define SLANG_SCALAR_TYPE_UINT8   11
#define SLANG_SCALAR_TYPE_INT16   12
#define SLANG_SCALAR_TYPE_UINT16  13
#define SLANG_SCALAR_TYPE_INTPTR  14
#define SLANG_SCALAR_TYPE_UINTPTR 15

/* ================================================================== */
/*  Decl kind (SlangDeclKind)                                          */
/* ================================================================== */

#define SLANG_DECL_KIND_UNSUPPORTED_FOR_REFLECTION 0
#define SLANG_DECL_KIND_STRUCT                     1
#define SLANG_DECL_KIND_FUNC                       2
#define SLANG_DECL_KIND_MODULE                     3
#define SLANG_DECL_KIND_GENERIC                    4
#define SLANG_DECL_KIND_VARIABLE                   5
#define SLANG_DECL_KIND_NAMESPACE                  6
#define SLANG_DECL_KIND_ENUM                       7

/* ================================================================== */
/*  Resource shape (SlangResourceShape)                                */
/* ================================================================== */

#define SLANG_RESOURCE_BASE_SHAPE_MASK          0x0F
#define SLANG_RESOURCE_NONE                     0x00
#define SLANG_TEXTURE_1D                        0x01
#define SLANG_TEXTURE_2D                        0x02
#define SLANG_TEXTURE_3D                        0x03
#define SLANG_TEXTURE_CUBE                      0x04
#define SLANG_TEXTURE_BUFFER                    0x05
#define SLANG_STRUCTURED_BUFFER                 0x06
#define SLANG_BYTE_ADDRESS_BUFFER               0x07
#define SLANG_RESOURCE_UNKNOWN                  0x08
#define SLANG_ACCELERATION_STRUCTURE            0x09
#define SLANG_TEXTURE_SUBPASS                   0x0A
#define SLANG_TEXTURE_FEEDBACK_FLAG             0x10
#define SLANG_TEXTURE_SHADOW_FLAG               0x20
#define SLANG_TEXTURE_ARRAY_FLAG                0x40
#define SLANG_TEXTURE_MULTISAMPLE_FLAG          0x80
#define SLANG_TEXTURE_COMBINED_FLAG             0x100
#define SLANG_TEXTURE_1D_ARRAY                  (SLANG_TEXTURE_1D | SLANG_TEXTURE_ARRAY_FLAG)
#define SLANG_TEXTURE_2D_ARRAY                  (SLANG_TEXTURE_2D | SLANG_TEXTURE_ARRAY_FLAG)
#define SLANG_TEXTURE_CUBE_ARRAY                (SLANG_TEXTURE_CUBE | SLANG_TEXTURE_ARRAY_FLAG)
#define SLANG_TEXTURE_2D_MULTISAMPLE            (SLANG_TEXTURE_2D | SLANG_TEXTURE_MULTISAMPLE_FLAG)
#define SLANG_TEXTURE_2D_MULTISAMPLE_ARRAY      (SLANG_TEXTURE_2D | SLANG_TEXTURE_MULTISAMPLE_FLAG | SLANG_TEXTURE_ARRAY_FLAG)

/* ================================================================== */
/*  Resource access (SlangResourceAccess)                              */
/* ================================================================== */

#define SLANG_RESOURCE_ACCESS_NONE              0
#define SLANG_RESOURCE_ACCESS_READ              1
#define SLANG_RESOURCE_ACCESS_READ_WRITE        2
#define SLANG_RESOURCE_ACCESS_RASTER_ORDERED    3
#define SLANG_RESOURCE_ACCESS_APPEND            4
#define SLANG_RESOURCE_ACCESS_CONSUME           5
#define SLANG_RESOURCE_ACCESS_WRITE             6
#define SLANG_RESOURCE_ACCESS_FEEDBACK          7

/* ================================================================== */
/*  Parameter category (SlangParameterCategory)                        */
/* ================================================================== */

#define SLANG_PARAMETER_CATEGORY_NONE                          0
#define SLANG_PARAMETER_CATEGORY_MIXED                         1
#define SLANG_PARAMETER_CATEGORY_CONSTANT_BUFFER               2
#define SLANG_PARAMETER_CATEGORY_SHADER_RESOURCE               3
#define SLANG_PARAMETER_CATEGORY_UNORDERED_ACCESS              4
#define SLANG_PARAMETER_CATEGORY_VARYING_INPUT                 5
#define SLANG_PARAMETER_CATEGORY_VARYING_OUTPUT                6
#define SLANG_PARAMETER_CATEGORY_SAMPLER_STATE                 7
#define SLANG_PARAMETER_CATEGORY_UNIFORM                       8
#define SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT         9
#define SLANG_PARAMETER_CATEGORY_SPECIALIZATION_CONSTANT      10
#define SLANG_PARAMETER_CATEGORY_PUSH_CONSTANT_BUFFER         11
#define SLANG_PARAMETER_CATEGORY_REGISTER_SPACE               12
#define SLANG_PARAMETER_CATEGORY_GENERIC                      13
#define SLANG_PARAMETER_CATEGORY_RAY_PAYLOAD                  14
#define SLANG_PARAMETER_CATEGORY_HIT_ATTRIBUTES               15
#define SLANG_PARAMETER_CATEGORY_CALLABLE_PAYLOAD             16
#define SLANG_PARAMETER_CATEGORY_SHADER_RECORD                17
#define SLANG_PARAMETER_CATEGORY_EXISTENTIAL_TYPE_PARAM       18
#define SLANG_PARAMETER_CATEGORY_EXISTENTIAL_OBJECT_PARAM     19
#define SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE   20
#define SLANG_PARAMETER_CATEGORY_SUBPASS                      21
#define SLANG_PARAMETER_CATEGORY_METAL_ARGUMENT_BUFFER_ELEMENT 22
#define SLANG_PARAMETER_CATEGORY_METAL_ATTRIBUTE              23
#define SLANG_PARAMETER_CATEGORY_METAL_PAYLOAD                24

/* ================================================================== */
/*  Binding type (SlangBindingType)                                    */
/* ================================================================== */

#define SLANG_BINDING_TYPE_UNKNOWN                              0
#define SLANG_BINDING_TYPE_SAMPLER                              1
#define SLANG_BINDING_TYPE_TEXTURE                              2
#define SLANG_BINDING_TYPE_CONSTANT_BUFFER                      3
#define SLANG_BINDING_TYPE_PARAMETER_BLOCK                      4
#define SLANG_BINDING_TYPE_TYPED_BUFFER                         5
#define SLANG_BINDING_TYPE_RAW_BUFFER                           6
#define SLANG_BINDING_TYPE_COMBINED_TEXTURE_SAMPLER             7
#define SLANG_BINDING_TYPE_INPUT_RENDER_TARGET                  8
#define SLANG_BINDING_TYPE_INLINE_UNIFORM_DATA                  9
#define SLANG_BINDING_TYPE_RAY_TRACING_ACCELERATION_STRUCTURE  10
#define SLANG_BINDING_TYPE_VARYING_INPUT                       11
#define SLANG_BINDING_TYPE_VARYING_OUTPUT                      12
#define SLANG_BINDING_TYPE_EXISTENTIAL_VALUE                   13
#define SLANG_BINDING_TYPE_PUSH_CONSTANT                       14
#define SLANG_BINDING_TYPE_MUTABLE_FLAG                        0x100
#define SLANG_BINDING_TYPE_MUTABLE_TEXTURE   (SLANG_BINDING_TYPE_TEXTURE | SLANG_BINDING_TYPE_MUTABLE_FLAG)
#define SLANG_BINDING_TYPE_MUTABLE_TYPED_BUFFER (SLANG_BINDING_TYPE_TYPED_BUFFER | SLANG_BINDING_TYPE_MUTABLE_FLAG)
#define SLANG_BINDING_TYPE_MUTABLE_RAW_BUFFER (SLANG_BINDING_TYPE_RAW_BUFFER | SLANG_BINDING_TYPE_MUTABLE_FLAG)

/* ================================================================== */
/*  Layout rules (SlangLayoutRules)                                    */
/* ================================================================== */

#define SLANG_LAYOUT_RULES_DEFAULT                              0
#define SLANG_LAYOUT_RULES_METAL_ARGUMENT_BUFFER_TIER_2         1
#define SLANG_LAYOUT_RULES_DEFAULT_STRUCTURED_BUFFER            2
#define SLANG_LAYOUT_RULES_DEFAULT_CONSTANT_BUFFER              3

/* ================================================================== */
/*  Modifier ID (SlangModifierID)                                      */
/* ================================================================== */

#define SLANG_MODIFIER_SHARED           0
#define SLANG_MODIFIER_NO_DIFF          1
#define SLANG_MODIFIER_STATIC           2
#define SLANG_MODIFIER_CONST            3
#define SLANG_MODIFIER_EXPORT           4
#define SLANG_MODIFIER_EXTERN           5
#define SLANG_MODIFIER_DIFFERENTIABLE   6
#define SLANG_MODIFIER_MUTATING         7
#define SLANG_MODIFIER_IN               8
#define SLANG_MODIFIER_OUT              9
#define SLANG_MODIFIER_INOUT           10

/* ================================================================== */
/*  Pass-through compiler (SlangPassThrough)                           */
/* ================================================================== */

#define SLANG_PASS_THROUGH_NONE              0
#define SLANG_PASS_THROUGH_FXC              1
#define SLANG_PASS_THROUGH_DXC              2
#define SLANG_PASS_THROUGH_GLSLANG          3
#define SLANG_PASS_THROUGH_SPIRV_DIS        4
#define SLANG_PASS_THROUGH_CLANG            5
#define SLANG_PASS_THROUGH_VISUAL_STUDIO    6
#define SLANG_PASS_THROUGH_GCC              7
#define SLANG_PASS_THROUGH_GENERIC_C_CPP    8
#define SLANG_PASS_THROUGH_NVRTC            9
#define SLANG_PASS_THROUGH_LLVM            10
#define SLANG_PASS_THROUGH_SPIRV_OPT       11
#define SLANG_PASS_THROUGH_METAL           12
#define SLANG_PASS_THROUGH_TINT            13

/* ================================================================== */
/*  Result codes                                                       */
/* ================================================================== */

#define SLANG_OK      0
#define SLANG_FAIL    ((int32_t)0x80004005)

#define IMPORT __attribute__((import_module("env")))

/* ================================================================== */
/*  Global Session                                                     */
/* ================================================================== */

/** Create a new Slang global session. Returns handle or 0 on failure. */
IMPORT __attribute__((import_name("slang_create_global_session")))
slang_global_session_t slang_create_global_session(void);

/** Destroy a global session. */
IMPORT __attribute__((import_name("slang_gs_destroy")))
void slang_gs_destroy(slang_global_session_t gs);

/** Look up a profile by name (e.g. "sm_6_0"). Returns SlangProfileID. */
IMPORT __attribute__((import_name("slang_gs_find_profile")))
int32_t slang_gs_find_profile(slang_global_session_t gs,
                              const void *name_ptr, int32_t name_len);

/** Create a compilation session from a global session.
 *  @param gs            Global session handle.
 *  @param target_format SlangCompileTarget (SLANG_WGSL, SLANG_SPIRV, etc.).
 *  @param profile       SlangProfileID (0 = unknown).
 *  @param fp_mode       SlangFloatingPointMode.
 *  @param matrix_layout SlangMatrixLayoutMode.
 *  @return Session handle or 0 on failure. */
IMPORT __attribute__((import_name("slang_gs_create_session")))
slang_session_t slang_gs_create_session(slang_global_session_t gs,
                                        int32_t target_format,
                                        int32_t profile,
                                        int32_t fp_mode,
                                        int32_t matrix_layout);

/* ================================================================== */
/*  Session                                                            */
/* ================================================================== */

/** Destroy a session. */
IMPORT __attribute__((import_name("slang_session_destroy")))
void slang_session_destroy(slang_session_t session);

/** Load a module by name (as if using `import`).
 *  @param diag_out_ptr  Pointer in WASM memory to receive diagnostic blob handle (i32).
 *                       Pass 0 to discard diagnostics.
 *  @return Component handle (module), or 0 on failure. */
IMPORT __attribute__((import_name("slang_session_load_module")))
slang_component_t slang_session_load_module(slang_session_t session,
                                            const void *name_ptr, int32_t name_len,
                                            int32_t *diag_out_ptr);

/** Load a module from a source string.
 *  @param mod_name_ptr/len  Module name.
 *  @param path_ptr/len      Virtual file path for diagnostics.
 *  @param src_ptr/len       Slang source code.
 *  @param diag_out_ptr      Pointer in WASM memory for diagnostic blob handle, or 0.
 *  @return Component handle (module), or 0 on failure. */
IMPORT __attribute__((import_name("slang_session_load_module_from_source")))
slang_component_t slang_session_load_module_from_source(
    slang_session_t session,
    const void *mod_name_ptr, int32_t mod_name_len,
    const void *path_ptr, int32_t path_len,
    const void *src_ptr, int32_t src_len,
    int32_t *diag_out_ptr);

/** Create a composite component type from an array of component handles.
 *  @param handles_ptr  Pointer to array of int32_t component handles in WASM memory.
 *  @param count        Number of components.
 *  @param diag_out_ptr Diagnostic blob output, or 0.
 *  @return Composite component handle, or 0 on failure. */
IMPORT __attribute__((import_name("slang_session_create_composite")))
slang_component_t slang_session_create_composite(slang_session_t session,
                                                  const int32_t *handles_ptr,
                                                  int32_t count,
                                                  int32_t *diag_out_ptr);

/** Get the number of modules currently loaded in the session. */
IMPORT __attribute__((import_name("slang_session_get_loaded_module_count")))
int32_t slang_session_get_loaded_module_count(slang_session_t session);

/** Get a loaded module by index. Returns component handle. */
IMPORT __attribute__((import_name("slang_session_get_loaded_module")))
slang_component_t slang_session_get_loaded_module(slang_session_t session, int32_t index);

/* ================================================================== */
/*  Component Type (Module / EntryPoint / Composed / Linked)           */
/* ================================================================== */

/** Release a component type (decrements COM reference count). */
IMPORT __attribute__((import_name("slang_component_destroy")))
void slang_component_destroy(slang_component_t component);

/** Get the reflection layout for this component on the given target.
 *  @param target_index  Target index (usually 0).
 *  @param diag_out_ptr  Diagnostic blob output, or 0.
 *  @return Reflection handle (ProgramLayout), or 0 on failure. */
IMPORT __attribute__((import_name("slang_component_get_layout")))
slang_reflection_t slang_component_get_layout(slang_component_t component,
                                              int32_t target_index,
                                              int32_t *diag_out_ptr);

/** Get compiled code for an entry point on a target.
 *  @param entry_index   Entry point index.
 *  @param target_index  Target index (usually 0).
 *  @param diag_out_ptr  Diagnostic blob output, or 0.
 *  @return Blob handle with compiled code, or 0 on failure. */
IMPORT __attribute__((import_name("slang_component_get_entry_point_code")))
slang_blob_t slang_component_get_entry_point_code(slang_component_t component,
                                                   int32_t entry_index,
                                                   int32_t target_index,
                                                   int32_t *diag_out_ptr);

/** Get compiled code for an entire target (whole-program).
 *  @return Blob handle, or 0 on failure. */
IMPORT __attribute__((import_name("slang_component_get_target_code")))
slang_blob_t slang_component_get_target_code(slang_component_t component,
                                              int32_t target_index,
                                              int32_t *diag_out_ptr);

/** Link this component type to resolve dependencies.
 *  @param diag_out_ptr  Diagnostic blob output, or 0.
 *  @return Linked component handle, or 0 on failure. */
IMPORT __attribute__((import_name("slang_component_link")))
slang_component_t slang_component_link(slang_component_t component,
                                        int32_t *diag_out_ptr);

/** Get the number of specialization parameters. */
IMPORT __attribute__((import_name("slang_component_get_specialization_param_count")))
int32_t slang_component_get_specialization_param_count(slang_component_t component);

/* ================================================================== */
/*  Module-specific                                                    */
/* ================================================================== */

/** Find an entry point in a module by name (requires [shader(...)] attribute).
 *  @return Component handle (entry point), or 0 on failure. */
IMPORT __attribute__((import_name("slang_module_find_entry_point")))
slang_component_t slang_module_find_entry_point(slang_component_t module,
                                                 const void *name_ptr, int32_t name_len);

/** Find and validate an entry point, even without [shader(...)] attribute.
 *  @param stage         SlangStage value.
 *  @param diag_out_ptr  Diagnostic blob output, or 0.
 *  @return Component handle (entry point), or 0 on failure. */
IMPORT __attribute__((import_name("slang_module_find_and_check_entry_point")))
slang_component_t slang_module_find_and_check_entry_point(
    slang_component_t module,
    const void *name_ptr, int32_t name_len,
    int32_t stage,
    int32_t *diag_out_ptr);

/** Get the number of entry points defined in the module. */
IMPORT __attribute__((import_name("slang_module_get_defined_entry_point_count")))
int32_t slang_module_get_defined_entry_point_count(slang_component_t module);

/** Get a defined entry point by index.
 *  @return Component handle (entry point), or 0. */
IMPORT __attribute__((import_name("slang_module_get_defined_entry_point")))
slang_component_t slang_module_get_defined_entry_point(slang_component_t module,
                                                        int32_t index);

/** Get the module name. See header comment for name function convention. */
IMPORT __attribute__((import_name("slang_module_get_name")))
int32_t slang_module_get_name(slang_component_t module,
                              void *dst_ptr, int32_t max_len);

/** Get the module file path. */
IMPORT __attribute__((import_name("slang_module_get_file_path")))
int32_t slang_module_get_file_path(slang_component_t module,
                                    void *dst_ptr, int32_t max_len);

/** Get the module-level declaration reflection. */
IMPORT __attribute__((import_name("slang_module_get_module_reflection")))
slang_decl_reflection_t slang_module_get_module_reflection(slang_component_t module);

/* ================================================================== */
/*  Blob                                                               */
/* ================================================================== */

/** Get the size of a blob in bytes. */
IMPORT __attribute__((import_name("slang_blob_get_size")))
int32_t slang_blob_get_size(slang_blob_t blob);

/** Copy blob data into WASM memory.
 *  @return Number of bytes copied. */
IMPORT __attribute__((import_name("slang_blob_read")))
int32_t slang_blob_read(slang_blob_t blob, void *dst_ptr, int32_t max_len);

/** Release a blob. */
IMPORT __attribute__((import_name("slang_blob_destroy")))
void slang_blob_destroy(slang_blob_t blob);

/* ================================================================== */
/*  Convenience: Shader module creation                                */
/* ================================================================== */

/** Create a WGPUShaderModule from a code blob (typically WGSL output).
 *  @param blob       Blob handle containing WGSL source.
 *  @param label_ptr  Optional label string, or 0.
 *  @param label_len  Label length.
 *  @return gpu_shader_t handle, or 0 on failure. */
IMPORT __attribute__((import_name("slang_create_shader_module_from_blob")))
gpu_shader_t slang_create_shader_module_from_blob(slang_blob_t blob,
                                                   const void *label_ptr,
                                                   int32_t label_len);

/* ================================================================== */
/*  Reflection — ProgramLayout (ShaderReflection)                      */
/* ================================================================== */

/** Get the number of global shader parameters. */
IMPORT __attribute__((import_name("slang_refl_get_parameter_count")))
int32_t slang_refl_get_parameter_count(slang_reflection_t refl);

/** Get a global shader parameter by index.
 *  @return Variable layout handle. */
IMPORT __attribute__((import_name("slang_refl_get_parameter_by_index")))
slang_variable_layout_t slang_refl_get_parameter_by_index(slang_reflection_t refl,
                                                           int32_t index);

/** Get the number of entry points in the program. */
IMPORT __attribute__((import_name("slang_refl_get_entry_point_count")))
int32_t slang_refl_get_entry_point_count(slang_reflection_t refl);

/** Get entry point reflection by index.
 *  @return Entry point layout handle. */
IMPORT __attribute__((import_name("slang_refl_get_entry_point_by_index")))
slang_ep_layout_t slang_refl_get_entry_point_by_index(slang_reflection_t refl,
                                                       int32_t index);

/** Find a type in the program by name.
 *  @return Type reflection handle, or 0 if not found. */
IMPORT __attribute__((import_name("slang_refl_find_type_by_name")))
slang_type_t slang_refl_find_type_by_name(slang_reflection_t refl,
                                           const void *name_ptr, int32_t name_len);

/** Get the type layout for a type under given layout rules.
 *  @param rules  SlangLayoutRules value (0 = default).
 *  @return Type layout handle. */
IMPORT __attribute__((import_name("slang_refl_get_type_layout")))
slang_type_layout_t slang_refl_get_type_layout(slang_reflection_t refl,
                                                slang_type_t type,
                                                int32_t rules);

/** Find entry point reflection by name.
 *  @return Entry point layout handle, or 0 if not found. */
IMPORT __attribute__((import_name("slang_refl_find_entry_point_by_name")))
slang_ep_layout_t slang_refl_find_entry_point_by_name(slang_reflection_t refl,
                                                       const void *name_ptr,
                                                       int32_t name_len);

/** Get the binding index for the global constant buffer. */
IMPORT __attribute__((import_name("slang_refl_get_global_constant_buffer_binding")))
int32_t slang_refl_get_global_constant_buffer_binding(slang_reflection_t refl);

/** Get the size of the global constant buffer. */
IMPORT __attribute__((import_name("slang_refl_get_global_constant_buffer_size")))
int32_t slang_refl_get_global_constant_buffer_size(slang_reflection_t refl);

/** Get the type layout for global parameters. */
IMPORT __attribute__((import_name("slang_refl_get_global_params_type_layout")))
slang_type_layout_t slang_refl_get_global_params_type_layout(slang_reflection_t refl);

/** Get the variable layout for global parameters. */
IMPORT __attribute__((import_name("slang_refl_get_global_params_var_layout")))
slang_variable_layout_t slang_refl_get_global_params_var_layout(slang_reflection_t refl);

/** Find a function by name.
 *  @return Function reflection handle, or 0 if not found. */
IMPORT __attribute__((import_name("slang_refl_find_function_by_name")))
slang_func_reflection_t slang_refl_find_function_by_name(slang_reflection_t refl,
                                                          const void *name_ptr,
                                                          int32_t name_len);

/** Serialize the reflection to JSON.
 *  @return Blob handle containing JSON text. */
IMPORT __attribute__((import_name("slang_refl_to_json")))
slang_blob_t slang_refl_to_json(slang_reflection_t refl);

/** Get the hashed string count. */
IMPORT __attribute__((import_name("slang_refl_get_hashed_string_count")))
int32_t slang_refl_get_hashed_string_count(slang_reflection_t refl);

/** Check if sub_type is a subtype of super_type. Returns 1 or 0. */
IMPORT __attribute__((import_name("slang_refl_is_sub_type")))
int32_t slang_refl_is_sub_type(slang_reflection_t refl,
                                slang_type_t sub_type,
                                slang_type_t super_type);

/* ================================================================== */
/*  Reflection — TypeReflection                                        */
/* ================================================================== */

/** Get the kind of a type (SlangTypeKind). */
IMPORT __attribute__((import_name("slang_type_get_kind")))
int32_t slang_type_get_kind(slang_type_t type);

/** Get the number of fields in a struct type. */
IMPORT __attribute__((import_name("slang_type_get_field_count")))
int32_t slang_type_get_field_count(slang_type_t type);

/** Get a field by index.
 *  @return Variable reflection handle. */
IMPORT __attribute__((import_name("slang_type_get_field_by_index")))
slang_variable_t slang_type_get_field_by_index(slang_type_t type, int32_t index);

/** Get the number of elements (array size, or 0 if not an array). */
IMPORT __attribute__((import_name("slang_type_get_element_count")))
int32_t slang_type_get_element_count(slang_type_t type);

/** Get the element type (for arrays, vectors, matrices). */
IMPORT __attribute__((import_name("slang_type_get_element_type")))
slang_type_t slang_type_get_element_type(slang_type_t type);

/** Get the row count (for matrices). */
IMPORT __attribute__((import_name("slang_type_get_row_count")))
int32_t slang_type_get_row_count(slang_type_t type);

/** Get the column count (for matrices). */
IMPORT __attribute__((import_name("slang_type_get_column_count")))
int32_t slang_type_get_column_count(slang_type_t type);

/** Get the scalar type (SlangScalarType). */
IMPORT __attribute__((import_name("slang_type_get_scalar_type")))
int32_t slang_type_get_scalar_type(slang_type_t type);

/** Get the resource result type. */
IMPORT __attribute__((import_name("slang_type_get_resource_result_type")))
slang_type_t slang_type_get_resource_result_type(slang_type_t type);

/** Get the resource shape (SlangResourceShape). */
IMPORT __attribute__((import_name("slang_type_get_resource_shape")))
int32_t slang_type_get_resource_shape(slang_type_t type);

/** Get the resource access mode (SlangResourceAccess). */
IMPORT __attribute__((import_name("slang_type_get_resource_access")))
int32_t slang_type_get_resource_access(slang_type_t type);

/** Get the type name. */
IMPORT __attribute__((import_name("slang_type_get_name")))
int32_t slang_type_get_name(slang_type_t type, void *dst_ptr, int32_t max_len);

/** Get the number of user-defined attributes on this type. */
IMPORT __attribute__((import_name("slang_type_get_user_attribute_count")))
int32_t slang_type_get_user_attribute_count(slang_type_t type);

/** Get a user-defined attribute by index. */
IMPORT __attribute__((import_name("slang_type_get_user_attribute")))
slang_user_attribute_t slang_type_get_user_attribute(slang_type_t type, int32_t index);

/** Get the generic container for this type, if any. */
IMPORT __attribute__((import_name("slang_type_get_generic_container")))
slang_generic_reflection_t slang_type_get_generic_container(slang_type_t type);

/* ================================================================== */
/*  Reflection — TypeLayoutReflection                                  */
/* ================================================================== */

/** Get the underlying type for a type layout. */
IMPORT __attribute__((import_name("slang_type_layout_get_type")))
slang_type_t slang_type_layout_get_type(slang_type_layout_t layout);

/** Get the kind of the type layout (same as the underlying type's kind). */
IMPORT __attribute__((import_name("slang_type_layout_get_kind")))
int32_t slang_type_layout_get_kind(slang_type_layout_t layout);

/** Get the size of a type in the given parameter category.
 *  @param category  SlangParameterCategory value.
 *  @return Size in bytes, or SLANG_UNBOUNDED_SIZE/SLANG_UNKNOWN_SIZE. */
IMPORT __attribute__((import_name("slang_type_layout_get_size")))
int32_t slang_type_layout_get_size(slang_type_layout_t layout, int32_t category);

/** Get the stride of a type in the given parameter category. */
IMPORT __attribute__((import_name("slang_type_layout_get_stride")))
int32_t slang_type_layout_get_stride(slang_type_layout_t layout, int32_t category);

/** Get the alignment of a type in the given parameter category. */
IMPORT __attribute__((import_name("slang_type_layout_get_alignment")))
int32_t slang_type_layout_get_alignment(slang_type_layout_t layout, int32_t category);

/** Get the number of fields in a struct type layout. */
IMPORT __attribute__((import_name("slang_type_layout_get_field_count")))
int32_t slang_type_layout_get_field_count(slang_type_layout_t layout);

/** Get field layout by index.
 *  @return Variable layout handle. */
IMPORT __attribute__((import_name("slang_type_layout_get_field_by_index")))
slang_variable_layout_t slang_type_layout_get_field_by_index(slang_type_layout_t layout,
                                                              int32_t index);

/** Find a field index by name. Returns -1 if not found. */
IMPORT __attribute__((import_name("slang_type_layout_find_field_index_by_name")))
int32_t slang_type_layout_find_field_index_by_name(slang_type_layout_t layout,
                                                    const void *name_ptr,
                                                    int32_t name_len);

/** Get the element stride for array/buffer types. */
IMPORT __attribute__((import_name("slang_type_layout_get_element_stride")))
int32_t slang_type_layout_get_element_stride(slang_type_layout_t layout, int32_t category);

/** Get the element type layout. */
IMPORT __attribute__((import_name("slang_type_layout_get_element_type_layout")))
slang_type_layout_t slang_type_layout_get_element_type_layout(slang_type_layout_t layout);

/** Get the element variable layout. */
IMPORT __attribute__((import_name("slang_type_layout_get_element_var_layout")))
slang_variable_layout_t slang_type_layout_get_element_var_layout(slang_type_layout_t layout);

/** Get the primary parameter category. */
IMPORT __attribute__((import_name("slang_type_layout_get_parameter_category")))
int32_t slang_type_layout_get_parameter_category(slang_type_layout_t layout);

/** Get the number of parameter categories used by this type. */
IMPORT __attribute__((import_name("slang_type_layout_get_category_count")))
int32_t slang_type_layout_get_category_count(slang_type_layout_t layout);

/** Get a parameter category by index. */
IMPORT __attribute__((import_name("slang_type_layout_get_category_by_index")))
int32_t slang_type_layout_get_category_by_index(slang_type_layout_t layout, int32_t index);

/** Get the number of binding ranges. */
IMPORT __attribute__((import_name("slang_type_layout_get_binding_range_count")))
int32_t slang_type_layout_get_binding_range_count(slang_type_layout_t layout);

/** Get the binding type for a binding range (SlangBindingType). */
IMPORT __attribute__((import_name("slang_type_layout_get_binding_range_type")))
int32_t slang_type_layout_get_binding_range_type(slang_type_layout_t layout, int32_t range_index);

/** Get the number of bindings in a binding range. */
IMPORT __attribute__((import_name("slang_type_layout_get_binding_range_binding_count")))
int32_t slang_type_layout_get_binding_range_binding_count(slang_type_layout_t layout,
                                                           int32_t range_index);

/** Get the index offset for a binding range. */
IMPORT __attribute__((import_name("slang_type_layout_get_binding_range_index_offset")))
int32_t slang_type_layout_get_binding_range_index_offset(slang_type_layout_t layout,
                                                          int32_t range_index);

/** Get the space offset for a binding range. */
IMPORT __attribute__((import_name("slang_type_layout_get_binding_range_space_offset")))
int32_t slang_type_layout_get_binding_range_space_offset(slang_type_layout_t layout,
                                                          int32_t range_index);

/** Get the leaf type layout for a binding range. */
IMPORT __attribute__((import_name("slang_type_layout_get_binding_range_leaf_type_layout")))
slang_type_layout_t slang_type_layout_get_binding_range_leaf_type_layout(
    slang_type_layout_t layout, int32_t range_index);

/** Get the leaf variable for a binding range. */
IMPORT __attribute__((import_name("slang_type_layout_get_binding_range_leaf_variable")))
slang_variable_t slang_type_layout_get_binding_range_leaf_variable(
    slang_type_layout_t layout, int32_t range_index);

/** Get the field's binding range offset. */
IMPORT __attribute__((import_name("slang_type_layout_get_field_binding_range_offset")))
int32_t slang_type_layout_get_field_binding_range_offset(slang_type_layout_t layout,
                                                          int32_t field_index);

/** Get the number of descriptor sets. */
IMPORT __attribute__((import_name("slang_type_layout_get_descriptor_set_count")))
int32_t slang_type_layout_get_descriptor_set_count(slang_type_layout_t layout);

/** Get the space offset for a descriptor set. */
IMPORT __attribute__((import_name("slang_type_layout_get_descriptor_set_space_offset")))
int32_t slang_type_layout_get_descriptor_set_space_offset(slang_type_layout_t layout,
                                                           int32_t set_index);

/** Get the number of descriptor ranges in a descriptor set. */
IMPORT __attribute__((import_name("slang_type_layout_get_desc_set_range_count")))
int32_t slang_type_layout_get_desc_set_range_count(slang_type_layout_t layout,
                                                    int32_t set_index);

/** Get the binding type for a descriptor range in a descriptor set. */
IMPORT __attribute__((import_name("slang_type_layout_get_desc_set_range_type")))
int32_t slang_type_layout_get_desc_set_range_type(slang_type_layout_t layout,
                                                   int32_t set_index,
                                                   int32_t range_index);

/** Get the descriptor count for a range in a descriptor set. */
IMPORT __attribute__((import_name("slang_type_layout_get_desc_set_range_desc_count")))
int32_t slang_type_layout_get_desc_set_range_desc_count(slang_type_layout_t layout,
                                                         int32_t set_index,
                                                         int32_t range_index);

/** Get the category for a descriptor range in a set. */
IMPORT __attribute__((import_name("slang_type_layout_get_desc_set_range_category")))
int32_t slang_type_layout_get_desc_set_range_category(slang_type_layout_t layout,
                                                       int32_t set_index,
                                                       int32_t range_index);

/** Get the number of sub-object ranges. */
IMPORT __attribute__((import_name("slang_type_layout_get_sub_object_range_count")))
int32_t slang_type_layout_get_sub_object_range_count(slang_type_layout_t layout);

/** Get the binding range index for a sub-object range. */
IMPORT __attribute__((import_name("slang_type_layout_get_sub_object_range_binding_range_index")))
int32_t slang_type_layout_get_sub_object_range_binding_range_index(
    slang_type_layout_t layout, int32_t sub_obj_index);

/** Get the matrix layout mode for this type layout. */
IMPORT __attribute__((import_name("slang_type_layout_get_matrix_layout_mode")))
int32_t slang_type_layout_get_matrix_layout_mode(slang_type_layout_t layout);

/** Get the container variable layout (for buffer / parameter block types). */
IMPORT __attribute__((import_name("slang_type_layout_get_container_var_layout")))
slang_variable_layout_t slang_type_layout_get_container_var_layout(slang_type_layout_t layout);

/* ================================================================== */
/*  Reflection — VariableReflection                                    */
/* ================================================================== */

/** Get the variable name. */
IMPORT __attribute__((import_name("slang_var_get_name")))
int32_t slang_var_get_name(slang_variable_t var, void *dst_ptr, int32_t max_len);

/** Get the variable's type.
 *  @return Type reflection handle. */
IMPORT __attribute__((import_name("slang_var_get_type")))
slang_type_t slang_var_get_type(slang_variable_t var);

/** Check if the variable has a default value. Returns 1 or 0. */
IMPORT __attribute__((import_name("slang_var_has_default_value")))
int32_t slang_var_has_default_value(slang_variable_t var);

/** Get the number of user-defined attributes on this variable. */
IMPORT __attribute__((import_name("slang_var_get_user_attribute_count")))
int32_t slang_var_get_user_attribute_count(slang_variable_t var);

/** Get a user-defined attribute by index. */
IMPORT __attribute__((import_name("slang_var_get_user_attribute")))
slang_user_attribute_t slang_var_get_user_attribute(slang_variable_t var, int32_t index);

/* ================================================================== */
/*  Reflection — VariableLayoutReflection                              */
/* ================================================================== */

/** Get the underlying variable for a variable layout.
 *  @return Variable reflection handle. */
IMPORT __attribute__((import_name("slang_var_layout_get_variable")))
slang_variable_t slang_var_layout_get_variable(slang_variable_layout_t layout);

/** Get the type layout for a variable layout. */
IMPORT __attribute__((import_name("slang_var_layout_get_type_layout")))
slang_type_layout_t slang_var_layout_get_type_layout(slang_variable_layout_t layout);

/** Get the byte offset in a given parameter category. */
IMPORT __attribute__((import_name("slang_var_layout_get_offset")))
int32_t slang_var_layout_get_offset(slang_variable_layout_t layout, int32_t category);

/** Get the binding index (register/binding number). */
IMPORT __attribute__((import_name("slang_var_layout_get_binding_index")))
int32_t slang_var_layout_get_binding_index(slang_variable_layout_t layout);

/** Get the binding space (register space / descriptor set). */
IMPORT __attribute__((import_name("slang_var_layout_get_binding_space")))
int32_t slang_var_layout_get_binding_space(slang_variable_layout_t layout);

/** Get the space offset for a given parameter category. */
IMPORT __attribute__((import_name("slang_var_layout_get_space")))
int32_t slang_var_layout_get_space(slang_variable_layout_t layout, int32_t category);

/** Get the semantic name. */
IMPORT __attribute__((import_name("slang_var_layout_get_semantic_name")))
int32_t slang_var_layout_get_semantic_name(slang_variable_layout_t layout,
                                            void *dst_ptr, int32_t max_len);

/** Get the semantic index. */
IMPORT __attribute__((import_name("slang_var_layout_get_semantic_index")))
int32_t slang_var_layout_get_semantic_index(slang_variable_layout_t layout);

/** Get the shader stage this variable belongs to (SlangStage). */
IMPORT __attribute__((import_name("slang_var_layout_get_stage")))
int32_t slang_var_layout_get_stage(slang_variable_layout_t layout);

/* ================================================================== */
/*  Reflection — EntryPointReflection                                  */
/* ================================================================== */

/** Get the entry point name. */
IMPORT __attribute__((import_name("slang_ep_layout_get_name")))
int32_t slang_ep_layout_get_name(slang_ep_layout_t ep, void *dst_ptr, int32_t max_len);

/** Get the entry point name override (for renamed entry points). */
IMPORT __attribute__((import_name("slang_ep_layout_get_name_override")))
int32_t slang_ep_layout_get_name_override(slang_ep_layout_t ep, void *dst_ptr, int32_t max_len);

/** Get the shader stage (SlangStage). */
IMPORT __attribute__((import_name("slang_ep_layout_get_stage")))
int32_t slang_ep_layout_get_stage(slang_ep_layout_t ep);

/** Get the number of parameters for this entry point. */
IMPORT __attribute__((import_name("slang_ep_layout_get_parameter_count")))
int32_t slang_ep_layout_get_parameter_count(slang_ep_layout_t ep);

/** Get a parameter by index.
 *  @return Variable layout handle. */
IMPORT __attribute__((import_name("slang_ep_layout_get_parameter_by_index")))
slang_variable_layout_t slang_ep_layout_get_parameter_by_index(slang_ep_layout_t ep,
                                                                int32_t index);

/** Get the compute thread group size.
 *  Writes 3 int32_t values (x, y, z) to *out_ptr. */
IMPORT __attribute__((import_name("slang_ep_layout_get_compute_thread_group_size")))
void slang_ep_layout_get_compute_thread_group_size(slang_ep_layout_t ep, int32_t *out_ptr);

/** Get the compute wave size.
 *  Writes 3 int32_t values to *out_ptr. */
IMPORT __attribute__((import_name("slang_ep_layout_get_compute_wave_size")))
void slang_ep_layout_get_compute_wave_size(slang_ep_layout_t ep, int32_t *out_ptr);

/** Check if this entry point uses any sample-rate inputs. Returns 1 or 0. */
IMPORT __attribute__((import_name("slang_ep_layout_uses_any_sample_rate_input")))
int32_t slang_ep_layout_uses_any_sample_rate_input(slang_ep_layout_t ep);

/** Check if this entry point has a default constant buffer. Returns 1 or 0. */
IMPORT __attribute__((import_name("slang_ep_layout_has_default_constant_buffer")))
int32_t slang_ep_layout_has_default_constant_buffer(slang_ep_layout_t ep);

/** Get the result variable layout (return value layout). */
IMPORT __attribute__((import_name("slang_ep_layout_get_result_var_layout")))
slang_variable_layout_t slang_ep_layout_get_result_var_layout(slang_ep_layout_t ep);

/** Get the function reflection for this entry point. */
IMPORT __attribute__((import_name("slang_ep_layout_get_function")))
slang_func_reflection_t slang_ep_layout_get_function(slang_ep_layout_t ep);

/* ================================================================== */
/*  Reflection — FunctionReflection                                    */
/* ================================================================== */

/** Get the function name. */
IMPORT __attribute__((import_name("slang_func_get_name")))
int32_t slang_func_get_name(slang_func_reflection_t func, void *dst_ptr, int32_t max_len);

/** Get the return type. */
IMPORT __attribute__((import_name("slang_func_get_result_type")))
slang_type_t slang_func_get_result_type(slang_func_reflection_t func);

/** Get the number of parameters. */
IMPORT __attribute__((import_name("slang_func_get_parameter_count")))
int32_t slang_func_get_parameter_count(slang_func_reflection_t func);

/** Get a parameter by index.
 *  @return Variable reflection handle. */
IMPORT __attribute__((import_name("slang_func_get_parameter")))
slang_variable_t slang_func_get_parameter(slang_func_reflection_t func, int32_t index);

/** Get the number of user-defined attributes. */
IMPORT __attribute__((import_name("slang_func_get_user_attribute_count")))
int32_t slang_func_get_user_attribute_count(slang_func_reflection_t func);

/** Get a user-defined attribute by index. */
IMPORT __attribute__((import_name("slang_func_get_user_attribute")))
slang_user_attribute_t slang_func_get_user_attribute(slang_func_reflection_t func, int32_t index);

/** Check if this function is overloaded. Returns 1 or 0. */
IMPORT __attribute__((import_name("slang_func_is_overloaded")))
int32_t slang_func_is_overloaded(slang_func_reflection_t func);

/** Get the number of overloads. */
IMPORT __attribute__((import_name("slang_func_get_overload_count")))
int32_t slang_func_get_overload_count(slang_func_reflection_t func);

/** Get an overload by index. */
IMPORT __attribute__((import_name("slang_func_get_overload")))
slang_func_reflection_t slang_func_get_overload(slang_func_reflection_t func, int32_t index);

/** Get the generic container for this function. */
IMPORT __attribute__((import_name("slang_func_get_generic_container")))
slang_generic_reflection_t slang_func_get_generic_container(slang_func_reflection_t func);

/* ================================================================== */
/*  Reflection — GenericReflection                                     */
/* ================================================================== */

/** Get the declaration underlying this generic. */
IMPORT __attribute__((import_name("slang_generic_as_decl")))
slang_decl_reflection_t slang_generic_as_decl(slang_generic_reflection_t gen);

/** Get the generic name. */
IMPORT __attribute__((import_name("slang_generic_get_name")))
int32_t slang_generic_get_name(slang_generic_reflection_t gen, void *dst_ptr, int32_t max_len);

/** Get the number of type parameters. */
IMPORT __attribute__((import_name("slang_generic_get_type_parameter_count")))
int32_t slang_generic_get_type_parameter_count(slang_generic_reflection_t gen);

/** Get a type parameter by index.
 *  @return Variable reflection handle. */
IMPORT __attribute__((import_name("slang_generic_get_type_parameter")))
slang_variable_t slang_generic_get_type_parameter(slang_generic_reflection_t gen, int32_t index);

/** Get the number of value parameters. */
IMPORT __attribute__((import_name("slang_generic_get_value_parameter_count")))
int32_t slang_generic_get_value_parameter_count(slang_generic_reflection_t gen);

/** Get a value parameter by index.
 *  @return Variable reflection handle. */
IMPORT __attribute__((import_name("slang_generic_get_value_parameter")))
slang_variable_t slang_generic_get_value_parameter(slang_generic_reflection_t gen, int32_t index);

/** Get the number of type parameter constraints.
 *  @param type_param_index  Handle to a type parameter variable (from slang_generic_get_type_parameter). */
IMPORT __attribute__((import_name("slang_generic_get_type_param_constraint_count")))
int32_t slang_generic_get_type_param_constraint_count(slang_generic_reflection_t gen,
                                                       slang_variable_t type_param);

/** Get a constraint type by index.
 *  @param type_param  Handle to a type parameter variable.
 *  @param index       Constraint index. */
IMPORT __attribute__((import_name("slang_generic_get_type_param_constraint_type")))
slang_type_t slang_generic_get_type_param_constraint_type(slang_generic_reflection_t gen,
                                                           slang_variable_t type_param,
                                                           int32_t index);

/** Get the inner declaration. */
IMPORT __attribute__((import_name("slang_generic_get_inner_decl")))
slang_decl_reflection_t slang_generic_get_inner_decl(slang_generic_reflection_t gen);

/** Get the inner kind (SlangDeclKind). */
IMPORT __attribute__((import_name("slang_generic_get_inner_kind")))
int32_t slang_generic_get_inner_kind(slang_generic_reflection_t gen);

/** Get the outer generic container. */
IMPORT __attribute__((import_name("slang_generic_get_outer_generic_container")))
slang_generic_reflection_t slang_generic_get_outer_generic_container(
    slang_generic_reflection_t gen);

/* ================================================================== */
/*  Reflection — DeclReflection                                        */
/* ================================================================== */

/** Get the declaration name. */
IMPORT __attribute__((import_name("slang_decl_get_name")))
int32_t slang_decl_get_name(slang_decl_reflection_t decl, void *dst_ptr, int32_t max_len);

/** Get the declaration kind (SlangDeclKind). */
IMPORT __attribute__((import_name("slang_decl_get_kind")))
int32_t slang_decl_get_kind(slang_decl_reflection_t decl);

/** Get the number of child declarations. */
IMPORT __attribute__((import_name("slang_decl_get_children_count")))
int32_t slang_decl_get_children_count(slang_decl_reflection_t decl);

/** Get a child declaration by index. */
IMPORT __attribute__((import_name("slang_decl_get_child")))
slang_decl_reflection_t slang_decl_get_child(slang_decl_reflection_t decl, int32_t index);

/** Get the type of this declaration. */
IMPORT __attribute__((import_name("slang_decl_get_type")))
slang_type_t slang_decl_get_type(slang_decl_reflection_t decl);

/** Cast this declaration to a variable (returns 0 if not a variable). */
IMPORT __attribute__((import_name("slang_decl_cast_to_variable")))
slang_variable_t slang_decl_cast_to_variable(slang_decl_reflection_t decl);

/** Cast this declaration to a function (returns 0 if not a function). */
IMPORT __attribute__((import_name("slang_decl_cast_to_function")))
slang_func_reflection_t slang_decl_cast_to_function(slang_decl_reflection_t decl);

/** Cast this declaration to a generic (returns 0 if not a generic). */
IMPORT __attribute__((import_name("slang_decl_cast_to_generic")))
slang_generic_reflection_t slang_decl_cast_to_generic(slang_decl_reflection_t decl);

/** Get the parent declaration. */
IMPORT __attribute__((import_name("slang_decl_get_parent")))
slang_decl_reflection_t slang_decl_get_parent(slang_decl_reflection_t decl);

/* ================================================================== */
/*  Reflection — UserAttribute                                         */
/* ================================================================== */

/** Get the attribute name. */
IMPORT __attribute__((import_name("slang_attr_get_name")))
int32_t slang_attr_get_name(slang_user_attribute_t attr, void *dst_ptr, int32_t max_len);

/** Get the number of arguments. */
IMPORT __attribute__((import_name("slang_attr_get_argument_count")))
int32_t slang_attr_get_argument_count(slang_user_attribute_t attr);

/** Get the type of an argument by index. */
IMPORT __attribute__((import_name("slang_attr_get_argument_type")))
slang_type_t slang_attr_get_argument_type(slang_user_attribute_t attr, int32_t index);

/** Get an integer argument value. Writes to *out_ptr. Returns SLANG_OK or error. */
IMPORT __attribute__((import_name("slang_attr_get_argument_value_int")))
int32_t slang_attr_get_argument_value_int(slang_user_attribute_t attr,
                                           int32_t index, int32_t *out_ptr);

/** Get a float argument value. Writes to *out_ptr. Returns SLANG_OK or error. */
IMPORT __attribute__((import_name("slang_attr_get_argument_value_float")))
int32_t slang_attr_get_argument_value_float(slang_user_attribute_t attr,
                                             int32_t index, float *out_ptr);

/** Get a string argument value. */
IMPORT __attribute__((import_name("slang_attr_get_argument_value_string")))
int32_t slang_attr_get_argument_value_string(slang_user_attribute_t attr,
                                              int32_t index,
                                              void *dst_ptr, int32_t max_len);

#undef IMPORT

#endif /* WASM_SLANG_H */
