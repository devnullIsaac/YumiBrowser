#!/usr/bin/env bash
# Builds Slang via its bundled cmake presets.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building Slang"

if [ -f "${SLANG_INSTALL}/lib/libslang.so" ] || \
   [ -f "${SLANG_INSTALL}/lib/libslang.dylib" ] || \
   [ -f "${SLANG_INSTALL}/lib/libslang-compiler.so" ] || \
   [ -f "${SLANG_INSTALL}/lib/libslang-compiler.dylib" ]; then
    warn "Slang already built — skipping (delete ${SLANG_INSTALL} to rebuild)"
    exit 0
fi

pushd "${SLANG_DIR}" >/dev/null

if [ ! -f "${SLANG_BUILD}/build.ninja" ] && [ ! -f "${SLANG_BUILD}/Makefile" ]; then
    cmake --preset default
fi

cmake --build --preset release

cmake --install build --prefix "${SLANG_INSTALL}" --config Release
ok "Slang installed to ${SLANG_INSTALL}"

popd >/dev/null
