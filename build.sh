#!/usr/bin/env bash
set -euo pipefail

# ---------- Configuration ----------
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
DEPS_DIR="${PROJECT_ROOT}/deps"
RELEASE_DIR="${PROJECT_ROOT}/release"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

#/////////////////////////////////////////////////////
# This project is always intended to be built with Clang/LLVM.
# Any other compiler is unsupported.
#/////////////////////////////////////////////////////
export CC=clang
export CXX=clang++

# Install prefixes per dependency
SDL_DIR="${DEPS_DIR}/sdl"
SDL_BUILD="${SDL_DIR}/build"
SDL_INSTALL="${SDL_DIR}/install"

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

# NEW — LibRaw camera RAW decoding
LIBRAW_DIR="${DEPS_DIR}/libRaw"
LIBRAW_BUILD="${LIBRAW_DIR}/build"
LIBRAW_INSTALL="${LIBRAW_DIR}/install"

BINARY_NAME="yumibrowser"
CONTRIB_FONT_PAINT="font-paint-cli"
CONTRIB_SHADER_BIND="shader-bind"
CONTRIB_SHADER_PREVIEW="shader-preview"
CONTRIB_SVG_TO_SLANG="svg-to-slang"

# Icon source directory
ICON_DIR="${PROJECT_ROOT}/icon"

# XDG app ID — matches APP_ID in dashboard_runtime.c
APP_ID="com.yumi.browser"

# Parse --release and --flatpak flags
RELEASE_MODE=false
FLATPAK_MODE=false
for arg in "$@"; do
    case "$arg" in
        --release) RELEASE_MODE=true ;;
        --flatpak) FLATPAK_MODE=true ;;
    esac
done

if $RELEASE_MODE; then
    MESON_BUILDTYPE="release"
    MESON_RELEASE_OPT="-Drelease_build=true"
    CMAKE_BUILD_TYPE="Release"
else
    MESON_BUILDTYPE="debug"
    MESON_RELEASE_OPT="-Drelease_build=false"
    CMAKE_BUILD_TYPE="Release"  # deps are always Release
fi

# Regular submodules — safe to recurse
SUBMODULE_DIRS=(
    "deps/bzip2"
    "deps/clay"
    "deps/duckdb"
    "deps/ffmpeg"
    "deps/freetype"
    "deps/harfbuzz"
    "deps/icu"
    "deps/libjpeg-turbo"
    "deps/libRaw"
    "deps/openssl"
    "deps/oqs-provider"
    "deps/sdl"
    "deps/wasmer"
    "deps/libjuice"
    "deps/doxygen-awesome-css"
)

# Dawn is handled separately — DO NOT recurse its submodules.
DAWN_SUBMODULE="deps/dawn"

# Slang is handled separately — must be built with cmake presets.
SLANG_SUBMODULE="deps/slang"

# ---------- Colors ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

step()  { echo -e "\n${CYAN}▸ $*${NC}"; }
ok()    { echo -e "  ${GREEN}✓  $*${NC}"; }
warn()  { echo -e "  ${YELLOW}⚠  $*${NC}"; }
fail()  { echo -e "\n  ${RED}✗  $*${NC}"; exit 1; }

# In flatpak mode, wipe all prebuilt dep artifacts so everything builds
# fresh inside the SDK container (matching the runtime's glibc, etc.)
if $FLATPAK_MODE; then
    step "Flatpak mode: clearing prebuilt dependency artifacts"
    for d in bzip2 libjpeg-turbo sdl freetype openssl oqs-provider icu \
             harfbuzz ffmpeg libRaw wasmer slang dawn duckdb; do
        for sub in build install _build; do
            if [ -d "${DEPS_DIR}/${d}/${sub}" ]; then
                rm -rf "${DEPS_DIR}/${d}/${sub}"
            fi
        done
    done
    # liboqs is nested inside oqs-provider
    rm -rf "${DEPS_DIR}/oqs-provider/liboqs/_build"
    # Also clear the meson build dirs (main + webapps)
    rm -rf "${BUILD_DIR}"
    rm -rf "${PROJECT_ROOT}/webapps/_build"
    ok "Cleared all prebuilt artifacts"
fi

# ---------- Freedesktop icon installer ----------
# Installs icon PNGs from icon/ into the hicolor icon theme.
# Usage: install_icons <prefix>
#   e.g. install_icons "${HOME}/.local/share"
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

# ---------- Initialise submodules ----------
step "Initialising submodules"

if [ -d "${PROJECT_ROOT}/.git" ]; then
    for dir in "${SUBMODULE_DIRS[@]}"; do
        full_path="${PROJECT_ROOT}/${dir}"
        if [ ! -d "$full_path" ] || [ -z "$(ls -A "$full_path" 2>/dev/null)" ]; then
            step "Cloning submodule: ${dir}"
            git -C "${PROJECT_ROOT}" submodule update --init --depth 1 -- "$dir"
        fi
    done

    # Dawn — init only, never recurse
    if [ ! -d "${PROJECT_ROOT}/${DAWN_SUBMODULE}" ] || \
       [ -z "$(ls -A "${PROJECT_ROOT}/${DAWN_SUBMODULE}" 2>/dev/null)" ]; then
        step "Cloning Dawn submodule (no recurse)"
        git -C "${PROJECT_ROOT}" submodule update --init --depth 1 -- "${DAWN_SUBMODULE}"
    fi

    # Wasmer needs its nested lib/napi submodule for the CLI to build
    if [ -d "${WASMER_DIR}/.git" ] || [ -f "${WASMER_DIR}/.git" ]; then
        if [ ! -f "${WASMER_DIR}/lib/napi/Cargo.toml" ]; then
            step "Initialising nested wasmer submodules (lib/napi)"
            git -C "${WASMER_DIR}" submodule update --init --depth 1 -- lib/napi
        fi
    fi

    # Slang — init only, never recurse
    if [ ! -d "${PROJECT_ROOT}/${SLANG_SUBMODULE}" ] || \
       [ -z "$(ls -A "${PROJECT_ROOT}/${SLANG_SUBMODULE}" 2>/dev/null)" ]; then
        step "Cloning Slang submodule (no recurse)"
        git -C "${PROJECT_ROOT}" submodule update --init --depth 1 -- "${SLANG_SUBMODULE}"
    fi

    # Slang requires a subset of its external submodules to configure even
    # when examples/tests/gfx/rhi are disabled.
    if [ -d "${SLANG_DIR}/.git" ] || [ -f "${SLANG_DIR}/.git" ]; then
        for sub in external/unordered_dense external/miniz external/lz4 \
                   external/cmark external/vulkan external/spirv-headers \
                   external/spirv-tools external/glslang external/lua; do
            if [ ! -f "${SLANG_DIR}/${sub}/CMakeLists.txt" ] && \
               [ ! -d "${SLANG_DIR}/${sub}/build" ] && \
               [ -z "$(ls -A "${SLANG_DIR}/${sub}" 2>/dev/null)" ]; then
                step "Initialising nested slang submodule: ${sub}"
                git -C "${SLANG_DIR}" submodule update --init --depth 1 -- "${sub}"
            fi
        done
    fi
else
    warn "Not a git repo — skipping submodule init"
fi

# ---------- Verify submodule dirs ----------
step "Verifying submodule directories"

all_ok=true
for dir in "${SUBMODULE_DIRS[@]}" "${DAWN_SUBMODULE}" "${SLANG_SUBMODULE}"; do
    full_path="${PROJECT_ROOT}/${dir}"
    if [ ! -d "$full_path" ]; then
        echo -e "  ${RED}✗  ${dir} — not found${NC}"
        all_ok=false
    elif [ -z "$(ls -A "$full_path" 2>/dev/null)" ]; then
        echo -e "  ${RED}✗  ${dir} — directory empty${NC}"
        all_ok=false
    else
        echo -e "  ${GREEN}✓  ${dir}${NC}"
    fi
done

if ! $all_ok; then
    echo ""
    fail "One or more submodules failed to clone. Check your .gitmodules and network connection."
fi

ok "All submodule directories present and non-empty"

# ---------- Build tool checks ----------
check_tool() {
    if ! command -v "$1" &>/dev/null; then
        fail "Required tool '$1' not found. Please install it."
    fi
}

step "Checking build tools"
check_tool cmake
check_tool meson
check_tool ninja
check_tool pkg-config
ok "All build tools found"

if $RELEASE_MODE; then
    step "Build mode: RELEASE (-O3)"
else
    step "Build mode: DEBUG (use --release for optimized build)"
fi

# ======================================================================
#  0. Host-glibc compatibility sweep
# ======================================================================
#
# When build artifacts persist across environments (e.g. a Flatpak SDK
# upgrade or container migration) the previously-built shared libraries
# in deps/*/install can require a newer glibc than the current host
# provides — producing link errors like:
#
#   /usr/.../ld.bfd: deps/sdl/build/libSDL3.so: undefined reference to
#       `log10f@GLIBC_2.43'
#
# This sweep walks every build-managed dependency and removes the
# stale install + build dirs whenever any of their .so files reference
# a glibc symbol newer than what the host actually provides.  On the
# next steps of build.sh those deps are then rebuilt from source.
# ======================================================================

# Detect host glibc version once (e.g. "2.42").
# We avoid `head -1` in front of `grep` to dodge SIGPIPE-vs-pipefail
# interaction (head closes the pipe early, grep gets SIGPIPE, script aborts).
HOST_GLIBC_VER=$(ldd --version 2>/dev/null \
    | awk 'NR==1 { for (i=1; i<=NF; i++) if ($i ~ /^[0-9]+\.[0-9]+$/) { print $i; exit } }' \
    || true)

