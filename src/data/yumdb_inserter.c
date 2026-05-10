/*
    YumDB — Inserter Builder Implementation
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

#include "yumi_internal.h"
#include <string.h>
#include <stdlib.h>

typedef struct InsImpl {
    struct YumDB_Inserter vt;
    YumDB_Impl*  db;
    YumDB_Tx     tx;           /* non-NULL if inside explicit tx */
    char*        table_name;
    YumDB_Row    staged;       /* row being built via Set() */
    YumDB_Row*   batch;
    size_t       batch_n;
    bool         on_conflict_ignore;
    bool         on_conflict_replace;
    YumDB_Error  last_err;
} InsImpl;

#define II(x) ((InsImpl*)(x))

/* Ensure staged row exists, lazily constructed once schema is known. */
static YumDB_Error ensure_staged(InsImpl* ii) {
    if (ii->staged) return YumDB_Error_OK;
    Table* t = db_find_table(ii->db, ii->table_name);
    if (!t) return YumDB_Error_NOT_FOUND;
    ii->staged = YumDB_RowNew(t->schema);
    if (!ii->staged) return YumDB_Error_OUT_OF_MEMORY;
    return YumDB_Error_OK;
}

static YumDB_Inserter ins_set(YumDB_Inserter self, const char* col, YumDB_Value v) {
    InsImpl* ii = II(self);
    if (ensure_staged(ii) != YumDB_Error_OK) { ii->last_err = YumDB_Error_NOT_FOUND; return self; }
    YumDB_Error e = ii->staged->Set(ii->staged, col, v);
    if (e != YumDB_Error_OK) ii->last_err = e;
    return self;
}

static YumDB_Inserter ins_set_row(YumDB_Inserter self, YumDB_Row row) {
    InsImpl* ii = II(self);
    if (ii->staged) { ii->staged->Destroy(ii->staged); ii->staged = NULL; }
    ii->staged = row ? row->Clone(row) : NULL;
    return self;
}

static YumDB_Inserter ins_batch(YumDB_Inserter self, YumDB_Row* rows, size_t n) {
    InsImpl* ii = II(self);
    if (ii->batch) {
        for (size_t i = 0; i < ii->batch_n; i++) ii->batch[i]->Destroy(ii->batch[i]);
        free(ii->batch);
    }
    ii->batch = (YumDB_Row*)calloc(n ? n : 1, sizeof(*ii->batch));
    ii->batch_n = n;
    for (size_t i = 0; i < n; i++) ii->batch[i] = rows[i] ? rows[i]->Clone(rows[i]) : NULL;
    return self;
}

static YumDB_Inserter ins_oci(YumDB_Inserter self) { II(self)->on_conflict_ignore = true;  II(self)->on_conflict_replace = false; return self; }
static YumDB_Inserter ins_ocr(YumDB_Inserter self) { II(self)->on_conflict_replace = true; II(self)->on_conflict_ignore = false; return self; }

/* ---- validation + constraint enforcement ---- */

static YumDB_Error validate_and_apply_defaults(Table* t, YumDB_Row row) {
    for (size_t i = 0; i < t->schema->columns_count; i++) {
        YumDB_Column c = t->schema->columns[i];
        YumDB_Value* v = &row->values[i];

        /* Apply default if null and a default exists */
        if (v->is_null && !c->default_value.is_null) {
            YumDB_ValueFree(v);
            *v = YumDB_ValueClone(c->default_value);
        }

        /* Nullability */
        if (v->is_null) {
            if (!c->nullable && !c->auto_increment)
                return YumDB_Error_NULL_VIOLATION;
            continue;
        }

        /* Type check */
        if (v->type != c->type) {
            /* allow numeric coercion */
            if (YumDB_ColumnTypeIsNumeric(v->type) && YumDB_ColumnTypeIsNumeric(c->type)) {
                YumDB_Value cast;
                YumDB_Error e = YumDB_ValueCast(*v, c->type, &cast);
                if (e != YumDB_Error_OK) return YumDB_Error_INVALID_TYPE;
                YumDB_ValueFree(v);
                *v = cast;
            } else {
                return YumDB_Error_INVALID_TYPE;
            }
        }

        /* Length / numeric bounds via min/max */
        if (c->type == YumDB_ColumnType_STRING || c->type == YumDB_ColumnType_BINARY) {
            size_t len = (c->type == YumDB_ColumnType_STRING) ? v->as.str.len : v->as.bin.len;
            if (c->len  && len != c->len) return YumDB_Error_CONSTRAINT_VIOLATION;
            if (c->min  && len <  c->min) return YumDB_Error_CONSTRAINT_VIOLATION;
            if (c->max  && len >  c->max) return YumDB_Error_CONSTRAINT_VIOLATION;
        } else if (YumDB_ColumnTypeIsIntegral(c->type)) {
            /* min/max are size_t; interpret as unsigned bounds */
            uint64_t u = 0;
            switch (c->type) {
                case YumDB_ColumnType_INT8:   u = (uint64_t)(int64_t)v->as.i8; break;
                case YumDB_ColumnType_UINT8:  u = v->as.u8; break;
                case YumDB_ColumnType_INT16:  u = (uint64_t)(int64_t)v->as.i16; break;
                case YumDB_ColumnType_UINT16: u = v->as.u16; break;
                case YumDB_ColumnType_INT32:  u = (uint64_t)(int64_t)v->as.i32; break;
                case YumDB_ColumnType_UINT32: u = v->as.u32; break;
                case YumDB_ColumnType_INT64:  u = (uint64_t)v->as.i64; break;
                case YumDB_ColumnType_UINT64: u = v->as.u64; break;
                default: break;
            }
            if (c->min && u < (uint64_t)c->min) return YumDB_Error_CONSTRAINT_VIOLATION;
            if (c->max && u > (uint64_t)c->max) return YumDB_Error_CONSTRAINT_VIOLATION;
        }
    }
    return YumDB_Error_OK;
}

