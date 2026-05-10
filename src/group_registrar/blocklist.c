/*
    Group Registrar — IP Blocklist for Bootstrap Listener
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
 * @file blocklist.c
 * @brief DuckDB-backed IP blocklist for the Group Registrar bootstrap listener.
 *
 * Tracks per-IP failed authentication attempts and blocks IPs that exceed
 * a configurable threshold.  Persists across restarts via the gr_ip_blocklist
 * table.
 */

#include "internal.h"

/* Wall-clock milliseconds */
static int64_t now_ms(void) {
    return gr_timestamp_ns() / 1000000LL;
}

gr_error_t gr_blocklist_check(gr_registrar_t *reg, const char *ip,
                              int64_t block_duration_ms, bool *blocked_out)
{
    if (!reg || !ip || !blocked_out) return GR_ERR_INVALID_PARAM;
    *blocked_out = false;

    GR_LOCK(reg);

    duckdb_prepared_statement stmt = reg->ps_block_get;
    duckdb_clear_bindings(stmt);
    duckdb_bind_varchar(stmt, 1, ip);

    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(stmt, &res);
    if (st != DuckDBSuccess) {
        duckdb_destroy_result(&res);
        GR_UNLOCK(reg);
        return GR_ERR_DB;
    }

    duckdb_data_chunk chunk = NULL;
    if (gr_db_fetch_first_chunk(&res, &chunk) != GR_OK) {
        duckdb_destroy_result(&res);
        GR_UNLOCK(reg);
        return GR_OK; /* not in blocklist */
    }

    bool blocked = false;
    int64_t blocked_at = 0;
    gr_db_vec_get_bool(duckdb_data_chunk_get_vector(chunk, 1), 0, &blocked);
    gr_db_vec_get_i64(duckdb_data_chunk_get_vector(chunk, 2), 0, &blocked_at);
    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&res);

    if (blocked) {
        int64_t elapsed = now_ms() - blocked_at;
        if (elapsed >= block_duration_ms) {
            /* Expired — delete the entry */
            duckdb_prepared_statement del = reg->ps_block_delete;
            duckdb_clear_bindings(del);
            duckdb_bind_varchar(del, 1, ip);
            duckdb_result dr;
            duckdb_execute_prepared(del, &dr);
            duckdb_destroy_result(&dr);
            /* blocked_out stays false */
        } else {
            *blocked_out = true;
        }
    }

    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_blocklist_record_fail(gr_registrar_t *reg, const char *ip,
                                    int max_fails, bool *just_blocked_out)
{
    if (!reg || !ip || !just_blocked_out) return GR_ERR_INVALID_PARAM;
    *just_blocked_out = false;

    int64_t ts = now_ms();

    GR_LOCK(reg);

    /* Read current state */
    int32_t fail_count = 0;
    bool already_blocked = false;

    {
        duckdb_prepared_statement stmt = reg->ps_block_get;
        duckdb_clear_bindings(stmt);
        duckdb_bind_varchar(stmt, 1, ip);

        duckdb_result res;
        duckdb_state st = duckdb_execute_prepared(stmt, &res);
        if (st != DuckDBSuccess) {
            duckdb_destroy_result(&res);
            GR_UNLOCK(reg);
            return GR_ERR_DB;
        }
        duckdb_data_chunk chunk = NULL;
        if (gr_db_fetch_first_chunk(&res, &chunk) == GR_OK) {
            gr_db_vec_get_i32(duckdb_data_chunk_get_vector(chunk, 0), 0,
                              &fail_count);
            gr_db_vec_get_bool(duckdb_data_chunk_get_vector(chunk, 1), 0,
                               &already_blocked);
            duckdb_destroy_data_chunk(&chunk);
        }
        duckdb_destroy_result(&res);
    }

    if (already_blocked) {
        GR_UNLOCK(reg);
        return GR_OK; /* already blocked, nothing to do */
    }

    fail_count++;
    bool should_block = (fail_count >= max_fails);

    /* Upsert: ip, fail_count, blocked, blocked_at, last_attempt */
    {
        duckdb_prepared_statement stmt = reg->ps_block_upsert;
        duckdb_clear_bindings(stmt);
        duckdb_bind_varchar(stmt, 1, ip);
        duckdb_bind_int32(stmt, 2, fail_count);
        duckdb_bind_boolean(stmt, 3, should_block);
        duckdb_bind_int64(stmt, 4, should_block ? ts : 0);
        duckdb_bind_int64(stmt, 5, ts);

        duckdb_result res;
        duckdb_state st = duckdb_execute_prepared(stmt, &res);
        duckdb_destroy_result(&res);
        if (st != DuckDBSuccess) {
            GR_UNLOCK(reg);
            return GR_ERR_DB;
        }
    }

    *just_blocked_out = should_block;
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_blocklist_reset(gr_registrar_t *reg, const char *ip)
{
    if (!reg || !ip) return GR_ERR_INVALID_PARAM;

    GR_LOCK(reg);

    duckdb_prepared_statement stmt = reg->ps_block_delete;
    duckdb_clear_bindings(stmt);
    duckdb_bind_varchar(stmt, 1, ip);

    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(stmt, &res);
    duckdb_destroy_result(&res);

    GR_UNLOCK(reg);
    return (st == DuckDBSuccess) ? GR_OK : GR_ERR_DB;
}

gr_error_t gr_blocklist_cleanup(gr_registrar_t *reg,
                                int64_t block_duration_ms)
{
    if (!reg) return GR_ERR_INVALID_PARAM;

    int64_t cutoff = now_ms() - block_duration_ms;

    GR_LOCK(reg);

    duckdb_prepared_statement stmt = reg->ps_block_expire;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int64(stmt, 1, cutoff);

    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(stmt, &res);
    duckdb_destroy_result(&res);

    GR_UNLOCK(reg);
    return (st == DuckDBSuccess) ? GR_OK : GR_ERR_DB;
}
