/*
 * gles_bindings.h - WebGL2 / OpenGL ES 3.0 emulation layer over WebGPU: GL constants, state tracking, SPIRV-Reflect shader reflection, pipeline cache.
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
 * @file gles_bindings.h
 * @brief WebGL2 / OpenGL ES 3.0 emulation layer over WebGPU.
 *
 * Translates OpenGL ES 3.0 API calls from guest WASM modules into
 * WebGPU commands at runtime. Includes GL constant definitions,
 * state tracking structures, shader reflection via SPIRV-Reflect,
 * pipeline caching, and a full WebGL2Context that maps the GL state
 * machine onto wgpu-native.
 *
 * ## Example
 *
 * @code{.c}
 * #include "gles_bindings.h"
 *
 * WebGL2Context gl;
 * webgl2_init(&gl, &gpu_context);
 * webgl2_set_memory(&gl, wasm_memory);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t n = webgl2_get_imports(&gl, store, &names, &funcs);
 * // Register imports; guest WASM now calls glClear, glDrawArrays, etc.
 *
 * // Per frame:
 * webgl2_frame_begin(&gl);
 * // ... guest renders via GL calls ...
 * webgl2_frame_end(&gl);
 *
 * webgl2_destroy(&gl);
 * @endcode
 */

#ifndef WEBGL2_H
#define WEBGL2_H

#include "deps.h"
#include "gpu.h"
#include "handle_table.h"
#include "spirv_reflect.h"
#include <stdbool.h>
#include <stdint.h>

/* ---- GL constants (matching WebGL2 / ES 3.0 values) ---- */

#define YGL_DEPTH_BUFFER_BIT 0x00000100
#define YGL_STENCIL_BUFFER_BIT 0x00000400
#define YGL_COLOR_BUFFER_BIT 0x00004000

#define YGL_POINTS 0x0000
#define YGL_LINES 0x0001
#define YGL_LINE_LOOP 0x0002
#define YGL_LINE_STRIP 0x0003
#define YGL_TRIANGLES 0x0004
#define YGL_TRIANGLE_STRIP 0x0005
#define YGL_TRIANGLE_FAN 0x0006

#define YGL_ARRAY_BUFFER 0x8892
#define YGL_ELEMENT_ARRAY_BUFFER 0x8893
#define YGL_COPY_READ_BUFFER 0x8F36
#define YGL_COPY_WRITE_BUFFER 0x8F37
#define YGL_TRANSFORM_FEEDBACK_BUFFER 0x8C8E
#define YGL_UNIFORM_BUFFER 0x8A11
#define YGL_PIXEL_PACK_BUFFER 0x88EB
#define YGL_PIXEL_UNPACK_BUFFER 0x88EC

#define YGL_STREAM_DRAW 0x88E0
#define YGL_STREAM_READ 0x88E1
#define YGL_STREAM_COPY 0x88E2
#define YGL_STATIC_DRAW 0x88E4
#define YGL_STATIC_READ 0x88E5
#define YGL_STATIC_COPY 0x88E6
#define YGL_DYNAMIC_DRAW 0x88E8
#define YGL_DYNAMIC_READ 0x88E9
#define YGL_DYNAMIC_COPY 0x88EA

#define YGL_BYTE 0x1400
#define YGL_UNSIGNED_BYTE 0x1401
#define YGL_SHORT 0x1402
#define YGL_UNSIGNED_SHORT 0x1403
#define YGL_INT 0x1404
#define YGL_UNSIGNED_INT 0x1405
#define YGL_FLOAT 0x1406
#define YGL_HALF_FLOAT 0x140B

#define YGL_FALSE 0
#define YGL_TRUE 1

#define YGL_CULL_FACE 0x0B44
#define YGL_BLEND 0x0BE2
#define YGL_DITHER 0x0BD0
#define YGL_STENCIL_TEST 0x0B90
#define YGL_DEPTH_TEST 0x0B71
#define YGL_SCISSOR_TEST 0x0C11
#define YGL_POLYGON_OFFSET_FILL 0x8037
#define YGL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define YGL_SAMPLE_COVERAGE 0x80A0
#define YGL_RASTERIZER_DISCARD 0x8C89

#define YGL_NO_ERROR 0
#define YGL_INVALID_ENUM 0x0500
#define YGL_INVALID_VALUE 0x0501
#define YGL_INVALID_OPERATION 0x0502
#define YGL_OUT_OF_MEMORY 0x0505

#define YGL_FRONT 0x0404
#define YGL_BACK 0x0405
#define YGL_FRONT_AND_BACK 0x0408

#define YGL_CW 0x0900
#define YGL_CCW 0x0901

