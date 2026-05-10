/*
    Group Registrar — Invite Creation, Parsing, and Lifecycle
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
 * @file invite.c
 * @brief Invite creation, parsing, and lifecycle management.
 *
 * The invite is a lightweight ticket that does NOT contain the
 * full registrar. It provides enough information for the invitee to:
 *   1. Identify the group (group_id, name, type)
 *   2. Know the claimed owner (owner_sign_key) for cross-checking
 *   3. Locate the group on the network (bootstrap peers + signaling)
 *   4. Verify the ticket's authenticity (inviter ML-DSA signature)
 *   5. Initiate key exchange (inviter's ML-KEM-1024 public key)
 *
 * Wire format (v3):
 *   "GRINV" (5 bytes magic)
 *   u32     version = GR_INVITE_VERSION (3)
 *   [128]   group_id
 *   str     group_name (length-prefixed)
 *   u32     group_type
 *   [2592]  owner_sign_key
 *   [128]   registrar_hash
 *   [128]   verification_token
 *   [2592]  inviter_sign_pk (ML-DSA-87)
 *   [1568]  inviter_kem_pk  (ML-KEM-1024)
 *   i64     expires_at
 *   u32     bootstrap_peer_count (max GR_INVITE_MAX_BOOTSTRAP)
 *     per peer:
 *       [32]    peer_id
 *       str     ip
 *       u16     port
 *       [2592]  sign_key (ML-DSA pk)
 *   u32     signaling_server_count
 *     per server:
 *       [128]   id_hash
 *       str     ip
 *       u16     port
 *       [2592]  sign_key (ML-DSA pk)
 *   [4627]  inviter ML-DSA-87 signature (over all preceding bytes)
 */

#include "internal.h"
#include "buf.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Internal: select random bootstrap peers
 * ═══════════════════════════════════════════════════════════════════ */

