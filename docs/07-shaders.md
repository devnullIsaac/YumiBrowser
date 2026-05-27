# Shaders

The shader pipeline uses **Slang** as the shading language, compiled to WGSL at runtime or build time. Built-in shaders live in `sdk/shaders/`.

## Slang Integration

The `SlangBindings` module (`include/slang_bindings.h`, `src/slang_bindings.cpp`) exposes the full Slang compiler API to WASM guests. This enables runtime shader compilation with reflection:

### Typical workflow

```
1. slang_create_global_session()                      → global_session
2. slang_gs_create_session(gs, SLANG_WGSL, ...)      → session
3. slang_session_load_module_from_source(session, ...)→ module
4. slang_module_find_and_check_entry_point(module, ...)→ entry_point
5. slang_session_create_composite(session, [...], 2)  → composite
6. slang_component_link(composite)                    → linked
7. slang_component_get_entry_point_code(linked, 0, 0) → blob (WGSL source)
8. slang_create_shader_module_from_blob(blob, ...)    → gpu_shader_t
9. slang_component_get_layout(linked, 0)              → reflection
10. Query reflection: types, bindings, entry points...
11. Cleanup: destroy blob, components, session, global_session
```

### Supported targets

Yumi targets **WGSL only**. Dawn, the WebGPU implementation used by
the runtime, does not accept SPIR-V, and the other Slang back-ends
(HLSL, GLSL, Metal, DXIL, CUDA) are not wired into the runtime. Slang
is used purely as a higher-level front-end that emits the WGSL Dawn
consumes.

### Reflection API

Full introspection of compiled shaders:
- Types, type layouts, variable bindings
- Entry points, functions, generics
- User attributes, declarations
- Binding locations and buffer layouts

## Build-Time Shader Binding

The `contrib/shader_bind/` tool pre-compiles `.slang` shaders into C header files containing WGSL string literals. Each shader ships as both:

- `shader_name.slang` — Slang source
- `shader_name.h` — Pre-compiled WGSL as a C string constant

## Built-In Shader Library — `sdk/shaders/`

| Shader | Files | Purpose |
|--------|-------|---------|
| **bitmap_text** | `.slang` + `.h` | Bitmap font text rendering |
| **text** | `.slang` + `.h` | Vector/SDF text rendering |
| **text_deco** | `.slang` + `.h` | Text decorations (underline, strikethrough) |
| **rect** | `.slang` + `.h` | Solid colour rectangles |
| **blit_simple** | `.slang` + `.h` | Texture blit (copy) |
| **blit_blend** | `.slang` + `.h` | Texture blit with blend modes |
| **nodegraph_rects** | `.slang` + `.h` | Node rectangles for node graph widget |
| **nodegraph_connections** | `.slang` + `.h` | Bézier connection lines |
| **nodegraph_grid** | `.slang` + `.h` | Background grid pattern |
| **picturebox_blit** | `.slang` + `.h` | Image display with transform |
| **picturebox_controls** | `.slang` + `.h` | Overlay controls for image viewer |
| **picturebox_spectrum** | `.slang` + `.h` | Audio spectrum visualization |
| **tree_lines** | `.slang` + `.h` | Tree view indent/connection lines |
| **ocean** | `.slang` + `.h` | Ocean/water rendering effect |

### Lygia shader library

`sdk/shaders/lygia/` contains the [Lygia](https://lygia.xyz/) shader library — a collection of reusable shader functions for math, lighting, color, SDF, and more. Available as `#include` paths from Slang sources.

## Shader Preview Tool

`contrib/shader_preview/` provides a standalone shader preview application for development — load `.slang` files interactively with hot reload.

## Usage in Widgets

The GUI widgets use the built-in shaders directly. For example:

- **TextBox/TextLabel** use `text.slang` (vector) and `bitmap_text.slang` (bitmap)
- **NodeGraph** uses `nodegraph_rects.slang`, `nodegraph_connections.slang`, `nodegraph_grid.slang`
- **PictureBox** uses `picturebox_blit.slang`, `picturebox_controls.slang`, `picturebox_spectrum.slang`
- **TreeView** uses `tree_lines.slang`
- **GPUSurface** uses `blit_simple.slang` and `blit_blend.slang` for compositing
