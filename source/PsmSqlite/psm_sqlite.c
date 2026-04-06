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
 * psm_sqlite.c — PSM SQLite storage engine.
 *
 * Provides a thread-safe (SQLITE_OPEN_FULLMUTEX), persistent key-value store
 * for PSM parameters.  Uses WAL journal mode for concurrent multi-process
 * access without additional application-level locking.
 *
 * Schema:
 *   CREATE TABLE psm_records (
 *     name   TEXT PRIMARY KEY NOT NULL,
 *     type   TEXT NOT NULL DEFAULT 'astr',
 *     ctype  TEXT,
 *     value  TEXT
 *   );
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "psm_sqlite.h"

/* --------------------------------------------------------------------------
 * Module-level state
 * -------------------------------------------------------------------------- */
static sqlite3      *g_db          = NULL;
static sqlite3_stmt *g_stmt_get    = NULL;
static sqlite3_stmt *g_stmt_set    = NULL;
static sqlite3_stmt *g_stmt_del    = NULL;
static sqlite3_stmt *g_stmt_clear  = NULL;
static sqlite3_stmt *g_stmt_count  = NULL;

#define PSM_DB_LOG_ERR(fmt, ...)  \
    fprintf(stderr, "[PSM-SQLite] ERROR %s:%d: " fmt "\n", \
            __FUNCTION__, __LINE__, ##__VA_ARGS__)

/* --------------------------------------------------------------------------
 * DDL
 * -------------------------------------------------------------------------- */
#define CREATE_TABLE_SQL \
    "CREATE TABLE IF NOT EXISTS psm_records (" \
    "  name   TEXT PRIMARY KEY NOT NULL,"       \
    "  type   TEXT NOT NULL DEFAULT 'astr',"    \
    "  ctype  TEXT,"                            \
    "  value  TEXT"                             \
    ");"

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */
static int prepare_statements(void)
{
    if (sqlite3_prepare_v2(g_db,
            "SELECT type, ctype, value FROM psm_records WHERE name = ?1;",
            -1, &g_stmt_get, NULL) != SQLITE_OK) {
        PSM_DB_LOG_ERR("prepare get: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    if (sqlite3_prepare_v2(g_db,
            "INSERT OR REPLACE INTO psm_records (name, type, ctype, value)"
            " VALUES (?1, ?2, ?3, ?4);",
            -1, &g_stmt_set, NULL) != SQLITE_OK) {
        PSM_DB_LOG_ERR("prepare set: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    if (sqlite3_prepare_v2(g_db,
            "DELETE FROM psm_records WHERE name = ?1;",
            -1, &g_stmt_del, NULL) != SQLITE_OK) {
        PSM_DB_LOG_ERR("prepare del: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    if (sqlite3_prepare_v2(g_db,
            "DELETE FROM psm_records;",
            -1, &g_stmt_clear, NULL) != SQLITE_OK) {
        PSM_DB_LOG_ERR("prepare clear: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    if (sqlite3_prepare_v2(g_db,
            "SELECT COUNT(*) FROM psm_records;",
            -1, &g_stmt_count, NULL) != SQLITE_OK) {
        PSM_DB_LOG_ERR("prepare count: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    return 0;
}

static void finalize_statements(void)
{
    sqlite3_finalize(g_stmt_get);   g_stmt_get   = NULL;
    sqlite3_finalize(g_stmt_set);   g_stmt_set   = NULL;
    sqlite3_finalize(g_stmt_del);   g_stmt_del   = NULL;
    sqlite3_finalize(g_stmt_clear); g_stmt_clear = NULL;
    sqlite3_finalize(g_stmt_count); g_stmt_count = NULL;
}

/* --------------------------------------------------------------------------
 * psm_sqlite_open
 * -------------------------------------------------------------------------- */
int psm_sqlite_open(const char *db_path)
{
    int is_new;
    int rc;

    if (db_path == NULL) {
        PSM_DB_LOG_ERR("db_path is NULL");
        return -1;
    }

    /* Detect whether we are creating a new file (for chmod below). */
    is_new = (access(db_path, F_OK) != 0);

    rc = sqlite3_open_v2(db_path, &g_db,
                         SQLITE_OPEN_FULLMUTEX  |
                         SQLITE_OPEN_CREATE     |
                         SQLITE_OPEN_READWRITE,
                         NULL);
    if (rc != SQLITE_OK) {
        PSM_DB_LOG_ERR("open '%s': %s", db_path, sqlite3_errmsg(g_db));
        sqlite3_close_v2(g_db);
        g_db = NULL;
        return -1;
    }

    /* Restrict file permissions on newly created databases.
     * This protects stored secrets (WiFi passphrases, etc.). */
    if (is_new)
        chmod(db_path, S_IRUSR | S_IWUSR);  /* 0600 */

    /* WAL mode: safe concurrent access from multiple processes. */
    if (sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;",
                     NULL, NULL, NULL) != SQLITE_OK)
        PSM_DB_LOG_ERR("PRAGMA journal_mode=WAL: %s", sqlite3_errmsg(g_db));

    /* NORMAL sync: durable with WAL, better performance than FULL. */
    if (sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;",
                     NULL, NULL, NULL) != SQLITE_OK)
        PSM_DB_LOG_ERR("PRAGMA synchronous=NORMAL: %s", sqlite3_errmsg(g_db));

    /* Create the records table. */
    if (sqlite3_exec(g_db, CREATE_TABLE_SQL,
                     NULL, NULL, NULL) != SQLITE_OK) {
        PSM_DB_LOG_ERR("CREATE TABLE: %s", sqlite3_errmsg(g_db));
        sqlite3_close_v2(g_db);
        g_db = NULL;
        return -1;
    }

    if (prepare_statements() != 0) {
        finalize_statements();
        sqlite3_close_v2(g_db);
        g_db = NULL;
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * psm_sqlite_close
 * -------------------------------------------------------------------------- */
void psm_sqlite_close(void)
{
    if (g_db == NULL)
        return;
    finalize_statements();
    sqlite3_close_v2(g_db);
    g_db = NULL;
}

/* --------------------------------------------------------------------------
 * psm_sqlite_is_empty
 * -------------------------------------------------------------------------- */
int psm_sqlite_is_empty(void)
{
    int count = 0;

    if (g_db == NULL || g_stmt_count == NULL)
        return 1;

    sqlite3_reset(g_stmt_count);
    if (sqlite3_step(g_stmt_count) == SQLITE_ROW)
        count = sqlite3_column_int(g_stmt_count, 0);
    sqlite3_reset(g_stmt_count);

    return (count == 0) ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * psm_sqlite_get
 * -------------------------------------------------------------------------- */
int psm_sqlite_get(const char *name,
                   char      **type_out,
                   char      **ctype_out,
                   char      **value_out)
{
    int         rc;
    const char *col;

    if (g_db == NULL || g_stmt_get == NULL || name == NULL)
        return -1;

    if (type_out)  *type_out  = NULL;
    if (ctype_out) *ctype_out = NULL;
    if (value_out) *value_out = NULL;

    sqlite3_reset(g_stmt_get);
    sqlite3_bind_text(g_stmt_get, 1, name, -1, SQLITE_STATIC);

    rc = sqlite3_step(g_stmt_get);
    if (rc == SQLITE_ROW) {
        if (type_out) {
            col = (const char *)sqlite3_column_text(g_stmt_get, 0);
            *type_out = strdup(col ? col : "astr");
        }
        if (ctype_out) {
            col = (const char *)sqlite3_column_text(g_stmt_get, 1);
            *ctype_out = (col && col[0]) ? strdup(col) : NULL;
        }
        if (value_out) {
            col = (const char *)sqlite3_column_text(g_stmt_get, 2);
            *value_out = strdup(col ? col : "");
        }
        sqlite3_reset(g_stmt_get);
        return 0; /* found */
    }

    sqlite3_reset(g_stmt_get);
    return (rc == SQLITE_DONE) ? 1 : -1; /* 1 = not found, -1 = error */
}

/* --------------------------------------------------------------------------
 * psm_sqlite_set
 * -------------------------------------------------------------------------- */
int psm_sqlite_set(const char *name,
                   const char *type,
                   const char *ctype,
                   const char *value)
{
    char *cur_value = NULL;
    int   write_needed = 1;
    int   rc;

    if (g_db == NULL || g_stmt_set == NULL || name == NULL || type == NULL)
        return -1;

    /* RDKB-24884 optimisation: skip the write when the value is unchanged. */
    if (psm_sqlite_get(name, NULL, NULL, &cur_value) == 0) {
        if (cur_value && value && (strcmp(cur_value, value) == 0))
            write_needed = 0;
        free(cur_value);
    }

    if (!write_needed)
        return 0;

    sqlite3_reset(g_stmt_set);
    sqlite3_bind_text(g_stmt_set, 1, name,  -1, SQLITE_STATIC);
    sqlite3_bind_text(g_stmt_set, 2, type,  -1, SQLITE_STATIC);

    if (ctype && ctype[0])
        sqlite3_bind_text(g_stmt_set, 3, ctype, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(g_stmt_set, 3);

    if (value)
        sqlite3_bind_text(g_stmt_set, 4, value, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(g_stmt_set, 4);

    rc = sqlite3_step(g_stmt_set);
    sqlite3_reset(g_stmt_set);

    if (rc != SQLITE_DONE) {
        PSM_DB_LOG_ERR("set '%s': %s", name, sqlite3_errmsg(g_db));
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * psm_sqlite_delete
 * -------------------------------------------------------------------------- */
int psm_sqlite_delete(const char *name)
{
    int rc;

    if (g_db == NULL || g_stmt_del == NULL || name == NULL)
        return -1;

    sqlite3_reset(g_stmt_del);
    sqlite3_bind_text(g_stmt_del, 1, name, -1, SQLITE_STATIC);
    rc = sqlite3_step(g_stmt_del);
    sqlite3_reset(g_stmt_del);

    if (rc != SQLITE_DONE) {
        PSM_DB_LOG_ERR("delete '%s': %s", name, sqlite3_errmsg(g_db));
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * psm_sqlite_get_names
 * -------------------------------------------------------------------------- */
int psm_sqlite_get_names(const char  *prefix,
                         int          next_level,
                         char      ***names_out,
                         int         *count_out)
{
    sqlite3_stmt *stmt      = NULL;
    char        **names     = NULL;
    int           count     = 0;
    int           cap       = 64;
    char          pattern[2048];
    int           prefix_len;

    if (g_db == NULL || names_out == NULL || count_out == NULL)
        return -1;

    *names_out = NULL;
    *count_out = 0;

    prefix_len = prefix ? (int)strlen(prefix) : 0;

    /* Build LIKE pattern: escape '%' and '_' in the prefix itself,
     * then append the wildcard. */
    if (prefix_len > 0) {
        int i, j = 0;
        char safe[1024] = {0};
        for (i = 0; i < prefix_len && j < (int)sizeof(safe) - 2; i++) {
            if (prefix[i] == '%' || prefix[i] == '_' || prefix[i] == '\\')
                safe[j++] = '\\';
            safe[j++] = prefix[i];
        }
        safe[j] = '\0';
        snprintf(pattern, sizeof(pattern), "%s%%", safe);
    } else {
        snprintf(pattern, sizeof(pattern), "%%");
    }

    if (sqlite3_prepare_v2(g_db,
            "SELECT name FROM psm_records"
            " WHERE name LIKE ?1 ESCAPE '\\' ORDER BY name;",
            -1, &stmt, NULL) != SQLITE_OK) {
        PSM_DB_LOG_ERR("get_names prepare: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);

    names = malloc(cap * sizeof(char *));
    if (!names) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(stmt, 0);
        if (!n)
            continue;

        /* Direct-children filter: drop names that contain '.' after prefix. */
        if (next_level && prefix_len > 0) {
            const char *rest = n + prefix_len;
            if (strchr(rest, '.') != NULL)
                continue;
        }

        /* Grow the output array if needed. */
        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(names, cap * sizeof(char *));
            if (!tmp) {
                for (int i = 0; i < count; i++) free(names[i]);
                free(names);
                sqlite3_finalize(stmt);
                return -1;
            }
            names = tmp;
        }

        names[count] = strdup(n);
        if (!names[count]) {
            for (int i = 0; i < count; i++) free(names[i]);
            free(names);
            sqlite3_finalize(stmt);
            return -1;
        }
        count++;
    }

    sqlite3_finalize(stmt);

    *names_out = names;
    *count_out = count;
    return 0;
}

/* --------------------------------------------------------------------------
 * psm_sqlite_clear
 * -------------------------------------------------------------------------- */
int psm_sqlite_clear(void)
{
    int rc;

    if (g_db == NULL || g_stmt_clear == NULL)
        return -1;

    sqlite3_reset(g_stmt_clear);
    rc = sqlite3_step(g_stmt_clear);
    sqlite3_reset(g_stmt_clear);

    if (rc != SQLITE_DONE) {
        PSM_DB_LOG_ERR("clear: %s", sqlite3_errmsg(g_db));
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Transaction helpers
 * -------------------------------------------------------------------------- */
int psm_sqlite_begin(void)
{
    if (g_db == NULL) return -1;
    return (sqlite3_exec(g_db, "BEGIN;", NULL, NULL, NULL) == SQLITE_OK) ? 0 : -1;
}

int psm_sqlite_commit(void)
{
    if (g_db == NULL) return -1;
    return (sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK) ? 0 : -1;
}

int psm_sqlite_rollback(void)
{
    if (g_db == NULL) return -1;
    return (sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL) == SQLITE_OK) ? 0 : -1;
}

/* --------------------------------------------------------------------------
 * psm_sqlite_export_xml
 * -------------------------------------------------------------------------- */
int psm_sqlite_export_xml(char **buf, int *size)
{
    sqlite3_stmt *stmt = NULL;
    char         *out  = NULL;
    int           cap  = 128 * 1024;
    int           off  = 0;
    int           n;

    if (g_db == NULL || buf == NULL || size == NULL)
        return -1;

    out = malloc(cap);
    if (!out)
        return -1;

    n    = snprintf(out, cap,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
                    "<Provision>\n");
    off += n;

    if (sqlite3_prepare_v2(g_db,
            "SELECT name, type, ctype, value FROM psm_records ORDER BY name;",
            -1, &stmt, NULL) != SQLITE_OK) {
        PSM_DB_LOG_ERR("export_xml prepare: %s", sqlite3_errmsg(g_db));
        free(out);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *rec_name  = (const char *)sqlite3_column_text(stmt, 0);
        const char *rec_type  = (const char *)sqlite3_column_text(stmt, 1);
        const char *rec_ctype = (const char *)sqlite3_column_text(stmt, 2);
        const char *rec_value = (const char *)sqlite3_column_text(stmt, 3);

        if (!rec_name || !rec_type)
            continue;
        if (!rec_value)
            rec_value = "";

        /* Grow the buffer if the upcoming record might overflow. */
        int needed = (int)(strlen(rec_name) + strlen(rec_type) +
                           (rec_ctype ? strlen(rec_ctype) : 0) +
                           strlen(rec_value) + 128);
        if (off + needed >= cap) {
            cap += needed + 32 * 1024;
            char *tmp = realloc(out, cap);
            if (!tmp) {
                free(out);
                sqlite3_finalize(stmt);
                return -1;
            }
            out = tmp;
        }

        if (rec_ctype && rec_ctype[0])
            n = snprintf(out + off, cap - off,
                         "  <Record name=\"%s\" type=\"%s\""
                         " contentType=\"%s\">%s</Record>\n",
                         rec_name, rec_type, rec_ctype, rec_value);
        else
            n = snprintf(out + off, cap - off,
                         "  <Record name=\"%s\" type=\"%s\">%s</Record>\n",
                         rec_name, rec_type, rec_value);
        off += n;
    }

    sqlite3_finalize(stmt);

    /* Ensure there is room for the closing tag. */
    if (off + 32 >= cap) {
        char *tmp = realloc(out, cap + 32);
        if (!tmp) { free(out); return -1; }
        out = tmp;
        cap += 32;
    }
    n    = snprintf(out + off, cap - off, "</Provision>\n");
    off += n;

    *buf  = out;
    *size = off;
    return 0;
}
