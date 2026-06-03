/*
 * db.c - Group Registrar database backbone: schema initialization, header load/save, DB helpers, and prepared-statement cache atop DuckDB.
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

/**
 * @file db.c
 * @brief Schema initialization, header persistence, DB helpers, and
 *        prepared statement cache for the Group Registrar.
 *
 * This file is the database backbone of the registrar. Every other .c
 * file in group_registrar/ depends on the helpers and schema defined here.
 *
 * @section responsibilities Responsibilities
 *
 * 1. **DB helpers** — thin wrappers around DuckDB's C API for executing
 *    SQL, extracting BLOB columns, and extracting VARCHAR columns.
 *
 * 2. **Schema initialization** — creates all eight tables if they don't
 *    already exist. Idempotent via IF NOT EXISTS.
 *
 * 3. **Header load/save** — the gr_header table is a single-row table
 *    that stores the registrar envelope. gr_header_load() populates
 *    reg->header from the DB; gr_header_save() writes it back using
 *    DELETE + INSERT.
 *
 * 4. **Prepared statement cache** — prepares all SQL statements used
 *    across the registrar and stores them on the gr_registrar_t handle.
 *
 * @section column_ordering Column Ordering Contract
 *
 * The registrar uses explicit column lists in all SELECT queries,
 * which means column values are extracted by **positional index**
 * matching the column order in the SQL statement.
 *
 * **If you add, remove, or reorder columns in any CREATE TABLE
 * statement, you must update every extraction site that reads from
 * that table.**
 *
 * @section header_persistence Header Persistence Strategy
 *
 * The gr_header table always contains exactly zero or one row.
 * gr_header_save() implements upsert as DELETE + INSERT.
 *
 * Two code paths:
 * - **Cached path** (stmts_ready == true): uses ps_header_save.
 * - **Fallback path** (stmts_ready == false): ad-hoc prepare/destroy
 *   used only during early init before statements are cached.
 */

#include "internal.h"
#include <inttypes.h>

/* ═══════════════════════════════════════════════════════════════════
 *  DB helpers
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Execute a raw SQL string and discard the result.
 */
bool gr_db_exec(duckdb_connection con, const char *sql) {
  duckdb_result result;
  duckdb_state st = duckdb_query(con, sql, &result);
  duckdb_destroy_result(&result);
  return st == DuckDBSuccess;
}

/* ------------------------------------------------------------------------- *
 *  Chunk/vector accessors (no deprecated value API).
 *
 *  NOTE: In DuckDB's chunk/vector ABI, BLOB columns are laid out as
 *        duckdb_string_t (same as VARCHAR), NOT duckdb_blob.
 *        duckdb_blob is only produced by duckdb_value_blob() (deprecated).
 *
 *  Consumers must:
 *    1. Call duckdb_fetch_chunk() on the result until it returns NULL.
 *    2. For each chunk, obtain column vectors via duckdb_data_chunk_get_vector
 *       and extract values via the gr_db_vec_get_* helpers below.
 *    3. Destroy each chunk with duckdb_destroy_data_chunk().
 * ------------------------------------------------------------------------- */

gr_error_t gr_db_vec_valid(duckdb_vector vec, idx_t row) {
  gr_error_t ret = GR_OK;
  uint64_t *validity = duckdb_vector_get_validity(vec);
  /* NULL validity => all rows are valid. */
  if (validity != NULL && !duckdb_validity_row_is_valid(validity, row))
    ret = GR_ERR_DB;
  return ret;
}

gr_error_t gr_db_vec_get_blob(duckdb_vector vec, idx_t row, uint8_t *out,
                              idx_t expected_len) {
  gr_error_t ret = gr_db_vec_valid(vec, row);
  if (ret == GR_OK) {
    duckdb_string_t *arr = (duckdb_string_t *)duckdb_vector_get_data(vec);
    const idx_t len = (idx_t)duckdb_string_t_length(arr[row]);
    if (len != expected_len) {
      ret = GR_ERR_DB;
    } else {
      memcpy(out, duckdb_string_t_data(&arr[row]), expected_len);
    }
  }
  return ret;
}

gr_error_t gr_db_vec_get_str(duckdb_vector vec, idx_t row, char *out,
                             idx_t buf_size) {
  if (buf_size == 0) return GR_ERR_INVALID_PARAM;
  gr_error_t ret = gr_db_vec_valid(vec, row);
  bool isNull = false;
  if (ret != GR_OK) {
    /* NULL => empty string (common for nullable VARCHAR columns). */
    out[0] = '\0';
    ret = GR_OK;
    isNull = true;
  }
  if (isNull == false)
  {
    duckdb_string_t *arr = (duckdb_string_t *)duckdb_vector_get_data(vec);
    const idx_t len = (idx_t)duckdb_string_t_length(arr[row]);
    if (len < buf_size)
    {
      memcpy(out, duckdb_string_t_data(&arr[row]), len);
      out[len] = '\0';
    }
    else
    {
      ret = GR_ERR_DB;
    }
  }
  return ret;
}

gr_error_t gr_db_vec_get_i32(duckdb_vector vec, idx_t row, int32_t *out) {
  gr_error_t ret = gr_db_vec_valid(vec, row);
  if (ret == GR_OK)
    *out = ((const int32_t *)duckdb_vector_get_data(vec))[row];
  return ret;
}

gr_error_t gr_db_vec_get_i64(duckdb_vector vec, idx_t row, int64_t *out) {
  gr_error_t ret = gr_db_vec_valid(vec, row);
  if (ret == GR_OK)
    *out = ((const int64_t *)duckdb_vector_get_data(vec))[row];
  return ret;
}

gr_error_t gr_db_vec_get_bool(duckdb_vector vec, idx_t row, bool *out) {
  gr_error_t ret = gr_db_vec_valid(vec, row);
  if (ret == GR_OK)
    *out = ((const bool *)duckdb_vector_get_data(vec))[row];
  return ret;
}

gr_error_t gr_db_vec_get_double(duckdb_vector vec, idx_t row, double *out) {
  gr_error_t ret = gr_db_vec_valid(vec, row);
  if (ret == GR_OK)
    *out = ((const double *)duckdb_vector_get_data(vec))[row];
  return ret;
}

/* Variable-length blob / VARCHAR extractor.
 * Allocates a new buffer with malloc() and copies the payload into it.
 * Caller takes ownership and must release via gr_free(). */
gr_error_t gr_db_vec_get_blob_alloc(duckdb_vector vec, idx_t row,
                                    uint8_t **out_buf, size_t *out_len) {
  gr_error_t ret = gr_db_vec_valid(vec, row);
  if (ret == GR_OK)
  {
    duckdb_string_t *arr = (duckdb_string_t *)duckdb_vector_get_data(vec);
    const size_t len = (size_t)duckdb_string_t_length(arr[row]);
    uint8_t *buf = NULL;
    if (len > 0) {
      buf = (uint8_t *)malloc(len);
      if (buf == NULL) {
        ret = GR_ERR_OUT_OF_MEMORY;
      }
      else
      {
        memcpy(buf, duckdb_string_t_data(&arr[row]), len);
      }
    }
    if (ret == GR_OK)
    {
      *out_buf = buf;
      *out_len = len;
    }
  }
  return ret;
}

