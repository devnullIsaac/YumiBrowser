/*
    YumDB — Value Type and Error String Helpers
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

#include "yumdb_data.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

const char* YumDB_ErrorString(YumDB_Error err) {
    switch (err) {
        case YumDB_Error_OK: return "OK";
        case YumDB_Error_OUT_OF_MEMORY: return "out of memory";
        case YumDB_Error_IO: return "I/O error";
        case YumDB_Error_WAL_CORRUPTED: return "WAL corrupted";
        case YumDB_Error_NOT_FOUND: return "not found";
        case YumDB_Error_ALREADY_EXISTS: return "already exists";
        case YumDB_Error_INVALID_ARGUMENT: return "invalid argument";
        case YumDB_Error_INVALID_TYPE: return "invalid type";
        case YumDB_Error_CONSTRAINT_VIOLATION: return "constraint violation";
        case YumDB_Error_UNIQUE_VIOLATION: return "unique violation";
        case YumDB_Error_NULL_VIOLATION: return "null violation";
        case YumDB_Error_SCHEMA_MISMATCH: return "schema mismatch";
        case YumDB_Error_TRANSACTION_FAILED: return "transaction failed";
        case YumDB_Error_TRANSACTION_CONFLICT: return "transaction conflict";
        case YumDB_Error_CONNECTION_CLOSED: return "connection closed";
        case YumDB_Error_CORRUPTED: return "corrupted";
        case YumDB_Error_PERMISSION_DENIED: return "permission denied";
        case YumDB_Error_OVERFLOW: return "overflow";
        default: return "unknown";
    }
}

const char* YumDB_ColumnTypeName(YumDB_ColumnType t) {
    static const char* names[] = {
        "NONE","BOOL","INT8","UINT8","INT16","UINT16","INT32","UINT32",
        "INT64","UINT64","FLOAT","DOUBLE","STRING","BINARY"
    };
    if ((int)t < 0 || t > YumDB_ColumnType_BINARY) return "INVALID";
    return names[t];
}

size_t YumDB_ColumnTypeSize(YumDB_ColumnType t) {
    switch (t) {
        case YumDB_ColumnType_BOOL:   return sizeof(bool);
        case YumDB_ColumnType_INT8:   case YumDB_ColumnType_UINT8:  return 1;
        case YumDB_ColumnType_INT16:  case YumDB_ColumnType_UINT16: return 2;
        case YumDB_ColumnType_INT32:  case YumDB_ColumnType_UINT32: return 4;
        case YumDB_ColumnType_INT64:  case YumDB_ColumnType_UINT64: return 8;
        case YumDB_ColumnType_FLOAT:  return 4;
        case YumDB_ColumnType_DOUBLE: return 8;
        default: return 0;
    }
}

bool YumDB_ColumnTypeIsNumeric(YumDB_ColumnType t) {
    return t >= YumDB_ColumnType_INT8 && t <= YumDB_ColumnType_DOUBLE;
}
bool YumDB_ColumnTypeIsIntegral(YumDB_ColumnType t) {
    return t >= YumDB_ColumnType_INT8 && t <= YumDB_ColumnType_UINT64;
}
bool YumDB_ColumnTypeIsVariable(YumDB_ColumnType t) {
    return t == YumDB_ColumnType_STRING || t == YumDB_ColumnType_BINARY;
}

/* ---- constructors ---- */
#define MAKE_SCALAR(FN, FIELD, CTYPE, TAG)                             \
    YumDB_Value FN(CTYPE v) {                                          \
        YumDB_Value r = {0}; r.type = TAG; r.is_null = false;          \
        r.as.FIELD = v; return r;                                      \
    }

