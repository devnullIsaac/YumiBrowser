# SDK — Guest-Side WASM Headers

The `sdk/` directory contains all headers that WASM guest modules include. These declare the host imports (functions provided by the runtime) and provide client-side widget/rendering libraries that run entirely within the WASM sandbox.

## Directory Structure

```
sdk/
├── wasm_gpu.h              WebGPU import declarations (handle types + constants)
├── wasm_surface.h          GPUSurface: layer compositing over WGPU
├── wasm_font.h             Font loading, glyph queries, bitmap atlas
├── wasm_text.h             Text shaping (HarfBuzz), BiDi, line/grapheme breaks
├── wasm_ddb.h              DuckDB query/insert/prepared statements
├── wasm_input.h            Keyboard, mouse, text input, cursor control
├── wasm_clipboard.h        System clipboard access (mediated)
├── wasm_sdl.h              SDL3 audio, joystick, gamepad (standalone mode)
├── wasm_media.h            Unified image / video / audio decode → GPU texture, audio, spectrum
├── wasm_image.h            Deprecated shim → wasm_media.h
├── wasm_video.h            Deprecated shim → wasm_media.h
├── wasm_log.h              Host-side logging with levels and macros
├── wasm_slang.h            Slang shader compiler (full reflection)
├── wasm_dashboard.h        Dashboard IPC: clipboard, file dialogs, friends, links
├── wasm_file_types.h       Structs for file dialog and folder scan results
├── wasm_gui.h              Umbrella header for all GUI widgets
├── font_bitmap_atlas.h     CPU-side bitmap glyph atlas
├── font_fallback.h         Lazy font registry with per-font atlas + GPU buffers
├── text_shaping.h          Render-side line shaping (BiDi + shape + atlas lookup)
├── gui/                    Widget toolkit
├── shaders/                Built-in Slang shaders (compiled to WGSL headers)
└── templates/              Starter webapp templates
```

---

## Import Convention

All host imports use the WASM import mechanism:

```c
#define IMPORT __attribute__((import_module("env")))
IMPORT __attribute__((import_name("function_name")))
return_type function_name(args...);
```

The host resolves these during WASM instantiation by matching import names to the binding module's function table.

---

## GPU — `wasm_gpu.h`

Declares integer handle types for every WebGPU object:

```c
typedef uint32_t gpu_device_t;
typedef uint32_t gpu_buffer_t;
typedef uint32_t gpu_texture_t;
typedef uint32_t gpu_render_pipeline_t;
// ... etc
```

Also defines all WebGPU constants (buffer usage, texture usage, load/store ops, blend factors, compare functions, etc.) matching Dawn's values.

---

## GPUSurface — `wasm_surface.h` + `wasm_surface.c`

A layer compositing system built on top of WebGPU. Runs entirely guest-side.

### Architecture

- Every surface owns **two textures** (A + B) for ping-pong rendering
- `tex_a` is always the authoritative content
- `tex_b` is scratch for shader-blend passes
- Hardware blends (normal, additive, replace) draw src onto dst's tex_a
- Shader blends sample both src.tex_a + dst.tex_a, render into dst.tex_b, then copy back

### Usage

```c
GPUSurface bg = GPUSurface_create(800, 600);
GPUSurface ui = GPUSurface_create(800, 600);

GPUSurface_begin_ex(bg, GPU_PASS_CLEAR, GPU_COLOR_BLACK);
  // draw scene
GPUSurface_end(bg);

GPUSurface dst = GPUSurface_bind_swapchain();
GPUSurface_compose(bg, dst);
GPUSurface_blit(ui, NULL, dst, NULL, GPU_BLEND_SCREEN, 0.8f);
GPUSurface_present(dst);
```

Supports 3D compositing with depth-based occlusion.

---

## Font System — `wasm_font.h`, `font_bitmap_atlas.h`, `font_fallback.h`

### wasm_font.h — Host imports

| Function | Description |
|----------|-------------|
| `font_load(data, len)` | Load font from raw bytes |
| `font_load_system(name, len)` | Load font by family name |
| `font_get_metrics(handle, out)` | Get ascender, descender, line height, UPM |
| `font_has_glyph(handle, codepoint)` | Check glyph coverage |
| `font_get_glyph(handle, gid, ...)` | Get glyph outlines (vector) |
| `font_get_glyph_bitmap(handle, gid, px, ...)` | Get rasterized bitmap |

### font_bitmap_atlas.h — Guest-side bitmap atlas

Manages a CPU-side pixel buffer for bitmap-rasterized glyphs. Entries track font handle, glyph ID, pixel offset, dimensions, and metrics. Scratch buffers for rasterization. Dirty flag for GPU upload.

### font_fallback.h — Lazy font registry

The host performs font fallback during text shaping. Each shaped glyph includes the `font_handle` that produced it. The guest-side registry lazily creates atlas + GPU buffer resources the first time a new font_handle appears:

```c
font_fallback_t fb;
fallback_init(&fb, device);
// As glyphs arrive with new font_handles,
// entries are created on demand with their own
// font_atlas_t + GPU buffers.
```

