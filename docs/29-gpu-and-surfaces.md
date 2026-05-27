# GPU, Surfaces, and Drawing

A webapp that cannot draw is not much of a webapp.  This chapter is
the longest of the course because drawing is the largest API surface
and because almost every non-trivial decision you will make about your
app turns out, eventually, to be a drawing decision.

We will work from the bottom up.  First the raw WebGPU layer
(`wasm_gpu.h`).  Then the compositor that sits on top of it
(`wasm_surface.h`).  Then shaders, and finally a practical pattern for
organising a render loop.

---

## 1. Why WebGPU

Yumi's drawing API is WebGPU, not OpenGL.  There is exactly one
reason: **WebGPU is the only modern graphics API with a sandbox
story**.  Every other option either leaks driver state into the guest
(OpenGL, Vulkan) or requires a separate rendering process with heavy
IPC (Metal, D3D12).  WebGPU was designed from day one to be driven
from untrusted code, and the browser vendors have already done the
validation work.

Practically, this means:

* No undefined behaviour.  Every out-of-range index, every buffer
  overrun, every format mismatch becomes a validation error, not a GPU
  hang.
* No driver crash takes down the process.  The worst a webapp can do
  with the GPU is slow down its own frame.
* The mental model is closer to modern APIs (pipelines, bind groups,
  command encoders) than to legacy GL.  If you have written Vulkan or
  Metal, WebGPU will feel familiar.

---

## 2. The handles

A WebGPU program is a dance among a small set of handles.  They are
all declared in [wasm_gpu.h](../sdk/wasm_gpu.h).

| Handle | What it is |
|---|---|
| `wgpu_device_t` | The logical GPU.  You get it from `wgpu_get_device()`.  There is one per runtime. |
| `wgpu_queue_t` | The submission queue.  You get it from `wgpu_get_queue()`.  There is one per device. |
| `wgpu_shader_t` | A compiled shader module, created from WGSL source. |
| `wgpu_buffer_t` | A GPU-visible buffer (vertex, index, uniform, or storage). |
| `wgpu_texture_t` | A 2D texture.  `wgpu_texture_view_t` is a view into it. |
| `wgpu_sampler_t` | Filtering/wrapping state for sampling textures. |
| `wgpu_pipeline_t` | A fully baked render or compute pipeline. |
| `wgpu_bind_group_t` | A set of bound resources (buffers, textures, samplers). |
| `wgpu_render_pass_t` | An active render pass encoder.  Short-lived. |

You will notice there is no *command encoder* handle.  The SDK wraps
command encoding inside the surface API and inside the render pass
handle — guests rarely need to see raw encoders.  If you do need one,
`GPUSurface_begin_ex` gives you a render pass whose implicit encoder
is submitted when you call `GPUSurface_end`.

---

## 3. The minimal WebGPU program

Here is the skeleton of a webapp that draws a single triangle using
`wasm_gpu.h` alone, with no compositor.  Read the comments.

