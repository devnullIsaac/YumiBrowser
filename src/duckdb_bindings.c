/*
 * duckdb_bindings.c - Implementation of the DuckDB WASM bindings: ResultWrap caching chunks via duckdb_fetch_chunk, prepared statements, appenders, table descriptions.
 * Copyright (C) 2026 DevNullIsaac
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "duckdb_bindings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  ResultWrap — owns a duckdb_result + cached data chunks             */
/* ------------------------------------------------------------------ */
/*
 * All query results on the host are wrapped in a ResultWrap that
 * progressively materializes chunks via the non-deprecated
 * `duckdb_fetch_chunk` API.  The wrap serves two purposes:
 *
 *   1. Back the modern chunk/vector API (ddb_fetch_chunk /
 *      ddb_result_get_chunk) without re-querying.
 *   2. Back the convenience cell-style API (ddb_value_*,
 *      ddb_result_row_count) by random-accessing the cached chunks.
 *
 * Chunks are owned by the ResultWrap.  The ChunkWrap handle the guest
 * receives is either an owning reference (to a chunk the wrap just
 * fetched) or a non-owning view (when the guest explicitly asked for a
 * chunk by index via ddb_result_get_chunk).  Destroying a non-owning
 * ChunkWrap is a no-op on the underlying chunk.
 */

typedef struct {
    duckdb_result      result;
    duckdb_data_chunk *chunks;            /* array of fetched chunks */
    idx_t             *chunk_row_offsets; /* cumulative; size = count+1 */
    idx_t              chunk_count;
    idx_t              chunk_cap;
    idx_t              total_rows;        /* sum of chunk sizes fetched so far */
    bool               all_fetched;

    /* When `synthetic` is true, `result` has never been populated by
     * DuckDB (sandbox rejected the call).  `synthetic_error` holds the
     * message surfaced through `ddb_result_error`.  rw_destroy() skips
     * duckdb_destroy_result() in that case. */
    bool               synthetic;
    char              *synthetic_error;
} ResultWrap;

typedef struct {
    duckdb_data_chunk chunk;
    bool              owned; /* false => view into a ResultWrap */
} ChunkWrap;

/* ------------------------------------------------------------------ */
/*  Memory access helpers                                              */
/* ------------------------------------------------------------------ */

static uint8_t *wasm_mem_base(DuckdbBindings *b) {
    if (!b->memory) return NULL;
    return (uint8_t *)wasm_memory_data(b->memory);
}

static size_t wasm_mem_size(DuckdbBindings *b) {
    if (!b->memory) return 0;
    return wasm_memory_data_size(b->memory);
}

static bool mem_read(DuckdbBindings *b, uint32_t ptr, void *dst, size_t len) {
    if ((size_t)ptr + len > wasm_mem_size(b)) return false;
    memcpy(dst, wasm_mem_base(b) + ptr, len);
    return true;
}

static bool mem_write(DuckdbBindings *b, uint32_t ptr, const void *src, size_t len) {
    if ((size_t)ptr + len > wasm_mem_size(b)) return false;
    memcpy(wasm_mem_base(b) + ptr, src, len);
    return true;
}

/* ---- String helpers ---- */

static char *mem_read_cstr(DuckdbBindings *b, uint32_t ptr, uint32_t len) {
    if (len == 0) return strdup("");
    if ((size_t)ptr + len > wasm_mem_size(b)) return strdup("");
    char *s = (char *)malloc(len + 1);
    memcpy(s, wasm_mem_base(b) + ptr, len);
    s[len] = '\0';
    return s;
}

static int32_t mem_write_cstr(DuckdbBindings *b, uint32_t out_ptr,
                               uint32_t out_cap, const char *str) {
    if (!str) return 0;
    uint32_t len = (uint32_t)strlen(str);
    if (out_cap > 0 && out_ptr != 0) {
        uint32_t to_write = len < out_cap ? len : out_cap;
        mem_write(b, out_ptr, str, to_write);
    }
    return (int32_t)len;
}

static int32_t mem_write_blob(DuckdbBindings *b, uint32_t out_ptr,
                               uint32_t out_cap, const void *data,
                               uint32_t len) {
    if (!data || len == 0) return (int32_t)len;
    if (out_cap > 0 && out_ptr != 0) {
        uint32_t to_write = len < out_cap ? len : out_cap;
        mem_write(b, out_ptr, data, to_write);
    }
    return (int32_t)len;
}

/* ------------------------------------------------------------------ */
/*  ResultWrap lifecycle                                               */
/* ------------------------------------------------------------------ */

static ResultWrap *rw_new(void) {
    return (ResultWrap *)calloc(1, sizeof(ResultWrap));
}

static bool rw_grow(ResultWrap *rw) {
    size_t newcap = rw->chunk_cap ? rw->chunk_cap * 2 : 4;
    duckdb_data_chunk *nc = (duckdb_data_chunk *)
        realloc(rw->chunks, newcap * sizeof(*nc));
    idx_t *no = (idx_t *)
        realloc(rw->chunk_row_offsets, (newcap + 1) * sizeof(*no));
    if (!nc || !no) {
        /* realloc on failure: either rw->chunks or the new pointer may
         * be valid; this failure path is best-effort. */
        if (nc) rw->chunks = nc;
        if (no) rw->chunk_row_offsets = no;
        return false;
    }
    if (rw->chunk_count == 0) no[0] = 0;
    rw->chunks = nc;
    rw->chunk_row_offsets = no;
    rw->chunk_cap = newcap;
    return true;
}

/* Fetch one more chunk from the result. Returns true if one was
 * appended, false on end-of-result or allocation failure. */
static bool rw_fetch_one(ResultWrap *rw) {
    if (rw->all_fetched) return false;
    duckdb_data_chunk chunk = duckdb_fetch_chunk(rw->result);
    if (!chunk) { rw->all_fetched = true; return false; }
    if (rw->chunk_count == rw->chunk_cap && !rw_grow(rw)) {
        duckdb_destroy_data_chunk(&chunk);
        rw->all_fetched = true;
        return false;
    }
    idx_t sz = duckdb_data_chunk_get_size(chunk);
    rw->chunks[rw->chunk_count] = chunk;
    rw->chunk_row_offsets[rw->chunk_count + 1] =
        rw->chunk_row_offsets[rw->chunk_count] + sz;
    rw->chunk_count++;
    rw->total_rows += sz;
    return true;
}

static void rw_materialize_all(ResultWrap *rw) {
    while (rw_fetch_one(rw)) {}
}

/* Ensure chunk index `n` exists (materializing if needed). */
static bool rw_ensure_chunk(ResultWrap *rw, idx_t n) {
    while (rw->chunk_count <= n) {
        if (!rw_fetch_one(rw)) break;
    }
    return rw->chunk_count > n;
}

/* Locate the chunk and in-chunk row index for a global row. */
static bool rw_locate(ResultWrap *rw, idx_t row,
                       idx_t *out_ci, idx_t *out_ri) {
    rw_materialize_all(rw);
    if (rw->chunk_count == 0 || row >= rw->total_rows) return false;
    idx_t lo = 0, hi = rw->chunk_count;
    while (lo + 1 < hi) {
        idx_t mid = (lo + hi) / 2;
        if (rw->chunk_row_offsets[mid] <= row) lo = mid;
        else hi = mid;
    }
    *out_ci = lo;
    *out_ri = row - rw->chunk_row_offsets[lo];
    return true;
}

static void rw_destroy(ResultWrap *rw) {
    if (!rw) return;
    for (idx_t i = 0; i < rw->chunk_count; i++) {
        duckdb_destroy_data_chunk(&rw->chunks[i]);
    }
    free(rw->chunks);
    free(rw->chunk_row_offsets);
    if (!rw->synthetic) duckdb_destroy_result(&rw->result);
    free(rw->synthetic_error);
    free(rw);
}

/* ------------------------------------------------------------------ */
/*  Sandbox / disk-quota enforcement                                   */
/* ------------------------------------------------------------------ */
/*
 * Each DuckdbBindings instance carries an optional on-disk byte
 * ceiling supplied by the dashboard.  The ceiling can be updated at
 * any time via duckdb_bindings_set_quota() — the next write statement
 * picks up the new value.
 *
 * Policy:
 *   - FORBIDDEN statement types are rejected unconditionally:
 *       ATTACH, DETACH, LOAD, SET, VARIABLE_SET, TRANSACTION,
 *       EXTENSION, EXPORT, COPY (writes external files).
 *   - WRITER statement types trigger a pre-execution disk-size check:
 *       INSERT, UPDATE, DELETE, CREATE, CREATE_FUNC, ALTER, DROP,
 *       VACUUM, ANALYZE, CALL, MULTI.
 *   - Appenders are treated as writers (checked in create/flush/close).
 *   - READER types (SELECT, EXPLAIN, PRAGMA, PREPARE, EXECUTE) pass.
 *
 * Rejection surfaces through `ddb_result_error()` on the returned
 * result handle (synthetic result) AND through `ddb_last_error()`.
 * Appender-path rejections surface through `ddb_appender_error()` is
 * host-side only — guests should call `ddb_last_error()`.
 */

