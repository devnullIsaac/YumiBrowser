/*
    YumDB — Condition AST Helpers
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

static char* dup_cstr(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* r = (char*)malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

void condlist_init(CondList* cl) {
    memset(cl, 0, sizeof(*cl));
    cl->pending_conn = CONN_AND;
}

static void free_node(CondNode* n) {
    free(n->column);
    YumDB_ValueFree(&n->v1);
    YumDB_ValueFree(&n->v2);
    if (n->vlist) {
        for (size_t i = 0; i < n->vlist_n; i++) YumDB_ValueFree(&n->vlist[i]);
        free(n->vlist);
    }
    free(n);
}

void condlist_free(CondList* cl) {
    CondNode* n = cl->head;
    while (n) { CondNode* nx = n->next; free_node(n); n = nx; }
    cl->head = cl->tail = NULL;
}

void condlist_append(CondList* cl, CondNode* n) {
    n->connector = cl->pending_conn;
    n->negated = cl->pending_not;
    cl->pending_conn = CONN_AND;
    cl->pending_not = false;
    if (!cl->head) cl->head = cl->tail = n;
    else { cl->tail->next = n; cl->tail = n; }
}

static bool eval_clause(const CondNode* n, YumDB_Row row) {
    if (n->kind == COND_GROUP_OPEN || n->kind == COND_GROUP_CLOSE) return true;
    YumDB_Value cell = row->Get(row, n->column);
    int cmp;
    bool r = false;
    switch (n->kind) {
        case COND_EQ:  r = YumDB_ValueEqual(cell, n->v1); break;
        case COND_NEQ: r = !YumDB_ValueEqual(cell, n->v1); break;
        case COND_LT:  r = YumDB_ValueCompare(cell, n->v1) <  0; break;
        case COND_LTE: r = YumDB_ValueCompare(cell, n->v1) <= 0; break;
        case COND_GT:  r = YumDB_ValueCompare(cell, n->v1) >  0; break;
        case COND_GTE: r = YumDB_ValueCompare(cell, n->v1) >= 0; break;
        case COND_IN:
            for (size_t i = 0; i < n->vlist_n; i++)
                if (YumDB_ValueEqual(cell, n->vlist[i])) { r = true; break; }
            break;
        case COND_NOT_IN:
            r = true;
            for (size_t i = 0; i < n->vlist_n; i++)
                if (YumDB_ValueEqual(cell, n->vlist[i])) { r = false; break; }
            break;
        case COND_BETWEEN:
            cmp = YumDB_ValueCompare(cell, n->v1);
            r = (cmp >= 0) && (YumDB_ValueCompare(cell, n->v2) <= 0);
            break;
        case COND_IS_NULL:     r = cell.is_null; break;
        case COND_IS_NOT_NULL: r = !cell.is_null; break;
        default: r = true;
    }
    if (n->negated) r = !r;
    return r;
}

/* Simple left-to-right short-circuit evaluation with group support.
 * Production-grade precedence handling (AND binds tighter than OR) would
 * require a proper expression tree; here we evaluate left-to-right within
 * each group and treat groups as atoms. */
bool condlist_eval_row(const CondList* cl, YumDB_Row row) {
    if (!cl->head) return true;

    /* recursive descent over a linear list w/ GROUP markers */
    const CondNode* n = cl->head;
    bool acc = true;
    bool first = true;
    CondConnector next_conn = CONN_AND;

    while (n) {
        if (n->kind == COND_GROUP_OPEN) {
            /* find matching close */
            int depth = 1;
            const CondNode* start = n->next;
            const CondNode* end = start;
            while (end && depth > 0) {
                if (end->kind == COND_GROUP_OPEN) depth++;
                else if (end->kind == COND_GROUP_CLOSE) { depth--; if (depth==0) break; }
                end = end->next;
            }
            /* build temp sublist by evaluating inline */
            CondList sub = {0};
            sub.pending_conn = CONN_AND;
            /* we don't modify original - just evaluate via a walker */
            bool sub_acc = true;
            bool sub_first = true;
            CondConnector sub_next = CONN_AND;
            for (const CondNode* m = start; m != end; m = m->next) {
                if (m->kind == COND_GROUP_OPEN || m->kind == COND_GROUP_CLOSE) continue;
                bool v = eval_clause(m, row);
                if (sub_first) { sub_acc = v; sub_first = false; }
                else sub_acc = (sub_next == CONN_AND) ? (sub_acc && v) : (sub_acc || v);
                sub_next = m->connector; /* connector of the next clause lives on it */
            }
            if (first) { acc = sub_acc; first = false; }
            else acc = (next_conn == CONN_AND) ? (acc && sub_acc) : (acc || sub_acc);
            n = end ? end->next : NULL;
            next_conn = n ? n->connector : CONN_AND;
            continue;
        }
        if (n->kind == COND_GROUP_CLOSE) { n = n->next; continue; }
        bool v = eval_clause(n, row);
        if (first) { acc = v; first = false; }
        else acc = (next_conn == CONN_AND) ? (acc && v) : (acc || v);
        next_conn = n->next ? n->next->connector : CONN_AND;
        n = n->next;
    }
    return acc;
}