```c
#include "wasm_gpu.h"
#include "wasm_log.h"

static wgpu_device_t   dev;
static wgpu_queue_t    q;
static wgpu_pipeline_t pipe;
static wgpu_buffer_t   vbuf;

static const char WGSL[] =
    "struct VSOut { @builtin(position) pos: vec4f, @location(0) col: vec3f };"
    "@vertex fn vs(@location(0) p: vec2f, @location(1) c: vec3f) -> VSOut {"
    "  return VSOut(vec4f(p, 0.0, 1.0), c);"
    "}"
    "@fragment fn fs(in: VSOut) -> @location(0) vec4f {"
    "  return vec4f(in.col, 1.0);"
    "}";

__attribute__((export_name("init")))
void init(void) {
    dev = wgpu_get_device();
    q   = wgpu_get_queue();

    wgpu_shader_t sh = wgpu_create_shader_wgsl(dev, WGSL, sizeof WGSL - 1);

    // Two attributes: vec2 position, vec3 color — interleaved, 20 bytes/stride.
    wgpu_pipeline_desc_t pd = { /* see wasm_gpu.h for the full struct */ };
    pd.shader = sh;
    pd.vertex_entry   = "vs";
    pd.fragment_entry = "fs";
    pipe = wgpu_create_pipeline(dev, &pd);

    wgpu_shader_release(sh);   // pipeline holds its own ref

    float verts[] = {
        /* pos     */ -0.5f, -0.5f, /* col */ 1,0,0,
                       0.5f, -0.5f,         0,1,0,
                       0.0f,  0.5f,         0,0,1,
    };
    vbuf = wgpu_create_buffer(dev, sizeof verts, WGPU_USAGE_VERTEX | WGPU_USAGE_COPY_DST);
    wgpu_buffer_write(q, vbuf, 0, verts, sizeof verts);
}

__attribute__((export_name("frame")))
void frame(void) {
    // Open a render pass on the swapchain via the surface layer.
    GPUSurface dst = GPUSurface_bind_swapchain();
    GPUSurface_clear_color(dst, GPU_COLOR_BLACK);
    wgpu_render_pass_t rp = GPUSurface_begin(dst, GPU_PASS_CLEAR, GPU_COLOR_BLACK);

    wgpu_pass_set_pipeline(rp, pipe);
    wgpu_pass_set_vertex_buffer(rp, 0, vbuf, 0, sizeof (float) * 15);
    wgpu_pass_draw(rp, 3, 1, 0, 0);

    GPUSurface_end(dst);
    GPUSurface_present(dst);
}
```

The important observations:

* **The device, queue, and pipelines are created once in `init`.**
  They are expensive.
* **The vertex buffer is also created once.**  Per-frame buffer
  allocation is a performance mistake in every GPU API; WebGPU is no
  exception.
* **The render pass lives entirely inside `frame`.**  It opens, you
  issue draw calls against it, it closes.  Passes never straddle
  frames.
* **The swapchain is reached through the compositor.**  You could
  bypass `GPUSurface_bind_swapchain` and talk to the raw WebGPU surface
  functions (`wgpu_get_surface_format`, `wgpu_get_surface_width`,
  `wgpu_get_surface_height`), but there is rarely a reason.

---

## 4. The surface compositor

`wasm_surface.h` is where the architecture gets interesting.  A
`GPUSurface` is a handle to a double-buffered texture pair — call them
A and B — with a built-in compositor that knows 31 blend modes, depth
occlusion, and scratch surfaces for post-processing.

### 4.1 Why ping-pong

Modern blend modes — *multiply*, *overlay*, *soft-light*, *screen* —
cannot be expressed as hardware alpha-blending.  They need to read the
destination pixel, run a formula, and write a new pixel.  That means a
shader that *samples* the destination, which means the destination
cannot simultaneously be the render target.

The compositor solves this with A/B textures.  "Authoritative" content
lives in A.  When you ask for a shader blend, the GPU reads A, writes
the blended result into B, and the SDK copies the affected rectangle
back into A.  From your perspective the surface is a single logical
image; the two textures are invisible.  Hardware-friendly blends
(`GPU_BLEND_NORMAL`, `GPU_BLEND_ADD`, `GPU_BLEND_REPLACE`) skip the
copy for speed.

### 4.2 Layers as a design discipline

The compositor encourages a layered way of thinking.  You rarely draw
directly to the swapchain.  Instead you build the frame in pieces:

