/*
    YumDB — Updater Builder Implementation
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

extern YumDB_Updater_Condition yumi_updcond_new(YumDB_Updater);
extern CondList* yumi_updcond_list(YumDB_Updater_Condition);
extern YumDB_Updater yumi_updcond_parent(YumDB_Updater_Condition);
extern void yumi_updcond_free(YumDB_Updater_Condition);

typedef struct SetEntry {
    char*       col;
    YumDB_Value val;
    bool        is_increment;
    int64_t     delta;
    struct SetEntry* next;
} SetEntry;

typedef struct UpdImpl {
    struct YumDB_Updater vt;
    YumDB_Impl* db;
    YumDB_Tx    tx;
    char*       table_name;
    SetEntry*   sets;
    YumDB_Updater_Condition pending_cond;
    CondList*   where;
    YumDB_Error last_err;
} UpdImpl;

#define UI(x) ((UpdImpl*)(x))

static YumDB_Updater upd_set(YumDB_Updater self, const char* col, YumDB_Value v) {
    UpdImpl* ui = UI(self);
    SetEntry* e = (SetEntry*)calloc(1, sizeof(*e));
    e->col = strdup(col);
    e->val = YumDB_ValueClone(v);
    e->next = ui->sets; ui->sets = e;
    return self;
}

static YumDB_Updater upd_inc(YumDB_Updater self, const char* col, int64_t delta) {
    UpdImpl* ui = UI(self);
    SetEntry* e = (SetEntry*)calloc(1, sizeof(*e));
    e->col = strdup(col);
    e->is_increment = true;
    e->delta = delta;
    e->next = ui->sets; ui->sets = e;
    return self;
}

static YumDB_Updater_Condition upd_where(YumDB_Updater self) {
    UpdImpl* ui = UI(self);
    if (ui->pending_cond) yumi_updcond_free(ui->pending_cond);
    ui->pending_cond = yumi_updcond_new(self);
    ui->where = yumi_updcond_list(ui->pending_cond);
    return ui->pending_cond;
}

/* applied to each matching slot, under write lock */
static YumDB_Error apply_sets_to_slot(Table* t, RowSlot* s, SetEntry* sets) {
    for (SetEntry* e = sets; e; e = e->next) {
        size_t idx = YumDB_TypeDefColumnIndex(t->schema, e->col);
        if (idx == SIZE_MAX) return YumDB_Error_NOT_FOUND;
        YumDB_Column c = t->schema->columns[idx];
        if (e->is_increment) {
            if (!YumDB_ColumnTypeIsIntegral(c->type)) return YumDB_Error_INVALID_TYPE;
            YumDB_Value* cur = &s->values[idx];
            int64_t nv = 0;
            switch (cur->type) {
                case YumDB_ColumnType_INT8:   nv = cur->as.i8 + e->delta;  cur->as.i8  = (int8_t)nv;  break;
                case YumDB_ColumnType_UINT8:  nv = cur->as.u8 + e->delta;  cur->as.u8  = (uint8_t)nv; break;
                case YumDB_ColumnType_INT16:  nv = cur->as.i16 + e->delta; cur->as.i16 = (int16_t)nv; break;
                case YumDB_ColumnType_UINT16: nv = cur->as.u16 + e->delta; cur->as.u16 = (uint16_t)nv;break;
                case YumDB_ColumnType_INT32:  nv = cur->as.i32 + e->delta; cur->as.i32 = (int32_t)nv; break;
                case YumDB_ColumnType_UINT32: nv = cur->as.u32 + e->delta; cur->as.u32 = (uint32_t)nv;break;
                case YumDB_ColumnType_INT64:  cur->as.i64 += e->delta; break;
                case YumDB_ColumnType_UINT64: cur->as.u64 += (uint64_t)e->delta; break;
                default: return YumDB_Error_INVALID_TYPE;
            }
        } else {
            YumDB_ValueFree(&s->values[idx]);
            s->values[idx] = YumDB_ValueClone(e->val);
        }
    }
    return YumDB_Error_OK;
}

