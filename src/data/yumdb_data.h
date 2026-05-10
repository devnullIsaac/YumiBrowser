/*
    YumDB — Public Data Types and API Surface
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

#ifndef YUMI_DATA_H
#define YUMI_DATA_H

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Error Handling
 * ========================================================================= */

typedef enum YumDB_Error {
    YumDB_Error_OK = 0,
    YumDB_Error_UNKNOWN,
    YumDB_Error_OUT_OF_MEMORY,
    YumDB_Error_IO,
    YumDB_Error_WAL_CORRUPTED,
    YumDB_Error_NOT_FOUND,
    YumDB_Error_ALREADY_EXISTS,
    YumDB_Error_INVALID_ARGUMENT,
    YumDB_Error_INVALID_TYPE,
    YumDB_Error_CONSTRAINT_VIOLATION,
    YumDB_Error_UNIQUE_VIOLATION,
    YumDB_Error_NULL_VIOLATION,
    YumDB_Error_SCHEMA_MISMATCH,
    YumDB_Error_TRANSACTION_FAILED,
    YumDB_Error_TRANSACTION_CONFLICT,  /* optimistic concurrency retry */
    YumDB_Error_CONNECTION_CLOSED,
    YumDB_Error_CORRUPTED,
    YumDB_Error_PERMISSION_DENIED,
    YumDB_Error_OVERFLOW
} YumDB_Error;

const char* YumDB_ErrorString(YumDB_Error err);

/* =========================================================================
 * Column Types — primitive C types only
 * ========================================================================= */

typedef enum YumDB_ColumnType {
    YumDB_ColumnType_NONE,
    YumDB_ColumnType_BOOL,
    YumDB_ColumnType_INT8,
    YumDB_ColumnType_UINT8,
    YumDB_ColumnType_INT16,
    YumDB_ColumnType_UINT16,
    YumDB_ColumnType_INT32,
    YumDB_ColumnType_UINT32,
    YumDB_ColumnType_INT64,
    YumDB_ColumnType_UINT64,
    YumDB_ColumnType_FLOAT,     /* C float  */
    YumDB_ColumnType_DOUBLE,    /* C double */
    YumDB_ColumnType_STRING,    /* UTF-8, length-prefixed */
    YumDB_ColumnType_BINARY     /* raw bytes, length-prefixed */
} YumDB_ColumnType;

const char* YumDB_ColumnTypeName(YumDB_ColumnType t);
size_t      YumDB_ColumnTypeSize(YumDB_ColumnType t);   /* 0 for variable */
bool        YumDB_ColumnTypeIsNumeric(YumDB_ColumnType t);
bool        YumDB_ColumnTypeIsIntegral(YumDB_ColumnType t);
bool        YumDB_ColumnTypeIsVariable(YumDB_ColumnType t);

/* =========================================================================
 * Value — tagged union for a single cell
 * ========================================================================= */

typedef struct YumDB_Value {
    YumDB_ColumnType type;
    bool             is_null;
    union {
        bool     b;
        int8_t   i8;
        uint8_t  u8;
        int16_t  i16;
        uint16_t u16;
        int32_t  i32;
        uint32_t u32;
        int64_t  i64;
        uint64_t u64;
        float    f32;
        double   f64;
        struct { char*    data; size_t len; } str;
        struct { uint8_t* data; size_t len; } bin;
    } as;
} YumDB_Value;

/* constructors */
YumDB_Value YumDB_Null(void);
YumDB_Value YumDB_Bool(bool v);
YumDB_Value YumDB_I8(int8_t v);
YumDB_Value YumDB_U8(uint8_t v);
YumDB_Value YumDB_I16(int16_t v);
YumDB_Value YumDB_U16(uint16_t v);
YumDB_Value YumDB_I32(int32_t v);
YumDB_Value YumDB_U32(uint32_t v);
YumDB_Value YumDB_I64(int64_t v);
YumDB_Value YumDB_U64(uint64_t v);
YumDB_Value YumDB_F32(float v);
YumDB_Value YumDB_F64(double v);
YumDB_Value YumDB_StrN(const char* s, size_t len);
YumDB_Value YumDB_Bin(const uint8_t* buf, size_t len);

