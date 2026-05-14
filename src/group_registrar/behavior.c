/**
 * @file behavior.c
 * @brief Passive behavioral analysis for the Group Registrar.
 *
 * 100% read-only logic — no mutations, no callbacks, no side effects.
 * Analyses audit logs, peer history, admin/mod actions, sync patterns,
 * and epoch rotation behaviour to produce quantitative scores that
 * the host application can use for anomaly detection and UX prompts.
 *
 * All detection thresholds are configurable via gr_behavior_config_t.
 * Defaults are applied at gr_create/gr_open.  Group-size scaling
 * (when enabled) adjusts group-wide thresholds by sqrt(active/10)
 * so that large groups aren't false-flagged for legitimate activity.
 *
 * @section optimizations Optimizations
 *
 * - Single gr_timestamp_ms() call per function
 * - Coalesced actor query: actor_burst and admin_score share one helper
 * - Snapshot does ONE scan of the audit window, classifying per-actor
 *   stats in a single pass — no N+1 queries
 * - Stale peer count via SELECT COUNT(*) instead of row fetch
 * - Entry interval via MIN/MAX/COUNT aggregate instead of LAG() window
 */

#include "internal.h"

#define BEH_MAX_ACTORS 65536

/* ═══════════════════════════════════════════════════════════════════
 *  Group-size scaling
 *
 *  For group-wide thresholds (swarm, epoch, delta), multiply the
 *  base threshold by sqrt(active_peers / 10).  Clamped to >= 1.0
 *  so thresholds never shrink below the configured baseline.
 *
 *  Per-actor thresholds (burst, abuse) are NOT scaled — one person
 *  kicking 20 people per minute is suspicious in any group.
 * ═══════════════════════════════════════════════════════════════════ */

static float group_scale(const gr_behavior_config_t *cfg,
                         uint32_t active_peers) {
    if (!cfg->scale_by_group_size || active_peers <= 10)
        return 1.0f;
    float s = sqrtf((float)active_peers / 10.0f);
    return s > 1.0f ? s : 1.0f;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Coalesced actor stats helper
 *
 *  Both gr_behavior_actor_burst and gr_behavior_admin_score need
 *  the same query (ps_beh_actor_actions).  This helper runs it
 *  once and fills a combined stats struct.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total;
    uint32_t kicks;
    uint32_t bans;
    uint32_t removes;
    uint32_t role_mods;
    uint32_t perm_escalations;
    uint32_t invites;
    uint32_t epochs;
} actor_stats_t;

