#!/usr/bin/env bash
# YumiBrowser top-level build orchestrator. The actual logic lives in
# scripts/. Each scripts/deps/<name>.sh is self-contained and can be
# re-run individually for debugging:
#
#   bash scripts/deps/slang.sh            # rebuild just Slang
#   bash scripts/meson_build.sh           # reconfigure + recompile
#   bash scripts/assemble_release.sh      # rebuild release/ from build/
#
# Flags:
#   --release          build with -O3 + meson release_build=true
#   --flatpak          wipe prebuilt dep artefacts before building (so deps
#                      are rebuilt against the SDK container's glibc); also
#                      wipes the project build/ and webapps/_build trees
#   --rebuild          wipe ALL prebuilt dep artefacts then rebuild (keeps
#                      project build/ — use with --flatpak for full clean)
#   --rebuild=a,b,c    wipe only the listed deps (logical names matching
#                      scripts/deps/<name>.sh) and rebuild them. Example:
#                          ./build.sh --rebuild=slang,dawn
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/scripts" && pwd)"

# ---------- Parse flags ----------
export YUMI_RELEASE_MODE=false
export YUMI_FLATPAK_MODE=false
REBUILD_REQUESTED=false
REBUILD_LIST=""
for arg in "$@"; do
    case "$arg" in
        --release) export YUMI_RELEASE_MODE=true ;;
        --flatpak) export YUMI_FLATPAK_MODE=true ;;
        --rebuild) REBUILD_REQUESTED=true; REBUILD_LIST="" ;;
        --rebuild=*) REBUILD_REQUESTED=true; REBUILD_LIST="${arg#--rebuild=}" ;;
        -h|--help)
            sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "Unknown flag: $arg" >&2; exit 1 ;;
    esac
done

source "${SCRIPTS_DIR}/common.sh"

# ---------- Per-dependency builds (order matters) ----------
# bzip2 + libjpeg-turbo are pre-reqs for FreeType / FFmpeg / LibRaw.
# OpenSSL must precede liboqs/oqs-provider (handled inside openssl.sh).
# FreeType + ICU must precede HarfBuzz.
DEP_SCRIPTS=(
    sdl
    bzip2
    libjpeg_turbo
    freetype
    openssl
    icu
    harfbuzz
    ffmpeg
    libraw
    wasmer
    slang
    dawn
    duckdb
)

# ---------- --rebuild: wipe selected (or all) prebuilt deps ----------
if [ "$REBUILD_REQUESTED" = "true" ]; then
    if [ -z "$REBUILD_LIST" ]; then
        REBUILD_TARGETS=("${DEP_SCRIPTS[@]}")
        step "Rebuild mode: wiping ALL dependency artifacts"
    else
        IFS=',' read -r -a REBUILD_TARGETS <<< "$REBUILD_LIST"
        step "Rebuild mode: wiping ${#REBUILD_TARGETS[@]} dep(s): ${REBUILD_TARGETS[*]}"
        # Validate each target up-front so we fail fast on typos.
        for t in "${REBUILD_TARGETS[@]}"; do
            if [ -z "${DEP_DIR_NAMES[$t]:-}" ]; then
                fail "Unknown dep '$t' for --rebuild. Valid: ${DEP_SCRIPTS[*]}"
            fi
        done
    fi
    for t in "${REBUILD_TARGETS[@]}"; do
        wipe_dep "$t"
    done
    ok "Rebuild wipe complete"
fi

# ---------- Flatpak: wipe prebuilt dep artefacts + project build trees ----------
if [ "$FLATPAK_MODE" = "true" ]; then
    step "Flatpak mode: clearing prebuilt dependency artifacts"
    for t in "${DEP_SCRIPTS[@]}"; do
        wipe_dep "$t"
    done
    rm -rf "${BUILD_DIR}"
    rm -rf "${PROJECT_ROOT}/webapps/_build"
    ok "Cleared all prebuilt artifacts"
fi

# ---------- Submodules + WASI toolchain ----------
bash "${SCRIPTS_DIR}/bootstrap_submodules.sh"
bash "${SCRIPTS_DIR}/install_wasi_sdk.sh"

for dep in "${DEP_SCRIPTS[@]}"; do
    bash "${SCRIPTS_DIR}/deps/${dep}.sh"
done

# ---------- Project compile + release staging ----------
bash "${SCRIPTS_DIR}/meson_build.sh"
bash "${SCRIPTS_DIR}/assemble_release.sh"
bash "${SCRIPTS_DIR}/install_local.sh"
bash "${SCRIPTS_DIR}/desktop_entry.sh"

INSTALLED_LOCAL=false
[ -f "${BUILD_DIR}/.installed_local" ] && INSTALLED_LOCAL=true

# ---------- Summary ----------
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Build complete!                       ${NC}"
if [ "$RELEASE_MODE" = "true" ]; then
    echo -e "${GREEN}  Mode: RELEASE (-O3)                  ${NC}"
else
    echo -e "${GREEN}  Mode: DEBUG                           ${NC}"
fi
echo -e "${GREEN}  Self-contained release in: release/   ${NC}"
if $INSTALLED_LOCAL; then
    echo -e "${GREEN}  Installed to ~/.local/bin + lib       ${NC}"
    echo -e "${GREEN}  Run:  yumibrowser                     ${NC}"
else
    echo -e "${GREEN}  Run:  cd release && ./${BINARY_NAME}  ${NC}"
fi
for tool in "${CONTRIB_FONT_PAINT}" "${CONTRIB_SHADER_BIND}" "${CONTRIB_SHADER_PREVIEW}" "${CONTRIB_SVG_TO_SLANG}"; do
    if [ -f "${RELEASE_DIR}/${tool}" ]; then
        echo -e "${GREEN}  Tool: ./${tool}${NC}"
    fi
done
echo -e "${GREEN}========================================${NC}"
