# Building

## Prerequisites

- Linux (x86_64)
- GCC or Clang (C and C++)
- CMake, Meson, Ninja, pkg-config
- Git
- autoconf, automake, libtool (for LibRaw)
- Rust/Cargo (if building Wasmer from source)
- fontconfig, liblzma (system packages)

## Build from Source

> **Note:** Building from source compiles all dependencies (Dawn, FFmpeg, OpenSSL, DuckDB, ICU, Slang, etc.) from scratch. This takes a long time and significant disk space. For most users, **binary distribution via Flatpak is strongly recommended** over building from source.

```bash
git clone --recursive <repo-url>
cd YumiBrowser

# Debug build (builds all dependencies from source, then the project)
./build.sh

# Release build (-O3)
./build.sh --release
```

The build script handles all dependency compilation in the correct order and assembles a self-contained `release/` directory with the binary, shared libraries, dashboard WASM, and webapps.

Yumi does not use `-march=native`. This is a deliberate portability choice: binaries are expected to run on the machine they were built on *and* on other machines with the same ISA baseline, without requiring per-host recompilation. The only measurable difference `-march=native` produces is slightly higher throughput for low-threshold packet transmissions involving small data. Bulk cryptographic operations run on the GPU path, and WASM execution is bounded by the Wasmer and WebGPU layers regardless of host compiler flags, so the portability win is essentially free.

## Dependencies

All dependencies are built from source via `build.sh`:

| Dependency | Purpose |
|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | Windowing, input, audio |
| [Dawn](https://dawn.googlesource.com/dawn) | WebGPU implementation (Vulkan, Metal, D3D12) |
| [Wasmer](https://wasmer.io/) | WebAssembly runtime |
| [DuckDB](https://duckdb.org/) | Embedded database (Group Registrar storage, webapp data layer) |
| [FreeType](https://freetype.org/) | Font rendering |
| [HarfBuzz](https://harfbuzz.github.io/) | Complex text shaping |
| [ICU4C](https://icu.unicode.org/) | Unicode services (bidi, line/grapheme breaking) |
| [FFmpeg](https://ffmpeg.org/) | Multimedia decoding (see [FFmpeg Policy](19-ffmpeg.md)) |
| [OpenSSL 3.x](https://www.openssl.org/) | Cryptographic foundation (BrainPool ECDH, PQ via oqs-provider) |
| [oqs-provider](https://github.com/open-quantum-safe/oqs-provider) + [liboqs](https://github.com/open-quantum-safe/liboqs) | Post-quantum crypto (ML-DSA-87, ML-KEM-1024, FrodoKEM-1344-SHAKE) |
| [Slang](https://github.com/shader-slang/slang) | Shader compiler |
| [libjuice](https://github.com/paullouisageneau/libjuice) | ICE/STUN/TURN for NAT traversal |
| [Clay](https://github.com/nicbarker/clay) | UI layout engine |
| [LibRaw](https://www.libraw.org/) | Camera RAW decoding (optional) |
| [bzip2](https://sourceware.org/bzip2/) | Compression |
| [libjpeg-turbo](https://libjpeg-turbo.org/) | JPEG decoding |

## Flatpak

```bash
flatpak-builder flatpak-build com.yumi.browser.yml --force-clean
```
