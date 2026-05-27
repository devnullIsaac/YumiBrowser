/*
 * wgpu_annotated.h - wgpu-native extensions to the WebGPU C API: push constants, SPIR-V passthrough, polygon modes, multi-draw indirect, pipeline statistics, backend selection.
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
 * @file wgpu.h
 * @brief wgpu-native extension header for the WebGPU C API.
 *
 * This header extends the standard WebGPU specification (webgpu.h) with
 * wgpu-native–specific types, enumerations, structures, and functions.
 * It exposes capabilities that the upstream WebGPU spec intentionally omits
 * (push constants, SPIR-V passthrough, polygon modes, multi-draw indirect,
 * pipeline statistics, etc.) and provides control over backend selection,
 * instance configuration, logging, and device polling.
 *
 * @par Rendering Pipeline Overview
 * The WebGPU rendering pipeline works roughly as follows:
 *
 * 1. **Instance Creation** — An @ref WGPUInstance is created (optionally with
 *    @ref WGPUInstanceExtras chained in to select backends and flags).
 * 2. **Adapter Enumeration** — Physical GPUs are discovered via
 *    @ref wgpuInstanceEnumerateAdapters or `wgpuInstanceRequestAdapter`.
 * 3. **Device & Queue** — A logical device and its default queue are obtained
 *    from the chosen adapter. The device is the central object that creates
 *    every other GPU resource.
 * 4. **Resource Creation** — Buffers, textures, samplers, bind groups,
 *    shader modules, and pipeline objects are created from the device.
 * 5. **Command Encoding** — A @ref WGPUCommandEncoder records render and
 *    compute passes. Inside a render pass you bind pipelines, vertex/index
 *    buffers, bind groups, set push constants, and issue draw calls.
 * 6. **Submission** — Finished command buffers are submitted to the queue
 *    (optionally via @ref wgpuQueueSubmitForIndex to get a trackable index).
 * 7. **Presentation** — If rendering to a surface, the swapchain texture is
 *    presented. @ref wgpuDevicePoll can be used to block or check completion.
 *
 * @par How This Header Fits In
 * ```
 * ┌──────────────────────────────────────────────────┐
 * │                  Application                     │
 * ├──────────────────────────────────────────────────┤
 * │   webgpu.h  (spec-compliant API)                 │
 * │   wgpu.h    (native extensions – THIS FILE)      │
 * ├──────────────────────────────────────────────────┤
 * │         wgpu-native runtime (Rust)               │
 * ├────────┬────────┬────────┬────────┬──────────────┤
 * │ Vulkan │ Metal  │ DX12   │ DX11   │ OpenGL / ES  │
 * └────────┴────────┴────────┴────────┴──────────────┘
 * ```
 *
 * @note All enum values in the `0x0003xxxx` range are allocated to
 *       wgpu-native so they never collide with upstream WebGPU constants.
 */

#ifndef WGPU_H_
#define WGPU_H_

#include "webgpu.h"

/* ========================================================================= */
/*  Enumerations                                                             */
/* ========================================================================= */

/**
 * @enum WGPUNativeSType
 * @brief Structure type tags used in the `sType` field of chained structs.
 *
 * When you chain a wgpu-native extension struct onto a standard WebGPU
 * descriptor, you set its `chain.sType` to one of these values so the
 * runtime knows how to interpret the memory.
 *
 * @par Example – chaining instance extras
 * @code
 * WGPUInstanceExtras extras = {
 *     .chain = {
 *         .next  = NULL,
 *         .sType = WGPUSType_InstanceExtras,
 *     },
 *     .backends = WGPUInstanceBackend_Vulkan,
 *     .flags    = WGPUInstanceFlag_Validation,
 * };
 *
 * WGPUInstanceDescriptor desc = {
 *     .nextInChain = (const WGPUChainedStruct *)&extras,
 * };
 * WGPUInstance instance = wgpuCreateInstance(&desc);
 * @endcode
 */
typedef enum WGPUNativeSType {
    WGPUSType_DeviceExtras                = 0x00030001, /**< Identifies @ref WGPUDeviceExtras. */
    WGPUSType_NativeLimits                = 0x00030002, /**< Identifies @ref WGPUNativeLimits. */
    WGPUSType_PipelineLayoutExtras        = 0x00030003, /**< Identifies @ref WGPUPipelineLayoutExtras (push constant ranges). */
    WGPUSType_ShaderSourceGLSL            = 0x00030004, /**< Identifies @ref WGPUShaderSourceGLSL. */
    WGPUSType_InstanceExtras              = 0x00030006, /**< Identifies @ref WGPUInstanceExtras. */
    WGPUSType_BindGroupEntryExtras        = 0x00030007, /**< Identifies @ref WGPUBindGroupEntryExtras (binding arrays). */
    WGPUSType_BindGroupLayoutEntryExtras  = 0x00030008, /**< Identifies @ref WGPUBindGroupLayoutEntryExtras. */
    WGPUSType_QuerySetDescriptorExtras    = 0x00030009, /**< Identifies @ref WGPUQuerySetDescriptorExtras (pipeline statistics). */
    WGPUSType_SurfaceConfigurationExtras  = 0x0003000A, /**< Identifies @ref WGPUSurfaceConfigurationExtras. */
    WGPUSType_SurfaceSourceSwapChainPanel = 0x0003000B, /**< Identifies @ref WGPUSurfaceSourceSwapChainPanel (WinUI). */
    WGPUSType_PrimitiveStateExtras        = 0x0003000C, /**< Identifies @ref WGPUPrimitiveStateExtras (polygon mode). */
    WGPUNativeSType_Force32               = 0x7FFFFFFF  /**< Force enum to 32 bits. */
} WGPUNativeSType;

