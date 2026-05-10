#!/usr/bin/env bash
# Builds OpenSSL 3.x, then liboqs (post-quantum), then oqs-provider (PQ
# OpenSSL provider module). All three install into ${OPENSSL_INSTALL} so
# they're co-located.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

# =====================================================================
# 1. OpenSSL 3.x
# =====================================================================
step "Building OpenSSL"

if [ -f "${OPENSSL_INSTALL}/lib/libcrypto.so" ] || \
   [ -f "${OPENSSL_INSTALL}/lib/libcrypto.so.3" ] || \
   [ -f "${OPENSSL_INSTALL}/lib/libcrypto.dylib" ]; then
    warn "OpenSSL already built — skipping (delete ${OPENSSL_INSTALL} to rebuild)"
else
    pushd "${OPENSSL_DIR}" >/dev/null

    ./Configure \
        --prefix="${OPENSSL_INSTALL}" \
        --openssldir="${OPENSSL_INSTALL}/ssl" \
        --libdir=lib \
        shared \
        no-tests

    make -j "${JOBS}"
    make install_sw install_ssldirs
    ok "OpenSSL installed to ${OPENSSL_INSTALL}"

    popd >/dev/null
fi

# =====================================================================
# 2. liboqs
# =====================================================================
step "Building liboqs"

if [ -f "${LIBOQS_INSTALL}/lib/liboqs.a" ] || \
   [ -f "${LIBOQS_INSTALL}/lib/liboqs.so" ] || \
   [ -f "${LIBOQS_INSTALL}/lib/liboqs.dylib" ]; then
    warn "liboqs already built — skipping (delete ${LIBOQS_INSTALL}/lib/liboqs.* to rebuild)"
else
    if [ ! -d "${LIBOQS_SRC}" ]; then
        echo "Cloning liboqs (main branch)..."
        git clone --depth 1 --branch main \
            https://github.com/open-quantum-safe/liboqs.git "${LIBOQS_SRC}"
    fi

    # liboqs 0.15.0 changed OQS_ALGS_ENABLED=STD (the default) to drop SPHINCS+
    # in favour of SLH-DSA, but oqs-provider 0.11.0 still references the
    # SPHINCS+ symbols. Build with the full algorithm set so those exist.
    cmake -S "${LIBOQS_SRC}" -B "${LIBOQS_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${LIBOQS_INSTALL}" \
        -DOPENSSL_ROOT_DIR="${OPENSSL_INSTALL}" \
        -DOQS_ALGS_ENABLED=All \
        -DOQS_LIBJADE_BUILD=OFF \
        -G Ninja

    cmake --build "${LIBOQS_BUILD}" -j "${JOBS}"
    cmake --install "${LIBOQS_BUILD}"
    ok "liboqs installed to ${LIBOQS_INSTALL}"
fi

# =====================================================================
# 3. oqs-provider — installs oqsprovider.so into OpenSSL's ossl-modules/
# =====================================================================
step "Building oqs-provider"

if [ -f "${OPENSSL_INSTALL}/lib/ossl-modules/oqsprovider.so" ] || \
   [ -f "${OPENSSL_INSTALL}/lib/ossl-modules/oqsprovider.dylib" ]; then
    warn "oqs-provider already built — skipping (delete ${OPENSSL_INSTALL}/lib/ossl-modules/oqsprovider.* to rebuild)"
    exit 0
fi

if [ -f "${OQS_BUILD}/CMakeCache.txt" ]; then
    warn "Removing stale oqs-provider CMake cache"
    rm -rf "${OQS_BUILD}"
fi

cmake -S "${OQS_DIR}" -B "${OQS_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${OPENSSL_INSTALL}" \
    -DOPENSSL_ROOT_DIR="${OPENSSL_INSTALL}" \
    -Dliboqs_DIR="${LIBOQS_INSTALL}/lib/cmake/liboqs" \
    -G Ninja

cmake --build "${OQS_BUILD}" -j "${JOBS}"

mkdir -p "${OPENSSL_INSTALL}/lib/ossl-modules"
OQS_SO="$(find "${OQS_BUILD}" -name 'oqsprovider.so' -o -name 'oqsprovider.dylib' | head -1)"
if [ -n "${OQS_SO}" ] && [ -f "${OQS_SO}" ]; then
    cp "${OQS_SO}" "${OPENSSL_INSTALL}/lib/ossl-modules/"
    ok "oqs-provider installed to ${OPENSSL_INSTALL}/lib/ossl-modules/"
else
    fail "Could not find oqsprovider.so in ${OQS_BUILD}"
fi
