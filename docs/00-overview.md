# Yumi Browser — Architectural Overview

Yumi Browser is a native, peer-to-peer application platform built in C11. It runs sandboxed **WASM webapps** inside a GPU-accelerated host, connected through post-quantum encrypted peer groups.

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        main.c  (Entry Point)                        │
│  SDL3 window + event loop, XDG data dir resolution, CLI args        │
└────────────────────┬───────────────────┬────────────────────────────┘
                     │                   │
          ┌──────────▼──────────┐  ┌─────▼──────────────────────┐
          │     GpuContext      │  │   DashboardRuntime          │
          │  (gpu.h / gpu.c)    │  │  (dashboard_runtime.h/c)   │
          │  wgpu-native device │  │  Tiling supervisor, IPC,   │
          │  surface, pipeline  │  │  friend list, clipboard,   │
          │  immediate-mode     │  │  settings DB, compositing  │
          └──────────┬──────────┘  └─────┬──────────────────────┘
                     │                   │
                     │        ┌──────────▼──────────────────┐
                     │        │    WebAppRuntime (×N)        │
                     │        │  (webapp_runtime.h/c)        │
                     │        │  Per-webapp Wasmer instance   │
                     │        │  Offscreen texture target     │
                     │        │  Per-instance binding modules │
                     │        └──────────┬──────────────────┘
                     │                   │
                     ▼                   ▼
          ┌──────────────────────────────────────────────┐
          │           Host Binding Modules                │
          │  wgpu, gles, font, text, image, video,       │
          │  duckdb, clipboard, input, sdl, slang, log   │
          └──────────────────────────────────────────────┘
                              │
         ┌────────────────────┤
         ▼                    ▼
  ┌──────────────┐   ┌───────────────────────────────┐
  │     SDK      │   │    Network + Crypto Layer      │
  │  sdk/ folder │   │  group_registrar, yumi_client, │
  │  Guest-side  │   │  sudp, udp, bbr congestion     │
  │  WASM headers│   │  Post-quantum (ML-DSA, ML-KEM) │
  └──────────────┘   └───────────────────────────────┘
```

## Three Pillars

| Pillar | Location | Role |
|--------|----------|------|
| **Host Runtime** | `src/`, `include/` | Native C process: SDL window, GPU, Wasmer, binding modules |
| **SDK** | `sdk/` | Guest-side C headers for WASM webapps (imports + widgets) |
| **Network / Crypto** | `src/crypto/`, `src/network/`, `src/group_registrar/`, `include/network/` | P2P group governance, encrypted transport, congestion control |

## Key Design Decisions

- **WASM sandboxing via Wasmer**: Every webapp is a `.wasm` module. No filesystem, no raw sockets. All capabilities are host-provided imports.
- **WebGPU (wgpu-native)**: GPU rendering goes through Dawn/wgpu, not OpenGL. A full WebGL2 → WebGPU translation layer exists for legacy code.
- **Post-quantum cryptography**: ML-DSA-87 signatures, ML-KEM-1024 key encapsulation, Threefish-1024 symmetric encryption, Skein-1024 hashing. All via OpenSSL + oqs-provider.
- **DuckDB for storage**: Both host settings and per-webapp sandboxed databases use embedded DuckDB.
- **Handle tables**: All GPU/SDL/DuckDB objects crossing the WASM boundary use integer handles via `HandleTable`, never raw pointers.
- **Meson build system**: Single `meson.build` at the root, deps in `deps/`.

## Directory Map

| Path | Contents |
|------|----------|
| `src/` | Host-side implementation (runtime, bindings, crypto, networking) |
| `include/` | Public headers for host-side modules |
| `sdk/` | Guest-side WASM headers, GUI widgets, shaders, templates |
| `deps/` | Vendored dependencies (SDL, wgpu, DuckDB, FreeType, HarfBuzz, etc.) |
| `webapps/` | Example/built-in webapp source code |
| `build/` | Meson build output |
| `release/` | Shared libraries and distributable assets |

## Related Documentation

- [Host Runtime](01-host-runtime.md) — Main loop, GPU, dashboard, webapp lifecycle
- [Binding Modules](02-binding-modules.md) — WGPU, GLES, font, text, image, video, DuckDB, etc.
- [SDK](03-sdk.md) — Guest-side WASM headers and widget toolkit
- [Cryptography](04-cryptography.md) — Post-quantum crypto abstraction layer
- [Group Registrar](05-group-registrar.md) — P2P group governance
- [Networking](06-networking.md) — UDP transport, SUDP encryption, BBR congestion control, Yumi Client
- [Shaders](07-shaders.md) — Slang shader pipeline and built-in shader library
- [Social Design](26-social-design.md) — Why Yumi is built around groups