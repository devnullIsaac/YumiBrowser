/*
    YumDB — Column Definition Helpers
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
#include <string.h>
#include <stdlib.h>

static char* dup_cstr(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* r = (char*)malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

YumDB_Column YumDB_ColumnCreate(const char* name, YumDB_ColumnType type) {
    YumDB_Column c = (YumDB_Column)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->name = dup_cstr(name);
    c->type = type;
    c->nullable = true;
    c->default_value = YumDB_Null();
    return c;
}

void YumDB_ColumnDestroy(YumDB_Column c) {
    if (!c) return;
    free(c->name);
    YumDB_ValueFree(&c->default_value);
    free(c);
}

/* ---- builder ---- */

typedef struct {
    struct YumDB_ColumnBuilder vt;
    YumDB_Column col;
} CBImpl;

#define CB(this) ((CBImpl*)(this))

static YumDB_ColumnBuilder cb_len(YumDB_ColumnBuilder this, size_t v)    { CB(this)->col->len = v; return this; }
static YumDB_ColumnBuilder cb_min(YumDB_ColumnBuilder this, size_t v)    { CB(this)->col->min = v; return this; }
static YumDB_ColumnBuilder cb_max(YumDB_ColumnBuilder this, size_t v)    { CB(this)->col->max = v; return this; }
static YumDB_ColumnBuilder cb_unique(YumDB_ColumnBuilder this, bool v)   { CB(this)->col->unique = v; return this; }
static YumDB_ColumnBuilder cb_nullable(YumDB_ColumnBuilder this, bool v) { CB(this)->col->nullable = v; return this; }
static YumDB_ColumnBuilder cb_pk(YumDB_ColumnBuilder this, bool v)       { CB(this)->col->primary_key = v; if (v) CB(this)->col->nullable = false; return this; }
static YumDB_ColumnBuilder cb_ai(YumDB_ColumnBuilder this, bool v)       { CB(this)->col->auto_increment = v; return this; }
static YumDB_ColumnBuilder cb_idx(YumDB_ColumnBuilder this, bool v)      { CB(this)->col->indexed = v; return this; }
static YumDB_ColumnBuilder cb_default(YumDB_ColumnBuilder this, YumDB_Value v) {
    YumDB_ValueFree(&CB(this)->col->default_value);
    CB(this)->col->default_value = YumDB_ValueClone(v);
    return this;
}

static YumDB_Column cb_build(YumDB_ColumnBuilder this) {
    YumDB_Column c = CB(this)->col;
    CB(this)->col = NULL;
    free(this);
    return c;
}

static void cb_destroy(YumDB_ColumnBuilder this) {
    if (!this) return;
    if (CB(this)->col) YumDB_ColumnDestroy(CB(this)->col);
    free(this);
}

YumDB_ColumnBuilder YumDB_ColumnBuilderNew(const char* name, YumDB_ColumnType type) {
    CBImpl* b = (CBImpl*)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->col = YumDB_ColumnCreate(name, type);
    if (!b->col) { free(b); return NULL; }
    b->vt.Len = cb_len;
    b->vt.Min = cb_min;
    b->vt.Max = cb_max;
    b->vt.Unique = cb_unique;
    b->vt.Nullable = cb_nullable;
    b->vt.PrimaryKey = cb_pk;
    b->vt.AutoIncrement = cb_ai;
    b->vt.Indexed = cb_idx;
    b->vt.Default = cb_default;
    b->vt.Build = cb_build;
    b->vt.Destroy = cb_destroy;
    return (YumDB_ColumnBuilder)b;
}
