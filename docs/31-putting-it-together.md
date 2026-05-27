# Putting It Together

We end the course the same way every programming course should end:
with a single small application that exercises most of what we have
covered and nothing we have not.  The application is a **note-taking
webapp** that lets the user type styled notes, stores them in a
private DuckDB, and renders them with real typography.  It is about
250 lines of C in total.

The complete sources live under
[sdk/templates/webapp_template.c](../sdk/templates/webapp_template.c);
what follows is a commentary on the design, not a line-by-line
transcription.

---

## 1. The problem

We want a webapp that:

1. Draws a list of notes on the left and the selected note's body on
   the right.
2. Lets the user click a note to select it, and type into the body
   area with full Unicode input (including IME, BiDi, and system
   fonts).
3. Persists every edit to DuckDB so the notes survive a restart.
4. Uses zero network I/O and no filesystem.

Nothing here is novel.  That is the point — this is a tour of
familiar territory using our API.

---

## 2. State

```c
static font_handle   g_font;
static ddb_conn      g_db;
static GPUSurface    g_list_layer;
static GPUSurface    g_body_layer;

static int           g_selected_id = -1;
static char          g_body_buf[64 * 1024];
static int           g_body_len;
static int           g_body_dirty;
```

Four host handles — a font, a database connection, two offscreen
surfaces — and a handful of plain old C scalars.  The rule of thumb is
that handles stay in global state; everything else is either per-frame
or per-event.

---

## 3. `init`: one-time setup

```c
__attribute__((export_name("init")))
void init(void) {
    log_write("notes webapp starting", 22);

    GPUSurface_init();
    g_list_layer = GPUSurface_create(300, 800);
    g_body_layer = GPUSurface_create(900, 800);

    g_font = font_load_system("Inter", 16);
    if (!g_font) g_font = font_load_system("sans-serif", 16);

    g_db = ddb_open("notes.duckdb");
    ddb_exec(g_db,
        "CREATE TABLE IF NOT EXISTS notes("
        " id INTEGER PRIMARY KEY,"
        " title VARCHAR,"
        " body VARCHAR,"
        " updated_at TIMESTAMP DEFAULT now())");

    load_first_note();
}
```

Four things worth pointing out.

1. **Fallback font.**  We ask for Inter and settle for the system
   sans.  The shaper will do its own fallbacks on top of this for
   codepoints the font does not cover.
2. **`CREATE TABLE IF NOT EXISTS` is how you do migrations day one.**
   When you need to evolve the schema, add a version column and
   conditional `ALTER TABLE`s.  You do not need a migration framework.
3. **The database path is a name, not a path.**  The host resolves it
   inside the webapp's private data directory.  There is no `../` and
   no way to escape.
4. **`log_write` at startup.**  It is good practice.  Your log lines
   are how the host tells you what went wrong in the field.

---

## 4. `frame`: the render loop

```c
__attribute__((export_name("frame")))
void frame(void) {
    if (g_body_dirty) {
        save_body();
        g_body_dirty = 0;
    }

    render_list(g_list_layer);
    render_body(g_body_layer);

    GPUSurface dst = GPUSurface_bind_swapchain();
    GPUSurface_clear_color(dst, GPU_COLOR_WHITE);
    GPUSurface_blit(g_list_layer, NULL, dst, &RECT(0,  0, 300, 800),
                    GPU_BLEND_NORMAL, 1.0f);
    GPUSurface_blit(g_body_layer, NULL, dst, &RECT(300, 0, 900, 800),
                    GPU_BLEND_NORMAL, 1.0f);
    GPUSurface_present(dst);
}
```

The shape is deliberate:

* A dirty flag guards the save.  We do not write to DuckDB on every
  keystroke; we write when we draw.  If you *really* care about
  durability, move the write into the keystroke handler.
* Each layer renders to its own offscreen and is blitted to the
  swapchain.  This is cheap (the blits are hardware-blend `NORMAL`)
  and it makes it easy to cache the list layer if we ever want to.
* The swapchain is cleared before compositing.  Always.  Stale pixels
  from the previous frame have never made anyone happy.

`render_list` iterates a DuckDB query chunk by chunk and calls
`draw_text_run` for each title.  `render_body` wraps the body buffer
with `text_line_breaks`, shapes each line with `text_shape`, and feeds
the resulting glyphs into the bitmap atlas.  We are not reproducing
those 80 lines here because they are ordinary — the architecture is
the lesson, not the glyph loop.

