/*
 * util.c - Group Registrar utility helpers: Skein hashing, peer-id equality, monotonic timestamp_ms / timestamp_ns.
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

gr_error_t gr_hash(const uint8_t *data, size_t data_len,
                   uint8_t out[GR_HASH_LEN]) {
    if (!data || !out) return GR_ERR_INVALID_PARAM;
    if (yumi_skein_hash(out, data, data_len) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;
    return GR_OK;
}

bool gr_id_equal(const uint8_t a[GR_PEER_ID_LEN],
                 const uint8_t b[GR_PEER_ID_LEN]) {
    if (!a || !b) return false;
    return yumi_memcmp(a, b, GR_PEER_ID_LEN) == 0;
}

int64_t gr_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int64_t gr_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

uint64_t gr_schema_version(void) {
    return 3;
}

void gr_free(void *ptr) {
    free(ptr);
}

const char *gr_error_str(gr_error_t err) {
    switch (err) {
        case GR_OK:                       return "success";
        case GR_ERR_INVALID_PARAM:        return "invalid parameter";
        case GR_ERR_NOT_FOUND:            return "not found";
        case GR_ERR_ALREADY_EXISTS:       return "already exists";
        case GR_ERR_UNAUTHORIZED:         return "unauthorized";
        case GR_ERR_SIGNATURE_INVALID:    return "invalid signature";
        case GR_ERR_CRYPTO:               return "cryptographic error";
        case GR_ERR_DB:                   return "database error";
        case GR_ERR_SIZE_EXCEEDED:        return "size exceeded";
        case GR_ERR_ROLE_LIMIT:           return "role limit reached";
        case GR_ERR_EPOCH_MISMATCH:       return "epoch mismatch";
        case GR_ERR_SERIALIZATION:        return "serialization error";
        case GR_ERR_OUT_OF_MEMORY:        return "out of memory";
        case GR_ERR_INVITE_EXPIRED:       return "invite expired";
        case GR_ERR_INVITE_INVALID:       return "invite invalid";
        case GR_ERR_MERGE_CONFLICT:       return "merge conflict";
        case GR_ERR_JOIN_UNVERIFIED:      return "registrar not yet verified by peers";
        case GR_ERR_JOIN_FAILED:          return "registrar verification failed - authority mismatch";
        case GR_ERR_JOIN_INVITER_EXCLUDED: return "inviter cannot attest their own invite";
        default:                          return "unknown error";
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Callback registration
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_set_delta_anomaly_callback(gr_registrar_t *reg,
                                         gr_delta_anomaly_fn callback,
                                         void *user_data) {
    if (!reg) return GR_ERR_INVALID_PARAM;
    reg->delta_anomaly_fn   = callback;
    reg->delta_anomaly_data = user_data;
    return GR_OK;
}

gr_error_t gr_set_persist_prompt_callback(gr_registrar_t *reg,
                                          gr_persist_prompt_fn callback,
                                          void *user_data) {
    if (!reg) return GR_ERR_INVALID_PARAM;
    reg->persist_prompt_fn   = callback;
    reg->persist_prompt_data = user_data;
    return GR_OK;
}

gr_error_t gr_set_behavior_alert_callback(gr_registrar_t *reg,
                                          gr_behavior_alert_fn callback,
                                          void *user_data) {
    if (!reg) return GR_ERR_INVALID_PARAM;
    reg->behavior_alert_fn   = callback;
    reg->behavior_alert_data = user_data;
    return GR_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Size estimation
 *
 *  Returns a rough byte estimate of the registrar's total content
 *  by summing row counts × per-row size estimates across all tables.
 *  Used by the persist prompt to decide whether to recommend disk
 *  storage over RAM.
 * ═══════════════════════════════════════════════════════════════════ */

size_t gr_estimate_size(const gr_registrar_t *reg) {
    if (!reg) return 0;

    size_t est = sizeof(gr_header_t);

    uint32_t n = 0;
    if (gr_peer_count(reg, GR_PEER_STATUS_ANY, &n) == GR_OK)
        est += (size_t)n * 300;  /* ~300 bytes per peer row */
    n = 0;
    if (gr_role_count(reg, &n) == GR_OK)
        est += (size_t)n * 200;
    n = 0;
    if (gr_webapp_count(reg, &n) == GR_OK)
        est += (size_t)n * 200;
    n = 0;
    if (gr_server_count(reg, GR_SERVER_SIGNALING, &n) == GR_OK)
        est += (size_t)n * 250;
    n = 0;
    if (gr_server_count(reg, GR_SERVER_REBROADCAST, &n) == GR_OK)
        est += (size_t)n * 250;
    n = 0;
    if (gr_epoch_count(reg, &n) == GR_OK)
        est += (size_t)n * 80;
    n = 0;
    if (gr_audit_count(reg, &n) == GR_OK)
        est += (size_t)n * GR_AUDIT_EST_ROW_BYTES;

    return est;
}