/**
 * @enum WGPUNativeFeature
 * @brief Feature flags beyond the core WebGPU specification.
 *
 * Request these when creating a device (in the `requiredFeatures` array) to
 * unlock native-only functionality.  The adapter must report the feature as
 * available or device creation will fail.
 *
 * @par Categories
 * | Category               | Features                                                       |
 * |------------------------|----------------------------------------------------------------|
 * | Push constants         | @ref WGPUNativeFeature_PushConstants                           |
 * | Binding arrays         | TextureBindingArray, BufferBindingArray, StorageResource…       |
 * | Draw indirect          | MultiDrawIndirectCount                                         |
 * | Polygon modes          | PolygonModeLine, PolygonModePoint, ConservativeRasterization    |
 * | Shader extensions      | ShaderF64, ShaderI16, ShaderInt64, ShaderPrimitiveIndex, …      |
 * | Subgroups              | Subgroup, SubgroupVertex, SubgroupBarrier                      |
 * | Texture formats        | TextureFormat16bitNorm, TextureFormatNv12                      |
 * | Timestamps in passes   | TimestampQueryInsideEncoders, TimestampQueryInsidePasses        |
 * | Misc                   | VertexWritableStorage, SpirvShaderPassthrough, RayQuery, …     |
 */
typedef enum WGPUNativeFeature {
    /** Enables push constants via @ref WGPUPipelineLayoutExtras and the
     *  `wgpu*SetPushConstants` family of functions. */
    WGPUNativeFeature_PushConstants                                          = 0x00030001,
    /** Allows querying adapter-specific texture format capabilities. */
    WGPUNativeFeature_TextureAdapterSpecificFormatFeatures                   = 0x00030002,
    /** Enables @ref wgpuRenderPassEncoderMultiDrawIndirectCount and its
     *  indexed variant, reading draw count from a GPU buffer. */
    WGPUNativeFeature_MultiDrawIndirectCount                                 = 0x00030004,
    /** Permits vertex shaders to write to storage buffers / textures. */
    WGPUNativeFeature_VertexWritableStorage                                  = 0x00030005,
    /** Allows arrays of sampled textures in bind groups. */
    WGPUNativeFeature_TextureBindingArray                                    = 0x00030006,
    /** Non-uniform indexing into sampled-texture + storage-buffer arrays. */
    WGPUNativeFeature_SampledTextureAndStorageBufferArrayNonUniformIndexing   = 0x00030007,
    /** Enables pipeline statistics queries (vertex, fragment, compute invocations, etc.). */
    WGPUNativeFeature_PipelineStatisticsQuery                                = 0x00030008,
    /** Allows arrays of storage resources in bind groups. */
    WGPUNativeFeature_StorageResourceBindingArray                            = 0x00030009,
    /** Bind group arrays may be only partially bound. */
    WGPUNativeFeature_PartiallyBoundBindingArray                             = 0x0003000A,
    /** Unlocks R16/RG16/RGBA16 Unorm and Snorm texture formats. */
    WGPUNativeFeature_TextureFormat16bitNorm                                 = 0x0003000B,
    /** ASTC HDR texture compression. */
    WGPUNativeFeature_TextureCompressionAstcHdr                              = 0x0003000C,
    /** Buffers used as map targets may also be used as primaries. */
    WGPUNativeFeature_MappablePrimaryBuffers                                 = 0x0003000E,
    /** Allows arrays of uniform / storage buffers in bind groups. */
    WGPUNativeFeature_BufferBindingArray                                     = 0x0003000F,
    /** Non-uniform indexing into uniform-buffer + storage-texture arrays. */
    WGPUNativeFeature_UniformBufferAndStorageTextureArrayNonUniformIndexing   = 0x00030010,
    /** Wireframe rendering – fills polygons as lines. */
    WGPUNativeFeature_PolygonModeLine                                        = 0x00030013,
    /** Point rendering – fills polygons as points. */
    WGPUNativeFeature_PolygonModePoint                                       = 0x00030014,
    /** Conservative rasterization – rasterises any pixel a primitive touches. */
    WGPUNativeFeature_ConservativeRasterization                              = 0x00030015,
    /** Allows loading pre-compiled SPIR-V directly (bypasses WGSL/naga). */
    WGPUNativeFeature_SpirvShaderPassthrough                                 = 0x00030017,
    /** 64-bit vertex attributes (e.g. dvec2, dvec3). */
    WGPUNativeFeature_VertexAttribute64bit                                   = 0x00030019,
    /** NV12 planar video texture format. */
    WGPUNativeFeature_TextureFormatNv12                                      = 0x0003001A,
    /** Hardware ray-query support. */
    WGPUNativeFeature_RayQuery                                               = 0x0003001C,
    /** 64-bit floating-point in shaders. */
    WGPUNativeFeature_ShaderF64                                              = 0x0003001D,
    /** 16-bit integer in shaders. */
    WGPUNativeFeature_ShaderI16                                              = 0x0003001E,
    /** Access to `gl_PrimitiveID` in fragment shaders. */
    WGPUNativeFeature_ShaderPrimitiveIndex                                   = 0x0003001F,
    /** Early depth test attribute in fragment shaders. */
    WGPUNativeFeature_ShaderEarlyDepthTest                                   = 0x00030020,
    /** Subgroup operations in compute shaders. */
    WGPUNativeFeature_Subgroup                                               = 0x00030021,
    /** Subgroup operations in vertex shaders. */
    WGPUNativeFeature_SubgroupVertex                                         = 0x00030022,
    /** Subgroup barrier intrinsics. */
    WGPUNativeFeature_SubgroupBarrier                                        = 0x00030023,
    /** Timestamp queries inside command encoders (outside passes). */
    WGPUNativeFeature_TimestampQueryInsideEncoders                           = 0x00030024,
    /** Timestamp queries inside render/compute passes. */
    WGPUNativeFeature_TimestampQueryInsidePasses                             = 0x00030025,
    /** Full 64-bit integer support in shaders. */
    WGPUNativeFeature_ShaderInt64                                            = 0x00030026,
    WGPUNativeFeature_Force32                                                = 0x7FFFFFFF
} WGPUNativeFeature;

/**
 * @enum WGPULogLevel
 * @brief Severity levels for the wgpu-native logging callback.
 *
 * Set with @ref wgpuSetLogLevel and received by @ref WGPULogCallback.
 * Levels are ordered from most severe (Error) to most verbose (Trace).
 */
