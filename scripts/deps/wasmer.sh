#!/usr/bin/env bash
# Builds Wasmer's C-API + stages it into deps/wasmer/package/{include,lib}.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Preparing Wasmer"

if [ -f "${WASMER_INSTALL}/lib/libwasmer.a" ] || \
   [ -f "${WASMER_INSTALL}/lib/libwasmer.so" ] || \
   [ -f "${WASMER_INSTALL}/lib/libwasmer.dylib" ]; then
    warn "Wasmer already present — skipping"
    exit 0
fi

if [ -f "${WASMER_DIR}/Makefile" ]; then
    # The bare `make` target builds the wasmer CLI but does not stage the
    # C-API into package/. We need libwasmer + headers under
    # ${WASMER_INSTALL}/{lib,include}, which `package-capi` produces.
    check_tool cargo
    make -C "${WASMER_DIR}" -j "${JOBS}" build-capi
    make -C "${WASMER_DIR}" package-capi
    ok "Wasmer C-API built and packaged via Makefile"
elif [ -f "${WASMER_DIR}/Cargo.toml" ]; then
    check_tool cargo
    (cd "${WASMER_DIR}" && cargo build --release -p wasmer-c-api --no-default-features --features cranelift,wasi)
    mkdir -p "${WASMER_INSTALL}/lib" "${WASMER_INSTALL}/include"
    cp "${WASMER_DIR}/target/release/libwasmer_c_api."* "${WASMER_INSTALL}/lib/" 2>/dev/null || true
    cp "${WASMER_DIR}"/lib/c-api/*.h "${WASMER_INSTALL}/include/" 2>/dev/null || true
    ok "Wasmer built via Cargo"
else
    if [ -d "${WASMER_INSTALL}/include" ]; then
        ok "Wasmer pre-built package found"
    else
        fail "No Wasmer build system or package found in ${WASMER_DIR}"
    fi
fi
