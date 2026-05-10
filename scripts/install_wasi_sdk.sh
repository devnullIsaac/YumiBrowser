#!/usr/bin/env bash
# Install wasi-sdk + WASIX sysroot into deps/wasi/{bin,include,lib,share}.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

step "Installing wasi-sdk + WASIX sysroot"

# Detect host arch/OS for wasi-sdk asset selection
HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
HOST_ARCH="$(uname -m)"
case "${HOST_OS}" in
    linux)  WASI_SDK_OS="linux" ;;
    darwin) WASI_SDK_OS="macos" ;;
    *) fail "Unsupported host OS for wasi-sdk: ${HOST_OS}" ;;
esac
case "${HOST_ARCH}" in
    x86_64|amd64)        WASI_SDK_ARCH="x86_64" ;;
    aarch64|arm64)       WASI_SDK_ARCH="arm64" ;;
    riscv64)             WASI_SDK_ARCH="riscv64" ;;
    *) fail "Unsupported host arch for wasi-sdk: ${HOST_ARCH}" ;;
esac
WASI_SDK_ASSET="wasi-sdk-${WASI_SDK_VERSION}-${WASI_SDK_ARCH}-${WASI_SDK_OS}.tar.gz"
WASI_SDK_URL="https://github.com/WebAssembly/wasi-sdk/releases/download/${WASI_SDK_TAG}/${WASI_SDK_ASSET}"

WASI_STAMP="${WASI_DIR}/.version"
WASI_STAMP_CONTENT="${WASI_SDK_TAG} | wasix-libc=${WASIX_TAG}"
WASI_NEEDS_INSTALL=true
if [ -f "${WASI_STAMP}" ] && grep -qx "${WASI_STAMP_CONTENT}" "${WASI_STAMP}" 2>/dev/null; then
    if [ -x "${WASI_DIR}/bin/clang" ]; then
        WASI_NEEDS_INSTALL=false
    fi
fi

if ! $WASI_NEEDS_INSTALL; then
    warn "wasi-sdk ${WASI_SDK_TAG} already installed — skipping (delete ${WASI_DIR} to reinstall)"
