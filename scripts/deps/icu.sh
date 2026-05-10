#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building ICU4C"

if [ -f "${ICU_INSTALL}/lib/libicuuc.so" ] || \
   [ -f "${ICU_INSTALL}/lib/libicuuc.dylib" ]; then
    warn "ICU already built — skipping (delete ${ICU_INSTALL} to rebuild)"
    exit 0
fi

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
pushd "${ICU_BUILD}" >/dev/null

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

popd >/dev/null