---

## 5. Input

```c
__attribute__((export_name("on_char")))
void on_char(uint32_t cp) {
    if (g_selected_id < 0) return;
    if (g_body_len + 4 >= (int)sizeof g_body_buf) return;
    g_body_len += utf8_encode(&g_body_buf[g_body_len], cp);
    g_body_dirty = 1;
}

__attribute__((export_name("on_key")))
void on_key(int sc, int kc, int mod, int pressed) {
    if (!pressed) return;
    if (kc == KEY_BACKSPACE) backspace_one_grapheme();
    if (kc == KEY_ESCAPE)    g_selected_id = -1;
}

__attribute__((export_name("on_mouse_button")))
void on_mouse_button(int btn, int pressed, int x, int y) {
    if (btn == 1 && pressed && x < 300) select_note_at_y(y);
}
```

Small, boring, total.  This is what we want event handlers to look
like — each export does *one* thing, state changes are explicit, and
anything expensive is deferred to `frame`.

`on_char` and `on_key` are both present because they do different
things.  `on_char` gives you the post-IME codepoint that the user
meant to type (good for inserting into a buffer).  `on_key` gives you
the raw key event (good for Backspace, Escape, modifier shortcuts).

---

## 6. Persistence

```c
static void save_body(void) {
    ddb_stmt s = ddb_prepare(g_db,
        "UPDATE notes SET body = ?, updated_at = now() WHERE id = ?");
    ddb_bind_varchar(s, 0, g_body_buf, g_body_len);
    ddb_bind_int32  (s, 1, g_selected_id);
    ddb_execute(s);
    ddb_stmt_destroy(s);
}
```

Three lines of API, one transactional write, no string concatenation,
no escape characters to get wrong.  This is what DuckDB buys you.

---

## 7. Resize

```c
__attribute__((export_name("on_resize")))
void on_resize(int w, int h) {
    int list_w = w / 4;
    int body_w = w - list_w;
    GPUSurface_destroy(g_list_layer);
    GPUSurface_destroy(g_body_layer);
    g_list_layer = GPUSurface_create(list_w, h);
    g_body_layer = GPUSurface_create(body_w, h);
}
```

Tearing down and rebuilding the layers is the right answer.  Do not
try to call `GPUSurface_resize` on offscreens unless you have
measured; a fresh surface is almost always cheaper than a resize in
our implementation.

---

## 8. Everything we did not use

Worth inventorying, because a production webapp usually reaches for
one of these eventually.

* **Gamepad / joystick** — `on_gamepad_added`, `sdl_gamepad_*`.  For
  games and media players.
* **Touch** — `on_touch`.  Already implied for mobile; most desktop
  webapps can ignore it.
* **Media (images, video, audio)** — `wasm_media.h`.  Buffer-only,
  format-sniffing decoder for anything visual or audible.
* **Slang shader compiler** — `wasm_slang.h`.  For webapps that ship
  user-editable shaders.
* **Clipboard** — `wasm_clipboard.h`.  A four-line patch if the notes
  app needs copy-paste.
* **Clay layout, the GUI toolkit** — `sdk/gui/`.  The widget library
  turns the hand-rolled list rendering above into half a dozen lines
  with a `ListView`.

We left them out because the point of the exercise was to show the
bones.  Once the bones feel familiar, reach for the toolkit.

---

## 9. What you should take away

1. **The runtime is your event loop.**  You do not own `main`.  You
   export `frame`, `init`, and whatever event callbacks you care about,
   and the host drives you.
2. **Handles are global; data is local.**  Fonts, surfaces, database
   connections live across frames.  Almost everything else is
   per-frame or per-event.
3. **DuckDB is the filesystem.**  Stop looking for `fopen`.
4. **The compositor is the window system.**  You do not draw directly
   to the display; you draw to offscreens and compose.
5. **Input is push; state is pull.**  Both are available; use the one
   that fits the moment.
6. **The sandbox is a feature.**  The limits — no sockets, no
   threads, no inter-webapp calls — are what let users run your code
   without reading it first.

That is the whole of Yumi webapp development.  Everything else in the
docs is reference for when you need a specific function.  Build
something.
