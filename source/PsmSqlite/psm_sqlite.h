/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2015 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * psm_sqlite.h — PSM SQLite storage engine (internal header).
 *
 * Not part of the public API.  Include psm_api.h instead.
 */

#ifndef PSM_SQLITE_H
#define PSM_SQLITE_H

#ifdef PSM_BUNDLED_SQLITE3
#  include "sqlite3.h"
#else
#  include <sqlite3.h>
#endif

/**
 * psm_sqlite_open - open (or create) the PSM database.
 *
 * Creates the psm_records table if absent.
 * Configures PRAGMA journal_mode=WAL and synchronous=NORMAL.
 * Applies chmod 600 to the file on first creation (protects stored secrets).
 *
 * @param db_path  Absolute path to the SQLite file.
 * @return 0 on success, -1 on failure.
 */
int psm_sqlite_open(const char *db_path);

/**
 * psm_sqlite_close - finalize all prepared statements and close the database.
 */
void psm_sqlite_close(void);

/**
 * psm_sqlite_is_empty - return 1 if psm_records has zero rows, 0 otherwise.
 */
int psm_sqlite_is_empty(void);

/**
 * psm_sqlite_get - fetch a record by exact name.
 *
 * On success (return 0) type_out, ctype_out, and value_out are heap-allocated;
 * the caller must free() each non-NULL pointer.
 * ctype_out is set to NULL when the stored ctype is NULL or empty.
 *
 * @return 0 if found, 1 if not found, -1 on error.
 */
int psm_sqlite_get(const char *name,
                   char      **type_out,
                   char      **ctype_out,
                   char      **value_out);

/**
 * psm_sqlite_set - insert or replace a record.
 *
 * Skips the write if the current stored value is byte-identical to the
 * supplied value (RDKB-24884 write-no-op optimisation).
 *
 * @param ctype  May be NULL for records with no content type.
 * @param value  May be NULL.
 * @return 0 on success, -1 on error.
 */
int psm_sqlite_set(const char *name,
                   const char *type,
                   const char *ctype,
                   const char *value);

/**
 * psm_sqlite_delete - remove a record by name.
 *
 * Succeeds even when the record does not exist.
 * @return 0 on success, -1 on error.
 */
int psm_sqlite_delete(const char *name);

/**
 * psm_sqlite_get_names - list record names matching a prefix.
 *
 * When next_level is non-zero only names without an additional '.' after
 * the prefix are returned (direct-children semantics).
 *
 * On success names_out is a heap-allocated array of heap-allocated strings;
 * the caller must free() each string and then the array.
 *
 * @return 0 on success, -1 on error.
 */
int psm_sqlite_get_names(const char  *prefix,
                         int          next_level,
                         char      ***names_out,
                         int         *count_out);

/**
 * psm_sqlite_clear - delete every record from the database.
 *
 * Used by factory reset and ImportConfig.
 * @return 0 on success, -1 on error.
 */
int psm_sqlite_clear(void);

/**
 * Transaction wrappers for bulk operations (e.g., migration).
 * Return 0 on success, -1 on error.
 */
int psm_sqlite_begin(void);
int psm_sqlite_commit(void);
int psm_sqlite_rollback(void);

/**
 * psm_sqlite_export_xml - serialise all records to an XML buffer.
 *
 * Produces the same format used by the legacy XML config files:
 *   <?xml version="1.0" encoding="UTF-8" ?>
 *   <Provision>
 *     <Record name="..." type="..." [contentType="..."]>value</Record>
 *     ...
 *   </Provision>
 *
 * The caller must free(*buf) after use.
 * @return 0 on success, -1 on error.
 */
int psm_sqlite_export_xml(char **buf, int *size);

#endif /* PSM_SQLITE_H */
