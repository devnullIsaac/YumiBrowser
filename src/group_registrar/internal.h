/*
 * internal.h - Shared private header for the group_registrar implementation: DuckDB handles, locks, internal types, and helpers used by every .c file.
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

/*
 * internal.h — Shared private header for group_registrar implementation
 */

#ifndef GR_INTERNAL_H
#define GR_INTERNAL_H

#include "group_registrar.h"
#include "crypto.h"
#include <duckdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Join verification state (internal) ────────────────────────── */

typedef struct {
    gr_join_state_t      state;
    int64_t              started_at;

    uint8_t              nonce[GR_JOIN_NONCE_LEN];
    uint8_t              inviter_peer_id[GR_PEER_ID_LEN];
    uint32_t             required_attestations;

    uint8_t              ticket_owner_key[GR_PUBLIC_KEY_LEN];
    uint8_t              ticket_group_id[GR_HASH_LEN];
    uint8_t              ticket_registrar_hash[GR_HASH_LEN];

    gr_join_peer_vote_t  votes[GR_JOIN_MAX_TRACKED_PEERS];
    uint32_t             vote_count;
    uint32_t             agree_count;
    uint32_t             disagree_count;

    bool                 has_dissent;
    bool                 dissent_callback_fired;
    uint8_t              dissent_peer_id[GR_PEER_ID_LEN];
    uint8_t              dissent_owner_id[GR_PEER_ID_LEN];
    uint8_t              dissent_owner_key[GR_PUBLIC_KEY_LEN];

    bool                 small_group_bypass;
    bool                 user_override;

    gr_join_dissent_fn   dissent_callback;
    void                *dissent_user_data;
} gr_join_verify_state_t;

/* ── Opaque handle ─────────────────────────────────────────────── */

struct gr_registrar {
    duckdb_database    db;
    duckdb_connection  con;
    pthread_mutex_t    db_lock;   /* serializes prepared-statement usage */
    gr_header_t        header;
    bool               header_dirty;

    gr_join_verify_state_t *join_state;

    duckdb_prepared_statement ps_peer_get;
    duckdb_prepared_statement ps_peer_count_all;
    duckdb_prepared_statement ps_peer_count_status;
    duckdb_prepared_statement ps_peer_list_all;
    duckdb_prepared_statement ps_peer_list_status;
    duckdb_prepared_statement ps_peer_insert;
    duckdb_prepared_statement ps_peer_update_status;
    duckdb_prepared_statement ps_peer_update_addr;
    duckdb_prepared_statement ps_peer_touch;
    duckdb_prepared_statement ps_peer_set_role;
    duckdb_prepared_statement ps_peer_reset_role;

    duckdb_prepared_statement ps_role_get;
    duckdb_prepared_statement ps_role_list;
    duckdb_prepared_statement ps_role_count;
    duckdb_prepared_statement ps_role_insert;
    duckdb_prepared_statement ps_role_delete;
    duckdb_prepared_statement ps_role_set_perms;
    duckdb_prepared_statement ps_role_next_id;
    duckdb_prepared_statement ps_role_perms_join;

    duckdb_prepared_statement ps_webapp_insert;
    duckdb_prepared_statement ps_webapp_delete;
    duckdb_prepared_statement ps_webapp_check;
    duckdb_prepared_statement ps_webapp_list;
    duckdb_prepared_statement ps_webapp_count;
    duckdb_prepared_statement ps_webapp_get;
    duckdb_prepared_statement ps_webapp_update_perm_data;
    duckdb_prepared_statement ps_webapp_update_role_mask;

    duckdb_prepared_statement ps_server_insert;
    duckdb_prepared_statement ps_server_delete;
    duckdb_prepared_statement ps_server_list;
    duckdb_prepared_statement ps_server_count;
    duckdb_prepared_statement ps_server_get;

    duckdb_prepared_statement ps_epoch_get;
    duckdb_prepared_statement ps_epoch_list;
    duckdb_prepared_statement ps_epoch_count;
    duckdb_prepared_statement ps_epoch_insert;
    duckdb_prepared_statement ps_epoch_expire;

