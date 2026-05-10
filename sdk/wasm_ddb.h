/*
    Yumi SDK — DuckDB SQL Database WASM Imports
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/**
 * @file wasm_ddb.h
 * @brief WebAssembly guest header for host-side DuckDB SQL database bindings.
 *
 * @details
 * This header declares the host-imported functions that provide SQL database
 * access via DuckDB to WASM guest modules running inside Yumi Browser. The
 * database connection is implicit — the host owns and manages it.
 *
 * ## String Convention
 * | Direction | Format | Notes |
 * |-----------|--------|-------|
 * | Input     | `(ptr, len)` | Pointer into WASM linear memory + byte length |
 * | Output    | `(out_ptr, out_cap) → actual_len` | Call with `out_cap=0` to query length, allocate, then call again. Strings are **not** NUL-terminated unless you add it yourself. |
 *
 * ## Handle Types
 * Opaque `uint32_t` handles are returned for:
 *   - `ddb_result_h`     — query results (streamable)
 *   - `ddb_chunk_h`      — data chunks (column-batched rows)
 *   - `ddb_prepared_h`   — prepared statements
 *   - `ddb_appender_h`   — bulk appenders
 *   - `ddb_table_desc_h` — table descriptions
 *   - `ddb_extracted_h`  — extracted statements
 *
 * Pass handles back to the corresponding functions. Destroy when done.
 * Handle 0 is universally invalid.
 *
 * ## Return Conventions
 *   - **State returns**: 0 = success, nonzero = error
 *   - **Handle returns**: 0 = failure / end-of-stream, nonzero = valid handle
 *   - **Value fetches**: return the value directly as a WASM primitive
 *
 * ## Retrieval: two equivalent styles
 *
 * ### 1. Chunk/Vector style (recommended, non-deprecated)
 * Rows are grouped into fixed-size chunks.  Each chunk holds one
 * vector per column.  Iterate with `ddb_fetch_chunk` until it returns
 * 0, destroying each chunk when done.
 *
 * @code
 *   ddb_result_h r = ddb_query_cstr("SELECT id, name FROM users");
 *   if (ddb_result_has_error(r)) { ... }
 *   for (ddb_chunk_h c; (c = ddb_fetch_chunk(r)) != 0; ddb_destroy_chunk(c)) {
 *       uint32_t rows = (uint32_t)ddb_chunk_size(c);
 *       for (uint32_t row = 0; row < rows; row++) {
 *           int32_t id = ddb_chunk_get_int32(c, 0, row);
 *           char name[128];
 *           int32_t len = ddb_chunk_get_varchar(c, 1, row, name, sizeof name);
 *           if ((uint32_t)len < sizeof name) name[len] = 0;
 *       }
 *   }
 *   ddb_destroy_result(r);
 * @endcode
 *
 * ### 2. Cell style (convenience, backed by cached chunks)
 * Random access by (col, row) with total row count.  Internally forces
 * full materialization of all chunks — fine for small results.
 *
 * @code
 *   ddb_result_h r = ddb_query_cstr("SELECT name FROM users");
 *   uint32_t rows = (uint32_t)ddb_result_row_count(r);
 *   for (uint32_t row = 0; row < rows; row++) {
 *       int32_t len = ddb_value_varchar(r, 0, row, 0, 0);
 *       char *name = malloc((size_t)len + 1);
 *       ddb_value_varchar(r, 0, row, name, (uint32_t)len);
 *       name[len] = 0;
 *       free(name);
 *   }
 *   ddb_destroy_result(r);
 * @endcode
 *
 * @see https://duckdb.org/
 */

#ifndef DUCKDB_WASM_H
#define DUCKDB_WASM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  DuckDB type enum (mirrors duckdb_type)                             */
/* ------------------------------------------------------------------ */

