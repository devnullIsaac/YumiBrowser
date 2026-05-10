#!/usr/bin/env bash
# Builds Dawn (WebGPU). Prefers clang because Dawn/Tint upstream is built
# with Clang and recent GCC rejects Tint's constexpr lambdas with
# -Winvalid-constexpr.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building Dawn"

# Dawn may install to lib or lib64 depending on distro
if [ -d "${DAWN_INSTALL}/lib64" ]; then
    DAWN_LIB="${DAWN_INSTALL}/lib64"
else
    DAWN_LIB="${DAWN_INSTALL}/lib"
fi

if [ -f "${DAWN_LIB}/libwebgpu_dawn.a" ] || \
   [ -f "${DAWN_LIB}/libwebgpu_dawn.so" ] || \
   [ -f "${DAWN_INSTALL}/lib/libwebgpu_dawn.a" ] || \
   [ -f "${DAWN_INSTALL}/lib/libwebgpu_dawn.so" ]; then
    warn "Dawn already built — skipping (delete ${DAWN_INSTALL} to rebuild)"
    exit 0
fi

if [ -f "${DAWN_BUILD}/CMakeCache.txt" ]; then
    warn "Removing stale Dawn CMake cache"
    rm -rf "${DAWN_BUILD}"
fi

# Prefer clang; otherwise neutralise -Winvalid-constexpr for GCC.
DAWN_CMAKE_EXTRA=()
if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    DAWN_CMAKE_EXTRA+=(
        -DCMAKE_C_COMPILER=clang
        -DCMAKE_CXX_COMPILER=clang++
    )
else
    warn "clang not found — building Dawn with GCC and suppressing -Winvalid-constexpr"
    DAWN_CMAKE_EXTRA+=(
        -DCMAKE_CXX_FLAGS="-Wno-invalid-constexpr -Wno-error=invalid-constexpr"
    )
fi

cmake -S "${DAWN_DIR}" -B "${DAWN_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${DAWN_INSTALL}" \
    -DDAWN_FETCH_DEPENDENCIES=ON \
    -DDAWN_ENABLE_INSTALL=ON \
    -DDAWN_USE_WAYLAND=ON \
    -DDAWN_USE_X11=ON \
    -DDAWN_BUILD_SAMPLES=OFF \
    -DTINT_BUILD_TESTS=OFF \
    -DTINT_BUILD_CMD_TOOLS=OFF \
    "${DAWN_CMAKE_EXTRA[@]}" \
    -G Ninja

cmake --build "${DAWN_BUILD}" -j "${JOBS}"
cmake --install "${DAWN_BUILD}"
ok "Dawn installed to ${DAWN_INSTALL}"