/* Fetch the first non-empty chunk from a result.  Returns GR_ERR_NOT_FOUND
 * when the result is empty.  Caller must destroy the returned chunk with
 * duckdb_destroy_data_chunk(). */
gr_error_t gr_db_fetch_first_chunk(duckdb_result *result,
                                   duckdb_data_chunk *out_chunk) {
  gr_error_t ret = GR_OK;
  duckdb_data_chunk chunk = duckdb_fetch_chunk(*result);
  while (chunk != NULL && duckdb_data_chunk_get_size(chunk) == 0) {
    duckdb_destroy_data_chunk(&chunk);
    chunk = duckdb_fetch_chunk(*result);
  }
  if (chunk == NULL) {
    *out_chunk = NULL;
    ret = GR_ERR_NOT_FOUND;
  }
  if (ret == GR_OK)
  {
    *out_chunk = chunk;
  }
  return ret;
}

/* Scalar fetch helpers: read the single value at (col 0, row 0).
 * Returns GR_ERR_NOT_FOUND when the result is empty. */
gr_error_t gr_db_fetch_i64_scalar(duckdb_result *result, int64_t *out) {
  duckdb_data_chunk chunk = NULL;
  gr_error_t err = gr_db_fetch_first_chunk(result, &chunk);
  if (err == GR_OK)
  {
    err = gr_db_vec_get_i64(duckdb_data_chunk_get_vector(chunk, 0), 0, out);
    duckdb_destroy_data_chunk(&chunk);
  }
  return err;
}