typedef enum {
    DDB_TYPE_INVALID      = 0,
    DDB_TYPE_BOOLEAN      = 1,
    DDB_TYPE_TINYINT      = 2,
    DDB_TYPE_SMALLINT     = 3,
    DDB_TYPE_INTEGER      = 4,
    DDB_TYPE_BIGINT       = 5,
    DDB_TYPE_UTINYINT     = 6,
    DDB_TYPE_USMALLINT    = 7,
    DDB_TYPE_UINTEGER     = 8,
    DDB_TYPE_UBIGINT      = 9,
    DDB_TYPE_FLOAT        = 10,
    DDB_TYPE_DOUBLE       = 11,
    DDB_TYPE_TIMESTAMP    = 12,
    DDB_TYPE_DATE         = 13,
    DDB_TYPE_TIME         = 14,
    DDB_TYPE_INTERVAL     = 15,
    DDB_TYPE_HUGEINT      = 16,
    DDB_TYPE_UHUGEINT     = 32,
    DDB_TYPE_VARCHAR      = 17,
    DDB_TYPE_BLOB         = 18,
    DDB_TYPE_DECIMAL      = 19,
    DDB_TYPE_TIMESTAMP_S  = 20,
    DDB_TYPE_TIMESTAMP_MS = 21,
    DDB_TYPE_TIMESTAMP_NS = 22,
    DDB_TYPE_ENUM         = 23,
    DDB_TYPE_LIST         = 24,
    DDB_TYPE_STRUCT       = 25,
    DDB_TYPE_MAP          = 26,
    DDB_TYPE_ARRAY        = 33,
    DDB_TYPE_UUID         = 27,
    DDB_TYPE_UNION        = 28,
    DDB_TYPE_BIT          = 29,
    DDB_TYPE_TIME_TZ      = 30,
    DDB_TYPE_TIMESTAMP_TZ = 31,
} ddb_type_t;

/* ------------------------------------------------------------------ */
/*  Handle types (all uint32_t, zero = invalid)                        */
/* ------------------------------------------------------------------ */

typedef uint32_t ddb_result_h;
typedef uint32_t ddb_chunk_h;
typedef uint32_t ddb_prepared_h;
typedef uint32_t ddb_appender_h;
typedef uint32_t ddb_table_desc_h;
typedef uint32_t ddb_extracted_h;

/* ------------------------------------------------------------------ */
/*  Hugeint (for reading via ddb_value_hugeint)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t lower;
    int64_t  upper;
} ddb_hugeint_t;

/* ================================================================== */
/*  IMPORTS — implemented by the host                                  */
/* ================================================================== */

/* All functions are imported from the host environment.               */
/* When compiling with clang for wasm32, use:                          */
/*   __attribute__((import_module("env"), import_name("fname")))       */
/* The macros below handle this.                                       */

