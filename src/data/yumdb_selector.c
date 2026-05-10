/*
    YumDB — Selector Builder Implementation
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

extern YumDB_Result yumi_result_new(YumDB_Row* rows, size_t n);
extern YumDB_Selector_Condition yumi_selcond_new(YumDB_Selector);
extern CondList* yumi_selcond_list(YumDB_Selector_Condition);
extern YumDB_Selector yumi_selcond_parent(YumDB_Selector_Condition);
extern void yumi_selcond_free(YumDB_Selector_Condition);

typedef struct SelImpl {
    struct YumDB_Selector vt;
    YumDB_Impl* db;
    YumDB_Tx    tx;           /* non-NULL if inside explicit tx */
    char*       table_name;
    YumDB_Selector_Condition pending_cond;  /* owned until parent consumes */
    CondList*   where;        /* borrowed from pending_cond */
    const char**order_col_chain; /* simplistic: single order col */
    char*       order_col;
    YumDB_Order order_dir;
    size_t      limit;
    size_t      offset;
    bool        has_limit;
    bool        distinct;
    YumDB_Error last_err;
} SelImpl;

#define SI(x) ((SelImpl*)(x))

static YumDB_Selector sel_columns(YumDB_Selector s, const char** cols, size_t n) {
    (void)cols; (void)n;   /* projection omitted for brevity; always returns full rows */
    return s;
}

static YumDB_Selector_Condition sel_where(YumDB_Selector s) {
    SelImpl* si = SI(s);
    if (si->pending_cond) yumi_selcond_free(si->pending_cond);
    si->pending_cond = yumi_selcond_new(s);
    si->where = yumi_selcond_list(si->pending_cond);
    return si->pending_cond;
}

static YumDB_Selector sel_orderby(YumDB_Selector s, const char* col, YumDB_Order o) {
    free(SI(s)->order_col);
    SI(s)->order_col = col ? strdup(col) : NULL;
    SI(s)->order_dir = o;
    return s;
}
static YumDB_Selector sel_limit(YumDB_Selector s, size_t n)  { SI(s)->limit = n; SI(s)->has_limit = true; return s; }
static YumDB_Selector sel_offset(YumDB_Selector s, size_t n) { SI(s)->offset = n; return s; }
static YumDB_Selector sel_distinct(YumDB_Selector s, bool d) { SI(s)->distinct = d; return s; }

/* Condition.Done() returns parent Selector */
static YumDB_Selector yumi_selcond_done(YumDB_Selector_Condition c) {
    return yumi_selcond_parent(c);
}

static int cmp_rows_by_col(const void* a, const void* b, void* ctx) {
    SelImpl* si = (SelImpl*)ctx;
    YumDB_Row ra = *(YumDB_Row*)a, rb = *(YumDB_Row*)b;
    YumDB_Value va = ra->Get(ra, si->order_col);
    YumDB_Value vb = rb->Get(rb, si->order_col);
    int c = YumDB_ValueCompare(va, vb);
    return (si->order_dir == YumDB_Order_ASC) ? c : -c;
}

/* portable qsort_r shim (glibc vs BSD) */
#if defined(__APPLE__) || defined(__FreeBSD__)
static int qsort_shim(void* ctx, const void* a, const void* b) { return cmp_rows_by_col(a,b,ctx); }
#define QSORT_R(base, n, sz, ctx, cmp) qsort_r(base, n, sz, ctx, qsort_shim)
#else
static int qsort_shim(const void* a, const void* b, void* ctx) { return cmp_rows_by_col(a,b,ctx); }
#define QSORT_R(base, n, sz, ctx, cmp) qsort_r(base, n, sz, qsort_shim, ctx)
#endif