gr_error_t gr_db_fetch_i32_scalar(duckdb_result *result, int32_t *out) {
  duckdb_data_chunk chunk = NULL;
  gr_error_t err = gr_db_fetch_first_chunk(result, &chunk);
  if (err == GR_OK)
  {
    err = gr_db_vec_get_i32(duckdb_data_chunk_get_vector(chunk, 0), 0, out);
    duckdb_destroy_data_chunk(&chunk);
  }
  return err;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Nestable Transaction Helpers
 *
 *  DuckDB does not support nested BEGIN TRANSACTION.  These helpers
 *  use a depth counter so that only the outermost begin/commit pair
 *  actually issues SQL.  Every public mutating function wraps its
 *  mutation + audit_append in gr_txn_begin/commit; gr_header_save
 *  also calls them, but the inner calls are no-ops when an outer
 *  transaction is already active.
 * ═══════════════════════════════════════════════════════════════════ */

bool gr_txn_begin(gr_registrar_t *reg) {
  bool ret = false;
  if (reg != NULL) {
    if (reg->txn_depth == 0) {
      if (gr_db_exec(reg->con, "BEGIN TRANSACTION") == true) {
        ret = true;
        reg->txn_saved_header = reg->header;
      }
    }
    reg->txn_depth++;
    ret = true;
  }
  return ret;
}

bool gr_txn_commit(gr_registrar_t *reg) {
  bool ret = false;
  if (reg->txn_depth > 0) {
    ret = true;
    reg->txn_depth--;
    if (reg->txn_depth == 0)
      ret = gr_db_exec(reg->con, "COMMIT");
  }
  return ret;
}

void gr_txn_rollback(gr_registrar_t *reg) {
  if (reg->txn_depth > 0) {
    gr_db_exec(reg->con, "ROLLBACK");
    reg->header = reg->txn_saved_header;
    reg->txn_depth = 0;
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Schema Initialization
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Validate column ordering for a single table.
 *
 * Uses PRAGMA table_info to verify column names appear in the
 * expected positional order. Fails fast on mismatch instead of
 * allowing silent data corruption from positional extraction.
 */
static bool validate_table_columns(duckdb_connection con, size_t table_name_len,
                                   const char *table_name,
                                   const char *const *expected_cols,
                                   idx_t expected_count) {
  bool ret = true;
  bool executed = false;
  duckdb_prepared_statement tableInfo = NULL;
  duckdb_result result = {0};

  /* Callers pass sizeof("literal") which includes the terminating NUL.
   * Strip it so the bound parameter matches the actual table name. */
  if ((table_name_len > 0U) && (table_name[table_name_len - 1U] == '\0'))
    table_name_len -= 1U;

  /* Use the pragma_table_info() table function form — DuckDB's PRAGMA
   * statement does not support parameter binding, but the table
   * function does. */
  if (duckdb_prepare(con,
                     "SELECT name FROM pragma_table_info($1) ORDER BY cid",
                     &tableInfo) !=
      DuckDBSuccess) {
    ret = false;
  }

  if ((ret == true) &&
      (duckdb_bind_varchar_length(tableInfo, 1U, table_name,
                                  (idx_t)table_name_len) != DuckDBSuccess)) {
    ret = false;
  }

  if ((ret == true) &&
      (duckdb_execute_prepared(tableInfo, &result) == DuckDBSuccess)) {
    executed = true;
  } else {
    ret = false;
  }

  if (ret == true) {
    bool valid = true;
    idx_t rowIdx = 0U;
    duckdb_data_chunk chunk = duckdb_fetch_chunk(result);

    while ((chunk != NULL) && (valid == true)) {
      idx_t row_count = duckdb_data_chunk_get_size(chunk);
      duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, 0U);
      uint64_t *validity = duckdb_vector_get_validity(vec);
      duckdb_string_t *data = (duckdb_string_t *)duckdb_vector_get_data(vec);

      for (idx_t row = 0U; (row < row_count) && (valid == true); ++row) {
        bool is_valid = (validity == NULL) ||
                        (duckdb_validity_row_is_valid(validity, row) == true);

        if (is_valid == false) {
          valid = false;
        } else {
          const char *name = duckdb_string_t_data(&data[row]);
          uint32_t nameLen = duckdb_string_t_length(data[row]);
          size_t expLen = strlen(expected_cols[rowIdx + row]);

          if (((size_t)nameLen != expLen) ||
              (strncmp(name, expected_cols[rowIdx + row], expLen) != 0)) {
            valid = false;
          }
        }
      }

      rowIdx += row_count;
      duckdb_destroy_data_chunk(&chunk);
      chunk = duckdb_fetch_chunk(result);
    }

    if (rowIdx != expected_count) {
      valid = false;
    }

    ret = valid;
  }

  if (executed == true) {
    duckdb_destroy_result(&result);
  }
  if (tableInfo != NULL) {
    duckdb_destroy_prepare(&tableInfo);
  }

  return ret;
}

/**
 * @brief Run pending schema migrations.
 *
 * Creates the gr_schema_meta table and ensures it contains exactly one
 * row reflecting the current schema version (as reported by
 * gr_schema_version()). No actual migration steps are required yet,
 * but the version row is maintained so downstream code can detect
 * future on-disk format changes.
 */
static bool gr_db_migrate(duckdb_connection con) {
  bool ret = gr_db_exec(con, "CREATE TABLE IF NOT EXISTS gr_schema_meta ("
                             "  id INTEGER PRIMARY KEY DEFAULT 1,"
                             "  schema_version UBIGINT NOT NULL"
                             ")");

  /* Version 2→3: add per-webapp permission blobs */
  if (ret == true)
    ret = gr_db_exec(con, "ALTER TABLE gr_webapps "
                          "ADD COLUMN IF NOT EXISTS perm_data BLOB");
  if (ret == true)
    ret = gr_db_exec(con, "ALTER TABLE gr_webapps "
                          "ADD COLUMN IF NOT EXISTS role_mask BLOB");

  if (ret == true) {
    duckdb_prepared_statement stmt = NULL;
    if (duckdb_prepare(con,
                       "INSERT INTO gr_schema_meta (id, schema_version) "
                       "VALUES (1, $1) "
                       "ON CONFLICT (id) DO UPDATE SET schema_version = $1",
                       &stmt) != DuckDBSuccess) {
      ret = false;
    }

    if ((ret == true) &&
        (duckdb_bind_uint64(stmt, 1U, gr_schema_version()) != DuckDBSuccess)) {
      ret = false;
    }

    if ((ret == true) &&
        (duckdb_execute_prepared(stmt, NULL) != DuckDBSuccess)) {
      ret = false;
    }

    if (stmt != NULL) {
      duckdb_destroy_prepare(&stmt);
    }
  }

  return ret;
}

/**
 * @brief Create all registrar tables if they don't exist.
 */
bool gr_db_init_schema(duckdb_connection con) {
  static const char *tables[] = {
      /* ── gr_header (16 columns) ────────────────────────────────
       *  0: group_id              BLOB     (128 bytes, Skein-1024 hash)
       *  1: group_type            INTEGER  (0=private, 1=public)
       *  2: group_name            VARCHAR  (up to 128 chars)
       *  3: version               INTEGER  (monotonic counter)
       *  4: created_at            BIGINT   (unix ms)
       *  5: updated_at            BIGINT   (unix ms)
       *  6: epoch_id              INTEGER  (current epoch)
       *  7: message_retention_ms  BIGINT   (0=forever)
       *  8: file_retention_ms     BIGINT   (0=forever)
       *  9: registrar_max_bytes   BIGINT   (default 200MB)
       * 10: owner_id              BLOB     (32 bytes, peer ID)
       * 11: owner_sign_key        BLOB     (2592 bytes, ML-DSA-87 pk)
       * 12: signer_id             BLOB     (32 bytes, last signer peer ID)
       * 13: signer_sign_key       BLOB     (2592 bytes, last signer ML-DSA-87)
       * 14: signature             BLOB     (4627 bytes, ML-DSA-87 sig)
       * 15: hash                  BLOB     (128 bytes, Skein-1024)
       * ──────────────────────────────────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_header ("
      "  group_id BLOB,"
      "  group_type INTEGER,"
      "  group_name VARCHAR(128),"
      "  version INTEGER,"
      "  created_at BIGINT,"
      "  updated_at BIGINT,"
      "  epoch_id INTEGER,"
      "  message_retention_ms BIGINT,"
      "  file_retention_ms BIGINT,"
      "  registrar_max_bytes BIGINT,"
      "  owner_id BLOB,"
      "  owner_sign_key BLOB,"
      "  signer_id BLOB,"
      "  signer_sign_key BLOB,"
      "  signature BLOB,"
      "  hash BLOB"
      ")",

      /* ── gr_peers (12 columns) ──────────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_peers ("
      "  peer_id BLOB PRIMARY KEY,"
      "  kem_pk BLOB,"
      "  sign_key BLOB,"
      "  ip VARCHAR(46),"
      "  port INTEGER,"
      "  status INTEGER,"
      "  role_id INTEGER,"
      "  joined_at BIGINT,"
      "  removed_at BIGINT,"
      "  last_seen BIGINT,"
      "  removed_reason VARCHAR(128),"
      "  removed_by BLOB"
      ")",

      /* ── gr_roles (6 columns) ───────────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_roles ("
      "  role_id INTEGER PRIMARY KEY,"
      "  name VARCHAR(128),"
      "  permissions INTEGER,"
      "  sign_key BLOB,"
      "  created_at BIGINT,"
      "  modified_at BIGINT"
      ")",

      /* ── gr_webapps (7 columns) ─────────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_webapps ("
      "  hash BLOB PRIMARY KEY,"
      "  name VARCHAR(128),"
      "  version INTEGER,"
      "  added_at BIGINT,"
      "  added_by BLOB,"
      "  perm_data BLOB,"
      "  role_mask BLOB"
      ")",

      /* ── gr_servers (8 columns) ─────────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_servers ("
      "  id_hash BLOB PRIMARY KEY,"
      "  type INTEGER,"
      "  ip VARCHAR(46),"
      "  port INTEGER,"
      "  sign_key BLOB,"
      "  service_hash BLOB,"
      "  content_kem_pk BLOB,"
      "  content_kem_sk BLOB"
      ")",

      /* ── gr_epochs (5 columns) ──────────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_epochs ("
      "  epoch_id INTEGER PRIMARY KEY,"
      "  epoch_key BLOB,"
      "  created_at BIGINT,"
      "  expired_at BIGINT,"
      "  created_by BLOB"
      ")",

      /* ── gr_audit_log (9 columns) ───────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_audit_log ("
      "  entry_hash BLOB PRIMARY KEY,"
      "  timestamp BIGINT,"
      "  change_type INTEGER,"
      "  actor_id BLOB,"
      "  target_id BLOB,"
      "  signature BLOB,"
      "  registrar_version INTEGER,"
      "  detail VARCHAR(256),"
      "  prev_hash BLOB,"
      "  timestamp_ns BIGINT"
      ")",

      /* ── gr_invites (7 columns) ─────────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_invites ("
      "  verification_token BLOB PRIMARY KEY,"
      "  created_at BIGINT,"
      "  expires_at BIGINT,"
      "  created_by BLOB,"
      "  invalidated BOOLEAN,"
      "  used BOOLEAN,"
      "  used_by BLOB"
      ")",

      /* ── gr_group_icon (10 columns) ─────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_group_icon ("
      "  id INTEGER PRIMARY KEY DEFAULT 1,"
      "  data BLOB NOT NULL,"
      "  mime_type VARCHAR(64) NOT NULL,"
      "  width INTEGER NOT NULL,"
      "  height INTEGER NOT NULL,"
      "  is_video BOOLEAN NOT NULL DEFAULT FALSE,"
      "  static_frame BLOB,"
      "  content_hash BLOB NOT NULL,"
      "  updated_at BIGINT NOT NULL,"
      "  updated_by BLOB NOT NULL"
      ")",

      /* ── gr_ip_blocklist (5 columns) ─────────────────────────── */
      "CREATE TABLE IF NOT EXISTS gr_ip_blocklist ("
      "  ip VARCHAR(46) PRIMARY KEY,"
      "  fail_count INTEGER NOT NULL DEFAULT 0,"
      "  blocked BOOLEAN NOT NULL DEFAULT FALSE,"
      "  blocked_at BIGINT NOT NULL DEFAULT 0,"
      "  last_attempt BIGINT NOT NULL DEFAULT 0"
      ")",

      NULL};

  /* ── Validate column ordering ──────────────────────────────
   *  After tables are created and migrated, verify that each
   *  table's column names appear in the positional order that
   *  every positional extraction site assumes.
   * ─────────────────────────────────────────────────────────── */
  static const char *hdr_cols[] = {"group_id",          "group_type",
                                   "group_name",        "version",
                                   "created_at",        "updated_at",
                                   "epoch_id",          "message_retention_ms",
                                   "file_retention_ms", "registrar_max_bytes",
                                   "owner_id",          "owner_sign_key",
                                   "signer_id",         "signer_sign_key",
                                   "signature",         "hash"};
  static const char *peer_cols[] = {
      "peer_id",    "kem_pk",    "sign_key",       "ip",
      "port",       "status",    "role_id",        "joined_at",
      "removed_at", "last_seen", "removed_reason", "removed_by"};
  static const char *role_cols[] = {"role_id",  "name",       "permissions",
                                    "sign_key", "created_at", "modified_at"};
  static const char *webapp_cols[] = {"hash", "name", "version", "added_at",
                                      "added_by", "perm_data", "role_mask"};
  static const char *server_cols[] = {
      "id_hash",        "type",          "ip",
      "port",           "sign_key",      "service_hash",
      "content_kem_pk", "content_kem_sk"};
  static const char *epoch_cols[] = {"epoch_id", "epoch_key", "created_at",
                                     "expired_at", "created_by"};
  static const char *audit_cols[] = {
      "entry_hash", "timestamp",   "change_type",       "actor_id",
      "target_id",  "signature",   "registrar_version", "detail",
      "prev_hash",  "timestamp_ns"};
  static const char *invite_cols[] = {
      "verification_token", "created_at", "expires_at", "created_by",
      "invalidated",        "used",       "used_by"};
  static const char *icon_cols[] = {
      "id",       "data",         "mime_type",    "width",      "height",
      "is_video", "static_frame", "content_hash", "updated_at", "updated_by"};

  bool ret = true;
  for (int i = 0; ret == true && tables[i] != NULL; i++) {
    if (gr_db_exec(con, tables[i]) == false)
      ret = false;
  }

  /* Run schema migrations before column validation so that
   * existing databases with old column names get renamed. */
  if (ret == true && gr_db_migrate(con) == false)
    ret = false;

  if (ret == true && validate_table_columns(con, sizeof("gr_header"),
                                            "gr_header", hdr_cols, 16) == false)
    ret = false;
  if (ret == true && validate_table_columns(con, sizeof("gr_peers"), "gr_peers",
                                            peer_cols, 12) == false)
    ret = false;
  if (ret == true && validate_table_columns(con, sizeof("gr_roles"), "gr_roles",
                                            role_cols, 6) == false)
    ret = false;
  if (ret == true &&
      validate_table_columns(con, sizeof("gr_webapps"), "gr_webapps",
                             webapp_cols, 7) == false)
    ret = false;
  if (ret == true &&
      validate_table_columns(con, sizeof("gr_servers"), "gr_servers",
                             server_cols, 8) == false)
    ret = false;
  if (ret == true &&
      validate_table_columns(con, sizeof("gr_epochs"), "gr_epochs", epoch_cols,
                             5) == false)
    ret = false;
  if (ret == true &&
      validate_table_columns(con, sizeof("gr_audit_log"), "gr_audit_log",
                             audit_cols, 10) == false)
    ret = false;
  if (ret == true &&
      validate_table_columns(con, sizeof("gr_invites"), "gr_invites",
                             invite_cols, 7) == false)
    ret = false;
  if (ret == true &&
      validate_table_columns(con, sizeof("gr_group_icon"), "gr_group_icon",
                             icon_cols, 10) == false)
    ret = false;

  return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Header Load / Save
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Load the registrar header from the gr_header table.
 *
 * Column order (16 columns) must match CREATE TABLE gr_header exactly.
 */
gr_error_t gr_header_load(gr_registrar_t *reg) {
  gr_error_t ret = GR_OK;
  duckdb_result result = {0};
  bool result_ok = false;

  if (reg->stmts_ready == true) {
    duckdb_prepared_statement stmt = reg->ps_header_load;
    (void)duckdb_clear_bindings(stmt);
    if (duckdb_execute_prepared(stmt, &result) != DuckDBSuccess) {
      ret = GR_ERR_DB;
    } else {
      result_ok = true;
    }
  } else {
    if (duckdb_query(
            reg->con,
            "SELECT group_id, group_type, group_name, version, "
            "created_at, updated_at, epoch_id, "
            "message_retention_ms, file_retention_ms, registrar_max_bytes, "
            "owner_id, owner_sign_key, signer_id, signer_sign_key, "
            "signature, hash "
            "FROM gr_header LIMIT 1",
            &result) != DuckDBSuccess) {
      ret = GR_ERR_DB;
    } else {
      result_ok = true;
    }
  }

  if (ret == GR_OK) {
    duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
    if (chunk == NULL) {
      ret = GR_ERR_NOT_FOUND;
    } else {
      const idx_t rows = duckdb_data_chunk_get_size(chunk);
      if (rows == 0U) {
        ret = GR_ERR_NOT_FOUND;
      } else {
        gr_header_t *h = &reg->header;
        const idx_t row = 0U;
        int32_t i32 = 0;

        /*  0: group_id           8: file_retention_ms
         *  1: group_type         9: registrar_max_bytes
         *  2: group_name        10: owner_id
         *  3: version           11: owner_sign_key
         *  4: created_at        12: signer_id
         *  5: updated_at        13: signer_sign_key
         *  6: epoch_id          14: signature
         *  7: message_retention_ms  15: hash
         */

        if (ret == GR_OK) {
          ret = gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 0U), row,
                                   h->group_id, GR_HASH_LEN);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_i32(duckdb_data_chunk_get_vector(chunk, 1U), row,
                                  &i32);
          if (ret == GR_OK) {
            h->group_type = (gr_group_type_t)i32;
          }
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_str(duckdb_data_chunk_get_vector(chunk, 2U), row,
                                  h->group_name, GR_MAX_NAME_LEN);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_i32(duckdb_data_chunk_get_vector(chunk, 3U), row,
                                  &i32);
          if (ret == GR_OK) {
            h->version = (uint32_t)i32;
          }
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_i64(duckdb_data_chunk_get_vector(chunk, 4U), row,
                                  &h->created_at);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_i64(duckdb_data_chunk_get_vector(chunk, 5U), row,
                                  &h->updated_at);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_i32(duckdb_data_chunk_get_vector(chunk, 6U), row,
                                  &i32);
          if (ret == GR_OK) {
            h->epoch_id = (uint32_t)i32;
          }
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_i64(duckdb_data_chunk_get_vector(chunk, 7U), row,
                                  &h->retention.message_retention_ms);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_i64(duckdb_data_chunk_get_vector(chunk, 8U), row,
                                  &h->retention.file_retention_ms);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_i64(duckdb_data_chunk_get_vector(chunk, 9U), row,
                                  &h->retention.registrar_max_bytes);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 10U),
                                   row, h->owner_id, GR_PEER_ID_LEN);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 11U),
                                   row, h->owner_sign_key, GR_PUBLIC_KEY_LEN);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 12U),
                                   row, h->signer_id, GR_PEER_ID_LEN);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 13U),
                                   row, h->signer_sign_key, GR_PUBLIC_KEY_LEN);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 14U),
                                   row, h->signature, GR_SIGN_LEN);
        }
        if (ret == GR_OK) {
          ret = gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 15U),
                                   row, h->hash, GR_HASH_LEN);
        }
      }
      duckdb_destroy_data_chunk(&chunk);
    }
  }

  if (result_ok == true) {
    duckdb_destroy_result(&result);
  }

  if (ret == GR_OK) {
    reg->header_dirty = false;
  }
  return ret;
}

