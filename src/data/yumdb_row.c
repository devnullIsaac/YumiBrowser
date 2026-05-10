/*
    YumDB — Row Object Implementation
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

#define ROW_IMPL(this) ((RowImpl*)(this))

typedef struct RowImpl {
    struct YumDB_Row vt;
} RowImpl;

static YumDB_Value row_get(YumDB_Row this, const char* column) {
    size_t i = YumDB_TypeDefColumnIndex(this->type, column);
    if (i == SIZE_MAX) return YumDB_Null();
    return this->values[i];
}

static YumDB_Value row_get_at(YumDB_Row this, size_t idx) {
    if (idx >= this->values_count) return YumDB_Null();
    return this->values[idx];
}

static YumDB_Error row_set(YumDB_Row this, const char* column, YumDB_Value v) {
    size_t i = YumDB_TypeDefColumnIndex(this->type, column);
    if (i == SIZE_MAX) return YumDB_Error_NOT_FOUND;
    YumDB_ValueFree(&this->values[i]);
    this->values[i] = YumDB_ValueClone(v);
    return YumDB_Error_OK;
}

static YumDB_Error row_set_at(YumDB_Row this, size_t i, YumDB_Value v) {
    if (i >= this->values_count) return YumDB_Error_INVALID_ARGUMENT;
    YumDB_ValueFree(&this->values[i]);
    this->values[i] = YumDB_ValueClone(v);
    return YumDB_Error_OK;
}

static bool row_has(YumDB_Row this, const char* column) {
    return YumDB_TypeDefColumnIndex(this->type, column) != SIZE_MAX;
}

static bool row_is_null(YumDB_Row this, const char* column) {
    size_t i = YumDB_TypeDefColumnIndex(this->type, column);
    if (i == SIZE_MAX) return true;
    return this->values[i].is_null;
}

static void row_destroy(YumDB_Row this) {
    if (!this) return;
    for (size_t i = 0; i < this->values_count; i++)
        YumDB_ValueFree(&this->values[i]);
    free(this->values);
    free(this);
}

static YumDB_Row row_clone(YumDB_Row this);

static void install_vtable(YumDB_Row r) {
    r->Get = row_get;
    r->GetAt = row_get_at;
    r->Set = row_set;
    r->SetAt = row_set_at;
    r->HasColumn = row_has;
    r->IsNull = row_is_null;
    r->Clone = row_clone;
    r->Destroy = row_destroy;
}

static YumDB_Row row_clone(YumDB_Row this) {
    YumDB_Row r = (YumDB_Row)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->id = this->id;
    r->type = this->type;
    r->values_count = this->values_count;
    r->values = (YumDB_Value*)calloc(r->values_count, sizeof(YumDB_Value));
    for (size_t i = 0; i < r->values_count; i++)
        r->values[i] = YumDB_ValueClone(this->values[i]);
    install_vtable(r);
    return r;
}

YumDB_Row YumDB_RowNew(YumDB_TypeDef type) {
    if (!type) return NULL;
    YumDB_Row r = (YumDB_Row)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->type = type;
    r->values_count = type->columns_count;
    r->values = (YumDB_Value*)calloc(r->values_count, sizeof(YumDB_Value));
    for (size_t i = 0; i < r->values_count; i++)
        r->values[i] = YumDB_ValueClone(type->columns[i]->default_value);
    install_vtable(r);
    return r;
}