/* value utilities */
YumDB_Value YumDB_ValueClone(YumDB_Value v);
void        YumDB_ValueFree(YumDB_Value* v);
bool        YumDB_ValueEqual(YumDB_Value a, YumDB_Value b);
int         YumDB_ValueCompare(YumDB_Value a, YumDB_Value b);
YumDB_Error YumDB_ValueCast(YumDB_Value in, YumDB_ColumnType to, YumDB_Value* out);

/* =========================================================================
 * Column definition
 * ========================================================================= */

typedef struct YumDB_Column* YumDB_Column;
struct YumDB_Column {
    YumDB_Column     next;            /* intrusive list */
    char*            name;
    YumDB_ColumnType type;
    size_t           len;             /* fixed length (STRING/BINARY) */
    size_t           min;             /* min bound (length or numeric) */
    size_t           max;             /* max bound (length or numeric) */
    bool             unique;
    bool             nullable;
    bool             primary_key;
    bool             auto_increment;
    bool             indexed;
    YumDB_Value      default_value;
};

YumDB_Column YumDB_ColumnCreate(const char* name, YumDB_ColumnType type);
void         YumDB_ColumnDestroy(YumDB_Column c);

/* =========================================================================
 * Column Builder (fluent)
 * ========================================================================= */

typedef struct YumDB_ColumnBuilder* YumDB_ColumnBuilder;
struct YumDB_ColumnBuilder {
    YumDB_ColumnBuilder (*Len)(YumDB_ColumnBuilder this, size_t len);
    YumDB_ColumnBuilder (*Min)(YumDB_ColumnBuilder this, size_t min);
    YumDB_ColumnBuilder (*Max)(YumDB_ColumnBuilder this, size_t max);
    YumDB_ColumnBuilder (*Unique)(YumDB_ColumnBuilder this, bool unique);
    YumDB_ColumnBuilder (*Nullable)(YumDB_ColumnBuilder this, bool nullable);
    YumDB_ColumnBuilder (*PrimaryKey)(YumDB_ColumnBuilder this, bool pk);
    YumDB_ColumnBuilder (*AutoIncrement)(YumDB_ColumnBuilder this, bool ai);
    YumDB_ColumnBuilder (*Indexed)(YumDB_ColumnBuilder this, bool idx);
    YumDB_ColumnBuilder (*Default)(YumDB_ColumnBuilder this, YumDB_Value v);
    YumDB_Column        (*Build)(YumDB_ColumnBuilder this);
    void                (*Destroy)(YumDB_ColumnBuilder this);
};

YumDB_ColumnBuilder YumDB_ColumnBuilderNew(const char* name, YumDB_ColumnType type);

/* =========================================================================
 * TypeDef — a table's schema
 * ========================================================================= */

typedef struct YumDB_TypeDef {
    char*         name;
    size_t        columns_count;
    YumDB_Column* columns;
} *YumDB_TypeDef;

typedef struct YumDB_TypeDefBuilder* YumDB_TypeDefBuilder;
struct YumDB_TypeDefBuilder {
    YumDB_TypeDefBuilder (*AddColumn)(YumDB_TypeDefBuilder this, YumDB_Column c);
    YumDB_TypeDefBuilder (*PrimaryKey)(YumDB_TypeDefBuilder this, const char* col);
    YumDB_TypeDefBuilder (*CompositeKey)(YumDB_TypeDefBuilder this,
                                         const char** cols, size_t n);
    YumDB_TypeDefBuilder (*Index)(YumDB_TypeDefBuilder this, const char* col);
    YumDB_TypeDef        (*Build)(YumDB_TypeDefBuilder this);
    void                 (*Destroy)(YumDB_TypeDefBuilder this);
};

