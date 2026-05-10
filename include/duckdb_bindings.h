/*
    DuckDB WASM Bindings
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
 * @file duckdb_bindings.h
 * @brief DuckDB WASM bindings for Yumi Browser.
 *
 * Exposes DuckDB database operations to guest WASM modules through
 * integer handles. The host owns the database and connection; the guest
 * manipulates results, prepared statements, appenders, and table
 * descriptions via opaque handles.
 *
 * ## Example
 *
 * @code{.c}
 * #include "duckdb_bindings.h"
 *
 * duckdb_database db;
 * duckdb_connection con;
 * duckdb_open(NULL, &db);
 * duckdb_connect(db, &con);
 *
 * DuckdbBindings bindings;
 * duckdb_bindings_init(&bindings, db, con);
 * duckdb_bindings_set_memory(&bindings, wasm_memory);
 *
 * const char **names;
 * wasm_func_t **funcs;
 * size_t n = duckdb_bindings_get_imports(&bindings, store, &names, &funcs);
 *
 * // ... instantiate WASM module with these imports ...
 *
 * duckdb_bindings_destroy(&bindings);
 * duckdb_disconnect(&con);
 * duckdb_close(&db);
 * @endcode
 */

#ifndef DUCKDB_BINDINGS_H
#define DUCKDB_BINDINGS_H

#include "deps.h"
#include "handle_table.h"
#include "deps/duckdb/src/include/duckdb.h"

/**
 * @brief DuckDB WASM binding state.
 *
 * Holds handle tables for each DuckDB object type and a reference
 * to the shared database/connection.
 */
typedef struct {
    duckdb_database   database;     /**< Host-owned database (never exposed to guest). */
    duckdb_connection  connection;   /**< Host-owned connection. */

    HandleTable ht_result;      /**< Maps handle → ResultWrap* (owns duckdb_result + cached chunks). */
    HandleTable ht_prepared;    /**< Maps handle → duckdb_prepared_statement. */
    HandleTable ht_appender;    /**< Maps handle → duckdb_appender. */
    HandleTable ht_table_desc;  /**< Maps handle → duckdb_table_description. */
    HandleTable ht_extracted;   /**< Maps handle → duckdb_extracted_statements. */
    HandleTable ht_chunk;       /**< Maps handle → ChunkWrap* (owns duckdb_data_chunk). */

    wasm_memory_t *memory;      /**< Guest WASM linear memory. */

    /* --- Disk quota + statement gating ---------------------------- */
    char     *db_path;          /**< strdup'd main database file path (NULL => no disk quota). */
    uint64_t  quota_bytes;      /**< On-disk size ceiling (main file + WAL). 0 => unlimited. */
    char     *last_error;       /**< Last sandbox/quota error (strdup'd, read via ddb_last_error). */
} DuckdbBindings;

/**
 * @brief Initialize DuckDB bindings with an existing database and connection.
 *
 * @param[out] b    Bindings to initialize.
 * @param[in]  db   Open DuckDB database.
 * @param[in]  con  Open DuckDB connection.
 */
void     duckdb_bindings_init(DuckdbBindings *b,
                               duckdb_database db,
                               duckdb_connection con);

/**
 * @brief Destroy DuckDB bindings and free all handle tables.
 * @param[in,out] b  Bindings to destroy.
 */
void     duckdb_bindings_destroy(DuckdbBindings *b);

/**
 * @brief Set the WASM linear memory reference.
 * @param[in,out] b    Bindings.
 * @param[in]     mem  Guest WASM memory.
 */
void     duckdb_bindings_set_memory(DuckdbBindings *b, wasm_memory_t *mem);

/**
 * @brief Build the import function table for WASM instantiation.
 *
 * @param[in,out] b          Bindings.
 * @param[in]     store      WASM store for function creation.
 * @param[out]    out_names  Receives array of import name strings.
 * @param[out]    out_funcs  Receives array of wasm_func_t pointers.
 * @return Number of imports.
 */
size_t   duckdb_bindings_get_imports(DuckdbBindings *b, wasm_store_t *store,
                                      const char ***out_names,
                                      wasm_func_t ***out_funcs);

/**
 * @brief Associate a database file path for on-disk quota tracking.
 *
 * The guard stats `path` and `path.wal` to measure current usage.
 * Pass NULL to disable tracking.
 *
 * @param[in,out] b     Bindings.
 * @param[in]     path  Path to the `.duckdb` file (will be copied), or NULL.
 */
void     duckdb_bindings_set_db_path(DuckdbBindings *b, const char *path);

/**
 * @brief Set or update the on-disk quota for this sandbox.
 *
 * Safe to call at any time (e.g. the dashboard may grow or shrink a
 * webapp's quota during runtime).  A value of 0 means unlimited.
 *
 * Enforcement points:
 *   - Before executing a writer statement (`INSERT`/`UPDATE`/`DELETE`/
 *     `CREATE`/`ALTER`/`DROP`/`VACUUM`/`CALL`/`COPY` etc.) the guard
 *     rejects the call if current disk usage already exceeds the quota.
 *   - Before creating or flushing an appender.
 *   - Forbidden statement types (`ATTACH`, `DETACH`, `LOAD`, `SET`,
 *     `VARIABLE_SET`, `TRANSACTION`, `EXTENSION`, `EXPORT`) are always
 *     rejected regardless of quota.
 *
 * @param[in,out] b            Bindings.
 * @param[in]     quota_bytes  Ceiling in bytes (main file + WAL). 0 => unlimited.
 */
void     duckdb_bindings_set_quota(DuckdbBindings *b, uint64_t quota_bytes);

/** @brief Read the currently configured quota (bytes). 0 => unlimited. */
uint64_t duckdb_bindings_get_quota(const DuckdbBindings *b);

/** @brief Measure current on-disk usage (main file + WAL), in bytes. */
uint64_t duckdb_bindings_get_db_size(const DuckdbBindings *b);

#endif