typedef enum WGPULogLevel {
    WGPULogLevel_Off    = 0x00000000, /**< Logging disabled. */
    WGPULogLevel_Error  = 0x00000001, /**< Unrecoverable errors. */
    WGPULogLevel_Warn   = 0x00000002, /**< Potential issues. */
    WGPULogLevel_Info   = 0x00000003, /**< Informational messages. */
    WGPULogLevel_Debug  = 0x00000004, /**< Debug-level detail. */
    WGPULogLevel_Trace  = 0x00000005, /**< Maximum verbosity. */
    WGPULogLevel_Force32 = 0x7FFFFFFF
} WGPULogLevel;

/* ========================================================================= */
/*  Backend & Instance Flags                                                 */
/* ========================================================================= */

/**
 * @typedef WGPUInstanceBackend
 * @brief Bitmask selecting which GPU backends the instance may use.
 *
 * Pass one or more of these flags in @ref WGPUInstanceExtras::backends to
 * restrict which native APIs wgpu-native will attempt to load.
 *
 * @par Predefined Combinations
 * - **Primary** = Vulkan | Metal | DX12 | BrowserWebGPU — production-quality.
 * - **Secondary** = GL | DX11 — broader compatibility, less capability.
 * - **All** (0) = try everything.
 */
typedef WGPUFlags WGPUInstanceBackend;
static const WGPUInstanceBackend WGPUInstanceBackend_All           = 0x00000000;
static const WGPUInstanceBackend WGPUInstanceBackend_Vulkan        = 1 << 0;
static const WGPUInstanceBackend WGPUInstanceBackend_GL            = 1 << 1;
static const WGPUInstanceBackend WGPUInstanceBackend_Metal         = 1 << 2;
static const WGPUInstanceBackend WGPUInstanceBackend_DX12          = 1 << 3;
static const WGPUInstanceBackend WGPUInstanceBackend_DX11          = 1 << 4;
static const WGPUInstanceBackend WGPUInstanceBackend_BrowserWebGPU = 1 << 5;
/** Vulkan + Metal + DX12 + BrowserWebGPU. */
static const WGPUInstanceBackend WGPUInstanceBackend_Primary       = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5);
/** GL + DX11. */
static const WGPUInstanceBackend WGPUInstanceBackend_Secondary     = (1 << 1) | (1 << 4);
static const WGPUInstanceBackend WGPUInstanceBackend_Force32       = 0x7FFFFFFF;

/**
 * @typedef WGPUInstanceFlag
 * @brief Bitmask of instance-level behaviour toggles.
 *
 * - **Debug**: enables backend debug layers (e.g. Vulkan validation layers).
 * - **Validation**: enables wgpu's own internal validation on top of the
 *   backend's.  Useful for catching API misuse.
 * - **DiscardHalLabels**: strips debug labels before sending them to the HAL,
 *   slightly reducing overhead in release builds.
 */
typedef WGPUFlags WGPUInstanceFlag;
static const WGPUInstanceFlag WGPUInstanceFlag_Default          = 0x00000000;
static const WGPUInstanceFlag WGPUInstanceFlag_Debug            = 1 << 0;
static const WGPUInstanceFlag WGPUInstanceFlag_Validation       = 1 << 1;
static const WGPUInstanceFlag WGPUInstanceFlag_DiscardHalLabels = 1 << 2;
static const WGPUInstanceFlag WGPUInstanceFlag_Force32          = 0x7FFFFFFF;

/* ========================================================================= */
/*  DX12 / GLES Enumerations                                                */
/* ========================================================================= */

/**
 * @enum WGPUDx12Compiler
 * @brief Selects the HLSL shader compiler on DirectX 12.
 *
 * - **Fxc**: Legacy compiler. Always available on Windows.
 * - **Dxc**: Modern compiler. Required for Shader Model 6.0+.
 *   The path to dxcompiler.dll can be set in @ref WGPUInstanceExtras::dxcPath.
 */
typedef enum WGPUDx12Compiler {
    WGPUDx12Compiler_Undefined = 0x00000000,
    WGPUDx12Compiler_Fxc      = 0x00000001,
    WGPUDx12Compiler_Dxc      = 0x00000002,
    WGPUDx12Compiler_Force32  = 0x7FFFFFFF
} WGPUDx12Compiler;

/**
 * @enum WGPUGles3MinorVersion
 * @brief Forces a specific OpenGL ES 3.x minor version.
 *
 * By default (`Automatic`) wgpu-native probes the driver. Override when the
 * driver misreports its version.
 */
typedef enum WGPUGles3MinorVersion {
    WGPUGles3MinorVersion_Automatic = 0x00000000,
    WGPUGles3MinorVersion_Version0  = 0x00000001, /**< Force GLES 3.0. */
    WGPUGles3MinorVersion_Version1  = 0x00000002, /**< Force GLES 3.1. */
    WGPUGles3MinorVersion_Version2  = 0x00000003, /**< Force GLES 3.2. */
    WGPUGles3MinorVersion_Force32   = 0x7FFFFFFF
} WGPUGles3MinorVersion;

/**
 * @enum WGPUPipelineStatisticName
 * @brief Individual pipeline statistics that can be queried.
 *
 * Used inside @ref WGPUQuerySetDescriptorExtras to specify which counters
 * a pipeline statistics query set should record.
 */
typedef enum WGPUPipelineStatisticName {
    WGPUPipelineStatisticName_VertexShaderInvocations   = 0x00000000, /**< Number of vertex shader runs. */
    WGPUPipelineStatisticName_ClipperInvocations        = 0x00000001, /**< Primitives entering the clipper. */
    WGPUPipelineStatisticName_ClipperPrimitivesOut      = 0x00000002, /**< Primitives surviving clipping. */
    WGPUPipelineStatisticName_FragmentShaderInvocations = 0x00000003, /**< Number of fragment shader runs. */
    WGPUPipelineStatisticName_ComputeShaderInvocations  = 0x00000004, /**< Number of compute shader runs. */
    WGPUPipelineStatisticName_Force32                   = 0x7FFFFFFF
} WGPUPipelineStatisticName WGPU_ENUM_ATTRIBUTE;

/**
 * @enum WGPUNativeQueryType
 * @brief Native query type for pipeline statistics.
 *
 * Passed as the query type when creating a query set that records
 * pipeline statistics (as opposed to timestamp or occlusion queries).
 */
typedef enum WGPUNativeQueryType {
    WGPUNativeQueryType_PipelineStatistics = 0x00030000,
    WGPUNativeQueryType_Force32            = 0x7FFFFFFF
} WGPUNativeQueryType WGPU_ENUM_ATTRIBUTE;

