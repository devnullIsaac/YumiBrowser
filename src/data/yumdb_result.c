/*
    YumDB — Result Set Cursor Implementation
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

#include "yumi_data.h"
#include <stdlib.h>

typedef struct ResultImpl {
    struct YumDB_Result vt;
    size_t cursor;
} ResultImpl;

#define RI(x) ((ResultImpl*)(x))

static bool      r_has_next(YumDB_Result r) { return RI(r)->cursor < r->count; }
static YumDB_Row r_next(YumDB_Result r) {
    if (RI(r)->cursor >= r->count) return NULL;
    return r->rows[RI(r)->cursor++];
}
static void      r_reset(YumDB_Result r) { RI(r)->cursor = 0; }
static YumDB_Row r_at(YumDB_Result r, size_t i) { return (i < r->count) ? r->rows[i] : NULL; }
static size_t    r_count(YumDB_Result r) { return r->count; }
static void      r_destroy(YumDB_Result r) {
    if (!r) return;
    for (size_t i = 0; i < r->count; i++)
        if (r->rows[i]) r->rows[i]->Destroy(r->rows[i]);
    free(r->rows);
    free(r);
}

YumDB_Result yumi_result_new(YumDB_Row* rows, size_t n) {
    ResultImpl* r = (ResultImpl*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->vt.rows = rows;
    r->vt.count = n;
    r->vt.HasNext = r_has_next;
    r->vt.Next = r_next;
    r->vt.Reset = r_reset;
    r->vt.At = r_at;
    r->vt.Count = r_count;
    r->vt.Destroy = r_destroy;
    return (YumDB_Result)r;
}