static uint64_t file_size_bytes(const char *path) {
    if (!path || !*path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

static uint64_t ddb_disk_usage(const DuckdbBindings *b) {
    if (!b->db_path) return 0;
    uint64_t total = file_size_bytes(b->db_path);
    /* DuckDB's WAL lives next to the main file as `<path>.wal`. */
    size_t n = strlen(b->db_path);
    char *wal = (char *)malloc(n + 5);
    if (wal) {
        memcpy(wal, b->db_path, n);
        memcpy(wal + n, ".wal", 5);
        total += file_size_bytes(wal);
        free(wal);
    }
    return total;
}

static void bindings_set_error(DuckdbBindings *b, const char *msg) {
    free(b->last_error);
    b->last_error = msg ? strdup(msg) : NULL;
}

static bool is_forbidden_stmt(duckdb_statement_type t) {
    switch (t) {
    case DUCKDB_STATEMENT_TYPE_ATTACH:
    case DUCKDB_STATEMENT_TYPE_DETACH:
    case DUCKDB_STATEMENT_TYPE_LOAD:
    case DUCKDB_STATEMENT_TYPE_SET:
    case DUCKDB_STATEMENT_TYPE_VARIABLE_SET:
    case DUCKDB_STATEMENT_TYPE_TRANSACTION:
    case DUCKDB_STATEMENT_TYPE_EXTENSION:
    case DUCKDB_STATEMENT_TYPE_EXPORT:
    case DUCKDB_STATEMENT_TYPE_COPY:
        return true;
    default:
        return false;
    }
}

static bool is_writer_stmt(duckdb_statement_type t) {
    switch (t) {
    case DUCKDB_STATEMENT_TYPE_INSERT:
    case DUCKDB_STATEMENT_TYPE_UPDATE:
    case DUCKDB_STATEMENT_TYPE_DELETE:
    case DUCKDB_STATEMENT_TYPE_CREATE:
    case DUCKDB_STATEMENT_TYPE_CREATE_FUNC:
    case DUCKDB_STATEMENT_TYPE_ALTER:
    case DUCKDB_STATEMENT_TYPE_DROP:
    case DUCKDB_STATEMENT_TYPE_VACUUM:
    case DUCKDB_STATEMENT_TYPE_ANALYZE:
    case DUCKDB_STATEMENT_TYPE_CALL:
    case DUCKDB_STATEMENT_TYPE_MULTI:
    case DUCKDB_STATEMENT_TYPE_RELATION:
    case DUCKDB_STATEMENT_TYPE_LOGICAL_PLAN:
        return true;
    default:
        return false;
    }
}

/** Classify a SQL string via duckdb_extract_statements.
 *  Returns true when classification succeeded; out-params report
 *  whether any statement is forbidden or writer.  On parse failure
 *  returns false and `*out_err` is set to a strdup'd message (caller
 *  must free). */
static bool classify_sql(duckdb_connection con, const char *sql,
                         bool *out_forbidden, bool *out_writer,
                         char **out_err) {
    *out_forbidden = false;
    *out_writer    = false;
    *out_err       = NULL;

    duckdb_extracted_statements ext = NULL;
    idx_t n = duckdb_extract_statements(con, sql, &ext);
    if (n == 0) {
        const char *e = duckdb_extract_statements_error(ext);
        *out_err = strdup(e ? e : "SQL parse error");
        if (ext) duckdb_destroy_extracted(&ext);
        return false;
    }

    for (idx_t i = 0; i < n; i++) {
        duckdb_prepared_statement s = NULL;
        if (duckdb_prepare_extracted_statement(con, ext, i, &s) != DuckDBSuccess) {
            /* Treat unpreparable statements as writers + surface error. */
            const char *e = s ? duckdb_prepare_error(s) : NULL;
            if (e && !*out_err) *out_err = strdup(e);
            *out_writer = true;
            if (s) duckdb_destroy_prepare(&s);
            continue;
        }
        duckdb_statement_type t = duckdb_prepared_statement_type(s);
        if (is_forbidden_stmt(t)) *out_forbidden = true;
        if (is_writer_stmt(t))    *out_writer    = true;
        duckdb_destroy_prepare(&s);
    }
    duckdb_destroy_extracted(&ext);
    return true;
}

/** Build a ResultWrap that carries only a synthetic error message. */
static ResultWrap *rw_new_synthetic(const char *msg) {
    ResultWrap *rw = rw_new();
    rw->synthetic       = true;
    rw->synthetic_error = strdup(msg ? msg : "sandbox error");
    return rw;
}

static bool quota_exceeded(const DuckdbBindings *b, uint64_t *out_used) {
    if (b->quota_bytes == 0) {
        if (out_used) *out_used = 0;
        return false;
    }
    uint64_t used = ddb_disk_usage(b);
    if (out_used) *out_used = used;
    return used >= b->quota_bytes;
}

/* ------------------------------------------------------------------ */
/*  Vector helpers (size-in-bytes of a flat primitive type)            */
/* ------------------------------------------------------------------ */

static size_t ddb_primitive_size(duckdb_type t) {
    switch (t) {
    case DUCKDB_TYPE_BOOLEAN:
    case DUCKDB_TYPE_TINYINT:
    case DUCKDB_TYPE_UTINYINT:    return 1;
    case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_USMALLINT:   return 2;
    case DUCKDB_TYPE_INTEGER:
    case DUCKDB_TYPE_UINTEGER:
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DATE:        return 4;
    case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_DOUBLE:
    case DUCKDB_TYPE_TIME:
    case DUCKDB_TYPE_TIMESTAMP:
    case DUCKDB_TYPE_TIMESTAMP_S:
    case DUCKDB_TYPE_TIMESTAMP_MS:
    case DUCKDB_TYPE_TIMESTAMP_NS:
    case DUCKDB_TYPE_TIMESTAMP_TZ:
    case DUCKDB_TYPE_TIME_TZ:     return 8;
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT:
    case DUCKDB_TYPE_UUID:
    case DUCKDB_TYPE_INTERVAL:    return 16;
    default:                       return 0; /* not flat/primitive */
    }
}

/* Read a single cell of type T from a vector. */
#define VEC_READ_TYPED(T, vec, row) (((T *)duckdb_vector_get_data(vec))[(row)])

/* Check validity bit for a row. Returns true if row is NULL. */
static bool vec_is_null(duckdb_vector vec, idx_t row) {
    uint64_t *valid = duckdb_vector_get_validity(vec);
    if (!valid) return false;
    return !duckdb_validity_row_is_valid(valid, row);
}

/* ------------------------------------------------------------------ */
/*  Shortcut macros                                                    */
/* ------------------------------------------------------------------ */

#define B     ((DuckdbBindings *)env)
#define CON   (B->connection)

#define ARG_I32(n) (args->data[(n)].of.i32)
#define ARG_I64(n) (args->data[(n)].of.i64)
#define ARG_F32(n) (args->data[(n)].of.f32)
#define ARG_F64(n) (args->data[(n)].of.f64)

#define RET_I32(v) do { res->data[0] = (wasm_val_t){.kind=WASM_I32, .of.i32=(v)}; } while(0)
#define RET_I64(v) do { res->data[0] = (wasm_val_t){.kind=WASM_I64, .of.i64=(v)}; } while(0)
#define RET_F32(v) do { res->data[0] = (wasm_val_t){.kind=WASM_F32, .of.f32=(v)}; } while(0)
#define RET_F64(v) do { res->data[0] = (wasm_val_t){.kind=WASM_F64, .of.f64=(v)}; } while(0)

#define GET_RW() \
    ResultWrap *rw = (ResultWrap *)htable_get(&B->ht_result, (uint32_t)ARG_I32(0)); \
    if (rw && rw->synthetic) rw = NULL

/* Same, but does not hide synthetic results — used by the error
 * accessor which needs to surface the synthetic message. */
#define GET_RW_RAW() \
    ResultWrap *rw = (ResultWrap *)htable_get(&B->ht_result, (uint32_t)ARG_I32(0))

#define GET_CW() \
    ChunkWrap *cw = (ChunkWrap *)htable_get(&B->ht_chunk, (uint32_t)ARG_I32(0))

/* ------------------------------------------------------------------ */
/*  Query execution                                                    */
/* ------------------------------------------------------------------ */

/* ddb_query(query_ptr: i32, query_len: i32) -> result_h: i32 */
static wasm_trap_t *fn_query(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t q_ptr = (uint32_t)ARG_I32(0);
    uint32_t q_len = (uint32_t)ARG_I32(1);

    char *query = mem_read_cstr(B, q_ptr, q_len);

    /* --- Sandbox gate ------------------------------------------- */
    bool forbidden = false, writer = false;
    char *parse_err = NULL;
    ResultWrap *rw;
    if (!classify_sql(CON, query, &forbidden, &writer, &parse_err)) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "Yumi sandbox: %s",
                 parse_err ? parse_err : "could not parse SQL");
        bindings_set_error(B, msg);
        rw = rw_new_synthetic(msg);
        free(parse_err);
        free(query);
        uint32_t h = htable_insert(&B->ht_result, rw);
        RET_I32(h);
        return NULL;
    }
    free(parse_err);

    if (forbidden) {
        const char *msg =
            "Yumi sandbox: statement type forbidden "
            "(ATTACH/DETACH/LOAD/SET/TRANSACTION/EXTENSION/EXPORT/COPY)";
        bindings_set_error(B, msg);
        rw = rw_new_synthetic(msg);
        free(query);
        uint32_t h = htable_insert(&B->ht_result, rw);
        RET_I32(h);
        return NULL;
    }

    if (writer) {
        uint64_t used = 0;
        if (quota_exceeded(B, &used)) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "Yumi sandbox: database size %llu bytes reached quota %llu bytes. "
                     "Remove data or ask the user to increase the quota.",
                     (unsigned long long)used,
                     (unsigned long long)B->quota_bytes);
            bindings_set_error(B, msg);
            rw = rw_new_synthetic(msg);
            free(query);
            uint32_t h = htable_insert(&B->ht_result, rw);
            RET_I32(h);
            return NULL;
        }
    }

    rw = rw_new();
    duckdb_query(CON, query, &rw->result);
    free(query);

    /* Post-execution check: if the writer overshot, record it so a
     * follow-up call sees the condition.  DuckDB has already committed
     * the rows, but subsequent writers will be rejected. */
    if (writer) {
        uint64_t used = 0;
        if (quota_exceeded(B, &used)) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "Yumi sandbox: post-write database size %llu bytes "
                     "exceeds quota %llu bytes.",
                     (unsigned long long)used,
                     (unsigned long long)B->quota_bytes);
            bindings_set_error(B, msg);
        }
    }

    /* Always insert — guest checks ddb_result_error to distinguish success */
    uint32_t h = htable_insert(&B->ht_result, rw);
    RET_I32(h);
    return NULL;
}

