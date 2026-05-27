# Host Runtime

The host runtime is the native C process that owns the SDL3 window, the WebGPU device, and all Wasmer-based WASM execution.

## Entry Point — `src/main.c`

1. **Crypto init** — `yumi_crypto_init()` loads OpenSSL + oqs-provider.
2. **CLI parsing** — `--webapp <path>` for single-app dev mode, `--data-dir <path>` to override XDG.
3. **Data directory resolution** — checks `$XDG_DATA_HOME/com.yumi.browser`, then `~/.local/share/com.yumi.browser`, then `release/` next to the binary.
4. **SDL3 init** — video, joystick, gamepad, audio subsystems.
5. **GPU init** — `gpu_init()` creates the wgpu-native device, surface, and rectangle pipeline.
6. **Dashboard init** — `dashboard_init()` sets up the supervisor runtime.
7. **Webapp loading** — either a single `--webapp` in dev mode, or auto-discovery from `dashboard/` and `webapps/` subdirectories.
8. **Main loop** — `SDL_PollEvent` when focused, `SDL_WaitEventTimeout` when unfocused (power saving). All events go through `dashboard_dispatch_event()`.

## GpuContext — `include/gpu.h`, `src/gpu.c`

The GPU context wraps wgpu-native and provides:

| Component | Description |
|-----------|-------------|
| `WGPUInstance`, `WGPUAdapter`, `WGPUDevice`, `WGPUQueue` | Core WebGPU objects |
| `WGPUSurface` | Window surface for presentation |
| `WGPURenderPipeline` | Simple coloured-rectangle pipeline |
| `WGPUBuffer vertices[]` | Dynamic vertex batch (max 65536 vertices, 6 floats each: x, y, r, g, b, a) |
| `clear_color[4]` | Per-frame clear colour |

### Frame lifecycle

```c
gpu_set_clear_color(&gpu, 0.1f, 0.1f, 0.1f, 1.0f);
if (gpu_frame_begin(&gpu)) {
    gpu_push_rect(&gpu, x, y, w, h, r, g, b, a);  // queue geometry
    gpu_frame_end(&gpu);                             // upload, submit, present
}
```

`gpu_resize()` reconfigures the surface after window resize events.

## DashboardRuntime — `include/dashboard_runtime.h`, `src/dashboard_runtime.c`

The dashboard is the **privileged supervisor** that mediates between the native OS and sandboxed webapps.

### Responsibilities

- **Tiling** — up to `DASHBOARD_MAX_SLOTS` (16) webapp slots, each with a viewport and offscreen texture.
- **Compositing** — `GpuCompositor` blits offscreen webapp textures onto the real swapchain.
- **Group management** — up to `DASHBOARD_MAX_GROUPS` (32) peer groups, each with a `yumi_client_t`, connection state, and per-group DuckDB.
- **Clipboard mediation** — webapps request paste/copy via IPC; the dashboard is designed to prevent silent data exfiltration across webapp boundaries.
- **File dialogs** — native open/save/folder dialogs, results delivered asynchronously to the requesting webapp.
- **Friend list** — up to 256 friends, stored with peer ID, display name, and timestamp.
- **Settings DB** — privileged DuckDB for global settings (not accessible to webapps).
- **Recovery mode** — greyed-out views for groups in read-only recovery.
- **Folder scans** — streaming directory iteration for file-manager webapps (up to 4 concurrent).

### Slot structure

```
WebAppSlot {
    WebAppRuntime *rt;       // the sandboxed WASM instance
    WebAppViewport viewport; // x, y, w, h in window pixels
    bool focused;
    uint32_t group_index;    // which DashboardGroup owns this slot
    WGPUTexture offscreen_tex;
    WGPUTextureView offscreen_view;
}
```

### IPC flow

Webapps call dashboard IPC imports (e.g. `dashboard_request_paste`). The host enqueues an `IPCRequest`, confirms with the user if needed, then delivers the result by calling the webapp's `on_paste_result` / `on_file_result` / `on_friend_list_result` export.

### Views

| Enum | Description |
|------|-------------|
| `DASHBOARD_VIEW_NORMAL` | Tiled webapp view |
| `DASHBOARD_VIEW_SETTINGS` | Settings panel |
| `DASHBOARD_VIEW_RECOVERY` | Recovery mode |

## WebAppRuntime — `include/webapp_runtime.h`, `src/webapp_runtime.c`

Each webapp runs in its own Wasmer instance with isolated state.

### What webapps CAN access (via imports)

- WebGPU rendering (offscreen texture)
- Per-webapp sandboxed DuckDB
- Font loading and text shaping
- Image and video decode
- Logging
- Input events (translated coordinates)
- Clipboard (mediated by dashboard)
- Dashboard IPC (file dialogs, friend list, link buffer)
- Slang shader compiler

### What webapps are not granted access to

- SDL window / audio device creation
- Filesystem
- Raw network sockets
- Other webapps' memory or databases

### Per-instance bindings

Each `WebAppRuntime` holds its own instances of all binding modules (`WgpuBindings`, `DuckdbBindings`, `FontBindings`, `TextBindings`, `InputBindings`, `VideoBindings`, `ImageBindings`, `ClipboardBindings`, `SlangBindings`, `LogBindings`). These are NOT globals — they are per-webapp.

### Guest exports

| Export | Required | Description |
|--------|----------|-------------|
| `frame()` | Yes | Called every frame |
| `init()` | No | Called once after instantiation |
| `on_key(scancode, keycode, mod, pressed)` | No | Key press/release |
| `on_char(codepoint)` | No | Text input |
| `on_mouse_button(button, pressed, x, y)` | No | Mouse click |
| `on_mouse_motion(x, y, dx, dy, buttons)` | No | Mouse move |
| `on_mouse_wheel(dx, dy)` | No | Scroll |
| `on_touch(finger_id, type, x, y, pressure)` | No | Touch input |
| `on_resize(w, h)` | No | Viewport resize |
| `on_focus(gained)` | No | Focus change |
| `on_file_result(info_ptr)` | No | File dialog result |
| `on_paste_result(info_ptr)` | No | Clipboard paste result |
| `on_friend_list_result(peer_ids_ptr, count)` | No | Friend list query result |

## WasmRuntime — `include/runtime.h`, `src/runtime.c`

A simpler, standalone WASM runtime used for single-webapp dev/testing mode. Shares the same export signature as `WebAppRuntime` but without dashboard IPC or offscreen rendering. Renders directly to the swapchain.

## HandleTable — `include/handle_table.h`

All host objects crossing the WASM boundary use integer handles, never raw pointers. The `HandleTable` provides:

- 1-indexed handles (0 = null/invalid)
- Automatic growth when full (capacity doubles)
- Per-slot generation counter for use-after-free detection
- Freelist-based slot recycling

```c
HandleTable ht;
htable_init(&ht, 64);
uint32_t h = htable_insert(&ht, my_ptr);  // → non-zero handle
void *got = htable_get(&ht, h);            // → my_ptr
htable_remove(&ht, h);                     // recycles slot
```