# Returns 0 if `lib` requires only GLIBC versions <= the host's;
# 1 if it requires a newer GLIBC.
glibc_compatible() {
    local lib="$1"
    [ ! -f "$lib" ] && return 0
    command -v objdump >/dev/null 2>&1 || return 0
    [ -z "${HOST_GLIBC_VER}" ] && return 0
    # `sort -V` correctly orders GLIBC_2.43 > GLIBC_2.42 > GLIBC_2.6.
    # If the lib's highest GLIBC requirement sorts above the host version,
    # the lib is stale.
    local required_max
    required_max=$(objdump -T "$lib" 2>/dev/null \
        | grep -oE 'GLIBC_[0-9]+\.[0-9]+' \
        | sort -uV \
        | tail -1)
    [ -z "$required_max" ] && return 0
    local highest
    highest=$(printf 'GLIBC_%s\n%s\n' "$HOST_GLIBC_VER" "$required_max" | sort -V | tail -1)
    [ "$highest" = "GLIBC_${HOST_GLIBC_VER}" ] && return 0
    return 1
}

# Track whether any dep was nuked (used to invalidate meson cache)
GLIBC_SWEEP_NUKED_ANY=false

# Nuke a dependency's install + build dirs if ANY of its .so files
# requires a newer glibc than the host provides.
#
# Args: <label> <space-separated dirs to scan> <space-separated dirs to nuke>
sweep_glibc_stale() {
    local label="$1"
    local scan_dirs="$2"
    local nuke_dirs="$3"
    local stale=0
    local d f
    for d in $scan_dirs; do
        [ -d "$d" ] || continue
        # Recursive scan for any .so* under the dir
        while IFS= read -r f; do
            [ -f "$f" ] || continue
            if ! glibc_compatible "$f"; then
                warn "  ${f#${PROJECT_ROOT}/} → requires newer glibc than host"
                stale=1
            fi
        done < <(find "$d" -maxdepth 4 \
                    \( -name '*.so' -o -name '*.so.*' \) -type f 2>/dev/null)
    done
    if [ $stale -eq 1 ]; then
        warn "Removing stale ${label} (host-glibc mismatch — will rebuild)"
        for d in $nuke_dirs; do
            [ -e "$d" ] && rm -rf "$d"
        done
        GLIBC_SWEEP_NUKED_ANY=true
    fi
}

step "Checking dependency glibc compatibility (host glibc: ${HOST_GLIBC_VER:-unknown})"

if [ -n "${HOST_GLIBC_VER}" ] && command -v objdump >/dev/null 2>&1; then
    # SDL3
    sweep_glibc_stale "SDL3" \
        "${SDL_INSTALL} ${SDL_BUILD}" \
        "${SDL_INSTALL} ${SDL_BUILD}"

    # FFmpeg
    sweep_glibc_stale "FFmpeg" \
        "${FFMPEG_INSTALL} ${FFMPEG_BUILD}" \
        "${FFMPEG_INSTALL} ${FFMPEG_BUILD}"

    # LibRaw
    sweep_glibc_stale "LibRaw" \
        "${LIBRAW_INSTALL} ${LIBRAW_BUILD}" \
        "${LIBRAW_INSTALL} ${LIBRAW_BUILD}"

    # DuckDB — build dir holds the .so directly (no install step)
    sweep_glibc_stale "DuckDB" \
        "${DEPS_DIR}/duckdb/build" \
        "${DEPS_DIR}/duckdb/build"

    # FreeType
    sweep_glibc_stale "FreeType" \
        "${FREETYPE_INSTALL} ${FREETYPE_BUILD}" \
        "${FREETYPE_INSTALL} ${FREETYPE_BUILD}"

    # HarfBuzz
    sweep_glibc_stale "HarfBuzz" \
        "${HARFBUZZ_INSTALL} ${HARFBUZZ_BUILD}" \
        "${HARFBUZZ_INSTALL} ${HARFBUZZ_BUILD}"

    # OpenSSL
    sweep_glibc_stale "OpenSSL" \
        "${OPENSSL_INSTALL} ${DEPS_DIR}/openssl/build" \
        "${OPENSSL_INSTALL} ${DEPS_DIR}/openssl/build"

    # ICU
    sweep_glibc_stale "ICU" \
        "${ICU_INSTALL} ${ICU_BUILD}" \
        "${ICU_INSTALL} ${ICU_BUILD}"

    # Slang
    sweep_glibc_stale "Slang" \
        "${SLANG_INSTALL} ${SLANG_BUILD}" \
        "${SLANG_INSTALL} ${SLANG_BUILD}"

    # libjpeg-turbo
    sweep_glibc_stale "libjpeg-turbo" \
        "${DEPS_DIR}/libjpeg-turbo/install ${DEPS_DIR}/libjpeg-turbo/build" \
        "${DEPS_DIR}/libjpeg-turbo/install ${DEPS_DIR}/libjpeg-turbo/build"

    # bzip2
    sweep_glibc_stale "bzip2" \
        "${DEPS_DIR}/bzip2/install ${DEPS_DIR}/bzip2/build" \
        "${DEPS_DIR}/bzip2/install ${DEPS_DIR}/bzip2/build"

    # Dawn (WebGPU) — build dir contains the .so
    sweep_glibc_stale "Dawn" \
        "${DAWN_INSTALL} ${DAWN_BUILD}" \
        "${DAWN_INSTALL} ${DAWN_BUILD}"

    # Wasmer (prebuilt package — only check, don't nuke; warn if stale)
    if [ -d "${WASMER_INSTALL}" ]; then
        for f in "${WASMER_INSTALL}"/lib/lib*.so*; do
            [ -f "$f" ] || continue
            if ! glibc_compatible "$f"; then
                warn "Wasmer (${f##*/}) requires newer glibc — re-download package"
                warn "  delete ${WASMER_INSTALL} and re-extract the official tarball"
            fi
        done
    fi
else
    warn "Skipping glibc compatibility sweep (objdump or glibc version not detected)"
fi
ok "Glibc compatibility sweep complete"

# Invalidate meson cache ONLY if a dep was actually nuked above
if $GLIBC_SWEEP_NUKED_ANY && [ -d "${BUILD_DIR}" ]; then
    warn "Invalidating meson build cache (deps were rebuilt due to glibc mismatch)"
    rm -rf "${BUILD_DIR}"
fi

# ======================================================================
#  1. Build SDL3
# ======================================================================
step "Building SDL3"

if [ -f "${SDL_INSTALL}/lib/libSDL3.so" ] || \
   [ -f "${SDL_INSTALL}/lib64/libSDL3.so" ] || \
   [ -f "${SDL_INSTALL}/lib/libSDL3.dylib" ] || \
   [ -f "${SDL_INSTALL}/lib/libSDL3.a" ]; then
    warn "SDL3 already built — skipping (delete ${SDL_INSTALL} to rebuild)"
else
    if [ -f "${SDL_BUILD}/CMakeCache.txt" ]; then
        warn "Removing stale SDL CMake cache"
        rm -rf "${SDL_BUILD}"
    fi

    cmake -S "${SDL_DIR}" -B "${SDL_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${SDL_INSTALL}" \
        -DSDL_SHARED=ON \
        -DSDL_STATIC=OFF \
        -DSDL_TEST=OFF \
        -G Ninja

    cmake --build "${SDL_BUILD}" -j "${JOBS}"
    cmake --install "${SDL_BUILD}"
    ok "SDL3 installed to ${SDL_INSTALL}"
fi

# ======================================================================
#  1b. Build bzip2 (needed by FreeType)
# ======================================================================
step "Building bzip2"

BZIP2_DIR="${DEPS_DIR}/bzip2"
BZIP2_BUILD="${BZIP2_DIR}/build"
BZIP2_INSTALL="${BZIP2_DIR}/install"

if [ -f "${BZIP2_INSTALL}/lib/libbz2.so" ] || \
   [ -f "${BZIP2_INSTALL}/lib64/libbz2.so" ]; then
    warn "bzip2 already built — skipping (delete ${BZIP2_INSTALL} to rebuild)"
else
    cmake -S "${BZIP2_DIR}" -B "${BZIP2_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${BZIP2_INSTALL}" \
        -DENABLE_SHARED_LIB=ON \
        -DENABLE_STATIC_LIB=OFF \
        -G Ninja

    cmake --build "${BZIP2_BUILD}" -j "${JOBS}"
    cmake --install "${BZIP2_BUILD}"
    ok "bzip2 installed to ${BZIP2_INSTALL}"
fi

# ======================================================================
#  1c. Build libjpeg-turbo (needed by FFmpeg / LibRaw)
# ======================================================================
step "Building libjpeg-turbo"

LIBJPEG_DIR="${DEPS_DIR}/libjpeg-turbo"
LIBJPEG_BUILD="${LIBJPEG_DIR}/build"
LIBJPEG_INSTALL="${LIBJPEG_DIR}/install"

if [ -f "${LIBJPEG_INSTALL}/lib/libjpeg.so" ] || \
   [ -f "${LIBJPEG_INSTALL}/lib64/libjpeg.so" ]; then
    warn "libjpeg-turbo already built — skipping (delete ${LIBJPEG_INSTALL} to rebuild)"
else
    cmake -S "${LIBJPEG_DIR}" -B "${LIBJPEG_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${LIBJPEG_INSTALL}" \
        -DENABLE_SHARED=ON \
        -DENABLE_STATIC=OFF \
        -DWITH_JPEG8=ON \
        -G Ninja

    cmake --build "${LIBJPEG_BUILD}" -j "${JOBS}"
    cmake --install "${LIBJPEG_BUILD}"
    ok "libjpeg-turbo installed to ${LIBJPEG_INSTALL}"
fi

# ======================================================================
#  2. Build FreeType
# ======================================================================
step "Building FreeType"

if [ -f "${FREETYPE_INSTALL}/lib/libfreetype.so" ] || \
   [ -f "${FREETYPE_INSTALL}/lib64/libfreetype.so" ] || \
   [ -f "${FREETYPE_INSTALL}/lib/libfreetype.dylib" ] || \
   [ -f "${FREETYPE_INSTALL}/lib/libfreetype.a" ]; then
    warn "FreeType already built — skipping (delete ${FREETYPE_INSTALL} to rebuild)"
