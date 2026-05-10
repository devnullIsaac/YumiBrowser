#!/usr/bin/env bash
# Shared configuration + helper functions for YumiBrowser build scripts.
# All scripts under scripts/ source this file. Idempotent.

# Guard against double-sourcing
if [ -n "${YUMI_COMMON_SOURCED:-}" ]; then
    return 0
fi
YUMI_COMMON_SOURCED=1

# ---------- Project layout ----------
# scripts/common.sh -> scripts/.. -> project root
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPTS_DIR="${PROJECT_ROOT}/scripts"
BUILD_DIR="${PROJECT_ROOT}/build"
DEPS_DIR="${PROJECT_ROOT}/deps"
RELEASE_DIR="${PROJECT_ROOT}/release"
ICON_DIR="${PROJECT_ROOT}/icon"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# ---------- Per-dependency paths ----------
SDL_DIR="${DEPS_DIR}/sdl"
SDL_BUILD="${SDL_DIR}/build"
SDL_INSTALL="${SDL_DIR}/install"
SDL_LIB="${SDL_INSTALL}/lib"

WASMER_DIR="${DEPS_DIR}/wasmer"
WASMER_BUILD="${WASMER_DIR}/build"
WASMER_INSTALL="${WASMER_DIR}/package"

DAWN_DIR="${DEPS_DIR}/dawn"
DAWN_BUILD="${DAWN_DIR}/build"
DAWN_INSTALL="${DAWN_DIR}/install"

DUCKDB_DIR="${DEPS_DIR}/duckdb"

FREETYPE_DIR="${DEPS_DIR}/freetype"
FREETYPE_BUILD="${FREETYPE_DIR}/build"
FREETYPE_INSTALL="${FREETYPE_DIR}/install"

OPENSSL_DIR="${DEPS_DIR}/openssl"
OPENSSL_INSTALL="${OPENSSL_DIR}/install"

LIBOQS_SRC="${DEPS_DIR}/oqs-provider/liboqs"
LIBOQS_BUILD="${LIBOQS_SRC}/_build"
LIBOQS_INSTALL="${OPENSSL_INSTALL}"

OQS_DIR="${DEPS_DIR}/oqs-provider"
OQS_BUILD="${OQS_DIR}/build"

ICU_DIR="${DEPS_DIR}/icu"
ICU_SOURCE="${ICU_DIR}/icu4c/source"
ICU_BUILD="${ICU_DIR}/build"
ICU_INSTALL="${ICU_DIR}/install"

HARFBUZZ_DIR="${DEPS_DIR}/harfbuzz"
HARFBUZZ_BUILD="${HARFBUZZ_DIR}/build"
HARFBUZZ_INSTALL="${HARFBUZZ_DIR}/install"

FFMPEG_DIR="${DEPS_DIR}/ffmpeg"
FFMPEG_BUILD="${FFMPEG_DIR}/build"
FFMPEG_INSTALL="${FFMPEG_DIR}/install"

SLANG_DIR="${DEPS_DIR}/slang"
SLANG_BUILD="${SLANG_DIR}/build"
SLANG_INSTALL="${SLANG_DIR}/install"

LIBRAW_DIR="${DEPS_DIR}/libRaw"
LIBRAW_BUILD="${LIBRAW_DIR}/build"
LIBRAW_INSTALL="${LIBRAW_DIR}/install"

BZIP2_DIR="${DEPS_DIR}/bzip2"
BZIP2_BUILD="${BZIP2_DIR}/build"
BZIP2_INSTALL="${BZIP2_DIR}/install"

LIBJPEG_DIR="${DEPS_DIR}/libjpeg-turbo"
LIBJPEG_BUILD="${LIBJPEG_DIR}/build"
LIBJPEG_INSTALL="${LIBJPEG_DIR}/install"

# WASI/WASIX toolchain
WASI_DIR="${DEPS_DIR}/wasi"

# ---------- Build outputs ----------
BINARY_NAME="yumibrowser"
APP_ID="com.yumi.browser"
CONTRIB_FONT_PAINT="font-paint-cli"
CONTRIB_SHADER_BIND="shader-bind"
CONTRIB_SHADER_PREVIEW="shader-preview"
CONTRIB_SVG_TO_SLANG="svg-to-slang"

# ---------- Submodule lists ----------
SUBMODULE_DIRS=(
    "deps/clay"
    "deps/duckdb"
    "deps/ffmpeg"
    "deps/freetype"
    "deps/harfbuzz"
    "deps/icu"
    "deps/libRaw"
    "deps/openssl"
    "deps/oqs-provider"
    "deps/sdl"
    "deps/wasmer"
)
DAWN_SUBMODULE="deps/dawn"
SLANG_SUBMODULE="deps/slang"

# ---------- Pinned dependency tags ----------
declare -A DEP_TAGS=(
    ["deps/sdl"]="release-3.4.8"
    ["deps/wasmer"]="v7.1.0"
    ["deps/duckdb"]="v1.5.2"
    ["deps/clay"]="v0.14"
    ["deps/icu"]="release-78.3"
    ["deps/freetype"]="VER-2-14-3"
    ["deps/harfbuzz"]="14.2.0"
    ["deps/openssl"]="openssl-3.5.6"
    ["deps/oqs-provider"]="0.11.0"
    ["deps/libjuice"]="v1.7.1"
    ["deps/ffmpeg"]="n8.1.1"
    ["deps/libRaw"]="0.22.1"
    ["deps/bzip2"]="bzip2-1.0.8"
    ["deps/libjpeg-turbo"]="3.1.4"
    ["deps/doxygen-awesome-css"]="v2.4.2"
    # Dawn uses chromium/NNNN release branches, not semver tags.
    ["deps/dawn"]="chromium/7834"
    ["deps/slang"]="v2026.8.1"
)