#ifdef __wasm__
#define DDB_IMPORT(name) \
    __attribute__((import_module("env"), import_name(#name)))
#else
#define DDB_IMPORT(name)
#endif

/* ------------------------------------------------------------------ */
/*  Query execution                                                    */
/* ------------------------------------------------------------------ */

/**
 * Execute a SQL query string.
 * @param query_ptr  Pointer to query string in linear memory.
 * @param query_len  Byte length of the query string.
 * @return           Result handle (always nonzero; check ddb_result_error).
 */
DDB_IMPORT(ddb_query)
extern ddb_result_h ddb_query(const char *query_ptr, uint32_t query_len);

/**
 * Destroy a result and free host resources.
 */
DDB_IMPORT(ddb_destroy_result)
extern void ddb_destroy_result(ddb_result_h result);

/* ------------------------------------------------------------------ */
/*  Result inspection                                                  */
/* ------------------------------------------------------------------ */

/**
 * Get the error message for a result.
 * @return Byte length of the error string (0 = no error / success).
 */
DDB_IMPORT(ddb_result_error)
extern int32_t ddb_result_error(ddb_result_h result,
                                 char *out_ptr, uint32_t out_cap);

/**
 * Number of columns in the result.
 */
DDB_IMPORT(ddb_result_column_count)
extern int32_t ddb_result_column_count(ddb_result_h result);

/**
 * Number of rows in the result.
 */
DDB_IMPORT(ddb_result_row_count)
extern int32_t ddb_result_row_count(ddb_result_h result);

/**
 * Number of rows changed by a DML statement.
 */
DDB_IMPORT(ddb_result_rows_changed)
extern int32_t ddb_result_rows_changed(ddb_result_h result);

/**
 * Get the name of a column.
 * @return Byte length of the column name.
 */
DDB_IMPORT(ddb_result_column_name)
extern int32_t ddb_result_column_name(ddb_result_h result, uint32_t col,
                                       char *out_ptr, uint32_t out_cap);

/**
 * Get the ddb_type_t of a column.
 */
DDB_IMPORT(ddb_result_column_type)
extern int32_t ddb_result_column_type(ddb_result_h result, uint32_t col);

/**
 * Get the statement type (SELECT, INSERT, etc.) for the result.
 */
DDB_IMPORT(ddb_result_statement_type)
extern int32_t ddb_result_statement_type(ddb_result_h result);

/* ------------------------------------------------------------------ */
/*  Data chunks — modern, streaming retrieval                          */
/* ------------------------------------------------------------------ */

/**
 * Fetch the next data chunk from the result, streaming-style.
 * @return Chunk handle (0 = end-of-result).
 *
 * The returned chunk is owned by the guest and must be destroyed with
 * `ddb_destroy_chunk`.  Call repeatedly until 0 is returned.
 */
DDB_IMPORT(ddb_fetch_chunk)
extern ddb_chunk_h ddb_fetch_chunk(ddb_result_h result);

/**
 * Random-access fetch of a specific chunk by index.  Forces
 * materialization of chunks up to and including `chunk_idx`.
 * @return Chunk handle (0 if index is out of bounds).
 *
 * @warning The returned chunk references data cached inside the
 *          result; it must still be destroyed with
 *          `ddb_destroy_chunk`, but the underlying rows remain live
 *          until the result itself is destroyed.
 */
DDB_IMPORT(ddb_result_get_chunk)
extern ddb_chunk_h ddb_result_get_chunk(ddb_result_h result,
                                         uint32_t chunk_idx);

/** Number of chunks currently materialized in the result. */
DDB_IMPORT(ddb_result_chunk_count)
extern int32_t ddb_result_chunk_count(ddb_result_h result);

/** Destroy a chunk handle. */
DDB_IMPORT(ddb_destroy_chunk)
extern void ddb_destroy_chunk(ddb_chunk_h chunk);

/** Number of rows in the chunk. */
DDB_IMPORT(ddb_chunk_size)
extern int32_t ddb_chunk_size(ddb_chunk_h chunk);

/** Number of columns in the chunk. */
DDB_IMPORT(ddb_chunk_column_count)
extern int32_t ddb_chunk_column_count(ddb_chunk_h chunk);

/** @return The ddb_type_t of a chunk column. */
DDB_IMPORT(ddb_chunk_column_type)
extern int32_t ddb_chunk_column_type(ddb_chunk_h chunk, uint32_t col);

/** @return 1 if the cell is NULL, 0 otherwise. */
DDB_IMPORT(ddb_chunk_is_null)
extern int32_t ddb_chunk_is_null(ddb_chunk_h chunk,
                                  uint32_t col, uint32_t row);

/* ---- Typed cell accessors (chunk-based) ---- */

DDB_IMPORT(ddb_chunk_get_bool)
extern int32_t ddb_chunk_get_bool(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_int8)
extern int32_t ddb_chunk_get_int8(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_int16)
extern int32_t ddb_chunk_get_int16(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_int32)
extern int32_t ddb_chunk_get_int32(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_int64)
extern int64_t ddb_chunk_get_int64(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_uint8)
extern int32_t ddb_chunk_get_uint8(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_uint16)
extern int32_t ddb_chunk_get_uint16(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_uint32)
extern int32_t ddb_chunk_get_uint32(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_uint64)
extern int64_t ddb_chunk_get_uint64(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_float)
extern float ddb_chunk_get_float(ddb_chunk_h c, uint32_t col, uint32_t row);
DDB_IMPORT(ddb_chunk_get_double)
extern double ddb_chunk_get_double(ddb_chunk_h c, uint32_t col, uint32_t row);

/** Days since Unix epoch. */
DDB_IMPORT(ddb_chunk_get_date)
extern int32_t ddb_chunk_get_date(ddb_chunk_h c, uint32_t col, uint32_t row);

/** Microseconds since midnight. */
DDB_IMPORT(ddb_chunk_get_time)
extern int64_t ddb_chunk_get_time(ddb_chunk_h c, uint32_t col, uint32_t row);

/** Microseconds since Unix epoch. */
DDB_IMPORT(ddb_chunk_get_timestamp)
extern int64_t ddb_chunk_get_timestamp(ddb_chunk_h c,
                                        uint32_t col, uint32_t row);

/** Copy a VARCHAR value into guest memory. @return Byte length. */
DDB_IMPORT(ddb_chunk_get_varchar)
extern int32_t ddb_chunk_get_varchar(ddb_chunk_h c,
                                      uint32_t col, uint32_t row,
                                      char *out_ptr, uint32_t out_cap);

/** Copy a BLOB value into guest memory. @return Byte length. */
DDB_IMPORT(ddb_chunk_get_blob)
extern int32_t ddb_chunk_get_blob(ddb_chunk_h c,
                                   uint32_t col, uint32_t row,
                                   void *out_ptr, uint32_t out_cap);

/** Write a HUGEINT cell (16 bytes: [lower u64, upper i64]) at out_ptr. */
DDB_IMPORT(ddb_chunk_get_hugeint)
extern void ddb_chunk_get_hugeint(ddb_chunk_h c,
                                   uint32_t col, uint32_t row,
                                   ddb_hugeint_t *out_ptr);

/* ---- Bulk vector access (efficient columnar copy) ---- */

/**
 * Copy the raw contiguous data of a primitive column vector into
 * guest memory.  The layout matches the native type for the column's
 * `ddb_type_t` (e.g. `int32_t[]` for `DDB_TYPE_INTEGER`,
 * `double[]` for `DDB_TYPE_DOUBLE`, 16-byte records for
 * `DDB_TYPE_HUGEINT`, etc.).  Not supported for VARCHAR/BLOB or
 * nested types — use the per-row accessors for those.
 *
 * @return Byte length of the vector data
 *         (`element_size * row_count`).  Returns 0 if the column
 *         type is not flat/primitive.
 */
DDB_IMPORT(ddb_chunk_copy_column)
extern int32_t ddb_chunk_copy_column(ddb_chunk_h chunk, uint32_t col,
                                      void *out_ptr, uint32_t out_cap);

/**
 * Copy the validity bitmap of a vector into guest memory.
 *
 * The bitmap is a packed `uint64_t[]` where bit N is 1 if row N is
 * valid (non-NULL) and 0 if NULL.  If the host has no validity mask
 * for this column (all values valid), the destination is filled with
 * 0xFF bytes.
 *
 * @return Byte length written (`ceil(row_count / 64) * 8`).
 */
DDB_IMPORT(ddb_chunk_copy_validity)
extern int32_t ddb_chunk_copy_validity(ddb_chunk_h chunk, uint32_t col,
                                        void *out_ptr, uint32_t out_cap);

/* ------------------------------------------------------------------ */
/*  Cell-style accessors (convenience; backed by cached chunks)        */
/* ------------------------------------------------------------------ */

/** Returns 1 if the cell is NULL, 0 otherwise. */
DDB_IMPORT(ddb_value_is_null)
extern int32_t ddb_value_is_null(ddb_result_h result,
                                  uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_boolean)
extern int32_t ddb_value_boolean(ddb_result_h result,
                                  uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_int8)
extern int32_t ddb_value_int8(ddb_result_h result,
                               uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_int16)
extern int32_t ddb_value_int16(ddb_result_h result,
                                uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_int32)
extern int32_t ddb_value_int32(ddb_result_h result,
                                uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_int64)
extern int64_t ddb_value_int64(ddb_result_h result,
                                uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_uint8)
extern int32_t ddb_value_uint8(ddb_result_h result,
                                uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_uint16)
extern int32_t ddb_value_uint16(ddb_result_h result,
                                 uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_uint32)
extern int32_t ddb_value_uint32(ddb_result_h result,
                                 uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_uint64)
extern int64_t ddb_value_uint64(ddb_result_h result,
                                 uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_float)
extern float ddb_value_float(ddb_result_h result,
                              uint32_t col, uint32_t row);

DDB_IMPORT(ddb_value_double)
extern double ddb_value_double(ddb_result_h result,
                                uint32_t col, uint32_t row);

/** Returns days since Unix epoch. */
DDB_IMPORT(ddb_value_date)
extern int32_t ddb_value_date(ddb_result_h result,
                               uint32_t col, uint32_t row);

/** Returns microseconds since midnight. */
DDB_IMPORT(ddb_value_time)
extern int64_t ddb_value_time(ddb_result_h result,
                               uint32_t col, uint32_t row);

/** Returns microseconds since Unix epoch. */
DDB_IMPORT(ddb_value_timestamp)
extern int64_t ddb_value_timestamp(ddb_result_h result,
                                    uint32_t col, uint32_t row);

/**
 * Get any value as a string (universal stringifier).
 * @return Byte length of the string.
 */
DDB_IMPORT(ddb_value_varchar)
extern int32_t ddb_value_varchar(ddb_result_h result,
                                  uint32_t col, uint32_t row,
                                  char *out_ptr, uint32_t out_cap);

/**
 * Get a BLOB value.
 * @return Byte length of the blob.
 */
DDB_IMPORT(ddb_value_blob)
extern int32_t ddb_value_blob(ddb_result_h result,
                               uint32_t col, uint32_t row,
                               void *out_ptr, uint32_t out_cap);

/**
 * Get a HUGEINT value.
 * Writes 16 bytes at out_ptr: [lower: uint64, upper: int64].
 */
DDB_IMPORT(ddb_value_hugeint)
extern void ddb_value_hugeint(ddb_result_h result,
                               uint32_t col, uint32_t row,
                               ddb_hugeint_t *out_ptr);

/* ------------------------------------------------------------------ */
/*  Prepared statements                                                */
/* ------------------------------------------------------------------ */

/**
 * Prepare a SQL statement.
 * @return Prepared handle (always nonzero; check ddb_prepare_error).
 */
DDB_IMPORT(ddb_prepare)
extern ddb_prepared_h ddb_prepare(const char *query_ptr, uint32_t query_len);

DDB_IMPORT(ddb_destroy_prepare)
extern void ddb_destroy_prepare(ddb_prepared_h stmt);

/**
 * Get the error message for a prepared statement.
 * @return Byte length of the error string (0 = no error).
 */
DDB_IMPORT(ddb_prepare_error)
extern int32_t ddb_prepare_error(ddb_prepared_h stmt,
                                  char *out_ptr, uint32_t out_cap);

/** Number of parameters in the prepared statement. */
DDB_IMPORT(ddb_nparams)
extern int32_t ddb_nparams(ddb_prepared_h stmt);

/** Get the ddb_type_t of a parameter. */
DDB_IMPORT(ddb_param_type)
extern int32_t ddb_param_type(ddb_prepared_h stmt, uint32_t param_idx);

/** Clear all parameter bindings. Returns 0 on success. */
DDB_IMPORT(ddb_clear_bindings)
extern int32_t ddb_clear_bindings(ddb_prepared_h stmt);

/* ---- Bind functions (all return 0 on success) ---- */

DDB_IMPORT(ddb_bind_boolean)
extern int32_t ddb_bind_boolean(ddb_prepared_h stmt,
                                 uint32_t idx, int32_t val);

DDB_IMPORT(ddb_bind_int8)
extern int32_t ddb_bind_int8(ddb_prepared_h stmt,
                              uint32_t idx, int32_t val);

DDB_IMPORT(ddb_bind_int16)
extern int32_t ddb_bind_int16(ddb_prepared_h stmt,
                               uint32_t idx, int32_t val);

DDB_IMPORT(ddb_bind_int32)
extern int32_t ddb_bind_int32(ddb_prepared_h stmt,
                               uint32_t idx, int32_t val);

DDB_IMPORT(ddb_bind_int64)
extern int32_t ddb_bind_int64(ddb_prepared_h stmt,
                               uint32_t idx, int64_t val);

DDB_IMPORT(ddb_bind_uint8)
extern int32_t ddb_bind_uint8(ddb_prepared_h stmt,
                               uint32_t idx, int32_t val);

DDB_IMPORT(ddb_bind_uint16)
extern int32_t ddb_bind_uint16(ddb_prepared_h stmt,
                                uint32_t idx, int32_t val);

DDB_IMPORT(ddb_bind_uint32)
extern int32_t ddb_bind_uint32(ddb_prepared_h stmt,
                                uint32_t idx, int32_t val);

DDB_IMPORT(ddb_bind_uint64)
extern int32_t ddb_bind_uint64(ddb_prepared_h stmt,
                                uint32_t idx, int64_t val);

DDB_IMPORT(ddb_bind_float)
extern int32_t ddb_bind_float(ddb_prepared_h stmt,
                               uint32_t idx, float val);

DDB_IMPORT(ddb_bind_double)
extern int32_t ddb_bind_double(ddb_prepared_h stmt,
                                uint32_t idx, double val);

/**
 * Bind a string parameter.
 * @param ptr  Pointer to string bytes in linear memory.
 * @param len  Byte length of the string.
 */
DDB_IMPORT(ddb_bind_varchar)
extern int32_t ddb_bind_varchar(ddb_prepared_h stmt,
                                 uint32_t idx,
                                 const char *ptr, uint32_t len);

/**
 * Bind a blob parameter.
 * @param ptr  Pointer to blob bytes in linear memory.
 * @param len  Byte length of the blob.
 */
DDB_IMPORT(ddb_bind_blob)
extern int32_t ddb_bind_blob(ddb_prepared_h stmt,
                              uint32_t idx,
                              const void *ptr, uint32_t len);

DDB_IMPORT(ddb_bind_null)
extern int32_t ddb_bind_null(ddb_prepared_h stmt, uint32_t idx);

/** Bind a date as days since Unix epoch. */
DDB_IMPORT(ddb_bind_date)
extern int32_t ddb_bind_date(ddb_prepared_h stmt,
                              uint32_t idx, int32_t days);

/** Bind a time as microseconds since midnight. */
DDB_IMPORT(ddb_bind_time)
extern int32_t ddb_bind_time(ddb_prepared_h stmt,
                              uint32_t idx, int64_t micros);

/** Bind a timestamp as microseconds since Unix epoch. */
DDB_IMPORT(ddb_bind_timestamp)
extern int32_t ddb_bind_timestamp(ddb_prepared_h stmt,
                                   uint32_t idx, int64_t micros);

/**
 * Execute a prepared statement.
 * @return Result handle (check ddb_result_error).
 */
DDB_IMPORT(ddb_execute_prepared)
extern ddb_result_h ddb_execute_prepared(ddb_prepared_h stmt);

/* ------------------------------------------------------------------ */
/*  Appender — fast bulk insert                                        */
/* ------------------------------------------------------------------ */

/**
 * Create an appender for a table.
 * @param schema_ptr  Pointer to schema name (or 0/NULL for default).
 * @param schema_len  Byte length of schema name (0 for default).
 * @param table_ptr   Pointer to table name.
 * @param table_len   Byte length of table name.
 * @return            Appender handle (0 = failure).
 */
DDB_IMPORT(ddb_appender_create)
extern ddb_appender_h ddb_appender_create(const char *schema_ptr,
                                           uint32_t schema_len,
                                           const char *table_ptr,
                                           uint32_t table_len);

DDB_IMPORT(ddb_appender_destroy)
extern void ddb_appender_destroy(ddb_appender_h appender);

DDB_IMPORT(ddb_appender_error)
extern int32_t ddb_appender_error(ddb_appender_h appender,
                                   char *out_ptr, uint32_t out_cap);

/** Flush pending rows to storage. Returns 0 on success. */
DDB_IMPORT(ddb_appender_flush)
extern int32_t ddb_appender_flush(ddb_appender_h appender);

/** Close the appender (flushes remaining data). Returns 0 on success. */
DDB_IMPORT(ddb_appender_close)
extern int32_t ddb_appender_close(ddb_appender_h appender);

DDB_IMPORT(ddb_appender_begin_row)
extern int32_t ddb_appender_begin_row(ddb_appender_h appender);

DDB_IMPORT(ddb_appender_end_row)
extern int32_t ddb_appender_end_row(ddb_appender_h appender);

/* ---- Append typed values (all return 0 on success) ---- */

DDB_IMPORT(ddb_append_boolean)
extern int32_t ddb_append_boolean(ddb_appender_h appender, int32_t val);

DDB_IMPORT(ddb_append_int8)
extern int32_t ddb_append_int8(ddb_appender_h appender, int32_t val);

DDB_IMPORT(ddb_append_int16)
extern int32_t ddb_append_int16(ddb_appender_h appender, int32_t val);

DDB_IMPORT(ddb_append_int32)
extern int32_t ddb_append_int32(ddb_appender_h appender, int32_t val);

DDB_IMPORT(ddb_append_int64)
extern int32_t ddb_append_int64(ddb_appender_h appender, int64_t val);

DDB_IMPORT(ddb_append_uint8)
extern int32_t ddb_append_uint8(ddb_appender_h appender, int32_t val);

DDB_IMPORT(ddb_append_uint16)
extern int32_t ddb_append_uint16(ddb_appender_h appender, int32_t val);

DDB_IMPORT(ddb_append_uint32)
extern int32_t ddb_append_uint32(ddb_appender_h appender, int32_t val);

DDB_IMPORT(ddb_append_uint64)
extern int32_t ddb_append_uint64(ddb_appender_h appender, int64_t val);

DDB_IMPORT(ddb_append_float)
extern int32_t ddb_append_float(ddb_appender_h appender, float val);

DDB_IMPORT(ddb_append_double)
extern int32_t ddb_append_double(ddb_appender_h appender, double val);

DDB_IMPORT(ddb_append_varchar)
extern int32_t ddb_append_varchar(ddb_appender_h appender,
                                   const char *ptr, uint32_t len);

DDB_IMPORT(ddb_append_blob)
extern int32_t ddb_append_blob(ddb_appender_h appender,
                                const void *ptr, uint32_t len);

DDB_IMPORT(ddb_append_null)
extern int32_t ddb_append_null(ddb_appender_h appender);

/** Append a date as days since Unix epoch. */
DDB_IMPORT(ddb_append_date)
extern int32_t ddb_append_date(ddb_appender_h appender, int32_t days);

/** Append a time as microseconds since midnight. */
DDB_IMPORT(ddb_append_time)
extern int32_t ddb_append_time(ddb_appender_h appender, int64_t micros);

/** Append a timestamp as microseconds since Unix epoch. */
DDB_IMPORT(ddb_append_timestamp)
extern int32_t ddb_append_timestamp(ddb_appender_h appender, int64_t micros);

/* ------------------------------------------------------------------ */
/*  Table description                                                  */
/* ------------------------------------------------------------------ */

/**
 * Create a table description to inspect table metadata.
 * @return Table description handle (0 = failure).
 */
DDB_IMPORT(ddb_table_description_create)
extern ddb_table_desc_h ddb_table_description_create(
    const char *schema_ptr, uint32_t schema_len,
    const char *table_ptr,  uint32_t table_len);

DDB_IMPORT(ddb_table_description_destroy)
extern void ddb_table_description_destroy(ddb_table_desc_h desc);

DDB_IMPORT(ddb_table_description_error)
extern int32_t ddb_table_description_error(ddb_table_desc_h desc,
                                            char *out_ptr, uint32_t out_cap);

/**
 * Check if a column has a DEFAULT value.
 * @return 1 = yes, 0 = no, -1 = error.
 */
DDB_IMPORT(ddb_column_has_default)
extern int32_t ddb_column_has_default(ddb_table_desc_h desc, uint32_t col_idx);

/**
 * Get the name of a column from a table description.
 * @return Byte length of the column name.
 */
DDB_IMPORT(ddb_table_description_column_name)
extern int32_t ddb_table_description_column_name(ddb_table_desc_h desc,
                                                   uint32_t col_idx,
                                                   char *out_ptr,
                                                   uint32_t out_cap);

/* ------------------------------------------------------------------ */
/*  Extracted statements (multi-statement batches)                     */
/* ------------------------------------------------------------------ */

/**
 * Extract individual statements from a multi-statement query string.
 * @return Extracted handle (0 = failure; check ddb_extract_statements_error).
 */
DDB_IMPORT(ddb_extract_statements)
extern ddb_extracted_h ddb_extract_statements(const char *query_ptr,
                                               uint32_t query_len);

DDB_IMPORT(ddb_destroy_extracted)
extern void ddb_destroy_extracted(ddb_extracted_h ext);

DDB_IMPORT(ddb_extract_statements_error)
extern int32_t ddb_extract_statements_error(ddb_extracted_h ext,
                                             char *out_ptr, uint32_t out_cap);

/**
 * Prepare one extracted statement by index.
 * @return Prepared handle (0 = failure).
 */
DDB_IMPORT(ddb_prepare_extracted)
extern ddb_prepared_h ddb_prepare_extracted(ddb_extracted_h ext,
                                             uint32_t index);

/* ------------------------------------------------------------------ */
/*  Sandbox / quota telemetry                                          */
/* ------------------------------------------------------------------ */
/*
 * The host enforces an on-disk byte ceiling on the per-webapp
 * database file (main + WAL).  The ceiling is supplied by the
 * dashboard and may change at any time.
 *
 * The host also blocks structurally unsafe statement types:
 * `ATTACH`, `DETACH`, `LOAD`, `SET`, `VARIABLE_SET`, `TRANSACTION`,
 * `EXTENSION`, `EXPORT`, `COPY`.  Such calls return a result whose
 * `ddb_result_error` string begins with "Yumi sandbox: ..." and whose
 * row/column count are zero.
 *
 * When a writer statement would cross the quota (or an appender is
 * flushed past it) the call is rejected similarly.  The webapp should:
 *   1. Call ddb_last_error() to obtain a human-readable message.
 *   2. Delete unneeded rows (DELETE / DROP) — these are allowed even
 *      past the quota boundary since they shrink the database.
 *   3. If still insufficient, surface a warning to the user prompting
 *      them to request a larger quota via the dashboard UI.
 */

/**
 * Read + clear the last sandbox/quota error message.
 * @param out_ptr  Output buffer (may be NULL to query length only).
 * @param out_cap  Buffer capacity.
 * @return Byte length of the message (0 when no error pending).
 */
DDB_IMPORT(ddb_last_error)
extern int32_t ddb_last_error(char *out_ptr, uint32_t out_cap);

/** Bytes currently used by the database file + WAL on host disk. */
DDB_IMPORT(ddb_db_size)
extern int64_t ddb_db_size(void);

/** Configured disk-size ceiling (bytes).  0 => unlimited. */
DDB_IMPORT(ddb_db_quota)
extern int64_t ddb_db_quota(void);

/* ------------------------------------------------------------------ */
/*  Inline convenience helpers (guest-side only)                       */
/* ------------------------------------------------------------------ */

#include <string.h>

/** Query with a C string literal (NUL-terminated). */
static inline ddb_result_h ddb_query_cstr(const char *sql) {
    return ddb_query(sql, (uint32_t)strlen(sql));
}

/** Prepare with a C string literal. */
static inline ddb_prepared_h ddb_prepare_cstr(const char *sql) {
    return ddb_prepare(sql, (uint32_t)strlen(sql));
}

/**
 * Read a varchar cell into a stack buffer. Returns the byte length.
 * If the value is longer than buf_cap, only buf_cap bytes are written.
 *
 * Example:
 *   char buf[256];
 *   int32_t len = ddb_read_varchar(result, col, row, buf, sizeof(buf));
 *   buf[len < 256 ? len : 255] = '\0';
 */
static inline int32_t ddb_read_varchar(ddb_result_h result,
                                        uint32_t col, uint32_t row,
                                        char *buf, uint32_t buf_cap) {
    return ddb_value_varchar(result, col, row, buf, buf_cap);
}

/**
 * Read a hugeint cell into a ddb_hugeint_t struct.
 */
static inline ddb_hugeint_t ddb_read_hugeint(ddb_result_h result,
                                              uint32_t col, uint32_t row) {
    ddb_hugeint_t hi = {0, 0};
    ddb_value_hugeint(result, col, row, &hi);
    return hi;
}

/**
 * Check if a result has an error.
 * Returns 1 if there is an error, 0 if success.
 */
static inline int32_t ddb_result_has_error(ddb_result_h result) {
    return ddb_result_error(result, 0, 0) > 0 ? 1 : 0;
}

/**
 * Check if a prepared statement has an error.
 */
static inline int32_t ddb_prepare_has_error(ddb_prepared_h stmt) {
    return ddb_prepare_error(stmt, 0, 0) > 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Chunk-style convenience wrappers                                   */
/* ------------------------------------------------------------------ */

/**
 * Read a varchar cell from a chunk into a stack buffer.
 * Returns the byte length of the value (may exceed buf_cap — the
 * value is then truncated to buf_cap bytes in the buffer).
 */
static inline int32_t ddb_chunk_read_varchar(ddb_chunk_h chunk,
                                              uint32_t col, uint32_t row,
                                              char *buf, uint32_t buf_cap) {
    return ddb_chunk_get_varchar(chunk, col, row, buf, buf_cap);
}

/** Read a blob cell from a chunk into a stack buffer. */
static inline int32_t ddb_chunk_read_blob(ddb_chunk_h chunk,
                                           uint32_t col, uint32_t row,
                                           void *buf, uint32_t buf_cap) {
    return ddb_chunk_get_blob(chunk, col, row, buf, buf_cap);
}

/** Read a hugeint cell from a chunk. */
static inline ddb_hugeint_t ddb_chunk_read_hugeint(ddb_chunk_h chunk,
                                                    uint32_t col,
                                                    uint32_t row) {
    ddb_hugeint_t hi = {0, 0};
    ddb_chunk_get_hugeint(chunk, col, row, &hi);
    return hi;
}

/**
 * Iterate every chunk of a result, invoking `fn(chunk, rows, user)`
 * for each one.  Each chunk is destroyed automatically after `fn`
 * returns.  Stops early if `fn` returns nonzero.
 *
 * Returns the last value returned by `fn` (0 = iteration completed).
 *
 * Example:
 *   static int32_t print_row(ddb_chunk_h c, uint32_t rows, void *u) {
 *       for (uint32_t r = 0; r < rows; r++) {
 *           int32_t id = ddb_chunk_get_int32(c, 0, r);
 *           ...
 *       }
 *       return 0;
 *   }
 *   ddb_for_each_chunk(result, print_row, NULL);
 */
typedef int32_t (*ddb_chunk_fn_t)(ddb_chunk_h chunk,
                                    uint32_t rows, void *user);

static inline int32_t ddb_for_each_chunk(ddb_result_h result,
                                          ddb_chunk_fn_t fn,
                                          void *user) {
    int32_t rc = 0;
    ddb_chunk_h c;
    while ((c = ddb_fetch_chunk(result)) != 0) {
        rc = fn(c, (uint32_t)ddb_chunk_size(c), user);
        ddb_destroy_chunk(c);
        if (rc != 0) return rc;
    }
    return rc;
}

/**
 * RAII-ish scoped chunk iteration using for-loop syntax.
 *
 *   DDB_FOREACH_CHUNK(c, rows, result) {
 *       for (uint32_t r = 0; r < rows; r++) {
 *           int32_t x = ddb_chunk_get_int32(c, 0, r);
 *           ...
 *       }
 *   }
 *
 * The chunk handle is destroyed automatically at loop exit or
 * continuation.
 */
#define DDB_FOREACH_CHUNK(chunk_var, rows_var, result_h_expr)               \
    for (ddb_chunk_h chunk_var = ddb_fetch_chunk(result_h_expr);            \
         chunk_var != 0;                                                    \
         ddb_destroy_chunk(chunk_var),                                      \
             chunk_var = ddb_fetch_chunk(result_h_expr))                    \
        for (uint32_t rows_var = (uint32_t)ddb_chunk_size(chunk_var),       \
                      _ddb_once = 1;                                        \
             _ddb_once;                                                     \
             _ddb_once = 0)

#ifdef __cplusplus
}
#endif

#endif /* DUCKDB_WASM_H */
