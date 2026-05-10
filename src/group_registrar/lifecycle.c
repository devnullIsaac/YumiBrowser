/*
    Group Registrar — Handle Lifecycle (create / open / close)
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

/*
 * lifecycle.c — gr_create, gr_open, gr_close, gr_get_header
 *
 * Manages the full lifecycle of a gr_registrar_t handle:
 *   - Database open/close
 *   - Schema initialization
 *   - Prepared statement caching
 *   - Header cache management
 *   - Owner peer and first epoch creation
 *   - Join verification state cleanup
 */

#include "internal.h"

/* ── Internal: cleanup helper for error paths ──────────────────── */

static void teardown(gr_registrar_t *reg) {
    if (!reg) return;
    gr_destroy_statements(reg);
    if (reg->join_state) {
        yumi_memzero(reg->join_state, sizeof(gr_join_verify_state_t));
        free(reg->join_state);
        reg->join_state = NULL;
    }
    duckdb_disconnect(&reg->con);
    duckdb_close(&reg->db);
    pthread_mutex_destroy(&reg->db_lock);
    yumi_memzero(&reg->header, sizeof(reg->header));
    free(reg);
}

/* ── Internal: common init sequence after DB is open ───────────── */

static gr_error_t init_common(gr_registrar_t *reg) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(&reg->db_lock, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return GR_ERR_OUT_OF_MEMORY;
    }
    pthread_mutexattr_destroy(&attr);

    if (!gr_db_init_schema(reg->con))
        return GR_ERR_DB;

    gr_error_t err = gr_prepare_statements(reg);
    if (err != GR_OK) return err;

    return GR_OK;
}

/* ── Internal: insert the owner as the first peer ──────────────── */

