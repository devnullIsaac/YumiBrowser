#!/usr/bin/env bash
# FreeType depends on bzip2 (run scripts/deps/bzip2.sh first).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building FreeType"

if [ -f "${FREETYPE_INSTALL}/lib/libfreetype.so" ] || \
   [ -f "${FREETYPE_INSTALL}/lib/libfreetype.dylib" ] || \
   [ -f "${FREETYPE_INSTALL}/lib/libfreetype.a" ]; then
    warn "FreeType already built — skipping (delete ${FREETYPE_INSTALL} to rebuild)"
    exit 0
fi

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
