/*
    Group Registrar — Group Icon Management
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
 * @file icon.c
 * @brief Group icon management for the Group Registrar.
 *
 * The group icon is stored as a BLOB in the gr_group_icon table.
 * It supports static images (PNG, JPEG, WebP) and short video
 * loops (MP4, WebM) up to 512x512 at 10 MB.
 *
 * For video icons, the caller provides both the video data and
 * a pre-extracted first frame (PNG) as the static fallback. The
 * UI displays the static frame by default and plays the video
 * on hover/focus.
 *
 * The icon is NOT included in the serialized registrar blob to
 * avoid bloating sync traffic. Instead, the icon's content hash
 * is stored in the registrar header area, and the networking
 * layer uses gr_group_icon_hash() to check if a peer's icon is
 * current before fetching the full blob.
 *
 * Schema (created by db.c init_schema):
 *   CREATE TABLE IF NOT EXISTS gr_group_icon (
 *     id            INTEGER PRIMARY KEY DEFAULT 1,
 *     data          BLOB NOT NULL,
 *     mime_type     VARCHAR(64) NOT NULL,
 *     width         INTEGER NOT NULL,
 *     height        INTEGER NOT NULL,
 *     is_video      BOOLEAN NOT NULL DEFAULT FALSE,
 *     static_frame  BLOB,
 *     content_hash  BLOB NOT NULL,
 *     updated_at    BIGINT NOT NULL,
 *     updated_by    BLOB NOT NULL
 *   );
 */

#include "internal.h"

/* ── Validation helpers ────────────────────────────────────────── */

static bool is_valid_mime(const char *mime) {
    if (!mime) return false;
    /* Static images */
    if (strcmp(mime, "image/png") == 0) return true;
    if (strcmp(mime, "image/jpeg") == 0) return true;
    if (strcmp(mime, "image/webp") == 0) return true;
    /* Video loops */
    if (strcmp(mime, "video/mp4") == 0) return true;
    if (strcmp(mime, "video/webm") == 0) return true;
    return false;
}

static bool is_video_mime(const char *mime) {
    return strcmp(mime, "video/mp4") == 0 ||
           strcmp(mime, "video/webm") == 0;
}

/* ── Public API ────────────────────────────────────────────────── */

