/*
 * role.c - Group Registrar roles and permissions: role CRUD, peer↔role assignment, owner/role permission resolution via cached prepared statements.
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

#include "internal.h"

/* ── Permission helpers (used by all other files via internal.h) ── */

bool gr_is_owner(const gr_registrar_t *reg,
                 const uint8_t peer_id[GR_PEER_ID_LEN]) {
    return yumi_memcmp(reg->header.owner_id, peer_id, GR_PEER_ID_LEN) == 0;
}

uint32_t gr_get_peer_permissions(const gr_registrar_t *reg,
                                 const uint8_t peer_id[GR_PEER_ID_LEN]) {
    if (gr_is_owner(reg, peer_id)) return GR_PERM_OWNER;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_role_perms_join;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, peer_id, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);

    uint32_t perms = GR_PERM_NONE;
    if (st == DuckDBSuccess) {
        int32_t p = 0;
        if (gr_db_fetch_i32_scalar(&result, &p) == GR_OK)
            perms = (uint32_t)p;
    }
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return perms;
}

bool gr_check_perm(const gr_registrar_t *reg,
                   const gr_identity_t *signer,
                   gr_permission_t perm) {
    if (!signer) return false;
    return (gr_get_peer_permissions(reg, signer->peer_id) & (uint32_t)perm) != 0;
}

/* ── Public role API ───────────────────────────────────────────── */

gr_error_t gr_role_add(gr_registrar_t *reg,
                       const char *name,
                       uint32_t permissions,
                       const gr_identity_t *signer,
                       uint32_t *role_id_out) {
    if (!reg || !name || !signer || !role_id_out)
        return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, signer, GR_PERM_EDIT_ROLES))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    uint32_t count;
    gr_error_t err = gr_role_count(reg, &count);
    if (err != GR_OK) { GR_UNLOCK(reg); return err; }
    if (count >= GR_MAX_ROLES) { GR_UNLOCK(reg); return GR_ERR_ROLE_LIMIT; }

    duckdb_prepared_statement id_stmt = reg->ps_role_next_id;
    duckdb_clear_bindings(id_stmt);
    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(id_stmt, &result);
    if (st != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        GR_UNLOCK(reg);
        return GR_ERR_DB;
    }
    int32_t next_id = 0;
    gr_error_t ferr = gr_db_fetch_i32_scalar(&result, &next_id);
    duckdb_destroy_result(&result);
    if (ferr != GR_OK) { GR_UNLOCK(reg); return GR_ERR_DB; }
    uint32_t new_id = (uint32_t)next_id;

    uint8_t role_pk[GR_PUBLIC_KEY_LEN], role_sk[GR_SECRET_KEY_LEN];
    if (yumi_mldsa_keygen(role_pk, role_sk) != YUMI_CRYPTO_OK)
        { GR_UNLOCK(reg); return GR_ERR_CRYPTO; }
    yumi_memzero(role_sk, GR_SECRET_KEY_LEN);

    int64_t now = gr_timestamp_ms();
    duckdb_prepared_statement stmt = reg->ps_role_insert;
    duckdb_clear_bindings(stmt);

    duckdb_bind_int32(stmt, 1, (int32_t)new_id);
    duckdb_bind_varchar(stmt, 2, name);
    duckdb_bind_int32(stmt, 3, (int32_t)permissions);
    duckdb_bind_blob(stmt, 4, role_pk, GR_PUBLIC_KEY_LEN);
    duckdb_bind_int64(stmt, 5, now);
    duckdb_bind_int64(stmt, 6, now);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    *role_id_out = new_id;

    char detail[128];
    snprintf(detail, sizeof(detail), "role '%.64s' added (id=%u)", name, new_id);
    gr_error_t aerr = gr_audit_append(reg, GR_CHANGE_ROLE_ADDED, signer, NULL, detail);
    if (aerr != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return aerr; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_role_remove(gr_registrar_t *reg,
                          uint32_t role_id,
                          const gr_identity_t *signer) {
    if (!reg || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, signer, GR_PERM_EDIT_ROLES))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    gr_role_t role;
    gr_error_t err = gr_role_get(reg, role_id, &role);
    if (err != GR_OK) { GR_UNLOCK(reg); return err; }

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    /* Revert peers with this role back to member */
    duckdb_prepared_statement reset_stmt = reg->ps_peer_reset_role;
    duckdb_clear_bindings(reset_stmt);
    duckdb_bind_int32(reset_stmt, 1, (int32_t)role_id);
    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(reset_stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_prepared_statement del_stmt = reg->ps_role_delete;
    duckdb_clear_bindings(del_stmt);
    duckdb_bind_int32(del_stmt, 1, (int32_t)role_id);
    st = duckdb_execute_prepared(del_stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    char detail[128];
    snprintf(detail, sizeof(detail), "role '%.64s' removed (id=%u)",
             role.name, role_id);
    err = gr_audit_append(reg, GR_CHANGE_ROLE_REMOVED, signer, NULL, detail);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_role_set_permissions(gr_registrar_t *reg,
                                   uint32_t role_id,
                                   uint32_t new_permissions,
                                   const gr_identity_t *signer) {
    if (!reg || !signer) return GR_ERR_INVALID_PARAM;

    /* Block mutations while registrar is unverified */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, signer, GR_PERM_EDIT_ROLES))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    duckdb_prepared_statement stmt = reg->ps_role_set_perms;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)new_permissions);
    duckdb_bind_int64(stmt, 2, gr_timestamp_ms());
    duckdb_bind_int32(stmt, 3, (int32_t)role_id);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    char detail[128];
    snprintf(detail, sizeof(detail), "role %u permissions set to 0x%08X",
             role_id, new_permissions);
    gr_error_t err = gr_audit_append(reg, GR_CHANGE_ROLE_MODIFIED, signer, NULL, detail);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