/* ddb_destroy_result(result_h: i32) */
static wasm_trap_t *fn_destroy_result(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    ResultWrap *rw = (ResultWrap *)htable_get(&B->ht_result, h);
    if (rw) {
        rw_destroy(rw);
        htable_remove(&B->ht_result, h);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Result inspection                                                  */
/* ------------------------------------------------------------------ */

/* ddb_result_error(result_h, out_ptr, out_cap) -> len: i32 */
static wasm_trap_t *fn_result_error(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW_RAW();
    if (!rw) { RET_I32(0); return NULL; }
    const char *err = rw->synthetic_error
                    ? rw->synthetic_error
                    : duckdb_result_error(&rw->result);
    RET_I32(mem_write_cstr(B, (uint32_t)ARG_I32(1),
                               (uint32_t)ARG_I32(2), err));
    return NULL;
}

/* ddb_result_column_count(result_h) -> i32 */
static wasm_trap_t *fn_result_column_count(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    RET_I32(rw ? (int32_t)duckdb_column_count(&rw->result) : 0);
    return NULL;
}

/* ddb_result_row_count(result_h) -> i32 (forces materialization) */
static wasm_trap_t *fn_result_row_count(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    if (!rw) { RET_I32(0); return NULL; }
    rw_materialize_all(rw);
    RET_I32((int32_t)rw->total_rows);
    return NULL;
}

/* ddb_result_rows_changed(result_h) -> i32 */
static wasm_trap_t *fn_result_rows_changed(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    RET_I32(rw ? (int32_t)duckdb_rows_changed(&rw->result) : 0);
    return NULL;
}

/* ddb_result_column_name(result_h, col, out_ptr, out_cap) -> len */
static wasm_trap_t *fn_result_column_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    if (!rw) { RET_I32(0); return NULL; }
    const char *name = duckdb_column_name(&rw->result,
                                          (idx_t)(uint32_t)ARG_I32(1));
    RET_I32(mem_write_cstr(B, (uint32_t)ARG_I32(2),
                               (uint32_t)ARG_I32(3), name));
    return NULL;
}

/* ddb_result_column_type(result_h, col) -> duckdb_type: i32 */
static wasm_trap_t *fn_result_column_type(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    if (!rw) { RET_I32(0); return NULL; }
    RET_I32((int32_t)duckdb_column_type(&rw->result,
                                         (idx_t)(uint32_t)ARG_I32(1)));
    return NULL;
}

/* ddb_result_statement_type(result_h) -> i32 */
static wasm_trap_t *fn_result_statement_type(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    if (!rw) { RET_I32(0); return NULL; }
    RET_I32((int32_t)duckdb_result_statement_type(rw->result));
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Data-chunk API                                                     */
/* ------------------------------------------------------------------ */

/* ddb_fetch_chunk(result_h) -> chunk_h (0 = end-of-result) */
static wasm_trap_t *fn_fetch_chunk(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    if (!rw) { RET_I32(0); return NULL; }
    if (!rw_fetch_one(rw)) { RET_I32(0); return NULL; }
    /* Hand out a view handle over the just-fetched chunk, which remains
     * owned by the ResultWrap. */
    ChunkWrap *cw = (ChunkWrap *)calloc(1, sizeof(*cw));
    cw->chunk = rw->chunks[rw->chunk_count - 1];
    cw->owned = false;
    RET_I32(htable_insert(&B->ht_chunk, cw));
    return NULL;
}

/* ddb_result_get_chunk(result_h, chunk_idx) -> chunk_h */
static wasm_trap_t *fn_result_get_chunk(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    if (!rw) { RET_I32(0); return NULL; }
    idx_t ci = (idx_t)(uint32_t)ARG_I32(1);
    if (!rw_ensure_chunk(rw, ci)) { RET_I32(0); return NULL; }
    ChunkWrap *cw = (ChunkWrap *)calloc(1, sizeof(*cw));
    cw->chunk = rw->chunks[ci];
    cw->owned = false;
    RET_I32(htable_insert(&B->ht_chunk, cw));
    return NULL;
}

/* ddb_result_chunk_count(result_h) -> i32 */
static wasm_trap_t *fn_result_chunk_count(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    RET_I32(rw ? (int32_t)rw->chunk_count : 0);
    return NULL;
}

/* ddb_destroy_chunk(chunk_h) */
static wasm_trap_t *fn_destroy_chunk(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    ChunkWrap *cw = (ChunkWrap *)htable_get(&B->ht_chunk, h);
    if (cw) {
        if (cw->owned) duckdb_destroy_data_chunk(&cw->chunk);
        free(cw);
        htable_remove(&B->ht_chunk, h);
    }
    return NULL;
}

/* ddb_chunk_size(chunk_h) -> i32 */
static wasm_trap_t *fn_chunk_size(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_CW();
    RET_I32(cw ? (int32_t)duckdb_data_chunk_get_size(cw->chunk) : 0);
    return NULL;
}

/* ddb_chunk_column_count(chunk_h) -> i32 */
static wasm_trap_t *fn_chunk_column_count(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_CW();
    RET_I32(cw ? (int32_t)duckdb_data_chunk_get_column_count(cw->chunk) : 0);
    return NULL;
}

/* ddb_chunk_column_type(chunk_h, col) -> i32 */
static wasm_trap_t *fn_chunk_column_type(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_CW();
    if (!cw) { RET_I32(0); return NULL; }
    duckdb_vector v = duckdb_data_chunk_get_vector(cw->chunk,
                          (idx_t)(uint32_t)ARG_I32(1));
    duckdb_logical_type lt = duckdb_vector_get_column_type(v);
    duckdb_type t = duckdb_get_type_id(lt);
    duckdb_destroy_logical_type(&lt);
    RET_I32((int32_t)t);
    return NULL;
}

/* ddb_chunk_is_null(chunk_h, col, row) -> i32 */
static wasm_trap_t *fn_chunk_is_null(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_CW();
    if (!cw) { RET_I32(1); return NULL; }
    duckdb_vector v = duckdb_data_chunk_get_vector(cw->chunk,
                          (idx_t)(uint32_t)ARG_I32(1));
    RET_I32(vec_is_null(v, (idx_t)(uint32_t)ARG_I32(2)) ? 1 : 0);
    return NULL;
}

/* ---- Chunk typed cell accessors ---- */

#define CHUNK_GET_PREAMBLE(default_ret)                                  \
    GET_CW();                                                             \
    if (!cw) { default_ret; return NULL; }                                \
    duckdb_vector v = duckdb_data_chunk_get_vector(cw->chunk,             \
                          (idx_t)(uint32_t)ARG_I32(1));                   \
    idx_t row = (idx_t)(uint32_t)ARG_I32(2)

#define DEF_CHUNK_GET_I32(name, T)                                       \
    static wasm_trap_t *name(void *env,                                   \
            const wasm_val_vec_t *args, wasm_val_vec_t *res) {            \
        CHUNK_GET_PREAMBLE(RET_I32(0));                                   \
        RET_I32((int32_t)VEC_READ_TYPED(T, v, row));                      \
        return NULL;                                                      \
    }

#define DEF_CHUNK_GET_I64(name, T)                                       \
    static wasm_trap_t *name(void *env,                                   \
            const wasm_val_vec_t *args, wasm_val_vec_t *res) {            \
        CHUNK_GET_PREAMBLE(RET_I64(0));                                   \
        RET_I64((int64_t)VEC_READ_TYPED(T, v, row));                      \
        return NULL;                                                      \
    }

DEF_CHUNK_GET_I32(fn_chunk_get_bool,   bool)
DEF_CHUNK_GET_I32(fn_chunk_get_int8,   int8_t)
DEF_CHUNK_GET_I32(fn_chunk_get_int16,  int16_t)
DEF_CHUNK_GET_I32(fn_chunk_get_int32,  int32_t)
DEF_CHUNK_GET_I64(fn_chunk_get_int64,  int64_t)
DEF_CHUNK_GET_I32(fn_chunk_get_uint8,  uint8_t)
DEF_CHUNK_GET_I32(fn_chunk_get_uint16, uint16_t)
DEF_CHUNK_GET_I32(fn_chunk_get_uint32, uint32_t)
DEF_CHUNK_GET_I64(fn_chunk_get_uint64, uint64_t)

static wasm_trap_t *fn_chunk_get_float(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CHUNK_GET_PREAMBLE(RET_F32(0.0f));
    RET_F32(VEC_READ_TYPED(float, v, row));
    return NULL;
}

static wasm_trap_t *fn_chunk_get_double(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CHUNK_GET_PREAMBLE(RET_F64(0.0));
    RET_F64(VEC_READ_TYPED(double, v, row));
    return NULL;
}

/* date stored as duckdb_date { int32_t days } */
static wasm_trap_t *fn_chunk_get_date(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CHUNK_GET_PREAMBLE(RET_I32(0));
    RET_I32(VEC_READ_TYPED(duckdb_date, v, row).days);
    return NULL;
}

static wasm_trap_t *fn_chunk_get_time(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CHUNK_GET_PREAMBLE(RET_I64(0));
    RET_I64(VEC_READ_TYPED(duckdb_time, v, row).micros);
    return NULL;
}

static wasm_trap_t *fn_chunk_get_timestamp(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CHUNK_GET_PREAMBLE(RET_I64(0));
    RET_I64(VEC_READ_TYPED(duckdb_timestamp, v, row).micros);
    return NULL;
}

/* Read varchar/blob raw bytes out of a string_t cell. */
static int32_t write_stringt_to_guest(DuckdbBindings *b,
                                       duckdb_vector vec, idx_t row,
                                       uint32_t out_ptr, uint32_t out_cap) {
    duckdb_string_t *strs = (duckdb_string_t *)duckdb_vector_get_data(vec);
    uint32_t len = duckdb_string_t_length(strs[row]);
    const char *data = duckdb_string_t_data(&strs[row]);
    return mem_write_blob(b, out_ptr, out_cap, data, len);
}

/* ddb_chunk_get_varchar(chunk, col, row, out_ptr, out_cap) -> len */
static wasm_trap_t *fn_chunk_get_varchar(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_CW();
    if (!cw) { RET_I32(0); return NULL; }
    duckdb_vector v = duckdb_data_chunk_get_vector(cw->chunk,
                          (idx_t)(uint32_t)ARG_I32(1));
    idx_t row = (idx_t)(uint32_t)ARG_I32(2);
    uint32_t out_ptr = (uint32_t)ARG_I32(3);
    uint32_t out_cap = (uint32_t)ARG_I32(4);
    RET_I32(write_stringt_to_guest(B, v, row, out_ptr, out_cap));
    return NULL;
}

/* ddb_chunk_get_blob(chunk, col, row, out_ptr, out_cap) -> len */
static wasm_trap_t *fn_chunk_get_blob(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    /* BLOB vectors are stored as duckdb_string_t too */
    GET_CW();
    if (!cw) { RET_I32(0); return NULL; }
    duckdb_vector v = duckdb_data_chunk_get_vector(cw->chunk,
                          (idx_t)(uint32_t)ARG_I32(1));
    idx_t row = (idx_t)(uint32_t)ARG_I32(2);
    uint32_t out_ptr = (uint32_t)ARG_I32(3);
    uint32_t out_cap = (uint32_t)ARG_I32(4);
    RET_I32(write_stringt_to_guest(B, v, row, out_ptr, out_cap));
    return NULL;
}

/* ddb_chunk_get_hugeint(chunk, col, row, out_ptr) */
static wasm_trap_t *fn_chunk_get_hugeint(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    GET_CW();
    if (!cw) return NULL;
    duckdb_vector v = duckdb_data_chunk_get_vector(cw->chunk,
                          (idx_t)(uint32_t)ARG_I32(1));
    idx_t row = (idx_t)(uint32_t)ARG_I32(2);
    uint32_t out_ptr = (uint32_t)ARG_I32(3);
    duckdb_hugeint hi = VEC_READ_TYPED(duckdb_hugeint, v, row);
    mem_write(B, out_ptr,     &hi.lower, 8);
    mem_write(B, out_ptr + 8, &hi.upper, 8);
    return NULL;
}

/* ddb_chunk_copy_column(chunk, col, out_ptr, out_cap) -> bytes_written */
static wasm_trap_t *fn_chunk_copy_column(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_CW();
    if (!cw) { RET_I32(0); return NULL; }
    idx_t col    = (idx_t)(uint32_t)ARG_I32(1);
    uint32_t op  = (uint32_t)ARG_I32(2);
    uint32_t cap = (uint32_t)ARG_I32(3);

    duckdb_vector v = duckdb_data_chunk_get_vector(cw->chunk, col);
    duckdb_logical_type lt = duckdb_vector_get_column_type(v);
    duckdb_type t = duckdb_get_type_id(lt);
    duckdb_destroy_logical_type(&lt);

    size_t esz = ddb_primitive_size(t);
    if (esz == 0) { RET_I32(0); return NULL; }

    idx_t rows = duckdb_data_chunk_get_size(cw->chunk);
    uint32_t bytes = (uint32_t)(esz * rows);
    RET_I32(mem_write_blob(B, op, cap, duckdb_vector_get_data(v), bytes));
    return NULL;
}

/* ddb_chunk_copy_validity(chunk, col, out_ptr, out_cap) -> bytes_written */
static wasm_trap_t *fn_chunk_copy_validity(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_CW();
    if (!cw) { RET_I32(0); return NULL; }
    idx_t col    = (idx_t)(uint32_t)ARG_I32(1);
    uint32_t op  = (uint32_t)ARG_I32(2);
    uint32_t cap = (uint32_t)ARG_I32(3);

    duckdb_vector v = duckdb_data_chunk_get_vector(cw->chunk, col);
    idx_t rows = duckdb_data_chunk_get_size(cw->chunk);
    uint32_t n_u64 = (uint32_t)((rows + 63) / 64);
    uint32_t bytes = n_u64 * 8;

    uint64_t *valid = duckdb_vector_get_validity(v);
    if (valid) {
        RET_I32(mem_write_blob(B, op, cap, valid, bytes));
    } else {
        /* All-valid: emit 0xFF bytes. */
        if (cap > 0 && op != 0) {
            uint8_t *base = wasm_mem_base(B);
            if ((size_t)op + (cap < bytes ? cap : bytes) <= wasm_mem_size(B)) {
                memset(base + op, 0xFF, cap < bytes ? cap : bytes);
            }
        }
        RET_I32((int32_t)bytes);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Cell-style value fetch (backed by cached chunks)                   */
/* ------------------------------------------------------------------ */

/* Locate a (result, col, row) cell and populate (vec, row_in_chunk).
 * Returns true on success; on failure emits a default and returns false. */
static bool locate_cell(DuckdbBindings *b, ResultWrap *rw,
                         uint32_t col, uint32_t row,
                         duckdb_vector *out_vec, idx_t *out_row_in_chunk) {
    (void)b;
    if (!rw) return false;
    idx_t ci, ri;
    if (!rw_locate(rw, (idx_t)row, &ci, &ri)) return false;
    *out_vec = duckdb_data_chunk_get_vector(rw->chunks[ci], (idx_t)col);
    *out_row_in_chunk = ri;
    return true;
}

#define CELL_PREAMBLE(default_ret)                                            \
    GET_RW();                                                                 \
    duckdb_vector v; idx_t row;                                               \
    if (!locate_cell(B, rw, (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2),       \
                      &v, &row)) { default_ret; return NULL; }

/* ddb_value_is_null(r, col, row) -> i32 */
static wasm_trap_t *fn_value_is_null(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CELL_PREAMBLE(RET_I32(1));
    RET_I32(vec_is_null(v, row) ? 1 : 0);
    return NULL;
}

#define DEF_VALUE_I32(name, T)                                                \
    static wasm_trap_t *name(void *env,                                       \
            const wasm_val_vec_t *args, wasm_val_vec_t *res) {                \
        CELL_PREAMBLE(RET_I32(0));                                            \
        RET_I32((int32_t)VEC_READ_TYPED(T, v, row));                          \
        return NULL;                                                          \
    }

#define DEF_VALUE_I64(name, T)                                                \
    static wasm_trap_t *name(void *env,                                       \
            const wasm_val_vec_t *args, wasm_val_vec_t *res) {                \
        CELL_PREAMBLE(RET_I64(0));                                            \
        RET_I64((int64_t)VEC_READ_TYPED(T, v, row));                          \
        return NULL;                                                          \
    }

DEF_VALUE_I32(fn_value_boolean, bool)
DEF_VALUE_I32(fn_value_int8,    int8_t)
DEF_VALUE_I32(fn_value_int16,   int16_t)
DEF_VALUE_I32(fn_value_int32,   int32_t)
DEF_VALUE_I64(fn_value_int64,   int64_t)
DEF_VALUE_I32(fn_value_uint8,   uint8_t)
DEF_VALUE_I32(fn_value_uint16,  uint16_t)
DEF_VALUE_I32(fn_value_uint32,  uint32_t)
DEF_VALUE_I64(fn_value_uint64,  uint64_t)

static wasm_trap_t *fn_value_float(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CELL_PREAMBLE(RET_F32(0.0f));
    RET_F32(VEC_READ_TYPED(float, v, row));
    return NULL;
}

static wasm_trap_t *fn_value_double(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CELL_PREAMBLE(RET_F64(0.0));
    RET_F64(VEC_READ_TYPED(double, v, row));
    return NULL;
}

static wasm_trap_t *fn_value_date(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CELL_PREAMBLE(RET_I32(0));
    RET_I32(VEC_READ_TYPED(duckdb_date, v, row).days);
    return NULL;
}

static wasm_trap_t *fn_value_time(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CELL_PREAMBLE(RET_I64(0));
    RET_I64(VEC_READ_TYPED(duckdb_time, v, row).micros);
    return NULL;
}

static wasm_trap_t *fn_value_timestamp(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    CELL_PREAMBLE(RET_I64(0));
    RET_I64(VEC_READ_TYPED(duckdb_timestamp, v, row).micros);
    return NULL;
}

/* ddb_value_varchar(r, col, row, out_ptr, out_cap) -> len */
static wasm_trap_t *fn_value_varchar(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    duckdb_vector v; idx_t row;
    if (!locate_cell(B, rw, (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2),
                      &v, &row)) { RET_I32(0); return NULL; }
    RET_I32(write_stringt_to_guest(B, v, row,
                                    (uint32_t)ARG_I32(3),
                                    (uint32_t)ARG_I32(4)));
    return NULL;
}

/* ddb_value_blob(r, col, row, out_ptr, out_cap) -> len */
static wasm_trap_t *fn_value_blob(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    GET_RW();
    duckdb_vector v; idx_t row;
    if (!locate_cell(B, rw, (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2),
                      &v, &row)) { RET_I32(0); return NULL; }
    RET_I32(write_stringt_to_guest(B, v, row,
                                    (uint32_t)ARG_I32(3),
                                    (uint32_t)ARG_I32(4)));
    return NULL;
}

/* ddb_value_hugeint(r, col, row, out_ptr) */
static wasm_trap_t *fn_value_hugeint(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    GET_RW();
    duckdb_vector v; idx_t row;
    if (!locate_cell(B, rw, (uint32_t)ARG_I32(1), (uint32_t)ARG_I32(2),
                      &v, &row)) return NULL;
    uint32_t out_ptr = (uint32_t)ARG_I32(3);
    duckdb_hugeint hi = VEC_READ_TYPED(duckdb_hugeint, v, row);
    mem_write(B, out_ptr,     &hi.lower, 8);
    mem_write(B, out_ptr + 8, &hi.upper, 8);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Prepared statements                                                */
/* ------------------------------------------------------------------ */

/* ddb_prepare(query_ptr, query_len) -> prepared_h: i32 */
static wasm_trap_t *fn_prepare(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t q_ptr = (uint32_t)ARG_I32(0);
    uint32_t q_len = (uint32_t)ARG_I32(1);

    char *query = mem_read_cstr(B, q_ptr, q_len);

    duckdb_prepared_statement stmt = NULL;
    duckdb_prepare(CON, query, &stmt);
    free(query);

    /* Always insert — guest checks ddb_prepare_error */
    uint32_t h = htable_insert(&B->ht_prepared, stmt);
    RET_I32(h);
    return NULL;
}

/* ddb_destroy_prepare(prepared_h) */
static wasm_trap_t *fn_destroy_prepare(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    duckdb_prepared_statement stmt =
        (duckdb_prepared_statement)htable_get(&B->ht_prepared, h);
    if (stmt) {
        duckdb_destroy_prepare(&stmt);
        htable_remove(&B->ht_prepared, h);
    }
    return NULL;
}

/* ddb_prepare_error(prepared_h, out_ptr, out_cap) -> len: i32 */
static wasm_trap_t *fn_prepare_error(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_prepared_statement stmt =
        (duckdb_prepared_statement)htable_get(&B->ht_prepared,
                                              (uint32_t)ARG_I32(0));
    if (!stmt) { RET_I32(0); return NULL; }

    const char *err = duckdb_prepare_error(stmt);
    RET_I32(mem_write_cstr(B, (uint32_t)ARG_I32(1),
                               (uint32_t)ARG_I32(2), err));
    return NULL;
}

/* ddb_nparams(prepared_h) -> i32 */
static wasm_trap_t *fn_nparams(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_prepared_statement stmt =
        (duckdb_prepared_statement)htable_get(&B->ht_prepared,
                                              (uint32_t)ARG_I32(0));
    RET_I32(stmt ? (int32_t)duckdb_nparams(stmt) : 0);
    return NULL;
}

/* ddb_param_type(prepared_h, param_idx) -> duckdb_type: i32 */
static wasm_trap_t *fn_param_type(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_prepared_statement stmt =
        (duckdb_prepared_statement)htable_get(&B->ht_prepared,
                                              (uint32_t)ARG_I32(0));
    if (!stmt) { RET_I32(0); return NULL; }
    RET_I32((int32_t)duckdb_param_type(stmt, (idx_t)(uint32_t)ARG_I32(1)));
    return NULL;
}

/* ddb_clear_bindings(prepared_h) -> state: i32 */
static wasm_trap_t *fn_clear_bindings(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_prepared_statement stmt =
        (duckdb_prepared_statement)htable_get(&B->ht_prepared,
                                              (uint32_t)ARG_I32(0));
    RET_I32(stmt ? (int32_t)duckdb_clear_bindings(stmt) : 1);
    return NULL;
}

/* ---- Bind helpers ---- */

#define BIND_PREAMBLE \
    duckdb_prepared_statement stmt = \
        (duckdb_prepared_statement)htable_get(&B->ht_prepared, \
                                              (uint32_t)ARG_I32(0)); \
    if (!stmt) { RET_I32(1); return NULL; } \
    idx_t idx = (idx_t)(uint32_t)ARG_I32(1);

/* ddb_bind_boolean(prepared_h, idx, val) -> state: i32 */
static wasm_trap_t *fn_bind_boolean(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_boolean(stmt, idx, ARG_I32(2) != 0));
    return NULL;
}

/* ddb_bind_int8(prepared_h, idx, val) -> state: i32 */
static wasm_trap_t *fn_bind_int8(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_int8(stmt, idx, (int8_t)ARG_I32(2)));
    return NULL;
}

/* ddb_bind_int16(prepared_h, idx, val) -> state: i32 */
static wasm_trap_t *fn_bind_int16(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_int16(stmt, idx, (int16_t)ARG_I32(2)));
    return NULL;
}

/* ddb_bind_int32(prepared_h, idx, val) -> state: i32 */
static wasm_trap_t *fn_bind_int32(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_int32(stmt, idx, ARG_I32(2)));
    return NULL;
}

/* ddb_bind_int64(prepared_h, idx, val: i64) -> state: i32 */
static wasm_trap_t *fn_bind_int64(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_int64(stmt, idx, ARG_I64(2)));
    return NULL;
}

