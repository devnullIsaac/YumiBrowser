#include "internal.h"

gr_error_t gr_peer_add(gr_registrar_t *reg,
                       const gr_peer_t *peer,
                       const gr_identity_t *signer) {
    if (!reg || !peer || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    GR_LOCK(reg);

    if (reg->header.group_type == GR_GROUP_PRIVATE) {
        if (!gr_check_perm(reg, signer, GR_PERM_INVITE_MEMBER))
            { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }
    }

    /* Verify peer_id is correctly derived from sign_key */
    uint8_t derived_id[GR_PEER_ID_LEN];
    gr_identity_derive_id(peer->sign_key, derived_id);
    if (yumi_memcmp(derived_id, peer->peer_id, GR_PEER_ID_LEN) != 0)
        { GR_UNLOCK(reg); return GR_ERR_INVALID_PARAM; }

    gr_peer_t existing;
    if (gr_peer_get(reg, peer->peer_id, &existing) == GR_OK) {
        if (existing.status == GR_PEER_KICKED) {
            /* Kicked peers may rejoin — reset to ACTIVE */
            if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

            duckdb_prepared_statement ustmt = reg->ps_peer_update_status;
            duckdb_clear_bindings(ustmt);
            duckdb_bind_int32(ustmt, 1, GR_PEER_ACTIVE);
            duckdb_bind_int64(ustmt, 2, 0);
            duckdb_bind_varchar(ustmt, 3, "");
            uint8_t zeroed[GR_PEER_ID_LEN];
            memset(zeroed, 0, GR_PEER_ID_LEN);
            duckdb_bind_blob(ustmt, 4, zeroed, GR_PEER_ID_LEN);
            duckdb_bind_blob(ustmt, 5, peer->peer_id, GR_PEER_ID_LEN);

            duckdb_result ures;
            duckdb_state ust = duckdb_execute_prepared(ustmt, &ures);
            duckdb_destroy_result(&ures);
            if (ust != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

            gr_error_t aerr = gr_audit_append(reg, GR_CHANGE_PEER_ADDED, signer,
                                   peer->peer_id, "peer re-added (was kicked)");
            if (aerr != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return aerr; }
            if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
            GR_UNLOCK(reg);
            return GR_OK;
        }
        GR_UNLOCK(reg);
        return GR_ERR_ALREADY_EXISTS;
    }

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    /* Insert the peer row into gr_peers */
    duckdb_prepared_statement stmt = reg->ps_peer_insert;
    duckdb_clear_bindings(stmt);

    uint8_t zeroed_id[GR_PEER_ID_LEN];
    memset(zeroed_id, 0, GR_PEER_ID_LEN);

    duckdb_bind_blob(stmt, 1,  peer->peer_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 2,  peer->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    duckdb_bind_blob(stmt, 3,  peer->sign_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_varchar(stmt, 4, peer->ip);
    duckdb_bind_int32(stmt, 5,  (int32_t)peer->port);
    duckdb_bind_int32(stmt, 6,  (int32_t)peer->status);
    duckdb_bind_int32(stmt, 7,  (int32_t)peer->role_id);
    duckdb_bind_int64(stmt, 8,  peer->joined_at);
    duckdb_bind_int64(stmt, 9,  peer->removed_at);
    duckdb_bind_int64(stmt, 10, peer->last_seen);
    duckdb_bind_varchar(stmt, 11, peer->removed_reason);
    duckdb_bind_blob(stmt, 12, peer->removed_by[0] ? peer->removed_by : zeroed_id,
                     GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    gr_error_t err = gr_audit_append(reg, GR_CHANGE_PEER_ADDED, signer,
                           peer->peer_id, "peer added");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_peer_kick(gr_registrar_t *reg,
                        const uint8_t peer_id[GR_PEER_ID_LEN],
                        const char *reason,
                        const gr_identity_t *signer) {
    if (!reg || !peer_id || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    GR_LOCK(reg);

    if (!gr_check_perm(reg, signer, GR_PERM_KICK_MEMBER))
        { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }

    gr_peer_t peer;
    gr_error_t err = gr_peer_get(reg, peer_id, &peer);
    if (err != GR_OK) { GR_UNLOCK(reg); return err; }
    if (peer.status != GR_PEER_ACTIVE) { GR_UNLOCK(reg); return GR_ERR_NOT_FOUND; }
    if (gr_is_owner(reg, peer_id)) { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    int64_t now = gr_timestamp_ms();
    duckdb_prepared_statement stmt = reg->ps_peer_update_status;
    duckdb_clear_bindings(stmt);

    duckdb_bind_int32(stmt, 1, GR_PEER_KICKED);
    duckdb_bind_int64(stmt, 2, now);
    duckdb_bind_varchar(stmt, 3, reason ? reason : "");
    duckdb_bind_blob(stmt, 4, signer->peer_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 5, peer_id, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    err = gr_audit_append(reg, GR_CHANGE_PEER_KICKED, signer, peer_id,
                           reason ? reason : "kicked");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_peer_ban(gr_registrar_t *reg,
                       const uint8_t peer_id[GR_PEER_ID_LEN],
                       const char *reason,
                       const gr_identity_t *signer) {
    if (!reg || !peer_id || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    GR_LOCK(reg);

    if (!gr_check_perm(reg, signer, GR_PERM_BAN_MEMBER))
        { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }

    gr_peer_t peer;
    gr_error_t err = gr_peer_get(reg, peer_id, &peer);
    if (err != GR_OK) { GR_UNLOCK(reg); return err; }
    if (gr_is_owner(reg, peer_id)) { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    int64_t now = gr_timestamp_ms();
    duckdb_prepared_statement stmt = reg->ps_peer_update_status;
    duckdb_clear_bindings(stmt);

    duckdb_bind_int32(stmt, 1, GR_PEER_BANNED);
    duckdb_bind_int64(stmt, 2, now);
    duckdb_bind_varchar(stmt, 3, reason ? reason : "");
    duckdb_bind_blob(stmt, 4, signer->peer_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 5, peer_id, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    err = gr_audit_append(reg, GR_CHANGE_PEER_BANNED, signer, peer_id,
                           reason ? reason : "banned");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_peer_leave(gr_registrar_t *reg, const gr_identity_t *peer) {
    if (!reg || !peer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    GR_LOCK(reg);

    if (gr_is_owner(reg, peer->peer_id)) { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }

    /* Verify peer is active before updating */
    gr_peer_t existing;
    gr_error_t err = gr_peer_get(reg, peer->peer_id, &existing);
    if (err != GR_OK) { GR_UNLOCK(reg); return err; }
    if (existing.status != GR_PEER_ACTIVE) { GR_UNLOCK(reg); return GR_ERR_NOT_FOUND; }

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    int64_t now = gr_timestamp_ms();
    duckdb_prepared_statement stmt = reg->ps_peer_update_status;
    duckdb_clear_bindings(stmt);

    duckdb_bind_int32(stmt, 1, GR_PEER_LEFT);
    duckdb_bind_int64(stmt, 2, now);
    duckdb_bind_varchar(stmt, 3, "left voluntarily");
    duckdb_bind_blob(stmt, 4, peer->peer_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 5, peer->peer_id, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    err = gr_audit_append(reg, GR_CHANGE_PEER_REMOVED, peer,
                           peer->peer_id, "left voluntarily");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

/* ── Address updates are allowed during provisional state ──────
 *
 * These are connectivity operations, not governance mutations.
 * The invitee needs to update their own address and observe
 * other peers' addresses during the verification window.
 * ────────────────────────────────────────────────────────────── */

gr_error_t gr_peer_update_address(gr_registrar_t *reg,
                                  const char *ip, uint16_t port,
                                  const gr_identity_t *peer) {
    if (!reg || !ip || !peer) return GR_ERR_INVALID_PARAM;

    GR_LOCK(reg);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    int64_t now = gr_timestamp_ms();
    duckdb_prepared_statement stmt = reg->ps_peer_update_addr;
    duckdb_clear_bindings(stmt);

    duckdb_bind_varchar(stmt, 1, ip);
    duckdb_bind_int32(stmt, 2, (int32_t)port);
    duckdb_bind_int64(stmt, 3, now);
    duckdb_bind_blob(stmt, 4, peer->peer_id, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    gr_error_t err = gr_audit_append(reg, GR_CHANGE_PEER_ADDRESS, peer,
                           peer->peer_id, "address updated");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_peer_observed_address(gr_registrar_t *reg,
                                    const uint8_t peer_id[GR_PEER_ID_LEN],
                                    const char *ip, uint16_t port,
                                    const gr_identity_t *observer) {
    if (!reg || !peer_id || !ip || !observer) return GR_ERR_INVALID_PARAM;

    GR_LOCK(reg);

    /* Verify the target peer exists and is active */
    gr_peer_t existing;
    gr_error_t err = gr_peer_get(reg, peer_id, &existing);
    if (err != GR_OK) { GR_UNLOCK(reg); return err; }
    if (existing.status != GR_PEER_ACTIVE) { GR_UNLOCK(reg); return GR_ERR_NOT_FOUND; }

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    int64_t now = gr_timestamp_ms();
    duckdb_prepared_statement stmt = reg->ps_peer_update_addr;
    duckdb_clear_bindings(stmt);

    duckdb_bind_varchar(stmt, 1, ip);
    duckdb_bind_int32(stmt, 2, (int32_t)port);
    duckdb_bind_int64(stmt, 3, now);
    duckdb_bind_blob(stmt, 4, peer_id, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    err = gr_audit_append(reg, GR_CHANGE_PEER_ADDRESS, observer,
                          peer_id, "observed address");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_peer_touch(gr_registrar_t *reg,
                         const uint8_t peer_id[GR_PEER_ID_LEN]) {
    if (!reg || !peer_id) return GR_ERR_INVALID_PARAM;

    GR_LOCK(reg);

    int64_t now = gr_timestamp_ms();
    duckdb_prepared_statement stmt = reg->ps_peer_touch;
    duckdb_clear_bindings(stmt);

    duckdb_bind_int64(stmt, 1, now);
    duckdb_bind_blob(stmt, 2, peer_id, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    GR_UNLOCK(reg);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

gr_error_t gr_peer_set_role(gr_registrar_t *reg,
                            const uint8_t peer_id[GR_PEER_ID_LEN],
                            uint32_t role_id,
                            const gr_identity_t *signer) {
    if (!reg || !peer_id || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    GR_LOCK(reg);

    if (!gr_check_perm(reg, signer, GR_PERM_EDIT_ROLES))
        { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }

    if (role_id > 0) {
        gr_role_t role;
        gr_error_t err = gr_role_get(reg, role_id, &role);
        if (err != GR_OK) { GR_UNLOCK(reg); return err; }
    }

    duckdb_prepared_statement stmt = reg->ps_peer_set_role;
    duckdb_clear_bindings(stmt);

    duckdb_bind_int32(stmt, 1, (int32_t)role_id);
    duckdb_bind_blob(stmt, 2, peer_id, GR_PEER_ID_LEN);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    char detail[64];
    snprintf(detail, sizeof(detail), "role set to %u", role_id);
    gr_error_t err = gr_audit_append(reg, GR_CHANGE_PEER_ROLE_CHANGED, signer,
                           peer_id, detail);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

/* ── Read-only operations — always allowed ─────────────────────── */

gr_error_t gr_peer_get(const gr_registrar_t *reg,
                       const uint8_t peer_id[GR_PEER_ID_LEN],
                       gr_peer_t *out) {
    if (!reg || !peer_id || !out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_peer_get;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, peer_id, GR_PEER_ID_LEN);

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
    int32_t port_val = 0, status_val = 0, role_val = 0;
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 0), 0,
                       out->peer_id, GR_PEER_ID_LEN);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 1), 0,
                       out->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 2), 0,
                       out->sign_key, GR_PUBLIC_KEY_LEN);
    gr_db_vec_get_str (duckdb_data_chunk_get_vector(chunk, 3), 0,
                       out->ip, GR_MAX_IP_LEN);
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 4), 0, &port_val);
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 5), 0, &status_val);
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 6), 0, &role_val);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 7), 0,
                       &out->joined_at);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 8), 0,
                       &out->removed_at);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 9), 0,
                       &out->last_seen);
    gr_db_vec_get_str (duckdb_data_chunk_get_vector(chunk, 10), 0,
                       out->removed_reason, GR_MAX_NAME_LEN);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 11), 0,
                       out->removed_by, GR_PEER_ID_LEN);
    out->port    = (uint16_t)port_val;
    out->status  = (gr_peer_status_t)status_val;
    out->role_id = (uint32_t)role_val;

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_peer_count(const gr_registrar_t *reg,
                         gr_peer_status_t status_filter,
                         uint32_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt;
    if (status_filter == GR_PEER_STATUS_ANY) {
        stmt = reg->ps_peer_count_all;
        duckdb_clear_bindings(stmt);
    } else {
        stmt = reg->ps_peer_count_status;
        duckdb_clear_bindings(stmt);
        duckdb_bind_int32(stmt, 1, (int32_t)status_filter);
    }

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

gr_error_t gr_peer_list(const gr_registrar_t *reg,
                        gr_peer_t *out, uint32_t max_count,
                        uint32_t *actual_count,
                        gr_peer_status_t status_filter) {
    if (!reg || !out || !actual_count) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt;
    if (status_filter == GR_PEER_STATUS_ANY) {
        stmt = reg->ps_peer_list_all;
        duckdb_clear_bindings(stmt);
        duckdb_bind_int32(stmt, 1, (int32_t)max_count);
    } else {
        stmt = reg->ps_peer_list_status;
        duckdb_clear_bindings(stmt);
        duckdb_bind_int32(stmt, 1, (int32_t)status_filter);
        duckdb_bind_int32(stmt, 2, (int32_t)max_count);
    }

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
        duckdb_vector v_pid    = duckdb_data_chunk_get_vector(chunk, 0);
        duckdb_vector v_kem    = duckdb_data_chunk_get_vector(chunk, 1);
        duckdb_vector v_sign   = duckdb_data_chunk_get_vector(chunk, 2);
        duckdb_vector v_ip     = duckdb_data_chunk_get_vector(chunk, 3);
        duckdb_vector v_port   = duckdb_data_chunk_get_vector(chunk, 4);
        duckdb_vector v_status = duckdb_data_chunk_get_vector(chunk, 5);
        duckdb_vector v_role   = duckdb_data_chunk_get_vector(chunk, 6);
        duckdb_vector v_jat    = duckdb_data_chunk_get_vector(chunk, 7);
        duckdb_vector v_rat    = duckdb_data_chunk_get_vector(chunk, 8);
        duckdb_vector v_ls     = duckdb_data_chunk_get_vector(chunk, 9);
        duckdb_vector v_rr     = duckdb_data_chunk_get_vector(chunk, 10);
        duckdb_vector v_rb     = duckdb_data_chunk_get_vector(chunk, 11);
        for (idx_t i = 0; i < rows && count < max_count; i++) {
            gr_peer_t *p = &out[count];
            int32_t port_val = 0, status_val = 0, role_val = 0;
            memset(p, 0, sizeof(*p));
            gr_db_vec_get_blob(v_pid,    i, p->peer_id,  GR_PEER_ID_LEN);
            gr_db_vec_get_blob(v_kem,    i, p->kem_pk,   GR_KEM_PUBLIC_KEY_LEN);
            gr_db_vec_get_blob(v_sign,   i, p->sign_key, GR_PUBLIC_KEY_LEN);
            gr_db_vec_get_str (v_ip,     i, p->ip, GR_MAX_IP_LEN);
            gr_db_vec_get_i32 (v_port,   i, &port_val);
            gr_db_vec_get_i32 (v_status, i, &status_val);
            gr_db_vec_get_i32 (v_role,   i, &role_val);
            gr_db_vec_get_i64 (v_jat,    i, &p->joined_at);
            gr_db_vec_get_i64 (v_rat,    i, &p->removed_at);
            gr_db_vec_get_i64 (v_ls,     i, &p->last_seen);
            gr_db_vec_get_str (v_rr,     i, p->removed_reason, GR_MAX_NAME_LEN);
            gr_db_vec_get_blob(v_rb,     i, p->removed_by, GR_PEER_ID_LEN);
            p->port    = (uint16_t)port_val;
            p->status  = (gr_peer_status_t)status_val;
            p->role_id = (uint32_t)role_val;
            count++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    *actual_count = count;
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

bool gr_peer_is_authorized(const gr_registrar_t *reg,
                           const uint8_t peer_id[GR_PEER_ID_LEN]) {
    if (!reg || !peer_id) return false;
    gr_peer_t peer;
    if (gr_peer_get(reg, peer_id, &peer) != GR_OK) return false;
    return peer.status == GR_PEER_ACTIVE;
}