static size_t upd_done(YumDB_Updater self) {
    UpdImpl* ui = UI(self);
    Table* t = db_find_table(ui->db, ui->table_name);
    if (!t) { ui->last_err = YumDB_Error_NOT_FOUND; return 0; }

    pthread_rwlock_wrlock(&t->lock);

    uint64_t tid = 0;
    bool implicit = (ui->tx == NULL);
    if (implicit) {
        tid = __sync_add_and_fetch(&ui->db->next_tx_id, 1);
        wal_append_begin(&ui->db->wal, tid);
    } else {
        tid = ui->tx->tx_id;
    }

    size_t affected = 0;
    for (size_t i = 0; i < t->rows_count; i++) {
        RowSlot* s = t->rows[i];
        if (!s || !s->live) continue;

        YumDB_Row view = YumDB_RowNew(t->schema);
        view->id = s->id;
        for (size_t k = 0; k < t->schema->columns_count; k++) {
            YumDB_ValueFree(&view->values[k]);
            view->values[k] = YumDB_ValueClone(s->values[k]);
        }
        bool match = ui->where ? condlist_eval_row(ui->where, view) : true;
        view->Destroy(view);
        if (!match) continue;

        /* snapshot prior state for tx rollback */
        YumDB_Value* prior = NULL;
        if (ui->tx) {
            prior = (YumDB_Value*)calloc(t->schema->columns_count, sizeof(YumDB_Value));
            for (size_t k = 0; k < t->schema->columns_count; k++)
                prior[k] = YumDB_ValueClone(s->values[k]);
        }

        YumDB_Error e = apply_sets_to_slot(t, s, ui->sets);
        if (e != YumDB_Error_OK) {
            ui->last_err = e;
            if (prior) {
                for (size_t k = 0; k < t->schema->columns_count; k++) YumDB_ValueFree(&prior[k]);
                free(prior);
            }
            break;
        }
        s->version++;

        /* build updated row for WAL */
        YumDB_Row updated = YumDB_RowNew(t->schema);
        updated->id = s->id;
        for (size_t k = 0; k < t->schema->columns_count; k++) {
            YumDB_ValueFree(&updated->values[k]);
            updated->values[k] = YumDB_ValueClone(s->values[k]);
        }
        wal_append_row_op(&ui->db->wal, WAL_UPDATE, tid, t->schema->name, updated);
        updated->Destroy(updated);

        if (ui->tx) tx_record_op(ui->tx, WAL_UPDATE, t, s, prior);
        affected++;
    }

    if (implicit) wal_append_commit(&ui->db->wal, tid);

    pthread_rwlock_unlock(&t->lock);
    ui->last_err = YumDB_Error_OK;
    return affected;
}

static YumDB_Error upd_lasterr(YumDB_Updater self) { return UI(self)->last_err; }

static void upd_destroy(YumDB_Updater self) {
    if (!self) return;
    UpdImpl* ui = UI(self);
    SetEntry* e = ui->sets;
    while (e) {
        SetEntry* nx = e->next;
        free(e->col);
        YumDB_ValueFree(&e->val);
        free(e);
        e = nx;
    }
    if (ui->pending_cond) yumi_updcond_free(ui->pending_cond);
    free(ui->table_name);
    free(ui);
}

YumDB_Updater yumi_updater_new(YumDB_Impl* db, YumDB_Tx tx, const char* table) {
    UpdImpl* u = (UpdImpl*)calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->db = db; u->tx = tx; u->table_name = strdup(table);
    u->vt.Set = upd_set;
    u->vt.Increment = upd_inc;
    u->vt.Where = upd_where;
    u->vt.Done = upd_done;
    u->vt.LastError = upd_lasterr;
    u->vt.Destroy = upd_destroy;
    return (YumDB_Updater)u;
}

/* wire the Updater_Condition.Done() */
YumDB_Updater yumi_updcond_done_impl(YumDB_Updater_Condition c) {
    return yumi_updcond_parent(c);
}
