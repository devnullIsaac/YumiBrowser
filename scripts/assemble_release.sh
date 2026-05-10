#!/usr/bin/env bash
# Stages the compiled binary, all required shared libraries, Slang standard
# modules, dashboard wasm, icons, and patches rpath into RELEASE_DIR.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

step "Assembling release folder"

# Re-detect Dawn lib dir
if [ -d "${DAWN_INSTALL}/lib64" ]; then
    DAWN_LIB="${DAWN_INSTALL}/lib64"
else
    DAWN_LIB="${DAWN_INSTALL}/lib"
fi

rm -rf "${RELEASE_DIR}"
mkdir -p "${RELEASE_DIR}"

# ---- Copy main binary ----
FOUND_BIN="${BUILD_DIR}/${BINARY_NAME}"
if [ ! -f "${FOUND_BIN}" ]; then
    FOUND_BIN="$(find "${BUILD_DIR}" -name "${BINARY_NAME}" -type f -executable | head -1)"
fi
if [ -z "${FOUND_BIN}" ] || [ ! -f "${FOUND_BIN}" ]; then
    fail "Could not find compiled binary '${BINARY_NAME}' in ${BUILD_DIR}"
fi
cp "${FOUND_BIN}" "${RELEASE_DIR}/${BINARY_NAME}"
chmod +x "${RELEASE_DIR}/${BINARY_NAME}"
ok "Copied ${BINARY_NAME}"

# ---- Copy any contrib tools that happen to exist (best-effort) ----
for tool in "${CONTRIB_FONT_PAINT}" "${CONTRIB_SHADER_BIND}" "${CONTRIB_SHADER_PREVIEW}" "${CONTRIB_SVG_TO_SLANG}"; do
    found="${BUILD_DIR}/${tool}"
    if [ ! -f "${found}" ]; then
        found="$(find "${BUILD_DIR}" -name "${tool}" -type f -executable 2>/dev/null | head -1)"
    fi
    if [ -n "${found}" ] && [ -f "${found}" ]; then
        cp "${found}" "${RELEASE_DIR}/${tool}"
        chmod +x "${RELEASE_DIR}/${tool}"
        ok "Copied ${tool}"
    fi
done

# ---- Strip in release mode ----
if [ "$RELEASE_MODE" = "true" ] && command -v strip &>/dev/null; then
    strip "${RELEASE_DIR}/${BINARY_NAME}"
    ok "Stripped ${BINARY_NAME}"
    for tool in "${CONTRIB_FONT_PAINT}" "${CONTRIB_SHADER_BIND}" "${CONTRIB_SHADER_PREVIEW}" "${CONTRIB_SVG_TO_SLANG}"; do
        if [ -f "${RELEASE_DIR}/${tool}" ]; then
            strip "${RELEASE_DIR}/${tool}"
            ok "Stripped ${tool}"
        fi
    done
fi

# ---- Collect shared libs ----
collect_so "${SDL_BUILD}"             "libSDL3.so*"
collect_so "${SDL_INSTALL}/lib"       "libSDL3.so*"
ok "Collected SDL3"

collect_so "${FREETYPE_INSTALL}/lib"  "libfreetype.so*"
ok "Collected FreeType"

collect_so "${OPENSSL_INSTALL}/lib"   "libcrypto.so*"
collect_so "${OPENSSL_INSTALL}/lib"   "libssl.so*"
ok "Collected OpenSSL"

if [ -d "${OPENSSL_INSTALL}/lib/ossl-modules" ]; then
    mkdir -p "${RELEASE_DIR}/ossl-modules"
    for f in "${OPENSSL_INSTALL}/lib/ossl-modules/"*; do
        [ -f "$f" ] || continue
        cp "$f" "${RELEASE_DIR}/ossl-modules/"
    done
    ok "Collected oqs-provider modules"
fi

collect_so "${ICU_INSTALL}/lib"   "libicuuc.so*"
collect_so "${ICU_INSTALL}/lib"   "libicui18n.so*"
collect_so "${ICU_INSTALL}/lib"   "libicudata.so*"
collect_so "${ICU_INSTALL}/lib64" "libicuuc.so*"
collect_so "${ICU_INSTALL}/lib64" "libicui18n.so*"
collect_so "${ICU_INSTALL}/lib64" "libicudata.so*"
ok "Collected ICU"

