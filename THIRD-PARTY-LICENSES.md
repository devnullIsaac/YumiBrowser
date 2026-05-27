# Third-Party Licenses

Yumi Browser bundles a number of third-party dependencies under `deps/`. Each is distributed under its own license, listed below. The authoritative license text for every dependency is the file shipped inside that dependency's directory in this source tree; the entries below are pointers to those files, plus a short identifier of the license.

Yumi Browser's own source code (everything outside `deps/` and `sdk/shaders/lygia/`) is licensed under AGPL-3.0; see [LICENSE](LICENSE) and [LEGAL.md](LEGAL.md).

| Dependency | License | Upstream license file |
|---|---|---|
| bzip2 | BSD-style (bzip2 license) | [deps/bzip2/COPYING](deps/bzip2/COPYING) |
| Clay | zlib/libpng | [deps/clay/LICENSE.md](deps/clay/LICENSE.md) |
| Dawn (and Tint) | BSD-3-Clause | [deps/dawn/LICENSE](deps/dawn/LICENSE) |
| doxygen-awesome-css | MIT | [deps/doxygen-awesome-css/LICENSE](deps/doxygen-awesome-css/LICENSE) |
| DuckDB | MIT | [deps/duckdb/LICENSE](deps/duckdb/LICENSE) |
| FFmpeg | LGPL-2.1-or-later (default build); some optional components are GPL-2.0-or-later / GPL-3.0-or-later | [deps/ffmpeg/LICENSE.md](deps/ffmpeg/LICENSE.md), [deps/ffmpeg/COPYING.LGPLv2.1](deps/ffmpeg/COPYING.LGPLv2.1), [deps/ffmpeg/COPYING.LGPLv3](deps/ffmpeg/COPYING.LGPLv3), [deps/ffmpeg/COPYING.GPLv2](deps/ffmpeg/COPYING.GPLv2), [deps/ffmpeg/COPYING.GPLv3](deps/ffmpeg/COPYING.GPLv3) |
| FreeType | Dual-licensed: FreeType License (FTL, BSD-style with credit clause) **or** GPL-2.0 — recipient may choose | [deps/freetype/LICENSE.TXT](deps/freetype/LICENSE.TXT) |
| HarfBuzz | "Old MIT" (MIT-style, see file for sub-component notices) | [deps/harfbuzz/COPYING](deps/harfbuzz/COPYING) |
| ICU | Unicode License v3 | [deps/icu/LICENSE](deps/icu/LICENSE) |
| LibRaw | Dual-licensed: LGPL-2.1 **or** CDDL-1.0 — recipient may choose | [deps/libRaw/COPYRIGHT](deps/libRaw/COPYRIGHT), [deps/libRaw/LICENSE.LGPL](deps/libRaw/LICENSE.LGPL), [deps/libRaw/LICENSE.CDDL](deps/libRaw/LICENSE.CDDL) |
| libjpeg-turbo | IJG License + modified BSD (zlib-style) — both apply, see file | [deps/libjpeg-turbo/LICENSE.md](deps/libjpeg-turbo/LICENSE.md) |
| libjuice | MPL-2.0 | [deps/libjuice/LICENSE](deps/libjuice/LICENSE) |
| OpenSSL | Apache-2.0 | [deps/openssl/LICENSE.txt](deps/openssl/LICENSE.txt) |
| oqs-provider | MIT | [deps/oqs-provider/LICENSE.txt](deps/oqs-provider/LICENSE.txt) |
| SDL (SDL3) | zlib | [deps/sdl/LICENSE.txt](deps/sdl/LICENSE.txt) |
| Slang | Apache-2.0 WITH LLVM-exception (additional component notices in `LICENSES/`) | [deps/slang/LICENSE](deps/slang/LICENSE), [deps/slang/LICENSES](deps/slang/LICENSES) |
| Wasmer | MIT | [deps/wasmer/LICENSE](deps/wasmer/LICENSE) |

## Other Bundled Material

- **lygia shader library** (`sdk/shaders/lygia/`) — see [sdk/shaders/lygia/LICENSE.md](sdk/shaders/lygia/LICENSE.md).

## Notes

- **FFmpeg build configuration.** Yumi Browser configures FFmpeg for an LGPL-compatible build. Enabling GPL-only components at build time would shift the effective license of the FFmpeg build to GPL; see [docs/19-ffmpeg.md](docs/19-ffmpeg.md) for the project's FFmpeg policy.
- **Dual-licensed components.** Where a dependency offers a choice of licenses (FreeType, LibRaw), Yumi Browser does not pre-select on the recipient's behalf; both license texts are preserved in the source tree and the recipient may rely on whichever choice they prefer.
- **Sub-component notices.** Some dependencies (HarfBuzz, oqs-provider, Slang, libjpeg-turbo, FFmpeg) include code under additional licenses for sub-components. Those sub-license notices live inside the dependency's own tree and are not duplicated here; the linked top-level license file in each row is the entry point.
- **Vendoring posture.** Dependencies are vendored as upstream source trees; the project does not modify upstream license files. If an upstream license changes between releases, the version shipped in this source tree is the one that applies to that release.

If a dependency listed above appears mis-categorized, please open an issue or send a correction. The license file shipped in the dependency's own directory is always controlling over the summary in this table.