else
    if [ -f "${FREETYPE_BUILD}/CMakeCache.txt" ]; then
        warn "Removing stale FreeType CMake cache"
        rm -rf "${FREETYPE_BUILD}"
    fi

    cmake -S "${FREETYPE_DIR}" -B "${FREETYPE_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${FREETYPE_INSTALL}" \
        -DCMAKE_PREFIX_PATH="${BZIP2_INSTALL}" \
        -DBUILD_SHARED_LIBS=ON \
        -G Ninja

    cmake --build "${FREETYPE_BUILD}" -j "${JOBS}"
    cmake --install "${FREETYPE_BUILD}"
    ok "FreeType installed to ${FREETYPE_INSTALL}"
fi

# ======================================================================
#  3a. Build OpenSSL 3.x
# ======================================================================
step "Building OpenSSL"

if [ -f "${OPENSSL_INSTALL}/lib/libcrypto.so" ] || \
   [ -f "${OPENSSL_INSTALL}/lib/libcrypto.so.3" ] || \
   [ -f "${OPENSSL_INSTALL}/lib64/libcrypto.so" ] || \
   [ -f "${OPENSSL_INSTALL}/lib64/libcrypto.so.3" ] || \
   [ -f "${OPENSSL_INSTALL}/lib/libcrypto.dylib" ]; then
    warn "OpenSSL already built — skipping (delete ${OPENSSL_INSTALL} to rebuild)"
else
    cd "${OPENSSL_DIR}"

    # Configure in-source, install out-of-source to install/
    ./Configure \
        --prefix="${OPENSSL_INSTALL}" \
        --openssldir="${OPENSSL_INSTALL}/ssl" \
        --libdir=lib \
        shared \
        no-tests

    make -j "${JOBS}"
    make install_sw install_ssldirs
    ok "OpenSSL installed to ${OPENSSL_INSTALL}"

    cd "${PROJECT_ROOT}"
fi

# ======================================================================
#  3b. Build liboqs (post-quantum crypto library — depends on OpenSSL 3.x)
# ======================================================================
step "Building liboqs"

if [ -f "${LIBOQS_INSTALL}/lib/liboqs.a" ] || \
   [ -f "${LIBOQS_INSTALL}/lib/liboqs.so" ] || \
   [ -f "${LIBOQS_INSTALL}/lib64/liboqs.a" ] || \
   [ -f "${LIBOQS_INSTALL}/lib64/liboqs.so" ] || \
   [ -f "${LIBOQS_INSTALL}/lib/liboqs.dylib" ]; then
    warn "liboqs already built — skipping (delete ${LIBOQS_INSTALL}/lib/liboqs.* to rebuild)"
else
    if [ ! -d "${LIBOQS_SRC}" ]; then
        echo "Cloning liboqs 0.15.0..."
        git clone --depth 1 --branch 0.15.0 \
            https://github.com/open-quantum-safe/liboqs.git "${LIBOQS_SRC}"
    fi

    cmake -S "${LIBOQS_SRC}" -B "${LIBOQS_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${LIBOQS_INSTALL}" \
        -DOPENSSL_ROOT_DIR="${OPENSSL_INSTALL}" \
        -DOQS_ALGS_ENABLED=All \
        -DOQS_LIBJADE_BUILD=OFF \
        -G Ninja

    cmake --build "${LIBOQS_BUILD}" -j "${JOBS}"
    cmake --install "${LIBOQS_BUILD}"
    ok "liboqs installed to ${LIBOQS_INSTALL}"

    cd "${PROJECT_ROOT}"
fi

# ======================================================================
#  3c. Build oqs-provider (post-quantum — depends on OpenSSL 3.x + liboqs)
#
#  Installs oqsprovider.so into OpenSSL's ossl-modules/ directory.
# ======================================================================
step "Building oqs-provider"

if [ -f "${OPENSSL_INSTALL}/lib/ossl-modules/oqsprovider.so" ] || \
   [ -f "${OPENSSL_INSTALL}/lib64/ossl-modules/oqsprovider.so" ] || \
   [ -f "${OPENSSL_INSTALL}/lib/ossl-modules/oqsprovider.dylib" ]; then
    warn "oqs-provider already built — skipping (delete ${OPENSSL_INSTALL}/lib/ossl-modules/oqsprovider.* to rebuild)"
else
    if [ -f "${OQS_BUILD}/CMakeCache.txt" ]; then
        warn "Removing stale oqs-provider CMake cache"
        rm -rf "${OQS_BUILD}"
    fi

    cmake -S "${OQS_DIR}" -B "${OQS_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="${OPENSSL_INSTALL}" \
        -DOPENSSL_ROOT_DIR="${OPENSSL_INSTALL}" \
        -Dliboqs_DIR="${LIBOQS_INSTALL}/lib/cmake/liboqs" \
        -G Ninja

    cmake --build "${OQS_BUILD}" -j "${JOBS}"

    # Install provider module into OpenSSL's module search path
    mkdir -p "${OPENSSL_INSTALL}/lib/ossl-modules"
    OQS_SO="$(find "${OQS_BUILD}" -name 'oqsprovider.so' -o -name 'oqsprovider.dylib' | head -1)"
    if [ -n "${OQS_SO}" ] && [ -f "${OQS_SO}" ]; then
        cp "${OQS_SO}" "${OPENSSL_INSTALL}/lib/ossl-modules/"
        ok "oqs-provider installed to ${OPENSSL_INSTALL}/lib/ossl-modules/"
    else
        fail "Could not find oqsprovider.so in ${OQS_BUILD}"
    fi

    cd "${PROJECT_ROOT}"
fi

# ======================================================================
#  4. Build ICU4C
# ======================================================================
step "Building ICU4C"

if [ -f "${ICU_INSTALL}/lib/libicuuc.so" ] || \
   [ -f "${ICU_INSTALL}/lib64/libicuuc.so" ] || \
   [ -f "${ICU_INSTALL}/lib/libicuuc.dylib" ]; then
    warn "ICU already built — skipping (delete ${ICU_INSTALL} to rebuild)"
else
    if [ ! -d "${ICU_SOURCE}" ]; then
        if [ -f "${ICU_DIR}/source/configure" ]; then
            ICU_SOURCE="${ICU_DIR}/source"
        else
            fail "ICU source not found. Expected ${ICU_SOURCE} or ${ICU_DIR}/source"
        fi
    fi

    if [ ! -f "${ICU_SOURCE}/configure" ]; then
        fail "ICU configure script not found at ${ICU_SOURCE}/configure"
    fi

    mkdir -p "${ICU_BUILD}"
    cd "${ICU_BUILD}"

    "${ICU_SOURCE}/configure" \
        --prefix="${ICU_INSTALL}" \
        --enable-shared \
        --disable-static \
        --disable-samples \
        --disable-tests \
        --disable-extras \
        --with-data-packaging=library

    make -j "${JOBS}"
    make install
    ok "ICU installed to ${ICU_INSTALL}"

    cd "${PROJECT_ROOT}"
fi

# ======================================================================
#  5. Build HarfBuzz (depends on FreeType + ICU)
# ======================================================================
step "Building HarfBuzz"

if [ -f "${HARFBUZZ_INSTALL}/lib/libharfbuzz.so" ] || \
   [ -f "${HARFBUZZ_INSTALL}/lib64/libharfbuzz.so" ] || \
   [ -f "${HARFBUZZ_INSTALL}/lib/libharfbuzz.dylib" ] || \
   [ -f "${HARFBUZZ_INSTALL}/lib/libharfbuzz.a" ]; then
    warn "HarfBuzz already built — skipping (delete ${HARFBUZZ_INSTALL} to rebuild)"
else
    if [ -f "${HARFBUZZ_BUILD}/CMakeCache.txt" ]; then
        warn "Removing stale HarfBuzz CMake cache"
        rm -rf "${HARFBUZZ_BUILD}"
    fi

    export PKG_CONFIG_PATH="${ICU_INSTALL}/lib/pkgconfig:${FREETYPE_INSTALL}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

    cmake -S "${HARFBUZZ_DIR}" -B "${HARFBUZZ_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${HARFBUZZ_INSTALL}" \
        -DCMAKE_PREFIX_PATH="${ICU_INSTALL};${FREETYPE_INSTALL}" \
        -DHB_HAVE_FREETYPE=ON \
        -DHB_HAVE_ICU=ON \
        -DHB_HAVE_GLIB=OFF \
        -DHB_HAVE_GOBJECT=OFF \
        -DHB_BUILD_TESTS=OFF \
        -DHB_BUILD_SUBSET=OFF \
        -G Ninja

    cmake --build "${HARFBUZZ_BUILD}" -j "${JOBS}"
    cmake --install "${HARFBUZZ_BUILD}"
    ok "HarfBuzz installed to ${HARFBUZZ_INSTALL}"
fi

# ======================================================================
#  5a. Build FFmpeg (AGPL-3.0 — shared libraries, patent-free lite build)
# ======================================================================
step "Building FFmpeg (patent-free lite build)"

if [ -f "${FFMPEG_INSTALL}/lib/libavcodec.so" ] || \
   [ -f "${FFMPEG_INSTALL}/lib64/libavcodec.so" ] || \
   [ -f "${FFMPEG_INSTALL}/lib/libavcodec.dylib" ]; then
    warn "FFmpeg already built — skipping (delete ${FFMPEG_INSTALL} to rebuild)"
