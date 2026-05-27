# The WASM API Surface

The previous chapter ended with a three-line webapp that cleared the
screen to black.  Before we make it do anything more interesting, we
need a map of the territory.  This chapter is that map.

Every capability the host offers is declared in a header under
[sdk/](../sdk).  Each header is self-contained — you include exactly
the ones you use, and the unused imports cost you nothing at runtime
because WebAssembly linking is by name.  Think of the SDK as an à la
carte menu, not a framework.

---

## 1. The thirteen headers

Here is the entire surface, grouped by role.  We will cover each group
in detail across the next three chapters.

### 1.1 Drawing

| Header | Role |
|---|---|
| [wasm_gpu.h](../sdk/wasm_gpu.h) | Low-level WebGPU: devices, queues, buffers, textures, pipelines, render passes |
| [wasm_surface.h](../sdk/wasm_surface.h) | High-level compositor built on top of `wasm_gpu.h`: offscreen layers, 31 blend modes, 3D depth compositing, swapchain |
| [wasm_slang.h](../sdk/wasm_slang.h) | Host-side Slang shader compiler with full reflection |

### 1.2 Input and windowing

| Header | Role |
|---|---|
| [wasm_sdl.h](../sdk/wasm_sdl.h) | SDL3 subsystems: displays, keyboard state, mouse, joystick, gamepad, audio streams |
| [wasm_input.h](../sdk/wasm_input.h) | Polling-style input: current key state, mod state, mouse position, cursor control, warp, relative mode |
| [wasm_clipboard.h](../sdk/wasm_clipboard.h) | System clipboard: `clipboard_available`, `clipboard_get`, `clipboard_set` |

### 1.3 Text and media

| Header | Role |
|---|---|
| [wasm_font.h](../sdk/wasm_font.h) | Font loading (file or system), metrics, glyph outlines, glyph bitmaps |
| [wasm_text.h](../sdk/wasm_text.h) | HarfBuzz shaping, ICU BiDi, line and grapheme break iteration |
| [wasm_media.h](../sdk/wasm_media.h) | Unified buffer-only decode for images (PNG, JPEG, GIF, WebP, AVIF, HEIC, JPEG XL, JPEG 2000, BMP, TIFF, RAW, …), video (MP4, MKV, WebM, AVI, FLV, TS, …) with frame stepping / seeking / texture views, and standalone audio (WAV, MP3, FLAC, Ogg) with mute / volume / spectrum |
| [wasm_image.h](../sdk/wasm_image.h) | Deprecated compatibility shim around `wasm_media.h` (legacy `image_*` names) |
| [wasm_video.h](../sdk/wasm_video.h) | Deprecated compatibility shim around `wasm_media.h` (legacy `video_*` names) |

### 1.4 Storage, networking, and diagnostics

| Header | Role |
|---|---|
| [wasm_ddb.h](../sdk/wasm_ddb.h) | Embedded DuckDB: connections, prepared statements, chunked result streaming |
| [wasm_network.h](../sdk/wasm_network.h) | Peer-to-peer messaging and data-shard transfer using opaque peer IDs |
| [wasm_log.h](../sdk/wasm_log.h) | Structured logging and assertions |

### 1.5 Dashboard IPC (out of scope)

| Header | Role |
|---|---|
| [wasm_dashboard.h](../sdk/wasm_dashboard.h) | Only available when the module runs *inside* the dashboard; not usable from a standalone webapp |

We will not discuss dashboard IPC further in this course.  It is listed
here only so you know not to reach for it.

---

## 2. The two programming models

You will notice that the SDK offers both *push* and *pull* styles for
input, and both *chunk* and *cell* styles for DuckDB.  This is
intentional.  Each pair exists because real applications need both
temperaments:

* **Push** (callback-based) is right when you want to *react* to
  something rare.  A keystroke, a window resize, a gamepad plug-in.
* **Pull** (polling-based) is right when you want to *sample* state
  during your frame.  "Is the W key held?" "Where is the mouse right
  now?"

The same logic applies to data:

* **Chunk iteration** is the efficient path for large result sets.  You
  walk the columns directly.
* **Cell access** is the convenient path for small results.  You ask
  for row *r* column *c* and get a value.

You are expected to mix styles fluently.  Nothing in the SDK punishes
you for it.

---

## 3. Handle conventions

Almost every long-lived host resource is represented on the guest side
by an opaque handle — a typedef'd integer, usually 32-bit.  The rules
are uniform across the SDK:

1. **Zero is always invalid.**  If a creation function returns `0`, it
   failed.  Check it.
2. **Handles are owned by you.**  If you create a buffer, you are
   responsible for destroying it.  The host will not garbage-collect.
3. **Handles are not pointers.**  Do not cast them, do not do
   arithmetic on them.  They index into a host-side table.
4. **Handles survive `frame()` boundaries.**  You can keep them in
   global state across frames.  They do *not* survive a module reload.

`GPUSurface` is the one exception — it is a pointer to a small struct
the SDK owns inside the guest's own memory, because the compositor
needs a bit of guest-visible state (`width`, `height`, `tex_a`,
`tex_b`).  You still never dereference it yourself.

---

## 4. String and buffer ABI

The host cannot read your null-terminated strings; it does not know
where they end without walking them and there is no reason to pay that
cost.  The SDK therefore uses `(ptr, len)` pairs everywhere:

```c
log_write("hello", 5);                 // input:  (ptr, len)
int n = clipboard_get(buf, sizeof buf); // output: (ptr, cap) -> length
```

For outputs, the host writes up to `cap` bytes and returns the number
of bytes actually written.  If the return value equals `cap` the buffer
may have been truncated; resize and try again.  If the return value is
negative, something went wrong (usually "no data available").

For large or variable-length outputs — query results, glyph bitmaps,
video frames — the host will generally give you a way to ask the size
first, then hand you a buffer to copy into.  The pattern is consistent
enough that once you have written three of them you can guess the
fourth.

---

## 5. What you *cannot* do

It is worth stating the limits plainly:

* You cannot open sockets.  There is no POSIX networking in the
  sandbox.  Everything a webapp might want to do over the network —
  share state, sync with peers, stream media — goes through the Yumi
  group model, which is a host service not a webapp capability.
* You cannot open arbitrary files.  The database is the storage API.
* You cannot spawn threads.  A webapp is single-threaded.  This is
  fine: all the heavy work (GPU submission, video decode, font
  shaping) happens on host threads that you invoke synchronously.
* You cannot call into other webapps.  Isolation is strict.
* You cannot call `exit()`, `abort()`, or anything that terminates the
  process.  `log_assert` is the nearest thing to a controlled abort and
  it traps only your module.

These restrictions are the shape of the sandbox.  They are why the
rest of the system can trust you.

---

## 6. Reading the headers

Every SDK header is aggressively commented with Doxygen-style blocks.
When in doubt, open the header.  A typical entry looks like:

```c
/**
 * @brief Write a frame to the audio stream.
 * @param stream  Audio stream handle (non-zero).
 * @param pcm     Pointer to interleaved samples in the stream's format.
 * @param bytes   Number of bytes in @p pcm.
 * @return        Number of bytes actually queued, or -1 on error.
 */
IMPORT __attribute__((import_name("sdl_audio_stream_put")))
int sdl_audio_stream_put(sdl_audio_stream_t stream,
                         const void *pcm, int bytes);
```

The prose in the header is the authoritative spec.  This course gives
you the shape of things; the header gives you the exact contract.

---

Next: [29 — GPU, Surfaces, and Drawing](29-gpu-and-surfaces.md).