/**
 * @enum WGPUDxcMaxShaderModel
 * @brief Maximum DXC shader model version to allow.
 *
 * Limits the shader model that wgpu-native will target when compiling
 * HLSL with DXC.  Useful when you know your minimum hardware target.
 */
typedef enum WGPUDxcMaxShaderModel {
    WGPUDxcMaxShaderModel_V6_0   = 0x00000000,
    WGPUDxcMaxShaderModel_V6_1   = 0x00000001,
    WGPUDxcMaxShaderModel_V6_2   = 0x00000002,
    WGPUDxcMaxShaderModel_V6_3   = 0x00000003,
    WGPUDxcMaxShaderModel_V6_4   = 0x00000004,
    WGPUDxcMaxShaderModel_V6_5   = 0x00000005,
    WGPUDxcMaxShaderModel_V6_6   = 0x00000006,
    WGPUDxcMaxShaderModel_V6_7   = 0x00000007,
    WGPUDxcMaxShaderModel_Force32 = 0x7FFFFFFF
} WGPUDxcMaxShaderModel;

/**
 * @enum WGPUGLFenceBehaviour
 * @brief Controls OpenGL fence synchronization behaviour.
 *
 * - **Normal**: standard glFenceSync / glClientWaitSync.
 * - **AutoFinish**: implicitly finishes the GL command stream, working
 *   around drivers that don't implement fences correctly.
 */
typedef enum WGPUGLFenceBehaviour {
    WGPUGLFenceBehaviour_Normal     = 0x00000000,
    WGPUGLFenceBehaviour_AutoFinish = 0x00000001,
    WGPUGLFenceBehaviour_Force32    = 0x7FFFFFFF
} WGPUGLFenceBehaviour;

/**
 * @enum WGPUDx12SwapchainKind
 * @brief Selects the DX12 presentation / swap chain creation strategy.
 *
 * - **DxgiFromHwnd**: classic Win32 HWND-based swap chain.
 * - **DxgiFromVisual**: composition-based (used with WinUI / XAML visuals).
 */
typedef enum WGPUDx12SwapchainKind {
    WGPUDx12SwapchainKind_Undefined      = 0x00000000,
    WGPUDx12SwapchainKind_DxgiFromHwnd   = 0x00000001,
    WGPUDx12SwapchainKind_DxgiFromVisual = 0x00000002,
    WGPUDx12SwapchainKind_Force32        = 0x7FFFFFFF
} WGPUDx12SwapchainKind;

/**
 * @enum WGPUPolygonMode
 * @brief How polygon primitives are rasterized.
 *
 * Chained onto `WGPUPrimitiveState` via @ref WGPUPrimitiveStateExtras.
 * Requires @ref WGPUNativeFeature_PolygonModeLine or
 * @ref WGPUNativeFeature_PolygonModePoint for non-fill modes.
 */
typedef enum WGPUPolygonMode {
    WGPUPolygonMode_Fill  = 0, /**< Default solid fill. */
    WGPUPolygonMode_Line  = 1, /**< Wireframe (edges only). */
    WGPUPolygonMode_Point = 2, /**< Vertices only. */
} WGPUPolygonMode;

/**
 * @enum WGPUNativeTextureFormat
 * @brief Additional texture formats not in the core WebGPU specification.
 *
 * Requires the corresponding native feature to be enabled on the device
 * (e.g. @ref WGPUNativeFeature_TextureFormat16bitNorm for the R16/RG16/RGBA16
 * variants).
 */
typedef enum WGPUNativeTextureFormat {
    WGPUNativeTextureFormat_R16Unorm    = 0x00030001,
    WGPUNativeTextureFormat_R16Snorm    = 0x00030002,
    WGPUNativeTextureFormat_Rg16Unorm   = 0x00030003,
    WGPUNativeTextureFormat_Rg16Snorm   = 0x00030004,
    WGPUNativeTextureFormat_Rgba16Unorm = 0x00030005,
    WGPUNativeTextureFormat_Rgba16Snorm = 0x00030006,
    WGPUNativeTextureFormat_NV12        = 0x00030007, /**< YCbCr 4:2:0 planar (requires TextureFormatNv12). */
    WGPUNativeTextureFormat_P010        = 0x00030008, /**< 10-bit YCbCr 4:2:0 planar. */
} WGPUNativeTextureFormat;


/* ========================================================================= */
/*  Structures                                                               */
/* ========================================================================= */

/**
 * @struct WGPUInstanceExtras
 * @brief Extended instance configuration – chain onto WGPUInstanceDescriptor.
 *
 * This is the most important native extension struct. It lets you:
 * - Choose which GPU backends to probe (@ref backends).
 * - Enable debug / validation layers (@ref flags).
 * - Select the DX12 shader compiler and its maximum shader model.
 * - Override the OpenGL ES minor version.
 * - Configure OpenGL fence behaviour.
 * - Set DX12 presentation strategy.
 *
 * @par Chaining
 * Set `chain.sType = WGPUSType_InstanceExtras` and pass the struct's
 * address as `WGPUInstanceDescriptor.nextInChain`.
 */
typedef struct WGPUInstanceExtras {
    WGPUChainedStruct chain;                    /**< Must have sType = WGPUSType_InstanceExtras. */
    WGPUInstanceBackend backends;               /**< Bitmask of backends to enable (0 = all). */
    WGPUInstanceFlag flags;                     /**< Debug / validation flags. */
    WGPUDx12Compiler dx12ShaderCompiler;        /**< FXC or DXC for DX12 HLSL compilation. */
    WGPUGles3MinorVersion gles3MinorVersion;    /**< Override for GLES 3.x minor version. */
    WGPUGLFenceBehaviour glFenceBehaviour;       /**< GL fence workaround mode. */
    WGPUStringView dxcPath;                     /**< Filesystem path to dxcompiler.dll (DXC only). */
    WGPUDxcMaxShaderModel dxcMaxShaderModel;    /**< Cap on target shader model for DXC. */
    WGPUDx12SwapchainKind dx12PresentationSystem; /**< DX12 swap chain creation strategy. */

    WGPU_NULLABLE const uint8_t* budgetForDeviceCreation; /**< Optional memory budget hint for device creation. */
    WGPU_NULLABLE const uint8_t* budgetForDeviceLoss;     /**< Optional memory budget hint on device loss recovery. */
} WGPUInstanceExtras;