#define YGL_NEVER 0x0200
#define YGL_LESS 0x0201
#define YGL_EQUAL 0x0202
#define YGL_LEQUAL 0x0203
#define YGL_GREATER 0x0204
#define YGL_NOTEQUAL 0x0205
#define YGL_GEQUAL 0x0206
#define YGL_ALWAYS 0x0207

#define YGL_KEEP 0x1E00
#define YGL_REPLACE 0x1E01
#define YGL_INCR 0x1E02
#define YGL_DECR 0x1E03
#define YGL_INVERT 0x150A
#define YGL_INCR_WRAP 0x8507
#define YGL_DECR_WRAP 0x8508

#define YGL_ZERO 0x0000
#define YGL_ONE 0x0001
#define YGL_SRC_COLOR 0x0300
#define YGL_ONE_MINUS_SRC_COLOR 0x0301
#define YGL_SRC_ALPHA 0x0302
#define YGL_ONE_MINUS_SRC_ALPHA 0x0303
#define YGL_DST_ALPHA 0x0304
#define YGL_ONE_MINUS_DST_ALPHA 0x0305
#define YGL_DST_COLOR 0x0306
#define YGL_ONE_MINUS_DST_COLOR 0x0307
#define YGL_SRC_ALPHA_SATURATE 0x0308
#define YGL_CONSTANT_COLOR 0x8001
#define YGL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define YGL_CONSTANT_ALPHA 0x8003
#define YGL_ONE_MINUS_CONSTANT_ALPHA 0x8004

#define YGL_FUNC_ADD 0x8006
#define YGL_FUNC_SUBTRACT 0x800A
#define YGL_FUNC_REVERSE_SUBTRACT 0x800B
#define YGL_MIN 0x8007
#define YGL_MAX 0x8008

#define YGL_TEXTURE_2D 0x0DE1
#define YGL_TEXTURE_CUBE_MAP 0x8513
#define YGL_TEXTURE_3D 0x806F
#define YGL_TEXTURE_2D_ARRAY 0x8C1A
#define YGL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define YGL_TEXTURE_CUBE_MAP_NEGATIVE_X 0x8516
#define YGL_TEXTURE_CUBE_MAP_POSITIVE_Y 0x8517
#define YGL_TEXTURE_CUBE_MAP_NEGATIVE_Y 0x8518
#define YGL_TEXTURE_CUBE_MAP_POSITIVE_Z 0x8519
#define YGL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A

#define YGL_TEXTURE_MIN_FILTER 0x2801
#define YGL_TEXTURE_MAG_FILTER 0x2800
#define YGL_TEXTURE_WRAP_S 0x2802
#define YGL_TEXTURE_WRAP_T 0x2803
#define YGL_TEXTURE_WRAP_R 0x8072
#define YGL_TEXTURE_MIN_LOD 0x813A
#define YGL_TEXTURE_MAX_LOD 0x813B
#define YGL_TEXTURE_BASE_LEVEL 0x813C
#define YGL_TEXTURE_MAX_LEVEL 0x813D
#define YGL_TEXTURE_COMPARE_MODE 0x884C
#define YGL_TEXTURE_COMPARE_FUNC 0x884D
#define YGL_NEAREST 0x2600
#define YGL_LINEAR 0x2601
#define YGL_NEAREST_MIPMAP_NEAREST 0x2700
#define YGL_LINEAR_MIPMAP_NEAREST 0x2701
#define YGL_NEAREST_MIPMAP_LINEAR 0x2702
#define YGL_LINEAR_MIPMAP_LINEAR 0x2703
#define YGL_CLAMP_TO_EDGE 0x812F
#define YGL_MIRRORED_REPEAT 0x8370
#define YGL_REPEAT 0x2901