YumDB_Value YumDB_Null(void) {
    YumDB_Value r = {0}; r.type = YumDB_ColumnType_NONE; r.is_null = true; return r;
}
MAKE_SCALAR(YumDB_Bool, b,   bool,     YumDB_ColumnType_BOOL)
MAKE_SCALAR(YumDB_I8,   i8,  int8_t,   YumDB_ColumnType_INT8)
MAKE_SCALAR(YumDB_U8,   u8,  uint8_t,  YumDB_ColumnType_UINT8)
MAKE_SCALAR(YumDB_I16,  i16, int16_t,  YumDB_ColumnType_INT16)
MAKE_SCALAR(YumDB_U16,  u16, uint16_t, YumDB_ColumnType_UINT16)
MAKE_SCALAR(YumDB_I32,  i32, int32_t,  YumDB_ColumnType_INT32)
MAKE_SCALAR(YumDB_U32,  u32, uint32_t, YumDB_ColumnType_UINT32)
MAKE_SCALAR(YumDB_I64,  i64, int64_t,  YumDB_ColumnType_INT64)
MAKE_SCALAR(YumDB_U64,  u64, uint64_t, YumDB_ColumnType_UINT64)
MAKE_SCALAR(YumDB_F32,  f32, float,    YumDB_ColumnType_FLOAT)
MAKE_SCALAR(YumDB_F64,  f64, double,   YumDB_ColumnType_DOUBLE)

YumDB_Value YumDB_StrN(const char* s, size_t len) {
    YumDB_Value r = {0};
    r.type = YumDB_ColumnType_STRING;
    if (!s) { r.is_null = true; return r; }
    r.as.str.data = (char*)malloc(len + 1);
    if (r.as.str.data) {
        memcpy(r.as.str.data, s, len);
        r.as.str.data[len] = '\0';
        r.as.str.len = len;
    }
    return r;
}

YumDB_Value YumDB_Bin(const uint8_t* buf, size_t len) {
    YumDB_Value r = {0};
    r.type = YumDB_ColumnType_BINARY;
    if (!buf) { r.is_null = true; return r; }
    r.as.bin.data = (uint8_t*)malloc(len);
    if (r.as.bin.data) {
        memcpy(r.as.bin.data, buf, len);
        r.as.bin.len = len;
    }
    return r;
}

YumDB_Value YumDB_ValueClone(YumDB_Value v) {
    if (v.is_null) return v;
    switch (v.type) {
        case YumDB_ColumnType_STRING:
            return YumDB_StrN(v.as.str.data, v.as.str.len);
        case YumDB_ColumnType_BINARY:
            return YumDB_Bin(v.as.bin.data, v.as.bin.len);
        default:
            return v;
    }
}

void YumDB_ValueFree(YumDB_Value* v) {
    if (!v) return;
    if (v->type == YumDB_ColumnType_STRING && v->as.str.data) {
        free(v->as.str.data); v->as.str.data = NULL;
    } else if (v->type == YumDB_ColumnType_BINARY && v->as.bin.data) {
        free(v->as.bin.data); v->as.bin.data = NULL;
    }
    v->is_null = true;
}