/**
 * @brief Bind all 16 header fields to a prepared statement.
 *
 * Bind order matches CREATE TABLE gr_header column order:
 *   $1  = group_id             $9  = file_retention_ms
 *   $2  = group_type           $10 = registrar_max_bytes
 *   $3  = group_name           $11 = owner_id
 *   $4  = version              $12 = owner_sign_key
 *   $5  = created_at           $13 = signer_id
 *   $6  = updated_at           $14 = signer_sign_key
 *   $7  = epoch_id             $15 = signature
 *   $8  = message_retention_ms $16 = hash
 */
static bool bind_header_fields(duckdb_prepared_statement stmt,
                               const gr_header_t *h) {
  bool ret = true;
  if (duckdb_bind_blob(stmt, 1, h->group_id, GR_HASH_LEN) != DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_int32(stmt, 2, (int32_t)h->group_type) != DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_varchar(stmt, 3, h->group_name) != DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_int32(stmt, 4, (int32_t)h->version) != DuckDBSuccess)
    ret = false;
  if (ret == true && duckdb_bind_int64(stmt, 5, h->created_at) != DuckDBSuccess)
    ret = false;
  if (ret == true && duckdb_bind_int64(stmt, 6, h->updated_at) != DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_int32(stmt, 7, (int32_t)h->epoch_id) != DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_int64(stmt, 8, h->retention.message_retention_ms) !=
          DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_int64(stmt, 9, h->retention.file_retention_ms) !=
          DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_int64(stmt, 10, h->retention.registrar_max_bytes) !=
          DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_blob(stmt, 11, h->owner_id, GR_PEER_ID_LEN) != DuckDBSuccess)
    ret = false;
  if (ret == true && duckdb_bind_blob(stmt, 12, h->owner_sign_key,
                                      GR_PUBLIC_KEY_LEN) != DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_blob(stmt, 13, h->signer_id, GR_PEER_ID_LEN) != DuckDBSuccess)
    ret = false;
  if (ret == true && duckdb_bind_blob(stmt, 14, h->signer_sign_key,
                                      GR_PUBLIC_KEY_LEN) != DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_blob(stmt, 15, h->signature, GR_SIGN_LEN) != DuckDBSuccess)
    ret = false;
  if (ret == true &&
      duckdb_bind_blob(stmt, 16, h->hash, GR_HASH_LEN) != DuckDBSuccess)
    ret = false;

  return ret;
}