#define YGL_ALPHA 0x1906
#define YGL_RGB 0x1907
#define YGL_RGBA 0x1908
#define YGL_LUMINANCE 0x1909
#define YGL_LUMINANCE_ALPHA 0x190A
#define YGL_RED 0x1903
#define YGL_RG 0x8227
#define YGL_R8 0x8229
#define YGL_RG8 0x822B
#define YGL_RGB8 0x8051
#define YGL_RGBA8 0x8058
#define YGL_R16F 0x822D
#define YGL_RG16F 0x822F
#define YGL_RGB16F 0x881B
#define YGL_RGBA16F 0x881A
#define YGL_R32F 0x822E
#define YGL_RG32F 0x8230
#define YGL_RGB32F 0x8815
#define YGL_RGBA32F 0x8814
#define YGL_R8I 0x8231
#define YGL_R8UI 0x8232
#define YGL_R16I 0x8233
#define YGL_R16UI 0x8234
#define YGL_R32I 0x8235
#define YGL_R32UI 0x8236
#define YGL_RG8I 0x8237
#define YGL_RG8UI 0x8238
#define YGL_RG16I 0x8239
#define YGL_RG16UI 0x823A
#define YGL_RG32I 0x823B
#define YGL_RG32UI 0x823C
#define YGL_RGBA8I 0x8D8E
#define YGL_RGBA8UI 0x8D7C
#define YGL_RGBA16I 0x8D88
#define YGL_RGBA16UI 0x8D76
#define YGL_RGBA32I 0x8D82
#define YGL_RGBA32UI 0x8D70
#define YGL_RGB10_A2 0x8059
#define YGL_R11F_G11F_B10F 0x8C3A
#define YGL_SRGB8 0x8C41
#define YGL_SRGB8_ALPHA8 0x8C43
#define YGL_DEPTH_COMPONENT16 0x81A5
#define YGL_DEPTH_COMPONENT24 0x81A6
#define YGL_DEPTH_COMPONENT32F 0x8CAC
#define YGL_DEPTH24_STENCIL8 0x88F0
#define YGL_DEPTH32F_STENCIL8 0x8CAD
#define YGL_DEPTH_COMPONENT 0x1902
#define YGL_DEPTH_STENCIL 0x84F9
#define YGL_RED_INTEGER 0x8D94
#define YGL_RG_INTEGER 0x8228
#define YGL_RGB_INTEGER 0x8D98
#define YGL_RGBA_INTEGER 0x8D99

#define YGL_UNPACK_ALIGNMENT 0x0CF5
#define YGL_PACK_ALIGNMENT 0x0D05
#define YGL_UNPACK_ROW_LENGTH 0x0CF2
#define YGL_UNPACK_SKIP_ROWS 0x0CF3
#define YGL_UNPACK_SKIP_PIXELS 0x0CF4
#define YGL_UNPACK_IMAGE_HEIGHT 0x806E
#define YGL_UNPACK_SKIP_IMAGES 0x806D
#define YGL_UNPACK_FLIP_Y_WEBGL 0x9240
#define YGL_UNPACK_PREMULTIPLY_ALPHA_WEBGL 0x9241

#define YGL_VERTEX_SHADER 0x8B31
#define YGL_FRAGMENT_SHADER 0x8B30
#define YGL_COMPILE_STATUS 0x8B81
#define YGL_LINK_STATUS 0x8B82
#define YGL_SHADER_TYPE 0x8B4F
#define YGL_DELETE_STATUS 0x8B80
#define YGL_VALIDATE_STATUS 0x8B83
#define YGL_ATTACHED_SHADERS 0x8B85
#define YGL_ACTIVE_UNIFORMS 0x8B86
#define YGL_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#define YGL_ACTIVE_ATTRIBUTES 0x8B89
#define YGL_ACTIVE_ATTRIBUTE_MAX_LENGTH 0x8B8A
#define YGL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define YGL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH 0x8A35
#define YGL_INFO_LOG_LENGTH 0x8B84
#define YGL_SHADER_SOURCE_LENGTH 0x8B88

#define YGL_INVALID_INDEX 0xFFFFFFFF

#define YGL_UNIFORM_TYPE 0x8A37
#define YGL_UNIFORM_SIZE 0x8A38
#define YGL_UNIFORM_NAME_LENGTH 0x8A39
#define YGL_UNIFORM_BLOCK_INDEX 0x8A3A
#define YGL_UNIFORM_OFFSET 0x8A3B
#define YGL_UNIFORM_ARRAY_STRIDE 0x8A3C
#define YGL_UNIFORM_MATRIX_STRIDE 0x8A3D
#define YGL_UNIFORM_IS_ROW_MAJOR 0x8A3E
#define YGL_UNIFORM_BLOCK_BINDING 0x8A3F
#define YGL_UNIFORM_BLOCK_DATA_SIZE 0x8A40
#define YGL_UNIFORM_BLOCK_NAME_LENGTH 0x8A41
#define YGL_UNIFORM_BLOCK_ACTIVE_UNIFORMS 0x8A42
#define YGL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES 0x8A43
#define YGL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER 0x8A44
#define YGL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER 0x8A46

