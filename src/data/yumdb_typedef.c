/*
    YumDB — Schema and Column Type Definitions
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

void YumDB_TypeDefDestroy(YumDB_TypeDef t) {
    if (!t) return;
    free(t->name);
    for (size_t i = 0; i < t->columns_count; i++)
        YumDB_ColumnDestroy(t->columns[i]);
    free(t->columns);
    free(t);
}

YumDB_Column YumDB_TypeDefFindColumn(YumDB_TypeDef t, const char* name) {
    if (!t || !name) return NULL;
    for (size_t i = 0; i < t->columns_count; i++)
        if (strcmp(t->columns[i]->name, name) == 0)
            return t->columns[i];
    return NULL;
}

size_t YumDB_TypeDefColumnIndex(YumDB_TypeDef t, const char* name) {
    if (!t || !name) return SIZE_MAX;
    for (size_t i = 0; i < t->columns_count; i++)
        if (strcmp(t->columns[i]->name, name) == 0)
            return i;
    return SIZE_MAX;
}

/* ---- builder ---- */

typedef struct {
    struct YumDB_TypeDefBuilder vt;
    YumDB_TypeDef td;
    size_t cap;
} TDBImpl;

#define TDB(this) ((TDBImpl*)(this))

static YumDB_TypeDefBuilder tdb_add(YumDB_TypeDefBuilder this, YumDB_Column c) {
    TDBImpl* b = TDB(this);
    if (b->td->columns_count == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4;
        YumDB_Column* nn = (YumDB_Column*)realloc(b->td->columns, nc * sizeof(*nn));
        if (!nn) return this;
        b->td->columns = nn;
        b->cap = nc;
    }
    b->td->columns[b->td->columns_count++] = c;
    return this;
}

static YumDB_TypeDefBuilder tdb_pk(YumDB_TypeDefBuilder this, const char* col) {
    YumDB_Column c = YumDB_TypeDefFindColumn(TDB(this)->td, col);
    if (c) { c->primary_key = true; c->nullable = false; c->unique = true; }
    return this;
}

static YumDB_TypeDefBuilder tdb_ck(YumDB_TypeDefBuilder this, const char** cols, size_t n) {
    for (size_t i = 0; i < n; i++) {
        YumDB_Column c = YumDB_TypeDefFindColumn(TDB(this)->td, cols[i]);
        if (c) { c->primary_key = true; c->nullable = false; }
    }
    return this;
}

static YumDB_TypeDefBuilder tdb_index(YumDB_TypeDefBuilder this, const char* col) {
    YumDB_Column c = YumDB_TypeDefFindColumn(TDB(this)->td, col);
    if (c) c->indexed = true;
    return this;
}

static YumDB_TypeDef tdb_build(YumDB_TypeDefBuilder this) {
    YumDB_TypeDef t = TDB(this)->td;
    TDB(this)->td = NULL;
    free(this);
    return t;
}

static void tdb_destroy(YumDB_TypeDefBuilder this) {
    if (!this) return;
    if (TDB(this)->td) YumDB_TypeDefDestroy(TDB(this)->td);
    free(this);
}

YumDB_TypeDefBuilder YumDB_TypeDefBuilderNew(const char* name) {
    TDBImpl* b = (TDBImpl*)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->td = (YumDB_TypeDef)calloc(1, sizeof(*b->td));
    if (!b->td) { free(b); return NULL; }
    b->td->name = dup_cstr(name);
    b->vt.AddColumn = tdb_add;
    b->vt.PrimaryKey = tdb_pk;
    b->vt.CompositeKey = tdb_ck;
    b->vt.Index = tdb_index;
    b->vt.Build = tdb_build;
    b->vt.Destroy = tdb_destroy;
    return (YumDB_TypeDefBuilder)b;
}
