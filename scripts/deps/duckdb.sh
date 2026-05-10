#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building DuckDB"

DUCKDB_BUILD="${DUCKDB_DIR}/build/release"
DUCKDB_LIBDIR="${DUCKDB_BUILD}/src"

if [ -f "${DUCKDB_LIBDIR}/libduckdb.so" ]; then
    warn "DuckDB already built — skipping (delete ${DUCKDB_BUILD} to rebuild)"
    exit 0
fi

cmake -S "${DUCKDB_DIR}" -B "${DUCKDB_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja

cmake --build "${DUCKDB_BUILD}" -j "${JOBS}"
ok "DuckDB built"
