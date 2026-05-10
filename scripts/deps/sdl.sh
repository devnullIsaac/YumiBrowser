#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building SDL3"

if [ -f "${SDL_INSTALL}/lib/libSDL3.so" ] || \
   [ -f "${SDL_INSTALL}/lib/libSDL3.dylib" ] || \
   [ -f "${SDL_INSTALL}/lib/libSDL3.a" ]; then
    warn "SDL3 already built — skipping (delete ${SDL_INSTALL} to rebuild)"
    exit 0
fi

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
