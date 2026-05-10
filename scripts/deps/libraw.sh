#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building LibRaw"

if [ -f "${LIBRAW_INSTALL}/lib/libraw.so" ] || \
   [ -f "${LIBRAW_INSTALL}/lib/libraw.dylib" ] || \
   [ -f "${LIBRAW_INSTALL}/lib/libraw.a" ]; then
    warn "LibRaw already built — skipping (delete ${LIBRAW_INSTALL} to rebuild)"
    exit 0
fi

pushd "${LIBRAW_DIR}" >/dev/null

# LibRaw ships configure.ac + Makefile.am; generate configure
if [ ! -f "configure" ]; then
    step "Running autoreconf for LibRaw"
    autoreconf --install
fi

if [ ! -f "configure" ]; then
    fail "autoreconf did not produce configure. Install autoconf, automake, libtool."
fi

popd >/dev/null

mkdir -p "${LIBRAW_BUILD}"
pushd "${LIBRAW_BUILD}" >/dev/null

"${LIBRAW_DIR}/configure" \
    --prefix="${LIBRAW_INSTALL}" \
    --enable-shared \
    --disable-static \
    --disable-examples \
    --disable-openmp \
    --with-pic

make -j "${JOBS}"
make install
ok "LibRaw installed to ${LIBRAW_INSTALL}"

popd >/dev/null