/* ── Read-only operations — always allowed ─────────────────────── */

gr_error_t gr_role_get(const gr_registrar_t *reg,
                       uint32_t role_id,
                       gr_role_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_role_get;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)role_id);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_DB;
    }
    duckdb_data_chunk chunk = NULL;
    gr_error_t ferr = gr_db_fetch_first_chunk(&result, &chunk);
    if (ferr != GR_OK) {
        duckdb_destroy_result(&result);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    int32_t id_val = 0, perm_val = 0;
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 0), 0, &id_val);
    gr_db_vec_get_str (duckdb_data_chunk_get_vector(chunk, 1), 0,
                       out->name, GR_MAX_NAME_LEN);
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 2), 0, &perm_val);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 3), 0,
                       out->sign_key, GR_PUBLIC_KEY_LEN);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 4), 0,
                       &out->created_at);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 5), 0,
                       &out->modified_at);
    out->role_id     = (uint32_t)id_val;
    out->permissions = (uint32_t)perm_val;

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_role_list(const gr_registrar_t *reg,
                        gr_role_t *out, uint32_t max_count,
                        uint32_t *actual_count) {
    if (!reg || !out || !actual_count) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_role_list;
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
        duckdb_vector v_id    = duckdb_data_chunk_get_vector(chunk, 0);
        duckdb_vector v_name  = duckdb_data_chunk_get_vector(chunk, 1);
        duckdb_vector v_perms = duckdb_data_chunk_get_vector(chunk, 2);
        duckdb_vector v_sk    = duckdb_data_chunk_get_vector(chunk, 3);
        duckdb_vector v_ct    = duckdb_data_chunk_get_vector(chunk, 4);
        duckdb_vector v_mt    = duckdb_data_chunk_get_vector(chunk, 5);
        for (idx_t i = 0; i < rows && count < max_count; i++) {
            gr_role_t *r = &out[count];
            int32_t id_val = 0, perm_val = 0;
            memset(r, 0, sizeof(*r));
            gr_db_vec_get_i32 (v_id,    i, &id_val);
            gr_db_vec_get_str (v_name,  i, r->name, GR_MAX_NAME_LEN);
            gr_db_vec_get_i32 (v_perms, i, &perm_val);
            gr_db_vec_get_blob(v_sk,    i, r->sign_key, GR_PUBLIC_KEY_LEN);
            gr_db_vec_get_i64 (v_ct,    i, &r->created_at);
            gr_db_vec_get_i64 (v_mt,    i, &r->modified_at);
            r->role_id     = (uint32_t)id_val;
            r->permissions = (uint32_t)perm_val;
            count++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    *actual_count = count;
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_role_count(const gr_registrar_t *reg, uint32_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_role_count;
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

bool gr_has_permission(const gr_registrar_t *reg,
                       const gr_identity_t *identity,
                       gr_permission_t perm) {
    if (!reg || !identity) return false;
    return (gr_get_peer_permissions(reg, identity->peer_id)
            & (uint32_t)perm) != 0;
}
