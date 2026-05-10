/*
    Group Registrar — Signed Audit Log
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
 * @file audit.c
 * @brief Signed audit log for the Group Registrar.
 *
 * Every mutation to the registrar is recorded as a signed, hash-chained
 * entry in the gr_audit_log table.
 */

#include "internal.h"
#include "buf.h"

/* ── Column layout shared by every gr_audit_log SELECT used here.
 *    0: entry_hash         5: signature
 *    1: timestamp          6: registrar_version
 *    2: change_type        7: detail
 *    3: actor_id           8: prev_hash
 *    4: target_id          9: timestamp_ns
 * --------------------------------------------------------------- */
static void read_audit_row(duckdb_data_chunk chunk, idx_t row,
                           gr_audit_entry_t *out);

void gr_audit_read_row(duckdb_data_chunk chunk, idx_t row,
                       gr_audit_entry_t *out) {
    read_audit_row(chunk, row, out);
}

static void read_audit_row(duckdb_data_chunk chunk, idx_t row,
                           gr_audit_entry_t *out) {
    int32_t ct = 0, rv = 0;
    memset(out, 0, sizeof(*out));
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 0), row,
                       out->entry_hash, GR_HASH_LEN);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 1), row,
                       &out->timestamp);
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 2), row, &ct);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 3), row,
                       out->actor_id, GR_PEER_ID_LEN);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 4), row,
                       out->target_id, GR_PEER_ID_LEN);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 5), row,
                       out->signature, GR_SIGN_LEN);
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 6), row, &rv);
    gr_db_vec_get_str (duckdb_data_chunk_get_vector(chunk, 7), row,
                       out->detail, sizeof(out->detail));
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 8), row,
                       out->prev_hash, GR_HASH_LEN);
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 9), row,
                       &out->timestamp_ns);
    out->change_type       = (gr_change_type_t)ct;
    out->registrar_version = (uint32_t)rv;
}

/**
 * @brief Append a signed audit entry and save the updated header.
 */