```c
GPUSurface bg     = GPUSurface_create(W, H);  // 3D scene
GPUSurface ui     = GPUSurface_create(W, H);  // 2D widgets
GPUSurface tooltip= GPUSurface_create(W, H);  // occasional overlay

GPUSurface_begin_ex(bg, GPU_PASS_CLEAR, GPU_COLOR_BLACK);
  /* render scene */
GPUSurface_end(bg);

GPUSurface_begin_ex(ui, GPU_PASS_CLEAR, GPU_COLOR_TRANSPARENT);
  /* render widgets */
GPUSurface_end(ui);

GPUSurface dst = GPUSurface_bind_swapchain();
GPUSurface_clear_color(dst, GPU_COLOR_BLACK);
GPUSurface_compose(bg, dst);                            // fully opaque
GPUSurface_blit(ui, NULL, dst, NULL, GPU_BLEND_NORMAL, 1.0f);
if (show_tooltip)
    GPUSurface_blit(tooltip, NULL, dst, NULL, GPU_BLEND_NORMAL, 0.95f);
GPUSurface_present(dst);
```

Three things to notice.

1. **`GPUSurface_compose` is the fast path.**  It is a straight copy;
   use it when you know the source is fully opaque and covers the
   destination.
2. **`GPUSurface_blit` is the general path.**  It takes source/dest
   rects, a blend mode, and an opacity.  Rects are `NULL` to mean
   "whole surface".
3. **Layers are recreated only when they resize.**  Keep them in
   global state.  Creating a surface every frame defeats the entire
   purpose.

### 4.3 The 31 blend modes

The full list is in `wasm_surface.h` with the prefix `GPU_BLEND_`.
They divide into three groups.

| Group | Examples | Cost |
|---|---|---|
| Hardware | `NORMAL`, `ADD`, `REPLACE`, `MULTIPLY_ALPHA` | Free |
| Shader, one-pass | `MULTIPLY`, `SCREEN`, `OVERLAY`, `DARKEN`, `LIGHTEN`, … | One ping-pong copy |
| Shader, expensive | `SOFT_LIGHT`, `COLOR_DODGE`, `HUE`, `SATURATION`, `COLOR`, `LUMINOSITY` | One ping-pong copy + heavier math |

If you never leave the hardware group, the compositor is almost free
and the SDK behaves like a simple layer-stack.

### 4.4 3D compositing

`GPU_PASS_3D` enables a depth buffer on the surface.  You can mix 3D
geometry with 2D widgets by drawing *occluders* — invisible
depth-writing rectangles that define where the 3D content is allowed
to peek through.  The typical recipe is in the header (look for the
"3D compositing" comment block).  The occluder system is what makes
things like a floating UI inspector over a running 3D scene possible
without abandoning the compositor.

---

## 5. Shaders

The only shading language the runtime accepts is **WGSL**.  Dawn — the
WebGPU implementation Yumi uses — does not ingest SPIR-V, so there is
no `wgpu_create_shader_spirv`, no GLSL compiler, no HLSL bridge.  You
either write WGSL by hand or you compile something else *down to*
WGSL before you ship.

### 5.1 Writing WGSL by hand

For small effects, inline WGSL as in the triangle example is the
path of least resistance.  The language is small, well-specified, and
pleasant once you get used to the `@location(n)` / `@group(n)
@binding(n)` sigils.

### 5.2 Slang, compiled to WGSL

For shaders that grow past a screenful, [wasm_slang.h](../sdk/wasm_slang.h)
exposes the full Slang compiler to the guest, with reflection.  Slang
gives you generics, interfaces, modules, and an entry-point model that
scales to large material systems — and it targets WGSL directly.  A
webapp can ship a single `.slang` file and compile it at startup, or
pre-compile it at build time and ship the resulting WGSL blob.

Reflection — parameter names, types, bind-group slots — is exposed so
you can build a uniform editor or a shader debugger without re-parsing
the source.

### 5.3 `shader-bind`: automated binding wrappers

Writing the CPU-side glue for a shader is the boring, error-prone part
of graphics programming.  Uniform buffer layouts, bind-group slots,
vertex attribute formats — every one of them is a place where a
single-character typo becomes a validation error at draw time.

The [`shader-bind`](../contrib/shader_bind/) CLI solves this.  Point
it at a `.slang` or `.wgsl` file and it emits a C header that declares
strongly-typed structs for every uniform block, constants for every
binding slot, and a small `setup_*` function that creates the pipeline
and bind group layout for you.  Your webapp includes the generated
header and calls `MyShader_create(dev)`; everything else is done.

