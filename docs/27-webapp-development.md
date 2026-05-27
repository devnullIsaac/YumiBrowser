# Developing Webapps for Yumi Browser

This is the first lecture in a short course on building webapps for Yumi
Browser.  The goal is not a tour of headers but a working understanding
of what a webapp *is*, what the host will and will not do for you, and
how to get from an empty `.c` file to a running sandboxed module.

The dashboard is a separate topic and is deliberately out of scope here.
Everything in this chapter applies to **standalone webapps** — modules
that the runtime loads directly, without dashboard IPC.

---

## 1. What a webapp is, in one paragraph

A Yumi webapp is a **WebAssembly module** compiled from ordinary C (or
any language that can target `wasm32-wasip1`).  The runtime instantiates
the module, wires up a fixed set of host imports (GPU, audio, database,
logging, text shaping, and so on), invokes `init()` once, and then calls
`frame()` on every vsync until you close the window.  There is no DOM,
no JavaScript bridge, no HTML, and no network socket.  A webapp draws
pixels, reads input, persists data in a private DuckDB, and does nothing
else unless the host grants it a capability.

---

## 2. Why WebAssembly, and why C

Three reasons, in order of importance.

1. **Safety.**  The module cannot reach outside its linear memory.  It
   cannot open arbitrary files, cannot call the operating system, cannot
   leak memory into the host process, and cannot crash the browser by
   dereferencing a bad pointer.  The worst a malicious or buggy webapp
   can do is produce wrong pixels and make the runtime return early.
2. **Portability.**  The same `.wasm` binary runs on Linux, Windows,
   macOS, and eventually on mobile.  You do not ship per-platform
   builds.
3. **Performance.**  WebAssembly runs at roughly native speed for
   compute and memory-bound code, and all the expensive work — GPU,
   decoding, SQL, font shaping — happens in native host code anyway.

C is the language of the SDK headers because it has a stable ABI, a
predictable memory model, and compiles to WASM with almost zero
overhead.  You can use C++, Rust, Zig, or anything else that speaks the
same import/export convention.  The examples in this course are C.

---

## 3. The host/guest contract

Everything a webapp can do with the outside world is expressed as a
host *import*.  Everything the runtime can ask the webapp to do is
expressed as a guest *export*.  There is no other interface.

### 3.1 Imports — what the host provides

Imports are declared with an attribute that tells the WASM linker the
symbol is external and will be provided by the host at instantiation:

```c
#define IMPORT __attribute__((import_module("env")))

IMPORT __attribute__((import_name("log_write")))
void log_write(const char *ptr, int len);
```

When the module is instantiated the host walks the import section and
binds each entry to a C function in one of its binding modules
(`log_bindings.c`, `wgpu_bindings.c`, `duckdb_bindings.c`, and so on).
If an import name is misspelled the instantiation fails loudly — there
is no fallback.

### 3.2 Exports — what the host expects

Exports are declared on the guest side with a companion attribute:

```c
__attribute__((export_name("frame")))
void frame(void) { /* draw one frame */ }
```

The runtime looks for these names by convention.  A missing export is
usually tolerated (the runtime simply does not call it) unless it is
the required `frame` export.

### 3.3 Memory

The WASM module owns a single contiguous block of linear memory, often
just called "the heap".  When you pass a pointer to the host, you are
passing a 32-bit offset into that block.  The host translates it to a
native pointer using the memory that was registered during setup.  This
has two important consequences.

* **Pointers must be live for the duration of the host call.**  If the
  host reads a string you allocated with `malloc`, that string must
  still exist when the call returns.  Fire-and-forget calls (async
  requests) either copy the data or require a lifetime policy.
* **The host never keeps pointers.**  Every piece of data the host needs
  to retain is copied out of the guest immediately.  This is why most
  APIs are `(ptr, len)` in and `(out_ptr, out_cap)` out — there is no
  stable address the host could hold on to.

---

## 4. The two lifecycle exports

A standalone webapp has two obligatory moments in its life.

### 4.1 `init` — once, at startup

`init()` runs after the module is fully instantiated and memory is
wired up.  Do your one-time setup here: load fonts, create GPU
pipelines, open database tables, allocate atlases.  You can call any
host import; imports are always available.

```c
__attribute__((export_name("init")))
void init(void) {
    log_write("hello from init", 15);
    my_app_boot();
}
```

### 4.2 `frame` — once per vsync

`frame()` is the only export the runtime truly requires.  It runs at
the display refresh rate (usually 60 Hz).  Everything — simulation,
rendering, input response — happens inside `frame`.  There is no
separate update loop because there is no useful distinction in a
sandboxed module; the runtime already owns the clock.