    duckdb_prepared_statement ps_audit_insert;
    duckdb_prepared_statement ps_audit_dedup;
    duckdb_prepared_statement ps_audit_list;
    duckdb_prepared_statement ps_audit_count;
    duckdb_prepared_statement ps_audit_delta;
    duckdb_prepared_statement ps_audit_actor_pk;
    duckdb_prepared_statement ps_audit_last_hash;

    duckdb_prepared_statement ps_invite_insert;
    duckdb_prepared_statement ps_invite_check;
    duckdb_prepared_statement ps_invite_invalidate;
    duckdb_prepared_statement ps_invite_mark_used;
    duckdb_prepared_statement ps_invite_list;
    duckdb_prepared_statement ps_invite_count;

    duckdb_prepared_statement ps_icon_upsert;
    duckdb_prepared_statement ps_icon_get;
    duckdb_prepared_statement ps_icon_hash;
    duckdb_prepared_statement ps_icon_delete;

    duckdb_prepared_statement ps_header_save;
    duckdb_prepared_statement ps_header_load;
    duckdb_prepared_statement ps_audit_prune;
    duckdb_prepared_statement ps_audit_fork_resolve;
    duckdb_prepared_statement ps_audit_verify_batch;
    duckdb_prepared_statement ps_audit_fork_check;
    duckdb_prepared_statement ps_audit_list_forks;

    /* Behavioral analysis */
    duckdb_prepared_statement ps_beh_actor_actions;
    duckdb_prepared_statement ps_beh_window_all;
    duckdb_prepared_statement ps_beh_stale_peers;
    duckdb_prepared_statement ps_beh_stale_count;
    duckdb_prepared_statement ps_beh_epoch_window;
    duckdb_prepared_statement ps_beh_epoch_avg;
    duckdb_prepared_statement ps_beh_peer_joined;
    duckdb_prepared_statement ps_beh_peer_removed;
    duckdb_prepared_statement ps_beh_entry_timespan;

    /* Delta merge - entity upserts */
    duckdb_prepared_statement ps_delta_peer_upsert;
    duckdb_prepared_statement ps_delta_role_upsert;
    duckdb_prepared_statement ps_delta_webapp_upsert;
    duckdb_prepared_statement ps_delta_server_upsert;
    duckdb_prepared_statement ps_delta_epoch_upsert;

    /* IP Blocklist */
    duckdb_prepared_statement ps_block_get;
    duckdb_prepared_statement ps_block_upsert;
    duckdb_prepared_statement ps_block_delete;
    duckdb_prepared_statement ps_block_expire;

    gr_behavior_config_t behavior_config;

    bool stmts_ready;
    int  txn_depth;
    gr_header_t txn_saved_header;

    gr_delta_anomaly_fn    delta_anomaly_fn;
    void                  *delta_anomaly_data;
    gr_persist_prompt_fn   persist_prompt_fn;
    void                  *persist_prompt_data;
    gr_behavior_alert_fn   behavior_alert_fn;
    void                  *behavior_alert_data;
};

/* ── DB helpers ────────────────────────────────────────────────── */

/* Recursive mutex — safe to nest.  Every public function that touches
 * a prepared statement (read or write) must bracket its DB work with
 * GR_LOCK / GR_UNLOCK so two threads sharing a registrar handle
 * (e.g. SUDP worker + test thread) never race on statement state. */
#define GR_LOCK(reg)   pthread_mutex_lock(&(reg)->db_lock)
#define GR_UNLOCK(reg) pthread_mutex_unlock(&(reg)->db_lock)

bool gr_db_exec(duckdb_connection con, const char *sql);

/* ── Chunk / vector accessors (modern DuckDB result API) ──────────
 *
 * The legacy duckdb_value_* / duckdb_row_count API is deprecated.
 * All readers in this library must:
 *
 *   duckdb_data_chunk chunk;
 *   while ((chunk = duckdb_fetch_chunk(result)) != NULL) {
 *       idx_t rows = duckdb_data_chunk_get_size(chunk);
 *       duckdb_vector v0 = duckdb_data_chunk_get_vector(chunk, 0);
 *       ... use gr_db_vec_get_* helpers ...
 *       duckdb_destroy_data_chunk(&chunk);
 *   }
 *
 * The helpers below wrap vector access with validity (NULL) checks
 * and return GR_ERR_DB on unexpected width / NULL mismatches.
 * gr_db_vec_get_str treats NULL strings as empty. */