static gr_error_t query_actor_stats(const gr_registrar_t *reg,
                                    const uint8_t actor_id[GR_PEER_ID_LEN],
                                    int64_t cutoff,
                                    actor_stats_t *s) {
    memset(s, 0, sizeof(*s));

    duckdb_prepared_statement stmt = reg->ps_beh_actor_actions;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, actor_id, GR_PEER_ID_LEN);
    duckdb_bind_int64(stmt, 2, cutoff);

    duckdb_result result;
    if (duckdb_execute_prepared(stmt, &result) != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        return GR_ERR_DB;
    }

    duckdb_data_chunk chunk;
    while ((chunk = duckdb_fetch_chunk(result)) != NULL) {
        idx_t rows = duckdb_data_chunk_get_size(chunk);
        duckdb_vector v_ct = duckdb_data_chunk_get_vector(chunk, 0);
        for (idx_t i = 0; i < rows; i++) {
            int32_t ct_val = 0;
            gr_db_vec_get_i32(v_ct, i, &ct_val);
            s->total++;
            switch ((gr_change_type_t)ct_val) {
                case GR_CHANGE_PEER_KICKED:        s->kicks++; break;
                case GR_CHANGE_PEER_BANNED:        s->bans++; break;
                case GR_CHANGE_PEER_REMOVED:       s->removes++; break;
                case GR_CHANGE_ROLE_ADDED:
                case GR_CHANGE_ROLE_REMOVED:
                case GR_CHANGE_ROLE_MODIFIED:       s->role_mods++; break;
                case GR_CHANGE_PEER_ROLE_CHANGED:   s->perm_escalations++; break;
                case GR_CHANGE_INVITE_CREATED:      s->invites++; break;
                case GR_CHANGE_EPOCH_ROTATED:       s->epochs++; break;
                default: break;
            }
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    duckdb_destroy_result(&result);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Per-mutation behavioral alert
 *
 *  Called from gr_audit_append() after every successful mutation.
 *  Runs ONE query (coalesced actor stats) over alert_window_ms,
 *  builds burst + admin scores, and fires the callback if either
 *  threshold is exceeded.  No-op if no callback is registered.
 * ═══════════════════════════════════════════════════════════════════ */

void gr_behavior_check_actor(gr_registrar_t *reg,
                             const uint8_t actor_id[GR_PEER_ID_LEN],
                             gr_change_type_t change_type) {
    if (!reg->behavior_alert_fn)
        return;

    const gr_behavior_config_t *cfg = &reg->behavior_config;
    if (cfg->alert_window_ms <= 0)
        return;

    int64_t cutoff = gr_timestamp_ms() - cfg->alert_window_ms;

    actor_stats_t s;
    if (query_actor_stats(reg, actor_id, cutoff, &s) != GR_OK)
        return;

    /* Build burst stats */
    gr_actor_burst_t burst;
    memset(&burst, 0, sizeof(burst));
    burst.actions_in_window   = s.total;
    burst.destructive_actions = s.kicks + s.bans + s.removes;
    burst.role_changes        = s.role_mods + s.perm_escalations;
    burst.invites_created     = s.invites;
    burst.epoch_rotations     = s.epochs;

    float window_min = (float)cfg->alert_window_ms / 60000.0f;
    if (window_min > 0.0f)
        burst.actions_per_minute = (float)s.total / window_min;
    burst.burst_detected = burst.actions_per_minute > cfg->burst_actions_per_min;

    /* Build admin abuse stats */
    gr_admin_score_t admin;
    memset(&admin, 0, sizeof(admin));
    admin.total_admin_actions    = s.total;
    admin.kicks                  = s.kicks;
    admin.bans                   = s.bans;
    admin.removes                = s.removes;
    admin.role_modifications     = s.role_mods;
    admin.permission_escalations = s.perm_escalations;

    uint32_t destructive = s.kicks + s.bans + s.removes;
    if (s.total > 0)
        admin.destructive_ratio = (float)destructive / (float)s.total;
    admin.abuse_suspected = admin.destructive_ratio > cfg->abuse_destructive_ratio
                         && s.total >= cfg->abuse_min_actions;

    /* Fire callback only if at least one flag is raised */
    uint32_t alerts = 0;
    if (burst.burst_detected)  alerts |= GR_ALERT_BURST;
    if (admin.abuse_suspected) alerts |= GR_ALERT_ABUSE;

    if (alerts != 0) {
        reg->behavior_alert_fn(alerts, actor_id, change_type,
                               &burst, &admin, reg->behavior_alert_data);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Config management
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_set_config(gr_registrar_t *reg,
                                  const gr_behavior_config_t *config) {
    if (!reg || !config) return GR_ERR_INVALID_PARAM;
    reg->behavior_config = *config;
    return GR_OK;
}

gr_error_t gr_behavior_get_config(const gr_registrar_t *reg,
                                  gr_behavior_config_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;
    *out = reg->behavior_config;
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Per-actor burst detection
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_actor_burst(const gr_registrar_t *reg,
                                   const uint8_t actor_id[GR_PEER_ID_LEN],
                                   int64_t window_ms,
                                   gr_actor_burst_t *out) {
    if (!reg || !actor_id || !out || window_ms <= 0)
        return GR_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    GR_LOCK((gr_registrar_t *)reg);

    int64_t now = gr_timestamp_ms();
    int64_t cutoff = now - window_ms;

    actor_stats_t s;
    gr_error_t err = query_actor_stats(reg, actor_id, cutoff, &s);
    if (err != GR_OK) { GR_UNLOCK((gr_registrar_t *)reg); return err; }

    out->actions_in_window  = s.total;
    out->destructive_actions = s.kicks + s.bans + s.removes;
    out->role_changes       = s.role_mods + s.perm_escalations;
    out->invites_created    = s.invites;
    out->epoch_rotations    = s.epochs;

    float window_min = (float)window_ms / 60000.0f;
    if (window_min > 0.0f)
        out->actions_per_minute = (float)s.total / window_min;

    /* Per-actor: no group-size scaling */
    out->burst_detected = out->actions_per_minute
                        > reg->behavior_config.burst_actions_per_min;

    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group-wide mutation rate
 *
 *  Uses ps_beh_window_all (one scan) to get both total count and
 *  distinct actors in a single pass through the result set.
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_mutation_rate(const gr_registrar_t *reg,
                                     int64_t window_ms,
                                     gr_mutation_rate_t *out) {
    if (!reg || !out || window_ms <= 0)
        return GR_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    GR_LOCK((gr_registrar_t *)reg);

    int64_t now = gr_timestamp_ms();
    int64_t cutoff = now - window_ms;

    duckdb_prepared_statement stmt = reg->ps_beh_window_all;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int64(stmt, 1, cutoff);

    duckdb_result r;
    if (duckdb_execute_prepared(stmt, &r) != DuckDBSuccess) {
        duckdb_destroy_result(&r);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_DB;
    }

    /* Count distinct actors — heap-allocated for large groups */
    uint8_t (*seen_actors)[GR_PEER_ID_LEN] = malloc((size_t)BEH_MAX_ACTORS * GR_PEER_ID_LEN);
    if (!seen_actors) {
        duckdb_destroy_result(&r);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_OUT_OF_MEMORY;
    }
    uint32_t seen_count = 0;
    uint32_t total = 0;

    duckdb_data_chunk chunk;
    while ((chunk = duckdb_fetch_chunk(r)) != NULL) {
        idx_t rows = duckdb_data_chunk_get_size(chunk);
        duckdb_vector v_actor = duckdb_data_chunk_get_vector(chunk, 1);
        total += (uint32_t)rows;
        for (idx_t i = 0; i < rows; i++) {
            uint8_t aid[GR_PEER_ID_LEN];
            gr_db_vec_get_blob(v_actor, i, aid, GR_PEER_ID_LEN);

            bool found = false;
            for (uint32_t j = 0; j < seen_count; j++) {
                if (yumi_memcmp(seen_actors[j], aid, GR_PEER_ID_LEN) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found && seen_count < BEH_MAX_ACTORS) {
                memcpy(seen_actors[seen_count++], aid, GR_PEER_ID_LEN);
            }
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    duckdb_destroy_result(&r);
    free(seen_actors);

    out->total_mutations = total;
    out->distinct_actors = seen_count;

    float window_min = (float)window_ms / 60000.0f;
    if (window_min > 0.0f)
        out->mutations_per_minute = (float)out->total_mutations / window_min;
    if (out->distinct_actors > 0)
        out->mutations_per_actor = (float)out->total_mutations
                                 / (float)out->distinct_actors;

    uint32_t active = 0;
    gr_peer_count(reg, GR_PEER_ACTIVE, &active);
    float scale = group_scale(&reg->behavior_config, active);

    out->swarm_detected = out->mutations_per_minute
                        > reg->behavior_config.swarm_mutations_per_min * scale;

    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Admin / moderator abuse scoring (coalesced with actor query)
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_admin_score(const gr_registrar_t *reg,
                                   const uint8_t admin_id[GR_PEER_ID_LEN],
                                   int64_t window_ms,
                                   gr_admin_score_t *out) {
    if (!reg || !admin_id || !out || window_ms <= 0)
        return GR_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    GR_LOCK((gr_registrar_t *)reg);

    int64_t now = gr_timestamp_ms();
    int64_t cutoff = now - window_ms;

    actor_stats_t s;
    gr_error_t err = query_actor_stats(reg, admin_id, cutoff, &s);
    if (err != GR_OK) { GR_UNLOCK((gr_registrar_t *)reg); return err; }

    out->total_admin_actions   = s.total;
    out->kicks                 = s.kicks;
    out->bans                  = s.bans;
    out->removes               = s.removes;
    out->role_modifications    = s.role_mods;
    out->permission_escalations = s.perm_escalations;

    uint32_t destructive = s.kicks + s.bans + s.removes;
    if (s.total > 0)
        out->destructive_ratio = (float)destructive / (float)s.total;

    /* Per-actor: no group-size scaling */
    const gr_behavior_config_t *cfg = &reg->behavior_config;
    out->abuse_suspected = out->destructive_ratio > cfg->abuse_destructive_ratio
                        && s.total >= cfg->abuse_min_actions;

    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Peer churn analysis (uses COUNT for stale, single timestamp)
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_peer_churn(const gr_registrar_t *reg,
                                  int64_t window_ms,
                                  gr_peer_churn_t *out) {
    if (!reg || !out || window_ms <= 0)
        return GR_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    GR_LOCK((gr_registrar_t *)reg);

    int64_t now = gr_timestamp_ms();
    int64_t cutoff = now - window_ms;

    gr_peer_count(reg, GR_PEER_ACTIVE, &out->active_peers);
    gr_peer_count(reg, GR_PEER_KICKED, &out->kicked_peers);
    gr_peer_count(reg, GR_PEER_BANNED, &out->banned_peers);
    gr_peer_count(reg, GR_PEER_LEFT,   &out->left_peers);

    /* Joined in window */
    {
        duckdb_prepared_statement stmt = reg->ps_beh_peer_joined;
        duckdb_clear_bindings(stmt);
        duckdb_bind_int64(stmt, 1, cutoff);
        duckdb_result r;
        if (duckdb_execute_prepared(stmt, &r) == DuckDBSuccess) {
            int64_t n = 0;
            if (gr_db_fetch_i64_scalar(&r, &n) == GR_OK)
                out->joined_in_window = (uint32_t)n;
        }
        duckdb_destroy_result(&r);
    }

    /* Removed in window */
    {
        duckdb_prepared_statement stmt = reg->ps_beh_peer_removed;
        duckdb_clear_bindings(stmt);
        duckdb_bind_int64(stmt, 1, cutoff);
        duckdb_result r;
        if (duckdb_execute_prepared(stmt, &r) == DuckDBSuccess) {
            int64_t n = 0;
            if (gr_db_fetch_i64_scalar(&r, &n) == GR_OK)
                out->removed_in_window = (uint32_t)n;
        }
        duckdb_destroy_result(&r);
    }

    if (out->active_peers > 0)
        out->churn_rate = (float)(out->joined_in_window + out->removed_in_window)
                        / (float)out->active_peers;

    /* Stale count via efficient COUNT(*) — no row fetch */
    {
        int64_t stale_cutoff = now - (window_ms / 2);
        duckdb_prepared_statement stmt = reg->ps_beh_stale_count;
        duckdb_clear_bindings(stmt);
        duckdb_bind_int64(stmt, 1, stale_cutoff);
        duckdb_result r;
        if (duckdb_execute_prepared(stmt, &r) == DuckDBSuccess) {
            int64_t n = 0;
            if (gr_db_fetch_i64_scalar(&r, &n) == GR_OK)
                out->stale_count = (uint32_t)n;
        }
        duckdb_destroy_result(&r);
    }

    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Delta / download anomaly scoring (group-size aware)
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_delta_score(const gr_registrar_t *reg,
                                   size_t delta_bytes,
                                   uint32_t entry_count,
                                   gr_delta_score_t *out) {
    if (!reg || !out)
        return GR_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    GR_LOCK((gr_registrar_t *)reg);

    int64_t now = gr_timestamp_ms();

    out->delta_bytes = delta_bytes;
    out->entry_count = entry_count;
    out->offline_duration_ms = now - reg->header.updated_at;
    if (out->offline_duration_ms < 1)
        out->offline_duration_ms = 1;

    float offline_days = (float)out->offline_duration_ms / 86400000.0f;
    if (offline_days < 0.001f) offline_days = 0.001f;

    out->bytes_per_offline_day   = (float)delta_bytes / offline_days;
    out->entries_per_offline_day = (float)entry_count / offline_days;

    uint32_t active = 0;
    gr_peer_count(reg, GR_PEER_ACTIVE, &active);
    float scale = group_scale(&reg->behavior_config, active);

    float threshold = reg->behavior_config.delta_anomaly_entries_per_day * scale;

    out->anomalous = out->entries_per_offline_day > threshold
                  && out->offline_duration_ms < GR_DELTA_ANOMALY_GAP_MS;

    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Stale peer listing
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_stale_peers(const gr_registrar_t *reg,
                                   int64_t stale_threshold_ms,
                                   uint8_t *peer_ids_out,
                                   uint32_t max_count,
                                   uint32_t *actual_count) {
    if (!reg || !peer_ids_out || !actual_count || stale_threshold_ms <= 0)
        return GR_ERR_INVALID_PARAM;

    *actual_count = 0;

    GR_LOCK((gr_registrar_t *)reg);

    int64_t cutoff = gr_timestamp_ms() - stale_threshold_ms;

    duckdb_prepared_statement stmt = reg->ps_beh_stale_peers;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int64(stmt, 1, cutoff);

    duckdb_result result;
    if (duckdb_execute_prepared(stmt, &result) != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_DB;
    }

    uint32_t count = 0;
    duckdb_data_chunk chunk;
    while (count < max_count &&
           (chunk = duckdb_fetch_chunk(result)) != NULL) {
        idx_t rows = duckdb_data_chunk_get_size(chunk);
        duckdb_vector v = duckdb_data_chunk_get_vector(chunk, 0);
        for (idx_t i = 0; i < rows && count < max_count; i++) {
            gr_db_vec_get_blob(v, i,
                peer_ids_out + (size_t)count * GR_PEER_ID_LEN,
                GR_PEER_ID_LEN);
            count++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    *actual_count = count;
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Epoch rotation pattern (group-size aware)
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_epoch_pattern(const gr_registrar_t *reg,
                                     int64_t window_ms,
                                     gr_epoch_pattern_t *out) {
    if (!reg || !out || window_ms <= 0)
        return GR_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    GR_LOCK((gr_registrar_t *)reg);

    int64_t now = gr_timestamp_ms();
    int64_t cutoff = now - window_ms;

    /* Rotations in window */
    {
        duckdb_prepared_statement stmt = reg->ps_beh_epoch_window;
        duckdb_clear_bindings(stmt);
        duckdb_bind_int64(stmt, 1, cutoff);
        duckdb_result r;
        if (duckdb_execute_prepared(stmt, &r) == DuckDBSuccess) {
            int64_t n = 0;
            if (gr_db_fetch_i64_scalar(&r, &n) == GR_OK)
                out->rotations_in_window = (uint32_t)n;
        }
        duckdb_destroy_result(&r);
    }

    /* Average epoch lifetime */
    {
        duckdb_prepared_statement stmt = reg->ps_beh_epoch_avg;
        duckdb_clear_bindings(stmt);
        duckdb_bind_int64(stmt, 1, now);
        duckdb_result r;
        if (duckdb_execute_prepared(stmt, &r) == DuckDBSuccess) {
            duckdb_data_chunk ch = NULL;
            if (gr_db_fetch_first_chunk(&r, &ch) == GR_OK) {
                double avg = 0.0;
                gr_db_vec_get_double(duckdb_data_chunk_get_vector(ch, 0), 0,
                                     &avg);
                out->avg_epoch_lifetime_ms = (int64_t)avg;
                duckdb_destroy_data_chunk(&ch);
            }
        }
        duckdb_destroy_result(&r);
    }

    uint32_t active = 0;
    gr_peer_count(reg, GR_PEER_ACTIVE, &active);
    float scale = group_scale(&reg->behavior_config, active);

    float window_hours = (float)window_ms / 3600000.0f;
    if (window_hours > 0.0f) {
        float rate = (float)out->rotations_in_window / window_hours;
        out->excessive_rotation = rate
            > reg->behavior_config.epoch_max_per_hour * scale;
    }

    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Network / sync interaction score
 *
 *  Entry interval: (max_ts - min_ts) / (count - 1) via a single
 *  MIN/MAX/COUNT aggregate — O(1) in DuckDB vs O(N) LAG window.
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_network_score(const gr_registrar_t *reg,
                                     gr_network_score_t *out) {
    if (!reg || !out)
        return GR_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    GR_LOCK((gr_registrar_t *)reg);

    int64_t now = gr_timestamp_ms();

    out->last_update_ms = reg->header.updated_at;
    out->time_since_update_ms = now - reg->header.updated_at;
    out->estimated_registrar_bytes = gr_estimate_size(reg);

    gr_audit_count(reg, &out->total_audit_entries);

    /* Average inter-entry interval via MIN/MAX/COUNT aggregate */
    {
        duckdb_prepared_statement stmt = reg->ps_beh_entry_timespan;
        duckdb_clear_bindings(stmt);
        duckdb_result r;
        if (duckdb_execute_prepared(stmt, &r) == DuckDBSuccess) {
            duckdb_data_chunk ch = NULL;
            if (gr_db_fetch_first_chunk(&r, &ch) == GR_OK) {
                int64_t min_ts = 0, max_ts = 0, cnt = 0;
                gr_db_vec_get_i64(duckdb_data_chunk_get_vector(ch, 0), 0,
                                  &min_ts);
                gr_db_vec_get_i64(duckdb_data_chunk_get_vector(ch, 1), 0,
                                  &max_ts);
                gr_db_vec_get_i64(duckdb_data_chunk_get_vector(ch, 2), 0,
                                  &cnt);
                if (cnt > 1)
                    out->avg_entry_interval_ms = (float)(max_ts - min_ts)
                                               / (float)(cnt - 1);
                duckdb_destroy_data_chunk(&ch);
            }
        }
        duckdb_destroy_result(&r);
    }

    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Full group health snapshot — single-pass window scan
 *
 *  Instead of calling individual functions (which each query the
 *  audit log separately), the snapshot performs ONE scan of all
 *  audit entries in the window via ps_beh_window_all.  From that
 *  single result set it derives:
 *    - Mutation rate (total + distinct actors)
 *    - Per-actor stats (via in-memory bucketing)
 *    - Worst admin abuse score
 *
 *  Peer churn, epoch pattern, and network score use their own
 *  efficient indexed lookups (no redundant audit scans).
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_behavior_snapshot(const gr_registrar_t *reg,
                                int64_t window_ms,
                                gr_behavior_snapshot_t *out) {
    if (!reg || !out || window_ms <= 0)
        return GR_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    GR_LOCK((gr_registrar_t *)reg);

    int64_t now = gr_timestamp_ms();
    int64_t cutoff = now - window_ms;

    const gr_behavior_config_t *cfg = &reg->behavior_config;

    /* ── Peer churn (indexed lookups, no audit scan) ───────────── */
    {
        gr_peer_count(reg, GR_PEER_ACTIVE, &out->churn.active_peers);
        gr_peer_count(reg, GR_PEER_KICKED, &out->churn.kicked_peers);
        gr_peer_count(reg, GR_PEER_BANNED, &out->churn.banned_peers);
        gr_peer_count(reg, GR_PEER_LEFT,   &out->churn.left_peers);

        duckdb_prepared_statement s1 = reg->ps_beh_peer_joined;
        duckdb_clear_bindings(s1);
        duckdb_bind_int64(s1, 1, cutoff);
        duckdb_result r1;
        if (duckdb_execute_prepared(s1, &r1) == DuckDBSuccess) {
            int64_t n = 0;
            if (gr_db_fetch_i64_scalar(&r1, &n) == GR_OK)
                out->churn.joined_in_window = (uint32_t)n;
        }
        duckdb_destroy_result(&r1);

        duckdb_prepared_statement s2 = reg->ps_beh_peer_removed;
        duckdb_clear_bindings(s2);
        duckdb_bind_int64(s2, 1, cutoff);
        duckdb_result r2;
        if (duckdb_execute_prepared(s2, &r2) == DuckDBSuccess) {
            int64_t n = 0;
            if (gr_db_fetch_i64_scalar(&r2, &n) == GR_OK)
                out->churn.removed_in_window = (uint32_t)n;
        }
        duckdb_destroy_result(&r2);

        if (out->churn.active_peers > 0)
            out->churn.churn_rate = (float)(out->churn.joined_in_window
                                          + out->churn.removed_in_window)
                                  / (float)out->churn.active_peers;

        int64_t stale_cutoff = now - (window_ms / 2);
        duckdb_prepared_statement s3 = reg->ps_beh_stale_count;
        duckdb_clear_bindings(s3);
        duckdb_bind_int64(s3, 1, stale_cutoff);
        duckdb_result r3;
        if (duckdb_execute_prepared(s3, &r3) == DuckDBSuccess) {
            int64_t n = 0;
            if (gr_db_fetch_i64_scalar(&r3, &n) == GR_OK)
                out->churn.stale_count = (uint32_t)n;
        }
        duckdb_destroy_result(&r3);
    }

    float scale = group_scale(cfg, out->churn.active_peers);

    /* ── Single-pass audit window scan ─────────────────────────── */
    {
        duckdb_prepared_statement stmt = reg->ps_beh_window_all;
        duckdb_clear_bindings(stmt);
        duckdb_bind_int64(stmt, 1, cutoff);

        duckdb_result wr;
        if (duckdb_execute_prepared(stmt, &wr) != DuckDBSuccess) {
            duckdb_destroy_result(&wr);
            memset(out, 0, sizeof(*out));
            GR_UNLOCK((gr_registrar_t *)reg);
            return GR_ERR_DB;
        }

        /* Per-actor buckets — heap-allocated for large groups */
        actor_stats_t *actors = calloc(BEH_MAX_ACTORS, sizeof(actor_stats_t));
        uint8_t (*actor_ids)[GR_PEER_ID_LEN] = malloc((size_t)BEH_MAX_ACTORS * GR_PEER_ID_LEN);
        uint32_t actor_count = 0;
        uint32_t total_rows = 0;

        if (!actors || !actor_ids) {
            free(actors);
            free(actor_ids);
            duckdb_destroy_result(&wr);
            memset(out, 0, sizeof(*out));
            GR_UNLOCK((gr_registrar_t *)reg);
            return GR_ERR_OUT_OF_MEMORY;
        }

        duckdb_data_chunk chunk;
        while ((chunk = duckdb_fetch_chunk(wr)) != NULL) {
            idx_t rows = duckdb_data_chunk_get_size(chunk);
            duckdb_vector v_ct    = duckdb_data_chunk_get_vector(chunk, 0);
            duckdb_vector v_actor = duckdb_data_chunk_get_vector(chunk, 1);

            for (idx_t i = 0; i < rows; i++) {
                int32_t ct_raw = 0;
                gr_db_vec_get_i32(v_ct, i, &ct_raw);
                gr_change_type_t ct = (gr_change_type_t)ct_raw;
                uint8_t aid[GR_PEER_ID_LEN];
                gr_db_vec_get_blob(v_actor, i, aid, GR_PEER_ID_LEN);

                total_rows++;

                /* Find or create actor bucket */
                actor_stats_t *a = NULL;
                for (uint32_t j = 0; j < actor_count; j++) {
                    if (yumi_memcmp(actor_ids[j], aid, GR_PEER_ID_LEN) == 0) {
                        a = &actors[j];
                        break;
                    }
                }
                if (!a && actor_count < BEH_MAX_ACTORS) {
                    a = &actors[actor_count];
                    memcpy(actor_ids[actor_count], aid, GR_PEER_ID_LEN);
                    actor_count++;
                }

                if (a) {
                    a->total++;
                    switch (ct) {
                        case GR_CHANGE_PEER_KICKED:        a->kicks++; break;
                        case GR_CHANGE_PEER_BANNED:        a->bans++; break;
                        case GR_CHANGE_PEER_REMOVED:       a->removes++; break;
                        case GR_CHANGE_ROLE_ADDED:
                        case GR_CHANGE_ROLE_REMOVED:
                        case GR_CHANGE_ROLE_MODIFIED:       a->role_mods++; break;
                        case GR_CHANGE_PEER_ROLE_CHANGED:   a->perm_escalations++; break;
                        case GR_CHANGE_INVITE_CREATED:      a->invites++; break;
                        case GR_CHANGE_EPOCH_ROTATED:       a->epochs++; break;
                        default: break;
                    }
                }
            }
            duckdb_destroy_data_chunk(&chunk);
        }

        out->mutation_rate.total_mutations = total_rows;
        duckdb_destroy_result(&wr);

        /* Mutation rate */
        out->mutation_rate.distinct_actors = actor_count;
        float window_min = (float)window_ms / 60000.0f;
        if (window_min > 0.0f)
            out->mutation_rate.mutations_per_minute =
                (float)out->mutation_rate.total_mutations / window_min;
        if (actor_count > 0)
            out->mutation_rate.mutations_per_actor =
                (float)out->mutation_rate.total_mutations / (float)actor_count;
        out->mutation_rate.swarm_detected =
            out->mutation_rate.mutations_per_minute
            > cfg->swarm_mutations_per_min * scale;

        /* Find worst admin by destructive ratio */
        float worst_ratio = 0.0f;
        int worst_idx = -1;
        for (uint32_t j = 0; j < actor_count; j++) {
            actor_stats_t *a = &actors[j];
            uint32_t destructive = a->kicks + a->bans + a->removes;
            if (a->total < cfg->abuse_min_actions)
                continue;
            float ratio = (float)destructive / (float)a->total;
            if (ratio > worst_ratio) {
                worst_ratio = ratio;
                worst_idx = (int)j;
            }
        }

        if (worst_idx >= 0 && worst_ratio > cfg->abuse_destructive_ratio) {
            actor_stats_t *w = &actors[worst_idx];
            out->has_worst_admin = true;
            memcpy(out->worst_admin_id, actor_ids[worst_idx], GR_PEER_ID_LEN);
            out->worst_admin.total_admin_actions   = w->total;
            out->worst_admin.kicks                 = w->kicks;
            out->worst_admin.bans                  = w->bans;
            out->worst_admin.removes               = w->removes;
            out->worst_admin.role_modifications    = w->role_mods;
            out->worst_admin.permission_escalations = w->perm_escalations;
            out->worst_admin.destructive_ratio     = worst_ratio;
            out->worst_admin.abuse_suspected        = true;
        }

        free(actors);
        free(actor_ids);
    }

    /* ── Epoch pattern (indexed lookup) ────────────────────────── */
    {
        int64_t epoch_cutoff = cutoff; /* reuse, same window */

        duckdb_prepared_statement s1 = reg->ps_beh_epoch_window;
        duckdb_clear_bindings(s1);
        duckdb_bind_int64(s1, 1, epoch_cutoff);
        duckdb_result r1;
        if (duckdb_execute_prepared(s1, &r1) == DuckDBSuccess) {
            int64_t n = 0;
            if (gr_db_fetch_i64_scalar(&r1, &n) == GR_OK)
                out->epoch_pattern.rotations_in_window = (uint32_t)n;
        }
        duckdb_destroy_result(&r1);

        duckdb_prepared_statement s2 = reg->ps_beh_epoch_avg;
        duckdb_clear_bindings(s2);
        duckdb_bind_int64(s2, 1, now);
        duckdb_result r2;
        if (duckdb_execute_prepared(s2, &r2) == DuckDBSuccess) {
            duckdb_data_chunk ch = NULL;
            if (gr_db_fetch_first_chunk(&r2, &ch) == GR_OK) {
                double avg = 0.0;
                gr_db_vec_get_double(duckdb_data_chunk_get_vector(ch, 0), 0,
                                     &avg);
                out->epoch_pattern.avg_epoch_lifetime_ms = (int64_t)avg;
                duckdb_destroy_data_chunk(&ch);
            }
        }
        duckdb_destroy_result(&r2);

        float window_hours = (float)window_ms / 3600000.0f;
        if (window_hours > 0.0f) {
            float rate = (float)out->epoch_pattern.rotations_in_window
                       / window_hours;
            out->epoch_pattern.excessive_rotation =
                rate > cfg->epoch_max_per_hour * scale;
        }
    }

    /* ── Network score ─────────────────────────────────────────── */
    {
        out->network.last_update_ms = reg->header.updated_at;
        out->network.time_since_update_ms = now - reg->header.updated_at;
        out->network.estimated_registrar_bytes = gr_estimate_size(reg);

        gr_audit_count(reg, &out->network.total_audit_entries);

        duckdb_prepared_statement ts = reg->ps_beh_entry_timespan;
        duckdb_clear_bindings(ts);
        duckdb_result tr;
        if (duckdb_execute_prepared(ts, &tr) == DuckDBSuccess) {
            duckdb_data_chunk ch = NULL;
            if (gr_db_fetch_first_chunk(&tr, &ch) == GR_OK) {
                int64_t min_ts = 0, max_ts = 0, cnt = 0;
                gr_db_vec_get_i64(duckdb_data_chunk_get_vector(ch, 0), 0,
                                  &min_ts);
                gr_db_vec_get_i64(duckdb_data_chunk_get_vector(ch, 1), 0,
                                  &max_ts);
                gr_db_vec_get_i64(duckdb_data_chunk_get_vector(ch, 2), 0,
                                  &cnt);
                if (cnt > 1)
                    out->network.avg_entry_interval_ms =
                        (float)(max_ts - min_ts) / (float)(cnt - 1);
                duckdb_destroy_data_chunk(&ch);
            }
        }
        duckdb_destroy_result(&tr);
    }

    out->estimated_size = out->network.estimated_registrar_bytes;

    out->needs_attention = out->mutation_rate.swarm_detected
                        || out->epoch_pattern.excessive_rotation
                        || out->churn.churn_rate > cfg->churn_attention_threshold
                        || out->has_worst_admin;

    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}
