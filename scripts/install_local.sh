#!/usr/bin/env bash
# Optional installation to ~/.local/{bin,lib,share}. Interactive — prompts
# the user. Sets YUMI_INSTALLED_LOCAL=1 in a stamp file under ${BUILD_DIR}/
# so desktop_entry.sh can know whether to offer the standalone option.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

LOCAL_BIN_DIR="${HOME}/.local/bin"
LOCAL_LIB_DIR="${HOME}/.local/lib"
LOCAL_SHARE_DIR="${HOME}/.local/share"
LOCAL_APPS_DIR="${LOCAL_SHARE_DIR}/applications"
INSTALL_STAMP="${BUILD_DIR}/.installed_local"

rm -f "${INSTALL_STAMP}"

echo ""
echo -en "${CYAN}Install yumibrowser to ~/.local (bin + lib + icons + desktop entry)? [y/N]: ${NC}"
read -r install_answer
case "$install_answer" in
    [yY]|[yY][eE][sS]) ;;
    *)
        warn "Skipped local installation"
        exit 0
        ;;
esac

step "Installing to ~/.local"
mkdir -p "${LOCAL_BIN_DIR}" "${LOCAL_LIB_DIR}" "${LOCAL_APPS_DIR}"

# ---- Binary + tools ----
cp "${RELEASE_DIR}/${BINARY_NAME}" "${LOCAL_BIN_DIR}/${BINARY_NAME}"
chmod +x "${LOCAL_BIN_DIR}/${BINARY_NAME}"
ok "Installed ${BINARY_NAME} to ${LOCAL_BIN_DIR}"

for tool in "${CONTRIB_FONT_PAINT}" "${CONTRIB_SHADER_BIND}" "${CONTRIB_SHADER_PREVIEW}" "${CONTRIB_SVG_TO_SLANG}"; do
    if [ -f "${RELEASE_DIR}/${tool}" ]; then
        cp "${RELEASE_DIR}/${tool}" "${LOCAL_BIN_DIR}/${tool}"
        chmod +x "${LOCAL_BIN_DIR}/${tool}"
        ok "Installed ${tool} to ${LOCAL_BIN_DIR}"
    fi
done

# ---- Shared libraries (preserve symlinks) ----
for f in "${RELEASE_DIR}"/*.so* ; do
    [ -e "$f" ] || [ -L "$f" ] || continue
    cp -a "$f" "${LOCAL_LIB_DIR}/"
done
ok "Installed shared libraries to ${LOCAL_LIB_DIR}"

# ---- Slang standard modules next to libslang.so ----
step "Installing Slang standard modules to ${LOCAL_LIB_DIR}"
SLANG_LOCAL_INSTALLED=false
for d in "${RELEASE_DIR}/slang-standard-module-"*; do
    if [ -d "$d" ]; then
        LOCAL_STDMOD_DIR="${LOCAL_LIB_DIR}/$(basename "$d")"
        rm -rf "${LOCAL_STDMOD_DIR}"
        cp -r "$d" "${LOCAL_STDMOD_DIR}"
        SLANG_LOCAL_INSTALLED=true
        ok "Installed $(basename "$d") to ${LOCAL_LIB_DIR}"
        break
    fi
done
for f in "${RELEASE_DIR}"/*.slang-module; do
    if [ -f "$f" ]; then
        cp "$f" "${LOCAL_LIB_DIR}/"
        SLANG_LOCAL_INSTALLED=true
    fi
done
if $SLANG_LOCAL_INSTALLED; then
    ok "Slang standard modules installed next to libslang.so"
else
    warn "No Slang standard modules found to install"
fi

# ---- Icons ----
install_icons "${LOCAL_SHARE_DIR}"

# ---- WASM dashboard ----
YUMI_DATA_DIR="${LOCAL_SHARE_DIR}/${APP_ID}"
step "Installing mato_dashboard to ${YUMI_DATA_DIR}"
if [ -d "${RELEASE_DIR}/dashboard" ]; then
    mkdir -p "${YUMI_DATA_DIR}/dashboard"
    rm -f "${YUMI_DATA_DIR}/dashboard/"*.wasm 2>/dev/null || true
    rm -rf "${YUMI_DATA_DIR}/webapps" 2>/dev/null || true
    cp -a "${RELEASE_DIR}/dashboard/mato_dashboard.wasm" "${YUMI_DATA_DIR}/dashboard/"
    ok "Installed mato_dashboard.wasm to ${YUMI_DATA_DIR}/dashboard/"
fi

# ---- rpath ----
if command -v patchelf &>/dev/null; then
    patchelf --set-rpath "${LOCAL_LIB_DIR}" "${LOCAL_BIN_DIR}/${BINARY_NAME}"
    ok "Patched ${BINARY_NAME} rpath to ${LOCAL_LIB_DIR}"
    for tool in "${CONTRIB_FONT_PAINT}" "${CONTRIB_SHADER_BIND}" "${CONTRIB_SHADER_PREVIEW}" "${CONTRIB_SVG_TO_SLANG}"; do
        if [ -f "${LOCAL_BIN_DIR}/${tool}" ]; then
            patchelf --set-rpath "${LOCAL_LIB_DIR}" "${LOCAL_BIN_DIR}/${tool}"
            ok "Patched ${tool} rpath to ${LOCAL_LIB_DIR}"
        fi
    done
    for lib in "${LOCAL_LIB_DIR}"/*.so*; do
        [ -f "$lib" ] || continue
        [ -L "$lib" ] && continue
        patchelf --set-rpath "${LOCAL_LIB_DIR}" "$lib" 2>/dev/null || true
    done
    ok "Patched shared library rpaths to ${LOCAL_LIB_DIR}"
else
    warn "patchelf not found — set LD_LIBRARY_PATH=${LOCAL_LIB_DIR}"
fi

# ---- Desktop entry ----
LOCAL_DESKTOP="${LOCAL_APPS_DIR}/yumibrowser.desktop"
cat > "${LOCAL_DESKTOP}" <<DESKEOF
[Desktop Entry]
Type=Application
Name=Yumi Browser
Comment=WebAssembly browser runtime with GPU acceleration
Exec=${LOCAL_BIN_DIR}/${BINARY_NAME} %f
Icon=yumibrowser
Terminal=false
Categories=Development;WebBrowser;
StartupWMClass=yumibrowser
MimeType=application/wasm;
DESKEOF
ok "Installed desktop entry to ${LOCAL_DESKTOP}"

if command -v update-desktop-database &>/dev/null; then
    update-desktop-database "${LOCAL_APPS_DIR}" 2>/dev/null || true
    ok "Desktop database updated"
else
    warn "update-desktop-database not found — desktop entry may not appear until next login"
fi

# Stamp file so desktop_entry.sh skips the standalone-prompt path
mkdir -p "${BUILD_DIR}"
echo "1" > "${INSTALL_STAMP}"

# ---- PATH reminder ----
if ! echo "$PATH" | tr ':' '\n' | grep -qx "${LOCAL_BIN_DIR}"; then
    echo ""
    warn "${LOCAL_BIN_DIR} is not in your PATH"
    echo "  Add it to your shell profile:"
    echo "    echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.bashrc"
    echo "  Then restart your shell or run:  source ~/.bashrc"
fi