/**
 * @struct WGPUDeviceExtras
 * @brief Extended device configuration – chain onto WGPUDeviceDescriptor.
 *
 * Currently allows setting a file-system path for GPU API trace capture,
 * which is extremely useful for debugging rendering issues.
 *
 * @par Example
 * @code
 * WGPUDeviceExtras devExtras = {
 *     .chain     = { .sType = WGPUSType_DeviceExtras },
 *     .tracePath = { .data = "./gpu_trace", .length = 11 },
 * };
 * @endcode
 */
typedef struct WGPUDeviceExtras {
    WGPUChainedStruct chain;  /**< Must have sType = WGPUSType_DeviceExtras. */
    WGPUStringView tracePath; /**< Directory for API trace output (empty = disabled). */
} WGPUDeviceExtras;

/**
 * @struct WGPUNativeLimits
 * @brief Additional device limits beyond the core WebGPU set.
 *
 * Chain onto `WGPUSupportedLimits` (output) or `WGPURequiredLimits` (input)
 * to query or request these native-only limits.
 *
 * @note Uses `WGPUChainedStructOut` because the same struct may appear in
 *       both mutable (adapter query) and immutable (device request) contexts.
 */
typedef struct WGPUNativeLimits {
    WGPUChainedStructOut chain;                      /**< sType = WGPUSType_NativeLimits. */
    uint32_t maxPushConstantSize;                    /**< Maximum push constant block size in bytes. */
    uint32_t maxNonSamplerBindings;                  /**< Maximum total non-sampler bindings per pipeline. */
    uint32_t maxBindingArrayElementsPerShaderStage;  /**< Maximum elements in a binding array per stage. */
} WGPUNativeLimits;

/**
 * @struct WGPUPushConstantRange
 * @brief Describes one contiguous push-constant range visible to specific
 *        shader stages.
 *
 * Push constants are a small, fast-path mechanism for uploading a handful of
 * bytes (typically 128–256) to shaders each draw/dispatch without going
 * through a buffer bind.
 *
 * @par Alignment
 * `start` and `end` must be multiples of 4.  The maximum range size is
 * governed by @ref WGPUNativeLimits::maxPushConstantSize.
 */
typedef struct WGPUPushConstantRange {
    WGPUShaderStage stages; /**< Shader stages that can read this range. */
    uint32_t start;         /**< Byte offset of the range start (4-byte aligned). */
    uint32_t end;           /**< Byte offset past the range end (4-byte aligned). */
} WGPUPushConstantRange;

/**
 * @struct WGPUPipelineLayoutExtras
 * @brief Adds push-constant ranges to a pipeline layout.
 *
 * Chain onto `WGPUPipelineLayoutDescriptor.nextInChain`.
 *
 * @par Example
 * @code
 * WGPUPushConstantRange range = {
 *     .stages = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
 *     .start  = 0,
 *     .end    = 64,  // 64 bytes of push constants
 * };
 * WGPUPipelineLayoutExtras layoutExtras = {
 *     .chain = { .sType = WGPUSType_PipelineLayoutExtras },
 *     .pushConstantRangeCount = 1,
 *     .pushConstantRanges     = &range,
 * };
 * @endcode
 */
typedef struct WGPUPipelineLayoutExtras {
    WGPUChainedStruct chain;
    size_t pushConstantRangeCount;                     /**< Number of ranges. */
    WGPUPushConstantRange const * pushConstantRanges;  /**< Array of ranges. */
} WGPUPipelineLayoutExtras;

/**
 * @typedef WGPUSubmissionIndex
 * @brief Opaque monotonic index returned by @ref wgpuQueueSubmitForIndex.
 *
 * Can be passed to @ref wgpuDevicePoll to wait for or check completion
 * of a specific submission rather than polling all outstanding work.
 */
typedef uint64_t WGPUSubmissionIndex;

/**
 * @struct WGPUShaderDefine
 * @brief A preprocessor #define injected into GLSL source.
 */
typedef struct WGPUShaderDefine {
    WGPUStringView name;   /**< Define name (e.g. "MAX_LIGHTS"). */
    WGPUStringView value;  /**< Define value (e.g. "16"). */
} WGPUShaderDefine;

/**
 * @struct WGPUShaderSourceGLSL
 * @brief Supplies GLSL source code for shader module creation.
 *
 * Chain onto `WGPUShaderModuleDescriptor.nextInChain` to create a shader
 * from GLSL instead of WGSL.  wgpu-native uses naga to cross-compile.
 */
typedef struct WGPUShaderSourceGLSL {
    WGPUChainedStruct chain;       /**< sType = WGPUSType_ShaderSourceGLSL. */
    WGPUShaderStage stage;         /**< Which stage this GLSL targets. */
    WGPUStringView code;           /**< The GLSL source text. */
    uint32_t defineCount;          /**< Number of preprocessor defines. */
    WGPUShaderDefine * defines;    /**< Array of defines. */
} WGPUShaderSourceGLSL;

/**
 * @struct WGPUShaderModuleDescriptorSpirV
 * @brief Descriptor for creating a shader module from pre-compiled SPIR-V.
 *
 * Used with @ref wgpuDeviceCreateShaderModuleSpirV.  Requires the
 * @ref WGPUNativeFeature_SpirvShaderPassthrough feature.
 *
 * @warning SPIR-V passthrough bypasses wgpu's internal validation.
 *          Invalid SPIR-V may cause driver crashes.
 */
typedef struct WGPUShaderModuleDescriptorSpirV {
    WGPUStringView label;           /**< Debug label for the module. */
    uint32_t sourceSize;            /**< SPIR-V word count (number of uint32_t elements). */
    uint32_t const * source;        /**< Pointer to SPIR-V word array. */
} WGPUShaderModuleDescriptorSpirV;

/* ---- Diagnostic / Report Structs --------------------------------------- */