/* ---- shared clause builders, parameterized on return type via macros ---- */

static CondNode* mk_binary(CondKind k, const char* col, YumDB_Value v) {
    CondNode* n = (CondNode*)calloc(1, sizeof(*n));
    n->kind = k; n->column = dup_cstr(col); n->v1 = YumDB_ValueClone(v);
    return n;
}
static CondNode* mk_list(CondKind k, const char* col, const YumDB_Value* vs, size_t m) {
    CondNode* n = (CondNode*)calloc(1, sizeof(*n));
    n->kind = k; n->column = dup_cstr(col);
    n->vlist = (YumDB_Value*)calloc(m, sizeof(YumDB_Value));
    n->vlist_n = m;
    for (size_t i = 0; i < m; i++) n->vlist[i] = YumDB_ValueClone(vs[i]);
    return n;
}
static CondNode* mk_between(const char* col, YumDB_Value lo, YumDB_Value hi) {
    CondNode* n = (CondNode*)calloc(1, sizeof(*n));
    n->kind = COND_BETWEEN; n->column = dup_cstr(col);
    n->v1 = YumDB_ValueClone(lo); n->v2 = YumDB_ValueClone(hi);
    return n;
}
static CondNode* mk_nullary(CondKind k, const char* col) {
    CondNode* n = (CondNode*)calloc(1, sizeof(*n));
    n->kind = k; n->column = dup_cstr(col);
    return n;
}

/* =========================================================================
 * The three sub-builders share one implementation struct; the vtable differs
 * only in its return types. We use X-macros to generate them.
 * ========================================================================= */

#define COND_IMPL(PREFIX, PARENT_T)                                                \
typedef struct PREFIX##CImpl {                                                     \
    struct PREFIX##_Condition vt;                                                  \
    CondList  cl;                                                                  \
    PARENT_T  parent;                                                              \
} PREFIX##CImpl;                                                                   \
static PREFIX##_Condition PREFIX##_eq(PREFIX##_Condition t, const char* c, YumDB_Value v)   { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_binary(COND_EQ, c, v));  return t; } \
static PREFIX##_Condition PREFIX##_neq(PREFIX##_Condition t, const char* c, YumDB_Value v)  { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_binary(COND_NEQ, c, v)); return t; } \
static PREFIX##_Condition PREFIX##_lt(PREFIX##_Condition t, const char* c, YumDB_Value v)   { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_binary(COND_LT, c, v));  return t; } \
static PREFIX##_Condition PREFIX##_lte(PREFIX##_Condition t, const char* c, YumDB_Value v)  { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_binary(COND_LTE, c, v)); return t; } \
static PREFIX##_Condition PREFIX##_gt(PREFIX##_Condition t, const char* c, YumDB_Value v)   { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_binary(COND_GT, c, v));  return t; } \
static PREFIX##_Condition PREFIX##_gte(PREFIX##_Condition t, const char* c, YumDB_Value v)  { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_binary(COND_GTE, c, v)); return t; } \
static PREFIX##_Condition PREFIX##_in(PREFIX##_Condition t, const char* c, const YumDB_Value* vs, size_t n)    { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_list(COND_IN, c, vs, n));     return t; } \
static PREFIX##_Condition PREFIX##_nin(PREFIX##_Condition t, const char* c, const YumDB_Value* vs, size_t n)   { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_list(COND_NOT_IN, c, vs, n)); return t; } \
static PREFIX##_Condition PREFIX##_bt(PREFIX##_Condition t, const char* c, YumDB_Value l, YumDB_Value h)       { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_between(c, l, h)); return t; } \
static PREFIX##_Condition PREFIX##_isnull(PREFIX##_Condition t, const char* c)     { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_nullary(COND_IS_NULL, c));     return t; } \
static PREFIX##_Condition PREFIX##_isnotnull(PREFIX##_Condition t, const char* c)  { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_nullary(COND_IS_NOT_NULL, c)); return t; } \
static PREFIX##_Condition PREFIX##_and(PREFIX##_Condition t) { ((PREFIX##CImpl*)t)->cl.pending_conn = CONN_AND; return t; } \
static PREFIX##_Condition PREFIX##_or(PREFIX##_Condition t)  { ((PREFIX##CImpl*)t)->cl.pending_conn = CONN_OR;  return t; } \
static PREFIX##_Condition PREFIX##_not(PREFIX##_Condition t) { ((PREFIX##CImpl*)t)->cl.pending_not = true; return t; } \
static PREFIX##_Condition PREFIX##_group(PREFIX##_Condition t)    { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_nullary(COND_GROUP_OPEN, NULL));  return t; } \
static PREFIX##_Condition PREFIX##_endgroup(PREFIX##_Condition t) { condlist_append(&((PREFIX##CImpl*)t)->cl, mk_nullary(COND_GROUP_CLOSE, NULL)); return t; }

COND_IMPL(YumDB_Selector, YumDB_Selector)
COND_IMPL(YumDB_Updater,  YumDB_Updater)
COND_IMPL(YumDB_Deleter,  YumDB_Deleter)

