#!/usr/bin/env bash
# HarfBuzz depends on FreeType + ICU. Run those scripts first.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building HarfBuzz"

if [ -f "${HARFBUZZ_INSTALL}/lib/libharfbuzz.so" ] || \
   [ -f "${HARFBUZZ_INSTALL}/lib/libharfbuzz.dylib" ] || \
   [ -f "${HARFBUZZ_INSTALL}/lib/libharfbuzz.a" ]; then
    warn "HarfBuzz already built — skipping (delete ${HARFBUZZ_INSTALL} to rebuild)"
    exit 0
fi

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