else
    if ! command -v curl &>/dev/null && ! command -v wget &>/dev/null; then
        fail "Need either curl or wget to download wasi-sdk"
    fi

    rm -rf "${WASI_DIR}"
    mkdir -p "${WASI_DIR}"

    WASI_TMP="$(mktemp -d)"
    trap 'rm -rf "${WASI_TMP}"' EXIT

    # ---- 1. wasi-sdk (compilers + sysroot) ----
    step "Downloading ${WASI_SDK_URL}"
    if command -v curl &>/dev/null; then
        curl -fL --retry 3 --progress-bar -o "${WASI_TMP}/wasi-sdk.tar.gz" "${WASI_SDK_URL}"
    else
        wget --show-progress -O "${WASI_TMP}/wasi-sdk.tar.gz" "${WASI_SDK_URL}"
    fi

    step "Extracting wasi-sdk"
    tar -xzf "${WASI_TMP}/wasi-sdk.tar.gz" -C "${WASI_TMP}"
    WASI_SDK_SRC="$(find "${WASI_TMP}" -mindepth 1 -maxdepth 1 -type d -name 'wasi-sdk-*' | head -1)"
    [ -d "${WASI_SDK_SRC}/bin" ] || fail "wasi-sdk tarball missing bin/"
    [ -d "${WASI_SDK_SRC}/lib" ] || fail "wasi-sdk tarball missing lib/"

    cp -a "${WASI_SDK_SRC}/bin"   "${WASI_DIR}/bin"
    cp -a "${WASI_SDK_SRC}/lib"   "${WASI_DIR}/lib"
    if [ -d "${WASI_SDK_SRC}/share" ]; then
        cp -a "${WASI_SDK_SRC}/share" "${WASI_DIR}/share"
    else
        mkdir -p "${WASI_DIR}/share"
    fi
    [ -f "${WASI_SDK_SRC}/VERSION" ] && cp "${WASI_SDK_SRC}/VERSION" "${WASI_DIR}/VERSION"

    ok "wasi-sdk ${WASI_SDK_TAG} installed (clang + sysroot)"

    # ---- 2. wasix-libc sysroot overlay ----
    step "Downloading ${WASIX_URL}"
    if command -v curl &>/dev/null; then
        curl -fL --retry 3 --progress-bar -o "${WASI_TMP}/wasix-sysroot.tar.gz" "${WASIX_URL}"
    else
        wget --show-progress -O "${WASI_TMP}/wasix-sysroot.tar.gz" "${WASIX_URL}"
    fi

    step "Extracting WASIX sysroot"
    mkdir -p "${WASI_TMP}/wasix-extract"
    tar -xzf "${WASI_TMP}/wasix-sysroot.tar.gz" -C "${WASI_TMP}/wasix-extract"

    # Tarball layout varies between releases: find the nearest dir containing
    # both include/ and lib/.
    WASIX_SRC=""
    while IFS= read -r d; do
        if [ -d "$d/include" ] && [ -d "$d/lib" ]; then
            WASIX_SRC="$d"
            break
        fi
    done < <(find "${WASI_TMP}/wasix-extract" -type d | sort)

    if [ -z "${WASIX_SRC}" ]; then
        warn "WASIX sysroot tarball layout unrecognised; contents:"
        find "${WASI_TMP}/wasix-extract" -maxdepth 3 -mindepth 1 | sed 's/^/    /' || true
        fail "WASIX sysroot tarball missing include/ or lib/"
    fi

    mkdir -p "${WASI_DIR}/share/wasix-sysroot"
    cp -a "${WASIX_SRC}/include" "${WASI_DIR}/share/wasix-sysroot/include"
    cp -a "${WASIX_SRC}/lib"     "${WASI_DIR}/share/wasix-sysroot/lib"
    [ -d "${WASIX_SRC}/share" ] && cp -a "${WASIX_SRC}/share" "${WASI_DIR}/share/wasix-sysroot/share"
    [ -f "${WASIX_SRC}/clang-wasm.cmake_toolchain" ] && \
        cp "${WASIX_SRC}/clang-wasm.cmake_toolchain" "${WASI_DIR}/share/cmake/wasix-toolchain.cmake" 2>/dev/null || true

    ok "WASIX sysroot ${WASIX_TAG} layered into ${WASI_DIR}/share/wasix-sysroot"

    # ---- 3. Convenience top-level layout: include/ symlink ----
    if [ -d "${WASI_DIR}/share/wasi-sysroot/include" ]; then
        ln -sfn share/wasi-sysroot/include "${WASI_DIR}/include"
    fi

    # ---- 4. Add `wasix` shortcut wrappers that target the WASIX sysroot ----
    cat > "${WASI_DIR}/bin/wasix-clang" <<EOF
#!/usr/bin/env bash
exec "${WASI_DIR}/bin/clang" --target=wasm32-wasi --sysroot="${WASI_DIR}/share/wasix-sysroot" "\$@"
EOF
    cat > "${WASI_DIR}/bin/wasix-clang++" <<EOF
#!/usr/bin/env bash
exec "${WASI_DIR}/bin/clang++" --target=wasm32-wasi --sysroot="${WASI_DIR}/share/wasix-sysroot" "\$@"
EOF
    chmod +x "${WASI_DIR}/bin/wasix-clang" "${WASI_DIR}/bin/wasix-clang++"

    rm -rf "${WASI_TMP}"
    trap - EXIT

    echo "${WASI_STAMP_CONTENT}" > "${WASI_STAMP}"
    ok "WASM toolchain ready at ${WASI_DIR}"
    ok "  Default WASI:  ${WASI_DIR}/bin/clang foo.c -o foo.wasm"
    ok "  WASIX target:  ${WASI_DIR}/bin/wasix-clang foo.c -o foo.wasm"
fi