/* ddb_bind_uint8(prepared_h, idx, val) -> state: i32 */
static wasm_trap_t *fn_bind_uint8(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_uint8(stmt, idx, (uint8_t)ARG_I32(2)));
    return NULL;
}

/* ddb_bind_uint16(prepared_h, idx, val) -> state: i32 */
static wasm_trap_t *fn_bind_uint16(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_uint16(stmt, idx, (uint16_t)ARG_I32(2)));
    return NULL;
}

/* ddb_bind_uint32(prepared_h, idx, val) -> state: i32 */
static wasm_trap_t *fn_bind_uint32(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_uint32(stmt, idx, (uint32_t)ARG_I32(2)));
    return NULL;
}

/* ddb_bind_uint64(prepared_h, idx, val: i64) -> state: i32 */
static wasm_trap_t *fn_bind_uint64(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_uint64(stmt, idx, (uint64_t)ARG_I64(2)));
    return NULL;
}

/* ddb_bind_float(prepared_h, idx, val: f32) -> state: i32 */
static wasm_trap_t *fn_bind_float(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_float(stmt, idx, ARG_F32(2)));
    return NULL;
}

/* ddb_bind_double(prepared_h, idx, val: f64) -> state: i32 */
static wasm_trap_t *fn_bind_double(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_double(stmt, idx, ARG_F64(2)));
    return NULL;
}