int YumDB_ValueCompare(YumDB_Value a, YumDB_Value b) {
    if (a.is_null && b.is_null) return 0;
    if (a.is_null) return -1;
    if (b.is_null) return 1;
    if (a.type != b.type) return (int)a.type - (int)b.type;
    switch (a.type) {
        case YumDB_ColumnType_BOOL:   return (int)a.as.b  - (int)b.as.b;
        case YumDB_ColumnType_INT8:   return a.as.i8  - b.as.i8;
        case YumDB_ColumnType_UINT8:  return a.as.u8  - b.as.u8;
        case YumDB_ColumnType_INT16:  return a.as.i16 - b.as.i16;
        case YumDB_ColumnType_UINT16: return a.as.u16 - b.as.u16;
        case YumDB_ColumnType_INT32:  return (a.as.i32 < b.as.i32) ? -1 : (a.as.i32 > b.as.i32);
        case YumDB_ColumnType_UINT32: return (a.as.u32 < b.as.u32) ? -1 : (a.as.u32 > b.as.u32);
        case YumDB_ColumnType_INT64:  return (a.as.i64 < b.as.i64) ? -1 : (a.as.i64 > b.as.i64);
        case YumDB_ColumnType_UINT64: return (a.as.u64 < b.as.u64) ? -1 : (a.as.u64 > b.as.u64);
        case YumDB_ColumnType_FLOAT: {
            float d = a.as.f32 - b.as.f32;
            return (d < 0) ? -1 : (d > 0);
        }
        case YumDB_ColumnType_DOUBLE: {
            double d = a.as.f64 - b.as.f64;
            return (d < 0) ? -1 : (d > 0);
        }
        case YumDB_ColumnType_STRING: {
            size_t n = a.as.str.len < b.as.str.len ? a.as.str.len : b.as.str.len;
            int c = memcmp(a.as.str.data, b.as.str.data, n);
            if (c) return c;
            return (int)(a.as.str.len - b.as.str.len);
        }
        case YumDB_ColumnType_BINARY: {
            size_t n = a.as.bin.len < b.as.bin.len ? a.as.bin.len : b.as.bin.len;
            int c = memcmp(a.as.bin.data, b.as.bin.data, n);
            if (c) return c;
            return (int)(a.as.bin.len - b.as.bin.len);
        }
        default: return 0;
    }
}

bool YumDB_ValueEqual(YumDB_Value a, YumDB_Value b) {
    return YumDB_ValueCompare(a, b) == 0;
}

YumDB_Error YumDB_ValueCast(YumDB_Value in, YumDB_ColumnType to, YumDB_Value* out) {
    if (!out) return YumDB_Error_INVALID_ARGUMENT;
    if (in.is_null) { *out = YumDB_Null(); out->type = to; return YumDB_Error_OK; }
    if (in.type == to) { *out = YumDB_ValueClone(in); return YumDB_Error_OK; }
    if (!YumDB_ColumnTypeIsNumeric(in.type) || !YumDB_ColumnTypeIsNumeric(to))
        return YumDB_Error_INVALID_TYPE;

    /* normalize through double for simplicity */
    double d = 0;
    switch (in.type) {
        case YumDB_ColumnType_INT8:   d = in.as.i8; break;
        case YumDB_ColumnType_UINT8:  d = in.as.u8; break;
        case YumDB_ColumnType_INT16:  d = in.as.i16; break;
        case YumDB_ColumnType_UINT16: d = in.as.u16; break;
        case YumDB_ColumnType_INT32:  d = in.as.i32; break;
        case YumDB_ColumnType_UINT32: d = in.as.u32; break;
        case YumDB_ColumnType_INT64:  d = (double)in.as.i64; break;
        case YumDB_ColumnType_UINT64: d = (double)in.as.u64; break;
        case YumDB_ColumnType_FLOAT:  d = in.as.f32; break;
        case YumDB_ColumnType_DOUBLE: d = in.as.f64; break;
        default: return YumDB_Error_INVALID_TYPE;
    }
    switch (to) {
        case YumDB_ColumnType_INT8:   *out = YumDB_I8((int8_t)d); break;
        case YumDB_ColumnType_UINT8:  *out = YumDB_U8((uint8_t)d); break;
        case YumDB_ColumnType_INT16:  *out = YumDB_I16((int16_t)d); break;
        case YumDB_ColumnType_UINT16: *out = YumDB_U16((uint16_t)d); break;
        case YumDB_ColumnType_INT32:  *out = YumDB_I32((int32_t)d); break;
        case YumDB_ColumnType_UINT32: *out = YumDB_U32((uint32_t)d); break;
        case YumDB_ColumnType_INT64:  *out = YumDB_I64((int64_t)d); break;
        case YumDB_ColumnType_UINT64: *out = YumDB_U64((uint64_t)d); break;
        case YumDB_ColumnType_FLOAT:  *out = YumDB_F32((float)d); break;
        case YumDB_ColumnType_DOUBLE: *out = YumDB_F64(d); break;
        default: return YumDB_Error_INVALID_TYPE;
    }
    return YumDB_Error_OK;
}
