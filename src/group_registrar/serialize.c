#include "internal.h"
#include "buf.h"

static const char SERIAL_MAGIC[4] = "GREG";
static const char DELTA_MAGIC[4]  = "GRDT";
#define SERIAL_VERSION 3

/* ── Per-type serialize/deserialize helpers ─────────────────────── */

static void ser_peer(gr_buf_t *b, const gr_peer_t *p) {
    gr_buf_write(b, p->peer_id, GR_PEER_ID_LEN);
    gr_buf_write(b, p->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    gr_buf_write(b, p->sign_key, GR_PUBLIC_KEY_LEN);
    gr_buf_write_str(b, p->ip, GR_MAX_IP_LEN);
    gr_buf_write_u16(b, p->port);
    gr_buf_write_u32(b, (uint32_t)p->status);
    gr_buf_write_u32(b, p->role_id);
    gr_buf_write_i64(b, p->joined_at);
    gr_buf_write_i64(b, p->removed_at);
    gr_buf_write_i64(b, p->last_seen);
    gr_buf_write_str(b, p->removed_reason, GR_MAX_NAME_LEN);
    gr_buf_write(b, p->removed_by, GR_PEER_ID_LEN);
}

static int de_peer(gr_reader_t *r, gr_peer_t *p) {
    memset(p, 0, sizeof(*p));
    if (gr_read_bytes(r, p->peer_id, GR_PEER_ID_LEN) != 0) return -1;
    if (gr_read_bytes(r, p->kem_pk, GR_KEM_PUBLIC_KEY_LEN) != 0) return -1;
    if (gr_read_bytes(r, p->sign_key, GR_PUBLIC_KEY_LEN) != 0) return -1;
    if (gr_read_str(r, p->ip, GR_MAX_IP_LEN) != 0) return -1;
    if (gr_read_u16(r, &p->port) != 0) return -1;
    uint32_t s; if (gr_read_u32(r, &s) != 0) return -1;
    p->status = (gr_peer_status_t)s;
    if (gr_read_u32(r, &p->role_id) != 0) return -1;
    if (gr_read_i64(r, &p->joined_at) != 0) return -1;
    if (gr_read_i64(r, &p->removed_at) != 0) return -1;
    if (gr_read_i64(r, &p->last_seen) != 0) return -1;
    if (gr_read_str(r, p->removed_reason, GR_MAX_NAME_LEN) != 0) return -1;
    if (gr_read_bytes(r, p->removed_by, GR_PEER_ID_LEN) != 0) return -1;
    return 0;
}

static void ser_role(gr_buf_t *b, const gr_role_t *rl) {
    gr_buf_write_u32(b, rl->role_id);
    gr_buf_write_str(b, rl->name, GR_MAX_NAME_LEN);
    gr_buf_write_u32(b, rl->permissions);
    gr_buf_write(b, rl->sign_key, GR_PUBLIC_KEY_LEN);
    gr_buf_write_i64(b, rl->created_at);
    gr_buf_write_i64(b, rl->modified_at);
}

static int de_role(gr_reader_t *r, gr_role_t *rl, uint32_t wire_ver) {
    memset(rl, 0, sizeof(*rl));
    if (gr_read_u32(r, &rl->role_id) != 0) return -1;
    if (gr_read_str(r, rl->name, GR_MAX_NAME_LEN) != 0) return -1;
    if (gr_read_u32(r, &rl->permissions) != 0) return -1;
    if (gr_read_bytes(r, rl->sign_key, GR_PUBLIC_KEY_LEN) != 0) return -1;
    if (gr_read_i64(r, &rl->created_at) != 0) return -1;
    if (wire_ver >= 2) {
        if (gr_read_i64(r, &rl->modified_at) != 0) return -1;
    } else {
        rl->modified_at = rl->created_at;
    }
    return 0;
}

static void ser_webapp(gr_buf_t *b, const gr_webapp_t *w) {
    gr_buf_write(b, w->hash, GR_SERVICE_HASH_LEN);
    gr_buf_write_str(b, w->name, GR_MAX_NAME_LEN);
    gr_buf_write_u32(b, w->version);
    gr_buf_write_i64(b, w->added_at);
    gr_buf_write(b, w->added_by, GR_PEER_ID_LEN);
}

static int de_webapp(gr_reader_t *r, gr_webapp_t *w) {
    memset(w, 0, sizeof(*w));
    if (gr_read_bytes(r, w->hash, GR_SERVICE_HASH_LEN) != 0) return -1;
    if (gr_read_str(r, w->name, GR_MAX_NAME_LEN) != 0) return -1;
    if (gr_read_u32(r, &w->version) != 0) return -1;
    if (gr_read_i64(r, &w->added_at) != 0) return -1;
    if (gr_read_bytes(r, w->added_by, GR_PEER_ID_LEN) != 0) return -1;
    return 0;
}

static void ser_server(gr_buf_t *b, const gr_server_t *s,
                       gr_serialize_mode_t mode) {
    gr_buf_write(b, s->id_hash, GR_HASH_LEN);
    gr_buf_write_u32(b, (uint32_t)s->type);
    gr_buf_write_str(b, s->ip, GR_MAX_IP_LEN);
    gr_buf_write_u16(b, s->port);
    gr_buf_write(b, s->sign_key, GR_PUBLIC_KEY_LEN);
    gr_buf_write(b, s->service_hash, GR_SERVICE_HASH_LEN);
    gr_buf_write(b, s->content_kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    if (mode == GR_SERIALIZE_OWNER)
        gr_buf_write(b, s->content_kem_sk, GR_KEM_SECRET_KEY_LEN);
    else {
        uint8_t z[GR_KEM_SECRET_KEY_LEN];
        memset(z, 0, sizeof(z));
        gr_buf_write(b, z, GR_KEM_SECRET_KEY_LEN);
    }
}

static int de_server(gr_reader_t *r, gr_server_t *s) {
    memset(s, 0, sizeof(*s));
    if (gr_read_bytes(r, s->id_hash, GR_HASH_LEN) != 0) return -1;
    uint32_t t; if (gr_read_u32(r, &t) != 0) return -1;
    s->type = (gr_server_type_t)t;
    if (gr_read_str(r, s->ip, GR_MAX_IP_LEN) != 0) return -1;
    if (gr_read_u16(r, &s->port) != 0) return -1;
    if (gr_read_bytes(r, s->sign_key, GR_PUBLIC_KEY_LEN) != 0) return -1;
    if (gr_read_bytes(r, s->service_hash, GR_SERVICE_HASH_LEN) != 0) return -1;
    if (gr_read_bytes(r, s->content_kem_pk, GR_KEM_PUBLIC_KEY_LEN) != 0) return -1;
    if (gr_read_bytes(r, s->content_kem_sk, GR_KEM_SECRET_KEY_LEN) != 0) return -1;
    return 0;
}

static void ser_epoch(gr_buf_t *b, const gr_epoch_t *e) {
    gr_buf_write_u32(b, e->epoch_id);
    gr_buf_write(b, e->epoch_key, GR_EPOCH_KEY_LEN);
    gr_buf_write_i64(b, e->created_at);
    gr_buf_write_i64(b, e->expired_at);
    gr_buf_write(b, e->created_by, GR_PEER_ID_LEN);
}

static int de_epoch(gr_reader_t *r, gr_epoch_t *e) {
    memset(e, 0, sizeof(*e));
    if (gr_read_u32(r, &e->epoch_id) != 0) return -1;
    if (gr_read_bytes(r, e->epoch_key, GR_EPOCH_KEY_LEN) != 0) return -1;
    if (gr_read_i64(r, &e->created_at) != 0) return -1;
    if (gr_read_i64(r, &e->expired_at) != 0) return -1;
    if (gr_read_bytes(r, e->created_by, GR_PEER_ID_LEN) != 0) return -1;
    return 0;
}

static void ser_audit(gr_buf_t *b, const gr_audit_entry_t *a) {
    gr_buf_write(b, a->entry_hash, GR_HASH_LEN);
    gr_buf_write_i64(b, a->timestamp);
    gr_buf_write_i64(b, a->timestamp_ns);
    gr_buf_write_u32(b, (uint32_t)a->change_type);
    gr_buf_write(b, a->actor_id, GR_PEER_ID_LEN);
    gr_buf_write(b, a->target_id, GR_PEER_ID_LEN);
    gr_buf_write(b, a->signature, GR_SIGN_LEN);
    gr_buf_write_u32(b, a->registrar_version);
    gr_buf_write_str(b, a->detail, sizeof(a->detail));
    gr_buf_write(b, a->prev_hash, GR_HASH_LEN);
}

static int de_audit(gr_reader_t *r, gr_audit_entry_t *a, uint32_t wire_ver) {
    memset(a, 0, sizeof(*a));
    if (gr_read_bytes(r, a->entry_hash, GR_HASH_LEN) != 0) return -1;
    if (gr_read_i64(r, &a->timestamp) != 0) return -1;
    if (wire_ver >= 2) {
        if (gr_read_i64(r, &a->timestamp_ns) != 0) return -1;
    } else {
        a->timestamp_ns = a->timestamp * 1000000LL;
    }
    uint32_t ct; if (gr_read_u32(r, &ct) != 0) return -1;
    a->change_type = (gr_change_type_t)ct;
    if (gr_read_bytes(r, a->actor_id, GR_PEER_ID_LEN) != 0) return -1;
    if (gr_read_bytes(r, a->target_id, GR_PEER_ID_LEN) != 0) return -1;
    if (gr_read_bytes(r, a->signature, GR_SIGN_LEN) != 0) return -1;
    if (gr_read_u32(r, &a->registrar_version) != 0) return -1;
    if (gr_read_str(r, a->detail, sizeof(a->detail)) != 0) return -1;
    if (gr_read_bytes(r, a->prev_hash, GR_HASH_LEN) != 0) return -1;
    return 0;
}

static void ser_header(gr_buf_t *b, const gr_header_t *h) {
    gr_buf_write(b, h->group_id, GR_HASH_LEN);
    gr_buf_write_u32(b, (uint32_t)h->group_type);
    gr_buf_write_str(b, h->group_name, GR_MAX_NAME_LEN);
    gr_buf_write_u32(b, h->version);
    gr_buf_write_i64(b, h->created_at);
    gr_buf_write_i64(b, h->updated_at);
    gr_buf_write_u32(b, h->epoch_id);
    gr_buf_write_i64(b, h->retention.message_retention_ms);
    gr_buf_write_i64(b, h->retention.file_retention_ms);
    gr_buf_write_i64(b, h->retention.registrar_max_bytes);
    gr_buf_write(b, h->owner_id, GR_PEER_ID_LEN);
    gr_buf_write(b, h->owner_sign_key, GR_PUBLIC_KEY_LEN);
    gr_buf_write(b, h->signer_id, GR_PEER_ID_LEN);
    gr_buf_write(b, h->signer_sign_key, GR_PUBLIC_KEY_LEN);
    gr_buf_write(b, h->signature, GR_SIGN_LEN);
    gr_buf_write(b, h->hash, GR_HASH_LEN);
}

static int de_header(gr_reader_t *r, gr_header_t *h) {
    memset(h, 0, sizeof(*h));
    if (gr_read_bytes(r, h->group_id, GR_HASH_LEN) != 0) return -1;
    uint32_t gt; if (gr_read_u32(r, &gt) != 0) return -1;
    h->group_type = (gr_group_type_t)gt;
    if (gr_read_str(r, h->group_name, GR_MAX_NAME_LEN) != 0) return -1;
    if (gr_read_u32(r, &h->version) != 0) return -1;
    if (gr_read_i64(r, &h->created_at) != 0) return -1;
    if (gr_read_i64(r, &h->updated_at) != 0) return -1;
    if (gr_read_u32(r, &h->epoch_id) != 0) return -1;
    if (gr_read_i64(r, &h->retention.message_retention_ms) != 0) return -1;
    if (gr_read_i64(r, &h->retention.file_retention_ms) != 0) return -1;
    if (gr_read_i64(r, &h->retention.registrar_max_bytes) != 0) return -1;
    if (gr_read_bytes(r, h->owner_id, GR_PEER_ID_LEN) != 0) return -1;
    if (gr_read_bytes(r, h->owner_sign_key, GR_PUBLIC_KEY_LEN) != 0) return -1;
    if (gr_read_bytes(r, h->signer_id, GR_PEER_ID_LEN) != 0) return -1;
    if (gr_read_bytes(r, h->signer_sign_key, GR_PUBLIC_KEY_LEN) != 0) return -1;
    if (gr_read_bytes(r, h->signature, GR_SIGN_LEN) != 0) return -1;
    if (gr_read_bytes(r, h->hash, GR_HASH_LEN) != 0) return -1;
    return 0;
}

/* ── DB insert helpers ─────────────────────────────────────────── */

static gr_error_t db_insert_peer(gr_registrar_t *reg, const gr_peer_t *p) {
    duckdb_prepared_statement stmt = reg->ps_peer_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, p->peer_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 2, p->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    duckdb_bind_blob(stmt, 3, p->sign_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_varchar(stmt, 4, p->ip);
    duckdb_bind_int32(stmt, 5, (int32_t)p->port);
    duckdb_bind_int32(stmt, 6, (int32_t)p->status);
    duckdb_bind_int32(stmt, 7, (int32_t)p->role_id);
    duckdb_bind_int64(stmt, 8, p->joined_at);
    duckdb_bind_int64(stmt, 9, p->removed_at);
    duckdb_bind_int64(stmt, 10, p->last_seen);
    duckdb_bind_varchar(stmt, 11, p->removed_reason);
    duckdb_bind_blob(stmt, 12, p->removed_by, GR_PEER_ID_LEN);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(stmt, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

static gr_error_t db_insert_role(gr_registrar_t *reg, const gr_role_t *r) {
    duckdb_prepared_statement stmt = reg->ps_role_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)r->role_id);
    duckdb_bind_varchar(stmt, 2, r->name);
    duckdb_bind_int32(stmt, 3, (int32_t)r->permissions);
    duckdb_bind_blob(stmt, 4, r->sign_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_int64(stmt, 5, r->created_at);
    duckdb_bind_int64(stmt, 6, r->modified_at);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(stmt, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

static gr_error_t db_insert_webapp(gr_registrar_t *reg, const gr_webapp_t *w) {
    duckdb_prepared_statement stmt = reg->ps_webapp_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, w->hash, GR_SERVICE_HASH_LEN);
    duckdb_bind_varchar(stmt, 2, w->name);
    duckdb_bind_int32(stmt, 3, (int32_t)w->version);
    duckdb_bind_int64(stmt, 4, w->added_at);
    duckdb_bind_blob(stmt, 5, w->added_by, GR_PEER_ID_LEN);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(stmt, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

static gr_error_t db_insert_server(gr_registrar_t *reg, const gr_server_t *s) {
    duckdb_prepared_statement stmt = reg->ps_server_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, s->id_hash, GR_HASH_LEN);
    duckdb_bind_int32(stmt, 2, (int32_t)s->type);
    duckdb_bind_varchar(stmt, 3, s->ip);
    duckdb_bind_int32(stmt, 4, (int32_t)s->port);
    duckdb_bind_blob(stmt, 5, s->sign_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_blob(stmt, 6, s->service_hash, GR_SERVICE_HASH_LEN);
    duckdb_bind_blob(stmt, 7, s->content_kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    duckdb_bind_blob(stmt, 8, s->content_kem_sk, GR_KEM_SECRET_KEY_LEN);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(stmt, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

static gr_error_t db_insert_epoch(gr_registrar_t *reg, const gr_epoch_t *e) {
    duckdb_prepared_statement stmt = reg->ps_epoch_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)e->epoch_id);
    duckdb_bind_blob(stmt, 2, e->epoch_key, GR_EPOCH_KEY_LEN);
    duckdb_bind_int64(stmt, 3, e->created_at);
    duckdb_bind_int64(stmt, 4, e->expired_at);
    duckdb_bind_blob(stmt, 5, e->created_by, GR_PEER_ID_LEN);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(stmt, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

static gr_error_t db_insert_audit(gr_registrar_t *reg, const gr_audit_entry_t *a) {
    duckdb_prepared_statement stmt = reg->ps_audit_insert;
    duckdb_clear_bindings(stmt);
    duckdb_bind_blob(stmt, 1, a->entry_hash, GR_HASH_LEN);
    duckdb_bind_int64(stmt, 2, a->timestamp);
    duckdb_bind_int32(stmt, 3, (int32_t)a->change_type);
    duckdb_bind_blob(stmt, 4, a->actor_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 5, a->target_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(stmt, 6, a->signature, GR_SIGN_LEN);
    duckdb_bind_int32(stmt, 7, (int32_t)a->registrar_version);
    duckdb_bind_varchar(stmt, 8, a->detail);
    duckdb_bind_blob(stmt, 9, a->prev_hash, GR_HASH_LEN);
    duckdb_bind_int64(stmt, 10, a->timestamp_ns);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(stmt, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

/* ── Public API ────────────────────────────────────────────────── */

gr_error_t gr_serialize(const gr_registrar_t *reg, gr_serialize_mode_t mode,
                        uint8_t **out_data, size_t *out_len) {
    if (!reg || !out_data || !out_len) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    gr_buf_t buf;
    if (gr_buf_init(&buf, 65536) != 0) { GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }

    gr_buf_write(&buf, SERIAL_MAGIC, 4);
    gr_buf_write_u32(&buf, SERIAL_VERSION);
    gr_buf_write_u8(&buf, (uint8_t)mode);
    ser_header(&buf, &reg->header);

    uint32_t n;
    gr_peer_count(reg, GR_PEER_STATUS_ANY, &n);
    gr_buf_write_u32(&buf, n);
    if (n > 0) {
        gr_peer_t *arr = calloc(n, sizeof(gr_peer_t));
        if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
        uint32_t actual; gr_peer_list(reg, arr, n, &actual, GR_PEER_STATUS_ANY);
        for (uint32_t i = 0; i < actual; i++) ser_peer(&buf, &arr[i]);
        free(arr);
    }

    gr_role_count(reg, &n);
    gr_buf_write_u32(&buf, n);
    if (n > 0) {
        gr_role_t *arr = calloc(n, sizeof(gr_role_t));
        if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
        uint32_t actual; gr_role_list(reg, arr, n, &actual);
        for (uint32_t i = 0; i < actual; i++) ser_role(&buf, &arr[i]);
        free(arr);
    }

    gr_webapp_count(reg, &n);
    gr_buf_write_u32(&buf, n);
    if (n > 0) {
        gr_webapp_t *arr = calloc(n, sizeof(gr_webapp_t));
        if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
        uint32_t actual; gr_webapp_list(reg, arr, n, &actual);
        for (uint32_t i = 0; i < actual; i++) ser_webapp(&buf, &arr[i]);
        free(arr);
    }

    for (uint32_t t = 0; t < GR_SERVER_TYPE_COUNT; t++) {
        gr_server_count(reg, (gr_server_type_t)t, &n);
        gr_buf_write_u32(&buf, n);
        if (n > 0) {
            gr_server_t *arr = calloc(n, sizeof(gr_server_t));
            if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
            uint32_t actual;
            gr_server_list(reg, (gr_server_type_t)t, arr, n, &actual);
            for (uint32_t i = 0; i < actual; i++) ser_server(&buf, &arr[i], mode);
            free(arr);
        }
    }

    gr_epoch_count(reg, &n);
    gr_buf_write_u32(&buf, n);
    if (n > 0) {
        gr_epoch_t *arr = calloc(n, sizeof(gr_epoch_t));
        if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
        uint32_t actual; gr_epoch_list(reg, arr, n, &actual);
        for (uint32_t i = 0; i < actual; i++) ser_epoch(&buf, &arr[i]);
        for (uint32_t i = 0; i < actual; i++)
            yumi_memzero(arr[i].epoch_key, GR_EPOCH_KEY_LEN);
        free(arr);
    }

    gr_audit_count(reg, &n);
    gr_buf_write_u32(&buf, n);
    if (n > 0) {
        gr_audit_entry_t *arr = calloc(n, sizeof(gr_audit_entry_t));
        if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
        uint32_t actual; gr_audit_list(reg, 0, arr, n, &actual);
        for (uint32_t i = 0; i < actual; i++) ser_audit(&buf, &arr[i]);
        free(arr);
    }

    if (buf.error) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }

    *out_data = buf.data;
    *out_len = buf.len;
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

gr_error_t gr_deserialize(gr_registrar_t *reg,
                          const uint8_t *data, size_t data_len) {
    if (!reg || !data) return GR_ERR_INVALID_PARAM;

    /* No blanket provisional guard here. The owner_sign_key
     * consistency check below is sufficient: it rejects blobs
     * from a different owner while allowing legitimate sync
     * updates to flow through during the verification window.
     * The joiner's registrar must stay current with group
     * changes (kicks, role edits, epoch rotations) that other
     * members are making while verification runs in background. */

    gr_reader_t r = { .data = data, .len = data_len, .pos = 0 };

    char magic[4];
    if (gr_read_bytes(&r, magic, 4) != 0) return GR_ERR_SERIALIZATION;
    if (memcmp(magic, SERIAL_MAGIC, 4) != 0) return GR_ERR_SERIALIZATION;

    uint32_t ver;
    if (gr_read_u32(&r, &ver) != 0 || ver > SERIAL_VERSION)
        return GR_ERR_SERIALIZATION;

    uint8_t mode;
    if (gr_read_u8(&r, &mode) != 0) return GR_ERR_SERIALIZATION;

    gr_header_t nh;
    if (de_header(&r, &nh) != 0) return GR_ERR_SERIALIZATION;

    if (yumi_mldsa_verify(nh.signature, nh.hash, GR_HASH_LEN,
                           nh.signer_sign_key) != YUMI_CRYPTO_OK)
        return GR_ERR_SIGNATURE_INVALID;

    /* Reject if the incoming blob claims a different owner than
     * we already know. A self-signed blob from a different group
     * (or an attacker-created group) would pass the signature
     * check above but must not replace our registrar. */
    if (yumi_memcmp(nh.owner_sign_key, reg->header.owner_sign_key,
                      GR_PUBLIC_KEY_LEN) != 0)
        return GR_ERR_UNAUTHORIZED;

    if (nh.version <= reg->header.version) return GR_OK;

    GR_LOCK(reg);

    if (!gr_txn_begin(reg)) {
        GR_UNLOCK(reg);
        return GR_ERR_DB;
    }

    gr_db_exec(reg->con, "DELETE FROM gr_peers");
    gr_db_exec(reg->con, "DELETE FROM gr_roles");
    gr_db_exec(reg->con, "DELETE FROM gr_webapps");
    gr_db_exec(reg->con, "DELETE FROM gr_servers");
    gr_db_exec(reg->con, "DELETE FROM gr_epochs");
    gr_db_exec(reg->con, "DELETE FROM gr_audit_log");

    uint32_t count;
    gr_error_t err;

    if (gr_read_u32(&r, &count) != 0) goto rollback_ser;
    for (uint32_t i = 0; i < count; i++) {
        gr_peer_t p; if (de_peer(&r, &p) != 0) goto rollback_ser;
        if ((err = db_insert_peer(reg, &p)) != GR_OK) goto rollback_err;
    }

    if (gr_read_u32(&r, &count) != 0) goto rollback_ser;
    for (uint32_t i = 0; i < count; i++) {
        gr_role_t rl; if (de_role(&r, &rl, ver) != 0) goto rollback_ser;
        if ((err = db_insert_role(reg, &rl)) != GR_OK) goto rollback_err;
    }

    if (gr_read_u32(&r, &count) != 0) goto rollback_ser;
    for (uint32_t i = 0; i < count; i++) {
        gr_webapp_t w; if (de_webapp(&r, &w) != 0) goto rollback_ser;
        if ((err = db_insert_webapp(reg, &w)) != GR_OK) goto rollback_err;
    }

    for (uint32_t t = 0; t < GR_SERVER_TYPE_COUNT; t++) {
        if (gr_read_u32(&r, &count) != 0) goto rollback_ser;
        for (uint32_t i = 0; i < count; i++) {
            gr_server_t s; if (de_server(&r, &s) != 0) goto rollback_ser;
            if ((err = db_insert_server(reg, &s)) != GR_OK) goto rollback_err;
        }
    }

    if (gr_read_u32(&r, &count) != 0) goto rollback_ser;
    for (uint32_t i = 0; i < count; i++) {
        gr_epoch_t e; if (de_epoch(&r, &e) != 0) goto rollback_ser;
        err = db_insert_epoch(reg, &e);
        yumi_memzero(e.epoch_key, GR_EPOCH_KEY_LEN);
        if (err != GR_OK) goto rollback_err;
    }

    if (gr_read_u32(&r, &count) != 0) goto rollback_ser;
    for (uint32_t i = 0; i < count; i++) {
        gr_audit_entry_t a; if (de_audit(&r, &a, ver) != 0) goto rollback_ser;
        if ((err = db_insert_audit(reg, &a)) != GR_OK) goto rollback_err;
    }

    reg->header = nh;
    err = gr_header_save(reg);
    if (err != GR_OK) goto rollback_err;

    if (!gr_txn_commit(reg)) {
        gr_txn_rollback(reg);
        GR_UNLOCK(reg);
        return GR_ERR_DB;
    }

    /* Prompt host if the group is large and a persist callback is set */
    if (reg->persist_prompt_fn) {
        size_t est = gr_estimate_size(reg);
        if (est > (size_t)GR_PERSIST_PROMPT_BYTES) {
            reg->persist_prompt_fn(est, reg->header.group_name,
                                   reg->persist_prompt_data);
        }
    }

    GR_UNLOCK(reg);
    return GR_OK;

rollback_ser:
    gr_txn_rollback(reg);
    GR_UNLOCK(reg);
    return GR_ERR_SERIALIZATION;
rollback_err:
    gr_txn_rollback(reg);
    GR_UNLOCK(reg);
    return err;
}

gr_error_t gr_serialize_delta(const gr_registrar_t *reg, uint32_t since_version,
                              uint8_t **out_data, size_t *out_len) {
    if (!reg || !out_data || !out_len) return GR_ERR_INVALID_PARAM;

    GR_LOCK((gr_registrar_t *)reg);

    gr_buf_t buf;
    if (gr_buf_init(&buf, 8192) != 0) { GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }

    gr_buf_write(&buf, DELTA_MAGIC, 4);
    gr_buf_write_u32(&buf, SERIAL_VERSION);
    gr_buf_write_u32(&buf, since_version);

    /* ── Sender's header snapshot ──────────────────────────────── */
    ser_header(&buf, &reg->header);

    /* ── Entity state payload ──────────────────────────────────── */
    uint32_t n;

    gr_peer_count(reg, GR_PEER_STATUS_ANY, &n);
    gr_buf_write_u32(&buf, n);
    if (n > 0) {
        gr_peer_t *arr = calloc(n, sizeof(gr_peer_t));
        if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
        uint32_t actual; gr_peer_list(reg, arr, n, &actual, GR_PEER_STATUS_ANY);
        for (uint32_t i = 0; i < actual; i++) ser_peer(&buf, &arr[i]);
        free(arr);
    }

    gr_role_count(reg, &n);
    gr_buf_write_u32(&buf, n);
    if (n > 0) {
        gr_role_t *arr = calloc(n, sizeof(gr_role_t));
        if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
        uint32_t actual; gr_role_list(reg, arr, n, &actual);
        for (uint32_t i = 0; i < actual; i++) ser_role(&buf, &arr[i]);
        free(arr);
    }

    gr_webapp_count(reg, &n);
    gr_buf_write_u32(&buf, n);
    if (n > 0) {
        gr_webapp_t *arr = calloc(n, sizeof(gr_webapp_t));
        if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
        uint32_t actual; gr_webapp_list(reg, arr, n, &actual);
        for (uint32_t i = 0; i < actual; i++) ser_webapp(&buf, &arr[i]);
        free(arr);
    }

    for (uint32_t t = 0; t < GR_SERVER_TYPE_COUNT; t++) {
        gr_server_count(reg, (gr_server_type_t)t, &n);
        gr_buf_write_u32(&buf, n);
        if (n > 0) {
            gr_server_t *arr = calloc(n, sizeof(gr_server_t));
            if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
            uint32_t actual;
            gr_server_list(reg, (gr_server_type_t)t, arr, n, &actual);
            for (uint32_t i = 0; i < actual; i++)
                ser_server(&buf, &arr[i], GR_SERIALIZE_FULL);
            free(arr);
        }
    }

    gr_epoch_count(reg, &n);
    gr_buf_write_u32(&buf, n);
    if (n > 0) {
        gr_epoch_t *arr = calloc(n, sizeof(gr_epoch_t));
        if (!arr) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
        uint32_t actual; gr_epoch_list(reg, arr, n, &actual);
        for (uint32_t i = 0; i < actual; i++) ser_epoch(&buf, &arr[i]);
        for (uint32_t i = 0; i < actual; i++)
            yumi_memzero(arr[i].epoch_key, GR_EPOCH_KEY_LEN);
        free(arr);
    }

    /* ── Audit entries since version ───────────────────────────── */
    duckdb_prepared_statement stmt = reg->ps_audit_delta;
    duckdb_clear_bindings(stmt);
    duckdb_bind_int32(stmt, 1, (int32_t)since_version);

    duckdb_result result;
    duckdb_state st = duckdb_execute_prepared(stmt, &result);
    if (st != DuckDBSuccess) {
        duckdb_destroy_result(&result);
        gr_buf_free(&buf);
        GR_UNLOCK((gr_registrar_t *)reg);
        return GR_ERR_DB;
    }

    /* Reserve slot for row count; patch after streaming chunks. */
    size_t count_offset = buf.len;
    gr_buf_write_u32(&buf, 0);
    uint32_t rows = 0;

    duckdb_data_chunk chunk;
    while ((chunk = duckdb_fetch_chunk(result)) != NULL) {
        idx_t n = duckdb_data_chunk_get_size(chunk);
        for (idx_t i = 0; i < n; i++) {
            gr_audit_entry_t entry;
            gr_audit_read_row(chunk, i, &entry);
            ser_audit(&buf, &entry);
            rows++;
        }
        duckdb_destroy_data_chunk(&chunk);
    }

    /* Patch the row count placeholder (little-endian u32). */
    if (buf.data && count_offset + 4 <= buf.len) {
        buf.data[count_offset + 0] = (uint8_t)(rows & 0xff);
        buf.data[count_offset + 1] = (uint8_t)((rows >> 8)  & 0xff);
        buf.data[count_offset + 2] = (uint8_t)((rows >> 16) & 0xff);
        buf.data[count_offset + 3] = (uint8_t)((rows >> 24) & 0xff);
    }

    duckdb_destroy_result(&result);
    if (buf.error) { gr_buf_free(&buf); GR_UNLOCK((gr_registrar_t *)reg); return GR_ERR_OUT_OF_MEMORY; }
    *out_data = buf.data;
    *out_len = buf.len;
    GR_UNLOCK((gr_registrar_t *)reg);
    return GR_OK;
}

/* ── Delta entity upsert helpers ───────────────────────────────── */

static gr_error_t db_delta_upsert_peer(gr_registrar_t *reg, const gr_peer_t *p) {
    duckdb_prepared_statement s = reg->ps_delta_peer_upsert;
    duckdb_clear_bindings(s);
    duckdb_bind_blob(s, 1, p->peer_id, GR_PEER_ID_LEN);
    duckdb_bind_blob(s, 2, p->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    duckdb_bind_blob(s, 3, p->sign_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_varchar(s, 4, p->ip);
    duckdb_bind_int32(s, 5, (int32_t)p->port);
    duckdb_bind_int32(s, 6, (int32_t)p->status);
    duckdb_bind_int32(s, 7, (int32_t)p->role_id);
    duckdb_bind_int64(s, 8, p->joined_at);
    duckdb_bind_int64(s, 9, p->removed_at);
    duckdb_bind_int64(s, 10, p->last_seen);
    duckdb_bind_varchar(s, 11, p->removed_reason);
    duckdb_bind_blob(s, 12, p->removed_by, GR_PEER_ID_LEN);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(s, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

static gr_error_t db_delta_upsert_role(gr_registrar_t *reg, const gr_role_t *rl) {
    duckdb_prepared_statement s = reg->ps_delta_role_upsert;
    duckdb_clear_bindings(s);
    duckdb_bind_int32(s, 1, (int32_t)rl->role_id);
    duckdb_bind_varchar(s, 2, rl->name);
    duckdb_bind_int32(s, 3, (int32_t)rl->permissions);
    duckdb_bind_blob(s, 4, rl->sign_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_int64(s, 5, rl->created_at);
    duckdb_bind_int64(s, 6, rl->modified_at);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(s, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

static gr_error_t db_delta_upsert_webapp(gr_registrar_t *reg, const gr_webapp_t *w) {
    duckdb_prepared_statement s = reg->ps_delta_webapp_upsert;
    duckdb_clear_bindings(s);
    duckdb_bind_blob(s, 1, w->hash, GR_SERVICE_HASH_LEN);
    duckdb_bind_varchar(s, 2, w->name);
    duckdb_bind_int32(s, 3, (int32_t)w->version);
    duckdb_bind_int64(s, 4, w->added_at);
    duckdb_bind_blob(s, 5, w->added_by, GR_PEER_ID_LEN);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(s, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

static gr_error_t db_delta_upsert_server(gr_registrar_t *reg, const gr_server_t *sv) {
    duckdb_prepared_statement s = reg->ps_delta_server_upsert;
    duckdb_clear_bindings(s);
    duckdb_bind_blob(s, 1, sv->id_hash, GR_HASH_LEN);
    duckdb_bind_int32(s, 2, (int32_t)sv->type);
    duckdb_bind_varchar(s, 3, sv->ip);
    duckdb_bind_int32(s, 4, (int32_t)sv->port);
    duckdb_bind_blob(s, 5, sv->sign_key, GR_PUBLIC_KEY_LEN);
    duckdb_bind_blob(s, 6, sv->service_hash, GR_SERVICE_HASH_LEN);
    duckdb_bind_blob(s, 7, sv->content_kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    duckdb_bind_blob(s, 8, sv->content_kem_sk, GR_KEM_SECRET_KEY_LEN);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(s, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

static gr_error_t db_delta_upsert_epoch(gr_registrar_t *reg, const gr_epoch_t *e) {
    duckdb_prepared_statement s = reg->ps_delta_epoch_upsert;
    duckdb_clear_bindings(s);
    duckdb_bind_int32(s, 1, (int32_t)e->epoch_id);
    duckdb_bind_blob(s, 2, e->epoch_key, GR_EPOCH_KEY_LEN);
    duckdb_bind_int64(s, 3, e->created_at);
    duckdb_bind_int64(s, 4, e->expired_at);
    duckdb_bind_blob(s, 5, e->created_by, GR_PEER_ID_LEN);
    duckdb_result res;
    duckdb_state st = duckdb_execute_prepared(s, &res);
    duckdb_destroy_result(&res);
    return st == DuckDBSuccess ? GR_OK : GR_ERR_DB;
}

/* ── Helper: max of three int64_t values ───────────────────────── */

static int64_t max3_i64(int64_t a, int64_t b, int64_t c) {
    int64_t m = a;
    if (b > m) m = b;
    if (c > m) m = c;
    return m;
}

/* ── Helper: free delta arrays ─────────────────────────────────── */

static void delta_state_free(gr_peer_t *peers, gr_role_t *roles,
                             gr_webapp_t *webapps,
                             gr_server_t **servers,
                             gr_epoch_t *epochs) {
    free(peers);
    free(roles);
    free(webapps);
    for (uint32_t t = 0; t < GR_SERVER_TYPE_COUNT; t++)
        free(servers[t]);
    if (epochs) {
        free(epochs);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Delta Merge — Full State Sync with Conflict Resolution
 *
 *  Wire format (v2):
 *    GRDT magic (4) | u32 version | u32 since_version
 *    header snapshot (sender's view)
 *    entity state: peers, roles, webapps, servers, epochs
 *    u32 audit entry count → [audit entries...]
 *
 *  Strategy:
 *  1.  Parse header + entity state + audit entries from wire.
 *  2.  Validate audit entries: hash integrity, Ed25519 signature,
 *      point-in-time authorization using the delta's entity state
 *      (representing the sender's permissions at delta creation).
 *  3.  Merge entity state via last-writer-wins with nanosecond
 *      timestamp resolution for deterministic conflict ordering.
 *  4.  Epochs are append-only: insert-if-not-exists.
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_apply_delta(gr_registrar_t *reg,
                          const uint8_t *data, size_t data_len,
                          gr_merge_result_t *result_out) {
    if (!reg || !data) return GR_ERR_INVALID_PARAM;

    /* ── Delta size cap ────────────────────────────────────────── */
    if (data_len > GR_DELTA_MAX_BYTES) {
        if (reg->delta_anomaly_fn) {
            int64_t gap = gr_timestamp_ms() - reg->header.updated_at;
            gr_delta_action_t action = reg->delta_anomaly_fn(
                data_len, 0, gap, reg->delta_anomaly_data);
            if (action == GR_DELTA_SUSPEND)
                return GR_ERR_SIZE_EXCEEDED;
        } else {
            return GR_ERR_SIZE_EXCEEDED;
        }
    }

    GR_LOCK(reg);

    gr_reader_t r = { .data = data, .len = data_len, .pos = 0 };

    char magic[4];
    if (gr_read_bytes(&r, magic, 4) != 0) { GR_UNLOCK(reg); return GR_ERR_SERIALIZATION; }
    if (memcmp(magic, DELTA_MAGIC, 4) != 0) { GR_UNLOCK(reg); return GR_ERR_SERIALIZATION; }

    uint32_t wire_ver;
    if (gr_read_u32(&r, &wire_ver) != 0 || wire_ver > SERIAL_VERSION)
        { GR_UNLOCK(reg); return GR_ERR_SERIALIZATION; }

    uint32_t since_version;
    if (gr_read_u32(&r, &since_version) != 0) { GR_UNLOCK(reg); return GR_ERR_SERIALIZATION; }

    /* ── Parse sender header + entity state (v2+) ──────────────── */
    gr_header_t delta_header;
    memset(&delta_header, 0, sizeof(delta_header));

    gr_peer_t   *delta_peers   = NULL;  uint32_t dpc = 0;
    gr_role_t   *delta_roles   = NULL;  uint32_t drc = 0;
    gr_webapp_t *delta_webapps = NULL;  uint32_t dwc = 0;
    gr_server_t *delta_servers[GR_SERVER_TYPE_COUNT];
    uint32_t     dsc[GR_SERVER_TYPE_COUNT];
    gr_epoch_t  *delta_epochs  = NULL;  uint32_t dec = 0;
    for (uint32_t t = 0; t < GR_SERVER_TYPE_COUNT; t++) {
        delta_servers[t] = NULL; dsc[t] = 0;
    }

    bool has_state = (wire_ver >= 2);

    if (has_state) {
        if (de_header(&r, &delta_header) != 0) { GR_UNLOCK(reg); return GR_ERR_SERIALIZATION; }

        /* Verify sender's header signature */
        if (yumi_mldsa_verify(delta_header.signature,
                delta_header.hash, GR_HASH_LEN,
                delta_header.signer_sign_key) != YUMI_CRYPTO_OK)
            { GR_UNLOCK(reg); return GR_ERR_SIGNATURE_INVALID; }

        /* Reject different owner */
        if (yumi_memcmp(delta_header.owner_sign_key,
                          reg->header.owner_sign_key,
                          GR_PUBLIC_KEY_LEN) != 0)
            { GR_UNLOCK(reg); return GR_ERR_UNAUTHORIZED; }

        /* Peers */
        if (gr_read_u32(&r, &dpc) != 0) goto parse_fail;
        if (dpc > r.len - r.pos) goto parse_fail;
        if (dpc > 0) {
            delta_peers = calloc(dpc, sizeof(gr_peer_t));
            if (!delta_peers) goto oom_fail;
            for (uint32_t i = 0; i < dpc; i++)
                if (de_peer(&r, &delta_peers[i]) != 0) goto parse_fail;
        }

        /* Roles */
        if (gr_read_u32(&r, &drc) != 0) goto parse_fail;
        if (drc > r.len - r.pos) goto parse_fail;
        if (drc > 0) {
            delta_roles = calloc(drc, sizeof(gr_role_t));
            if (!delta_roles) goto parse_fail;
            for (uint32_t i = 0; i < drc; i++)
                if (de_role(&r, &delta_roles[i], wire_ver) != 0) goto parse_fail;
        }

        /* Webapps */
        if (gr_read_u32(&r, &dwc) != 0) goto parse_fail;
        if (dwc > r.len - r.pos) goto parse_fail;
        if (dwc > 0) {
            delta_webapps = calloc(dwc, sizeof(gr_webapp_t));
            if (!delta_webapps) goto parse_fail;
            for (uint32_t i = 0; i < dwc; i++)
                if (de_webapp(&r, &delta_webapps[i]) != 0) goto parse_fail;
        }

        /* Servers */
        for (uint32_t t = 0; t < GR_SERVER_TYPE_COUNT; t++) {
            if (gr_read_u32(&r, &dsc[t]) != 0) goto parse_fail;
            if (dsc[t] > r.len - r.pos) goto parse_fail;
            if (dsc[t] > 0) {
                delta_servers[t] = calloc(dsc[t], sizeof(gr_server_t));
                if (!delta_servers[t]) goto oom_fail;
                for (uint32_t i = 0; i < dsc[t]; i++)
                    if (de_server(&r, &delta_servers[t][i]) != 0) goto parse_fail;
            }
        }

        /* Epochs */
        if (gr_read_u32(&r, &dec) != 0) goto parse_fail;
        if (dec > r.len - r.pos) goto parse_fail;
        if (dec > 0) {
            delta_epochs = calloc(dec, sizeof(gr_epoch_t));
            if (!delta_epochs) goto parse_fail;
            for (uint32_t i = 0; i < dec; i++)
                if (de_epoch(&r, &delta_epochs[i]) != 0) goto parse_fail;
        }
    }

    /* ── Parse audit entries ───────────────────────────────────── */
    uint32_t entry_count;
    if (gr_read_u32(&r, &entry_count) != 0) goto parse_fail;
    if (entry_count > r.len - r.pos) goto parse_fail;

    gr_merge_result_t mr = {0};
    mr.entries_received = entry_count;

    if (!gr_txn_begin(reg)) goto db_fail;

    for (uint32_t i = 0; i < entry_count; i++) {
        gr_audit_entry_t entry;
        if (de_audit(&r, &entry, wire_ver) != 0) {
            gr_txn_rollback(reg); goto parse_fail;
        }

        /* Dedup check */
        duckdb_prepared_statement dedup_stmt = reg->ps_audit_dedup;
        duckdb_clear_bindings(dedup_stmt);
        duckdb_bind_blob(dedup_stmt, 1, entry.entry_hash, GR_HASH_LEN);
        duckdb_result qr;
        duckdb_state st = duckdb_execute_prepared(dedup_stmt, &qr);
        if (st != DuckDBSuccess) {
            duckdb_destroy_result(&qr);
            gr_txn_rollback(reg); goto db_fail;
        }
        {
            int64_t dup_count = 0;
            gr_db_fetch_i64_scalar(&qr, &dup_count);
            duckdb_destroy_result(&qr);
            if (dup_count > 0) { mr.entries_duplicate++; continue; }
        }

        /* Hash integrity verification (Skein-1024) */
        uint8_t computed[GR_HASH_LEN];
        yumi_skein_ctx_t *gs = NULL;
        if (yumi_skein_init(&gs) != YUMI_CRYPTO_OK) {
            gr_txn_rollback(reg); goto parse_fail;
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
            mr.entries_rejected++; continue;
        }

        /* Signature verification (ML-DSA-87) */
        duckdb_prepared_statement pk_stmt = reg->ps_audit_actor_pk;
        duckdb_clear_bindings(pk_stmt);
        duckdb_bind_blob(pk_stmt, 1, entry.actor_id, GR_PEER_ID_LEN);
        st = duckdb_execute_prepared(pk_stmt, &qr);
        uint8_t actor_pk[GR_PUBLIC_KEY_LEN];
        {
            duckdb_data_chunk pk_ch = NULL;
            if (st != DuckDBSuccess
                || gr_db_fetch_first_chunk(&qr, &pk_ch) != GR_OK) {
                duckdb_destroy_result(&qr); mr.entries_rejected++; continue;
            }
            gr_db_vec_get_blob(duckdb_data_chunk_get_vector(pk_ch, 0), 0,
                               actor_pk, GR_PUBLIC_KEY_LEN);
            duckdb_destroy_data_chunk(&pk_ch);
        }
        duckdb_destroy_result(&qr);

        if (yumi_mldsa_verify(entry.signature, entry.entry_hash,
                               GR_HASH_LEN, actor_pk) != YUMI_CRYPTO_OK) {
            mr.entries_rejected++; continue;
        }

        /* ── Point-in-time authorization ───────────────────────
         *  Use the delta's entity state (the sender's view at
         *  delta-creation time) for permission lookup. This
         *  ensures an admin who was demoted AFTER creating
         *  legitimate entries still has those entries accepted.
         *
         *  Priority: delta state > local state, so that prior
         *  valid permissions are honoured even if the local
         *  registrar has already processed the demotion.
         *  The entry's cryptographic signature proves identity;
         *  the delta's role snapshot proves authorization at
         *  the time of creation.
         * ────────────────────────────────────────────────────── */
        uint32_t perms = 0;
        bool auth_found = false;

        if (has_state) {
            /* Owner always authorized */
            if (yumi_memcmp(entry.actor_id, delta_header.owner_id,
                              GR_PEER_ID_LEN) == 0) {
                perms = GR_PERM_OWNER;
                auth_found = true;
            }

            if (!auth_found) {
                for (uint32_t j = 0; j < dpc; j++) {
                    if (yumi_memcmp(delta_peers[j].peer_id,
                                      entry.actor_id,
                                      GR_PEER_ID_LEN) != 0)
                        continue;

                    /* Was the peer active at entry time? */
                    bool was_active =
                        (delta_peers[j].status == GR_PEER_ACTIVE) ||
                        (delta_peers[j].removed_at > 0 &&
                         entry.timestamp < delta_peers[j].removed_at);

                    if (was_active) {
                        if (delta_peers[j].role_id == 0) {
                            perms = GR_PERM_NONE;
                            auth_found = true;
                        } else {
                            for (uint32_t k = 0; k < drc; k++) {
                                if (delta_roles[k].role_id ==
                                    delta_peers[j].role_id) {
                                    perms = delta_roles[k].permissions;
                                    auth_found = true;
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }

        if (!auth_found) {
            /* Fallback to local state */
            if (gr_is_owner(reg, entry.actor_id)) {
                perms = GR_PERM_OWNER;
            } else {
                perms = gr_get_peer_permissions(reg, entry.actor_id);
            }
        }

        bool authorized = false;
        switch (entry.change_type) {
            case GR_CHANGE_PEER_ADDED: case GR_CHANGE_INVITE_CREATED:
            case GR_CHANGE_INVITE_USED: case GR_CHANGE_INVITE_INVALIDATED:
                authorized = (perms & GR_PERM_INVITE_MEMBER) != 0; break;
            case GR_CHANGE_PEER_KICKED: case GR_CHANGE_PEER_REMOVED:
                authorized = (perms & GR_PERM_KICK_MEMBER) != 0; break;
            case GR_CHANGE_PEER_BANNED:
                authorized = (perms & GR_PERM_BAN_MEMBER) != 0; break;
            case GR_CHANGE_PEER_ADDRESS:
                authorized = true; break;
            case GR_CHANGE_PEER_ROLE_CHANGED: case GR_CHANGE_ROLE_ADDED:
            case GR_CHANGE_ROLE_REMOVED: case GR_CHANGE_ROLE_MODIFIED:
                authorized = (perms & GR_PERM_EDIT_ROLES) != 0; break;
            case GR_CHANGE_WEBAPP_ADDED:
                authorized = (perms & GR_PERM_ADD_WEBAPP) != 0; break;
            case GR_CHANGE_WEBAPP_REMOVED:
                authorized = (perms & GR_PERM_REMOVE_WEBAPP) != 0; break;
            case GR_CHANGE_SERVER_ADDED: case GR_CHANGE_SERVER_REMOVED:
                authorized = (perms & GR_PERM_EDIT_SERVERS) != 0; break;
            case GR_CHANGE_EPOCH_ROTATED:
                authorized = (perms & GR_PERM_ROTATE_EPOCH) != 0; break;
            case GR_CHANGE_RETENTION_SET:
                authorized = (perms & GR_PERM_EDIT_RETENTION) != 0; break;
            case GR_CHANGE_REGISTRAR_SIGNED:
                authorized = has_state
                    ? yumi_memcmp(entry.actor_id, delta_header.owner_id,
                                    GR_PEER_ID_LEN) == 0
                    : gr_is_owner(reg, entry.actor_id);
                break;
            case GR_CHANGE_GROUP_ICON_SET: case GR_CHANGE_GROUP_ICON_REMOVED:
                authorized = (perms & GR_PERM_SET_GROUP_ICON) != 0; break;
            case GR_CHANGE_BOOT_IP_BLOCKED:
                /* Local-only event, always accept during delta merge */
                authorized = true; break;
        }

        if (!authorized) { mr.entries_rejected++; continue; }

        gr_error_t ierr = db_insert_audit(reg, &entry);
        if (ierr != GR_OK) {
            gr_txn_rollback(reg); goto db_fail;
        }
        mr.entries_new++;

        /* Fork detection: check if another entry already references
         * the same prev_hash — indicates a concurrent mutation
         * (partition-induced fork). Both branches are preserved. */
        {
            duckdb_prepared_statement fc = reg->ps_audit_fork_check;
            duckdb_clear_bindings(fc);
            duckdb_bind_blob(fc, 1, entry.prev_hash, GR_HASH_LEN);
            duckdb_result fr;
            if (duckdb_execute_prepared(fc, &fr) == DuckDBSuccess) {
                int64_t fcount = 0;
                if (gr_db_fetch_i64_scalar(&fr, &fcount) == GR_OK
                    && fcount > 1)
                    mr.forks_detected++;
            }
            duckdb_destroy_result(&fr);
        }

        if (entry.registrar_version > reg->header.version)
            reg->header.version = entry.registrar_version;
        if (entry.timestamp > reg->header.updated_at)
            reg->header.updated_at = entry.timestamp;
    }

    /* ── Entity state merge (v2+) ──────────────────────────────
     *  Last-writer-wins using nanosecond timestamp resolution.
     *  For each entity type, compare the incoming entity's
     *  freshness timestamp against the local entity's.  The
     *  entity with the higher timestamp wins (like git's
     *  fast-forward merge for non-conflicting changes).
     * ────────────────────────────────────────────────────────── */
    if (has_state) {
        /* Peers: keyed by peer_id, LWW via max(last_seen, joined_at, removed_at) */
        for (uint32_t i = 0; i < dpc; i++) {
            gr_peer_t local;
            gr_error_t err = gr_peer_get(reg, delta_peers[i].peer_id, &local);
            if (err == GR_OK) {
                int64_t local_ts = max3_i64(local.last_seen,
                                            local.joined_at,
                                            local.removed_at);
                int64_t incoming_ts = max3_i64(delta_peers[i].last_seen,
                                               delta_peers[i].joined_at,
                                               delta_peers[i].removed_at);
                if (incoming_ts <= local_ts) continue;
                mr.conflicts_resolved++;
            }
            db_delta_upsert_peer(reg, &delta_peers[i]);
        }

        /* Roles: keyed by role_id, LWW via modified_at */
        for (uint32_t i = 0; i < drc; i++) {
            gr_role_t local;
            gr_error_t err = gr_role_get(reg, delta_roles[i].role_id, &local);
            if (err == GR_OK) {
                if (delta_roles[i].modified_at <= local.modified_at) continue;
                mr.conflicts_resolved++;
            }
            db_delta_upsert_role(reg, &delta_roles[i]);
        }

        /* Webapps: keyed by hash, always upsert */
        for (uint32_t i = 0; i < dwc; i++) {
            db_delta_upsert_webapp(reg, &delta_webapps[i]);
        }

        /* Servers: keyed by id_hash, preserve local secret keys */
        for (uint32_t t = 0; t < GR_SERVER_TYPE_COUNT; t++) {
            for (uint32_t i = 0; i < dsc[t]; i++) {
                /* If incoming content_secret_key is zeroed (non-owner
                 * serialization), preserve the local copy. */
                uint8_t zero_key[GR_KEM_SECRET_KEY_LEN];
                memset(zero_key, 0, sizeof(zero_key));
                if (yumi_memcmp(delta_servers[t][i].content_kem_sk,
                                  zero_key, GR_KEM_SECRET_KEY_LEN) == 0) {
                    duckdb_prepared_statement gs = reg->ps_server_get;
                    duckdb_clear_bindings(gs);
                    duckdb_bind_blob(gs, 1, delta_servers[t][i].id_hash,
                                     GR_HASH_LEN);
                    duckdb_result sr;
                    if (duckdb_execute_prepared(gs, &sr) == DuckDBSuccess) {
                        duckdb_data_chunk sch = NULL;
                        if (gr_db_fetch_first_chunk(&sr, &sch) == GR_OK) {
                            gr_db_vec_get_blob(
                                duckdb_data_chunk_get_vector(sch, 7), 0,
                                delta_servers[t][i].content_kem_sk,
                                GR_KEM_SECRET_KEY_LEN);
                            duckdb_destroy_data_chunk(&sch);
                        }
                    }
                    duckdb_destroy_result(&sr);
                }
                db_delta_upsert_server(reg, &delta_servers[t][i]);
            }
        }

        /* Epochs: append-only, insert if not exists */
        for (uint32_t i = 0; i < dec; i++) {
            gr_epoch_t local;
            gr_error_t err = gr_epoch_get(reg, delta_epochs[i].epoch_id, &local);
            if (err == GR_OK) continue;
            db_delta_upsert_epoch(reg, &delta_epochs[i]);
            yumi_memzero(delta_epochs[i].epoch_key, GR_EPOCH_KEY_LEN);
        }
    }

    mr.new_version = reg->header.version;
    if (mr.entries_new > 0 || mr.conflicts_resolved > 0) {
        gr_error_t herr = gr_header_save(reg);
        if (herr != GR_OK) { gr_txn_rollback(reg); goto db_fail; }
    }
    if (!gr_txn_commit(reg)) { gr_txn_rollback(reg); goto db_fail; }

    delta_state_free(delta_peers, delta_roles, delta_webapps,
                     delta_servers, delta_epochs);

    /* Enforce audit log retention after successful merge */
    if (mr.entries_new > 0)
        gr_audit_enforce_retention_internal(reg);

    if (result_out) *result_out = mr;
    GR_UNLOCK(reg);
    return GR_OK;

parse_fail:
    delta_state_free(delta_peers, delta_roles, delta_webapps,
                     delta_servers, delta_epochs);
    GR_UNLOCK(reg);
    return GR_ERR_SERIALIZATION;

oom_fail:
    delta_state_free(delta_peers, delta_roles, delta_webapps,
                     delta_servers, delta_epochs);
    GR_UNLOCK(reg);
    return GR_ERR_OUT_OF_MEMORY;

db_fail:
    delta_state_free(delta_peers, delta_roles, delta_webapps,
                     delta_servers, delta_epochs);
    GR_UNLOCK(reg);
    return GR_ERR_DB;
}