gr_error_t gr_group_icon_set(gr_registrar_t *reg,
                             const uint8_t *data, size_t data_len,
                             const char *mime_type,
                             uint16_t width, uint16_t height,
                             bool is_video,
                             const uint8_t *static_frame,
                             size_t static_frame_len,
                             const gr_identity_t *signer) {
    if (!reg || !data || !mime_type || !signer)
        return GR_ERR_INVALID_PARAM;

    /* Block during unverified state */
    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    /* Permission check */
    if (!gr_check_perm(reg, signer, GR_PERM_SET_GROUP_ICON))
        return GR_ERR_UNAUTHORIZED;

    /* Validate dimensions */
    if (width == 0 || height == 0)
        return GR_ERR_INVALID_PARAM;
    if (width > GR_GROUP_ICON_MAX_DIM || height > GR_GROUP_ICON_MAX_DIM)
        return GR_ERR_SIZE_EXCEEDED;

    /* Validate size */
    if (data_len == 0 || data_len > GR_GROUP_ICON_MAX_BYTES)
        return GR_ERR_SIZE_EXCEEDED;

    /* Validate MIME type */
    if (!is_valid_mime(mime_type))
        return GR_ERR_INVALID_PARAM;

    /* Video consistency checks */
    if (is_video) {
        if (!is_video_mime(mime_type))
            return GR_ERR_INVALID_PARAM;
        if (!static_frame || static_frame_len == 0)
            return GR_ERR_INVALID_PARAM;
        if (static_frame_len > GR_GROUP_ICON_MAX_BYTES)
            return GR_ERR_SIZE_EXCEEDED;
    }

    /* Compute content hash */
    uint8_t content_hash[GR_HASH_LEN];
    yumi_skein_hash(content_hash, data, data_len);

    int64_t now = gr_timestamp_ms();

    /* Upsert: delete any existing icon, then insert (atomic) */
    GR_LOCK(reg);

    if (!gr_txn_begin(reg))
        { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_prepared_statement del = reg->ps_icon_delete;
    duckdb_clear_bindings(del);
    duckdb_result dr;
    duckdb_execute_prepared(del, &dr);
    duckdb_destroy_result(&dr);

    duckdb_prepared_statement stmt = reg->ps_icon_upsert;
    duckdb_clear_bindings(stmt);

    duckdb_bind_blob(stmt, 1, data, data_len);
    duckdb_bind_varchar(stmt, 2, mime_type);
    duckdb_bind_int32(stmt, 3, (int32_t)width);
    duckdb_bind_int32(stmt, 4, (int32_t)height);
    duckdb_bind_boolean(stmt, 5, is_video);
    if (static_frame && static_frame_len > 0)
        duckdb_bind_blob(stmt, 6, static_frame, static_frame_len);
    else
        duckdb_bind_null(stmt, 6);
    duckdb_bind_blob(stmt, 7, content_hash, GR_HASH_LEN);
    duckdb_bind_int64(stmt, 8, now);
    duckdb_bind_blob(stmt, 9, signer->peer_id, GR_PEER_ID_LEN);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);
    if (st != DuckDBSuccess) {
        gr_txn_rollback(reg);
        GR_UNLOCK(reg);
        return GR_ERR_DB;
    }

    char detail[128];
    snprintf(detail, sizeof(detail), "icon set (%s, %ux%u, %zu bytes)",
             mime_type, width, height, data_len);
    gr_error_t err = gr_audit_append(reg, GR_CHANGE_GROUP_ICON_SET, signer,
                           NULL, detail);
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_group_icon_get(const gr_registrar_t *reg,
                             gr_group_icon_t *out) {
    if (!reg || !out) return GR_ERR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_icon_get;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, 1);

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

    /* Icon data BLOB (variable length) */
    gr_error_t ge = gr_db_vec_get_blob_alloc(
        duckdb_data_chunk_get_vector(chunk, 0), 0,
        &out->data, &out->data_len);
    if (ge != GR_OK) {
        duckdb_destroy_data_chunk(&chunk);
        duckdb_destroy_result(&result);
        GR_UNLOCK((gr_registrar_t *)reg);
        return ge;
    }

    /* MIME type */
    gr_db_vec_get_str(duckdb_data_chunk_get_vector(chunk, 1), 0,
                      out->mime_type, sizeof(out->mime_type));

    /* Dimensions */
    int32_t w = 0, h = 0;
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 2), 0, &w);
    gr_db_vec_get_i32 (duckdb_data_chunk_get_vector(chunk, 3), 0, &h);
    gr_db_vec_get_bool(duckdb_data_chunk_get_vector(chunk, 4), 0,
                       &out->is_video);
    out->width  = (uint16_t)w;
    out->height = (uint16_t)h;

    /* Static frame for video icons */
    if (out->is_video) {
        ge = gr_db_vec_get_blob_alloc(
            duckdb_data_chunk_get_vector(chunk, 5), 0,
            &out->static_frame, &out->static_frame_len);
        if (ge != GR_OK) {
            free(out->data);
            out->data = NULL;
            duckdb_destroy_data_chunk(&chunk);
            duckdb_destroy_result(&result);
            GR_UNLOCK((gr_registrar_t *)reg);
            return ge;
        }
    }

    /* Content hash */
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 6), 0,
                       out->content_hash, GR_HASH_LEN);

    /* Metadata */
    gr_db_vec_get_i64 (duckdb_data_chunk_get_vector(chunk, 7), 0,
                       &out->updated_at);
    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 8), 0,
                       out->updated_by, GR_PEER_ID_LEN);

    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_group_icon_remove(gr_registrar_t *reg,
                                const gr_identity_t *signer) {
    if (!reg || !signer) return GR_ERR_INVALID_PARAM;

    if (gr_join_is_untrusted(reg))
        return GR_ERR_JOIN_UNVERIFIED;

    if (!gr_check_perm(reg, signer, GR_PERM_SET_GROUP_ICON))
        return GR_ERR_UNAUTHORIZED;

    GR_LOCK(reg);

    duckdb_prepared_statement stmt = reg->ps_icon_delete;
    duckdb_clear_bindings(stmt);

    if (!gr_txn_begin(reg)) { GR_UNLOCK(reg); return GR_ERR_DB; }

    duckdb_result result;
    duckdb_execute_prepared(stmt, &result);
    duckdb_destroy_result(&result);

    gr_error_t err = gr_audit_append(reg, GR_CHANGE_GROUP_ICON_REMOVED, signer,
                           NULL, "icon removed");
    if (err != GR_OK) { gr_txn_rollback(reg); GR_UNLOCK(reg); return err; }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); GR_UNLOCK(reg); return GR_ERR_DB; }
    GR_UNLOCK(reg);
    return GR_OK;
}

gr_error_t gr_group_icon_hash(const gr_registrar_t *reg,
                              uint8_t hash_out[GR_HASH_LEN]) {
    if (!reg || !hash_out) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    duckdb_prepared_statement stmt = reg->ps_icon_hash;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, 1);

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

    gr_db_vec_get_blob(duckdb_data_chunk_get_vector(chunk, 0), 0,
                       hash_out, GR_HASH_LEN);
    duckdb_destroy_data_chunk(&chunk);
    duckdb_destroy_result(&result);
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

void gr_group_icon_free(gr_group_icon_t *icon) {
    if (!icon) return;
    free(icon->data);
    icon->data = NULL;
    icon->data_len = 0;
    free(icon->static_frame);
    icon->static_frame = NULL;
    icon->static_frame_len = 0;
}
