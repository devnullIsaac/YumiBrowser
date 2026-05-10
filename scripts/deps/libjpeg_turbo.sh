#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building libjpeg-turbo"

if [ -f "${LIBJPEG_INSTALL}/lib/libjpeg.so" ] || \
   [ -f "${LIBJPEG_INSTALL}/lib64/libjpeg.so" ]; then
    warn "libjpeg-turbo already built — skipping (delete ${LIBJPEG_INSTALL} to rebuild)"
    exit 0
fi

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