The typical build-time flow is:

```bash
shader-bind my_shader.slang -o my_shader.gen.h
clang --target=wasm32-wasip1 -O2 -o app.wasm app.c  # includes my_shader.gen.h
```

### 5.4 `shader-preview`: interactive prototyping

[`shader-preview`](../contrib/shader_preview/) is a native tool (not
a webapp) for iterating on a Slang shader in a tight feedback loop.
It opens a window, live-recompiles the shader on save, and lets you
tweak uniforms with a sidebar.  Use it to get the pixels right, then
freeze the `.slang` into your webapp and wrap it with `shader-bind`.

The two tools are designed to work together: prototype in
`shader-preview`, generate wrappers with `shader-bind`, consume in
your webapp.

---

## 6. Render loop patterns

A few patterns that recur across real Yumi webapps.

### 6.1 Dirty rendering

You do not have to redraw every frame.  If your UI has not changed,
you can detect that at the top of `frame()` and simply present the
swapchain without touching anything else.  This is not merely a
battery optimisation; it dramatically lowers the thermal budget on
laptops and phones, which in turn lets you run more webapps at once.

```c
void frame(void) {
    if (!state_is_dirty) {
        GPUSurface_present(GPUSurface_bind_swapchain_nocopy());
        return;
    }
    /* full redraw */
    state_is_dirty = false;
}
```

### 6.2 Scratch surfaces

`GPUSurface_acquire_scratch` and `GPUSurface_release_scratch` loan you
a surface from a pool.  Use them for one-off post-effects — a blur
pass, a thumbnail, a transient mask.  The pool keeps allocation out of
the frame path.

### 6.3 Resize

`on_resize(w, h)` fires whenever the window size changes.  Two things
need to happen there, *not* inside `frame()`:

1. **Tell the swapchain surface its new size.**  `GPUSurface_bind_swapchain()`
   captures the viewport width and height at the moment it is called and
   caches them on the surface object.  Drawing primitives — including
   `GPUSurface_blit()` — clip destination rectangles against that cached
   size.  If the host window grows and you do nothing, blits to the
   newly-revealed area are silently clipped away; if the window shrinks,
   draws can land outside the live framebuffer.  The fix is one line:

   ```c
   __attribute__((export_name("on_resize")))
   void on_resize(int w, int h) {
       GPUSurface_resize(swapSurface, (uint32_t)w, (uint32_t)h);
       /* propagate to your layout / offscreen surfaces here */
   }
   ```

   `GPUSurface_resize` is cheap when the new size fits in the existing
   over-allocation (no GPU texture reallocation), and reallocates only
   when the window grows past the slack or shrinks well below it.

2. **Recreate or resize your own offscreen layers.**  The swapchain
   surface is the only one that *needs* a resize call from you on every
   change; offscreen surfaces you created with `GPUSurface_create(W, H)`
   keep whatever size they were given.  Either call `GPUSurface_resize`
   on each, or destroy and re-create them, depending on whether you want
   to preserve their contents.

A symptom that uniquely fingerprints "I forgot to call
`GPUSurface_resize` on the swapchain": layout-driven widgets in the
right half of the window disappear when you widen the window, or pile
on top of each other when you narrow it.  The layout engine is
positioning them correctly; the blits are being clipped by stale
swapchain dimensions.

---

## 7. Profiling and debugging

Shader bugs and validation errors surface through the host's log.
Always run with `wgpu_device_poll()` enabled early in development —
it forces the WebGPU backend to flush its error queue synchronously so
you see problems in the right place.

GPU timing is not exposed to the guest today.  If you need frame-level
profiling, lean on `log_fmt_int` with wallclock timestamps captured
from the host via `sdl_get_ticks()`.

---

Next: [30 — Data, Text, and Media](30-data-text-media.md).