static gr_error_t select_bootstrap_peers(const gr_registrar_t *reg,
                                         const uint8_t inviter_id[GR_PEER_ID_LEN],
                                         gr_bootstrap_peer_t *out,
                                         uint32_t max_count,
                                         uint32_t *actual_count) {
    *actual_count = 0;

    uint32_t total;
    gr_error_t err = gr_peer_count(reg, GR_PEER_ACTIVE, &total);
    if (err != GR_OK) return err;
    if (total == 0) return GR_OK;

    gr_peer_t *all = (gr_peer_t *)calloc(total, sizeof(gr_peer_t));
    if (!all) return GR_ERR_OUT_OF_MEMORY;

    uint32_t fetched;
    err = gr_peer_list(reg, all, total, &fetched, GR_PEER_ACTIVE);
    if (err != GR_OK) { free(all); return err; }

    gr_peer_t *candidates = (gr_peer_t *)calloc(fetched, sizeof(gr_peer_t));
    if (!candidates) { free(all); return GR_ERR_OUT_OF_MEMORY; }

    uint32_t cand_count = 0;
    for (uint32_t i = 0; i < fetched; i++) {
        if (yumi_memcmp(all[i].peer_id, inviter_id, GR_PEER_ID_LEN) == 0)
            continue;
        if (gr_is_owner(reg, all[i].peer_id))
            continue;
        if (all[i].ip[0] == '\0')
            continue;
        candidates[cand_count++] = all[i];
    }
    free(all);

    /* Fisher-Yates shuffle */
    for (uint32_t i = cand_count; i > 1; i--) {
        uint32_t j;
        yumi_randombytes((uint8_t *)&j, sizeof(j));
        j = j % i;
        gr_peer_t tmp = candidates[i - 1];
        candidates[i - 1] = candidates[j];
        candidates[j] = tmp;
    }

    uint32_t pick = cand_count < max_count ? cand_count : max_count;
    for (uint32_t i = 0; i < pick; i++) {
        memcpy(out[i].peer_id, candidates[i].peer_id, GR_PEER_ID_LEN);
        strncpy(out[i].ip, candidates[i].ip, GR_MAX_IP_LEN - 1);
        out[i].ip[GR_MAX_IP_LEN - 1] = '\0';
        out[i].port = candidates[i].port;
        memcpy(out[i].sign_key, candidates[i].sign_key, GR_PUBLIC_KEY_LEN);
    }

    *actual_count = pick;
    free(candidates);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_invite_create(gr_registrar_t *reg,
                            const gr_identity_t *inviter,
                            int64_t expiry_timestamp_ms,
                            uint8_t **out_data, size_t *out_len,
                            uint8_t verification_token_out[GR_HASH_LEN]) {
    if (!reg || !inviter || !out_data || !out_len || !verification_token_out)
        return GR_ERR_INVALID_PARAM;

    /* Block during unverified state */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, inviter, GR_PERM_INVITE_MEMBER))
        return GR_ERR_UNAUTHORIZED;

    if (yumi_randombytes(verification_token_out, GR_HASH_LEN) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;
    int64_t now = gr_timestamp_ms();

    GR_LOCK(reg);

    /* Store invite in DB */
    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_prepared_statement stmt = reg->ps_invite_insert;
    duckdb_clear_bindings(stmt);

    uint8_t zeroed[GR_PEER_ID_LEN] = {0};
    duckdb_bind_blob(stmt, 1, verification_token_out, GR_HASH_LEN);
    duckdb_bind_int64(stmt, 2, now);
    duckdb_bind_int64(stmt, 3, expiry_timestamp_ms);
    duckdb_bind_blob(stmt, 4, inviter->peer_id, GR_PEER_ID_LEN);
    duckdb_bind_boolean(stmt, 5, false);
    duckdb_bind_boolean(stmt, 6, false);
    duckdb_bind_blob(stmt, 7, zeroed, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    /* Select bootstrap peers */
    gr_bootstrap_peer_t bootstrap[GR_INVITE_MAX_BOOTSTRAP];
    uint32_t bootstrap_count;
    gr_error_t err = select_bootstrap_peers(reg, inviter->peer_id,
                                            bootstrap,
                                            GR_INVITE_MAX_BOOTSTRAP,
                                            &bootstrap_count);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }

    /* Build invite blob */
    gr_buf_t buf;
    if (gr_buf_init(&buf, 2048) != 0) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_OUT_OF_MEMORY; }

    gr_buf_write(&buf, "GRINV", 5);
    gr_buf_write_u32(&buf, GR_INVITE_VERSION);

    gr_buf_write(&buf, reg->header.group_id, GR_HASH_LEN);
    gr_buf_write_str(&buf, reg->header.group_name, GR_MAX_NAME_LEN);
    gr_buf_write_u32(&buf, (uint32_t)reg->header.group_type);
    gr_buf_write(&buf, reg->header.owner_sign_key, GR_PUBLIC_KEY_LEN);
    gr_buf_write(&buf, reg->header.hash, GR_HASH_LEN);
    gr_buf_write(&buf, verification_token_out, GR_HASH_LEN);
    gr_buf_write(&buf, inviter->public_key, GR_PUBLIC_KEY_LEN);
    gr_buf_write(&buf, inviter->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    gr_buf_write_i64(&buf, expiry_timestamp_ms);

    gr_buf_write_u32(&buf, bootstrap_count);
    for (uint32_t i = 0; i < bootstrap_count; i++) {
        gr_buf_write(&buf, bootstrap[i].peer_id, GR_PEER_ID_LEN);
        gr_buf_write_str(&buf, bootstrap[i].ip, GR_MAX_IP_LEN);
        gr_buf_write_u16(&buf, bootstrap[i].port);
        gr_buf_write(&buf, bootstrap[i].sign_key, GR_PUBLIC_KEY_LEN);
    }

    uint32_t sig_count;
    gr_server_count(reg, GR_SERVER_SIGNALING, &sig_count);
    gr_buf_write_u32(&buf, sig_count);

    if (sig_count > 0) {
        gr_server_t *servers = (gr_server_t *)calloc(sig_count,
                                                      sizeof(gr_server_t));
        if (servers) {
            uint32_t actual;
            gr_server_list(reg, GR_SERVER_SIGNALING, servers,
                           sig_count, &actual);
            for (uint32_t i = 0; i < actual; i++) {
                gr_buf_write(&buf, servers[i].id_hash, GR_HASH_LEN);
                gr_buf_write_str(&buf, servers[i].ip, GR_MAX_IP_LEN);
                gr_buf_write_u16(&buf, servers[i].port);
                gr_buf_write(&buf, servers[i].sign_key, GR_PUBLIC_KEY_LEN);
            }
            free(servers);
        }
    }

    /* Check for buffer OOM before signing */
    if (buf.error) { gr_buf_free(&buf); gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_OUT_OF_MEMORY; }

    /* Sign entire blob with inviter's ML-DSA-87 key */
    uint8_t sig[GR_SIGN_LEN];
    if (yumi_mldsa_sign(sig, buf.data, buf.len,
                        inviter->secret_key) != YUMI_CRYPTO_OK) {
        gr_buf_free(&buf); gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_CRYPTO;
    }
    gr_buf_write(&buf, sig, GR_SIGN_LEN);

    err = gr_audit_append(reg, GR_CHANGE_INVITE_CREATED,
                    inviter, NULL, "invite created");
    if (err != GR_OK) { gr_buf_free(&buf); gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_buf_free(&buf); gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    GR_UNLOCK(reg);
    *out_data = buf.data;
    *out_len = buf.len;
    return GR_OK;
}

gr_error_t gr_invite_parse(const uint8_t *invite_data, size_t invite_len,
                           gr_invite_ticket_t *ticket_out) {
    if (!invite_data || !ticket_out)
        return GR_ERR_INVALID_PARAM;

    gr_reader_t r = { .data = invite_data, .len = invite_len, .pos = 0 };
    memset(ticket_out, 0, sizeof(*ticket_out));

    char magic[5];
    if (gr_read_bytes(&r, magic, 5) != 0) return GR_ERR_INVITE_INVALID;
    if (memcmp(magic, "GRINV", 5) != 0) return GR_ERR_INVITE_INVALID;

    uint32_t version;
    if (gr_read_u32(&r, &version) != 0) return GR_ERR_INVITE_INVALID;
    if (version > GR_INVITE_VERSION) return GR_ERR_INVITE_INVALID;

    if (gr_read_bytes(&r, ticket_out->group_id, GR_HASH_LEN) != 0)
        return GR_ERR_INVITE_INVALID;
    if (gr_read_str(&r, ticket_out->group_name, GR_MAX_NAME_LEN) != 0)
        return GR_ERR_INVITE_INVALID;
    uint32_t gt;
    if (gr_read_u32(&r, &gt) != 0) return GR_ERR_INVITE_INVALID;
    ticket_out->group_type = (gr_group_type_t)gt;

    if (gr_read_bytes(&r, ticket_out->owner_sign_key, GR_PUBLIC_KEY_LEN) != 0)
        return GR_ERR_INVITE_INVALID;
    if (gr_read_bytes(&r, ticket_out->registrar_hash, GR_HASH_LEN) != 0)
        return GR_ERR_INVITE_INVALID;
    if (gr_read_bytes(&r, ticket_out->verification_token, GR_HASH_LEN) != 0)
        return GR_ERR_INVITE_INVALID;
    if (gr_read_bytes(&r, ticket_out->inviter_sign_pk, GR_PUBLIC_KEY_LEN) != 0)
        return GR_ERR_INVITE_INVALID;
    if (gr_read_bytes(&r, ticket_out->inviter_kem_pk, GR_KEM_PUBLIC_KEY_LEN) != 0)
        return GR_ERR_INVITE_INVALID;
    if (gr_read_i64(&r, &ticket_out->expires_at) != 0)
        return GR_ERR_INVITE_INVALID;

    if (ticket_out->expires_at > 0 &&
        gr_timestamp_ms() >= ticket_out->expires_at)
        return GR_ERR_INVITE_EXPIRED;

    /* Bootstrap peers */
    if (gr_read_u32(&r, &ticket_out->bootstrap_count) != 0)
        return GR_ERR_INVITE_INVALID;
    if (ticket_out->bootstrap_count > GR_INVITE_MAX_BOOTSTRAP)
        return GR_ERR_INVITE_INVALID;

    for (uint32_t i = 0; i < ticket_out->bootstrap_count; i++) {
        gr_bootstrap_peer_t *bp = &ticket_out->bootstrap_peers[i];
        if (gr_read_bytes(&r, bp->peer_id, GR_PEER_ID_LEN) != 0)
            return GR_ERR_INVITE_INVALID;
        if (gr_read_str(&r, bp->ip, GR_MAX_IP_LEN) != 0)
            return GR_ERR_INVITE_INVALID;
        if (gr_read_u16(&r, &bp->port) != 0)
            return GR_ERR_INVITE_INVALID;
        if (gr_read_bytes(&r, bp->sign_key, GR_PUBLIC_KEY_LEN) != 0)
            return GR_ERR_INVITE_INVALID;
    }

    /* Signaling servers — skip content, just count */
    if (gr_read_u32(&r, &ticket_out->signaling_count) != 0)
        return GR_ERR_INVITE_INVALID;

    for (uint32_t i = 0; i < ticket_out->signaling_count; i++) {
        uint8_t h[GR_HASH_LEN]; char ip[GR_MAX_IP_LEN]; uint16_t p;
        uint8_t pk[GR_PUBLIC_KEY_LEN];
        if (gr_read_bytes(&r, h, GR_HASH_LEN) != 0) return GR_ERR_INVITE_INVALID;
        if (gr_read_str(&r, ip, GR_MAX_IP_LEN) != 0) return GR_ERR_INVITE_INVALID;
        if (gr_read_u16(&r, &p) != 0) return GR_ERR_INVITE_INVALID;
        if (gr_read_bytes(&r, pk, GR_PUBLIC_KEY_LEN) != 0)
            return GR_ERR_INVITE_INVALID;
    }

    /* Verify inviter signature */
    size_t signed_len = r.pos;
    if (r.pos + GR_SIGN_LEN > r.len) return GR_ERR_INVITE_INVALID;

    const uint8_t *sig = r.data + r.pos;
    if (yumi_mldsa_verify(sig, invite_data, signed_len,
                           ticket_out->inviter_sign_pk) != YUMI_CRYPTO_OK)
        return GR_ERR_SIGNATURE_INVALID;

    return GR_OK;
}

/* ── Remaining invite operations ───────────────────────────────── */

gr_error_t gr_invite_invalidate(gr_registrar_t *reg,
                                const uint8_t verification_token[GR_HASH_LEN],
                                const gr_identity_t *admin) {
    if (!reg || !verification_token || !admin) return GR_ERR_INVALID_PARAM;
    if (!gr_check_perm(reg, admin, GR_PERM_INVITE_MEMBER))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    duckdb_prepared_statement stmt = reg->ps_invite_invalidate;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, verification_token, GR_HASH_LEN);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) { duckdb_destroy_result(&result); gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    idx_t changed = duckdb_rows_changed(&result);
    duckdb_destroy_result(&result);
    if (changed == 0) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_NOT_FOUND; }

    gr_error_t err = gr_audit_append(reg, GR_CHANGE_INVITE_INVALIDATED, admin, NULL,
                           "invite invalidated");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_invite_check(const gr_registrar_t *reg,
                           const uint8_t verification_token[GR_HASH_LEN],
                           bool *valid_out) {
    if (!reg || !verification_token || !valid_out)
        return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_invite_check;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, verification_token, GR_HASH_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) { duckdb_destroy_result(&result); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_DB; }
    duckdb_data_chunk chunk = NULL;
    if (gr_db_fetch_first_chunk(&result, &chunk) != GR_OK) {
        duckdb_destroy_result(&result); *valid_out = false; GR_UNLOCK((gr_registrar_t *)reg); return GR_OK;
    }

    bool inv = false, used = false;
    int64_t exp = 0;
    gr_db_vec_get_bool(duckdb_data_chunk_get_vector(chunk, 0), 0, &inv);
    gr_db_vec_get_bool(duckdb_data_chunk_get_vector(chunk, 1), 0, &used);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 2), 0, &exp);
    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);

    int64_t now = gr_timestamp_ms();
    *valid_out = !inv && !used && (exp == 0 || now < exp);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_invite_mark_used(gr_registrar_t *reg,
                               const uint8_t verification_token[GR_HASH_LEN],
                               const uint8_t peer_id[GR_PEER_ID_LEN],
                               const gr_identity_t *signer) {
    if (!reg || !verification_token || !peer_id || !signer)
        return GR_ERR_INVALID_PARAM;

    GR_LOCK(reg);

    bool valid;
    gr_error_t err = gr_invite_check(reg, verification_token, &valid);
    if (err != GR_OK) { GR_UNLOCK(reg); return err; }
    if (!valid) { GR_UNLOCK(reg); return GR_ERR_INVITE_EXPIRED; }

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_prepared_statement stmt = reg->ps_invite_mark_used;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, peer_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 2, verification_token, GR_HASH_LEN);
    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }

    err = gr_audit_append(reg, GR_CHANGE_INVITE_USED, signer, peer_id,
                          "invite redeemed");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_invite_list(const gr_registrar_t *reg,
                          gr_invite_info_t *out, uint32_t max_count,
                          uint32_t *actual_count) {
    if (!reg || !out || !actual_count) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_invite_list;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)max_count);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) { duckdb_destroy_result(&result); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_DB; }

    uint32_t count = 0;
    duckdb_data_chunk chunk;
    while (count < max_count &&
           (chunk = duckdb_fetch_chunk(result)) != NULL) {
        idx_t rows = duckdb_data_chunk_get_size(chunk);
        duckdb_vector v_tok = duckdb_data_chunk_get_vector(chunk, 0);
        duckdb_vector v_ct  = duckdb_data_chunk_get_vector(chunk, 1);
        duckdb_vector v_et  = duckdb_data_chunk_get_vector(chunk, 2);
        duckdb_vector v_cb  = duckdb_data_chunk_get_vector(chunk, 3);
        duckdb_vector v_inv = duckdb_data_chunk_get_vector(chunk, 4);
        duckdb_vector v_use = duckdb_data_chunk_get_vector(chunk, 5);
        duckdb_vector v_ub  = duckdb_data_chunk_get_vector(chunk, 6);
        for (idx_t i = 0; i < rows && count < max_count; i++) {
            gr_invite_info_t *info = &out[count];
            memset(info, 0, sizeof(*info));
            gr_db_vec_get_blob(v_tok, i, info->verification_token, GR_HASH_LEN);
            gr_db_vec_get_i64 (v_ct,  i, &info->created_at);
            gr_db_vec_get_i64 (v_et,  i, &info->expires_at);
            gr_db_vec_get_blob(v_cb,  i, info->created_by, GR_PEER_ID_LEN);
            gr_db_vec_get_bool(v_inv, i, &info->invalidated);
            gr_db_vec_get_bool(v_use, i, &info->used);
            gr_db_vec_get_blob(v_ub,  i, info->used_by, GR_PEER_ID_LEN);
            count++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    *actual_count = count;
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_invite_count(const gr_registrar_t *reg, uint32_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_invite_count;
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
