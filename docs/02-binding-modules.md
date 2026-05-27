# Binding Modules

Every host capability exposed to WASM guests is implemented as a **binding module**. Each module follows the same pattern:

1. A state struct (e.g. `WgpuBindings`, `FontBindings`)
2. `*_init()` — allocate handle tables and internal state
3. `*_set_memory()` — set the WASM linear memory pointer
4. `*_get_imports()` — return arrays of (name, wasm_func_t) pairs for WASM instantiation
5. `*_destroy()` — release all resources

All modules are instantiated **per-webapp** (not global singletons), ensuring isolation.

---

## WebGPU Bindings — `include/wgpu_bindings.h`, `src/wgpu_bindings.c`

Exposes the full WebGPU API to WASM guests via integer handles.

### Handle tables

| Table | Object type |
|-------|-------------|
| `ht_device` | WGPUDevice |
| `ht_queue` | WGPUQueue |
| `ht_buffer` | WGPUBuffer |
| `ht_texture` | WGPUTexture |
| `ht_texture_view` | WGPUTextureView |
| `ht_sampler` | WGPUSampler |
| `ht_shader` | WGPUShaderModule |
| `ht_bind_group_layout` | WGPUBindGroupLayout |
| `ht_bind_group` | WGPUBindGroup |
| `ht_pipeline_layout` | WGPUPipelineLayout |
| `ht_render_pipeline` | WGPURenderPipeline |
| `ht_compute_pipeline` | WGPUComputePipeline |
| `ht_command_encoder` | WGPUCommandEncoder |
| `ht_render_pass` | WGPURenderPassEncoder |
| `ht_compute_pass` | WGPUComputePassEncoder |
| `ht_command_buffer` | WGPUCommandBuffer |
| `ht_surface` | WGPUSurface |
| `ht_query_set` | WGPUQuerySet |

### Offscreen mode

When used by a `WebAppRuntime`, the bindings operate in **offscreen mode**: the guest renders to a texture owned by the dashboard, not the real swapchain. The dashboard composites all offscreen textures onto the window surface.

Pre-allocated handles (`h_device`, `h_queue`, `h_surface`) give the guest immediate access to the shared GPU resources.

## Font Bindings — `include/font_bindings.h`, `src/font_bindings.c`

Host-side font management backed by **FreeType**.

- Load fonts by name (system font lookup) or by file path
- Glyph queries (existence, metrics, outlines, bitmap rasterization)
- System font directory discovery
- Fontconfig integration for codepoint-based font search
- Font data access for HarfBuzz (cross-module with text bindings)

---

## Text Bindings — `include/text_bindings.h`, `src/text_bindings.c`

Text shaping with **HarfBuzz** and Unicode BiDi via **ICU**.

- Multi-font support with active-font selector
- Host-side font fallback: when the active font lacks a glyph, the host finds a system font automatically
- Each shaped glyph includes the `font_handle` that produced it
- ICU BiDi analysis (UAX#9)
- Line break and grapheme cluster boundary detection
- Reusable UTF-16 conversion buffer for ICU interop

---

## DuckDB Bindings — `include/duckdb_bindings.h`, `src/duckdb_bindings.c`

Exposes DuckDB operations to WASM guests through integer handles.

### Handle tables

| Table | Object type |
|-------|-------------|
| `ht_result` | duckdb_result |
| `ht_prepared` | duckdb_prepared_statement |
| `ht_appender` | duckdb_appender |
| `ht_table_desc` | duckdb_table_description |
| `ht_extracted` | duckdb_extracted_statements |

The host owns the database and connection. The guest can only manipulate results, prepared statements, and appenders. Each webapp gets its own sandboxed DuckDB file.

---

## Image Bindings — `include/image_bindings.h`, `src/image_bindings.c`

Still and animated image decode via **FFmpeg** (and optionally **LibRaw** for camera RAW).

- Open by file path or WASM memory buffer
- Decode all frames (GIF, APNG, WebP animate)
- Upload as BGRA WebGPU textures
- Returns `gpu_texture_view_t` handles for rendering

---

## Video Bindings — `include/video_bindings.h`, `src/video_bindings.c`

Streaming video decode via **FFmpeg** with hardware acceleration.

- Open by file path or WASM memory buffer
- Zero-copy HW decode via `WFFBridge` (VAAPI → DMA-BUF on Linux, VideoToolbox → IOSurface on macOS, D3D11VA on Windows)
- Software fallback when HW accel unavailable
- Audio playback via SDL audio device
- 12-band audio spectrum analysis for visualizations
- Per-handle mute/volume control

---

## WFF Bridge — `include/wgpu_ffmpeg.h`, `src/wgpu_ffmpeg.c`

Zero-copy bridge between FFmpeg hardware-decoded frames and WebGPU.

| Platform | HW API | Import mechanism |
|----------|--------|-----------------|
| Linux | VAAPI | DMA-BUF → `WGPUSharedTextureMemoryDmaBuf` |
| macOS | VideoToolbox | IOSurface → `WGPUSharedTextureMemoryIOSurface` |
| Windows | D3D11VA | DXGI Shared Handle → `WGPUSharedTextureMemoryDXGISharedHandle` |

Supports NV12, P010, BGRA, RGBA pixel layouts. Automatic access management (begin/end) is optional.

---

## Clipboard Bindings — `include/clipboard_bindings.h`, `src/clipboard_bindings.c`

Three imports: `clipboard_available`, `clipboard_get`, `clipboard_set`. The dashboard mediates all access — no data leaks without user knowledge.

---

## Input Bindings — `include/input_bindings.h`, `src/input_bindings.c`

Keyboard, mouse, joystick, gamepad, and touch input. Handle tables for joysticks and gamepads. Text input / IME control (start, stop, set cursor area).

---

## SDL Bindings — `include/sdl_bindings.h`, `src/sdl_bindings.c`

Direct SDL3 audio, joystick, gamepad, and keyboard state access for standalone webapps (non-dashboard mode). Handle tables for audio devices, audio streams, joysticks, and gamepads.

---

## Slang Bindings — `include/slang_bindings.h`, `src/slang_bindings.cpp`

Full-parity bindings for the **Slang** shader compiler. Exposes global sessions, compilation sessions, modules, entry points, linking, code generation, and the complete reflection API (types, layouts, variables, bindings, entry points, functions, generics, declarations, user attributes).

Handle tables: `ht_global_session`, `ht_session`, `ht_component`, `ht_blob`, `ht_reflection`.

---

## Log Bindings — `include/log_bindings.h`, `src/log_bindings.c`

Simple logging: `log_write`, `log_write_level`, `log_int`, `log_float`, `log_assert`. Output goes to host stdout.