static gr_error_t insert_owner_peer(gr_registrar_t *reg,
                                    const gr_identity_t *owner,
                                    int64_t now) {
    duckdb_prepared_statement stmt = reg->ps_peer_insert;
    duckdb_clear_bindings(stmt);

    uint8_t zeroed_id[GR_PEER_ID_LEN];
    memset(zeroed_id, 0, GR_PEER_ID_LEN);

    duckdb_bind_blob(stmt, 1,  owner->peer_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 2,  owner->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    duckdb_bind_blob(stmt, 3,  owner->public_key, GR_PUBLIC_KEY_LEN); /* sign_key */
    duckdb_bind_varchar(stmt, 4, "");       /* ip: unknown at creation */
    duckdb_bind_int32(stmt, 5,  0);         /* port */
    duckdb_bind_int32(stmt, 6,  GR_PEER_ACTIVE);
    duckdb_bind_int32(stmt, 7,  0);         /* role_id: owner has no role row */
    duckdb_bind_int64(stmt, 8,  now);       /* joined_at */
    duckdb_bind_int64(stmt, 9,  0);         /* removed_at */
    duckdb_bind_int64(stmt, 10, now);       /* last_seen */
    duckdb_bind_varchar(stmt, 11, "");      /* removed_reason */
    duckdb_bind_blob(stmt, 12, zeroed_id, GR_PEER_ID_LEN); /* removed_by */

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);

    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

/* ── Internal: generate and insert the first epoch key ─────────── */

static gr_error_t insert_first_epoch(gr_registrar_t *reg,
                                     const gr_identity_t *owner,
                                     int64_t now) {
    uint8_t epoch_key[GR_EPOCH_KEY_LEN];
    if (yumi_randombytes(epoch_key, GR_EPOCH_KEY_LEN) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;

    duckdb_prepared_statement stmt = reg->ps_epoch_insert;
    duckdb_clear_bindings(stmt);

    duckdb_bind_int32(stmt, 1, 1);                          /* epoch_id = 1 */
    duckdb_bind_blob(stmt, 2, epoch_key, GR_EPOCH_KEY_LEN);
    duckdb_bind_int64(stmt, 3, now);                         /* created_at */
    duckdb_bind_int64(stmt, 4, 0);                           /* expired_at: current */
    duckdb_bind_blob(stmt, 5, owner->peer_id, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);

    /* Wipe key from stack immediately */
    yumi_memzero(epoch_key, GR_EPOCH_KEY_LEN);

    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

/* ── Internal: build the initial header ────────────────────────── */

static void build_initial_header(gr_header_t *h,
                                 const char *group_name,
                                 gr_group_type_t group_type,
                                 const gr_identity_t *owner,
                                 int64_t now) {
    memset(h, 0, sizeof(*h));

    /* group_id = Skein-1024(owner_sign_pk || group_name || timestamp) */
    yumi_skein_ctx_t *gs = NULL;
    yumi_skein_init(&gs);
    yumi_skein_update(gs, owner->public_key, GR_PUBLIC_KEY_LEN);
    yumi_skein_update(gs, (const uint8_t *)group_name,
                       strlen(group_name));
    yumi_skein_update(gs, (const uint8_t *)&now, sizeof(now));
    uint8_t full_hash[YUMI_SKEIN_HASH_LEN];
    yumi_skein_final(gs, full_hash);
    yumi_skein_free(gs);
    memcpy(h->group_id, full_hash, GR_HASH_LEN);
    yumi_memzero(full_hash, YUMI_SKEIN_HASH_LEN);

    h->group_type = group_type;
    strncpy(h->group_name, group_name, GR_MAX_NAME_LEN - 1);
    h->group_name[GR_MAX_NAME_LEN - 1] = '\0';

    h->version = 1;
    h->created_at = now;
    h->updated_at = now;
    h->epoch_id = 1;
    h->retention = gr_retention_defaults();

    memcpy(h->owner_id, owner->peer_id, GR_PEER_ID_LEN);
    memcpy(h->owner_sign_key, owner->public_key, GR_PUBLIC_KEY_LEN);
}

/* ════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════ */

gr_error_t gr_create(gr_registrar_t **out,
                     const char *db_path,
                     const char *group_name,
                     gr_group_type_t group_type,
                     const gr_identity_t *owner) {
    if (!out || !db_path || !group_name || !owner)
        return GR_ERR_INVALID_PARAM;

    if (group_name[0] == '\0')
        return GR_ERR_INVALID_PARAM;

    if (group_type != GR_GROUP_PRIVATE && group_type != GR_GROUP_PUBLIC)
        return GR_ERR_INVALID_PARAM;

    *out = NULL;

    gr_registrar_t *reg = (gr_registrar_t *)calloc(1, sizeof(gr_registrar_t));
    if (!reg) return GR_ERR_OUT_OF_MEMORY;

    reg->behavior_config = gr_behavior_config_defaults();

    /* join_state is NULL — locally created registrars are implicitly trusted */
    reg->join_state = NULL;

    if (duckdb_open(db_path, &reg->db) != DuckDBSuccess) {
        free(reg);
        return GR_ERR_DB;
    }
    if (duckdb_connect(reg->db, &reg->con) != DuckDBSuccess) {
        duckdb_close(&reg->db);
        free(reg);
        return GR_ERR_DB;
    }

    gr_error_t err = init_common(reg);
    if (err != GR_OK) {
        teardown(reg);
        return err;
    }

    int64_t now = gr_timestamp_ms();

    build_initial_header(&reg->header, group_name, group_type, owner, now);

    err = gr_header_save(reg);
    if (err != GR_OK) {
        teardown(reg);
        return err;
    }

    err = insert_owner_peer(reg, owner, now);
    if (err != GR_OK) {
        teardown(reg);
        return err;
    }

    err = insert_first_epoch(reg, owner, now);
    if (err != GR_OK) {
        teardown(reg);
        return err;
    }

    err = gr_audit_append(reg, GR_CHANGE_PEER_ADDED, owner,
                          owner->peer_id, "group created");
    if (err != GR_OK) {
        teardown(reg);
        return err;
    }

    *out = reg;
    return GR_OK;
}

gr_error_t gr_open(gr_registrar_t **out,
                   const char *db_path,
                   const uint8_t group_id[GR_HASH_LEN]) {
    if (!out || !db_path || !group_id)
        return GR_ERR_INVALID_PARAM;

    *out = NULL;

    gr_registrar_t *reg = (gr_registrar_t *)calloc(1, sizeof(gr_registrar_t));
    if (!reg) return GR_ERR_OUT_OF_MEMORY;

    reg->behavior_config = gr_behavior_config_defaults();
    reg->join_state = NULL;

    if (duckdb_open(db_path, &reg->db) != DuckDBSuccess) {
        free(reg);
        return GR_ERR_DB;
    }
    if (duckdb_connect(reg->db, &reg->con) != DuckDBSuccess) {
        duckdb_close(&reg->db);
        free(reg);
        return GR_ERR_DB;
    }

    gr_error_t err = init_common(reg);
    if (err != GR_OK) {
        teardown(reg);
        return err;
    }

    err = gr_header_load(reg);
    if (err != GR_OK) {
        teardown(reg);
        return err;
    }

    if (yumi_memcmp(reg->header.group_id, group_id, GR_HASH_LEN) != 0) {
        teardown(reg);
        return GR_ERR_NOT_FOUND;
    }

    *out = reg;
    return GR_OK;
}

void gr_close(gr_registrar_t *reg) {
    if (!reg) return;

    gr_destroy_statements(reg);

    /* Clean up join verification state if present */
    if (reg->join_state) {
        yumi_memzero(reg->join_state, sizeof(gr_join_verify_state_t));
        free(reg->join_state);
        reg->join_state = NULL;
    }

    yumi_memzero(&reg->header, sizeof(reg->header));

    duckdb_disconnect(&reg->con);
    duckdb_close(&reg->db);
    pthread_mutex_destroy(&reg->db_lock);

    yumi_memzero(reg, sizeof(gr_registrar_t));
    free(reg);
}

gr_error_t gr_get_header(const gr_registrar_t *reg, gr_header_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;
    memcpy(out, &reg->header, sizeof(gr_header_t));
    return GR_OK;
}