/* Done() functions live with their parents to transfer CondList ownership.
 * They're declared here and defined in yumi_selector.c / yumi_updater.c /
 * yumi_deleter.c where the parent impl structs are visible. */

#define WIRE_COND_VTABLE(PREFIX, IMPL)                               \
    do {                                                              \
        IMPL->vt.Eq = PREFIX##_eq;                                    \
        IMPL->vt.Neq = PREFIX##_neq;                                  \
        IMPL->vt.Lt = PREFIX##_lt;                                    \
        IMPL->vt.Lte = PREFIX##_lte;                                  \
        IMPL->vt.Gt = PREFIX##_gt;                                    \
        IMPL->vt.Gte = PREFIX##_gte;                                  \
        IMPL->vt.In = PREFIX##_in;                                    \
        IMPL->vt.NotIn = PREFIX##_nin;                                \
        IMPL->vt.Between = PREFIX##_bt;                               \
        IMPL->vt.IsNull = PREFIX##_isnull;                            \
        IMPL->vt.IsNotNull = PREFIX##_isnotnull;                      \
        IMPL->vt.And = PREFIX##_and;                                  \
        IMPL->vt.Or = PREFIX##_or;                                    \
        IMPL->vt.Not = PREFIX##_not;                                  \
        IMPL->vt.Group = PREFIX##_group;                              \
        IMPL->vt.EndGroup = PREFIX##_endgroup;                        \
        condlist_init(&IMPL->cl);                                     \
    } while (0)

/* Exposed to sibling .c files via yumi_internal.h extension: */
YumDB_Selector_Condition yumi_selcond_new(YumDB_Selector parent);
YumDB_Updater_Condition  yumi_updcond_new(YumDB_Updater parent);
YumDB_Deleter_Condition  yumi_delcond_new(YumDB_Deleter parent);
CondList*                yumi_selcond_list(YumDB_Selector_Condition c);
CondList*                yumi_updcond_list(YumDB_Updater_Condition c);
CondList*                yumi_delcond_list(YumDB_Deleter_Condition c);
YumDB_Selector           yumi_selcond_parent(YumDB_Selector_Condition c);
YumDB_Updater            yumi_updcond_parent(YumDB_Updater_Condition c);
YumDB_Deleter            yumi_delcond_parent(YumDB_Deleter_Condition c);
void                     yumi_selcond_free(YumDB_Selector_Condition c);
void                     yumi_updcond_free(YumDB_Updater_Condition c);
void                     yumi_delcond_free(YumDB_Deleter_Condition c);

YumDB_Selector_Condition yumi_selcond_new(YumDB_Selector parent) {
    YumDB_SelectorCImpl* c = (YumDB_SelectorCImpl*)calloc(1, sizeof(*c));
    WIRE_COND_VTABLE(YumDB_Selector, c);
    c->parent = parent;
    return (YumDB_Selector_Condition)c;
}
YumDB_Updater_Condition yumi_updcond_new(YumDB_Updater parent) {
    YumDB_UpdaterCImpl* c = (YumDB_UpdaterCImpl*)calloc(1, sizeof(*c));
    WIRE_COND_VTABLE(YumDB_Updater, c);
    c->parent = parent;
    return (YumDB_Updater_Condition)c;
}
YumDB_Deleter_Condition yumi_delcond_new(YumDB_Deleter parent) {
    YumDB_DeleterCImpl* c = (YumDB_DeleterCImpl*)calloc(1, sizeof(*c));
    WIRE_COND_VTABLE(YumDB_Deleter, c);
    c->parent = parent;
    return (YumDB_Deleter_Condition)c;
}

CondList* yumi_selcond_list(YumDB_Selector_Condition c) { return &((YumDB_SelectorCImpl*)c)->cl; }
CondList* yumi_updcond_list(YumDB_Updater_Condition c)  { return &((YumDB_UpdaterCImpl*)c)->cl; }
CondList* yumi_delcond_list(YumDB_Deleter_Condition c)  { return &((YumDB_DeleterCImpl*)c)->cl; }

YumDB_Selector yumi_selcond_parent(YumDB_Selector_Condition c) { return ((YumDB_SelectorCImpl*)c)->parent; }
YumDB_Updater  yumi_updcond_parent(YumDB_Updater_Condition c)  { return ((YumDB_UpdaterCImpl*)c)->parent; }
YumDB_Deleter  yumi_delcond_parent(YumDB_Deleter_Condition c)  { return ((YumDB_DeleterCImpl*)c)->parent; }

void yumi_selcond_free(YumDB_Selector_Condition c) { if (!c) return; condlist_free(&((YumDB_SelectorCImpl*)c)->cl); free(c); }
void yumi_updcond_free(YumDB_Updater_Condition c)  { if (!c) return; condlist_free(&((YumDB_UpdaterCImpl*)c)->cl); free(c); }
void yumi_delcond_free(YumDB_Deleter_Condition c)  { if (!c) return; condlist_free(&((YumDB_DeleterCImpl*)c)->cl); free(c); }