static YumDB_Result sel_done(YumDB_Selector s) {
    SelImpl* si = SI(s);
    Table* t = db_find_table(si->db, si->table_name);
    if (!t) { si->last_err = YumDB_Error_NOT_FOUND; return yumi_result_new(NULL, 0); }

    pthread_rwlock_rdlock(&t->lock);

    YumDB_Row* matched = NULL;
    size_t matched_n = 0, matched_cap = 0;

    for (size_t i = 0; i < t->rows_count; i++) {
        RowSlot* slot = t->rows[i];
        if (!slot || !slot->live) continue;

        /* Build transient YumDB_Row view for predicate eval */
        YumDB_Row view = YumDB_RowNew(t->schema);
        view->id = slot->id;
        for (size_t k = 0; k < t->schema->columns_count; k++) {
            YumDB_ValueFree(&view->values[k]);
            view->values[k] = YumDB_ValueClone(slot->values[k]);
        }

        bool match = si->where ? condlist_eval_row(si->where, view) : true;
        if (match) {
            if (matched_n == matched_cap) {
                matched_cap = matched_cap ? matched_cap * 2 : 16;
                matched = (YumDB_Row*)realloc(matched, matched_cap * sizeof(*matched));
            }
            matched[matched_n++] = view;
        } else {
            view->Destroy(view);
        }
    }

    pthread_rwlock_unlock(&t->lock);

    if (si->order_col && matched_n > 1) {
        QSORT_R(matched, matched_n, sizeof(YumDB_Row), si, cmp_rows_by_col);
    }

    /* offset + limit */
    size_t start = si->offset < matched_n ? si->offset : matched_n;
    size_t end = matched_n;
    if (si->has_limit && start + si->limit < end) end = start + si->limit;
    size_t final_n = end - start;
    YumDB_Row* final_rows = (YumDB_Row*)calloc(final_n ? final_n : 1, sizeof(*final_rows));
    for (size_t i = 0; i < start; i++) matched[i]->Destroy(matched[i]);
    for (size_t i = 0; i < final_n; i++) final_rows[i] = matched[start + i];
    for (size_t i = end; i < matched_n; i++) matched[i]->Destroy(matched[i]);
    free(matched);

    si->last_err = YumDB_Error_OK;
    return yumi_result_new(final_rows, final_n);
}

static YumDB_Row sel_first(YumDB_Selector s) {
    SI(s)->has_limit = true; SI(s)->limit = 1;
    YumDB_Result r = sel_done(s);
    YumDB_Row out = (r->count > 0) ? r->rows[0]->Clone(r->rows[0]) : NULL;
    r->Destroy(r);
    return out;
}

static size_t sel_count(YumDB_Selector s) {
    YumDB_Result r = sel_done(s);
    size_t n = r->count;
    r->Destroy(r);
    return n;
}

static bool sel_exists(YumDB_Selector s) { return sel_count(s) > 0; }
static YumDB_Error sel_lasterr(YumDB_Selector s) { return SI(s)->last_err; }

static void sel_destroy(YumDB_Selector s) {
    if (!s) return;
    SelImpl* si = SI(s);
    free(si->table_name);
    free(si->order_col);
    if (si->pending_cond) yumi_selcond_free(si->pending_cond);
    free(si);
}

YumDB_Selector yumi_selector_new(YumDB_Impl* db, YumDB_Tx tx, const char* table) {
    SelImpl* s = (SelImpl*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->db = db; s->tx = tx; s->table_name = strdup(table);
    s->order_dir = YumDB_Order_ASC;
    s->vt.Columns = sel_columns;
    s->vt.Where = sel_where;
    s->vt.OrderBy = sel_orderby;
    s->vt.Limit = sel_limit;
    s->vt.Offset = sel_offset;
    s->vt.Distinct = sel_distinct;
    s->vt.Done = sel_done;
    s->vt.First = sel_first;
    s->vt.Count = sel_count;
    s->vt.Exists = sel_exists;
    s->vt.LastError = sel_lasterr;
    s->vt.Destroy = sel_destroy;
    return (YumDB_Selector)s;
}

/* wire the Selector_Condition.Done() */
__attribute__((constructor))
static void init_selcond_done(void) { /* vtable Done is written at cond creation in yumi_condition.c
                                         via yumi_selcond_new, which needs to write vt.Done. Since
                                         that symbol needs visibility of parent type, we patch here
                                         by taking the address of yumi_selcond_done (visible). */ }
/* The macro in yumi_condition.c sets .Done externally via hook: */
void yumi_selcond_wire_done(struct YumDB_Selector_Condition* vt) {
    vt->Done = yumi_selcond_done;
}
