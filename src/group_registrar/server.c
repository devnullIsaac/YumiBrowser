#include "internal.h"

gr_error_t gr_server_add(gr_registrar_t *reg,
                         const gr_server_t *server,
                         const gr_identity_t *signer) {
    if (!reg || !server || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    GR_LOCK(reg);

    if (!gr_check_perm(reg, signer, GR_PERM_EDIT_SERVERS))
        { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }

    duckdb_prepared_statement stmt = reg->ps_server_insert;
    duckdb_clear_bindings(stmt);

    duckdb_bind_blob(stmt, 1, server->id_hash, GR_HASH_LEN);
    duckdb_bind_int32(stmt, 2, (int32_t)server->type);
    duckdb_bind_varchar(stmt, 3, server->ip);
    duckdb_bind_int32(stmt, 4, (int32_t)server->port);
    duckdb_bind_blob(stmt, 5, server->sign_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_blob(stmt, 6, server->service_hash, GR_SERVICE_HASH_LEN);
    duckdb_bind_blob(stmt, 7, server->content_kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    duckdb_bind_blob(stmt, 8, server->content_kem_sk, GR_KEM_SECRET_KEY_LEN);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    const char *t = server->type == GR_SERVER_SIGNALING
                    ? "signaling" : "rebroadcast";
    char detail[128];
    snprintf(detail, sizeof(detail), "%s server added at %s:%u",
             t, server->ip, server->port);
    gr_error_t err = gr_audit_append(reg, GR_CHANGE_SERVER_ADDED, signer, NULL, detail);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_server_remove(gr_registrar_t *reg,
                            const uint8_t id_hash[GR_HASH_LEN],
                            const gr_identity_t *signer) {
    if (!reg || !id_hash || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    GR_LOCK(reg);

    if (!gr_check_perm(reg, signer, GR_PERM_EDIT_SERVERS))
        { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }

    duckdb_prepared_statement stmt = reg->ps_server_delete;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, id_hash, GR_HASH_LEN);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) { duckdb_destroy_result(&result); gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    idx_t changed = duckdb_rows_changed(&result);
    duckdb_destroy_result(&result);
    if (changed == 0) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_NOT_FOUND; }

    gr_error_t err = gr_audit_append(reg, GR_CHANGE_SERVER_REMOVED, signer, NULL,
                           "server removed");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

/* ── Read-only operations — always allowed ─────────────────────── */

gr_error_t gr_server_list(const gr_registrar_t *reg,
                          gr_server_type_t type,
                          gr_server_t *out, uint32_t max_count,
                          uint32_t *actual_count) {
    if (!reg || !out || !actual_count) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_server_list;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)type);
    duckdb_bind_int32(stmt, 2, (int32_t)max_count);

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
        duckdb_vector v_idh  = duckdb_data_chunk_get_vector(chunk, 0);
        duckdb_vector v_typ  = duckdb_data_chunk_get_vector(chunk, 1);
        duckdb_vector v_ip   = duckdb_data_chunk_get_vector(chunk, 2);
        duckdb_vector v_port = duckdb_data_chunk_get_vector(chunk, 3);
        duckdb_vector v_sk   = duckdb_data_chunk_get_vector(chunk, 4);
        duckdb_vector v_sh   = duckdb_data_chunk_get_vector(chunk, 5);
        duckdb_vector v_ckpk = duckdb_data_chunk_get_vector(chunk, 6);
        duckdb_vector v_cksk = duckdb_data_chunk_get_vector(chunk, 7);
        for (idx_t i = 0; i < rows && count < max_count; i++) {
            gr_server_t *s = &out[count];
            int32_t type_val = 0, port_val = 0;
            memset(s, 0, sizeof(*s));
            gr_db_vec_get_blob(v_idh,  i, s->id_hash, GR_HASH_LEN);
            gr_db_vec_get_i32 (v_typ,  i, &type_val);
            gr_db_vec_get_str (v_ip,   i, s->ip, GR_MAX_IP_LEN);
            gr_db_vec_get_i32 (v_port, i, &port_val);
            gr_db_vec_get_blob(v_sk,   i, s->sign_key, GR_PUBLIC_KEY_LEN);
            gr_db_vec_get_blob(v_sh,   i, s->service_hash, GR_SERVICE_HASH_LEN);
            gr_db_vec_get_blob(v_ckpk, i, s->content_kem_pk, GR_KEM_PUBLIC_KEY_LEN);
            gr_db_vec_get_blob(v_cksk, i, s->content_kem_sk, GR_KEM_SECRET_KEY_LEN);
            s->type = (gr_server_type_t)type_val;
            s->port = (uint16_t)port_val;
            count++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    *actual_count = count;
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_server_count(const gr_registrar_t *reg,
                           gr_server_type_t type,
                           uint32_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_server_count;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)type);

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