#define YGL_FRAMEBUFFER 0x8D40
#define YGL_READ_FRAMEBUFFER 0x8CA8
#define YGL_DRAW_FRAMEBUFFER 0x8CA9
#define YGL_RENDERBUFFER 0x8D41
#define YGL_COLOR_ATTACHMENT0 0x8CE0
#define YGL_COLOR_ATTACHMENT1 0x8CE1
#define YGL_COLOR_ATTACHMENT2 0x8CE2
#define YGL_COLOR_ATTACHMENT3 0x8CE3
#define YGL_COLOR_ATTACHMENT4 0x8CE4
#define YGL_COLOR_ATTACHMENT5 0x8CE5
#define YGL_COLOR_ATTACHMENT6 0x8CE6
#define YGL_COLOR_ATTACHMENT7 0x8CE7
#define YGL_DEPTH_ATTACHMENT 0x8D00
#define YGL_STENCIL_ATTACHMENT 0x8D20
#define YGL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define YGL_FRAMEBUFFER_COMPLETE 0x8CD5

#define YGL_MAX_TEXTURE_SIZE 0x0D33
#define YGL_MAX_VERTEX_ATTRIBS 0x8869
#define YGL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define YGL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define YGL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define YGL_MAX_RENDERBUFFER_SIZE 0x84E8
#define YGL_MAX_VIEWPORT_DIMS 0x0D3A
#define YGL_VIEWPORT 0x0BA2
#define YGL_ACTIVE_TEXTURE 0x84E0
#define YGL_CURRENT_PROGRAM 0x8B8D
#define YGL_FRAMEBUFFER_BINDING 0x8CA6
#define YGL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define YGL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define YGL_RENDERBUFFER_BINDING 0x8CA7
#define YGL_ARRAY_BUFFER_BINDING 0x8894
#define YGL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define YGL_VERTEX_ARRAY_BINDING 0x85B5
#define YGL_UNIFORM_BUFFER_BINDING 0x8A28
#define YGL_MAX_VERTEX_UNIFORM_BLOCKS 0x8A2B
#define YGL_MAX_FRAGMENT_UNIFORM_BLOCKS 0x8A2D
#define YGL_MAX_UNIFORM_BLOCK_SIZE 0x8A30
#define YGL_MAX_UNIFORM_BUFFER_BINDINGS 0x8A2F
#define YGL_MAX_DRAW_BUFFERS 0x8824
#define YGL_MAX_COLOR_ATTACHMENTS 0x8CDF
#define YGL_MAX_SAMPLES 0x8D57
#define YGL_MAX_3D_TEXTURE_SIZE 0x8073
#define YGL_MAX_ARRAY_TEXTURE_LAYERS 0x88FF
#define YGL_NUM_EXTENSIONS 0x821D
#define YGL_MAJOR_VERSION 0x821B
#define YGL_MINOR_VERSION 0x821C
#define YGL_DEPTH_RANGE 0x0B70
#define YGL_BLEND_COLOR 0x8005
#define YGL_STENCIL_WRITEMASK 0x0B98
#define YGL_STENCIL_BACK_WRITEMASK 0x8CA5
#define YGL_STENCIL_REF 0x0B97
#define YGL_STENCIL_BACK_REF 0x8CA3
#define YGL_STENCIL_VALUE_MASK 0x0B93
#define YGL_STENCIL_BACK_VALUE_MASK 0x8CA4

#define YGL_MAP_READ_BIT 0x0001
#define YGL_MAP_WRITE_BIT 0x0002
#define YGL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define YGL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define YGL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#define YGL_MAP_UNSYNCHRONIZED_BIT 0x0020
#define YGL_BUFFER_MAPPED 0x88BC
#define YGL_BUFFER_MAP_POINTER 0x88BD
#define YGL_BUFFER_MAP_OFFSET 0x88D1
#define YGL_BUFFER_MAP_LENGTH 0x88D2
#define YGL_BUFFER_SIZE 0x8764
#define YGL_BUFFER_USAGE 0x8765

#define YGL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define YGL_ALREADY_SIGNALED 0x911A
#define YGL_TIMEOUT_EXPIRED 0x911B
#define YGL_CONDITION_SATISFIED 0x911C
#define YGL_WAIT_FAILED 0x911D
#define YGL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define YGL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull

#define YGL_ANY_SAMPLES_PASSED 0x8C2F
#define YGL_ANY_SAMPLES_PASSED_CONSERVATIVE 0x8D6A
#define YGL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN 0x8C88
#define YGL_QUERY_RESULT 0x8866
#define YGL_QUERY_RESULT_AVAILABLE 0x8867
#define YGL_CURRENT_QUERY 0x8865
#define YGL_MAX_CUBE_MAP_TEXTURE_SIZE 0x851C
#define YGL_MAX_VERTEX_UNIFORM_COMPONENTS 0x8B4A