else
    cd "${FFMPEG_DIR}"

    if [ ! -f "configure" ]; then
        fail "FFmpeg configure script not found at ${FFMPEG_DIR}/configure"
    fi

    mkdir -p "${FFMPEG_BUILD}"
    cd "${FFMPEG_BUILD}"

    # Patent-free lite build: disable all encoders and all decoders,
    # then whitelist only royalty-free codecs (AV1, VP8/9, Opus,
    # Vorbis, FLAC, Theora, common image formats, raw PCM/video).
    # System FFmpeg is tried first at runtime; this bundled build is
    # a fallback for systems without FFmpeg installed.
    "${FFMPEG_DIR}/configure" \
        --prefix="${FFMPEG_INSTALL}" \
        --enable-shared \
        --disable-static \
        --enable-gpl \
        --enable-version3 \
        --disable-encoders \
        --disable-decoders \
        --enable-encoder=libaom_av1 \
        --enable-encoder=libopus \
        --enable-encoder=libvorbis \
        --enable-encoder=flac \
        --enable-encoder=vp8 \
        --enable-encoder=vp9 \
        --enable-encoder=libtheora \
        --enable-encoder=png \
        --enable-encoder=mjpeg \
        --enable-encoder=gif \
        --enable-encoder=bmp \
        --enable-encoder=tiff \
        --enable-encoder=targa \
        --enable-encoder=webp \
        --enable-encoder=apng \
        --enable-encoder=pcm_s16le \
        --enable-encoder=pcm_s16be \
        --enable-encoder=pcm_u8 \
        --enable-encoder=pcm_f32le \
        --enable-encoder=pcm_s24le \
        --enable-encoder=pcm_s32le \
        --enable-encoder=rawvideo \
        --enable-decoder=libaom_av1 \
        --enable-decoder=libdav1d \
        --enable-decoder=vp8 \
        --enable-decoder=vp9 \
        --enable-decoder=flac \
        --enable-decoder=opus \
        --enable-decoder=libopus \
        --enable-decoder=vorbis \
        --enable-decoder=libvorbis \
        --enable-decoder=theora \
        --enable-decoder=libtheora \
        --enable-decoder=png \
        --enable-decoder=mjpeg \
        --enable-decoder=gif \
        --enable-decoder=bmp \
        --enable-decoder=tiff \
        --enable-decoder=targa \
        --enable-decoder=webp \
        --enable-decoder=apng \
        --enable-decoder=pcm_s16le \
        --enable-decoder=pcm_s16be \
        --enable-decoder=pcm_u8 \
        --enable-decoder=pcm_f32le \
        --enable-decoder=pcm_s24le \
        --enable-decoder=pcm_s32le \
        --enable-decoder=rawvideo \
        --enable-libaom \
        --enable-libdav1d \
        --enable-libwebp \
        --disable-doc \
        --disable-programs \
        --disable-debug

    make -j "${JOBS}"
    make install
    ok "FFmpeg (patent-free lite) installed to ${FFMPEG_INSTALL}"

    cd "${PROJECT_ROOT}"
fi

# ======================================================================
#  5b. Build LibRaw (camera RAW decoding)                         # NEW
# ======================================================================
step "Building LibRaw"

if [ -f "${LIBRAW_INSTALL}/lib/libraw.so" ] || \
   [ -f "${LIBRAW_INSTALL}/lib64/libraw.so" ] || \
   [ -f "${LIBRAW_INSTALL}/lib/libraw.dylib" ] || \
   [ -f "${LIBRAW_INSTALL}/lib/libraw.a" ]; then
    warn "LibRaw already built — skipping (delete ${LIBRAW_INSTALL} to rebuild)"
else
    cd "${LIBRAW_DIR}"

    # LibRaw ships configure.ac + Makefile.am; generate configure
    if [ ! -f "configure" ]; then
        step "Running autoreconf for LibRaw"
        autoreconf --install
    fi

    if [ ! -f "configure" ]; then
        fail "autoreconf did not produce configure. Install autoconf, automake, libtool."
    fi

    mkdir -p "${LIBRAW_BUILD}"
    cd "${LIBRAW_BUILD}"

    # Locate the bundled libjpeg-turbo (it may install to lib/ or lib64/)
    if [ -d "${LIBJPEG_INSTALL}/lib64" ]; then
        LIBJPEG_LIBDIR="${LIBJPEG_INSTALL}/lib64"
    else
        LIBJPEG_LIBDIR="${LIBJPEG_INSTALL}/lib"
    fi

    # Force libRaw to link against OUR libjpeg-turbo (SONAME libjpeg.so.8),
    # not the system one (libjpeg.so.62), to avoid versioned-symbol mismatches
    # at final link time.
    PKG_CONFIG_PATH="${LIBJPEG_LIBDIR}/pkgconfig:${PKG_CONFIG_PATH:-}" \
    CPPFLAGS="-I${LIBJPEG_INSTALL}/include ${CPPFLAGS:-}" \
    LDFLAGS="-L${LIBJPEG_LIBDIR} -Wl,-rpath,${LIBJPEG_LIBDIR} ${LDFLAGS:-}" \
    LIBS="-ljpeg" \
    "${LIBRAW_DIR}/configure" \
        --prefix="${LIBRAW_INSTALL}" \
        --enable-shared \
        --disable-static \
        --disable-examples \
        --disable-openmp \
        --enable-jpeg \
        --with-pic

    make -j "${JOBS}"
    make install
    ok "LibRaw installed to ${LIBRAW_INSTALL} (linked against bundled libjpeg-turbo at ${LIBJPEG_LIBDIR})"

    cd "${PROJECT_ROOT}"
fi

# ======================================================================
#  6. Build / unpack Wasmer
# ======================================================================
step "Preparing Wasmer"

if [ -f "${WASMER_INSTALL}/lib/libwasmer.a" ] || \
   [ -f "${WASMER_INSTALL}/lib/libwasmer.so" ] || \
   [ -f "${WASMER_INSTALL}/lib64/libwasmer.a" ] || \
   [ -f "${WASMER_INSTALL}/lib64/libwasmer.so" ] || \
   [ -f "${WASMER_INSTALL}/lib/libwasmer.dylib" ]; then
    warn "Wasmer already present — skipping"