collect_so "${HARFBUZZ_INSTALL}/lib" "libharfbuzz.so*"
ok "Collected HarfBuzz"

collect_so "${FFMPEG_INSTALL}/lib" "libavcodec.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libavformat.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libavutil.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libswresample.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libswscale.so*"
collect_so "${FFMPEG_INSTALL}/lib" "libavfilter.so*"
ok "Collected FFmpeg"

collect_so "${LIBRAW_INSTALL}/lib" "libraw.so*"
collect_so "${LIBRAW_INSTALL}/lib" "libraw_r.so*"
ok "Collected LibRaw"

collect_so "${BZIP2_INSTALL}/lib"   "libbz2.so*"
collect_so "${BZIP2_INSTALL}/lib64" "libbz2.so*"
# bzip2 1.1.0 produces soname libbz2.so.1; older consumers want libbz2.so.1.0.
if [ -f "${RELEASE_DIR}/libbz2.so.1" ] && [ ! -e "${RELEASE_DIR}/libbz2.so.1.0" ]; then
    ln -sf libbz2.so.1 "${RELEASE_DIR}/libbz2.so.1.0"
fi
ok "Collected bzip2"

collect_so "${LIBJPEG_INSTALL}/lib"   "libjpeg.so*"
collect_so "${LIBJPEG_INSTALL}/lib64" "libjpeg.so*"
collect_so "${LIBJPEG_INSTALL}/lib"   "libturbojpeg.so*"
collect_so "${LIBJPEG_INSTALL}/lib64" "libturbojpeg.so*"
ok "Collected libjpeg-turbo"

collect_so "${WASMER_INSTALL}/lib" "libwasmer.so*"
ok "Collected Wasmer"

collect_so "${DAWN_LIB}" "libwebgpu_dawn.so*"
collect_so "${DAWN_LIB}" "libdawn*.so*"
ok "Collected Dawn"

collect_so "${DUCKDB_DIR}/build/release/src" "libduckdb.so*"
ok "Collected DuckDB"

# Slang — preserve symlink chain exactly (libslang.so -> libslang-compiler.so -> ...)
for f in "${SLANG_INSTALL}/lib/"libslang*.so* "${SLANG_INSTALL}/lib/"libgfx*.so*; do
    [ -e "$f" ] || [ -L "$f" ] || continue
    cp -a "$f" "${RELEASE_DIR}/"
done
ok "Collected Slang"

# ---- Slang standard modules ----
step "Collecting Slang standard modules"
SLANG_STDMOD_COLLECTED=false
for d in "${SLANG_INSTALL}/lib/slang-standard-module-"*; do
    if [ -d "$d" ]; then
        SLANG_STDMOD_DIRNAME="$(basename "$d")"
        rm -rf "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
        cp -r "$d" "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
        SLANG_STDMOD_COLLECTED=true
        ok "Collected ${SLANG_STDMOD_DIRNAME} into release/"
        break
    fi
done

if ! $SLANG_STDMOD_COLLECTED; then
    SLANG_MODULE_FILES=("${SLANG_INSTALL}/lib/"*.slang-module)
    if [ -e "${SLANG_MODULE_FILES[0]}" ]; then
        for f in "${SLANG_MODULE_FILES[@]}"; do
            cp "$f" "${RELEASE_DIR}/"
        done
        SLANG_STDMOD_COLLECTED=true
        ok "Collected loose .slang-module files into release/"
    fi
fi

if ! $SLANG_STDMOD_COLLECTED; then
    for search_dir in "${SLANG_INSTALL}/share/slang" "${SLANG_INSTALL}/share"; do
        if [ -d "$search_dir" ]; then
            for d in "${search_dir}/slang-standard-module-"*; do
                if [ -d "$d" ]; then
                    SLANG_STDMOD_DIRNAME="$(basename "$d")"
                    rm -rf "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
                    cp -r "$d" "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
                    SLANG_STDMOD_COLLECTED=true
                    ok "Collected ${SLANG_STDMOD_DIRNAME} from ${search_dir} into release/"
                    break 2
                fi
            done
        fi
    done