#define YGL_VENDOR 0x1F00
#define YGL_RENDERER 0x1F01
#define YGL_VERSION 0x1F02
#define YGL_SHADING_LANGUAGE_VERSION 0x8B8C
#define YGL_EXTENSIONS 0x1F03

#define YGL_TEXTURE0 0x84C0
#define YGL_TEXTURE1 0x84C1
#define YGL_TEXTURE2 0x84C2
#define YGL_TEXTURE3 0x84C3
#define YGL_TEXTURE4 0x84C4
#define YGL_TEXTURE5 0x84C5
#define YGL_TEXTURE6 0x84C6
#define YGL_TEXTURE7 0x84C7
#define YGL_TEXTURE8 0x84C8
#define YGL_TEXTURE9 0x84C9
#define YGL_TEXTURE10 0x84CA
#define YGL_TEXTURE11 0x84CB
#define YGL_TEXTURE12 0x84CC
#define YGL_TEXTURE13 0x84CD
#define YGL_TEXTURE14 0x84CE
#define YGL_TEXTURE15 0x84CF

#define YGL_FLOAT_TYPE 0x1406
#define YGL_FLOAT_VEC2 0x8B50
#define YGL_FLOAT_VEC3 0x8B51
#define YGL_FLOAT_VEC4 0x8B52
#define YGL_INT_TYPE 0x1404
#define YGL_INT_VEC2 0x8B53
#define YGL_INT_VEC3 0x8B54
#define YGL_INT_VEC4 0x8B55
#define YGL_UNSIGNED_INT_TYPE 0x1405
#define YGL_UNSIGNED_INT_VEC2 0x8DC6
#define YGL_UNSIGNED_INT_VEC3 0x8DC7
#define YGL_UNSIGNED_INT_VEC4 0x8DC8
#define YGL_FLOAT_MAT2 0x8B5A
#define YGL_FLOAT_MAT3 0x8B5B
#define YGL_FLOAT_MAT4 0x8B5C
#define YGL_FLOAT_MAT2x3 0x8B65
#define YGL_FLOAT_MAT2x4 0x8B66
#define YGL_FLOAT_MAT3x2 0x8B67
#define YGL_FLOAT_MAT3x4 0x8B68
#define YGL_FLOAT_MAT4x2 0x8B69
#define YGL_FLOAT_MAT4x3 0x8B6A
#define YGL_SAMPLER_2D 0x8B5E
#define YGL_SAMPLER_3D 0x8B5F
#define YGL_SAMPLER_CUBE 0x8B60

#define YGL_TEXTURE_BINDING_2D 0x8069
#define YGL_TEXTURE_BINDING_3D 0x806A
#define YGL_TEXTURE_BINDING_CUBE_MAP 0x8514
#define YGL_TEXTURE_BINDING_2D_ARRAY 0x8C1D
#define YGL_MAX_ELEMENT_INDEX 0x8D6B
#define YGL_MAX_ELEMENTS_VERTICES 0x80E8
#define YGL_MAX_ELEMENTS_INDICES 0x80E9
#define YGL_FRAGMENT_SHADER_DERIVATIVE_HINT 0x8B8B
#define YGL_GENERATE_MIPMAP_HINT 0x8192
#define YGL_DONT_CARE 0x1100
#define YGL_FASTEST 0x1101
#define YGL_NICEST 0x1102
#define YGL_SAMPLER_BINDING 0x8919

/* Packed pixel types for BPP calculation */
#define YGL_UNSIGNED_SHORT_5_6_5 0x8363
#define YGL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define YGL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define YGL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#define YGL_UNSIGNED_INT_10F_11F_11F_REV 0x8C3B
#define YGL_UNSIGNED_INT_24_8 0x84FA
#define YGL_FLOAT_32_UNSIGNED_INT_24_8_REV 0x8DAD
#define YGL_UNSIGNED_INT_5_9_9_9_REV 0x8C3E

/* Compressed texture formats (ETC2 / EAC — mandatory in GLES 3.0) */
#define YGL_COMPRESSED_R11_EAC 0x9270
#define YGL_COMPRESSED_SIGNED_R11_EAC 0x9271
#define YGL_COMPRESSED_RG11_EAC 0x9272
#define YGL_COMPRESSED_SIGNED_RG11_EAC 0x9273
#define YGL_COMPRESSED_RGB8_ETC2 0x9274
#define YGL_COMPRESSED_SRGB8_ETC2 0x9275
#define YGL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9276
#define YGL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9277
#define YGL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#define YGL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC 0x9279

/* S3TC / DXT (common extension) */
#define YGL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define YGL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define YGL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define YGL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3