YumDB_TypeDefBuilder YumDB_TypeDefBuilderNew(const char* table_name);
void                 YumDB_TypeDefDestroy(YumDB_TypeDef t);
YumDB_Column         YumDB_TypeDefFindColumn(YumDB_TypeDef t, const char* name);
size_t               YumDB_TypeDefColumnIndex(YumDB_TypeDef t, const char* name);

/* =========================================================================
 * Row
 * ========================================================================= */

typedef struct YumDB_Row* YumDB_Row;
struct YumDB_Row {
    uint64_t      id;
    YumDB_TypeDef type;
    size_t        values_count;
    YumDB_Value*  values;

    YumDB_Value (*Get)(YumDB_Row this, const char* column);
    YumDB_Value (*GetAt)(YumDB_Row this, size_t index);
    YumDB_Error (*Set)(YumDB_Row this, const char* column, YumDB_Value v);
    YumDB_Error (*SetAt)(YumDB_Row this, size_t index, YumDB_Value v);
    bool        (*HasColumn)(YumDB_Row this, const char* column);
    bool        (*IsNull)(YumDB_Row this, const char* column);
    YumDB_Row   (*Clone)(YumDB_Row this);
    void        (*Destroy)(YumDB_Row this);
};

YumDB_Row YumDB_RowNew(YumDB_TypeDef type);

/* =========================================================================
 * Result set
 * ========================================================================= */

typedef struct YumDB_Result* YumDB_Result;
struct YumDB_Result {
    size_t     count;
    YumDB_Row* rows;

    bool      (*HasNext)(YumDB_Result this);
    YumDB_Row (*Next)(YumDB_Result this);
    void      (*Reset)(YumDB_Result this);
    YumDB_Row (*At)(YumDB_Result this, size_t idx);
    size_t    (*Count)(YumDB_Result this);
    void      (*Destroy)(YumDB_Result this);
};

/* =========================================================================
 * Common ordering / sort
 * ========================================================================= */

typedef enum YumDB_Order {
    YumDB_Order_ASC,
    YumDB_Order_DESC
} YumDB_Order;

/* =========================================================================
 * Forward declarations (parent builders referenced by condition sub-builders)
 * ========================================================================= */

typedef struct YumDB_Selector* YumDB_Selector;
typedef struct YumDB_Updater*  YumDB_Updater;
typedef struct YumDB_Deleter*  YumDB_Deleter;

/* =========================================================================
 * Selector condition sub-builder
 *   sel->Where(sel)->Eq(..,"name",v)->And()->Gt(..,"age",v)->Done() -> Selector
 * ========================================================================= */

typedef struct YumDB_Selector_Condition* YumDB_Selector_Condition;
struct YumDB_Selector_Condition {
    /* clauses */
    YumDB_Selector_Condition (*Eq)(YumDB_Selector_Condition this, const char* col, YumDB_Value v);
    YumDB_Selector_Condition (*Neq)(YumDB_Selector_Condition this, const char* col, YumDB_Value v);
    YumDB_Selector_Condition (*Lt)(YumDB_Selector_Condition this, const char* col, YumDB_Value v);
    YumDB_Selector_Condition (*Lte)(YumDB_Selector_Condition this, const char* col, YumDB_Value v);
    YumDB_Selector_Condition (*Gt)(YumDB_Selector_Condition this, const char* col, YumDB_Value v);
    YumDB_Selector_Condition (*Gte)(YumDB_Selector_Condition this, const char* col, YumDB_Value v);
    YumDB_Selector_Condition (*In)(YumDB_Selector_Condition this, const char* col,
                                   const YumDB_Value* vals, size_t n);
    YumDB_Selector_Condition (*NotIn)(YumDB_Selector_Condition this, const char* col,
                                      const YumDB_Value* vals, size_t n);
    YumDB_Selector_Condition (*Between)(YumDB_Selector_Condition this, const char* col,
                                        YumDB_Value lo, YumDB_Value hi);
    YumDB_Selector_Condition (*IsNull)(YumDB_Selector_Condition this, const char* col);
    YumDB_Selector_Condition (*IsNotNull)(YumDB_Selector_Condition this, const char* col);

