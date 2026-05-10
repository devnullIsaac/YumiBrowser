#!/usr/bin/env bash
# Always writes release/yumibrowser.desktop. If the user did NOT run
# install_local.sh, optionally offers to install the desktop entry + icons
# to ~/.local/share standalone.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

LOCAL_SHARE_DIR="${HOME}/.local/share"
LOCAL_APPS_DIR="${LOCAL_SHARE_DIR}/applications"
INSTALL_STAMP="${BUILD_DIR}/.installed_local"

step "Creating desktop entry in release/"

DESKTOP_FILE="${RELEASE_DIR}/yumibrowser.desktop"
RELEASE_ABS="$(cd "${RELEASE_DIR}" && pwd)"

cat > "${DESKTOP_FILE}" <<DESKEOF
[Desktop Entry]
Type=Application
Name=Yumi Browser
Comment=WebAssembly browser runtime with GPU acceleration
Exec=${RELEASE_ABS}/${BINARY_NAME} %f
Icon=yumibrowser
Terminal=false
Categories=Development;WebBrowser;
StartupWMClass=yumibrowser
MimeType=application/wasm;
DESKEOF

ok "Created ${DESKTOP_FILE}"

# If install_local.sh ran, it already handled the desktop entry — stop here.
if [ -f "${INSTALL_STAMP}" ]; then
    exit 0
fi

LOCAL_DESKTOP="${LOCAL_APPS_DIR}/yumibrowser.desktop"
if [ -f "${LOCAL_DESKTOP}" ]; then
    warn "yumibrowser.desktop already installed at ${LOCAL_DESKTOP}"
    exit 0
fi

echo ""
echo -en "${CYAN}Install desktop entry + icons to ~/.local/share? [y/N]: ${NC}"
read -r desktop_answer
case "$desktop_answer" in
    [yY]|[yY][eE][sS])
        mkdir -p "${LOCAL_APPS_DIR}"
        cp "${DESKTOP_FILE}" "${LOCAL_DESKTOP}"
        ok "Installed to ${LOCAL_DESKTOP}"

        install_icons "${LOCAL_SHARE_DIR}"

        if command -v update-desktop-database &>/dev/null; then
            update-desktop-database "${LOCAL_APPS_DIR}" 2>/dev/null || true
            ok "Desktop database updated"
        else
            warn "update-desktop-database not found — desktop entry may not appear until next login"
        fi
        ;;
    *)
        warn "Skipped desktop entry installation"
        echo "  You can install it later with:"
        echo "    cp ${DESKTOP_FILE} ${LOCAL_APPS_DIR}/"
        echo "    update-desktop-database ${LOCAL_APPS_DIR}"
        ;;
esac