/**
 * @struct WGPURegistryReport
 * @brief Per-resource-type allocation statistics.
 *
 * Returned as part of @ref WGPUHubReport / @ref WGPUGlobalReport via
 * @ref wgpuGenerateReport.  Useful for detecting leaks.
 */
typedef struct WGPURegistryReport {
   size_t numAllocated;        /**< Total objects currently allocated. */
   size_t numKeptFromUser;     /**< Objects the user still holds a reference to. */
   size_t numReleasedFromUser; /**< Objects the user released but not yet freed internally. */
   size_t elementSize;         /**< Size in bytes of one element. */
} WGPURegistryReport;

/**
 * @struct WGPUHubReport
 * @brief Aggregated allocation statistics for every GPU resource type.
 */
typedef struct WGPUHubReport {
    WGPURegistryReport adapters;
    WGPURegistryReport devices;
    WGPURegistryReport queues;
    WGPURegistryReport pipelineLayouts;
    WGPURegistryReport shaderModules;
    WGPURegistryReport bindGroupLayouts;
    WGPURegistryReport bindGroups;
    WGPURegistryReport commandBuffers;
    WGPURegistryReport renderBundles;
    WGPURegistryReport renderPipelines;
    WGPURegistryReport computePipelines;
    WGPURegistryReport pipelineCaches;
    WGPURegistryReport querySets;
    WGPURegistryReport buffers;
    WGPURegistryReport textures;
    WGPURegistryReport textureViews;
    WGPURegistryReport samplers;
} WGPUHubReport;

/**
 * @struct WGPUGlobalReport
 * @brief Top-level report covering surfaces and all GPU resource types.
 *
 * Obtain via @ref wgpuGenerateReport.
 *
 * @par Leak Detection Example
 * @code
 * WGPUGlobalReport report;
 * wgpuGenerateReport(instance, &report);
 * printf("Buffers alive: %zu\n", report.hub.buffers.numAllocated);
 * @endcode
 */
typedef struct WGPUGlobalReport {
    WGPURegistryReport surfaces;
    WGPUHubReport hub;
} WGPUGlobalReport;

/**
 * @struct WGPUInstanceEnumerateAdapterOptions
 * @brief Options for @ref wgpuInstanceEnumerateAdapters.
 *
 * Set `backends` to limit which backends are queried.
 */
typedef struct WGPUInstanceEnumerateAdapterOptions {
    WGPUChainedStruct const * nextInChain;
    WGPUInstanceBackend backends; /**< Backend filter (0 = all). */
} WGPUInstanceEnumerateAdapterOptions;

/* ---- Bind Group Array Extensions --------------------------------------- */

/**
 * @struct WGPUBindGroupEntryExtras
 * @brief Extends a bind group entry with arrays of resources.
 *
 * Instead of binding a single buffer/sampler/texture-view to a slot,
 * this lets you bind an array of them (requires the matching
 * `*BindingArray` feature).
 *
 * Chain onto `WGPUBindGroupEntry.nextInChain`.
 */
typedef struct WGPUBindGroupEntryExtras {
    WGPUChainedStruct chain;
    WGPUBuffer const * buffers;          /**< Array of buffers (or NULL). */
    size_t bufferCount;                  /**< Number of buffers. */
    WGPUSampler const * samplers;        /**< Array of samplers (or NULL). */
    size_t samplerCount;                 /**< Number of samplers. */
    WGPUTextureView const * textureViews;/**< Array of texture views (or NULL). */
    size_t textureViewCount;             /**< Number of texture views. */
} WGPUBindGroupEntryExtras;

/**
 * @struct WGPUBindGroupLayoutEntryExtras
 * @brief Declares the array count for a binding in a bind group layout.
 *
 * Chain onto `WGPUBindGroupLayoutEntry.nextInChain`.
 */
typedef struct WGPUBindGroupLayoutEntryExtras {
    WGPUChainedStruct chain;
    uint32_t count; /**< Number of array elements at this binding. */
} WGPUBindGroupLayoutEntryExtras;

/**
 * @struct WGPUQuerySetDescriptorExtras
 * @brief Specifies which pipeline statistics to record.
 *
 * Chain onto `WGPUQuerySetDescriptor.nextInChain` when creating a
 * pipeline-statistics query set.
 */
typedef struct WGPUQuerySetDescriptorExtras {
    WGPUChainedStruct chain;
    WGPUPipelineStatisticName const * pipelineStatistics; /**< Array of statistics to record. */
    size_t pipelineStatisticCount;                        /**< Length of the array. */
} WGPUQuerySetDescriptorExtras WGPU_STRUCTURE_ATTRIBUTE;

/**
 * @struct WGPUSurfaceConfigurationExtras
 * @brief Extended surface / swap chain configuration.
 *
 * Chain onto `WGPUSurfaceConfiguration.nextInChain`.
 */
typedef struct WGPUSurfaceConfigurationExtras {
    WGPUChainedStruct chain;
    uint32_t desiredMaximumFrameLatency; /**< Max number of frames the compositor may queue ahead. */
} WGPUSurfaceConfigurationExtras WGPU_STRUCTURE_ATTRIBUTE;

/**
 * @struct WGPUSurfaceSourceSwapChainPanel
 * @brief Creates a surface from a WinUI XAML SwapChainPanel.
 *
 * Chain onto `WGPUSurfaceDescriptor.nextInChain` on Windows when
 * hosting WebGPU inside a WinUI 3 application.
 */
typedef struct WGPUSurfaceSourceSwapChainPanel {
    WGPUChainedStruct chain;
    void * panelNative; /**< Pointer to ISwapChainPanelNative COM interface. */
} WGPUSurfaceSourceSwapChainPanel WGPU_STRUCTURE_ATTRIBUTE;

/**
 * @struct WGPUPrimitiveStateExtras
 * @brief Extends primitive state with polygon mode and conservative rasterization.
 *
 * Chain onto `WGPUPrimitiveState.nextInChain`.
 */
typedef struct WGPUPrimitiveStateExtras {
    WGPUChainedStruct chain;
    WGPUPolygonMode polygonMode; /**< Fill, Line, or Point. */
    WGPUBool conservative;       /**< Enable conservative rasterization. */
} WGPUPrimitiveStateExtras WGPU_STRUCTURE_ATTRIBUTE;