/* ASTC (common extension) */
#define YGL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#define YGL_COMPRESSED_RGBA_ASTC_5x4_KHR 0x93B1
#define YGL_COMPRESSED_RGBA_ASTC_5x5_KHR 0x93B2
#define YGL_COMPRESSED_RGBA_ASTC_6x5_KHR 0x93B3
#define YGL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4
#define YGL_COMPRESSED_RGBA_ASTC_8x5_KHR 0x93B5
#define YGL_COMPRESSED_RGBA_ASTC_8x6_KHR 0x93B6
#define YGL_COMPRESSED_RGBA_ASTC_8x8_KHR 0x93B7
#define YGL_COMPRESSED_RGBA_ASTC_10x5_KHR 0x93B8
#define YGL_COMPRESSED_RGBA_ASTC_10x6_KHR 0x93B9
#define YGL_COMPRESSED_RGBA_ASTC_10x8_KHR 0x93BA
#define YGL_COMPRESSED_RGBA_ASTC_10x10_KHR 0x93BB
#define YGL_COMPRESSED_RGBA_ASTC_12x10_KHR 0x93BC
#define YGL_COMPRESSED_RGBA_ASTC_12x12_KHR 0x93BD

/* Anisotropy extension */
#define YGL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define YGL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF

/* ---- Limits ---- */

#define WEBGL2_MAX_ATTRIBS 16
#define WEBGL2_MAX_TEXTURE_UNITS 16
#define WEBGL2_MAX_UNIFORM_BUFFER_BINDINGS 24
#define WEBGL2_MAX_BIND_GROUPS 4
#define WEBGL2_DEFAULT_DEPTH_FORMAT WGPUTextureFormat_Depth24PlusStencil8

#define MAX_REFLECTED_UNIFORMS 256
#define MAX_REFLECTED_SAMPLERS 16
#define MAX_REFLECTED_ATTRIBS 16
#define MAX_UNIFORM_BLOCKS 16

#define YGL_INTERLEAVED_ATTRIBS 0x8C8C
#define YGL_SEPARATE_ATTRIBS    0x8C8D

/* ---- State structures ---- */

#define PIPELINE_CACHE_SIZE 1024
#define MAX_TF_VARYINGS 16
#define MAX_TF_BUFFERS  4

typedef struct {
  bool enabled;
  uint32_t buffer_handle;
  int size;
  uint32_t type;
  bool normalized;
  bool integer;
  int stride;
  uint32_t offset;
  uint32_t divisor;
} VertexAttribState;
typedef struct {
  bool enabled;
  uint32_t src_rgb, dst_rgb, src_alpha, dst_alpha, eq_rgb, eq_alpha;
  float color[4];
} BlendState;
typedef struct {
  bool enabled;
  uint32_t func;
  bool write;
  float range_near, range_far;
} DepthState;
typedef struct {
  bool enabled;
  uint32_t mode, front_face;
} CullState;

typedef struct {
  bool enabled;
  uint32_t func_front;
  int32_t ref_front;
  uint32_t mask_front;
  uint32_t sfail_front, dpfail_front, dppass_front, writemask_front;
  uint32_t func_back;
  int32_t ref_back;
  uint32_t mask_back;
  uint32_t sfail_back, dpfail_back, dppass_back, writemask_back;
} StencilState;

typedef struct {
  float r, g, b, a;
} ClearColorState;
typedef struct {
  int x, y, w, h;
} ViewportState;

/* ---- Reflected uniform member ---- */
typedef struct {
  char *name;
  uint32_t block_idx, offset, size;
  uint32_t vec_size, columns, matrix_stride;
  bool is_int, is_unsigned;
  uint32_t array_size, array_stride;
} ReflectedUniform;

/* ---- Reflected sampler ---- */
typedef struct {
  char *name;
  uint32_t set, binding;
  int32_t bound_unit;
  uint32_t
      desc_type; /* SPV_REFLECT_DESCRIPTOR_TYPE_* for combined vs separate */
} ReflectedSampler;

/* ---- Reflected vertex input ---- */
typedef struct {
  char *name;
  uint32_t location, gl_type;
  int32_t size;
} ReflectedAttrib;

/* ---- Uniform block ---- */
typedef struct {
  char *name;
  uint32_t set, binding, gl_binding, size;
  uint8_t *cpu_data;
  WGPUBuffer gpu_buf;
  bool dirty, ref_vert, ref_frag;
} UniformBlockInfo;

/* ---- Shader object ---- */
typedef struct {
  uint32_t type;
  uint32_t *spirv_code;
  size_t spirv_size;
  bool compiled;
  SpvReflectShaderModule reflect;
  bool reflect_valid;
} ShaderObject;

