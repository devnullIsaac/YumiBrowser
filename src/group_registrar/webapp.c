#include "internal.h"

gr_error_t gr_webapp_add(gr_registrar_t *reg,
                         const gr_webapp_t *webapp,
                         const gr_identity_t *signer) {
    if (!reg || !webapp || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, signer, GR_PERM_ADD_WEBAPP))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    int64_t now = gr_timestamp_ms();
    duckdb_prepared_statement stmt = reg->ps_webapp_insert;
    duckdb_clear_bindings(stmt);

    duckdb_bind_blob(stmt, 1, webapp->hash, GR_SERVICE_HASH_LEN);
    duckdb_bind_varchar(stmt, 2, webapp->name);
    duckdb_bind_int32(stmt, 3, (int32_t)webapp->version);
    duckdb_bind_int64(stmt, 4, now);
    duckdb_bind_blob(stmt, 5, signer->peer_id, GR_PEER_ID_LEN);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    char detail[128];
    snprintf(detail, sizeof(detail), "webapp '%.64s' v%u added",
             webapp->name, webapp->version);
    gr_error_t err = gr_audit_append(reg, GR_CHANGE_WEBAPP_ADDED, signer, NULL, detail);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_webapp_remove(gr_registrar_t *reg,
                            const uint8_t hash[GR_SERVICE_HASH_LEN],
                            const gr_identity_t *signer) {
    if (!reg || !hash || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, signer, GR_PERM_REMOVE_WEBAPP))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    duckdb_prepared_statement stmt = reg->ps_webapp_delete;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, hash, GR_SERVICE_HASH_LEN);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) { duckdb_destroy_result(&result); gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    idx_t changed = duckdb_rows_changed(&result);
    duckdb_destroy_result(&result);
    if (changed == 0) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_NOT_FOUND; }

    gr_error_t err = gr_audit_append(reg, GR_CHANGE_WEBAPP_REMOVED, signer, NULL,
                           "webapp removed");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

/* ── Read-only operations — always allowed ─────────────────────── */

bool gr_webapp_is_authorized(const gr_registrar_t *reg,
                             const uint8_t hash[GR_SERVICE_HASH_LEN]) {
    if (!reg || !hash) return false;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_webapp_check;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, hash, GR_SERVICE_HASH_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    bool ok = false;
    if (st == DuckDBSuccess) {
        int64_t n = 0;
        if (gr_db_fetch_i64_scalar(&result, &n) == GR_OK && n > 0)
            ok = true;
    }
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return ok;
}

gr_error_t gr_webapp_list(const gr_registrar_t *reg,
                          gr_webapp_t *out, uint32_t max_count,
                          uint32_t *actual_count) {
    if (!reg || !out || !actual_count) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_webapp_list;
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
        duckdb_vector v_hash     = duckdb_data_chunk_get_vector(chunk, 0);
        duckdb_vector v_name     = duckdb_data_chunk_get_vector(chunk, 1);
        duckdb_vector v_version  = duckdb_data_chunk_get_vector(chunk, 2);
        duckdb_vector v_added_at = duckdb_data_chunk_get_vector(chunk, 3);
        duckdb_vector v_added_by = duckdb_data_chunk_get_vector(chunk, 4);
        for (idx_t i = 0; i < rows && count < max_count; i++) {
            gr_webapp_t *w = &out[count];
            int32_t version = 0;
            memset(w, 0, sizeof(*w));
            gr_db_vec_get_blob(v_hash,     i, w->hash, GR_SERVICE_HASH_LEN);
            gr_db_vec_get_str (v_name,     i, w->name, GR_MAX_NAME_LEN);
            gr_db_vec_get_i32 (v_version,  i, &version);
            gr_db_vec_get_i64 (v_added_at, i, &w->added_at);
            gr_db_vec_get_blob(v_added_by, i, w->added_by, GR_PEER_ID_LEN);
            w->version = (uint32_t)version;
            count++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    *actual_count = count;
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_webapp_count(const gr_registrar_t *reg, uint32_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_webapp_count;
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