# Map logical dep name (matches scripts/deps/<name>.sh) to its
# deps/<subdir> directory name. Used by wipe_dep + --rebuild.
declare -A DEP_DIR_NAMES=(
    [sdl]="sdl"
    [bzip2]="bzip2"
    [libjpeg_turbo]="libjpeg-turbo"
    [freetype]="freetype"
    [openssl]="openssl"
    [icu]="icu"
    [harfbuzz]="harfbuzz"
    [ffmpeg]="ffmpeg"
    [libraw]="libRaw"
    [wasmer]="wasmer"
    [slang]="slang"
    [dawn]="dawn"
    [duckdb]="duckdb"
)

# wipe_dep <logical-name>: remove build/install/_build artefacts so the
# next run of scripts/deps/<name>.sh rebuilds from source. Special-cases:
#   - openssl also wipes oqs-provider + nested liboqs/_build
#   - duckdb has no separate build/install (built in tree)
wipe_dep() {
    local name="$1"
    local subdir="${DEP_DIR_NAMES[$name]:-}"
    if [ -z "$subdir" ]; then
        warn "wipe_dep: unknown dep '$name'"
        return 1
    fi
    local root="${DEPS_DIR}/${subdir}"
    local sub
    for sub in build install _build package; do
        [ -d "${root}/${sub}" ] && rm -rf "${root}/${sub}"
    done
    case "$name" in
        openssl)
            rm -rf "${DEPS_DIR}/oqs-provider/build"
            rm -rf "${DEPS_DIR}/oqs-provider/liboqs/_build"
            ;;
        duckdb)
            # duckdb builds into its own src tree; clean its build dir if any
            rm -rf "${DUCKDB_DIR}/build"
            ;;
    esac
    ok "Wiped ${subdir}"
}

LIBOQS_TAG="0.15.0"

# WASIX sysroot
WASIX_TAG="v2026-03-02.1"
WASIX_ASSET="sysroot.tar.gz"
WASIX_URL="https://github.com/wasix-org/wasix-libc/releases/download/${WASIX_TAG}/${WASIX_ASSET}"

# wasi-sdk
WASI_SDK_TAG="wasi-sdk-33"
WASI_SDK_VERSION="33.0"

# ---------- Build mode (driven by env from top-level build.sh) ----------
RELEASE_MODE="${YUMI_RELEASE_MODE:-false}"
FLATPAK_MODE="${YUMI_FLATPAK_MODE:-false}"

if [ "$RELEASE_MODE" = "true" ]; then
    MESON_BUILDTYPE="release"
    MESON_RELEASE_OPT="-Drelease_build=true"
else
    MESON_BUILDTYPE="debug"
    MESON_RELEASE_OPT="-Drelease_build=false"
fi
CMAKE_BUILD_TYPE="Release"  # deps are always Release

# ---------- Colors + log helpers ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

step()  { echo -e "\n${CYAN}▸ $*${NC}"; }
ok()    { echo -e "  ${GREEN}✓  $*${NC}"; }
warn()  { echo -e "  ${YELLOW}⚠  $*${NC}"; }
fail()  { echo -e "\n  ${RED}✗  $*${NC}"; exit 1; }

check_tool() {
    if ! command -v "$1" &>/dev/null; then
        fail "Required tool '$1' not found. Please install it."
    fi
}

# ---------- Freedesktop icon installer ----------
# install_icons <share_dir>  (e.g. ~/.local/share)
install_icons() {
    local share_dir="$1"
    local icon_theme_dir="${share_dir}/icons/hicolor"
    local sizes=(512 256 128 64 32)
    local installed_any=false

    step "Installing freedesktop icons to ${icon_theme_dir}"

    for size in "${sizes[@]}"; do
        local src="${ICON_DIR}/icon${size}.png"
        if [ -f "$src" ]; then
            local dest_dir="${icon_theme_dir}/${size}x${size}/apps"
            mkdir -p "$dest_dir"
            cp "$src" "${dest_dir}/yumibrowser.png"
            ok "Installed ${size}x${size} icon"
            installed_any=true
        else
            warn "icon${size}.png not found in ${ICON_DIR} — skipping"
        fi
    done

    if $installed_any; then
        if command -v gtk-update-icon-cache &>/dev/null; then
            gtk-update-icon-cache -f -t "${icon_theme_dir}" 2>/dev/null || true
            ok "Icon cache updated"
        elif command -v update-icon-caches &>/dev/null; then
            update-icon-caches "${icon_theme_dir}" 2>/dev/null || true
            ok "Icon cache updated"
        else
            warn "No icon cache tool found — icons may not appear until next login"
        fi
    else
        warn "No icon files found in ${ICON_DIR}"
    fi
}

# ---------- Shared library collector ----------
# collect_so <src_dir> [<glob>]
# Copies real shared library files (resolving symlinks) into RELEASE_DIR
# and re-creates the soname symlink chain.
collect_so() {
    local src_dir="$1"
    local pattern="${2:-*.so*}"
    for f in "${src_dir}"/${pattern}; do
        [ -f "$f" ] || continue
        local real
        real="$(readlink -f "$f")"
        local base
        base="$(basename "$f")"
        local real_base
        real_base="$(basename "$real")"

        if [ ! -f "${RELEASE_DIR}/${real_base}" ]; then
            cp "$real" "${RELEASE_DIR}/${real_base}"
        fi
        if [ "$base" != "$real_base" ] && [ ! -e "${RELEASE_DIR}/${base}" ]; then
            ln -sf "$real_base" "${RELEASE_DIR}/${base}"
        fi
    done
}
