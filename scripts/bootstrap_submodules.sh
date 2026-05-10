#!/usr/bin/env bash
# Initialise + pin git submodules. Handles Dawn (no recurse), Wasmer (recurse),
# Slang (recurse for external/*), and liboqs (nested in oqs-provider).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# Fetch and check out the pinned tag/branch for a submodule. Idempotent.
pin_submodule() {
    local dir="$1"
    local ref="$2"
    local full_path="${PROJECT_ROOT}/${dir}"

    [ -z "$ref" ] && return 0
    [ ! -d "${full_path}/.git" ] && [ ! -f "${full_path}/.git" ] && return 0

    # Already on the requested tag?
    local current_tag
    current_tag="$(git -C "$full_path" describe --tags --exact-match 2>/dev/null || true)"
    if [ "$current_tag" = "$ref" ]; then
        return 0
    fi
    # Already on the requested branch tip?
    if git -C "$full_path" show-ref --verify --quiet "refs/heads/${ref}" 2>/dev/null; then
        local head_ref
        head_ref="$(git -C "$full_path" symbolic-ref --short -q HEAD || true)"
        if [ "$head_ref" = "$ref" ]; then
            return 0
        fi
    fi

    step "Pinning ${dir} -> ${ref}"
    local fetched=false
    if git -C "$full_path" fetch --depth 1 origin "refs/tags/${ref}:refs/tags/${ref}" 2>/dev/null; then
        fetched=true
        git -C "$full_path" -c advice.detachedHead=false checkout "refs/tags/${ref}"
        return 0
    fi
    if git -C "$full_path" fetch --depth 1 origin "refs/heads/${ref}:refs/remotes/origin/${ref}" 2>/dev/null; then
        fetched=true
        git -C "$full_path" -c advice.detachedHead=false checkout --detach "refs/remotes/origin/${ref}"
        return 0
    fi
    if ! $fetched; then
        warn "Could not shallow-fetch ${ref} for ${dir}; falling back to full fetch"
        git -C "$full_path" fetch --tags --force origin || true
        git -C "$full_path" fetch --force origin "${ref}" || true
        if git -C "$full_path" rev-parse --verify --quiet "refs/tags/${ref}" >/dev/null; then
            git -C "$full_path" -c advice.detachedHead=false checkout "refs/tags/${ref}"
        elif git -C "$full_path" rev-parse --verify --quiet "refs/remotes/origin/${ref}" >/dev/null; then
            git -C "$full_path" -c advice.detachedHead=false checkout --detach "refs/remotes/origin/${ref}"
        else
            fail "Could not resolve ref '${ref}' in ${dir}"
        fi
    fi
}

step "Initialising submodules"

if [ ! -d "${PROJECT_ROOT}/.git" ]; then
    warn "Not a git repo — skipping submodule init"
else
    for dir in "${SUBMODULE_DIRS[@]}"; do
        full_path="${PROJECT_ROOT}/${dir}"
        if [ ! -d "$full_path" ] || [ -z "$(ls -A "$full_path" 2>/dev/null)" ]; then
            step "Cloning submodule: ${dir}"
            git -C "${PROJECT_ROOT}" submodule update --init --depth 1 -- "$dir"
        fi
        pin_submodule "$dir" "${DEP_TAGS[$dir]:-}"
    done

    # Wasmer ships nested submodules (lib/napi, …) referenced from its
    # workspace Cargo.toml. Initialise them recursively or `cargo build` will
    # fail with "failed to read lib/napi/Cargo.toml".
    if [ -d "${PROJECT_ROOT}/deps/wasmer/.git" ] || [ -f "${PROJECT_ROOT}/deps/wasmer/.git" ]; then
        step "Initialising Wasmer nested submodules"
        git -C "${PROJECT_ROOT}/deps/wasmer" submodule update --init --recursive --depth 1
    fi

    # Dawn — init only, never recurse (handled separately because Dawn fetches
    # its own deps via DAWN_FETCH_DEPENDENCIES at cmake time).
    if [ ! -d "${PROJECT_ROOT}/${DAWN_SUBMODULE}" ] || \
       [ -z "$(ls -A "${PROJECT_ROOT}/${DAWN_SUBMODULE}" 2>/dev/null)" ]; then
        step "Cloning Dawn submodule (no recurse)"
        git -C "${PROJECT_ROOT}" submodule update --init --depth 1 -- "${DAWN_SUBMODULE}"
    fi
    pin_submodule "${DAWN_SUBMODULE}" "${DEP_TAGS[$DAWN_SUBMODULE]:-}"

    # Slang — top-level shallow init, then recurse into external/* which holds
    # bundled deps (unordered_dense, miniz, lz4, cmark, vulkan-headers,
    # spirv-headers, spirv-tools, glslang, slang-rhi, …). Without recursion
    # CMake fails with "external/<x> does not contain a CMakeLists.txt".
    if [ ! -d "${PROJECT_ROOT}/${SLANG_SUBMODULE}" ] || \
       [ -z "$(ls -A "${PROJECT_ROOT}/${SLANG_SUBMODULE}" 2>/dev/null)" ]; then
        step "Cloning Slang submodule"
        git -C "${PROJECT_ROOT}" submodule update --init --depth 1 -- "${SLANG_SUBMODULE}"
    fi
    pin_submodule "${SLANG_SUBMODULE}" "${DEP_TAGS[$SLANG_SUBMODULE]:-}"
    if [ -d "${PROJECT_ROOT}/${SLANG_SUBMODULE}/.git" ] || [ -f "${PROJECT_ROOT}/${SLANG_SUBMODULE}/.git" ]; then
        step "Initialising Slang nested submodules"
        git -C "${PROJECT_ROOT}/${SLANG_SUBMODULE}" submodule update --init --recursive --depth 1
    fi

    # Pin liboqs (nested submodule inside oqs-provider).
    if [ -d "${LIBOQS_SRC}/.git" ] || [ -f "${LIBOQS_SRC}/.git" ]; then
        pin_submodule "deps/oqs-provider/liboqs" "${LIBOQS_TAG}"
    fi
fi

# ---------- Verify submodule dirs ----------
step "Verifying submodule directories"

all_ok=true
for dir in "${SUBMODULE_DIRS[@]}" "${DAWN_SUBMODULE}" "${SLANG_SUBMODULE}"; do
    full_path="${PROJECT_ROOT}/${dir}"
    if [ ! -d "$full_path" ]; then
        echo -e "  ${RED}✗  ${dir} — not found${NC}"
        all_ok=false
    elif [ -z "$(ls -A "$full_path" 2>/dev/null)" ]; then
        echo -e "  ${RED}✗  ${dir} — directory empty${NC}"
        all_ok=false
    else
        echo -e "  ${GREEN}✓  ${dir}${NC}"
    fi
done

if ! $all_ok; then
    echo ""
    fail "One or more submodules failed to clone. Check your .gitmodules and network connection."
fi

ok "All submodule directories present and non-empty"
