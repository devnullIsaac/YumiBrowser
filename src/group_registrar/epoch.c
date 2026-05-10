/*
    Group Registrar — Epoch Rotation
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

#include "internal.h"

gr_error_t gr_epoch_rotate(gr_registrar_t *reg,
                           const gr_identity_t *signer) {
    if (!reg || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, signer, GR_PERM_ROTATE_EPOCH))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    int64_t now = gr_timestamp_ms();

    if (!gr_txn_begin(reg)) {
        GR_UNLOCK(reg);
        return GR_ERR_DB;
    }

    /* Expire current epoch */
    duckdb_prepared_statement stmt = reg->ps_epoch_expire;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int64(stmt, 1, now);
    duckdb_bind_int32(stmt, 2, (int32_t)reg->header.epoch_id);
    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) {
        gr_txn_rollback(reg);
        GR_UNLOCK(reg);
        return GR_ERR_DB;
    }

    /* Create new epoch */
    uint32_t new_id = reg->header.epoch_id + 1;
    uint8_t new_key[GR_EPOCH_KEY_LEN];
    if (yumi_randombytes(new_key, GR_EPOCH_KEY_LEN) != YUMI_CRYPTO_OK) {
        gr_txn_rollback(reg);
        GR_UNLOCK(reg);
        return GR_ERR_CRYPTO;
    }

    stmt = reg->ps_epoch_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)new_id);
    duckdb_bind_blob(stmt, 2, new_key, GR_EPOCH_KEY_LEN);
    duckdb_bind_int64(stmt, 3, now);
    duckdb_bind_int64(stmt, 4, 0);
    duckdb_bind_blob(stmt, 5, signer->peer_id, GR_PEER_ID_LEN);

    st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    yumi_memzero(new_key, GR_EPOCH_KEY_LEN);
    if (st != DuckDBSuccess) {
        gr_txn_rollback(reg);
        GR_UNLOCK(reg);
        return GR_ERR_DB;
    }

    reg->header.epoch_id = new_id;

    char detail[64];
    snprintf(detail, sizeof(detail), "epoch rotated to %u", new_id);
    gr_error_t err = gr_audit_append(reg, GR_CHANGE_EPOCH_ROTATED, signer, NULL, detail);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

/* ── Read-only operations — always allowed ─────────────────────── */

gr_error_t gr_epoch_get_current(const gr_registrar_t *reg, gr_epoch_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;
    return gr_epoch_get(reg, reg->header.epoch_id, out);
}

gr_error_t gr_epoch_get(const gr_registrar_t *reg,
                        uint32_t epoch_id,
                        gr_epoch_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_epoch_get;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)epoch_id);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_DB;
    }
    duckdb_data_chunk chunk = NULL;
    if (gr_db_fetch_first_chunk(&result, &chunk) != GR_OK) {
        duckdb_destroy_result(&result);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    int32_t eid = 0;
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 0), 0, &eid);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 1), 0,
                       out->epoch_key, GR_EPOCH_KEY_LEN);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 2), 0,
                       &out->created_at);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 3), 0,
                       &out->expired_at);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 4), 0,
                       out->created_by, GR_PEER_ID_LEN);
    out->epoch_id = (uint32_t)eid;

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_epoch_list(const gr_registrar_t *reg,
                         gr_epoch_t *out, uint32_t max_count,
                         uint32_t *actual_count) {
    if (!reg || !out || !actual_count) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_epoch_list;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)max_count);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_DB;
    }

    uint32_t count = 0;
    duckdb_data_chunk chunk;
    while (count < max_count &&
           (chunk = duckdb_fetch_chunk(result)) != NULL) {
        idx_t rows = duckdb_data_chunk_get_size(chunk);
        duckdb_vector v_id  = duckdb_data_chunk_get_vector(chunk, 0);
        duckdb_vector v_key = duckdb_data_chunk_get_vector(chunk, 1);
        duckdb_vector v_ct  = duckdb_data_chunk_get_vector(chunk, 2);
        duckdb_vector v_et  = duckdb_data_chunk_get_vector(chunk, 3);
        duckdb_vector v_cb  = duckdb_data_chunk_get_vector(chunk, 4);
        for (idx_t i = 0; i < rows && count < max_count; i++) {
            gr_epoch_t *e = &out[count];
            int32_t eid = 0;
            memset(e, 0, sizeof(*e));
            gr_db_vec_get_i32 (v_id,  i, &eid);
            gr_db_vec_get_blob(v_key, i, e->epoch_key, GR_EPOCH_KEY_LEN);
            gr_db_vec_get_i64 (v_ct,  i, &e->created_at);
            gr_db_vec_get_i64 (v_et,  i, &e->expired_at);
            gr_db_vec_get_blob(v_cb,  i, e->created_by, GR_PEER_ID_LEN);
            e->epoch_id = (uint32_t)eid;
            count++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    *actual_count = count;
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_epoch_count(const gr_registrar_t *reg, uint32_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_epoch_count;
    duckdb_clear_bindings(stmt);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_DB;
    }
    int64_t n = 0;
    gr_error_t err = gr_db_fetch_i64_scalar(&result, &n);
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    if (err != GR_OK) return err;
    *out = (uint32_t)n;
    return GR_OK;
}