/**
 * @brief Write reg->header to the gr_header table.
 *
 * Executes DELETE FROM gr_header followed by INSERT with all 16 fields.
 * Uses the cached ps_header_save when available, otherwise falls back
 * to an ad-hoc prepare (only during early init / tests).
 */
gr_error_t gr_header_save(gr_registrar_t *reg) {
  gr_error_t ret = GR_OK;
  gr_header_t *h = &reg->header;

  if (reg->stmts_ready == true) {
    if (gr_txn_begin(reg) == false)
      ret = GR_ERR_DB;
    if ((ret == GR_OK) &&
        (gr_db_exec(reg->con, "DELETE FROM gr_header") == false)) {
      gr_txn_rollback(reg);
      ret = GR_ERR_DB;
    }

    duckdb_prepared_statement stmt = reg->ps_header_save;
    if ((ret == GR_OK) && (duckdb_clear_bindings(stmt) != DuckDBSuccess))
      ret = GR_ERR_DB;
    if ((ret == GR_OK) && (bind_header_fields(stmt, h) == false))
      ret = GR_ERR_DB;

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);

    if ((ret == GR_OK) && (st != DuckDBSuccess)) {
      gr_txn_rollback(reg);
      ret = GR_ERR_DB;
    }
    if ((ret == GR_OK) && (gr_txn_commit(reg) == false)) {
      gr_txn_rollback(reg);
      ret = GR_ERR_DB;
    }

    reg->header_dirty = false;
  } else {
    /* Fallback: ad-hoc prepare for early init / tests */
    if (gr_txn_begin(reg) == false)
      ret = GR_ERR_DB;
    if ((ret == GR_OK) && (gr_db_exec(reg->con, "DELETE FROM gr_header") == false)) {
      gr_txn_rollback(reg);
      ret = GR_ERR_DB;
    }

    duckdb_prepared_statement stmt = {0};
    bool isStmtPrepared = false;
    if ((ret == GR_OK) && (duckdb_prepare(reg->con,
                       "INSERT INTO gr_header VALUES "
                       "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,"
                       "$11,$12,$13,$14,$15,$16)",
                       &stmt) != DuckDBSuccess)) {
      gr_txn_rollback(reg);
      ret = GR_ERR_DB;
    }
    else
    {
      isStmtPrepared = true;
    }

    if ((ret == GR_OK) && bind_header_fields(stmt, h) == false)
    {
      ret = GR_ERR_DB;
    }

    duckdb_result result;
    if (ret == GR_OK &&
      (duckdb_execute_prepared(stmt, &result) != DuckDBSuccess))
    {
      duckdb_destroy_result(&result);
      gr_txn_rollback(reg);
      ret = GR_ERR_DB;
    }
    if (isStmtPrepared == true)
    {
      duckdb_destroy_prepare(&stmt);
    }

    if ((ret == GR_OK) && gr_txn_commit(reg) == false) {
      gr_txn_rollback(reg);
      ret = GR_ERR_DB;
    }

    if (ret == GR_OK)
      reg->header_dirty = false;
  }
  return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Prepared Statement Cache
 * ═══════════════════════════════════════════════════════════════════ */
