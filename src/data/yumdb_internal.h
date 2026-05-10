/*
    YumDB — Internal Shared Header
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

#ifndef YUMI_INTERNAL_H
#define YUMI_INTERNAL_H

#include "yumi_data.h"
#include <pthread.h>
#include <stdio.h>

/* ---------- Condition AST (shared across Sel/Upd/Del sub-builders) ---------- */

typedef enum {
    COND_EQ, COND_NEQ, COND_LT, COND_LTE, COND_GT, COND_GTE,
    COND_IN, COND_NOT_IN, COND_BETWEEN, COND_IS_NULL, COND_IS_NOT_NULL,
    COND_GROUP_OPEN, COND_GROUP_CLOSE
} CondKind;

typedef enum {
    CONN_AND, CONN_OR
} CondConnector;

typedef struct CondNode {
    CondKind       kind;
    CondConnector  connector;   /* how this node joins to the PREVIOUS */
    bool           negated;
    char*          column;      /* owned */
    YumDB_Value    v1;          /* for binary ops & BETWEEN low */
    YumDB_Value    v2;          /* BETWEEN high */
    YumDB_Value*   vlist;       /* IN / NOT IN, owned */
    size_t         vlist_n;
    struct CondNode* next;
} CondNode;

typedef struct CondList {
    CondNode* head;
    CondNode* tail;
    /* staged state for the next clause */
    CondConnector pending_conn;
    bool          pending_not;
} CondList;

void condlist_init(CondList* cl);
void condlist_free(CondList* cl);
void condlist_append(CondList* cl, CondNode* n);
bool condlist_eval_row(const CondList* cl, YumDB_Row row);

/* ---------- Table / Storage ---------- */

typedef struct RowSlot {
    bool           live;         /* tombstone flag */
    uint64_t       id;
    uint64_t       version;      /* MVCC version counter (monotonic) */
    uint64_t       created_tx;   /* tx id that created this version */
    uint64_t       deleted_tx;   /* tx id that deleted, 0 if live */
    YumDB_Value*   values;       /* columns_count entries, owned */
    struct RowSlot* next_version;/* chain for future MVCC expansion */
} RowSlot;

typedef struct Table {
    YumDB_TypeDef    schema;
    RowSlot**        rows;         /* dynamic array of slot pointers */
    size_t           rows_count;
    size_t           rows_cap;
    uint64_t         next_id;
    pthread_rwlock_t lock;
    struct Table*    next;
} Table;

/* ---------- WAL ---------- */

typedef enum {
    WAL_BEGIN = 1,
    WAL_COMMIT,
    WAL_ABORT,
    WAL_INSERT,
    WAL_UPDATE,
    WAL_DELETE,
    WAL_CREATE_TABLE,
    WAL_DROP_TABLE,
    WAL_CHECKPOINT
} WalOp;

typedef struct Wal {
    FILE*           fp;
    char*           path;
    size_t          segment_bytes;
    bool            fsync_on_commit;
    pthread_mutex_t mu;
    uint64_t        lsn;         /* monotonic */
} Wal;

YumDB_Error wal_open(Wal* w, const char* dir, size_t segment_bytes, bool fsync_on_commit);
void        wal_close(Wal* w);
YumDB_Error wal_append_begin(Wal* w, uint64_t tx_id);
YumDB_Error wal_append_commit(Wal* w, uint64_t tx_id);
YumDB_Error wal_append_abort(Wal* w, uint64_t tx_id);
YumDB_Error wal_append_row_op(Wal* w, WalOp op, uint64_t tx_id,
                              const char* table, YumDB_Row row);
YumDB_Error wal_fsync(Wal* w);

/* ---------- Transaction ---------- */

typedef enum { TX_ACTIVE, TX_COMMITTED, TX_ABORTED } TxState;

typedef struct TxOp {
    WalOp       op;
    Table*      table;
    RowSlot*    slot;           /* target slot */
    YumDB_Value* prior_values;  /* snapshot for rollback, owned */
    struct TxOp* next;
} TxOp;

struct YumDB_Tx {
    /* vtable (must match header) */
    YumDB_Selector (*Select)(YumDB_Tx, const char*);
    YumDB_Inserter (*Insert)(YumDB_Tx, const char*);
    YumDB_Updater  (*Update)(YumDB_Tx, const char*);
    YumDB_Deleter  (*Delete)(YumDB_Tx, const char*);
    YumDB_Error (*Savepoint)(YumDB_Tx, const char*);
    YumDB_Error (*RollbackTo)(YumDB_Tx, const char*);
    YumDB_Error (*Commit)(YumDB_Tx);
    YumDB_Error (*Rollback)(YumDB_Tx);
    bool        (*IsActive)(YumDB_Tx);

    /* state */
    struct YumDB_Impl* db;
    uint64_t  tx_id;
    TxState   state;
    TxOp*     ops_head;
    TxOp*     ops_tail;
    bool      implicit;   /* auto-commit on Done() */
};

YumDB_Tx  tx_begin(struct YumDB_Impl* db, bool implicit);
void      tx_record_op(YumDB_Tx tx, WalOp op, Table* t, RowSlot* slot, YumDB_Value* prior);

/* ---------- DB ---------- */

typedef struct YumDB_Impl {
    /* vtable (must match header layout) */
    char* dir_path;
    YumDB_Error    (*CreateTable)(YumDB, YumDB_TypeDef);
    YumDB_Error    (*DropTable)(YumDB, const char*);
    bool           (*HasTable)(YumDB, const char*);
    YumDB_TypeDef  (*GetSchema)(YumDB, const char*);
    YumDB_Error    (*RenameTable)(YumDB, const char*, const char*);
    YumDB_Error    (*AlterAddColumn)(YumDB, const char*, YumDB_Column);
    YumDB_Error    (*AlterDropColumn)(YumDB, const char*, const char*);
    YumDB_Error (*CreateIndex)(YumDB, const char*, const char*, YumDB_IndexType);
    YumDB_Error (*DropIndex)(YumDB, const char*, const char*);
    YumDB_Selector (*Select)(YumDB, const char*);
    YumDB_Inserter (*Insert)(YumDB, const char*);
    YumDB_Updater  (*Update)(YumDB, const char*);
    YumDB_Deleter  (*Delete)(YumDB, const char*);
    YumDB_Tx (*Begin)(YumDB);
    YumDB_Error (*Checkpoint)(YumDB);
    YumDB_Error (*Flush)(YumDB);
    YumDB_Error (*Compact)(YumDB);
    YumDB_Error (*Backup)(YumDB, const char*);
    YumDB_Error (*Restore)(YumDB, const char*);
    YumDB_Error  (*LastError)(YumDB);
    const char*  (*LastErrorMessage)(YumDB);
    size_t       (*TableCount)(YumDB);
    const char** (*TableNames)(YumDB, size_t*);

    /* internal state */
    Table*           tables;
    pthread_mutex_t  schema_mu;
    Wal              wal;
    uint64_t         next_tx_id;
    pthread_mutex_t  tx_mu;
    YumDB_Error      last_error;
    char             last_error_msg[256];
} YumDB_Impl;

/* table helpers */
Table*      db_find_table(YumDB_Impl* db, const char* name);
YumDB_Error table_insert(Table* t, YumDB_Row row, RowSlot** out_slot);
YumDB_Error table_validate_row(Table* t, YumDB_Row row);

#endif