/* ---- Program object ---- */
typedef struct {
  uint32_t vert_handle, frag_handle;
  bool linked, validated;
  WGPUShaderModule wgpu_vert, wgpu_frag;
  char *vert_entry, *frag_entry;
  UniformBlockInfo blocks[MAX_UNIFORM_BLOCKS];
  uint32_t block_count;
  ReflectedUniform uniforms[MAX_REFLECTED_UNIFORMS];
  uint32_t uniform_count;
  ReflectedSampler samplers[MAX_REFLECTED_SAMPLERS];
  uint32_t sampler_count;
  ReflectedAttrib attribs[MAX_REFLECTED_ATTRIBS];
  uint32_t attrib_count;
  WGPUBindGroupLayout bind_group_layouts[WEBGL2_MAX_BIND_GROUPS];
  uint32_t bind_group_layout_count;
  bool bind_group_layouts_valid;
  /* Forced attrib locations from glBindAttribLocation (applied at link time) */
  struct { char *name; uint32_t location; } forced_attrib_locs[MAX_REFLECTED_ATTRIBS];
  uint32_t forced_attrib_loc_count;
  char *tf_varyings[MAX_TF_VARYINGS];
  uint32_t tf_varying_count;
  uint32_t tf_buffer_mode;
} ProgramObject;

typedef struct {
  WGPUBuffer wgpu_buf;
  uint32_t size, usage;
  bool mapped;
  uint32_t map_offset, map_length, map_access, map_wasm_ptr;
  uint8_t *cpu_shadow;  /* CPU mirror for indexed fan/loop readback */
} BufferObject;
typedef struct {
  WGPUTexture wgpu_tex;
  WGPUTextureView wgpu_view;
  WGPUSampler wgpu_sampler;
  uint32_t target, width, height, depth, internal_format, min_filter,
      mag_filter, wrap_s, wrap_t, wrap_r;
  uint32_t mip_levels;
  float min_lod, max_lod, max_anisotropy;
  bool sampler_dirty, has_data;
} TextureObject;
typedef struct {
  WGPUTexture wgpu_tex;
  WGPUTextureView wgpu_view;
  uint32_t width, height, internal_format, samples;
} RenderbufferObject;
typedef struct {
  uint32_t color_attachments[8];
  uint32_t depth_attachment, stencil_attachment;
  bool is_renderbuffer[10];
  uint32_t num_draw_buffers, draw_buffers[8];
  uint32_t read_buffer;
  /* Layer/level for framebufferTextureLayer */
  int32_t color_tex_levels[8];
  int32_t color_tex_layers[8];
  int32_t depth_tex_level, depth_tex_layer;
  WGPUTextureView color_layer_views[8];   /* cached layer-specific views */
  WGPUTextureView depth_layer_view;
} FramebufferObject;
typedef struct {
  VertexAttribState attribs[WEBGL2_MAX_ATTRIBS];
  uint32_t element_buffer;
} VAOObject;

/* Pipeline cache — dual hash for collision safety */
typedef struct {
  uint64_t hash1, hash2;
  WGPURenderPipeline pipeline;
} PipelineCacheEntry;

typedef struct {
    uint32_t buffer_handles[MAX_TF_BUFFERS];
    uint32_t buffer_offsets[MAX_TF_BUFFERS];
    uint32_t buffer_sizes[MAX_TF_BUFFERS];
    bool active, paused;
    uint32_t primitive_mode;
} TransformFeedbackObject;

typedef struct {
  WGPUSampler wgpu_sampler;
  uint32_t min_filter, mag_filter, wrap_s, wrap_t, wrap_r, compare_mode,
      compare_func;
  float min_lod, max_lod;
  bool dirty;
} SamplerObject;
typedef struct {
  uint32_t target;
  bool active;
  uint32_t result;
  WGPUQuerySet wgpu_query_set;
  WGPUBuffer resolve_buf, readback_buf;
  bool result_available, resolve_pending;
} QueryObject;
typedef struct {
  uint32_t buffer_handle, offset, size;
} UBOBindingPoint;

/* ---- Main context ---- */

