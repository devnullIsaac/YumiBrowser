# Data, Text, and Media

A webapp that can draw but cannot *read* or *store* anything is a
screensaver.  This chapter covers the rest of the sandbox — the host
services that turn pixels into an application.  We move from structured
data, through text rendering, to bitmap and streaming media.

---

## 0. Webapps as a Business Logic Layer

Before we touch an API, a word about *what* a webapp should be.

In the classic three-tier split — presentation, business logic, data
— Yumi hands you the presentation layer (GPU, input, compositor) and
the data layer (DuckDB, assets) essentially pre-built.  What a webapp
mostly *is*, in this model, is the **business logic layer**: the
rules, state machines, and domain operations that sit between user
intent and persisted state.

This is not an accidental framing.  It has consequences for how you
should organise the code you write:

1. **Treat the database as the source of truth**, not a cache.  Your
   in-memory state exists to serve the current frame; the row in
   DuckDB is the real thing.  When in doubt, re-read.
2. **Keep domain operations pure and small.**  "Add a note", "rename a
   tag", "mark this frame watched" should each be one function that
   takes arguments, runs one SQL statement, and returns.  The
   rendering code should never know how they are implemented.
3. **Let the presentation layer stay dumb.**  The compositor knows
   how to draw a list; it must not know what the list *means*.  If
   your draw code is full of `if (note.is_archived && user.is_admin)`
   you have leaked business logic upward and it will hurt later.
4. **Resist the temptation to be clever at the edges.**  The SDK
   already handles GPU submission, font shaping, video decode, and
   SQL.  You are not there to re-implement any of that.  You are
   there to decide, for your particular application, *what* gets
   drawn, *when* it gets persisted, and *why*.

Everything that follows — DuckDB, fonts, images, video — is easier to
use well if you have already decided which layer of your webapp each
piece of state belongs to.

---

## 1. DuckDB: the only storage API

### 1.1 Why a database, not a filesystem

