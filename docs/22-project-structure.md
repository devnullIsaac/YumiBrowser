# Project Structure

```
src/
├── main.c                  # Entry point, XDG resolution, SDL/GPU init
├── runtime.c               # Base WASM runtime (Wasmer host bindings)
├── dashboard_runtime.c     # Dashboard supervisor (tiling, groups, IPC)
├── webapp_runtime.c        # Sandboxed per-webapp WASM runtime
├── gpu.c                   # WebGPU context (Dawn)
├── wgpu_bindings.c         # WebGPU host bindings for WASM
├── wgpu_ffmpeg.c           # FFmpeg → WebGPU texture upload
├── duckdb_bindings.c       # DuckDB host bindings for WASM
├── font_bindings.c         # FreeType/HarfBuzz host bindings
├── text_bindings.c         # Text shaping host bindings
├── video_bindings.c        # Video decode host bindings
├── image_bindings.c        # Image loading host bindings
├── input_bindings.c        # Input event host bindings
├── clipboard_bindings.c    # Clipboard mediation
├── sdl_bindings.c          # SDL host bindings (dashboard only)
├── log_bindings.c          # Logging host bindings
├── yumi_client.c           # Browser-facing P2P client (delta sync, registrar stream, mesh routing)
├── crypto/                 # Post-quantum + Threefish/Skein AEAD
├── group_registrar/        # Group membership ledger (DuckDB-backed)
└── network/                # Custom UDP stack + congestion control
    ├── bbr.c               # BBR-derived congestion control
    ├── yumi_udp.c          # UDP helper/utility functions
    ├── yumi_udp_client.c   # Channelized UDP (MPSC + epoll, ICE/STUN/TURN)
    └── yumi_sudp_client.c  # Secure UDP (triple-hybrid handshake, AEAD)
```

SDK headers, tests, contrib tools, and vendored dependencies are in their respective directories.