gr_error_t gr_audit_append(gr_registrar_t *reg, gr_change_type_t change_type,
                           const gr_identity_t *actor,
                           const uint8_t target_id[GR_PEER_ID_LEN],
                           const char *detail) {
    gr_audit_entry_t entry;
    memset(&entry, 0, sizeof(entry));

    /* Fetch previous entry hash for chaining */
    {
        duckdb_prepared_statement ls = reg->ps_audit_last_hash;
        duckdb_clear_bindings(ls);
        duckdb_result lr;
        duckdb_state lst = duckdb_execute_prepared(ls, &lr);
        if (lst == DuckDBSuccess) {
            duckdb_data_chunk ch = NULL;
            if (gr_db_fetch_first_chunk(&lr, &ch) == GR_OK) {
                gr_db_vec_get_blob(duckdb_data_chunk_get_vector(ch, 0), 0,
                                   entry.prev_hash, GR_HASH_LEN);
                duckdb_destroy_data_chunk(&ch);
            }
            /* else prev_hash stays zeroed (genesis entry) */
        }
        duckdb_destroy_result(&lr);
    }

    entry.timestamp_ns = gr_timestamp_ns();
    entry.timestamp = entry.timestamp_ns / 1000000LL;
    entry.change_type = change_type;
    memcpy(entry.actor_id, actor->peer_id, GR_PEER_ID_LEN);
    if (target_id)
        memcpy(entry.target_id, target_id, GR_PEER_ID_LEN);
    reg->header.version++;
    entry.registrar_version = reg->header.version;
    if (detail)
        strncpy(entry.detail, detail, sizeof(entry.detail) - 1);

    /* Hash entry content (Skein-1024) — includes prev_hash for chaining */
    yumi_skein_ctx_t *skein = NULL;
    if (yumi_skein_init(&skein) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;
    yumi_skein_update(skein, entry.prev_hash, GR_HASH_LEN);
    int64_t ts_le = (int64_t)gr_htole64((uint64_t)entry.timestamp);
    yumi_skein_update(skein, (const uint8_t *)&ts_le, 8);
    uint32_t ct = gr_htole32((uint32_t)entry.change_type);
    yumi_skein_update(skein, (const uint8_t *)&ct, 4);
    yumi_skein_update(skein, entry.actor_id, GR_PEER_ID_LEN);
    yumi_skein_update(skein, entry.target_id, GR_PEER_ID_LEN);
    uint32_t rv_le = gr_htole32(entry.registrar_version);
    yumi_skein_update(skein, (const uint8_t *)&rv_le, 4);
    yumi_skein_update(skein, (const uint8_t *)entry.detail,
                      strlen(entry.detail));
    yumi_skein_final(skein, entry.entry_hash);
    yumi_skein_free(skein);

    /* Sign the hash (ML-DSA-87 signature) */
    if (yumi_mldsa_sign(entry.signature, entry.entry_hash, GR_HASH_LEN,
                        actor->secret_key) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;

    /* Insert via cached prepared statement */
    duckdb_prepared_statement stmt = reg->ps_audit_insert;
    duckdb_clear_bindings(stmt);

    duckdb_bind_blob(stmt, 1, entry.entry_hash, GR_HASH_LEN);
    duckdb_bind_int64(stmt, 2, entry.timestamp);
    duckdb_bind_int32(stmt, 3, (int32_t)entry.change_type);
    duckdb_bind_blob(stmt, 4, entry.actor_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 5, entry.target_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 6, entry.signature, GR_SIGN_LEN);
    duckdb_bind_int32(stmt, 7, (int32_t)entry.registrar_version);
    duckdb_bind_varchar(stmt, 8, entry.detail);
    duckdb_bind_blob(stmt, 9, entry.prev_hash, GR_HASH_LEN);
    duckdb_bind_int64(stmt, 10, entry.timestamp_ns);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess)
        return GR_ERR_DB;

    reg->header.updated_at = entry.timestamp;
    gr_error_t save_err = gr_header_save(reg);
    if (save_err != GR_OK)
        return save_err;

    /* Enforce audit log retention after successful append */
    gr_audit_enforce_retention_internal(reg);

    /* Per-mutation behavioral check — fires alert callback if the
     * actor who just mutated exceeds burst or abuse thresholds.    */
    gr_behavior_check_actor(reg, actor->peer_id, change_type);

    return GR_OK;
}

/**
 * @brief List audit entries after a given timestamp.
 */
gr_error_t gr_audit_list(const gr_registrar_t *reg, int64_t since_timestamp,
                         gr_audit_entry_t *out, uint32_t max_count,
                         uint32_t *actual_count) {
    if (!reg || !out || !actual_count)
        return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_audit_list;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int64(stmt, 1, since_timestamp);
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
        for (idx_t i = 0; i < rows && count < max_count; i++) {
            read_audit_row(chunk, i, &out[count]);
            count++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    *actual_count = count;
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/**
 * @brief Count all entries in the audit log.
 */
gr_error_t gr_audit_count(const gr_registrar_t *reg, uint32_t *out) {
    if (!reg || !out)
        return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_audit_count;
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

/* ═══════════════════════════════════════════════════════════════════
 *  Audit Chain Verification
 *
 *  Walks the full audit log in registrar_version order. For each
 *  entry: recomputes the Skein-1024 hash and verifies the ML-DSA-87
 *  signature against the actor's signing key from the peer table.
 *
 *  Fork detection:  In a decentralized group with multiple admins,
 *  two peers may independently create entries that reference the
 *  same prev_hash. This is an expected condition caused by network
 *  partitions or concurrent mutations — NOT an integrity failure.
 *  The function counts fork points (prev_hash values shared by
 *  more than one entry) and reports them in the result.
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_audit_verify_chain(const gr_registrar_t *reg,
                                 gr_audit_chain_result_t *result) {
    if (!reg || !result) return GR_ERR_INVALID_PARAM;
    memset(result, 0, sizeof(*result));

    GR_LOCK((gr_registrar_t *)reg);

    /* Count total entries */
    uint32_t total = 0;
    gr_error_t err = gr_audit_count(reg, &total);
    if (err != GR_OK) { GR_UNLOCK((gr_registrar_t *)reg); return err; }
    result->total_entries = total;
    if (total == 0) { GR_UNLOCK((gr_registrar_t *)reg); return GR_OK; }

    /* Count fork points (prev_hash values referenced by >1 entry)
     * and resolve ordering by nanosecond timestamp + entry_hash. */
    {
        duckdb_prepared_statement fs = reg->ps_audit_fork_resolve;
        duckdb_clear_bindings(fs);
        duckdb_result fr;
        if (duckdb_execute_prepared(fs, &fr) == DuckDBSuccess) {
            uint8_t last_prev[GR_HASH_LEN];
            memset(last_prev, 0xFF, GR_HASH_LEN);
            duckdb_data_chunk fchunk;
            while ((fchunk = duckdb_fetch_chunk(fr)) != NULL) {
                idx_t frows = duckdb_data_chunk_get_size(fchunk);
                duckdb_vector v_ph =
                    duckdb_data_chunk_get_vector(fchunk, 2);
                for (idx_t fi = 0; fi < frows; fi++) {
                    uint8_t ph[GR_HASH_LEN];
                    gr_db_vec_get_blob(v_ph, fi, ph, GR_HASH_LEN);
                    if (yumi_memcmp(ph, last_prev, GR_HASH_LEN) != 0) {
                        result->forks_detected++;
                        memcpy(last_prev, ph, GR_HASH_LEN);
                    } else {
                        result->forks_resolved++;
                    }
                }
                duckdb_destroy_data_chunk(&fchunk);
            }
        }
        duckdb_destroy_result(&fr);
    }

    /* Walk entries in batches, verify hash + signature */
    static const uint32_t BATCH = 512;
    uint8_t zero_hash[GR_HASH_LEN];
    memset(zero_hash, 0, GR_HASH_LEN);

    for (uint32_t offset = 0; offset < total; offset += BATCH) {
        duckdb_prepared_statement vs = reg->ps_audit_verify_batch;
        duckdb_clear_bindings(vs);
        duckdb_bind_int32(vs, 1, (int32_t)BATCH);
        duckdb_bind_int32(vs, 2, (int32_t)offset);

        duckdb_result vr;
        if (duckdb_execute_prepared(vs, &vr) != DuckDBSuccess) {
            duckdb_destroy_result(&vr);
            GR_UNLOCK((gr_registrar_t *)reg);
            return GR_ERR_DB;
        }

        duckdb_data_chunk vchunk;
        while ((vchunk = duckdb_fetch_chunk(vr)) != NULL) {
            idx_t rows = duckdb_data_chunk_get_size(vchunk);
            for (idx_t i = 0; i < rows; i++) {
                gr_audit_entry_t entry;
                read_audit_row(vchunk, i, &entry);

                /* Check for genesis entry (all-zero prev_hash) */
                if (yumi_memcmp(entry.prev_hash, zero_hash, GR_HASH_LEN) == 0)
                    result->has_genesis = true;

                /* Recompute hash (Skein-1024) */
                uint8_t computed[GR_HASH_LEN];
                yumi_skein_ctx_t *gs = NULL;
                if (yumi_skein_init(&gs) != YUMI_CRYPTO_OK) {
                    duckdb_destroy_data_chunk(&vchunk);
                    duckdb_destroy_result(&vr);
                    GR_UNLOCK((gr_registrar_t *)reg);
                    return GR_ERR_CRYPTO;
                }
                yumi_skein_update(gs, entry.prev_hash, GR_HASH_LEN);
                int64_t ts_le = (int64_t)gr_htole64((uint64_t)entry.timestamp);
                yumi_skein_update(gs, (const uint8_t *)&ts_le, 8);
                uint32_t ct = gr_htole32((uint32_t)entry.change_type);
                yumi_skein_update(gs, (const uint8_t *)&ct, 4);
                yumi_skein_update(gs, entry.actor_id, GR_PEER_ID_LEN);
                yumi_skein_update(gs, entry.target_id, GR_PEER_ID_LEN);
                uint32_t rv_le = gr_htole32(entry.registrar_version);
                yumi_skein_update(gs, (const uint8_t *)&rv_le, 4);
                yumi_skein_update(gs, (const uint8_t *)entry.detail,
                                          strlen(entry.detail));
                yumi_skein_final(gs, computed);
                yumi_skein_free(gs);

                if (yumi_memcmp(computed, entry.entry_hash, GR_HASH_LEN) != 0) {
                    result->invalid_hash++;
                    continue;
                }

                /* Look up actor's signing key */
                duckdb_prepared_statement pk_stmt = reg->ps_audit_actor_pk;
                duckdb_clear_bindings(pk_stmt);
                duckdb_bind_blob(pk_stmt, 1, entry.actor_id, GR_PEER_ID_LEN);
                duckdb_result pkr;
                duckdb_state pst = duckdb_execute_prepared(pk_stmt, &pkr);
                duckdb_data_chunk pk_chunk = NULL;
                if (pst != DuckDBSuccess ||
                    gr_db_fetch_first_chunk(&pkr, &pk_chunk) != GR_OK) {
                    duckdb_destroy_result(&pkr);
                    result->unknown_actor++;
                    continue;
                }
                uint8_t actor_pk[GR_PUBLIC_KEY_LEN];
                gr_db_vec_get_blob(duckdb_data_chunk_get_vector(pk_chunk, 0), 0,
                                   actor_pk, GR_PUBLIC_KEY_LEN);
                duckdb_destroy_data_chunk(&pk_chunk);
                duckdb_destroy_result(&pkr);

                /* Verify ML-DSA-87 signature */
                if (yumi_mldsa_verify(entry.signature, entry.entry_hash,
                                       GR_HASH_LEN, actor_pk) != YUMI_CRYPTO_OK) {
                    result->invalid_signature++;
                    continue;
                }

                result->verified_entries++;
            }
            duckdb_destroy_data_chunk(&vchunk);
        }
        duckdb_destroy_result(&vr);
    }

    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Audit Log Retention Enforcement
 *
 *  Maintains a 25 MB ceiling (GR_AUDIT_MAX_BYTES) for the audit log
 *  by deleting the oldest entries when the estimated size exceeds
 *  the limit. Uses GR_AUDIT_EST_ROW_BYTES (~500 bytes) as a
 *  conservative per-row estimate.
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_audit_enforce_retention_internal(gr_registrar_t *reg) {
    uint32_t count = 0;
    gr_error_t err = gr_audit_count(reg, &count);
    if (err != GR_OK) return err;

    uint32_t max_rows = (uint32_t)(GR_AUDIT_MAX_BYTES / GR_AUDIT_EST_ROW_BYTES);
    if (count <= max_rows) return GR_OK;

    uint32_t excess = count - max_rows;

    duckdb_prepared_statement stmt = reg->ps_audit_prune;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)excess);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);

    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

gr_error_t gr_audit_enforce_retention(gr_registrar_t *reg) {
    if (!reg) return GR_ERR_INVALID_PARAM;
    GR_LOCK(reg);
    gr_error_t err = gr_audit_enforce_retention_internal(reg);
    GR_UNLOCK(reg);
    return err;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Audit Fork Listing
 *
 *  Returns fork points where two or more audit entries share the
 *  same prev_hash — evidence of concurrent mutations during a
 *  network partition. Both branches are preserved in the log;
 *  this function groups them for the owner to inspect.
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_audit_list_forks(const gr_registrar_t *reg,
                               gr_audit_fork_t *out,
                               uint32_t max_forks,
                               uint32_t *actual_count) {
    if (!reg || !out || !actual_count) return GR_ERR_INVALID_PARAM;
    *actual_count = 0;
    if (max_forks == 0) return GR_OK;

    GR_LOCK((gr_registrar_t *)reg);

    uint32_t row_limit = max_forks * GR_FORK_MAX_BRANCHES;

    duckdb_prepared_statement stmt = reg->ps_audit_list_forks;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)row_limit);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_DB;
    }

    uint32_t fork_idx = 0;
    uint8_t current_prev[GR_HASH_LEN];
    bool first = true;

    duckdb_data_chunk chunk;
    while (fork_idx < max_forks &&
           (chunk = duckdb_fetch_chunk(result)) != NULL) {
        idx_t rows = duckdb_data_chunk_get_size(chunk);
        for (idx_t i = 0; i < rows && fork_idx < max_forks; i++) {
            gr_audit_entry_t entry;
            read_audit_row(chunk, i, &entry);

            bool new_group = first ||
                yumi_memcmp(entry.prev_hash, current_prev, GR_HASH_LEN) != 0;

            if (new_group) {
                if (!first) fork_idx++;
                if (fork_idx >= max_forks) break;

                memset(&out[fork_idx], 0, sizeof(gr_audit_fork_t));
                memcpy(out[fork_idx].prev_hash, entry.prev_hash, GR_HASH_LEN);
                memcpy(current_prev, entry.prev_hash, GR_HASH_LEN);
                first = false;
            }

            if (out[fork_idx].branch_count < GR_FORK_MAX_BRANCHES) {
                out[fork_idx].branches[out[fork_idx].branch_count++] = entry;
            }
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    if (!first && fork_idx < max_forks) fork_idx++;
    *actual_count = fork_idx;
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}
