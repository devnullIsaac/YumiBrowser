#!/usr/bin/env bash
# Configures + compiles the main project via Meson.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

step "Checking build tools"
check_tool cmake
check_tool meson
check_tool ninja
check_tool pkg-config
ok "All build tools found"

if [ "$RELEASE_MODE" = "true" ]; then
    step "Build mode: RELEASE (-O3)"
else
    step "Build mode: DEBUG (use --release for optimized build)"
fi

step "Configuring project with Meson"

# Re-detect Dawn lib dir (lib64 vs lib)
if [ -d "${DAWN_INSTALL}/lib64" ]; then
    DAWN_LIB="${DAWN_INSTALL}/lib64"
else
    DAWN_LIB="${DAWN_INSTALL}/lib"
fi
DAWN_INC="${DAWN_INSTALL}/include"

SDL_PKG="${SDL_INSTALL}/lib/pkgconfig"
FREETYPE_PKG="${FREETYPE_INSTALL}/lib/pkgconfig"
OPENSSL_PKG="${OPENSSL_INSTALL}/lib/pkgconfig"
ICU_PKG="${ICU_INSTALL}/lib/pkgconfig"
HARFBUZZ_PKG="${HARFBUZZ_INSTALL}/lib/pkgconfig"
FFMPEG_PKG="${FFMPEG_INSTALL}/lib/pkgconfig"
LIBRAW_PKG="${LIBRAW_INSTALL}/lib/pkgconfig"
BZIP2_PKG="${BZIP2_INSTALL}/lib/pkgconfig"
LIBJPEG_PKG="${LIBJPEG_INSTALL}/lib/pkgconfig"

WASMER_LIB="${WASMER_INSTALL}/lib"
WASMER_INC="${WASMER_INSTALL}/include"
FREETYPE_LIB="${FREETYPE_INSTALL}/lib"
FREETYPE_INC="${FREETYPE_INSTALL}/include/freetype2"
OPENSSL_LIB="${OPENSSL_INSTALL}/lib"
OPENSSL_INC="${OPENSSL_INSTALL}/include"
ICU_LIB="${ICU_INSTALL}/lib"
ICU_INC="${ICU_INSTALL}/include"
HARFBUZZ_LIB="${HARFBUZZ_INSTALL}/lib"
HARFBUZZ_INC="${HARFBUZZ_INSTALL}/include"
FFMPEG_LIB="${FFMPEG_INSTALL}/lib"
FFMPEG_INC="${FFMPEG_INSTALL}/include"
LIBRAW_LIB="${LIBRAW_INSTALL}/lib"
LIBRAW_INC="${LIBRAW_INSTALL}/include"

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

step "Compiling ${BINARY_NAME}"
meson compile -C "${BUILD_DIR}" -j "${JOBS}"
ok "Compilation successful"