/**
 * @brief Prepare all cached SQL statements.
 */
gr_error_t gr_prepare_statements(gr_registrar_t *reg) {
  gr_error_t ret = GR_OK;
  duckdb_connection c = reg->con;

  /* ── Peer ──────────────────────────────────────────────────── */
  if (duckdb_prepare(c, "SELECT peer_id, kem_pk, sign_key, ip, port, status, "
                    "role_id, joined_at, removed_at, last_seen, "
                    "removed_reason, removed_by "
                    "FROM gr_peers WHERE peer_id=$1",
                     &reg->ps_peer_get) != DuckDBSuccess)
      ret = GR_ERR_DB;

  if ((ret == GR_OK) && (duckdb_prepare(c, "SELECT COUNT(*) FROM gr_peers",
                     &reg->ps_peer_count_all) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c, "SELECT COUNT(*) FROM gr_peers WHERE status=$1",
                     &reg->ps_peer_count_status) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c, "SELECT peer_id, kem_pk, sign_key, ip, port, status, "
                         "role_id, joined_at, removed_at, last_seen, "
                         "removed_reason, removed_by "
                         "FROM gr_peers LIMIT $1",
                     &reg->ps_peer_list_all) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT peer_id, kem_pk, sign_key, ip, port, status, "
       "role_id, joined_at, removed_at, last_seen, "
       "removed_reason, removed_by "
       "FROM gr_peers WHERE status=$1 LIMIT $2",
                     &reg->ps_peer_list_status) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT INTO gr_peers VALUES "
       "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)",
                     &reg->ps_peer_insert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_peers SET status=$1, removed_at=$2, "
       "removed_reason=$3, removed_by=$4 WHERE peer_id=$5",
                     &reg->ps_peer_update_status) != DuckDBSuccess))
      ret = GR_ERR_DB;

  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_peers SET ip=$1, port=$2, last_seen=$3 "
       "WHERE peer_id=$4 AND status=0",
                     &reg->ps_peer_update_addr) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_peers SET last_seen=$1 WHERE peer_id=$2",
                     &reg->ps_peer_touch) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_peers SET role_id=$1 WHERE peer_id=$2 AND status=0",
                     &reg->ps_peer_set_role) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_peers SET role_id=0 WHERE role_id=$1",
                     &reg->ps_peer_reset_role) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Role ──────────────────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT role_id, name, permissions, sign_key, "
       "created_at, modified_at "
       "FROM gr_roles WHERE role_id=$1",
                     &reg->ps_role_get) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT role_id, name, permissions, sign_key, "
       "created_at, modified_at "
       "FROM gr_roles ORDER BY role_id LIMIT $1",
                     &reg->ps_role_list) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c, "SELECT COUNT(*) FROM gr_roles",
                     &reg->ps_role_count) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT INTO gr_roles VALUES ($1,$2,$3,$4,$5,$6)",
                     &reg->ps_role_insert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "DELETE FROM gr_roles WHERE role_id=$1",
                     &reg->ps_role_delete) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_roles SET permissions=$1, modified_at=$2 WHERE role_id=$3",
                     &reg->ps_role_set_perms) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT COALESCE(MAX(role_id), 0) + 1 FROM gr_roles",
                     &reg->ps_role_next_id) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT r.permissions FROM gr_peers p "
       "JOIN gr_roles r ON p.role_id = r.role_id "
       "WHERE p.peer_id = $1 AND p.status = 0 AND p.role_id > 0",
                     &reg->ps_role_perms_join) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── WebApp ────────────────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT INTO gr_webapps VALUES ($1,$2,$3,$4,$5,$6,$7)",
                     &reg->ps_webapp_insert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "DELETE FROM gr_webapps WHERE hash=$1",
                     &reg->ps_webapp_delete) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT COUNT(*) FROM gr_webapps WHERE hash=$1",
                     &reg->ps_webapp_check) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT hash, name, version, added_at, added_by, perm_data, role_mask "
       "FROM gr_webapps LIMIT $1",
                     &reg->ps_webapp_list) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c, "SELECT COUNT(*) FROM gr_webapps",
                     &reg->ps_webapp_count) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT hash, name, version, added_at, added_by, perm_data, role_mask "
       "FROM gr_webapps WHERE hash=$1",
                     &reg->ps_webapp_get) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_webapps SET perm_data=$2 WHERE hash=$1",
                     &reg->ps_webapp_update_perm_data) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_webapps SET role_mask=$2 WHERE hash=$1",
                     &reg->ps_webapp_update_role_mask) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Server ────────────────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT INTO gr_servers VALUES ($1,$2,$3,$4,$5,$6,$7,$8)",
                     &reg->ps_server_insert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "DELETE FROM gr_servers WHERE id_hash=$1",
                     &reg->ps_server_delete) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT id_hash, type, ip, port, sign_key, "
       "service_hash, content_kem_pk, content_kem_sk "
       "FROM gr_servers WHERE type=$1 LIMIT $2",
                     &reg->ps_server_list) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT COUNT(*) FROM gr_servers WHERE type=$1",
                     &reg->ps_server_count) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT id_hash, type, ip, port, sign_key, "
       "service_hash, content_kem_pk, content_kem_sk "
       "FROM gr_servers WHERE id_hash=$1",
                     &reg->ps_server_get) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Epoch ─────────────────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT epoch_id, epoch_key, created_at, expired_at, created_by "
       "FROM gr_epochs WHERE epoch_id=$1",
                     &reg->ps_epoch_get) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT epoch_id, epoch_key, created_at, expired_at, created_by "
       "FROM gr_epochs ORDER BY epoch_id LIMIT $1",
                     &reg->ps_epoch_list) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c, "SELECT COUNT(*) FROM gr_epochs",
                     &reg->ps_epoch_count) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT INTO gr_epochs VALUES ($1,$2,$3,$4,$5)",
                     &reg->ps_epoch_insert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_epochs SET expired_at=$1 WHERE epoch_id=$2",
                     &reg->ps_epoch_expire) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Audit ─────────────────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT OR IGNORE INTO gr_audit_log VALUES "
       "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
                     &reg->ps_audit_insert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT COUNT(*) FROM gr_audit_log WHERE entry_hash=$1",
                     &reg->ps_audit_dedup) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT entry_hash, timestamp, change_type, actor_id, target_id, "
       "signature, registrar_version, detail, prev_hash, timestamp_ns "
       "FROM gr_audit_log WHERE timestamp > $1 "
       "ORDER BY timestamp ASC LIMIT $2",
                     &reg->ps_audit_list) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c, "SELECT COUNT(*) FROM gr_audit_log",
                     &reg->ps_audit_count) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT entry_hash, timestamp, change_type, actor_id, target_id, "
       "signature, registrar_version, detail, prev_hash, timestamp_ns "
       "FROM gr_audit_log WHERE registrar_version > $1 "
       "ORDER BY timestamp ASC",
                     &reg->ps_audit_delta) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT sign_key FROM gr_peers WHERE peer_id=$1",
                     &reg->ps_audit_actor_pk) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT entry_hash FROM gr_audit_log "
       "ORDER BY registrar_version DESC, timestamp DESC LIMIT 1",
                     &reg->ps_audit_last_hash) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Invite ────────────────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT INTO gr_invites VALUES ($1,$2,$3,$4,$5,$6,$7)",
                     &reg->ps_invite_insert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT invalidated, used, expires_at FROM gr_invites "
       "WHERE verification_token=$1",
                     &reg->ps_invite_check) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_invites SET invalidated=TRUE "
       "WHERE verification_token=$1",
                     &reg->ps_invite_invalidate) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "UPDATE gr_invites SET used=TRUE, used_by=$1 "
       "WHERE verification_token=$2",
                     &reg->ps_invite_mark_used) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT verification_token, created_at, expires_at, created_by, "
       "invalidated, used, used_by "
       "FROM gr_invites LIMIT $1",
                     &reg->ps_invite_list) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c, "SELECT COUNT(*) FROM gr_invites",
                     &reg->ps_invite_count) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Icon ───────────────────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT INTO gr_group_icon "
       "(id, data, mime_type, width, height, is_video, "
       " static_frame, content_hash, updated_at, updated_by) "
       "VALUES (1, $1, $2, $3, $4, $5, $6, $7, $8, $9)",
                     &reg->ps_icon_upsert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT data, mime_type, width, height, is_video, "
       "static_frame, content_hash, updated_at, updated_by "
       "FROM gr_group_icon WHERE id=$1",
                     &reg->ps_icon_get) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT content_hash FROM gr_group_icon WHERE id=$1",
                     &reg->ps_icon_hash) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c, "DELETE FROM gr_group_icon",
                     &reg->ps_icon_delete) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Header ────────────────────────────────────────────────── */

  /** @brief Insert the single header row (16 columns).
   *  The caller (gr_header_save) must DELETE FROM gr_header first. */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT INTO gr_header VALUES "
       "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,"
       "$11,$12,$13,$14,$15,$16)",
                     &reg->ps_header_save) != DuckDBSuccess))
      ret = GR_ERR_DB;

  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT group_id, group_type, group_name, version, "
       "created_at, updated_at, epoch_id, "
       "message_retention_ms, file_retention_ms, registrar_max_bytes, "
       "owner_id, owner_sign_key, signer_id, signer_sign_key, "
       "signature, hash "
       "FROM gr_header LIMIT 1",
                     &reg->ps_header_load) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Audit retention and verification ──────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "DELETE FROM gr_audit_log WHERE entry_hash IN "
       "(SELECT a.entry_hash FROM gr_audit_log a "
       "WHERE a.prev_hash NOT IN "
       "(SELECT prev_hash FROM gr_audit_log "
       "GROUP BY prev_hash HAVING COUNT(*) > 1) "
       "ORDER BY a.registrar_version ASC, a.timestamp ASC "
       "LIMIT $1)",
                     &reg->ps_audit_prune) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT entry_hash, timestamp_ns, prev_hash "
       "FROM gr_audit_log WHERE prev_hash IN "
       "(SELECT prev_hash FROM gr_audit_log "
       "GROUP BY prev_hash HAVING COUNT(*) > 1) "
       "ORDER BY prev_hash, timestamp_ns ASC, entry_hash ASC",
                     &reg->ps_audit_fork_resolve) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT entry_hash, timestamp, change_type, actor_id, target_id, "
       "signature, registrar_version, detail, prev_hash, timestamp_ns "
       "FROM gr_audit_log "
       "ORDER BY registrar_version ASC, timestamp ASC "
       "LIMIT $1 OFFSET $2",
                     &reg->ps_audit_verify_batch) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT COUNT(*) FROM gr_audit_log WHERE prev_hash=$1",
                     &reg->ps_audit_fork_check) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT entry_hash, timestamp, change_type, actor_id, target_id, "
       "signature, registrar_version, detail, prev_hash, timestamp_ns "
       "FROM gr_audit_log WHERE prev_hash IN "
       "(SELECT prev_hash FROM gr_audit_log "
       "GROUP BY prev_hash HAVING COUNT(*) > 1) "
       "ORDER BY prev_hash, timestamp_ns ASC, entry_hash ASC "
       "LIMIT $1",
                     &reg->ps_audit_list_forks) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Behavioral analysis ────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT change_type FROM gr_audit_log "
       "WHERE actor_id=$1 AND timestamp > $2",
                     &reg->ps_beh_actor_actions) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT change_type, actor_id FROM gr_audit_log "
       "WHERE timestamp > $1",
                     &reg->ps_beh_window_all) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT peer_id FROM gr_peers "
       "WHERE status=0 AND last_seen < $1 "
       "ORDER BY last_seen ASC",
                     &reg->ps_beh_stale_peers) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT COUNT(*) FROM gr_peers "
       "WHERE status=0 AND last_seen < $1",
                     &reg->ps_beh_stale_count) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT COUNT(*) FROM gr_epochs "
       "WHERE created_at > $1 AND epoch_id > "
       "(SELECT MIN(epoch_id) FROM gr_epochs)",
                     &reg->ps_beh_epoch_window) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT AVG(CASE WHEN expired_at > 0 "
       "THEN expired_at - created_at "
       "ELSE $1 - created_at END) "
       "FROM gr_epochs",
                     &reg->ps_beh_epoch_avg) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT COUNT(*) FROM gr_peers WHERE joined_at > $1",
                     &reg->ps_beh_peer_joined) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT COUNT(*) FROM gr_peers "
       "WHERE removed_at > $1 AND removed_at > 0",
                     &reg->ps_beh_peer_removed) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT MIN(timestamp), MAX(timestamp), COUNT(*) "
       "FROM gr_audit_log",
                     &reg->ps_beh_entry_timespan) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── Delta merge ───────────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT OR REPLACE INTO gr_peers VALUES "
       "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)",
                     &reg->ps_delta_peer_upsert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT OR REPLACE INTO gr_roles VALUES ($1,$2,$3,$4,$5,$6)",
                     &reg->ps_delta_role_upsert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT OR REPLACE INTO gr_webapps VALUES ($1,$2,$3,$4,$5,$6,$7)",
                     &reg->ps_delta_webapp_upsert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT OR REPLACE INTO gr_servers VALUES "
       "($1,$2,$3,$4,$5,$6,$7,$8)",
                     &reg->ps_delta_server_upsert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT OR REPLACE INTO gr_epochs VALUES ($1,$2,$3,$4,$5)",
                     &reg->ps_delta_epoch_upsert) != DuckDBSuccess))
      ret = GR_ERR_DB;

  /* ── IP Blocklist ───────────────────────────────────────────── */
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "SELECT fail_count, blocked, blocked_at "
       "FROM gr_ip_blocklist WHERE ip=$1",
                     &reg->ps_block_get) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "INSERT OR REPLACE INTO gr_ip_blocklist "
       "VALUES ($1, $2, $3, $4, $5)",
                     &reg->ps_block_upsert) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "DELETE FROM gr_ip_blocklist WHERE ip=$1",
                     &reg->ps_block_delete) != DuckDBSuccess))
      ret = GR_ERR_DB;
  if ((ret == GR_OK) && (duckdb_prepare(c,
       "DELETE FROM gr_ip_blocklist "
       "WHERE blocked=TRUE AND blocked_at < $1",
                     &reg->ps_block_expire) != DuckDBSuccess))
      ret = GR_ERR_DB;

  if (ret == GR_OK)
    reg->stmts_ready = true;
  return ret;
}