fi

if ! $SLANG_STDMOD_COLLECTED; then
    FOUND_STDMOD="$(find "${SLANG_DIR}" -type d -name 'slang-standard-module-*' 2>/dev/null | head -1)"
    if [ -n "${FOUND_STDMOD}" ] && [ -d "${FOUND_STDMOD}" ]; then
        SLANG_STDMOD_DIRNAME="$(basename "${FOUND_STDMOD}")"
        rm -rf "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
        cp -r "${FOUND_STDMOD}" "${RELEASE_DIR}/${SLANG_STDMOD_DIRNAME}"
        SLANG_STDMOD_COLLECTED=true
        ok "Collected ${SLANG_STDMOD_DIRNAME} from build tree into release/"
    fi
fi

if ! $SLANG_STDMOD_COLLECTED; then
    warn "Slang standard modules not found — shader-bind may fail with 'module load failed'"
fi

# ---- mato_dashboard WASM ----
step "Building mato_dashboard WASM"
MATO_BUILD_SH="${PROJECT_ROOT}/webapps/mato_dashboard/build.sh"

if [ -f "${MATO_BUILD_SH}" ]; then
    chmod +x "${MATO_BUILD_SH}"
    bash "${MATO_BUILD_SH}"
    if [ -f "${RELEASE_DIR}/dashboard/mato_dashboard.wasm" ]; then
        ok "mato_dashboard.wasm built (release/dashboard/)"
    else
        fail "mato_dashboard build script ran but mato_dashboard.wasm not found in ${RELEASE_DIR}/dashboard/"
    fi
else
    warn "${MATO_BUILD_SH} not found — skipping dashboard WASM build"
fi

# ---- Icons ----
step "Collecting icons into release/"
if [ -d "${ICON_DIR}" ]; then
    mkdir -p "${RELEASE_DIR}/icon"
    for size in 512 256 128 64 32; do
        if [ -f "${ICON_DIR}/icon${size}.png" ]; then
            cp "${ICON_DIR}/icon${size}.png" "${RELEASE_DIR}/icon/"
            ok "Collected icon${size}.png"
        fi
    done
else
    warn "Icon directory ${ICON_DIR} not found — skipping icon collection"
fi

# ---- rpath patching ----
if command -v patchelf &>/dev/null; then
    step "Patching rpath on binaries"
    patchelf --set-rpath '$ORIGIN' "${RELEASE_DIR}/${BINARY_NAME}"
    ok "Patched ${BINARY_NAME} rpath to \$ORIGIN"

    for tool in "${CONTRIB_FONT_PAINT}" "${CONTRIB_SHADER_BIND}" "${CONTRIB_SHADER_PREVIEW}" "${CONTRIB_SVG_TO_SLANG}"; do
        if [ -f "${RELEASE_DIR}/${tool}" ]; then
            patchelf --set-rpath '$ORIGIN' "${RELEASE_DIR}/${tool}"
            ok "Patched ${tool} rpath to \$ORIGIN"
        fi
    done

    step "Patching rpath on collected shared libraries"
    for lib in "${RELEASE_DIR}"/*.so*; do
        [ -f "$lib" ] || continue
        [ -L "$lib" ] && continue
        patchelf --set-rpath '$ORIGIN' "$lib" 2>/dev/null || true
    done
    ok "All shared libraries patched to \$ORIGIN"
else
    echo ""
    echo -e "${YELLOW}WARNING: patchelf is not installed.${NC}"
    echo "Binaries will not have \$ORIGIN rpath and may fail to find shared"
    echo "libraries at runtime unless you set LD_LIBRARY_PATH manually."
    echo ""
    echo "Please install patchelf:"
    echo "  Debian/Ubuntu:  sudo apt install patchelf"
    echo "  Fedora:         sudo dnf install patchelf"
    echo "  Arch:           sudo pacman -S patchelf"
    echo ""
    warn "Continuing without rpath patching"
fi