/* ddb_bind_varchar(prepared_h, idx, ptr, len) -> state: i32 */
static wasm_trap_t *fn_bind_varchar(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    uint32_t s_ptr = (uint32_t)ARG_I32(2);
    uint32_t s_len = (uint32_t)ARG_I32(3);
    char *str = mem_read_cstr(B, s_ptr, s_len);
    duckdb_state st = duckdb_bind_varchar_length(stmt, idx, str, s_len);
    free(str);
    RET_I32((int32_t)st);
    return NULL;
}

/* ddb_bind_blob(prepared_h, idx, ptr, len) -> state: i32 */
static wasm_trap_t *fn_bind_blob(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    uint32_t d_ptr = (uint32_t)ARG_I32(2);
    uint32_t d_len = (uint32_t)ARG_I32(3);
    uint8_t *base = wasm_mem_base(B);
    if ((size_t)d_ptr + d_len > wasm_mem_size(B)) { RET_I32(1); return NULL; }
    RET_I32((int32_t)duckdb_bind_blob(stmt, idx, base + d_ptr, d_len));
    return NULL;
}

/* ddb_bind_null(prepared_h, idx) -> state: i32 */
static wasm_trap_t *fn_bind_null(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    RET_I32((int32_t)duckdb_bind_null(stmt, idx));
    return NULL;
}

/* ddb_bind_date(prepared_h, idx, days: i32) -> state: i32 */
static wasm_trap_t *fn_bind_date(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    duckdb_date d = { .days = ARG_I32(2) };
    RET_I32((int32_t)duckdb_bind_date(stmt, idx, d));
    return NULL;
}

/* ddb_bind_time(prepared_h, idx, micros: i64) -> state: i32 */
static wasm_trap_t *fn_bind_time(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    duckdb_time t = { .micros = ARG_I64(2) };
    RET_I32((int32_t)duckdb_bind_time(stmt, idx, t));
    return NULL;
}

/* ddb_bind_timestamp(prepared_h, idx, micros: i64) -> state: i32 */
static wasm_trap_t *fn_bind_timestamp(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    BIND_PREAMBLE;
    duckdb_timestamp ts = { .micros = ARG_I64(2) };
    RET_I32((int32_t)duckdb_bind_timestamp(stmt, idx, ts));
    return NULL;
}

/* ddb_execute_prepared(prepared_h) -> result_h: i32 */
static wasm_trap_t *fn_execute_prepared(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_prepared_statement stmt =
        (duckdb_prepared_statement)htable_get(&B->ht_prepared,
                                              (uint32_t)ARG_I32(0));
    if (!stmt) { RET_I32(0); return NULL; }

    /* --- Sandbox gate --------------------------------------------- */
    duckdb_statement_type t = duckdb_prepared_statement_type(stmt);
    if (is_forbidden_stmt(t)) {
        const char *msg =
            "Yumi sandbox: prepared statement type forbidden "
            "(ATTACH/DETACH/LOAD/SET/TRANSACTION/EXTENSION/EXPORT/COPY)";
        bindings_set_error(B, msg);
        ResultWrap *rw = rw_new_synthetic(msg);
        uint32_t h = htable_insert(&B->ht_result, rw);
        RET_I32(h);
        return NULL;
    }
    if (is_writer_stmt(t)) {
        uint64_t used = 0;
        if (quota_exceeded(B, &used)) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "Yumi sandbox: database size %llu bytes reached quota %llu bytes. "
                     "Remove data or ask the user to increase the quota.",
                     (unsigned long long)used,
                     (unsigned long long)B->quota_bytes);
            bindings_set_error(B, msg);
            ResultWrap *rw = rw_new_synthetic(msg);
            uint32_t h = htable_insert(&B->ht_result, rw);
            RET_I32(h);
            return NULL;
        }
    }

    ResultWrap *rw = rw_new();
    duckdb_execute_prepared(stmt, &rw->result);

    if (is_writer_stmt(t)) {
        uint64_t used = 0;
        if (quota_exceeded(B, &used)) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "Yumi sandbox: post-write database size %llu bytes "
                     "exceeds quota %llu bytes.",
                     (unsigned long long)used,
                     (unsigned long long)B->quota_bytes);
            bindings_set_error(B, msg);
        }
    }

    uint32_t h = htable_insert(&B->ht_result, rw);
    RET_I32(h);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Appender                                                           */
/* ------------------------------------------------------------------ */

/* ddb_appender_create(schema_ptr, schema_len, table_ptr, table_len) -> appender_h */
static wasm_trap_t *fn_appender_create(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    /* Appenders are always writers — pre-check quota. */
    uint64_t used = 0;
    if (quota_exceeded(B, &used)) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "Yumi sandbox: database size %llu bytes reached quota %llu bytes. "
                 "Cannot create appender. Remove data or raise the quota.",
                 (unsigned long long)used,
                 (unsigned long long)B->quota_bytes);
        bindings_set_error(B, msg);
        RET_I32(0);
        return NULL;
    }

    uint32_t s_ptr = (uint32_t)ARG_I32(0);
    uint32_t s_len = (uint32_t)ARG_I32(1);
    uint32_t t_ptr = (uint32_t)ARG_I32(2);
    uint32_t t_len = (uint32_t)ARG_I32(3);

    char *schema = s_len > 0 ? mem_read_cstr(B, s_ptr, s_len) : NULL;
    char *table  = mem_read_cstr(B, t_ptr, t_len);

    duckdb_appender appender = NULL;
    duckdb_appender_create(CON, schema, table, &appender);

    free(schema);
    free(table);

    uint32_t h = appender ? htable_insert(&B->ht_appender, appender) : 0;
    RET_I32(h);
    return NULL;
}

/* ddb_appender_destroy(appender_h) */
static wasm_trap_t *fn_appender_destroy(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    duckdb_appender ap = (duckdb_appender)htable_get(&B->ht_appender, h);
    if (ap) {
        duckdb_appender_destroy(&ap);
        htable_remove(&B->ht_appender, h);
    }
    return NULL;
}

/* ddb_appender_error(appender_h, out_ptr, out_cap) -> len: i32 */
static wasm_trap_t *fn_appender_error(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_appender ap = (duckdb_appender)htable_get(&B->ht_appender,
                                                      (uint32_t)ARG_I32(0));
    if (!ap) { RET_I32(0); return NULL; }
    const char *err = duckdb_appender_error(ap);
    RET_I32(mem_write_cstr(B, (uint32_t)ARG_I32(1),
                               (uint32_t)ARG_I32(2), err));
    return NULL;
}

