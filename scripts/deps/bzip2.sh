#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building bzip2"

if [ -f "${BZIP2_INSTALL}/lib/libbz2.so" ] || \
   [ -f "${BZIP2_INSTALL}/lib64/libbz2.so" ]; then
    warn "bzip2 already built — skipping (delete ${BZIP2_INSTALL} to rebuild)"
    exit 0
fi

cmake -S "${BZIP2_DIR}" -B "${BZIP2_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${BZIP2_INSTALL}" \
    -DENABLE_SHARED_LIB=ON \
    -DENABLE_STATIC_LIB=OFF \
    -G Ninja

cmake --build "${BZIP2_BUILD}" -j "${JOBS}"
cmake --install "${BZIP2_BUILD}"
ok "bzip2 installed to ${BZIP2_INSTALL}"