/* Caller must hold t->lock for writing. */
static YumDB_Error check_uniques(Table* t, YumDB_Row row, RowSlot* ignore) {
    for (size_t i = 0; i < t->schema->columns_count; i++) {
        YumDB_Column c = t->schema->columns[i];
        if (!c->unique && !c->primary_key) continue;
        YumDB_Value* nv = &row->values[i];
        if (nv->is_null) continue;
        for (size_t k = 0; k < t->rows_count; k++) {
            RowSlot* s = t->rows[k];
            if (!s || !s->live || s == ignore) continue;
            if (YumDB_ValueEqual(s->values[i], *nv))
                return YumDB_Error_UNIQUE_VIOLATION;
        }
    }
    return YumDB_Error_OK;
}

/* Apply auto-increment to any AI integral column whose value is null. */
static void apply_auto_increment(Table* t, YumDB_Row row) {
    for (size_t i = 0; i < t->schema->columns_count; i++) {
        YumDB_Column c = t->schema->columns[i];
        if (!c->auto_increment) continue;
        if (!row->values[i].is_null) continue;
        uint64_t next = ++t->next_id;
        YumDB_ValueFree(&row->values[i]);
        switch (c->type) {
            case YumDB_ColumnType_INT32:  row->values[i] = YumDB_I32((int32_t)next); break;
            case YumDB_ColumnType_UINT32: row->values[i] = YumDB_U32((uint32_t)next); break;
            case YumDB_ColumnType_INT64:  row->values[i] = YumDB_I64((int64_t)next); break;
            case YumDB_ColumnType_UINT64: row->values[i] = YumDB_U64(next); break;
            default: break;
        }
    }
}

/* ---- commit single staged row ---- */