/* ddb_appender_flush(appender_h) -> state: i32 */
static wasm_trap_t *fn_appender_flush(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_appender ap = (duckdb_appender)htable_get(&B->ht_appender,
                                                      (uint32_t)ARG_I32(0));
    if (!ap) { RET_I32(1); return NULL; }
    /* Pre-flush quota check. */
    uint64_t used = 0;
    if (quota_exceeded(B, &used)) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "Yumi sandbox: quota %llu bytes reached (used %llu). "
                 "Flush rejected — remove data or raise the quota.",
                 (unsigned long long)B->quota_bytes,
                 (unsigned long long)used);
        bindings_set_error(B, msg);
        RET_I32(1);
        return NULL;
    }
    duckdb_state st = duckdb_appender_flush(ap);
    /* Post-flush overshoot is informational; surface via last_error. */
    if (quota_exceeded(B, &used)) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "Yumi sandbox: appender flush overshot quota "
                 "(used %llu, quota %llu).",
                 (unsigned long long)used,
                 (unsigned long long)B->quota_bytes);
        bindings_set_error(B, msg);
    }
    RET_I32((int32_t)st);
    return NULL;
}

/* ddb_appender_close(appender_h) -> state: i32 */
static wasm_trap_t *fn_appender_close(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_appender ap = (duckdb_appender)htable_get(&B->ht_appender,
                                                      (uint32_t)ARG_I32(0));
    if (!ap) { RET_I32(1); return NULL; }
    /* Always allow close to run (it needs to flush buffered state), but
     * report quota overshoot via last_error afterwards. */
    duckdb_state st = duckdb_appender_close(ap);
    uint64_t used = 0;
    if (quota_exceeded(B, &used)) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "Yumi sandbox: appender close overshot quota "
                 "(used %llu, quota %llu).",
                 (unsigned long long)used,
                 (unsigned long long)B->quota_bytes);
        bindings_set_error(B, msg);
    }
    RET_I32((int32_t)st);
    return NULL;
}

/* ddb_appender_begin_row(appender_h) -> state: i32 */
static wasm_trap_t *fn_appender_begin_row(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_appender ap = (duckdb_appender)htable_get(&B->ht_appender,
                                                      (uint32_t)ARG_I32(0));
    RET_I32(ap ? (int32_t)duckdb_appender_begin_row(ap) : 1);
    return NULL;
}

/* ddb_appender_end_row(appender_h) -> state: i32 */
static wasm_trap_t *fn_appender_end_row(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_appender ap = (duckdb_appender)htable_get(&B->ht_appender,
                                                      (uint32_t)ARG_I32(0));
    RET_I32(ap ? (int32_t)duckdb_appender_end_row(ap) : 1);
    return NULL;
}

#define APPEND_PREAMBLE \
    duckdb_appender ap = (duckdb_appender)htable_get(&B->ht_appender, \
                                                      (uint32_t)ARG_I32(0)); \
    if (!ap) { RET_I32(1); return NULL; }

/* ddb_append_boolean(appender_h, val) -> state: i32 */
static wasm_trap_t *fn_append_boolean(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_bool(ap, ARG_I32(1) != 0));
    return NULL;
}

/* ddb_append_int8(appender_h, val) -> state: i32 */
static wasm_trap_t *fn_append_int8(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_int8(ap, (int8_t)ARG_I32(1)));
    return NULL;
}

/* ddb_append_int16(appender_h, val) -> state: i32 */
static wasm_trap_t *fn_append_int16(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_int16(ap, (int16_t)ARG_I32(1)));
    return NULL;
}

/* ddb_append_int32(appender_h, val) -> state: i32 */
static wasm_trap_t *fn_append_int32(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_int32(ap, ARG_I32(1)));
    return NULL;
}

/* ddb_append_int64(appender_h, val: i64) -> state: i32 */
static wasm_trap_t *fn_append_int64(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_int64(ap, ARG_I64(1)));
    return NULL;
}

/* ddb_append_uint8(appender_h, val) -> state: i32 */
static wasm_trap_t *fn_append_uint8(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_uint8(ap, (uint8_t)ARG_I32(1)));
    return NULL;
}

/* ddb_append_uint16(appender_h, val) -> state: i32 */
static wasm_trap_t *fn_append_uint16(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_uint16(ap, (uint16_t)ARG_I32(1)));
    return NULL;
}

/* ddb_append_uint32(appender_h, val) -> state: i32 */
static wasm_trap_t *fn_append_uint32(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_uint32(ap, (uint32_t)ARG_I32(1)));
    return NULL;
}

/* ddb_append_uint64(appender_h, val: i64) -> state: i32 */
static wasm_trap_t *fn_append_uint64(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_uint64(ap, (uint64_t)ARG_I64(1)));
    return NULL;
}

/* ddb_append_float(appender_h, val: f32) -> state: i32 */
static wasm_trap_t *fn_append_float(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_float(ap, ARG_F32(1)));
    return NULL;
}

/* ddb_append_double(appender_h, val: f64) -> state: i32 */
static wasm_trap_t *fn_append_double(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_double(ap, ARG_F64(1)));
    return NULL;
}

/* ddb_append_varchar(appender_h, ptr, len) -> state: i32 */
static wasm_trap_t *fn_append_varchar(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    uint32_t s_ptr = (uint32_t)ARG_I32(1);
    uint32_t s_len = (uint32_t)ARG_I32(2);
    char *str = mem_read_cstr(B, s_ptr, s_len);
    duckdb_state st = duckdb_append_varchar_length(ap, str, s_len);
    free(str);
    RET_I32((int32_t)st);
    return NULL;
}

/* ddb_append_blob(appender_h, ptr, len) -> state: i32 */
static wasm_trap_t *fn_append_blob(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    uint32_t d_ptr = (uint32_t)ARG_I32(1);
    uint32_t d_len = (uint32_t)ARG_I32(2);
    uint8_t *base = wasm_mem_base(B);
    if ((size_t)d_ptr + d_len > wasm_mem_size(B)) { RET_I32(1); return NULL; }
    RET_I32((int32_t)duckdb_append_blob(ap, base + d_ptr, d_len));
    return NULL;
}

/* ddb_append_null(appender_h) -> state: i32 */
static wasm_trap_t *fn_append_null(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    RET_I32((int32_t)duckdb_append_null(ap));
    return NULL;
}

/* ddb_append_date(appender_h, days: i32) -> state: i32 */
static wasm_trap_t *fn_append_date(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    duckdb_date d = { .days = ARG_I32(1) };
    RET_I32((int32_t)duckdb_append_date(ap, d));
    return NULL;
}

/* ddb_append_time(appender_h, micros: i64) -> state: i32 */
static wasm_trap_t *fn_append_time(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    duckdb_time t = { .micros = ARG_I64(1) };
    RET_I32((int32_t)duckdb_append_time(ap, t));
    return NULL;
}