gr_error_t gr_db_vec_valid(duckdb_vector vec, idx_t row);
gr_error_t gr_db_vec_get_blob(duckdb_vector vec, idx_t row, uint8_t *out,
                              idx_t expected_len);
gr_error_t gr_db_vec_get_str(duckdb_vector vec, idx_t row, char *out,
                             idx_t buf_size);
gr_error_t gr_db_vec_get_i32(duckdb_vector vec, idx_t row, int32_t *out);
gr_error_t gr_db_vec_get_i64(duckdb_vector vec, idx_t row, int64_t *out);
gr_error_t gr_db_vec_get_bool(duckdb_vector vec, idx_t row, bool *out);
gr_error_t gr_db_vec_get_double(duckdb_vector vec, idx_t row, double *out);

/* Variable-length blob / VARCHAR — allocates a buffer owned by the
 * caller (release via gr_free()). *out_buf is NULL when len == 0. */
gr_error_t gr_db_vec_get_blob_alloc(duckdb_vector vec, idx_t row,
                                    uint8_t **out_buf, size_t *out_len);

/* Fetch first non-empty chunk; GR_ERR_NOT_FOUND when the result is
 * empty. Caller destroys the chunk with duckdb_destroy_data_chunk(). */
gr_error_t gr_db_fetch_first_chunk(duckdb_result *result,
                                   duckdb_data_chunk *out_chunk);

/* Convenience: read a single INT64 / INT32 from (col 0, row 0). */
gr_error_t gr_db_fetch_i64_scalar(duckdb_result *result, int64_t *out);
gr_error_t gr_db_fetch_i32_scalar(duckdb_result *result, int32_t *out);

/* Decode one audit entry row from a chunk whose SELECT matches the
 * canonical 10-column audit layout:
 *   0: entry_hash     5: signature
 *   1: timestamp      6: registrar_version
 *   2: change_type    7: detail
 *   3: actor_id       8: prev_hash
 *   4: target_id      9: timestamp_ns
 */
gr_error_t gr_audit_read_row(duckdb_data_chunk chunk, idx_t row,
                       gr_audit_entry_t *out);

bool gr_db_init_schema(duckdb_connection con);
gr_error_t gr_header_load(gr_registrar_t *reg);
gr_error_t gr_header_save(gr_registrar_t *reg);

bool gr_txn_begin(gr_registrar_t *reg);
bool gr_txn_commit(gr_registrar_t *reg);
void gr_txn_rollback(gr_registrar_t *reg);

gr_error_t gr_prepare_statements(gr_registrar_t *reg);
void gr_destroy_statements(gr_registrar_t *reg);

/* ── Permission helpers ────────────────────────────────────────── */

bool gr_is_owner(const gr_registrar_t *reg,
                 const uint8_t peer_id[GR_PEER_ID_LEN]);
uint32_t gr_get_peer_permissions(const gr_registrar_t *reg,
                                 const uint8_t peer_id[GR_PEER_ID_LEN]);
bool gr_check_perm(const gr_registrar_t *reg,
                   const gr_identity_t *signer, gr_permission_t perm);

/* ── Audit helper ──────────────────────────────────────────────── */

gr_error_t gr_audit_append(gr_registrar_t *reg, gr_change_type_t change_type,
                           const gr_identity_t *actor,
                           const uint8_t target_id[GR_PEER_ID_LEN],
                           const char *detail);

gr_error_t gr_audit_enforce_retention_internal(gr_registrar_t *reg);

/* ── Estimated registrar size ──────────────────────────────────── */

size_t gr_estimate_size(const gr_registrar_t *reg);

/* ── Behavioral alert (called from audit_append) ──────────────── */

void gr_behavior_check_actor(gr_registrar_t *reg,
                             const uint8_t actor_id[GR_PEER_ID_LEN],
                             gr_change_type_t change_type);

/* ── Join verification ─────────────────────────────────────────── */

bool gr_join_is_untrusted(const gr_registrar_t *reg);

#ifdef __cplusplus
}
#endif

#endif /* GR_INTERNAL_H */
