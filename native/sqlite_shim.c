/* native/sqlite_shim.c — Depot SQLite FFI shim.
 * Wraps libsqlite3 for use from March via the C FFI ABI.
 * Build: compiled automatically by forge when [ffi] is configured.
 */
#include "march_ffi.h"
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>

int32_t march_ffi_abi_version(void) { return MARCH_FFI_ABI_VERSION; }

/* Stub — filled in Task 4. */
march_value depot_sqlite_open(march_value path_str)         { return march_err(march_str_new((const uint8_t*)"not implemented", 15)); }
march_value depot_sqlite_prepare(march_value db_val, march_value sql_str) { return march_err(march_str_new((const uint8_t*)"not implemented", 15)); }
march_value depot_sqlite_bind_text(march_value stmt_val, march_value idx_val, march_value text_val) { return march_make_int(0); }
march_value depot_sqlite_bind_null(march_value stmt_val, march_value idx_val) { return march_make_int(0); }
march_value depot_sqlite_step(march_value stmt_val)         { return march_make_int(21); } /* SQLITE_MISUSE */
march_value depot_sqlite_column_count(march_value stmt_val) { return march_make_int(0); }
march_value depot_sqlite_column_name(march_value stmt_val, march_value idx_val) { return march_str_new((const uint8_t*)"", 0); }
march_value depot_sqlite_column_text(march_value stmt_val, march_value idx_val) { return march_none(); }