Yumi webapps do not get a filesystem.  Every storage need — a preferences
file, a save game, a cache of downloaded thumbnails, a log of past
sessions — is expressed as rows in [DuckDB](https://duckdb.org).  There
are three reasons for this choice.

1. **Queryability.**  A webapp usually wants to read a *slice* of its
   data, not a whole file.  A database gives you that slice for free.
2. **Schema evolution.**  `ALTER TABLE` is a well-understood mechanism
   for upgrading a webapp across versions.  Binary file formats are
   not.
3. **Sandbox simplicity.**  One well-audited import surface
   (`wasm_ddb.h`) replaces the dozens of syscalls that a full
   filesystem API would need.  Each webapp gets exactly one private
   database; there is no way to reach another webapp's data, and no
   way to reach the host's.

### 1.2 The DuckDB sandbox

DuckDB is by default a very capable piece of software — it can read
CSVs from disk, attach remote databases, load extensions, and so on.
**None of that is available to a webapp.**  The host opens each
guest's connection with a locked-down configuration and then
*freezes* it:

* **No host filesystem access.**  The DuckDB `read_csv`,
  `read_parquet`, `COPY ... FROM '...'`, and `ATTACH 'file.db'`
  surfaces are disabled.  You cannot reach outside the private
  database, and a malicious `SELECT` cannot exfiltrate anything
  because there is nothing outside to read.
* **Resource constraints.**  Each webapp's database is capped in
  both on-disk size and working-set memory.  Runaway aggregations
  fail loudly rather than eating the machine's RAM; oversized
  tables are rejected before they grow past the group policy's
  quota.
* **Configuration is frozen.**  `SET`, `PRAGMA`, and
  `duckdb_config_*` have no effect after the connection is handed
  to the guest.  You cannot re-enable extensions, you cannot raise
  the memory limit, you cannot change the file search paths.  The
  configuration the host chose is the configuration you get, for
  the entire lifetime of the webapp.

What is left is still DuckDB — the optimiser, the columnar execution
engine, the full SQL dialect, and crucially the **multi-threaded
native execution**.  And that last point matters for how you design
the webapp.

### 1.3 Push work into SQL

A WebAssembly module runs on one guest thread with no SIMD guarantee.
DuckDB, on the host, runs with the full machine's thread count, the
full machine's SIMD width, and hardware-tuned columnar kernels.

The practical consequence is that **SQL is almost always faster than
a hand-written loop over the same data in WebAssembly**, provided the
operation can be expressed as SQL.  Filters, joins, aggregations,
windowed analytics, sorting, top-N — all of these are dramatically
cheaper done inside the engine than emitted as rows and processed in
C.

A rule of thumb: if a computation reads more than a few thousand
rows, try to express it as SQL first.  Reach for chunk-style
iteration only when you actually need per-row work that SQL cannot
do — such as building a GPU vertex buffer.  Running your business
logic as SQL is not a gimmick; it is how the runtime was designed to
be used.

### 1.4 The two APIs

The SDK exposes DuckDB through two programming styles that can be
freely mixed.

**Cell-style** is the obvious one: you get a row count and a column
count, and you ask for the value at `(row, col)`.

```c
ddb_result r = ddb_query(conn, "SELECT name, score FROM highscores");
int n = ddb_row_count(r);
for (int i = 0; i < n; i++) {
    char name[64];
    ddb_varchar(r, 0, i, name, sizeof name);
    int64_t score = ddb_int64(r, 1, i);
    /* ... */
}
ddb_destroy_result(r);
```

This is fine for small result sets — a few hundred rows of
configuration, the contents of a settings panel, the most-recent-20 of
anything.  It allocates and it copies strings, but you rarely notice.

**Chunk-style** is the performant one.  DuckDB is a columnar engine
and it wants to hand you columns, not rows.  You iterate over *chunks*
— batches of up to 2048 rows — and read straight out of each column's
vector.

```c
ddb_result r = ddb_query(conn, "SELECT x, y, z FROM points");
ddb_chunk ch;
while ((ch = ddb_fetch_chunk(r)).handle) {
    int rows = ddb_chunk_size(ch);
    const double *xs = ddb_chunk_column_f64(ch, 0);
    const double *ys = ddb_chunk_column_f64(ch, 1);
    const double *zs = ddb_chunk_column_f64(ch, 2);
    for (int i = 0; i < rows; i++) {
        plot(xs[i], ys[i], zs[i]);
    }
    ddb_chunk_destroy(ch);
}
ddb_destroy_result(r);
```

The convenience macro `DDB_FOREACH_CHUNK` writes the `while` loop and
the destroy for you.  Reach for chunks when you are processing
thousands of rows per frame or writing an aggregation pipeline.

### 1.5 Prepared statements and the appender

Never concatenate SQL.  The SDK has prepared statements for queries
that take parameters, and an *appender* for bulk inserts.  The
appender is an order of magnitude faster than a series of
`INSERT` statements and about three orders of magnitude safer than
building SQL with `snprintf`.

See [wasm_ddb.h](../sdk/wasm_ddb.h) for the full API — `ddb_prepare`,
`ddb_bind_*`, `ddb_execute_prepared`, `ddb_appender_create`,
`ddb_appender_append_*`, `ddb_appender_end_row`, `ddb_appender_close`.

---

## 2. Text

Text is an enormous subject and the SDK splits it into two halves that
you almost always use together.

* [wasm_font.h](../sdk/wasm_font.h) is about *fonts*: loading them,
  inspecting them, rasterising glyphs.
* [wasm_text.h](../sdk/wasm_text.h) is about *strings*: shaping them
  into glyph runs, running bidirectional reordering, finding line and
  grapheme breaks.

Together they give you proper internationalised typography.  The
combination is explicitly designed to render Arabic, Devanagari,
Chinese, and emoji correctly, not just Latin-1.

### 2.1 Loading fonts

Two paths:

```c
font_handle f = font_load("assets/Inter.ttf", 18);    // bundled file
font_handle s = font_load_system("Noto Sans", 18);     // host lookup
```

The system lookup walks the host's font catalogue using the same
rules the desktop environment uses.  If the font does not exist you
get `0`.

### 2.2 Shaping

Shaping turns a Unicode string into a sequence of glyph IDs with
positions.  This is where HarfBuzz earns its keep — ligatures, kerning,
contextual forms, all of it.

```c
shaped_glyph glyphs[256];
int n = text_shape(f, "fi — ḿəgħrūf", -1, glyphs, 256);
for (int i = 0; i < n; i++) {
    font_handle which = glyphs[i].font_handle;  // fallback may kick in
    draw_glyph(which, glyphs[i].glyph_id, glyphs[i].x, glyphs[i].y);
}
```

The `font_handle` inside each shaped glyph is important: when the
requested font does not have a glyph (say, the string contains CJK but
the font is a Latin one), the shaper transparently falls back to a
system font and records which one it used.  The webapp never has to
carry a fallback chain itself.

### 2.3 BiDi and breaks

Right-to-left mixed text needs to be *reordered* before shaping.
`text_bidi` runs the Unicode Bidirectional Algorithm and returns a
visual-order buffer.  `text_line_breaks` tells you where line breaks
are *allowed*; the actual wrapping decision is yours.
`text_grapheme_breaks` is what you walk when the user presses
backspace — one grapheme cluster, not one codepoint, not one byte.

### 2.4 The atlas

Rasterising a glyph on every frame is wasteful.  The SDK ships a
bitmap atlas in [font_bitmap_atlas.h](../sdk/font_bitmap_atlas.h) that
caches glyph bitmaps in a GPU texture.  It is header-only and entirely
optional; most webapps will use it unchanged.

### 2.5 A note on `font-paint-cli`

You will find a tool called [`font-paint-cli`](../contrib/font-paint-cli/)
in the repo.  It is the spiritual cousin of `shader-bind`: point it at
a font file and it emits a C header containing SDF (signed distance
field) glyph data for a chosen codepoint range, ready to be drawn by a
Slang shader.  It was useful when Yumi had no first-class text API.

It is now **largely deprecated**.  `wasm_font.h` and `wasm_text.h`
together give you correct shaping, BiDi, fallback, and a GPU atlas
without any per-webapp font pre-processing, which is almost always
what you want.  Reach for `font-paint-cli` only if you have an
unusual requirement — a custom SDF effect shader, say, or a need to
ship glyphs for a script the system fonts do not cover.  For ordinary
text, use the SDK path.

---

## 3. Images, video, and audio (one API)

[wasm_media.h](../sdk/wasm_media.h) is the unified decoder. It accepts a
byte buffer, sniffs the magic bytes, and routes it to the right path:
still / animated images (PNG, JPEG, GIF, WebP, AVIF, HEIC, JPEG XL, JPEG
2000, BMP, TIFF, ICO, OpenEXR, DPX, PSD, plus camera RAW when LibRaw is
built in), streaming video / containers (MP4, MOV, MKV / WebM, AVI,
FLV, MPEG-TS, …), and standalone audio (WAV, MP3, FLAC, Ogg). There is
no path-based variant — the host filesystem is not part of the surface.
The legacy `wasm_image.h` and `wasm_video.h` are now thin shims that
forward to this header and emit deprecation warnings.

After `media_open()`, call `media_kind()` to learn how to drive the
handle.

### Still / animated images

```c
media_handle_t img = media_open(png_bytes, png_len);
if (media_kind(img) == MEDIA_KIND_IMAGE) {
    int w = media_width(img);
    int h = media_height(img);
    int nframes = media_frame_count(img);
    for (int i = 0; i < nframes; ++i) {
        gpu_texture_view_t v = media_frame_view(img, i);
        /* render v for media_frame_duration_ms(img, i) ms ... */
    }
}
media_close(img);
```

Frames are decoded eagerly at `media_open()` time and cached on the
GPU, so `media_frame_view()` is cheap and the views remain valid until
`media_close()`. `media_has_alpha()` reports whether the source format
carries transparency.

### Video

Video is a streaming API, not a one-shot decode. The same `media_open()`
call returns a handle whose `media_kind()` is `MEDIA_KIND_VIDEO`; pump
it one frame at a time:

```c
media_handle_t v = media_open(mp4_bytes, mp4_len);

while (playing) {
    int got = media_decode_next(v);
    if (got == 1) {
        gpu_texture_view_t view = media_texture_view(v);
        draw_with_texture(view);
    } else if (got == 0) {
        break; /* end of stream */
    } else {
        break; /* -1 = error */
    }
    present();
}

media_close(v);
```

`media_decode_next` is **tri-state**: `1` means a new frame is ready,
`0` means end-of-stream, `-1` means an error. The audio track of a
video handle (if present) is mixed automatically by the host;
`media_audio_set_muted` / `media_audio_set_volume` control it, and
`media_audio_get_spectrum` returns a `MEDIA_SPECTRUM_BANDS`-band
logarithmic FFT of the device-wide post-mix output for visualisers.

### Standalone audio

When `media_kind()` is `MEDIA_KIND_AUDIO`, the host pumps audio
autonomously. The guest only manages mute, volume, and the spectrum.
Use this for music players, podcasts, and sound-only tracks.

---

## 4. Audio (the other kind)

For non-video audio — sound effects, generated tones, user-recorded
samples — you open an SDL audio stream through
[wasm_sdl.h](../sdk/wasm_sdl.h).  A stream has a format, a sample
rate, and a put/get pair for pushing PCM.  It is less convenient than
video's built-in audio but it is the right tool for interactive
sound.

```c
sdl_audio_stream_t s = sdl_audio_stream_open_default(
    SDL_AUDIO_FORMAT_F32, /*channels*/ 2, /*hz*/ 48000);
sdl_audio_stream_resume(s);

float buf[4096];
synthesize(buf, 4096);
sdl_audio_stream_put(s, buf, sizeof buf);
```

Keep the stream alive across frames.  Do not open one per sound
effect; mix them yourself and push the result.

---

## 5. Clipboard

The clipboard is three functions: `clipboard_available()` asks if
there is text to read, `clipboard_get(buf, cap)` copies it out,
`clipboard_set(ptr, len)` puts text on the clipboard.  Strings are
UTF-8.  The interface is deliberately tiny because the clipboard is
the last shared surface between webapps and the outside world, and
widening it would widen the privacy story.

---

## 6. Logging

`log_write(ptr, len)` is the `printf` of a webapp.  `log_fmt_int` and
`log_fmt_float` handle basic formatting without pulling in a runtime.
`log_assert(cond_ptr, cond_len, expr_ptr, expr_len)` traps the module
with a message, which is how you want fatal bugs to surface in
development.

There is no `log_write_json` or `log_write_event`.  Structured
telemetry, when you want it, goes into your own DuckDB table where it
can be queried like anything else.

---

## 7. Bundling assets

A webapp has no filesystem.  If you need to ship a shader, an image,
a font, or a SQL seed script *inside* the `.wasm` binary, you have to
convert the file to C source at build time and link it in.

The standard Unix tool for this is **`xxd -i`**, which turns any file
into a C array plus a length:

```bash
xxd -i assets/logo.png > assets/logo.png.h
```

Produces roughly:

```c
unsigned char assets_logo_png[] = { 0x89, 0x50, 0x4e, 0x47, /* ... */ };
unsigned int  assets_logo_png_len = 12345;
```

Include the header, hand the pointer and length to `media_open()` (or
any other host import that accepts an in-memory byte range), and you
are done.  The same approach works for Slang/WGSL shader sources, JSON
configuration, test fixtures, and anything else your webapp wants to
carry without talking to the outside world.

Some tools already emit header files directly — `shader-bind` for
shaders, `font-paint-cli` for SDF font atlases — so you do not always
reach for `xxd` by hand.  But when no specialised tool exists, `xxd
-i` (or the equivalent in your build system) is the fallback.  There
is no host import for "read a file out of the bundle"; bundling is
always a compile-time operation.

---

Next: [31 — Putting It Together](31-putting-it-together.md).
