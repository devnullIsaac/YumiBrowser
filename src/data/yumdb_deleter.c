/*
    YumDB — Deleter Builder Implementation
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

extern YumDB_Deleter_Condition yumi_delcond_new(YumDB_Deleter);
extern CondList* yumi_delcond_list(YumDB_Deleter_Condition);
extern YumDB_Deleter yumi_delcond_parent(YumDB_Deleter_Condition);
extern void yumi_delcond_free(YumDB_Deleter_Condition);

typedef struct DelImpl {
    struct YumDB_Deleter vt;
    YumDB_Impl* db;
    YumDB_Tx    tx;
    char*       table_name;
    YumDB_Deleter_Condition pending_cond;
    CondList*   where;
    size_t      limit;
    bool        has_limit;
    YumDB_Error last_err;
} DelImpl;

#define DI(x) ((DelImpl*)(x))

static YumDB_Deleter_Condition del_where(YumDB_Deleter self) {
    DelImpl* di = DI(self);
    if (di->pending_cond) yumi_delcond_free(di->pending_cond);
    di->pending_cond = yumi_delcond_new(self);
    di->where = yumi_delcond_list(di->pending_cond);
    return di->pending_cond;
}

static YumDB_Deleter del_limit(YumDB_Deleter self, size_t n) {
    DI(self)->has_limit = true; DI(self)->limit = n; return self;
}

static size_t del_done(YumDB_Deleter self) {
    DelImpl* di = DI(self);
    Table* t = db_find_table(di->db, di->table_name);
    if (!t) { di->last_err = YumDB_Error_NOT_FOUND; return 0; }

    pthread_rwlock_wrlock(&t->lock);

    uint64_t tid = 0;
    bool implicit = (di->tx == NULL);
    if (implicit) {
        tid = __sync_add_and_fetch(&di->db->next_tx_id, 1);
        wal_append_begin(&di->db->wal, tid);
    } else {
        tid = di->tx->tx_id;
    }

    size_t affected = 0;
    for (size_t i = 0; i < t->rows_count; i++) {
        if (di->has_limit && affected >= di->limit) break;
        RowSlot* s = t->rows[i];
        if (!s || !s->live) continue;

        YumDB_Row view = YumDB_RowNew(t->schema);
        view->id = s->id;
        for (size_t k = 0; k < t->schema->columns_count; k++) {
            YumDB_ValueFree(&view->values[k]);
            view->values[k] = YumDB_ValueClone(s->values[k]);
        }
        bool match = di->where ? condlist_eval_row(di->where, view) : true;

        if (match) {
            /* snapshot prior state for tx rollback */
            YumDB_Value* prior = NULL;
            if (di->tx) {
                prior = (YumDB_Value*)calloc(t->schema->columns_count, sizeof(YumDB_Value));
                for (size_t k = 0; k < t->schema->columns_count; k++)
                    prior[k] = YumDB_ValueClone(s->values[k]);
            }
            s->live = false;
            s->version++;
            s->deleted_tx = tid;

            wal_append_row_op(&di->db->wal, WAL_DELETE, tid, t->schema->name, view);
            if (di->tx) tx_record_op(di->tx, WAL_DELETE, t, s, prior);
            affected++;
        }
        view->Destroy(view);
    }

    if (implicit) wal_append_commit(&di->db->wal, tid);

    pthread_rwlock_unlock(&t->lock);
    di->last_err = YumDB_Error_OK;
    return affected;
}

static YumDB_Error del_lasterr(YumDB_Deleter self) { return DI(self)->last_err; }

static void del_destroy(YumDB_Deleter self) {
    if (!self) return;
    DelImpl* di = DI(self);
    if (di->pending_cond) yumi_delcond_free(di->pending_cond);
    free(di->table_name);
    free(di);
}

YumDB_Deleter yumi_deleter_new(YumDB_Impl* db, YumDB_Tx tx, const char* table) {
    DelImpl* d = (DelImpl*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->db = db; d->tx = tx; d->table_name = strdup(table);
    d->vt.Where = del_where;
    d->vt.Limit = del_limit;
    d->vt.Done = del_done;
    d->vt.LastError = del_lasterr;
    d->vt.Destroy = del_destroy;
    return (YumDB_Deleter)d;
}

YumDB_Deleter yumi_delcond_done_impl(YumDB_Deleter_Condition c) {
    return yumi_delcond_parent(c);
}