    /* connectors — set the combinator for the NEXT clause (default AND) */
    YumDB_Selector_Condition (*And)(YumDB_Selector_Condition this);
    YumDB_Selector_Condition (*Or)(YumDB_Selector_Condition this);
    YumDB_Selector_Condition (*Not)(YumDB_Selector_Condition this);

    /* grouping for parenthesized sub-expressions */
    YumDB_Selector_Condition (*Group)(YumDB_Selector_Condition this);
    YumDB_Selector_Condition (*EndGroup)(YumDB_Selector_Condition this);

    /* return to parent */
    YumDB_Selector (*Done)(YumDB_Selector_Condition this);
};

/* =========================================================================
 * Updater condition sub-builder
 * ========================================================================= */

typedef struct YumDB_Updater_Condition* YumDB_Updater_Condition;
struct YumDB_Updater_Condition {
    YumDB_Updater_Condition (*Eq)(YumDB_Updater_Condition this, const char* col, YumDB_Value v);
    YumDB_Updater_Condition (*Neq)(YumDB_Updater_Condition this, const char* col, YumDB_Value v);
    YumDB_Updater_Condition (*Lt)(YumDB_Updater_Condition this, const char* col, YumDB_Value v);
    YumDB_Updater_Condition (*Lte)(YumDB_Updater_Condition this, const char* col, YumDB_Value v);
    YumDB_Updater_Condition (*Gt)(YumDB_Updater_Condition this, const char* col, YumDB_Value v);
    YumDB_Updater_Condition (*Gte)(YumDB_Updater_Condition this, const char* col, YumDB_Value v);
    YumDB_Updater_Condition (*In)(YumDB_Updater_Condition this, const char* col,
                                  const YumDB_Value* vals, size_t n);
    YumDB_Updater_Condition (*NotIn)(YumDB_Updater_Condition this, const char* col,
                                     const YumDB_Value* vals, size_t n);
    YumDB_Updater_Condition (*Between)(YumDB_Updater_Condition this, const char* col,
                                       YumDB_Value lo, YumDB_Value hi);
    YumDB_Updater_Condition (*IsNull)(YumDB_Updater_Condition this, const char* col);
    YumDB_Updater_Condition (*IsNotNull)(YumDB_Updater_Condition this, const char* col);

    YumDB_Updater_Condition (*And)(YumDB_Updater_Condition this);
    YumDB_Updater_Condition (*Or)(YumDB_Updater_Condition this);
    YumDB_Updater_Condition (*Not)(YumDB_Updater_Condition this);
    YumDB_Updater_Condition (*Group)(YumDB_Updater_Condition this);
    YumDB_Updater_Condition (*EndGroup)(YumDB_Updater_Condition this);

    YumDB_Updater (*Done)(YumDB_Updater_Condition this);
};

/* =========================================================================
 * Deleter condition sub-builder
 * ========================================================================= */

typedef struct YumDB_Deleter_Condition* YumDB_Deleter_Condition;
struct YumDB_Deleter_Condition {
    YumDB_Deleter_Condition (*Eq)(YumDB_Deleter_Condition this, const char* col, YumDB_Value v);
    YumDB_Deleter_Condition (*Neq)(YumDB_Deleter_Condition this, const char* col, YumDB_Value v);
    YumDB_Deleter_Condition (*Lt)(YumDB_Deleter_Condition this, const char* col, YumDB_Value v);
    YumDB_Deleter_Condition (*Lte)(YumDB_Deleter_Condition this, const char* col, YumDB_Value v);
    YumDB_Deleter_Condition (*Gt)(YumDB_Deleter_Condition this, const char* col, YumDB_Value v);
    YumDB_Deleter_Condition (*Gte)(YumDB_Deleter_Condition this, const char* col, YumDB_Value v);
    YumDB_Deleter_Condition (*In)(YumDB_Deleter_Condition this, const char* col,
                                  const YumDB_Value* vals, size_t n);
    YumDB_Deleter_Condition (*NotIn)(YumDB_Deleter_Condition this, const char* col,
                                     const YumDB_Value* vals, size_t n);
    YumDB_Deleter_Condition (*Between)(YumDB_Deleter_Condition this, const char* col,
                                       YumDB_Value lo, YumDB_Value hi);
    YumDB_Deleter_Condition (*IsNull)(YumDB_Deleter_Condition this, const char* col);
    YumDB_Deleter_Condition (*IsNotNull)(YumDB_Deleter_Condition this, const char* col);