---

## Text — `wasm_text.h`, `text_shaping.h`

### wasm_text.h — Host imports

| Function | Description |
|----------|-------------|
| `text_shape(codepoints, count, size, dir, out, max)` | Shape with HarfBuzz (host does font fallback) |
| `text_bidi(codepoints, count, levels, runs, max)` | ICU BiDi analysis |
| `text_line_breaks(codepoints, count, out)` | Line break opportunities |
| `text_grapheme_breaks(codepoints, count, out)` | Grapheme cluster boundaries |
| `text_set_font(handle)` | Set active font for shaping |

Each shaped glyph is 24 bytes: `{glyph_index, x_advance, x_offset, y_offset, cluster, font_handle}`.

### text_shaping.h — Render-side shaping

Combines BiDi analysis + text shaping + atlas lookup into render-ready glyph data:

1. Run BiDi to get visual runs
2. Shape each run (host handles fallback)
3. Look up atlas entries via font_fallback registry

---

## DuckDB — `wasm_ddb.h`

Guest-side header for the sandboxed DuckDB. String inputs use `(ptr, len)` convention. Outputs use `(out_ptr, out_cap)` with length-query support (pass `out_cap=0` to get length first).

Handle types: `ddb_result_h`, `ddb_prepared_h`, `ddb_appender_h`, `ddb_table_desc_h`, `ddb_extracted_h`.

---

## Dashboard IPC — `wasm_dashboard.h`

Available only to webapps running inside a dashboard slot:

- **Clipboard**: `dashboard_request_paste()` (async), `dashboard_request_copy(ptr, len)` (fire-and-forget)
- **Intra-group links**: `webapp_copy_link()` / `webapp_paste_link()` — shared buffer within a group
- **File dialogs**: `dashboard_open_file_dialog()` / `dashboard_save_file_dialog()` / `dashboard_open_folder_dialog()` — results via export callbacks
- **Friend list**: query and add friends

---

## Widget Toolkit — `sdk/gui/`

A full GUI widget library that runs entirely guest-side, built on GPUSurface + text shaping.

### Widgets

| Widget | File | Description |
|--------|------|-------------|
| Button | `wasm_button.h/c` | Click button |
| TextBox | `wasm_textbox.h/c`, `textbox.h` | Full styled text editor (gap buffer, undo, BiDi, IME) |
| TextLabel | `wasm_textlabel.h/c`, `textlabel.h` | Static text display |
| ListView | `wasm_listview.h/c`, `listview.h` | Scrollable item list |
| TreeView | `wasm_treeview.h/c`, `treeview.h` | Hierarchical tree with expand/collapse |
| PictureBox | `wasm_picturebox.h/c`, `picturebox.h` | Image display with controls |
| DocBox | `wasm_docbox.h/c`, `docbox.h` | Document/rich-text container |
| NodeGraph | `wasm_nodegraph.h/c`, `nodegraph.h` | Visual node graph editor |
| Toolbox | `wasm_toolbox.h/c`, `toolbox.h` | Tool palette |
| Expander | `wasm_expander.h/c` | Collapsible section |
| Scrollbar | `wasm_scrollbar.h/c` | Scrollbar control |
| Slider | `wasm_slider.h/c` | Value slider |
| Divider | `wasm_divider.h/c` | Visual separator |
| MenuBar | `wasm_menubar.h/c` | Top-level menu bar |
| RadioGroup | `wasm_radiogroup.h/c` | Exclusive radio buttons |
| Clay Layout | `wasm_autolayout.h/c`, `clay_layout.h` | Declarative layout system |

### Widget base — `widget.h`

All widgets share a common `WidgetVTable` providing properties (bounds, visibility, opacity, focus, cursor, tooltip) and event handlers (key, mouse, touch, resize, paint, drag-and-drop). Full accessibility support with name and description properties.

### Theme — `yumi_theme.h`

Centralized color constants for the Yumi visual design language:

- **Primary**: warm grey (#d8dad3)
- **Secondary**: near-black (#100e0f)
- **Accent dark**: burnt orange (#5f2803)
- **Accent light**: bright orange (#f49b42)
- Derived colors for backgrounds, foreground, borders, overlays, scrollbars, tree lines

### TextBox architecture

The textbox is a sophisticated styled text editor:

- Dual gap-buffer: one for UTF-32 codepoints, one for per-char `uint16_t` style bitmasks
- Text analysis delegated to `unistring.h` (grapheme clusters, word boundaries, line breaks, BiDi)
- Operation-based undo/redo with grouped atomic operations
- Style system: bold, italic, underline, strikethrough per character
- Render services callback interface for font measurement and shaping

---

## Templates — `sdk/templates/`

| Template | Description |
|----------|-------------|
| `webapp_template.c` | Standalone webapp (SDL input, no dashboard IPC) |
| `dashboard_app_template.c` | Dashboard-hosted webapp (IPC for clipboard, files, friends) |

Both templates stub out all recognized export callbacks. Only `frame()` is required.