/**
 * @brief Destroy all cached prepared statements.
 */
void gr_destroy_statements(gr_registrar_t *reg) {
  if (!reg || !reg->stmts_ready)
    return;

  /* Peer */
  duckdb_destroy_prepare(&reg->ps_peer_get);
  duckdb_destroy_prepare(&reg->ps_peer_count_all);
  duckdb_destroy_prepare(&reg->ps_peer_count_status);
  duckdb_destroy_prepare(&reg->ps_peer_list_all);
  duckdb_destroy_prepare(&reg->ps_peer_list_status);
  duckdb_destroy_prepare(&reg->ps_peer_insert);
  duckdb_destroy_prepare(&reg->ps_peer_update_status);
  duckdb_destroy_prepare(&reg->ps_peer_update_addr);
  duckdb_destroy_prepare(&reg->ps_peer_touch);
  duckdb_destroy_prepare(&reg->ps_peer_set_role);
  duckdb_destroy_prepare(&reg->ps_peer_reset_role);

  /* Role */
  duckdb_destroy_prepare(&reg->ps_role_get);
  duckdb_destroy_prepare(&reg->ps_role_list);
  duckdb_destroy_prepare(&reg->ps_role_count);
  duckdb_destroy_prepare(&reg->ps_role_insert);
  duckdb_destroy_prepare(&reg->ps_role_delete);
  duckdb_destroy_prepare(&reg->ps_role_set_perms);
  duckdb_destroy_prepare(&reg->ps_role_next_id);
  duckdb_destroy_prepare(&reg->ps_role_perms_join);

  /* WebApp */
  duckdb_destroy_prepare(&reg->ps_webapp_insert);
  duckdb_destroy_prepare(&reg->ps_webapp_delete);
  duckdb_destroy_prepare(&reg->ps_webapp_check);
  duckdb_destroy_prepare(&reg->ps_webapp_list);
  duckdb_destroy_prepare(&reg->ps_webapp_count);
  duckdb_destroy_prepare(&reg->ps_webapp_get);
  duckdb_destroy_prepare(&reg->ps_webapp_update_perm_data);
  duckdb_destroy_prepare(&reg->ps_webapp_update_role_mask);

  /* Server */
  duckdb_destroy_prepare(&reg->ps_server_insert);
  duckdb_destroy_prepare(&reg->ps_server_delete);
  duckdb_destroy_prepare(&reg->ps_server_list);
  duckdb_destroy_prepare(&reg->ps_server_count);
  duckdb_destroy_prepare(&reg->ps_server_get);

  /* Epoch */
  duckdb_destroy_prepare(&reg->ps_epoch_get);
  duckdb_destroy_prepare(&reg->ps_epoch_list);
  duckdb_destroy_prepare(&reg->ps_epoch_count);
  duckdb_destroy_prepare(&reg->ps_epoch_insert);
  duckdb_destroy_prepare(&reg->ps_epoch_expire);

  /* Audit */
  duckdb_destroy_prepare(&reg->ps_audit_insert);
  duckdb_destroy_prepare(&reg->ps_audit_dedup);
  duckdb_destroy_prepare(&reg->ps_audit_list);
  duckdb_destroy_prepare(&reg->ps_audit_count);
  duckdb_destroy_prepare(&reg->ps_audit_delta);
  duckdb_destroy_prepare(&reg->ps_audit_actor_pk);
  duckdb_destroy_prepare(&reg->ps_audit_last_hash);

  /* Invite */
  duckdb_destroy_prepare(&reg->ps_invite_insert);
  duckdb_destroy_prepare(&reg->ps_invite_check);
  duckdb_destroy_prepare(&reg->ps_invite_invalidate);
  duckdb_destroy_prepare(&reg->ps_invite_mark_used);
  duckdb_destroy_prepare(&reg->ps_invite_list);
  duckdb_destroy_prepare(&reg->ps_invite_count);

  /* Icon */
  duckdb_destroy_prepare(&reg->ps_icon_upsert);
  duckdb_destroy_prepare(&reg->ps_icon_get);
  duckdb_destroy_prepare(&reg->ps_icon_hash);
  duckdb_destroy_prepare(&reg->ps_icon_delete);

  /* Header */
  duckdb_destroy_prepare(&reg->ps_header_save);
  duckdb_destroy_prepare(&reg->ps_header_load);

  /* Audit retention / verification */
  duckdb_destroy_prepare(&reg->ps_audit_prune);
  duckdb_destroy_prepare(&reg->ps_audit_fork_resolve);
  duckdb_destroy_prepare(&reg->ps_audit_verify_batch);
  duckdb_destroy_prepare(&reg->ps_audit_fork_check);
  duckdb_destroy_prepare(&reg->ps_audit_list_forks);

  /* Behavioral analysis */
  duckdb_destroy_prepare(&reg->ps_beh_actor_actions);
  duckdb_destroy_prepare(&reg->ps_beh_window_all);
  duckdb_destroy_prepare(&reg->ps_beh_stale_peers);
  duckdb_destroy_prepare(&reg->ps_beh_stale_count);
  duckdb_destroy_prepare(&reg->ps_beh_epoch_window);
  duckdb_destroy_prepare(&reg->ps_beh_epoch_avg);
  duckdb_destroy_prepare(&reg->ps_beh_peer_joined);
  duckdb_destroy_prepare(&reg->ps_beh_peer_removed);
  duckdb_destroy_prepare(&reg->ps_beh_entry_timespan);

  /* Delta merge */
  duckdb_destroy_prepare(&reg->ps_delta_peer_upsert);
  duckdb_destroy_prepare(&reg->ps_delta_role_upsert);
  duckdb_destroy_prepare(&reg->ps_delta_webapp_upsert);
  duckdb_destroy_prepare(&reg->ps_delta_server_upsert);
  duckdb_destroy_prepare(&reg->ps_delta_epoch_upsert);

  /* IP Blocklist */
  duckdb_destroy_prepare(&reg->ps_block_get);
  duckdb_destroy_prepare(&reg->ps_block_upsert);
  duckdb_destroy_prepare(&reg->ps_block_delete);
  duckdb_destroy_prepare(&reg->ps_block_expire);

  reg->stmts_ready = false;
}