    YumDB_Deleter_Condition (*And)(YumDB_Deleter_Condition this);
    YumDB_Deleter_Condition (*Or)(YumDB_Deleter_Condition this);
    YumDB_Deleter_Condition (*Not)(YumDB_Deleter_Condition this);
    YumDB_Deleter_Condition (*Group)(YumDB_Deleter_Condition this);
    YumDB_Deleter_Condition (*EndGroup)(YumDB_Deleter_Condition this);

    YumDB_Deleter (*Done)(YumDB_Deleter_Condition this);
};

/* =========================================================================
 * Selector
 * ========================================================================= */

struct YumDB_Selector {
    YumDB_Selector           (*Columns)(YumDB_Selector this, const char** cols, size_t n);
    YumDB_Selector_Condition (*Where)(YumDB_Selector this);
    YumDB_Selector           (*OrderBy)(YumDB_Selector this, const char* column, YumDB_Order order);
    YumDB_Selector           (*Limit)(YumDB_Selector this, size_t n);
    YumDB_Selector           (*Offset)(YumDB_Selector this, size_t n);
    YumDB_Selector           (*Distinct)(YumDB_Selector this, bool distinct);

    /* Execution endpoints — each commits its own implicit transaction
     * unless invoked through a YumDB_Tx. */
    YumDB_Result (*Done)(YumDB_Selector this);
    YumDB_Row    (*First)(YumDB_Selector this);
    size_t       (*Count)(YumDB_Selector this);
    bool         (*Exists)(YumDB_Selector this);

    YumDB_Error (*LastError)(YumDB_Selector this);
    void        (*Destroy)(YumDB_Selector this);
};

/* =========================================================================
 * Inserter
 * ========================================================================= */

typedef struct YumDB_Inserter* YumDB_Inserter;
struct YumDB_Inserter {
    YumDB_Inserter (*Set)(YumDB_Inserter this, const char* col, YumDB_Value v);
    YumDB_Inserter (*SetRow)(YumDB_Inserter this, YumDB_Row row);
    YumDB_Inserter (*Batch)(YumDB_Inserter this, YumDB_Row* rows, size_t n);
    YumDB_Inserter (*OnConflictIgnore)(YumDB_Inserter this);
    YumDB_Inserter (*OnConflictReplace)(YumDB_Inserter this);

    YumDB_Row   (*Done)(YumDB_Inserter this);
    size_t      (*DoneMany)(YumDB_Inserter this);
    YumDB_Error (*LastError)(YumDB_Inserter this);
    void        (*Destroy)(YumDB_Inserter this);
};

/* =========================================================================
 * Updater
 * ========================================================================= */

struct YumDB_Updater {
    YumDB_Updater           (*Set)(YumDB_Updater this, const char* col, YumDB_Value v);
    YumDB_Updater           (*Increment)(YumDB_Updater this, const char* col, int64_t delta);
    YumDB_Updater_Condition (*Where)(YumDB_Updater this);

    size_t      (*Done)(YumDB_Updater this);        /* rows affected */
    YumDB_Error (*LastError)(YumDB_Updater this);
    void        (*Destroy)(YumDB_Updater this);
};

/* =========================================================================
 * Deleter
 * ========================================================================= */

struct YumDB_Deleter {
    YumDB_Deleter_Condition (*Where)(YumDB_Deleter this);
    YumDB_Deleter           (*Limit)(YumDB_Deleter this, size_t n);

    size_t      (*Done)(YumDB_Deleter this);
    YumDB_Error (*LastError)(YumDB_Deleter this);
    void        (*Destroy)(YumDB_Deleter this);
};