typedef struct {
  GpuContext *gpu;

  HandleTable ht_buffers, ht_textures, ht_shaders, ht_programs;
  HandleTable ht_vaos, ht_framebuffers, ht_renderbuffers, ht_samplers,
      ht_queries;

  BlendState blend;
  DepthState depth;
  CullState cull;
  StencilState stencil;
  ClearColorState clear_color;
  float clear_depth;
  int clear_stencil;
  ViewportState viewport;
  bool scissor_test;
  ViewportState scissor;
  bool color_mask[4];
  bool dither;
  bool polygon_offset_fill;
  float polygon_offset_factor, polygon_offset_units, line_width;
  int unpack_alignment, pack_alignment, unpack_row_length, unpack_skip_rows;
  int unpack_skip_pixels, unpack_image_height, unpack_skip_images;
  bool unpack_flip_y, unpack_premultiply_alpha;
  bool sample_alpha_to_coverage, sample_coverage, rasterizer_discard;
  uint32_t last_error, hint_generate_mipmap, hint_frag_derivative;
  float sample_coverage_value;
  bool sample_coverage_invert;

  uint32_t current_program, current_vao, current_array_buffer;
  uint32_t current_element_buffer, current_uniform_buffer;
  uint32_t current_framebuffer, current_read_framebuffer, current_renderbuffer;
  uint32_t active_texture_unit;
  uint32_t bound_textures[WEBGL2_MAX_TEXTURE_UNITS];
  uint32_t bound_samplers[WEBGL2_MAX_TEXTURE_UNITS];
  UBOBindingPoint ubo_bindings[WEBGL2_MAX_UNIFORM_BUFFER_BINDINGS];
  uint32_t default_vao;

  PipelineCacheEntry pipeline_cache[PIPELINE_CACHE_SIZE];
  uint32_t pipeline_cache_count, pipeline_evict_cursor;

  WGPUCommandEncoder frame_encoder;
  WGPURenderPassEncoder frame_pass;
  WGPUTextureView frame_view;
  WGPUTexture frame_texture;
  bool in_frame;

  /* Default depth/stencil for default framebuffer */
  WGPUTexture default_depth_tex;
  WGPUTextureView default_depth_view;
  uint32_t default_depth_width, default_depth_height;

  /* Fallback 1x1 white texture + sampler for unbound texture units */
  WGPUTexture fallback_tex;
  WGPUTextureView fallback_tex_view;
  WGPUSampler fallback_sampler;

  /* Scratch index buffer for TRIANGLE_FAN / LINE_LOOP emulation */
  WGPUBuffer scratch_ibo;
  uint32_t scratch_ibo_capacity; /* bytes */

  /* Blit resources for generateMipmap and blitFramebuffer */
  WGPUShaderModule blit_shader;
  WGPUSampler blit_sampler;

  wasm_memory_t *memory;
  bool strings_inited;
  uint32_t str_version, str_vendor, str_renderer, str_glsl_version,
      str_extensions, str_next;
  uint32_t map_area_base, map_area_next;
  uint32_t active_occlusion_query;
  WGPUQuerySet active_query_set;

  /* Default vertex attribute values (context-global, per GL spec) */
  float default_attrib[WEBGL2_MAX_ATTRIBS][4];
  WGPUBuffer default_attrib_buf; /* single 16*16 byte buffer for all slots */
  bool default_attrib_dirty;
  uint32_t default_read_buffer;
  HandleTable ht_transform_feedbacks;
  uint32_t current_transform_feedback;
  uint32_t default_tf;
  uint32_t map_active_count; /* number of currently mapped buffers */
} WebGL2Context;

/**
 * @brief Initialize the WebGL2 emulation context.
 *
 * Sets up all handle tables, default GL state, fallback textures,
 * and the pipeline cache.
 *
 * @param[out] ctx  Context to initialize.
 * @param[in]  gpu  GPU context providing the wgpu device/queue.
 * @return true on success, false on failure.
 */
bool webgl2_init(WebGL2Context *ctx, GpuContext *gpu);

/**
 * @brief Destroy the WebGL2 context and free all GPU resources.
 * @param[in,out] ctx  Context to destroy.
 */
void webgl2_destroy(WebGL2Context *ctx);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] ctx  Context.
 * @param[in]     mem  Guest WASM memory.
 */
void webgl2_set_memory(WebGL2Context *ctx, wasm_memory_t *mem);

/**
 * @brief Begin a WebGL2 frame (acquire surface texture, create encoder).
 * @param[in,out] ctx  Context.
 */
void webgl2_frame_begin(WebGL2Context *ctx);

/**
 * @brief End a WebGL2 frame (submit command buffer, present surface).
 * @param[in,out] ctx  Context.
 */
void webgl2_frame_end(WebGL2Context *ctx);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * Provides all GL function imports (glDrawArrays, glUniform4f, etc.).
 *
 * @param[in,out] ctx        Context.
 * @param[in]     store      WASM store.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t webgl2_get_imports(WebGL2Context *ctx, wasm_store_t *store,
                          const char ***out_names, wasm_func_t ***out_funcs);

#endif