/* ========================================================================= */
/*  Callback Types                                                           */
/* ========================================================================= */

/**
 * @typedef WGPULogCallback
 * @brief Signature for the global logging callback.
 *
 * @param level    Severity of the message.
 * @param message  The log message text.
 * @param userdata Opaque pointer passed to @ref wgpuSetLogCallback.
 *
 * @par Example
 * @code
 * void myLogCb(WGPULogLevel level, WGPUStringView msg, void *ud) {
 *     fprintf(stderr, "[wgpu %d] %.*s\n", level, (int)msg.length, msg.data);
 * }
 * wgpuSetLogCallback(myLogCb, NULL);
 * wgpuSetLogLevel(WGPULogLevel_Warn);
 * @endcode
 */
typedef void (*WGPULogCallback)(WGPULogLevel level, WGPUStringView message, void * userdata);


/* ========================================================================= */
/*  Functions                                                                */
/* ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Global / Instance ------------------------------------------------- */

/**
 * @brief Generate a diagnostic report of every live GPU resource.
 *
 * Fills @p report with allocation counts for every resource type.
 * Useful for finding leaks at shutdown.
 *
 * @param instance  The wgpu instance.
 * @param report    Pointer to a @ref WGPUGlobalReport struct to fill.
 */
void wgpuGenerateReport(WGPUInstance instance, WGPUGlobalReport * report);

/**
 * @brief Enumerate all adapters visible to the instance.
 *
 * Call once with `adapters = NULL` to get the count, allocate an array,
 * then call again to fill it.
 *
 * @param instance  The wgpu instance.
 * @param options   Optional backend filter (may be NULL for all backends).
 * @param adapters  Output array, or NULL to query count only.
 * @return          Number of adapters found.
 *
 * @par Example
 * @code
 * size_t n = wgpuInstanceEnumerateAdapters(instance, NULL, NULL);
 * WGPUAdapter *adapters = malloc(n * sizeof(WGPUAdapter));
 * wgpuInstanceEnumerateAdapters(instance, NULL, adapters);
 * @endcode
 */
size_t wgpuInstanceEnumerateAdapters(WGPUInstance instance,
    WGPU_NULLABLE WGPUInstanceEnumerateAdapterOptions const * options,
    WGPUAdapter * adapters);

/* ---- Queue ------------------------------------------------------------- */

/**
 * @brief Submit command buffers and return a trackable submission index.
 *
 * Identical to `wgpuQueueSubmit` but returns a @ref WGPUSubmissionIndex
 * that can later be passed to @ref wgpuDevicePoll to wait for just
 * this submission.
 *
 * @param queue         The device queue.
 * @param commandCount  Number of command buffers.
 * @param commands      Array of finished command buffers.
 * @return              Monotonically increasing submission index.
 */
WGPUSubmissionIndex wgpuQueueSubmitForIndex(WGPUQueue queue,
    size_t commandCount, WGPUCommandBuffer const * commands);

/**
 * @brief Query the GPU timestamp period (nanoseconds per tick).
 *
 * Multiply a timestamp-query delta by this value to get elapsed nanoseconds.
 *
 * @param queue  The device queue.
 * @return       Nanoseconds per timestamp tick.
 */
float wgpuQueueGetTimestampPeriod(WGPUQueue queue);

/* ---- Device ------------------------------------------------------------ */

/**
 * @brief Poll the device for completed work.
 *
 * On native backends the GPU runs asynchronously.  Call this function to:
 * - Process completed map callbacks.
 * - Advance internal recycling of resources.
 * - Optionally block until a specific submission completes.
 *
 * @param device          The logical device.
 * @param wait            If true, blocks until the specified (or all) work is done.
 * @param submissionIndex If non-NULL, only poll up to this submission.
 * @return                @c true if the queue is now empty.
 */
WGPUBool wgpuDevicePoll(WGPUDevice device, WGPUBool wait,
    WGPU_NULLABLE WGPUSubmissionIndex const * submissionIndex);

/**
 * @brief Create a shader module directly from SPIR-V binary.
 *
 * Requires @ref WGPUNativeFeature_SpirvShaderPassthrough.
 *
 * @param device      The logical device.
 * @param descriptor  SPIR-V descriptor with pointer to the word array.
 * @return            A new shader module, or NULL on failure.
 */
WGPUShaderModule wgpuDeviceCreateShaderModuleSpirV(WGPUDevice device,
    WGPUShaderModuleDescriptorSpirV const * descriptor);

/* ---- Logging ----------------------------------------------------------- */

/**
 * @brief Register a global log callback.
 *
 * Only one callback is active at a time; calling again replaces the
 * previous one.  Pass NULL to remove.
 *
 * @param callback  Function to call on each log message.
 * @param userdata  Passed through to the callback unchanged.
 */
void wgpuSetLogCallback(WGPULogCallback callback, void * userdata);

/**
 * @brief Set the minimum log level that will be delivered to the callback.
 *
 * Messages below this level are discarded before reaching the callback.
 *
 * @param level  Minimum severity to report.
 */
void wgpuSetLogLevel(WGPULogLevel level);

/**
 * @brief Get the wgpu-native library version.
 *
 * @return Packed version number.
 */
uint32_t wgpuGetVersion(void);

/* ---- Push Constants ---------------------------------------------------- */

/**
 * @brief Upload push constants during a render pass.
 *
 * @param encoder   Active render pass encoder.
 * @param stages    Shader stages that will read these constants.
 * @param offset    Byte offset into the push constant block (4-byte aligned).
 * @param sizeBytes Number of bytes to upload (4-byte aligned).
 * @param data      Pointer to the source data.
 */
void wgpuRenderPassEncoderSetPushConstants(WGPURenderPassEncoder encoder,
    WGPUShaderStage stages, uint32_t offset, uint32_t sizeBytes, void const * data);

/**
 * @brief Upload push constants during a compute pass.
 *
 * Compute push constants are implicitly visible to the compute stage only,
 * so no `stages` parameter is needed.
 *
 * @param encoder   Active compute pass encoder.
 * @param offset    Byte offset (4-byte aligned).
 * @param sizeBytes Number of bytes (4-byte aligned).
 * @param data      Source data pointer.
 */