```c
__attribute__((export_name("frame")))
void frame(void) {
    update_simulation();
    render_to_swapchain();
}
```

If `frame` takes longer than a vsync, the display drops a frame and
carries on.  The runtime will not terminate you for being slow — you
simply look janky.

---

## 5. Input exports

Input is **push-based**: the host calls the guest with each event.
This is the opposite of the classic "poll an event queue" model and is
chosen deliberately.  A polled queue forces every webapp to call into
the host on every frame even if nothing happened; a push model lets the
runtime ignore webapps that do not care about, say, gamepads.

Every input export is **optional**.  If you do not declare it, the
runtime simply does not send those events.  The standard set lives in
[sdk/templates/webapp_template.c](../sdk/templates/webapp_template.c):

| Export | When it fires |
|---|---|
| `on_key(scancode, keycode, mod, pressed)` | Physical key up/down |
| `on_char(codepoint)` | Committed Unicode codepoint (post-IME) |
| `on_mouse_button(btn, pressed, x, y)` | Mouse click |
| `on_mouse_motion(x, y, dx, dy, btns)` | Pointer move |
| `on_mouse_wheel(dx, dy)` | Scroll |
| `on_touch(finger, type, x, y, pressure)` | Touchscreen event |
| `on_joystick_*` / `on_gamepad_*` | HID game controllers |
| `on_resize(w, h)` | Viewport resized |
| `on_focus(gained)` | Window focus state changed |

Pair these with the pull-based `wasm_input.h` imports when you need to
read current state — for example, "is Shift held right now" inside
`frame()`.  The two styles coexist.

---

## 6. The minimum viable webapp

Strip everything away and a complete webapp looks like this.

```c
#include "wasm_gpu.h"
#include "wasm_surface.h"
#include "wasm_log.h"

__attribute__((export_name("init")))
void init(void) {
    GPUSurface_init();
    log_write("init", 4);
}

__attribute__((export_name("frame")))
void frame(void) {
    GPUSurface dst = GPUSurface_bind_swapchain();
    GPUSurface_clear_color(dst, GPU_COLOR_BLACK);
    GPUSurface_present(dst);
}
```

That is a black window at 60 Hz.  Everything we cover in the next four
chapters is about replacing those three lines of clearing with
something interesting.

---

## 7. Building and running

### 7.1 Toolchain

Any clang with a `wasm32-wasip1` target works.  A typical command line:

```bash
clang \
  --target=wasm32-wasip1 \
  -O2 -flto \
  -nostdlib -Wl,--no-entry \
  -Wl,--export=init -Wl,--export=frame \
  -I /path/to/yumibrowser/sdk \
  -o myapp.wasm \
  myapp.c sdk/wasm_surface.c
```

The important flags:

* `--target=wasm32-wasip1` selects the WASM backend.
* `-nostdlib -Wl,--no-entry` stops the linker from looking for `_start`;
  Yumi webapps are libraries, not programs.
* `-Wl,--export=<name>` forces the export to survive LTO.  The attribute
  `export_name` is often enough, but some configurations strip
  unreferenced exports without help from the linker.
* You link `sdk/wasm_surface.c` in because `wasm_surface.h` ships with a
  real implementation, not just declarations.  Other SDK headers are
  header-only.

### 7.2 Running

Webapp `.wasm` files live under the `webapps/` tree and are loaded
by the `WebAppRuntime` subsystem.  You can drop a binary in and launch
it directly; no registration step is required for local development.

### 7.3 Debugging

The guest has no debugger attached, but three tools close the gap.

1. `log_write`, `log_int`, `log_fmt_int` from `wasm_log.h` — these are
   your `printf`.
2. Assertions go through `log_assert`, which the host will turn into a
   visible trap.
3. If you need a memory snapshot, take one on the host side; the
   runtime knows which `wasm_memory_t` belongs to which webapp.

---

## 8. What this course covers

The next four chapters go one layer at a time.

* **[28 — Wasm API Surface](28-wasm-api-surface.md)** — a map of every
  SDK header and what each one is for, with just enough detail that you
  can pick the right tool for a task.
* **[29 — GPU, Surfaces, and Drawing](29-gpu-and-surfaces.md)** — from
  `wgpu_get_device()` to a shader-blended overlay, with an emphasis on
  the GPUSurface compositor.
* **[30 — Data, Text, and Media](30-data-text-media.md)** — DuckDB,
  HarfBuzz-backed text shaping, the font system, and image/video
  decoding.
* **[31 — Putting It Together](31-putting-it-together.md)** — a single
  worked example that touches most of the API.

Read them in order the first time.  After that, they are reference.