else
    if [ -f "${WASMER_DIR}/Makefile" ]; then
        make -C "${WASMER_DIR}" -j "${JOBS}"
        ok "Wasmer built via Makefile"
    elif [ -f "${WASMER_DIR}/Cargo.toml" ]; then
        check_tool cargo
        (cd "${WASMER_DIR}" && cargo build --release -p wasmer-c-api --no-default-features --features cranelift,wasi)
        mkdir -p "${WASMER_INSTALL}/lib" "${WASMER_INSTALL}/include"
        cp "${WASMER_DIR}/target/release/libwasmer_c_api."* "${WASMER_INSTALL}/lib/" 2>/dev/null || true
        cp "${WASMER_DIR}"/lib/c-api/*.h "${WASMER_INSTALL}/include/" 2>/dev/null || true
        ok "Wasmer built via Cargo"
    else
        if [ -d "${WASMER_INSTALL}/include" ]; then
            ok "Wasmer pre-built package found"
        else
            fail "No Wasmer build system or package found in ${WASMER_DIR}"
        fi
    fi
fi

# ======================================================================
#  7. Build Slang (using its own cmake presets)
# ======================================================================
step "Building Slang"

if [ -f "${SLANG_INSTALL}/lib/libslang.so" ] || \
   [ -f "${SLANG_INSTALL}/lib64/libslang.so" ] || \
   [ -f "${SLANG_INSTALL}/lib/libslang.dylib" ] || \
   [ -f "${SLANG_INSTALL}/lib/libslang-compiler.so" ] || \
   [ -f "${SLANG_INSTALL}/lib64/libslang-compiler.so" ] || \
   [ -f "${SLANG_INSTALL}/lib/libslang-compiler.dylib" ]; then
    warn "Slang already built — skipping (delete ${SLANG_INSTALL} to rebuild)"
else
    cd "${SLANG_DIR}"

    # Configure with examples and tests disabled.  The cpu-shader-llvm-link
    # example pulls in an LLVM-dependent object that breaks the link when
    # LLVM/slang-llvm support isn't fully wired up in this environment.
    # SLANG_ENABLE_SLANG_RHI is also needed for the RHI dependency; we
    # keep it default ON but disable the test+example targets that
    # transitively require LLVM bits we don't have.
    if [ ! -f "${SLANG_BUILD}/build.ninja" ] && [ ! -f "${SLANG_BUILD}/Makefile" ]; then
        cmake --preset default \
            -DSLANG_ENABLE_EXAMPLES=OFF \
            -DSLANG_ENABLE_TESTS=OFF \
            -DSLANG_ENABLE_REPLAYER=OFF \
            -DSLANG_ENABLE_GFX=OFF \
            -DSLANG_ENABLE_SLANG_RHI=OFF \
            -DSLANG_ENABLE_SLANGRT=OFF \
            -DSLANG_SLANG_LLVM_FLAVOR=DISABLE
    fi

    # Build all enabled targets so cmake --install can find every artifact
    # it references (slang-glslang, slangd, slangc, etc.).
    cmake --build --preset release

    cmake --install build --prefix "${SLANG_INSTALL}" --config Release
    ok "Slang installed to ${SLANG_INSTALL}"

    cd "${PROJECT_ROOT}"
fi

# ======================================================================
#  8. Build Dawn
# ======================================================================
step "Building Dawn"

# Dawn may install to lib or lib64 depending on distro
if [ -d "${DAWN_INSTALL}/lib64" ]; then
    DAWN_LIB="${DAWN_INSTALL}/lib64"
else
    DAWN_LIB="${DAWN_INSTALL}/lib"
fi
DAWN_INC="${DAWN_INSTALL}/include"

if [ -f "${DAWN_LIB}/libwebgpu_dawn.a" ] || \
   [ -f "${DAWN_LIB}/libwebgpu_dawn.so" ] || \
   [ -f "${DAWN_INSTALL}/lib/libwebgpu_dawn.a" ] || \
   [ -f "${DAWN_INSTALL}/lib/libwebgpu_dawn.so" ]; then
    warn "Dawn already built — skipping (delete ${DAWN_INSTALL} to rebuild)"
else
    if [ -f "${DAWN_BUILD}/CMakeCache.txt" ]; then
        warn "Removing stale Dawn CMake cache"
        rm -rf "${DAWN_BUILD}"
    fi

    cmake -S "${DAWN_DIR}" -B "${DAWN_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${DAWN_INSTALL}" \
        -DCMAKE_CXX_FLAGS="-Wno-invalid-constexpr -Wno-error=invalid-constexpr" \
        -DDAWN_FETCH_DEPENDENCIES=ON \
        -DDAWN_ENABLE_INSTALL=ON \
        -DDAWN_USE_WAYLAND=ON \
        -DDAWN_USE_X11=ON \
        -DDAWN_BUILD_SAMPLES=OFF \
        -DTINT_BUILD_TESTS=OFF \
        -DTINT_BUILD_CMD_TOOLS=OFF \
        -G Ninja

    cmake --build "${DAWN_BUILD}" -j "${JOBS}"
    cmake --install "${DAWN_BUILD}"
    # Re-detect lib dir after install
    if [ -d "${DAWN_INSTALL}/lib64" ]; then
        DAWN_LIB="${DAWN_INSTALL}/lib64"
    fi
    ok "Dawn installed to ${DAWN_INSTALL}"
fi

# ======================================================================
#  8b. Build DuckDB
# ======================================================================
step "Building DuckDB"

DUCKDB_BUILD="${DUCKDB_DIR}/build/release"
DUCKDB_LIBDIR="${DUCKDB_BUILD}/src"

if [ -f "${DUCKDB_LIBDIR}/libduckdb.so" ]; then
    warn "DuckDB already built — skipping (delete ${DUCKDB_BUILD} to rebuild)"
else
    cmake -S "${DUCKDB_DIR}" -B "${DUCKDB_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -G Ninja

    cmake --build "${DUCKDB_BUILD}" -j "${JOBS}"
    ok "DuckDB built"
fi

# ======================================================================
#  9. Meson setup + compile
# ======================================================================
step "Configuring project with Meson"

# Detect lib vs lib64 for each dep
[ -d "${SDL_INSTALL}/lib64" ] && SDL_LIB="${SDL_INSTALL}/lib64" || SDL_LIB="${SDL_INSTALL}/lib"
[ -d "${SDL_INSTALL}/lib64/pkgconfig" ] && SDL_PKG="${SDL_INSTALL}/lib64/pkgconfig" || SDL_PKG="${SDL_INSTALL}/lib/pkgconfig"

FREETYPE_PKG="${FREETYPE_INSTALL}/lib/pkgconfig"
OPENSSL_PKG="${OPENSSL_INSTALL}/lib/pkgconfig"
ICU_PKG="${ICU_INSTALL}/lib/pkgconfig"
HARFBUZZ_PKG="${HARFBUZZ_INSTALL}/lib/pkgconfig"
FFMPEG_PKG="${FFMPEG_INSTALL}/lib/pkgconfig"
LIBRAW_PKG="${LIBRAW_INSTALL}/lib/pkgconfig"

WASMER_LIB="${WASMER_INSTALL}/lib"
WASMER_INC="${WASMER_INSTALL}/include"
FREETYPE_LIB="${FREETYPE_INSTALL}/lib"
FREETYPE_INC="${FREETYPE_INSTALL}/include/freetype2"
OPENSSL_LIB="${OPENSSL_INSTALL}/lib"
OPENSSL_INC="${OPENSSL_INSTALL}/include"
OPENSSL_MODULES="${OPENSSL_INSTALL}/lib/ossl-modules"
ICU_LIB="${ICU_INSTALL}/lib"
ICU_INC="${ICU_INSTALL}/include"
HARFBUZZ_LIB="${HARFBUZZ_INSTALL}/lib"
HARFBUZZ_INC="${HARFBUZZ_INSTALL}/include"
FFMPEG_LIB="${FFMPEG_INSTALL}/lib"
FFMPEG_INC="${FFMPEG_INSTALL}/include"
LIBRAW_LIB="${LIBRAW_INSTALL}/lib"
LIBRAW_INC="${LIBRAW_INSTALL}/include"

BZIP2_PKG="${BZIP2_INSTALL}/lib/pkgconfig"
LIBJPEG_PKG="${LIBJPEG_INSTALL}/lib/pkgconfig"

export PKG_CONFIG_PATH="${SDL_PKG}:${FREETYPE_PKG}:${OPENSSL_PKG}:${ICU_PKG}:${HARFBUZZ_PKG}:${FFMPEG_PKG}:${LIBRAW_PKG}:${BZIP2_PKG}:${LIBJPEG_PKG}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

EXTRA_C_ARGS="-I${WASMER_INC} -I${DAWN_INC} -I${FREETYPE_INC} -I${OPENSSL_INC} -I${ICU_INC} -I${HARFBUZZ_INC} -I${FFMPEG_INC} -I${LIBRAW_INC}"
EXTRA_LINK_ARGS="-L${WASMER_LIB} -L${DAWN_LIB} -L${SDL_LIB} -L${FREETYPE_LIB} -L${OPENSSL_LIB} -L${ICU_LIB} -L${HARFBUZZ_LIB} -L${FFMPEG_LIB} -L${LIBRAW_LIB}"

if [ -d "${BUILD_DIR}" ]; then
    step "Reconfiguring existing build directory"
    meson setup "${BUILD_DIR}" "${PROJECT_ROOT}" \
        --reconfigure \
        -Dc_args="${EXTRA_C_ARGS}" \
        -Dc_link_args="${EXTRA_LINK_ARGS}" \
        --buildtype="${MESON_BUILDTYPE}" \
        ${MESON_RELEASE_OPT}
else
    meson setup "${BUILD_DIR}" "${PROJECT_ROOT}" \
        -Dc_args="${EXTRA_C_ARGS}" \
        -Dc_link_args="${EXTRA_LINK_ARGS}" \
        --buildtype="${MESON_BUILDTYPE}" \
        ${MESON_RELEASE_OPT}
fi

ok "Meson configured (${MESON_BUILDTYPE})"

# ======================================================================
#  10. Compile
# ======================================================================
step "Compiling ${BINARY_NAME}"
meson compile -C "${BUILD_DIR}" -j "${JOBS}"
ok "Compilation successful"

# ======================================================================
#  11. Assemble release/ folder
# ======================================================================
step "Assembling release folder"

rm -rf "${RELEASE_DIR}"
mkdir -p "${RELEASE_DIR}"

# Copy binary
FOUND_BIN="${BUILD_DIR}/${BINARY_NAME}"
if [ ! -f "${FOUND_BIN}" ]; then
    FOUND_BIN="$(find "${BUILD_DIR}" -name "${BINARY_NAME}" -type f -executable | head -1)"
fi
if [ -z "${FOUND_BIN}" ] || [ ! -f "${FOUND_BIN}" ]; then
    fail "Could not find compiled binary '${BINARY_NAME}' in ${BUILD_DIR}"
fi
cp "${FOUND_BIN}" "${RELEASE_DIR}/${BINARY_NAME}"
chmod +x "${RELEASE_DIR}/${BINARY_NAME}"
ok "Copied ${BINARY_NAME}"

# Copy font-paint-cli contrib tool
FOUND_FPC="${BUILD_DIR}/${CONTRIB_FONT_PAINT}"
if [ ! -f "${FOUND_FPC}" ]; then
    FOUND_FPC="$(find "${BUILD_DIR}" -name "${CONTRIB_FONT_PAINT}" -type f -executable 2>/dev/null | head -1)"
fi
if [ -n "${FOUND_FPC}" ] && [ -f "${FOUND_FPC}" ]; then
    cp "${FOUND_FPC}" "${RELEASE_DIR}/${CONTRIB_FONT_PAINT}"
    chmod +x "${RELEASE_DIR}/${CONTRIB_FONT_PAINT}"
    ok "Copied ${CONTRIB_FONT_PAINT}"
else
    warn "${CONTRIB_FONT_PAINT} not found in build — skipping"
fi

# Copy shader-bind contrib tool
FOUND_SB="${BUILD_DIR}/${CONTRIB_SHADER_BIND}"
if [ ! -f "${FOUND_SB}" ]; then
    FOUND_SB="$(find "${BUILD_DIR}" -name "${CONTRIB_SHADER_BIND}" -type f -executable 2>/dev/null | head -1)"
fi
if [ -n "${FOUND_SB}" ] && [ -f "${FOUND_SB}" ]; then
    cp "${FOUND_SB}" "${RELEASE_DIR}/${CONTRIB_SHADER_BIND}"
    chmod +x "${RELEASE_DIR}/${CONTRIB_SHADER_BIND}"
    ok "Copied ${CONTRIB_SHADER_BIND}"
else
    warn "${CONTRIB_SHADER_BIND} not found in build — skipping"
fi

# Copy shader-preview contrib tool
FOUND_SP="${BUILD_DIR}/${CONTRIB_SHADER_PREVIEW}"
if [ ! -f "${FOUND_SP}" ]; then
    FOUND_SP="$(find "${BUILD_DIR}" -name "${CONTRIB_SHADER_PREVIEW}" -type f -executable 2>/dev/null | head -1)"
fi
if [ -n "${FOUND_SP}" ] && [ -f "${FOUND_SP}" ]; then
    cp "${FOUND_SP}" "${RELEASE_DIR}/${CONTRIB_SHADER_PREVIEW}"
    chmod +x "${RELEASE_DIR}/${CONTRIB_SHADER_PREVIEW}"
    ok "Copied ${CONTRIB_SHADER_PREVIEW}"
else
    warn "${CONTRIB_SHADER_PREVIEW} not found in build — skipping"
fi

# Copy svg-to-slang contrib tool
FOUND_SVG="${BUILD_DIR}/${CONTRIB_SVG_TO_SLANG}"
if [ ! -f "${FOUND_SVG}" ]; then
    FOUND_SVG="$(find "${BUILD_DIR}" -name "${CONTRIB_SVG_TO_SLANG}" -type f -executable 2>/dev/null | head -1)"
fi
if [ -n "${FOUND_SVG}" ] && [ -f "${FOUND_SVG}" ]; then
    cp "${FOUND_SVG}" "${RELEASE_DIR}/${CONTRIB_SVG_TO_SLANG}"
    chmod +x "${RELEASE_DIR}/${CONTRIB_SVG_TO_SLANG}"
    ok "Copied ${CONTRIB_SVG_TO_SLANG}"
else
    warn "${CONTRIB_SVG_TO_SLANG} not found in build — skipping (libxml2 missing?)"
fi

# Strip binaries in release mode
if $RELEASE_MODE && command -v strip &>/dev/null; then
    strip "${RELEASE_DIR}/${BINARY_NAME}"
    ok "Stripped ${BINARY_NAME}"
    if [ -f "${RELEASE_DIR}/${CONTRIB_FONT_PAINT}" ]; then
        strip "${RELEASE_DIR}/${CONTRIB_FONT_PAINT}"
        ok "Stripped ${CONTRIB_FONT_PAINT}"
    fi
    if [ -f "${RELEASE_DIR}/${CONTRIB_SHADER_BIND}" ]; then
        strip "${RELEASE_DIR}/${CONTRIB_SHADER_BIND}"
        ok "Stripped ${CONTRIB_SHADER_BIND}"
    fi
    if [ -f "${RELEASE_DIR}/${CONTRIB_SHADER_PREVIEW}" ]; then
        strip "${RELEASE_DIR}/${CONTRIB_SHADER_PREVIEW}"
        ok "Stripped ${CONTRIB_SHADER_PREVIEW}"
    fi
    if [ -f "${RELEASE_DIR}/${CONTRIB_SVG_TO_SLANG}" ]; then
        strip "${RELEASE_DIR}/${CONTRIB_SVG_TO_SLANG}"
        ok "Stripped ${CONTRIB_SVG_TO_SLANG}"
    fi
fi

# Collect all shared libraries the binary needs from our deps
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

# SDL3
collect_so "${SDL_BUILD}"        "libSDL3.so*"
collect_so "${SDL_INSTALL}/lib"  "libSDL3.so*"
ok "Collected SDL3"

# FreeType
collect_so "${FREETYPE_INSTALL}/lib" "libfreetype.so*"
ok "Collected FreeType"

# OpenSSL
collect_so "${OPENSSL_INSTALL}/lib" "libcrypto.so*"
collect_so "${OPENSSL_INSTALL}/lib" "libssl.so*"
ok "Collected OpenSSL"

# oqs-provider (OpenSSL module — goes into ossl-modules/ subdirectory)
if [ -d "${OPENSSL_INSTALL}/lib/ossl-modules" ]; then
    mkdir -p "${RELEASE_DIR}/ossl-modules"
    for f in "${OPENSSL_INSTALL}/lib/ossl-modules/"*; do
        [ -f "$f" ] || continue
        cp "$f" "${RELEASE_DIR}/ossl-modules/"
    done
    ok "Collected oqs-provider modules"
fi

# ICU
collect_so "${ICU_INSTALL}/lib"   "libicuuc.so*"
collect_so "${ICU_INSTALL}/lib"   "libicui18n.so*"
collect_so "${ICU_INSTALL}/lib"   "libicudata.so*"
collect_so "${ICU_INSTALL}/lib64" "libicuuc.so*"
collect_so "${ICU_INSTALL}/lib64" "libicui18n.so*"
collect_so "${ICU_INSTALL}/lib64" "libicudata.so*"
ok "Collected ICU"

# HarfBuzz
collect_so "${HARFBUZZ_INSTALL}/lib" "libharfbuzz.so*"
ok "Collected HarfBuzz"

# FFmpeg
collect_so "${FFMPEG_INSTALL}/lib" "libavcodec.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libavformat.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libavutil.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libswresample.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libswscale.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libavfilter.so*"
ok "Collected FFmpeg"

# LibRaw                                                          # NEW
collect_so "${LIBRAW_INSTALL}/lib" "libraw.so*"
collect_so "${LIBRAW_INSTALL}/lib" "libraw_r.so*"
ok "Collected LibRaw"

# bzip2
collect_so "${BZIP2_INSTALL}/lib" "libbz2.so*"
collect_so "${BZIP2_INSTALL}/lib64" "libbz2.so*"
# deps/bzip2 1.1.0 produces soname libbz2.so.1; add compat symlink for
# anything that was linked against older libbz2.so.1.0
if [ -f "${RELEASE_DIR}/libbz2.so.1" ] && [ ! -e "${RELEASE_DIR}/libbz2.so.1.0" ]; then
    ln -sf libbz2.so.1 "${RELEASE_DIR}/libbz2.so.1.0"
fi
ok "Collected bzip2"

# libjpeg-turbo
collect_so "${LIBJPEG_INSTALL}/lib" "libjpeg.so*"
collect_so "${LIBJPEG_INSTALL}/lib64" "libjpeg.so*"
collect_so "${LIBJPEG_INSTALL}/lib" "libturbojpeg.so*"
collect_so "${LIBJPEG_INSTALL}/lib64" "libturbojpeg.so*"
ok "Collected libjpeg-turbo"

# Wasmer
collect_so "${WASMER_INSTALL}/lib" "libwasmer.so*"
ok "Collected Wasmer"

# Dawn
collect_so "${DAWN_LIB}" "libwebgpu_dawn.so*"
collect_so "${DAWN_LIB}" "libdawn*.so*"
ok "Collected Dawn"

# DuckDB
collect_so "${DUCKDB_DIR}/build/release/src" "libduckdb.so*"
ok "Collected DuckDB"

# Slang — copy all .so files preserving symlink structure exactly.
# collect_so breaks Slang's unusual symlink layout (libslang.so can
# point to libslang-compiler.so.x.x.x.x via intermediate names).
# cp -a preserves symlinks as-is; we then fix up dangling ones.
for f in "${SLANG_INSTALL}/lib/"libslang*.so* "${SLANG_INSTALL}/lib/"libgfx*.so*; do
    [ -e "$f" ] || [ -L "$f" ] || continue
    cp -a "$f" "${RELEASE_DIR}/"
done
ok "Collected Slang"

# ======================================================================
#  11a. Slang standard modules — must live next to libslang.so
#
#  Slang's compiler API resolves its standard library modules
#  (core.slang-module, etc.) relative to the directory containing
#  libslang.so.  The modules ship in a directory named
#  slang-standard-module-<version>/ which must be a sibling of the .so.
#
#  We copy it into release/ (where libslang.so lives) and will also
#  copy it into ~/.local/lib/ during the local install step below.
# ======================================================================
step "Collecting Slang standard modules"

SLANG_STDMOD_COLLECTED=false
for d in "${SLANG_INSTALL}/lib/slang-standard-module-"*; do
    if [ -d "$d" ]; then
        SLANG_STDMOD_DIRNAME="$(basename "$d")"
        rm -rf "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
        cp -r "$d" "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
        SLANG_STDMOD_COLLECTED=true
        ok "Collected ${SLANG_STDMOD_DIRNAME} into release/"
        break
    fi
done

# Fallback: look for individual .slang-module files directly in lib/
if ! $SLANG_STDMOD_COLLECTED; then
    SLANG_MODULE_FILES=("${SLANG_INSTALL}/lib/"*.slang-module)
    if [ -e "${SLANG_MODULE_FILES[0]}" ]; then
        for f in "${SLANG_MODULE_FILES[@]}"; do
            cp "$f" "${RELEASE_DIR}/"
        done
        SLANG_STDMOD_COLLECTED=true
        ok "Collected loose .slang-module files into release/"
    fi
fi

# Second fallback: check in share/slang/ or share/
if ! $SLANG_STDMOD_COLLECTED; then
    for search_dir in "${SLANG_INSTALL}/share/slang" "${SLANG_INSTALL}/share"; do
        if [ -d "$search_dir" ]; then
            for d in "${search_dir}/slang-standard-module-"*; do
                if [ -d "$d" ]; then
                    SLANG_STDMOD_DIRNAME="$(basename "$d")"
                    rm -rf "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
                    cp -r "$d" "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
                    SLANG_STDMOD_COLLECTED=true
                    ok "Collected ${SLANG_STDMOD_DIRNAME} from ${search_dir} into release/"
                    break 2
                fi
            done
        fi
    done
fi

# Third fallback: search the entire Slang build tree
if ! $SLANG_STDMOD_COLLECTED; then
    FOUND_STDMOD="$(find "${SLANG_DIR}" -type d -name 'slang-standard-module-*' 2>/dev/null | head -1)"
    if [ -n "${FOUND_STDMOD}" ] && [ -d "${FOUND_STDMOD}" ]; then
        SLANG_STDMOD_DIRNAME="$(basename "${FOUND_STDMOD}")"
        rm -rf "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
        cp -r "${FOUND_STDMOD}" "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
        SLANG_STDMOD_COLLECTED=true
        ok "Collected ${SLANG_STDMOD_DIRNAME} from build tree into release/"
    fi
fi

if ! $SLANG_STDMOD_COLLECTED; then
    warn "Slang standard modules not found — shader-bind may fail with 'module load failed'"
    warn "Looked in: ${SLANG_INSTALL}/lib/, share/slang/, and build tree"
fi

# ======================================================================
#  11b. Build mato_dashboard WASM (the primary dashboard)
# ======================================================================
step "Building mato_dashboard WASM"

MATO_BUILD_SH="${PROJECT_ROOT}/webapps/mato_dashboard/build.sh"

if [ -f "${MATO_BUILD_SH}" ]; then
    chmod +x "${MATO_BUILD_SH}"
    bash "${MATO_BUILD_SH}"

    # Ensure the output landed in release/dashboard/
    if [ -f "${RELEASE_DIR}/dashboard/mato_dashboard.wasm" ]; then
        ok "mato_dashboard.wasm built (release/dashboard/)"
    else
        fail "mato_dashboard build script ran but mato_dashboard.wasm not found in ${RELEASE_DIR}/dashboard/"
    fi
else
    warn "${MATO_BUILD_SH} not found — skipping dashboard WASM build"
fi

# ======================================================================
#  11b2. Build demo webapp WASM (GUI chat demo)
# ======================================================================
step "Building demo webapp WASM"

DEMO_SRC_DIR="${PROJECT_ROOT}/webapps/demo"
DEMO_OUT_DIR="${RELEASE_DIR}/demo"
WASI_SYSROOT="${PROJECT_ROOT}/wasi"
WASM_CC="${WASI_SYSROOT}/bin/wasm32-wasi-clang"

if [ ! -f "${DEMO_SRC_DIR}/main.c" ]; then
    warn "${DEMO_SRC_DIR}/main.c not found — skipping demo WASM build"
elif [ ! -x "${WASM_CC}" ]; then
    warn "${WASM_CC} not found — skipping demo WASM build"
else
    SDK_DIR="${PROJECT_ROOT}/sdk"
    DEMO_SDK_SOURCES=(
        "${SDK_DIR}/wasm_surface.c"
        "${SDK_DIR}/gui/wasm_autolayout.c"
        "${SDK_DIR}/gui/wasm_textlabel.c"
        "${SDK_DIR}/gui/textlabel.c"
        "${SDK_DIR}/gui/wasm_textbox.c"
        "${SDK_DIR}/gui/textbox.c"
        "${SDK_DIR}/gui/unistring.c"
        "${SDK_DIR}/gui/wasm_button.c"
        "${SDK_DIR}/gui/wasm_treeview.c"
        "${SDK_DIR}/gui/treeview.c"
        "${SDK_DIR}/gui/wasm_docbox.c"
        "${SDK_DIR}/gui/docbox.c"
        "${SDK_DIR}/gui/wasm_picturebox.c"
        "${SDK_DIR}/gui/wasm_scrollbar.c"
    )

    for f in "${DEMO_SDK_SOURCES[@]}"; do
        [ -f "$f" ] || fail "Missing SDK source for demo build: $f"
    done

    mkdir -p "${DEMO_OUT_DIR}"

    # WINGIT.h embeds ~23 MiB of video data; bump initial memory to 64 MiB.
    "${WASM_CC}" \
        --target=wasm32-wasip1 \
        -nostartfiles \
        -O2 \
        -Wl,--no-entry \
        -Wl,--allow-undefined \
        --sysroot="${WASI_SYSROOT}" \
        -L "${WASI_SYSROOT}/lib" \
        -l c \
        -I "${SDK_DIR}" \
        -I "${DEMO_SRC_DIR}" \
        -I "${PROJECT_ROOT}/deps/clay/" \
        -Wl,--initial-memory=67108864 \
        -Wl,-z,stack-size=8388608 \
        -o "${DEMO_OUT_DIR}/demo.wasm" \
        "${DEMO_SRC_DIR}/main.c" \
        "${DEMO_SDK_SOURCES[@]}"

    if [ -f "${DEMO_OUT_DIR}/demo.wasm" ]; then
        ok "demo.wasm built (release/demo/, $(du -h "${DEMO_OUT_DIR}/demo.wasm" | cut -f1 | xargs))"
    else
        fail "demo.wasm was not produced at ${DEMO_OUT_DIR}/demo.wasm"
    fi
fi

# ======================================================================
#  11c. Copy icon PNGs into release/ for portability
# ======================================================================
step "Collecting icons into release/"

if [ -d "${ICON_DIR}" ]; then
    mkdir -p "${RELEASE_DIR}/icon"
    for size in 512 256 128 64 32; do
        if [ -f "${ICON_DIR}/icon${size}.png" ]; then
            cp "${ICON_DIR}/icon${size}.png" "${RELEASE_DIR}/icon/"
            ok "Collected icon${size}.png"
        fi
    done
else
    warn "Icon directory ${ICON_DIR} not found — skipping icon collection"
fi

# ======================================================================
#  11d. Patch rpath on ALL collected binaries and shared libraries
# ======================================================================
# Binaries get $ORIGIN so they find .so files next to themselves.
# Shared libraries ALSO get $ORIGIN so transitive deps (e.g. libharfbuzz
# loading libicudata) resolve from the same directory, not from the
# original build tree.

if command -v patchelf &>/dev/null; then
    step "Patching rpath on binaries"

    patchelf --set-rpath '$ORIGIN' "${RELEASE_DIR}/${BINARY_NAME}"
    ok "Patched ${BINARY_NAME} rpath to \$ORIGIN"

    if [ -f "${RELEASE_DIR}/${CONTRIB_FONT_PAINT}" ]; then
        patchelf --set-rpath '$ORIGIN' "${RELEASE_DIR}/${CONTRIB_FONT_PAINT}"
        ok "Patched ${CONTRIB_FONT_PAINT} rpath to \$ORIGIN"
    fi
    if [ -f "${RELEASE_DIR}/${CONTRIB_SHADER_BIND}" ]; then
        patchelf --set-rpath '$ORIGIN' "${RELEASE_DIR}/${CONTRIB_SHADER_BIND}"
        ok "Patched ${CONTRIB_SHADER_BIND} rpath to \$ORIGIN"
    fi
    if [ -f "${RELEASE_DIR}/${CONTRIB_SHADER_PREVIEW}" ]; then
        patchelf --set-rpath '$ORIGIN' "${RELEASE_DIR}/${CONTRIB_SHADER_PREVIEW}"
        ok "Patched ${CONTRIB_SHADER_PREVIEW} rpath to \$ORIGIN"
    fi
    if [ -f "${RELEASE_DIR}/${CONTRIB_SVG_TO_SLANG}" ]; then
        patchelf --set-rpath '$ORIGIN' "${RELEASE_DIR}/${CONTRIB_SVG_TO_SLANG}"
        ok "Patched ${CONTRIB_SVG_TO_SLANG} rpath to \$ORIGIN"
    fi

    step "Patching rpath on collected shared libraries"
    for lib in "${RELEASE_DIR}"/*.so*; do
        [ -f "$lib" ] || continue
        [ -L "$lib" ] && continue
        patchelf --set-rpath '$ORIGIN' "$lib" 2>/dev/null || true
    done
    ok "All shared libraries patched to \$ORIGIN"
else
    echo ""
    echo -e "${YELLOW}WARNING: patchelf is not installed.${NC}"
    echo "Binaries will not have \$ORIGIN rpath and may fail to find shared"
    echo "libraries at runtime unless you set LD_LIBRARY_PATH manually."
    echo ""
    echo "Please install patchelf for your distribution:"
    echo ""
    echo "  Ubuntu / Debian:"
    echo "    sudo apt update && sudo apt install patchelf"
    echo ""
    echo "  Fedora:"
    echo "    sudo dnf install patchelf"
    echo ""
    echo "  Red Hat Enterprise Linux (RHEL):"
    echo "    sudo yum install patchelf"
    echo ""
    echo "  openSUSE:"
    echo "    sudo zypper install patchelf"
    echo ""
    echo "  Arch Linux:"
    echo "    sudo pacman -S patchelf"
    echo ""
    echo "  Nix / NixOS:"
    echo "    nix-env -iA nixpkgs.patchelf"
    echo "    or add 'patchelf' to environment.systemPackages in configuration.nix"
    echo ""
    warn "Continuing without rpath patching"
fi

# ======================================================================
#  12. Optional local installation (~/.local)
# ======================================================================
LOCAL_BIN_DIR="${HOME}/.local/bin"
LOCAL_LIB_DIR="${HOME}/.local/lib"
LOCAL_SHARE_DIR="${HOME}/.local/share"
LOCAL_APPS_DIR="${LOCAL_SHARE_DIR}/applications"
INSTALLED_LOCAL=false

echo ""
echo -en "${CYAN}Install yumibrowser to ~/.local (bin + lib + icons + desktop entry)? [y/N]: ${NC}"
read -r install_answer
case "$install_answer" in
    [yY]|[yY][eE][sS])
        step "Installing to ~/.local"

        mkdir -p "${LOCAL_BIN_DIR}" "${LOCAL_LIB_DIR}" "${LOCAL_APPS_DIR}"

        # ---- Copy binary ----
        cp "${RELEASE_DIR}/${BINARY_NAME}" "${LOCAL_BIN_DIR}/${BINARY_NAME}"
        chmod +x "${LOCAL_BIN_DIR}/${BINARY_NAME}"
        ok "Installed ${BINARY_NAME} to ${LOCAL_BIN_DIR}"

        # ---- Copy font-paint-cli if it was built ----
        if [ -f "${RELEASE_DIR}/${CONTRIB_FONT_PAINT}" ]; then
            cp "${RELEASE_DIR}/${CONTRIB_FONT_PAINT}" "${LOCAL_BIN_DIR}/${CONTRIB_FONT_PAINT}"
            chmod +x "${LOCAL_BIN_DIR}/${CONTRIB_FONT_PAINT}"
            ok "Installed ${CONTRIB_FONT_PAINT} to ${LOCAL_BIN_DIR}"
        fi

        # ---- Copy shader-bind if it was built ----
        if [ -f "${RELEASE_DIR}/${CONTRIB_SHADER_BIND}" ]; then
            cp "${RELEASE_DIR}/${CONTRIB_SHADER_BIND}" "${LOCAL_BIN_DIR}/${CONTRIB_SHADER_BIND}"
            chmod +x "${LOCAL_BIN_DIR}/${CONTRIB_SHADER_BIND}"
            ok "Installed ${CONTRIB_SHADER_BIND} to ${LOCAL_BIN_DIR}"
        fi

        # ---- Copy shader-preview if it was built ----
        if [ -f "${RELEASE_DIR}/${CONTRIB_SHADER_PREVIEW}" ]; then
            cp "${RELEASE_DIR}/${CONTRIB_SHADER_PREVIEW}" "${LOCAL_BIN_DIR}/${CONTRIB_SHADER_PREVIEW}"
            chmod +x "${LOCAL_BIN_DIR}/${CONTRIB_SHADER_PREVIEW}"
            ok "Installed ${CONTRIB_SHADER_PREVIEW} to ${LOCAL_BIN_DIR}"
        fi

        # ---- Copy svg-to-slang if it was built ----
        if [ -f "${RELEASE_DIR}/${CONTRIB_SVG_TO_SLANG}" ]; then
            cp "${RELEASE_DIR}/${CONTRIB_SVG_TO_SLANG}" "${LOCAL_BIN_DIR}/${CONTRIB_SVG_TO_SLANG}"
            chmod +x "${LOCAL_BIN_DIR}/${CONTRIB_SVG_TO_SLANG}"
            ok "Installed ${CONTRIB_SVG_TO_SLANG} to ${LOCAL_BIN_DIR}"
        fi

        # ---- Copy shared libraries (preserving symlinks exactly) ----
        for f in "${RELEASE_DIR}"/*.so* ; do
            [ -e "$f" ] || [ -L "$f" ] || continue
            cp -a "$f" "${LOCAL_LIB_DIR}/"
        done
        ok "Installed shared libraries to ${LOCAL_LIB_DIR}"

        # ---- Copy Slang standard modules next to libslang.so in ~/.local/lib ----
        # Slang resolves its standard modules relative to libslang.so's directory.
        # Since libslang.so goes into ~/.local/lib, the modules must be there too.
        step "Installing Slang standard modules to ${LOCAL_LIB_DIR}"

        SLANG_LOCAL_INSTALLED=false
        for d in "${RELEASE_DIR}/slang-standard-module-"*; do
            if [ -d "$d" ]; then
                LOCAL_STDMOD_DIR="${LOCAL_LIB_DIR}/$(basename "$d")"
                rm -rf "${LOCAL_STDMOD_DIR}"
                cp -r "$d" "${LOCAL_STDMOD_DIR}"
                SLANG_LOCAL_INSTALLED=true
                ok "Installed $(basename "$d") to ${LOCAL_LIB_DIR}"
                break
            fi
        done
        # Also copy loose .slang-module files if present
        for f in "${RELEASE_DIR}"/*.slang-module; do
            if [ -f "$f" ]; then
                cp "$f" "${LOCAL_LIB_DIR}/"
                SLANG_LOCAL_INSTALLED=true
            fi
        done
        if $SLANG_LOCAL_INSTALLED; then
            ok "Slang standard modules installed next to libslang.so"
        else
            warn "No Slang standard modules found to install — shader-bind may fail"
        fi

        # ---- Install freedesktop icons ----
        install_icons "${LOCAL_SHARE_DIR}"

        # ---- Install WASM dashboard to XDG data dir ----
        YUMI_DATA_DIR="${LOCAL_SHARE_DIR}/${APP_ID}"
        step "Installing mato_dashboard to ${YUMI_DATA_DIR}"

        if [ -d "${RELEASE_DIR}/dashboard" ]; then
            mkdir -p "${YUMI_DATA_DIR}/dashboard"
            # Remove any stale wasm files from previous builds
            rm -f "${YUMI_DATA_DIR}/dashboard/"*.wasm 2>/dev/null || true
            rm -rf "${YUMI_DATA_DIR}/webapps" 2>/dev/null || true
            cp -a "${RELEASE_DIR}/dashboard/mato_dashboard.wasm" "${YUMI_DATA_DIR}/dashboard/"
            ok "Installed mato_dashboard.wasm to ${YUMI_DATA_DIR}/dashboard/"
        fi

        # ---- Patch rpath to find libs in ~/.local/lib ----
        if command -v patchelf &>/dev/null; then
            patchelf --set-rpath "${LOCAL_LIB_DIR}" "${LOCAL_BIN_DIR}/${BINARY_NAME}"
            ok "Patched ${BINARY_NAME} rpath to ${LOCAL_LIB_DIR}"
            if [ -f "${LOCAL_BIN_DIR}/${CONTRIB_FONT_PAINT}" ]; then
                patchelf --set-rpath "${LOCAL_LIB_DIR}" "${LOCAL_BIN_DIR}/${CONTRIB_FONT_PAINT}"
                ok "Patched ${CONTRIB_FONT_PAINT} rpath to ${LOCAL_LIB_DIR}"
            fi
            if [ -f "${LOCAL_BIN_DIR}/${CONTRIB_SHADER_BIND}" ]; then
                patchelf --set-rpath "${LOCAL_LIB_DIR}" "${LOCAL_BIN_DIR}/${CONTRIB_SHADER_BIND}"
                ok "Patched ${CONTRIB_SHADER_BIND} rpath to ${LOCAL_LIB_DIR}"
            fi
            if [ -f "${LOCAL_BIN_DIR}/${CONTRIB_SHADER_PREVIEW}" ]; then
                patchelf --set-rpath "${LOCAL_LIB_DIR}" "${LOCAL_BIN_DIR}/${CONTRIB_SHADER_PREVIEW}"
                ok "Patched ${CONTRIB_SHADER_PREVIEW} rpath to ${LOCAL_LIB_DIR}"
            fi
            if [ -f "${LOCAL_BIN_DIR}/${CONTRIB_SVG_TO_SLANG}" ]; then
                patchelf --set-rpath "${LOCAL_LIB_DIR}" "${LOCAL_BIN_DIR}/${CONTRIB_SVG_TO_SLANG}"
                ok "Patched ${CONTRIB_SVG_TO_SLANG} rpath to ${LOCAL_LIB_DIR}"
            fi
            # Also patch .so files in ~/.local/lib
            for lib in "${LOCAL_LIB_DIR}"/*.so*; do
                [ -f "$lib" ] || continue
                [ -L "$lib" ] && continue
                patchelf --set-rpath "${LOCAL_LIB_DIR}" "$lib" 2>/dev/null || true
            done
            ok "Patched shared library rpaths to ${LOCAL_LIB_DIR}"
        else
            warn "patchelf not found — you may need to set LD_LIBRARY_PATH=${LOCAL_LIB_DIR}"
        fi

        # ---- Desktop entry pointing to ~/.local/bin ----
        LOCAL_DESKTOP="${LOCAL_APPS_DIR}/yumibrowser.desktop"
        cat > "${LOCAL_DESKTOP}" <<DESKEOF
[Desktop Entry]
Type=Application
Name=Yumi Browser
Comment=WebAssembly browser runtime with GPU acceleration
Exec=${LOCAL_BIN_DIR}/${BINARY_NAME} %f
Icon=yumibrowser
Terminal=false
Categories=Development;WebBrowser;
StartupWMClass=yumibrowser
MimeType=application/wasm;
DESKEOF
        ok "Installed desktop entry to ${LOCAL_DESKTOP}"

        if command -v update-desktop-database &>/dev/null; then
            update-desktop-database "${LOCAL_APPS_DIR}" 2>/dev/null || true
            ok "Desktop database updated"
        else
            warn "update-desktop-database not found — desktop entry may not appear until next login"
        fi

        INSTALLED_LOCAL=true

        # ---- Remind about PATH ----
        if ! echo "$PATH" | tr ':' '\n' | grep -qx "${LOCAL_BIN_DIR}"; then
            echo ""
            warn "${LOCAL_BIN_DIR} is not in your PATH"
            echo "  Add it to your shell profile:"
            echo "    echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.bashrc"
            echo "  Then restart your shell or run:  source ~/.bashrc"
        fi
        ;;
    *)
        warn "Skipped local installation"
        ;;
esac

# ======================================================================
#  13. Desktop entry in release/ (always created for portability)
# ======================================================================
step "Creating desktop entry in release/"

DESKTOP_FILE="${RELEASE_DIR}/yumibrowser.desktop"
RELEASE_ABS="$(cd "${RELEASE_DIR}" && pwd)"

cat > "${DESKTOP_FILE}" <<DESKEOF2
[Desktop Entry]
Type=Application
Name=Yumi Browser
Comment=WebAssembly browser runtime with GPU acceleration
Exec=${RELEASE_ABS}/${BINARY_NAME} %f
Icon=yumibrowser
Terminal=false
Categories=Development;WebBrowser;
StartupWMClass=yumibrowser
MimeType=application/wasm;
DESKEOF2

ok "Created ${DESKTOP_FILE}"

# If user skipped local install, offer to install just the desktop entry + icons
if ! $INSTALLED_LOCAL; then
    LOCAL_DESKTOP="${LOCAL_APPS_DIR}/yumibrowser.desktop"
    if [ -f "${LOCAL_DESKTOP}" ]; then
        warn "yumibrowser.desktop already installed at ${LOCAL_DESKTOP}"
    else
        echo ""
        echo -en "${CYAN}Install desktop entry + icons to ~/.local/share? [y/N]: ${NC}"
        read -r desktop_answer
        case "$desktop_answer" in
            [yY]|[yY][eE][sS])
                mkdir -p "${LOCAL_APPS_DIR}"
                cp "${DESKTOP_FILE}" "${LOCAL_DESKTOP}"
                ok "Installed to ${LOCAL_DESKTOP}"

                # Install freedesktop icons
                install_icons "${HOME}/.local/share"

                if command -v update-desktop-database &>/dev/null; then
                    update-desktop-database "${LOCAL_APPS_DIR}" 2>/dev/null || true
                    ok "Desktop database updated"
                else
                    warn "update-desktop-database not found — desktop entry may not appear until next login"
                fi
                ;;
            *)
                warn "Skipped desktop entry installation"
                echo "  You can install it later with:"
                echo "    cp ${DESKTOP_FILE} ${LOCAL_APPS_DIR}/"
                echo "    update-desktop-database ${LOCAL_APPS_DIR}"
                ;;
        esac
    fi
fi

# ======================================================================
#  Done
# ======================================================================
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Build complete!                       ${NC}"
if $RELEASE_MODE; then
echo -e "${GREEN}  Mode: RELEASE (-O3)                  ${NC}"
else
echo -e "${GREEN}  Mode: DEBUG                           ${NC}"
fi
echo -e "${GREEN}  Self-contained release in: release/   ${NC}"
if $INSTALLED_LOCAL; then
echo -e "${GREEN}  Installed to ~/.local/bin + lib       ${NC}"
echo -e "${GREEN}  Run:  yumibrowser                     ${NC}"
else
echo -e "${GREEN}  Run:  cd release && ./${BINARY_NAME}  ${NC}"
fi
if [ -f "${RELEASE_DIR}/${CONTRIB_FONT_PAINT}" ]; then
echo -e "${GREEN}  Tool: ./${CONTRIB_FONT_PAINT} -f font.ttf -o font.h${NC}"
fi
if [ -f "${RELEASE_DIR}/${CONTRIB_SHADER_BIND}" ]; then
echo -e "${GREEN}  Tool: ./${CONTRIB_SHADER_BIND} -i x.slang -o x_bind.h${NC}"
fi
if [ -f "${RELEASE_DIR}/${CONTRIB_SHADER_PREVIEW}" ]; then
echo -e "${GREEN}  Tool: ./${CONTRIB_SHADER_PREVIEW} <shader.slang>${NC}"
fi
if [ -f "${RELEASE_DIR}/${CONTRIB_SVG_TO_SLANG}" ]; then
echo -e "${GREEN}  Tool: ./${CONTRIB_SVG_TO_SLANG} -i font.h${NC}"
fi
echo -e "${GREEN}========================================${NC}"
