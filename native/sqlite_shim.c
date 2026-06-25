/* native/sqlite_shim.c — Depot SQLite FFI shim.
 *
 * Wraps libsqlite3 for use from March via the C FFI ABI defined in
 * march_ffi.h. Compiled automatically by forge when [ffi] is configured.
 *
 * ABI conventions (from march_ffi.h and test/native/ffi_shim.c):
 *   - Int parameters arrive as raw int64_t, NOT as tagged march_value.
 *   - Int return values must be returned as raw int64_t, not march_make_int().
 *   - String / resource / Option / Result use march_value throughout.
 *
 * Resource lifetime: sqlite3* and sqlite3_stmt* are registered with
 * march_resource_type so Perceus RC fires sqlite3_close_v2 / sqlite3_finalize
 * automatically when the last March reference drops — no explicit cleanup
 * needed in March code.
 */

#include "march_ffi.h"
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* ---- Resource type IDs (initialised once, thread-safely) ---- */

static int32_t db_type_id   = -1;
static int32_t stmt_type_id = -1;
static pthread_once_t types_once = PTHREAD_ONCE_INIT;

/* sqlite3_close_v2 defers the close until all prepared statements are
 * finalized, avoiding SQLITE_BUSY when Perceus drops db before stmt. */
static void db_destructor(void *ptr)   { sqlite3_close_v2((sqlite3 *)ptr); }
static void stmt_destructor(void *ptr) { sqlite3_finalize((sqlite3_stmt *)ptr); }

static void init_types(void) {
    db_type_id   = march_resource_type("depot_sqlite3",      db_destructor);
    stmt_type_id = march_resource_type("depot_sqlite3_stmt", stmt_destructor);
}

static void ensure_types(void) { pthread_once(&types_once, init_types); }

/* ---- String helpers ---- */

/* Borrow a March string into a NUL-terminated C string (caller must free).
 * Returns NULL on malloc failure — callers must check before use. */
static char *str_to_cstr(march_value s) {
    march_slice sl = march_str_borrow(s);
    char *buf = malloc(sl.len + 1);
    if (!buf) return NULL;
    memcpy(buf, sl.ptr, sl.len);
    buf[sl.len] = '\0';
    return buf;
}

/* Wrap a NUL-terminated C string into a March string. NULL -> empty string. */
static march_value cstr_to_str(const char *s) {
    if (!s) return march_str_new((const uint8_t *)"", 0);
    return march_str_new((const uint8_t *)s, strlen(s));
}

/* ---- Exported functions ---- */

/* depot_sqlite_open(path: String): Result(Db, String) */
march_value depot_sqlite_open(march_value path_str) {
    ensure_types();
    char *path = str_to_cstr(path_str);
    if (!path) return march_err(cstr_to_str("out of memory"));
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    free(path);
    if (rc != SQLITE_OK) {
        march_value err = cstr_to_str(db ? sqlite3_errmsg(db) : "sqlite3_open failed");
        if (db) sqlite3_close_v2(db);
        return march_err(err);
    }
    return march_ok(march_resource_new(db_type_id, db));
}

/* depot_sqlite_prepare(db: Db, sql: String): Result(Stmt, String) */
march_value depot_sqlite_prepare(march_value db_val, march_value sql_str) {
    sqlite3 *db = march_resource_get(db_val, db_type_id);
    char *sql = str_to_cstr(sql_str);
    if (!sql) return march_err(cstr_to_str("out of memory"));
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    free(sql);
    if (rc != SQLITE_OK) {
        return march_err(cstr_to_str(sqlite3_errmsg(db)));
    }
    return march_ok(march_resource_new(stmt_type_id, stmt));
}

/* depot_sqlite_bind_text(stmt: Stmt, idx: Int, val: String): Int
 * idx is 1-based. Returns SQLITE_OK (0) on success. */
int64_t depot_sqlite_bind_text(march_value stmt_val, int64_t idx,
                               march_value text_val) {
    sqlite3_stmt *stmt = march_resource_get(stmt_val, stmt_type_id);
    march_slice s = march_str_borrow(text_val);
    return (int64_t)sqlite3_bind_text(stmt, (int)idx, (const char *)s.ptr,
                                      (int)s.len, SQLITE_TRANSIENT);
}

/* depot_sqlite_bind_null(stmt: Stmt, idx: Int): Int
 * idx is 1-based. Returns SQLITE_OK (0) on success. */
int64_t depot_sqlite_bind_null(march_value stmt_val, int64_t idx) {
    sqlite3_stmt *stmt = march_resource_get(stmt_val, stmt_type_id);
    return (int64_t)sqlite3_bind_null(stmt, (int)idx);
}

/* depot_sqlite_step(stmt: Stmt): Int
 * Returns 100 (SQLITE_ROW), 101 (SQLITE_DONE), or an error code. */
int64_t depot_sqlite_step(march_value stmt_val) {
    sqlite3_stmt *stmt = march_resource_get(stmt_val, stmt_type_id);
    return (int64_t)sqlite3_step(stmt);
}

/* depot_sqlite_db_id(db: Db): Int
 * Returns the sqlite3* pointer value as an integer — unique per connection,
 * safe to use as a conn_key even when multiple connections share the same path
 * (e.g. ":memory:" opened twice). */
int64_t depot_sqlite_db_id(march_value db_val) {
    sqlite3 *db = march_resource_get(db_val, db_type_id);
    return (int64_t)(uintptr_t)db;
}

/* depot_sqlite_column_count(stmt: Stmt): Int */
int64_t depot_sqlite_column_count(march_value stmt_val) {
    sqlite3_stmt *stmt = march_resource_get(stmt_val, stmt_type_id);
    return (int64_t)sqlite3_column_count(stmt);
}

/* depot_sqlite_column_name(stmt: Stmt, idx: Int): String
 * idx is zero-based. */
march_value depot_sqlite_column_name(march_value stmt_val, int64_t idx) {
    sqlite3_stmt *stmt = march_resource_get(stmt_val, stmt_type_id);
    return cstr_to_str(sqlite3_column_name(stmt, (int)idx));
}

/* depot_sqlite_column_text(stmt: Stmt, idx: Int): Option(String)
 * Returns None for NULL columns, Some(text) otherwise.
 * Uses niche encoding: Some(heap_ptr) == heap_ptr, None == 0.
 * sqlite3_column_text may return NULL on OOM during BLOB->TEXT coercion;
 * we treat that as None rather than silently returning Some(""). */
march_value depot_sqlite_column_text(march_value stmt_val, int64_t idx) {
    sqlite3_stmt *stmt = march_resource_get(stmt_val, stmt_type_id);
    if (sqlite3_column_type(stmt, (int)idx) == SQLITE_NULL) {
        return march_none();
    }
    const unsigned char *text = sqlite3_column_text(stmt, (int)idx);
    if (!text) return march_none();  /* OOM during BLOB->TEXT coercion */
    int len = sqlite3_column_bytes(stmt, (int)idx);
    return march_some(march_str_new(text, (size_t)len));
}