/* ddb_append_timestamp(appender_h, micros: i64) -> state: i32 */
static wasm_trap_t *fn_append_timestamp(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    APPEND_PREAMBLE;
    duckdb_timestamp ts = { .micros = ARG_I64(1) };
    RET_I32((int32_t)duckdb_append_timestamp(ap, ts));
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Table description                                                  */
/* ------------------------------------------------------------------ */

/* ddb_table_description_create(schema_ptr, schema_len, table_ptr, table_len) -> desc_h */
static wasm_trap_t *fn_table_desc_create(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t s_ptr = (uint32_t)ARG_I32(0);
    uint32_t s_len = (uint32_t)ARG_I32(1);
    uint32_t t_ptr = (uint32_t)ARG_I32(2);
    uint32_t t_len = (uint32_t)ARG_I32(3);

    char *schema = s_len > 0 ? mem_read_cstr(B, s_ptr, s_len) : NULL;
    char *table  = mem_read_cstr(B, t_ptr, t_len);

    duckdb_table_description desc = NULL;
    duckdb_table_description_create(CON, schema, table, &desc);

    free(schema);
    free(table);

    uint32_t h = desc ? htable_insert(&B->ht_table_desc, desc) : 0;
    RET_I32(h);
    return NULL;
}

/* ddb_table_description_destroy(desc_h) */
static wasm_trap_t *fn_table_desc_destroy(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    duckdb_table_description desc =
        (duckdb_table_description)htable_get(&B->ht_table_desc, h);
    if (desc) {
        duckdb_table_description_destroy(&desc);
        htable_remove(&B->ht_table_desc, h);
    }
    return NULL;
}

/* ddb_table_description_error(desc_h, out_ptr, out_cap) -> len: i32 */
static wasm_trap_t *fn_table_desc_error(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_table_description desc =
        (duckdb_table_description)htable_get(&B->ht_table_desc,
                                              (uint32_t)ARG_I32(0));
    if (!desc) { RET_I32(0); return NULL; }
    const char *err = duckdb_table_description_error(desc);
    RET_I32(mem_write_cstr(B, (uint32_t)ARG_I32(1),
                               (uint32_t)ARG_I32(2), err));
    return NULL;
}

/* ddb_column_has_default(desc_h, col_idx) -> i32 (1=yes, 0=no, -1=error) */
static wasm_trap_t *fn_column_has_default(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_table_description desc =
        (duckdb_table_description)htable_get(&B->ht_table_desc,
                                              (uint32_t)ARG_I32(0));
    if (!desc) { RET_I32(-1); return NULL; }

    bool has_default = false;
    duckdb_state st = duckdb_column_has_default(desc,
                          (idx_t)(uint32_t)ARG_I32(1), &has_default);
    RET_I32(st == DuckDBSuccess ? (has_default ? 1 : 0) : -1);
    return NULL;
}

/* ddb_table_description_column_name(desc_h, col_idx, out_ptr, out_cap) -> len */
static wasm_trap_t *fn_table_desc_column_name(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_table_description desc =
        (duckdb_table_description)htable_get(&B->ht_table_desc,
                                              (uint32_t)ARG_I32(0));
    if (!desc) { RET_I32(0); return NULL; }

    char *name = duckdb_table_description_get_column_name(
                     desc, (idx_t)(uint32_t)ARG_I32(1));
    int32_t len = mem_write_cstr(B, (uint32_t)ARG_I32(2),
                                     (uint32_t)ARG_I32(3), name);
    duckdb_free(name);
    RET_I32(len);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Extracted statements (multi-statement batches)                     */
/* ------------------------------------------------------------------ */

/* ddb_extract_statements(query_ptr, query_len) -> extracted_h: i32 */
static wasm_trap_t *fn_extract_statements(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t q_ptr = (uint32_t)ARG_I32(0);
    uint32_t q_len = (uint32_t)ARG_I32(1);

    char *query = mem_read_cstr(B, q_ptr, q_len);

    duckdb_extracted_statements ext = NULL;
    idx_t count = duckdb_extract_statements(CON, query, &ext);
    free(query);

    /* Store count in lower 16 bits of a wrapper, ext as the pointer */
    /* Actually, just store ext — guest queries count separately */
    if (count == 0 && ext) {
        /* Error case — still insert so guest can retrieve error */
    }
    uint32_t h = ext ? htable_insert(&B->ht_extracted, ext) : 0;
    RET_I32(h);
    return NULL;
}

/* ddb_extract_statements_count(extracted_h) -> i32 */
static wasm_trap_t *fn_extract_statements_count(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    /* Re-extract to get count — or we store it.
       Simpler: guest calls ddb_extract_statements which returns count,
       but our handle approach means we need a separate query.
       We'll re-query using the extracted object — DuckDB doesn't have
       a direct "count" function on extracted_statements, the count
       is the return value of duckdb_extract_statements.
       Let's store it alongside. */
    /* HACK: We don't have a good way without a side struct. For now,
       the guest should note the count from a wrapper that returns both.
       We'll provide a combined function instead. */
    RET_I32(0);
    return NULL;
}

/* ddb_destroy_extracted(extracted_h) */
static wasm_trap_t *fn_destroy_extracted(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    duckdb_extracted_statements ext =
        (duckdb_extracted_statements)htable_get(&B->ht_extracted, h);
    if (ext) {
        duckdb_destroy_extracted(&ext);
        htable_remove(&B->ht_extracted, h);
    }
    return NULL;
}

/* ddb_extract_statements_error(extracted_h, out_ptr, out_cap) -> len */
static wasm_trap_t *fn_extract_statements_error(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_extracted_statements ext =
        (duckdb_extracted_statements)htable_get(&B->ht_extracted,
                                                 (uint32_t)ARG_I32(0));
    if (!ext) { RET_I32(0); return NULL; }
    const char *err = duckdb_extract_statements_error(ext);
    RET_I32(mem_write_cstr(B, (uint32_t)ARG_I32(1),
                               (uint32_t)ARG_I32(2), err));
    return NULL;
}

/* ddb_prepare_extracted(extracted_h, index) -> prepared_h */
static wasm_trap_t *fn_prepare_extracted(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    duckdb_extracted_statements ext =
        (duckdb_extracted_statements)htable_get(&B->ht_extracted,
                                                 (uint32_t)ARG_I32(0));
    if (!ext) { RET_I32(0); return NULL; }

    duckdb_prepared_statement stmt = NULL;
    duckdb_state st = duckdb_prepare_extracted_statement(
        CON, ext, (idx_t)(uint32_t)ARG_I32(1), &stmt);

    if (st != DuckDBSuccess || !stmt) { RET_I32(0); return NULL; }

    uint32_t h = htable_insert(&B->ht_prepared, stmt);
    RET_I32(h);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Convenience: ddb_query_run — extract + execute all, return count   */
/*  ddb_query_run(query_ptr, query_len) -> i32 (number of statements) */
/*  Results for each statement accessible via                          */
/*  ddb_query_run_result(index) once we store them.                    */
/*  ... Actually this gets complex. Keep it simple: guest loops with   */
/*  extract_statements + prepare_extracted + execute_prepared.         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Quota / sandbox telemetry bindings                                 */
/* ------------------------------------------------------------------ */

/* ddb_last_error(out_ptr, out_cap) -> len: i32
 * Returns the last sandbox/quota error (from a rejected query or
 * appender op).  Cleared once the guest reads it with a non-zero
 * buffer capacity, so two-call length-probing is safe. */
static wasm_trap_t *fn_last_error(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    const char *e = B->last_error ? B->last_error : "";
    uint32_t out_ptr = (uint32_t)ARG_I32(0);
    uint32_t out_cap = (uint32_t)ARG_I32(1);
    int32_t n = mem_write_cstr(B, out_ptr, out_cap, e);
    if (out_cap > 0 && out_ptr != 0) {
        free(B->last_error);
        B->last_error = NULL;
    }
    RET_I32(n);
    return NULL;
}

/* ddb_db_size() -> i64 : bytes currently used on disk (main + WAL). */
static wasm_trap_t *fn_db_size(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I64((int64_t)ddb_disk_usage(B));
    return NULL;
}

/* ddb_db_quota() -> i64 : configured quota in bytes (0 = unlimited). */
static wasm_trap_t *fn_db_quota(void *env,
        const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)args;
    RET_I64((int64_t)B->quota_bytes);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Function registry                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char                    *name;
    wasm_func_callback_with_env_t  cb;
    uint32_t np;  wasm_valkind_t   params[5];
    uint32_t nr;  wasm_valkind_t   results[1];
} BindingEntry;

static const BindingEntry DUCKDB_BINDINGS[] = {

    /* ---- Query execution ---- */
    {"ddb_query",                       fn_query,                 2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_destroy_result",              fn_destroy_result,        1, {WASM_I32},                                               0, {0}},

    /* ---- Data chunks (modern streaming API) ---- */
    {"ddb_fetch_chunk",                 fn_fetch_chunk,           1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_result_get_chunk",            fn_result_get_chunk,      2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_result_chunk_count",          fn_result_chunk_count,    1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_destroy_chunk",               fn_destroy_chunk,         1, {WASM_I32},                                               0, {0}},
    {"ddb_chunk_size",                  fn_chunk_size,            1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_chunk_column_count",          fn_chunk_column_count,    1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_chunk_column_type",           fn_chunk_column_type,     2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_chunk_is_null",               fn_chunk_is_null,         3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_chunk_get_bool",              fn_chunk_get_bool,        3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_chunk_get_int8",              fn_chunk_get_int8,        3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_chunk_get_int16",             fn_chunk_get_int16,       3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_chunk_get_int32",             fn_chunk_get_int32,       3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_chunk_get_int64",             fn_chunk_get_int64,       3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I64}},
    {"ddb_chunk_get_uint8",             fn_chunk_get_uint8,       3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_chunk_get_uint16",            fn_chunk_get_uint16,      3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_chunk_get_uint32",            fn_chunk_get_uint32,      3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_chunk_get_uint64",            fn_chunk_get_uint64,      3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I64}},
    {"ddb_chunk_get_float",             fn_chunk_get_float,       3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_F32}},
    {"ddb_chunk_get_double",            fn_chunk_get_double,      3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_F64}},
    {"ddb_chunk_get_date",              fn_chunk_get_date,        3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_chunk_get_time",              fn_chunk_get_time,        3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I64}},
    {"ddb_chunk_get_timestamp",         fn_chunk_get_timestamp,   3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I64}},
    {"ddb_chunk_get_varchar",           fn_chunk_get_varchar,     5, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32},         1, {WASM_I32}},
    {"ddb_chunk_get_blob",              fn_chunk_get_blob,        5, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32},         1, {WASM_I32}},
    {"ddb_chunk_get_hugeint",           fn_chunk_get_hugeint,     4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},                  0, {0}},
    {"ddb_chunk_copy_column",           fn_chunk_copy_column,     4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},                  1, {WASM_I32}},
    {"ddb_chunk_copy_validity",         fn_chunk_copy_validity,   4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},                  1, {WASM_I32}},

    /* ---- Result inspection ---- */
    {"ddb_result_error",                fn_result_error,          3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_result_column_count",         fn_result_column_count,   1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_result_row_count",            fn_result_row_count,      1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_result_rows_changed",         fn_result_rows_changed,   1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_result_column_name",          fn_result_column_name,    4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},                   1, {WASM_I32}},
    {"ddb_result_column_type",          fn_result_column_type,    2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_result_statement_type",       fn_result_statement_type, 1, {WASM_I32},                                               1, {WASM_I32}},

    /* ---- Safe value fetch ---- */
    {"ddb_value_is_null",               fn_value_is_null,         3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_value_boolean",               fn_value_boolean,         3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_value_int8",                  fn_value_int8,            3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_value_int16",                 fn_value_int16,           3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_value_int32",                 fn_value_int32,           3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_value_int64",                 fn_value_int64,           3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I64}},
    {"ddb_value_uint8",                 fn_value_uint8,           3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_value_uint16",                fn_value_uint16,          3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_value_uint32",                fn_value_uint32,          3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_value_uint64",                fn_value_uint64,          3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I64}},
    {"ddb_value_float",                 fn_value_float,           3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_F32}},
    {"ddb_value_double",                fn_value_double,          3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_F64}},
    {"ddb_value_date",                  fn_value_date,            3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_value_time",                  fn_value_time,            3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I64}},
    {"ddb_value_timestamp",             fn_value_timestamp,       3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I64}},
    {"ddb_value_varchar",               fn_value_varchar,         5, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32},         1, {WASM_I32}},
    {"ddb_value_blob",                  fn_value_blob,            5, {WASM_I32,WASM_I32,WASM_I32,WASM_I32,WASM_I32},         1, {WASM_I32}},
    {"ddb_value_hugeint",               fn_value_hugeint,         4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},                  0, {0}},

    /* ---- Prepared statements ---- */
    {"ddb_prepare",                     fn_prepare,               2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_destroy_prepare",             fn_destroy_prepare,       1, {WASM_I32},                                               0, {0}},
    {"ddb_prepare_error",               fn_prepare_error,         3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_nparams",                     fn_nparams,               1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_param_type",                  fn_param_type,            2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_clear_bindings",              fn_clear_bindings,        1, {WASM_I32},                                               1, {WASM_I32}},

    /* ---- Bind ---- */
    {"ddb_bind_boolean",                fn_bind_boolean,          3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_bind_int8",                   fn_bind_int8,             3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_bind_int16",                  fn_bind_int16,            3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_bind_int32",                  fn_bind_int32,            3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_bind_int64",                  fn_bind_int64,            3, {WASM_I32,WASM_I32,WASM_I64},                            1, {WASM_I32}},
    {"ddb_bind_uint8",                  fn_bind_uint8,            3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_bind_uint16",                 fn_bind_uint16,           3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_bind_uint32",                 fn_bind_uint32,           3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_bind_uint64",                 fn_bind_uint64,           3, {WASM_I32,WASM_I32,WASM_I64},                            1, {WASM_I32}},
    {"ddb_bind_float",                  fn_bind_float,            3, {WASM_I32,WASM_I32,WASM_F32},                            1, {WASM_I32}},
    {"ddb_bind_double",                 fn_bind_double,           3, {WASM_I32,WASM_I32,WASM_F64},                            1, {WASM_I32}},
    {"ddb_bind_varchar",                fn_bind_varchar,          4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},                   1, {WASM_I32}},
    {"ddb_bind_blob",                   fn_bind_blob,             4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},                   1, {WASM_I32}},
    {"ddb_bind_null",                   fn_bind_null,             2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_bind_date",                   fn_bind_date,             3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_bind_time",                   fn_bind_time,             3, {WASM_I32,WASM_I32,WASM_I64},                            1, {WASM_I32}},
    {"ddb_bind_timestamp",              fn_bind_timestamp,        3, {WASM_I32,WASM_I32,WASM_I64},                            1, {WASM_I32}},
    {"ddb_execute_prepared",            fn_execute_prepared,      1, {WASM_I32},                                               1, {WASM_I32}},

    /* ---- Appender ---- */
    {"ddb_appender_create",             fn_appender_create,       4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},                   1, {WASM_I32}},
    {"ddb_appender_destroy",            fn_appender_destroy,      1, {WASM_I32},                                               0, {0}},
    {"ddb_appender_error",              fn_appender_error,        3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_appender_flush",              fn_appender_flush,        1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_appender_close",              fn_appender_close,        1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_appender_begin_row",          fn_appender_begin_row,    1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_appender_end_row",            fn_appender_end_row,      1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_append_boolean",              fn_append_boolean,        2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_append_int8",                 fn_append_int8,           2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_append_int16",                fn_append_int16,          2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_append_int32",                fn_append_int32,          2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_append_int64",                fn_append_int64,          2, {WASM_I32,WASM_I64},                                     1, {WASM_I32}},
    {"ddb_append_uint8",                fn_append_uint8,          2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_append_uint16",               fn_append_uint16,         2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_append_uint32",               fn_append_uint32,         2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_append_uint64",               fn_append_uint64,         2, {WASM_I32,WASM_I64},                                     1, {WASM_I32}},
    {"ddb_append_float",                fn_append_float,          2, {WASM_I32,WASM_F32},                                     1, {WASM_I32}},
    {"ddb_append_double",               fn_append_double,         2, {WASM_I32,WASM_F64},                                     1, {WASM_I32}},
    {"ddb_append_varchar",              fn_append_varchar,        3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_append_blob",                 fn_append_blob,           3, {WASM_I32,WASM_I32,WASM_I32},                            1, {WASM_I32}},
    {"ddb_append_null",                 fn_append_null,           1, {WASM_I32},                                               1, {WASM_I32}},
    {"ddb_append_date",                 fn_append_date,           2, {WASM_I32,WASM_I32},                                     1, {WASM_I32}},
    {"ddb_append_time",                 fn_append_time,           2, {WASM_I32,WASM_I64},                                     1, {WASM_I32}},
    {"ddb_append_timestamp",            fn_append_timestamp,      2, {WASM_I32,WASM_I64},                                     1, {WASM_I32}},

    /* ---- Table description ---- */
    {"ddb_table_description_create",       fn_table_desc_create,       4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},              1, {WASM_I32}},
    {"ddb_table_description_destroy",      fn_table_desc_destroy,      1, {WASM_I32},                                          0, {0}},
    {"ddb_table_description_error",        fn_table_desc_error,        3, {WASM_I32,WASM_I32,WASM_I32},                       1, {WASM_I32}},
    {"ddb_column_has_default",             fn_column_has_default,      2, {WASM_I32,WASM_I32},                                 1, {WASM_I32}},
    {"ddb_table_description_column_name",  fn_table_desc_column_name,  4, {WASM_I32,WASM_I32,WASM_I32,WASM_I32},              1, {WASM_I32}},

    /* ---- Extracted statements ---- */
    {"ddb_extract_statements",          fn_extract_statements,          2, {WASM_I32,WASM_I32},                                1, {WASM_I32}},
    {"ddb_destroy_extracted",           fn_destroy_extracted,           1, {WASM_I32},                                          0, {0}},
    {"ddb_extract_statements_error",    fn_extract_statements_error,    3, {WASM_I32,WASM_I32,WASM_I32},                       1, {WASM_I32}},
    {"ddb_prepare_extracted",           fn_prepare_extracted,           2, {WASM_I32,WASM_I32},                                 1, {WASM_I32}},

    /* ---- Sandbox / quota telemetry ---- */
    {"ddb_last_error",                  fn_last_error,                  2, {WASM_I32,WASM_I32},                                 1, {WASM_I32}},
    {"ddb_db_size",                     fn_db_size,                     0, {0},                                                 1, {WASM_I64}},
    {"ddb_db_quota",                    fn_db_quota,                    0, {0},                                                 1, {WASM_I64}},
};
#define NUM_DUCKDB_BINDINGS (sizeof(DUCKDB_BINDINGS) / sizeof(DUCKDB_BINDINGS[0]))

/* ------------------------------------------------------------------ */
/*  functype builder                                                   */
/* ------------------------------------------------------------------ */

static wasm_functype_t *make_ft(uint32_t np, const wasm_valkind_t p[],
                                 uint32_t nr, const wasm_valkind_t r[]) {
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[10];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else {
        wasm_valtype_vec_new_empty(&params);
    }
    if (nr > 0) {
        wasm_valtype_t *rt[1];
        for (uint32_t i = 0; i < nr; i++) rt[i] = wasm_valtype_new(r[i]);
        wasm_valtype_vec_new(&results, nr, rt);
    } else {
        wasm_valtype_vec_new_empty(&results);
    }
    return wasm_functype_new(&params, &results);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void duckdb_bindings_init(DuckdbBindings *b,
                           duckdb_database db,
                           duckdb_connection con) {
    memset(b, 0, sizeof(*b));
    b->database   = db;
    b->connection = con;

    htable_init(&b->ht_result,     128);
    htable_init(&b->ht_chunk,      256);
    htable_init(&b->ht_prepared,    64);
    htable_init(&b->ht_appender,    32);
    htable_init(&b->ht_table_desc,  32);
    htable_init(&b->ht_extracted,   16);
}

void duckdb_bindings_set_memory(DuckdbBindings *b, wasm_memory_t *mem) {
    b->memory = mem;
}

size_t duckdb_bindings_get_imports(DuckdbBindings *b, wasm_store_t *store,
                                    const char ***out_names,
                                    wasm_func_t ***out_funcs) {
    static const char *names[NUM_DUCKDB_BINDINGS];
    static wasm_func_t *funcs[NUM_DUCKDB_BINDINGS];

    for (size_t i = 0; i < NUM_DUCKDB_BINDINGS; i++) {
        names[i] = DUCKDB_BINDINGS[i].name;
        wasm_functype_t *ft = make_ft(
            DUCKDB_BINDINGS[i].np, DUCKDB_BINDINGS[i].params,
            DUCKDB_BINDINGS[i].nr, DUCKDB_BINDINGS[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft, DUCKDB_BINDINGS[i].cb, b, NULL);
        wasm_functype_delete(ft);
    }

    *out_names = names;
    *out_funcs = funcs;
    return NUM_DUCKDB_BINDINGS;
}

void duckdb_bindings_destroy(DuckdbBindings *b) {

    /* Destroy chunk views first (some may reference result-owned chunks). */
    for (uint32_t i = 0; i < b->ht_chunk.capacity; i++) {
        ChunkWrap *cw = (ChunkWrap *)b->ht_chunk.slots[i];
        if (cw) {
            if (cw->owned) duckdb_destroy_data_chunk(&cw->chunk);
            free(cw);
        }
    }

    for (uint32_t i = 0; i < b->ht_result.capacity; i++) {
        ResultWrap *rw = (ResultWrap *)b->ht_result.slots[i];
        if (rw) rw_destroy(rw);
    }

    for (uint32_t i = 0; i < b->ht_prepared.capacity; i++) {
        duckdb_prepared_statement stmt =
            (duckdb_prepared_statement)b->ht_prepared.slots[i];
        if (stmt) duckdb_destroy_prepare(&stmt);
    }

    for (uint32_t i = 0; i < b->ht_appender.capacity; i++) {
        duckdb_appender ap = (duckdb_appender)b->ht_appender.slots[i];
        if (ap) duckdb_appender_destroy(&ap);
    }

    for (uint32_t i = 0; i < b->ht_table_desc.capacity; i++) {
        duckdb_table_description desc =
            (duckdb_table_description)b->ht_table_desc.slots[i];
        if (desc) duckdb_table_description_destroy(&desc);
    }

    for (uint32_t i = 0; i < b->ht_extracted.capacity; i++) {
        duckdb_extracted_statements ext =
            (duckdb_extracted_statements)b->ht_extracted.slots[i];
        if (ext) duckdb_destroy_extracted(&ext);
    }

    htable_destroy(&b->ht_result);
    htable_destroy(&b->ht_chunk);
    htable_destroy(&b->ht_prepared);
    htable_destroy(&b->ht_appender);
    htable_destroy(&b->ht_table_desc);
    htable_destroy(&b->ht_extracted);

    free(b->db_path);    b->db_path    = NULL;
    free(b->last_error); b->last_error = NULL;
    b->quota_bytes = 0;
}

/* ------------------------------------------------------------------ */
/*  Public quota / telemetry API                                        */
/* ------------------------------------------------------------------ */

void duckdb_bindings_set_db_path(DuckdbBindings *b, const char *path) {
    free(b->db_path);
    b->db_path = path ? strdup(path) : NULL;
}

void duckdb_bindings_set_quota(DuckdbBindings *b, uint64_t quota_bytes) {
    b->quota_bytes = quota_bytes;
}

uint64_t duckdb_bindings_get_quota(const DuckdbBindings *b) {
    return b->quota_bytes;
}

uint64_t duckdb_bindings_get_db_size(const DuckdbBindings *b) {
    return ddb_disk_usage(b);
}