/* =========================================================================
 * Transactions — every operation is transactional by default.
 * Use a Tx to group multiple operations atomically.
 * ========================================================================= */

typedef struct YumDB_Tx* YumDB_Tx;
struct YumDB_Tx {
    YumDB_Selector (*Select)(YumDB_Tx this, const char* table);
    YumDB_Inserter (*Insert)(YumDB_Tx this, const char* table);
    YumDB_Updater  (*Update)(YumDB_Tx this, const char* table);
    YumDB_Deleter  (*Delete)(YumDB_Tx this, const char* table);

    YumDB_Error (*Savepoint)(YumDB_Tx this, const char* name);
    YumDB_Error (*RollbackTo)(YumDB_Tx this, const char* name);
    YumDB_Error (*Commit)(YumDB_Tx this);
    YumDB_Error (*Rollback)(YumDB_Tx this);
    bool        (*IsActive)(YumDB_Tx this);
};

/* =========================================================================
 * Indexes
 * ========================================================================= */

typedef enum YumDB_IndexType {
    YumDB_IndexType_BTREE,
    YumDB_IndexType_HASH,
    YumDB_IndexType_UNIQUE
} YumDB_IndexType;

/* =========================================================================
 * Main Database Handle
 * ========================================================================= */

typedef struct YumDB_Config {
    const char* dir_path;
    bool        create_if_missing;
    bool        read_only;
    size_t      cache_size_bytes;
    size_t      wal_segment_bytes;     /* size at which WAL rotates */
    bool        wal_fsync_on_commit;   /* durability vs throughput */
} YumDB_Config;

typedef struct YumDB* YumDB;
struct YumDB {
    char* dir_path;

    /* Schema */
    YumDB_Error    (*CreateTable)(YumDB this, YumDB_TypeDef type);
    YumDB_Error    (*DropTable)(YumDB this, const char* table);
    bool           (*HasTable)(YumDB this, const char* table);
    YumDB_TypeDef  (*GetSchema)(YumDB this, const char* table);
    YumDB_Error    (*RenameTable)(YumDB this, const char* old_name, const char* new_name);
    YumDB_Error    (*AlterAddColumn)(YumDB this, const char* table, YumDB_Column col);
    YumDB_Error    (*AlterDropColumn)(YumDB this, const char* table, const char* col);

    /* Indexes */
    YumDB_Error (*CreateIndex)(YumDB this, const char* table,
                               const char* column, YumDB_IndexType kind);
    YumDB_Error (*DropIndex)(YumDB this, const char* table, const char* column);

    /* CRUD — each call is an implicit single-operation transaction */
    YumDB_Selector (*Select)(YumDB this, const char* table);
    YumDB_Inserter (*Insert)(YumDB this, const char* table);
    YumDB_Updater  (*Update)(YumDB this, const char* table);
    YumDB_Deleter  (*Delete)(YumDB this, const char* table);

    /* Explicit multi-operation transaction */
    YumDB_Tx (*Begin)(YumDB this);

    /* WAL / maintenance */
    YumDB_Error (*Checkpoint)(YumDB this);   /* flush WAL into base files */
    YumDB_Error (*Flush)(YumDB this);        /* fsync WAL */
    YumDB_Error (*Compact)(YumDB this);
    YumDB_Error (*Backup)(YumDB this, const char* dest_dir);
    YumDB_Error (*Restore)(YumDB this, const char* src_dir);

    /* Diagnostics */
    YumDB_Error  (*LastError)(YumDB this);
    const char*  (*LastErrorMessage)(YumDB this);
    size_t       (*TableCount)(YumDB this);
    const char** (*TableNames)(YumDB this, size_t* out_count);
};

/* Lifecycle */
YumDB       YumDB_Open(const char* dir_path);
YumDB       YumDB_OpenWith(const YumDB_Config* cfg);
YumDB_Error YumDB_Close(YumDB db);

#endif /* YUMI_DATA_H */