static YumDB_Row do_insert_one(InsImpl* ii, YumDB_Row staged, YumDB_Error* err) {
    Table* t = db_find_table(ii->db, ii->table_name);
    if (!t) { *err = YumDB_Error_NOT_FOUND; return NULL; }

    pthread_rwlock_wrlock(&t->lock);

    apply_auto_increment(t, staged);

    YumDB_Error e = validate_and_apply_defaults(t, staged);
    if (e != YumDB_Error_OK) { pthread_rwlock_unlock(&t->lock); *err = e; return NULL; }

    e = check_uniques(t, staged, NULL);
    if (e == YumDB_Error_UNIQUE_VIOLATION) {
        if (ii->on_conflict_ignore) {
            pthread_rwlock_unlock(&t->lock);
            *err = YumDB_Error_OK;
            return NULL;   /* caller treats NULL + OK as ignored */
        }
        if (ii->on_conflict_replace) {
            /* find existing row by first unique/pk and replace */
            for (size_t i = 0; i < t->schema->columns_count; i++) {
                YumDB_Column c = t->schema->columns[i];
                if (!(c->unique || c->primary_key)) continue;
                for (size_t k = 0; k < t->rows_count; k++) {
                    RowSlot* s = t->rows[k];
                    if (!s || !s->live) continue;
                    if (YumDB_ValueEqual(s->values[i], staged->values[i])) {
                        for (size_t c2 = 0; c2 < t->schema->columns_count; c2++) {
                            YumDB_ValueFree(&s->values[c2]);
                            s->values[c2] = YumDB_ValueClone(staged->values[c2]);
                        }
                        s->version++;
                        staged->id = s->id;
                        /* WAL */
                        if (ii->tx) {
                            tx_record_op(ii->tx, WAL_UPDATE, t, s, NULL);
                            wal_append_row_op(&ii->db->wal, WAL_UPDATE, ii->tx->tx_id,
                                              t->schema->name, staged);
                        } else {
                            wal_append_row_op(&ii->db->wal, WAL_UPDATE, 0,
                                              t->schema->name, staged);
                        }
                        pthread_rwlock_unlock(&t->lock);
                        *err = YumDB_Error_OK;
                        return staged;
                    }
                }
            }
        }
        pthread_rwlock_unlock(&t->lock);
        *err = YumDB_Error_UNIQUE_VIOLATION;
        return NULL;
    } else if (e != YumDB_Error_OK) {
        pthread_rwlock_unlock(&t->lock);
        *err = e;
        return NULL;
    }

    /* Allocate slot */
    if (t->rows_count == t->rows_cap) {
        size_t nc = t->rows_cap ? t->rows_cap * 2 : 16;
        RowSlot** nr = (RowSlot**)realloc(t->rows, nc * sizeof(*nr));
        if (!nr) { pthread_rwlock_unlock(&t->lock); *err = YumDB_Error_OUT_OF_MEMORY; return NULL; }
        t->rows = nr;
        t->rows_cap = nc;
    }
    RowSlot* s = (RowSlot*)calloc(1, sizeof(*s));
    s->live = true;
    s->id = ++t->next_id;
    s->version = 1;
    s->values = (YumDB_Value*)calloc(t->schema->columns_count, sizeof(YumDB_Value));
    for (size_t i = 0; i < t->schema->columns_count; i++)
        s->values[i] = YumDB_ValueClone(staged->values[i]);
    t->rows[t->rows_count++] = s;

    staged->id = s->id;

    if (ii->tx) {
        s->created_tx = ii->tx->tx_id;
        tx_record_op(ii->tx, WAL_INSERT, t, s, NULL);
        wal_append_row_op(&ii->db->wal, WAL_INSERT, ii->tx->tx_id,
                          t->schema->name, staged);
    } else {
        /* implicit single-op tx: append BEGIN/INSERT/COMMIT atomically */
        uint64_t tid = __sync_add_and_fetch(&ii->db->next_tx_id, 1);
        wal_append_begin(&ii->db->wal, tid);
        wal_append_row_op(&ii->db->wal, WAL_INSERT, tid, t->schema->name, staged);
        wal_append_commit(&ii->db->wal, tid);
    }

    pthread_rwlock_unlock(&t->lock);
    *err = YumDB_Error_OK;
    return staged;
}

static YumDB_Row ins_done(YumDB_Inserter self) {
    InsImpl* ii = II(self);
    if (!ii->staged) { ii->last_err = YumDB_Error_INVALID_ARGUMENT; return NULL; }
    YumDB_Error e;
    YumDB_Row inserted = do_insert_one(ii, ii->staged, &e);
    ii->last_err = e;
    if (inserted) {
        /* staged ownership transferred into slot; return a clone to caller */
        YumDB_Row clone = ii->staged->Clone(ii->staged);
        ii->staged->Destroy(ii->staged);
        ii->staged = NULL;
        return clone;
    }
    if (ii->staged) { ii->staged->Destroy(ii->staged); ii->staged = NULL; }
    return NULL;
}

static size_t ins_done_many(YumDB_Inserter self) {
    InsImpl* ii = II(self);
    size_t n = 0;
    if (ii->staged) {
        YumDB_Error e;
        if (do_insert_one(ii, ii->staged, &e)) n++;
        ii->staged->Destroy(ii->staged);
        ii->staged = NULL;
    }
    for (size_t i = 0; i < ii->batch_n; i++) {
        YumDB_Error e;
        if (do_insert_one(ii, ii->batch[i], &e)) n++;
        ii->batch[i]->Destroy(ii->batch[i]);
        ii->batch[i] = NULL;
    }
    free(ii->batch); ii->batch = NULL; ii->batch_n = 0;
    return n;
}

static YumDB_Error ins_lasterr(YumDB_Inserter self) { return II(self)->last_err; }

static void ins_destroy(YumDB_Inserter self) {
    if (!self) return;
    InsImpl* ii = II(self);
    if (ii->staged) ii->staged->Destroy(ii->staged);
    for (size_t i = 0; i < ii->batch_n; i++) if (ii->batch[i]) ii->batch[i]->Destroy(ii->batch[i]);
    free(ii->batch);
    free(ii->table_name);
    free(ii);
}

YumDB_Inserter yumi_inserter_new(YumDB_Impl* db, YumDB_Tx tx, const char* table) {
    InsImpl* i = (InsImpl*)calloc(1, sizeof(*i));
    if (!i) return NULL;
    i->db = db; i->tx = tx; i->table_name = strdup(table);
    i->vt.Set = ins_set;
    i->vt.SetRow = ins_set_row;
    i->vt.Batch = ins_batch;
    i->vt.OnConflictIgnore = ins_oci;
    i->vt.OnConflictReplace = ins_ocr;
    i->vt.Done = ins_done;
    i->vt.DoneMany = ins_done_many;
    i->vt.LastError = ins_lasterr;
    i->vt.Destroy = ins_destroy;
    return (YumDB_Inserter)i;
}