void wgpuComputePassEncoderSetPushConstants(WGPUComputePassEncoder encoder,
    uint32_t offset, uint32_t sizeBytes, void const * data);

/**
 * @brief Upload push constants inside a render bundle.
 *
 * @param encoder   Render bundle encoder.
 * @param stages    Shader stages that will read these constants.
 * @param offset    Byte offset (4-byte aligned).
 * @param sizeBytes Number of bytes (4-byte aligned).
 * @param data      Source data pointer.
 */
void wgpuRenderBundleEncoderSetPushConstants(WGPURenderBundleEncoder encoder,
    WGPUShaderStage stages, uint32_t offset, uint32_t sizeBytes, void const * data);

/* ---- Multi-Draw Indirect ----------------------------------------------- */

/**
 * @brief Issue multiple non-indexed indirect draw calls from a buffer.
 *
 * The buffer contains `count` tightly-packed `WGPUDrawIndirect` structs.
 *
 * @param encoder  Active render pass encoder.
 * @param buffer   Indirect argument buffer.
 * @param offset   Byte offset to the first struct in the buffer.
 * @param count    Number of draws to issue.
 */
void wgpuRenderPassEncoderMultiDrawIndirect(WGPURenderPassEncoder encoder,
    WGPUBuffer buffer, uint64_t offset, uint32_t count);

/**
 * @brief Issue multiple indexed indirect draw calls from a buffer.
 *
 * The buffer contains `count` tightly-packed `WGPUDrawIndexedIndirect` structs.
 *
 * @param encoder  Active render pass encoder.
 * @param buffer   Indirect argument buffer.
 * @param offset   Byte offset to the first struct in the buffer.
 * @param count    Number of draws to issue.
 */
void wgpuRenderPassEncoderMultiDrawIndexedIndirect(WGPURenderPassEncoder encoder,
    WGPUBuffer buffer, uint64_t offset, uint32_t count);

/**
 * @brief Multi-draw indirect with the draw count read from a GPU buffer.
 *
 * This is the most flexible indirect drawing path: the GPU itself decides
 * how many draws to issue (up to @p max_count), enabling fully GPU-driven
 * rendering pipelines (e.g. GPU culling → indirect draw).
 *
 * Requires @ref WGPUNativeFeature_MultiDrawIndirectCount.
 *
 * @param encoder              Active render pass encoder.
 * @param buffer               Indirect argument buffer.
 * @param offset               Byte offset in @p buffer.
 * @param count_buffer         Buffer containing the actual draw count (uint32).
 * @param count_buffer_offset  Byte offset in @p count_buffer.
 * @param max_count            Upper bound on draws (clamped by the GPU-read count).
 */
void wgpuRenderPassEncoderMultiDrawIndirectCount(WGPURenderPassEncoder encoder,
    WGPUBuffer buffer, uint64_t offset,
    WGPUBuffer count_buffer, uint64_t count_buffer_offset, uint32_t max_count);

/**
 * @brief Indexed multi-draw indirect with GPU-read draw count.
 *
 * Same as @ref wgpuRenderPassEncoderMultiDrawIndirectCount but for
 * indexed geometry.
 *
 * @param encoder              Active render pass encoder.
 * @param buffer               Indirect argument buffer (DrawIndexedIndirect structs).
 * @param offset               Byte offset in @p buffer.
 * @param count_buffer         Buffer holding the draw count.
 * @param count_buffer_offset  Byte offset in @p count_buffer.
 * @param max_count            Maximum number of draws.
 */
void wgpuRenderPassEncoderMultiDrawIndexedIndirectCount(WGPURenderPassEncoder encoder,
    WGPUBuffer buffer, uint64_t offset,
    WGPUBuffer count_buffer, uint64_t count_buffer_offset, uint32_t max_count);

/* ---- Pipeline Statistics Queries --------------------------------------- */

/**
 * @brief Begin recording pipeline statistics in a compute pass.
 * @param computePassEncoder  Active compute pass encoder.
 * @param querySet            A pipeline-statistics query set.
 * @param queryIndex          Index within the query set.
 */
void wgpuComputePassEncoderBeginPipelineStatisticsQuery(
    WGPUComputePassEncoder computePassEncoder,
    WGPUQuerySet querySet, uint32_t queryIndex);

/**
 * @brief End the current pipeline statistics query in a compute pass.
 * @param computePassEncoder  Active compute pass encoder.
 */
void wgpuComputePassEncoderEndPipelineStatisticsQuery(
    WGPUComputePassEncoder computePassEncoder);

/**
 * @brief Begin recording pipeline statistics in a render pass.
 * @param renderPassEncoder  Active render pass encoder.
 * @param querySet           A pipeline-statistics query set.
 * @param queryIndex         Index within the query set.
 */
void wgpuRenderPassEncoderBeginPipelineStatisticsQuery(
    WGPURenderPassEncoder renderPassEncoder,
    WGPUQuerySet querySet, uint32_t queryIndex);

/**
 * @brief End the current pipeline statistics query in a render pass.
 * @param renderPassEncoder  Active render pass encoder.
 */
void wgpuRenderPassEncoderEndPipelineStatisticsQuery(
    WGPURenderPassEncoder renderPassEncoder);

/* ---- Timestamp Queries Inside Passes ----------------------------------- */

/**
 * @brief Write a GPU timestamp inside a compute pass.
 *
 * Requires @ref WGPUNativeFeature_TimestampQueryInsidePasses.
 *
 * @param computePassEncoder  Active compute pass encoder.
 * @param querySet            A timestamp query set.
 * @param queryIndex          Index within the query set.
 */
void wgpuComputePassEncoderWriteTimestamp(
    WGPUComputePassEncoder computePassEncoder,
    WGPUQuerySet querySet, uint32_t queryIndex);

/**
 * @brief Write a GPU timestamp inside a render pass.
 *
 * Requires @ref WGPUNativeFeature_TimestampQueryInsidePasses.
 *
 * @param renderPassEncoder  Active render pass encoder.
 * @param querySet           A timestamp query set.
 * @param queryIndex         Index within the query set.
 */
void wgpuRenderPassEncoderWriteTimestamp(
    WGPURenderPassEncoder renderPassEncoder,
    WGPUQuerySet querySet, uint32_t queryIndex);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* WGPU_H_ */
