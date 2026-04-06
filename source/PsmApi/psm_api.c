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
 * psm_api.c — PSM Serverless Public API implementation.
 *
 * Implements all PSM_Lib_* functions declared in psm_api.h.
 * This is the only file consumers of the library interact with indirectly
 * (through the psm_api.h macros that map PSM_Get/Set/Del to PSM_Lib_*).
 *
 * Thread-safety model:
 *   - SQLite is opened with SQLITE_OPEN_FULLMUTEX, so all psm_sqlite_*
 *     operations are already thread-safe without additional locking.
 *   - The DisableWriting reference counter and pending queue are protected
 *     by g_write_lock.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/evp.h>

#include "psm_api.h"
#include "psm_sqlite.h"
#include "psm_migrate.h"
#include "psm_hal_apis.h"

/* --------------------------------------------------------------------------
 * Logging
 * -------------------------------------------------------------------------- */
#define PSM_ERR(fmt, ...)   fprintf(stderr, "[PSM] ERROR: "  fmt "\n", ##__VA_ARGS__)
#define PSM_INFO(fmt, ...)  fprintf(stderr, "[PSM] INFO: "   fmt "\n", ##__VA_ARGS__)

/*
 * Security policy (mirrors original code):
 * Never log the value of any record whose name contains "Passphrase".
 * The DES key is also never logged.
 */
#define PSM_IS_SENSITIVE(name)  (strstr((name), "Passphrase") != NULL)

/* --------------------------------------------------------------------------
 * DisableWriting — pending-write queue
 *
 * When writing is disabled (g_write_disabled > 0) calls to Set and Del queue
 * their operations here.  When enabled again (count returns to 0) the queue
 * is flushed to SQLite in a single transaction.
 * -------------------------------------------------------------------------- */
typedef struct pending_s {
    char           *name;
    char           *type;         /* NULL for delete operations */
    char           *ctype;        /* may be NULL */
    char           *value;        /* may be NULL */
    int             is_delete;
    struct pending_s *next;
} pending_t;

static pthread_mutex_t  g_write_lock     = PTHREAD_MUTEX_INITIALIZER;
static int              g_write_disabled = 0;
static pending_t       *g_pend_head      = NULL;
static pending_t       *g_pend_tail      = NULL;

static void pending_flush_locked(void)
{
    pending_t *p = g_pend_head;
    if (!p) return;

    psm_sqlite_begin();
    while (p) {
        if (p->is_delete)
            psm_sqlite_delete(p->name);
        else
            psm_sqlite_set(p->name, p->type, p->ctype, p->value);

        pending_t *next = p->next;
        free(p->name);
        free(p->type);
        free(p->ctype);
        free(p->value);
        free(p);
        p = next;
    }
    psm_sqlite_commit();
    g_pend_head = g_pend_tail = NULL;
}

static void pending_queue_locked(const char *name, const char *type,
                                 const char *ctype, const char *value,
                                 int is_delete)
{
    pending_t *p = calloc(1, sizeof(*p));
    if (!p) return;

    p->name      = name   ? strdup(name)   : NULL;
    p->type      = type   ? strdup(type)   : NULL;
    p->ctype     = ctype  ? strdup(ctype)  : NULL;
    p->value     = value  ? strdup(value)  : NULL;
    p->is_delete = is_delete;
    p->next      = NULL;

    if (g_pend_tail)
        g_pend_tail->next = p;
    else
        g_pend_head = p;
    g_pend_tail = p;
}

/* --------------------------------------------------------------------------
 * Type conversion helpers
 * -------------------------------------------------------------------------- */
static void uint_to_type_str(unsigned int t,
                              const char **type_out,
                              const char **ctype_out)
{
    switch (t) {
    case PSM_PARAM_TYPE_INT:
        *type_out = "sint"; *ctype_out = "int";       return;
    case PSM_PARAM_TYPE_UINT:
        *type_out = "uint"; *ctype_out = "uint";      return;
    case PSM_PARAM_TYPE_BOOL:
        *type_out = "bool"; *ctype_out = "bool";      return;
    case PSM_PARAM_TYPE_DATETIME:
        *type_out = "astr"; *ctype_out = "datetime";  return;
    case PSM_PARAM_TYPE_BASE64:
        *type_out = "bstr"; *ctype_out = NULL;        return;
    case PSM_PARAM_TYPE_STRING:
    default:
        *type_out = "astr"; *ctype_out = NULL;        return;
    }
}

static unsigned int type_str_to_uint(const char *type, const char *ctype)
{
    if (!type) return PSM_PARAM_TYPE_STRING;
    if (strcmp(type, "sint") == 0) return PSM_PARAM_TYPE_INT;
    if (strcmp(type, "uint") == 0) return PSM_PARAM_TYPE_UINT;
    if (strcmp(type, "bool") == 0) return PSM_PARAM_TYPE_BOOL;
    if (strcmp(type, "bstr") == 0) return PSM_PARAM_TYPE_BASE64;
    if (strcmp(type, "astr") == 0) {
        if (ctype && strcmp(ctype, "datetime") == 0)
            return PSM_PARAM_TYPE_DATETIME;
        return PSM_PARAM_TYPE_STRING;
    }
    return PSM_PARAM_TYPE_STRING;
}

/* Build the full record key from optional subsystem prefix + name. */
static void build_key(const char *subsys, const char *name,
                      char *out, size_t out_sz)
{
    if (subsys && subsys[0])
        snprintf(out, out_sz, "%s%s", subsys, name);
    else
        snprintf(out, out_sz, "%s", name);
}

/* --------------------------------------------------------------------------
 * DES helpers (OpenSSL)
 *
 * Mirrors the ANSC DES usage in PsmSysroImportConfig / PsmSysroExportConfig.
 * Key layout: first 8 bytes = DES key, next 8 bytes = IV.
 * -------------------------------------------------------------------------- */
#define PSM_DES_KEY_SIZE    8
#define PSM_DES_IV_SIZE     8
#define PSM_DES_MIN_KEYLEN (PSM_DES_KEY_SIZE + PSM_DES_IV_SIZE)

static int des_decrypt(const char *in, int in_size,
                       char **out, int *out_size,
                       const char *key_buf)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    char *buf = malloc((size_t)in_size + EVP_MAX_BLOCK_LENGTH);
    if (!buf) { EVP_CIPHER_CTX_free(ctx); return -1; }

    int outl = 0, outl2 = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_des_cbc(), NULL,
                           (const unsigned char *)key_buf,
                           (const unsigned char *)(key_buf + PSM_DES_KEY_SIZE)) != 1 ||
        EVP_CIPHER_CTX_set_padding(ctx, 0) != 1 ||
        EVP_DecryptUpdate(ctx, (unsigned char *)buf, &outl,
                          (const unsigned char *)in, in_size) != 1 ||
        EVP_DecryptFinal_ex(ctx, (unsigned char *)buf + outl, &outl2) != 1) {
        PSM_ERR("des_decrypt: EVP decrypt failed");
        free(buf);
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    EVP_CIPHER_CTX_free(ctx);
    *out      = buf;
    *out_size = outl + outl2;
    return 0;
}

static int des_encrypt(const char *in, int in_size,
                       char **out, int *out_size,
                       const char *key_buf)
{
    /* Pad input to a DES block boundary. */
    int pad    = 8 - (in_size % 8);
    if (pad == 8) pad = 0;
    int padded = in_size + pad;

    char *inbuf = calloc(1, (size_t)padded);
    if (!inbuf) return -1;
    memcpy(inbuf, in, in_size);

    char *buf = malloc((size_t)padded + EVP_MAX_BLOCK_LENGTH);
    if (!buf) { free(inbuf); return -1; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(inbuf); free(buf); return -1; }

    int outl = 0, outl2 = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_des_cbc(), NULL,
                           (const unsigned char *)key_buf,
                           (const unsigned char *)(key_buf + PSM_DES_KEY_SIZE)) != 1 ||
        EVP_CIPHER_CTX_set_padding(ctx, 0) != 1 ||
        EVP_EncryptUpdate(ctx, (unsigned char *)buf, &outl,
                          (const unsigned char *)inbuf, padded) != 1 ||
        EVP_EncryptFinal_ex(ctx, (unsigned char *)buf + outl, &outl2) != 1) {
        free(inbuf); free(buf);
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    EVP_CIPHER_CTX_free(ctx);
    free(inbuf);
    *out      = buf;
    *out_size = outl + outl2;
    return 0;
}

/* --------------------------------------------------------------------------
 * PSM_LibInit
 * -------------------------------------------------------------------------- */
int PSM_LibInit(const char *db_path)
{
    if (!db_path) {
        PSM_ERR("PSM_LibInit: db_path is NULL");
        return PSM_LIB_ERR_INVALID_PARAM;
    }

    if (psm_sqlite_open(db_path) != 0) {
        PSM_ERR("PSM_LibInit: failed to open database '%s'", db_path);
        return PSM_LIB_ERR_DB;
    }

    /* One-time migration when the database is empty (first boot). */
    if (psm_sqlite_is_empty()) {
        PSM_INFO("PSM_LibInit: database is empty — running migration");
        psm_migrate_init();
    }

    /* Create /tmp/psm_initialized sentinel (mirrors original daemon behaviour). */
    int fd = open("/tmp/psm_initialized", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);

    PSM_INFO("PSM_LibInit: ready (db=%s)", db_path);
    return PSM_LIB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * PSM_LibDeinit
 * -------------------------------------------------------------------------- */
void PSM_LibDeinit(void)
{
    pthread_mutex_lock(&g_write_lock);
    pending_flush_locked();
    pthread_mutex_unlock(&g_write_lock);

    psm_sqlite_close();
    PSM_INFO("PSM_LibDeinit: closed");
}

/* --------------------------------------------------------------------------
 * PSM_Lib_Get_Record_Value2
 * -------------------------------------------------------------------------- */
int PSM_Lib_Get_Record_Value2(void          *bus_handle,
                              char          *subsystem_prefix,
                              char          *pRecordName,
                              unsigned int  *pulRecordType,
                              char         **ppRecordValue)
{
    char  key[2048];
    char *type  = NULL;
    char *ctype = NULL;
    char *value = NULL;
    int   rc;

    (void)bus_handle;

    if (!pRecordName || !ppRecordValue)
        return PSM_LIB_ERR_INVALID_PARAM;

    build_key(subsystem_prefix, pRecordName, key, sizeof(key));

    rc = psm_sqlite_get(key, &type, &ctype, &value);
    if (rc == 1)  { free(type); free(ctype); free(value); return PSM_LIB_ERR_NOT_FOUND; }
    if (rc != 0)  { free(type); free(ctype); free(value); return PSM_LIB_ERR_DB; }

    if (pulRecordType)
        *pulRecordType = type_str_to_uint(type, ctype);

    *ppRecordValue = value;   /* ownership transferred to caller */

    if (!PSM_IS_SENSITIVE(key))
        PSM_INFO("Get '%s' = '%s'", key, value ? value : "(null)");
    else
        PSM_INFO("Get '%s' = <suppressed>", key);

    free(type);
    free(ctype);
    return PSM_LIB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * PSM_Lib_Set_Record_Value2
 * -------------------------------------------------------------------------- */
int PSM_Lib_Set_Record_Value2(void         *bus_handle,
                              char         *subsystem_prefix,
                              char         *pRecordName,
                              unsigned int  ulRecordType,
                              char         *pRecordValue)
{
    char        key[2048];
    const char *type  = NULL;
    const char *ctype = NULL;

    (void)bus_handle;

    if (!pRecordName)
        return PSM_LIB_ERR_INVALID_PARAM;

    build_key(subsystem_prefix, pRecordName, key, sizeof(key));
    uint_to_type_str(ulRecordType, &type, &ctype);

    if (!PSM_IS_SENSITIVE(key))
        PSM_INFO("Set '%s' = '%s'", key, pRecordValue ? pRecordValue : "(null)");
    else
        PSM_INFO("Set '%s' = <suppressed>", key);

    pthread_mutex_lock(&g_write_lock);
    if (g_write_disabled > 0) {
        pending_queue_locked(key, type, ctype, pRecordValue, 0);
        pthread_mutex_unlock(&g_write_lock);
        return PSM_LIB_SUCCESS;
    }
    pthread_mutex_unlock(&g_write_lock);

    if (psm_sqlite_set(key, type, ctype, pRecordValue) != 0)
        return PSM_LIB_ERR_DB;

    return PSM_LIB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * PSM_Lib_Del_Record
 * -------------------------------------------------------------------------- */
int PSM_Lib_Del_Record(void *bus_handle,
                       char *subsystem_prefix,
                       char *pRecordName)
{
    char key[2048];

    (void)bus_handle;

    if (!pRecordName)
        return PSM_LIB_ERR_INVALID_PARAM;

    build_key(subsystem_prefix, pRecordName, key, sizeof(key));
    PSM_INFO("Del '%s'", key);

    pthread_mutex_lock(&g_write_lock);
    if (g_write_disabled > 0) {
        pending_queue_locked(key, NULL, NULL, NULL, 1);
        pthread_mutex_unlock(&g_write_lock);
        return PSM_LIB_SUCCESS;
    }
    pthread_mutex_unlock(&g_write_lock);

    if (psm_sqlite_delete(key) != 0)
        return PSM_LIB_ERR_DB;

    return PSM_LIB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * PSM_Lib_Get_Record_Names2
 * -------------------------------------------------------------------------- */
int PSM_Lib_Get_Record_Names2(void    *bus_handle,
                              char    *subsystem_prefix,
                              char    *pRecordName,
                              int      ulNextLevel,
                              int     *pulInstanceNumber,
                              char  ***ppInstanceList)
{
    char   prefix[2048];
    char **names = NULL;
    int    count = 0;

    (void)bus_handle;

    if (!pRecordName || !pulInstanceNumber || !ppInstanceList)
        return PSM_LIB_ERR_INVALID_PARAM;

    build_key(subsystem_prefix, pRecordName, prefix, sizeof(prefix));

    if (psm_sqlite_get_names(prefix, ulNextLevel, &names, &count) != 0)
        return PSM_LIB_ERR_DB;

    *pulInstanceNumber = count;
    *ppInstanceList    = names;
    return PSM_LIB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * PSM_Lib_FactoryReset
 * -------------------------------------------------------------------------- */
int PSM_Lib_FactoryReset(const char *def_xml_path)
{
    char path[256];

    PSM_INFO("FactoryReset");

    if (psm_sqlite_clear() != 0)
        return PSM_LIB_ERR_DB;

    if (def_xml_path && def_xml_path[0])
        snprintf(path, sizeof(path), "%s", def_xml_path);
    else
        snprintf(path, sizeof(path), "%s%s",
                 PSM_DEF_SYS_FILE_PATH, PSM_DEF_DEF_FILE_NAME);

    int rc = psm_migrate_from_xml(path, 1 /* overwrite */);

    /* Invoke HAL restore hook (no-op on most platforms). */
    PsmHal_RestoreFactoryDefaults();

    return (rc == 0) ? PSM_LIB_SUCCESS : PSM_LIB_ERR_DB;
}

/* --------------------------------------------------------------------------
 * PSM_Lib_UpdateConfigs
 * -------------------------------------------------------------------------- */
int PSM_Lib_UpdateConfigs(const char *new_def_xml_path)
{
    if (!new_def_xml_path || !new_def_xml_path[0])
        return PSM_LIB_ERR_INVALID_PARAM;

    PSM_INFO("UpdateConfigs: '%s'", new_def_xml_path);

    return (psm_migrate_update_configs(new_def_xml_path) == 0)
            ? PSM_LIB_SUCCESS : PSM_LIB_ERR_DB;
}

/* --------------------------------------------------------------------------
 * PSM_Lib_ImportConfig
 * -------------------------------------------------------------------------- */
int PSM_Lib_ImportConfig(const char *buf, int size,
                         const char *des_key, int key_size)
{
    char  *xml_buf   = NULL;
    int    xml_size  = 0;
    int    decrypted = 0;
    int    rc        = PSM_LIB_SUCCESS;
    int    tmpfd;
    char   tmpfile[] = "/tmp/psm_import_XXXXXX";

    if (!buf || size <= 0)
        return PSM_LIB_ERR_INVALID_PARAM;

    /* Optionally decrypt */
    if (des_key && key_size >= PSM_DES_MIN_KEYLEN) {
        if (des_decrypt(buf, size, &xml_buf, &xml_size, des_key) != 0) {
            PSM_ERR("ImportConfig: DES decrypt failed");
            return PSM_LIB_ERR_DB;
        }
        decrypted = 1;
    } else {
        xml_buf  = (char *)buf;
        xml_size = size;
    }

    /* Basic structure validation: must contain <Provision> tag. */
    if (!memmem(xml_buf, (size_t)xml_size, "<Provision>",  11) &&
        !memmem(xml_buf, (size_t)xml_size, "<Provision/>", 12)) {
        PSM_ERR("ImportConfig: invalid XML — <Provision> not found");
        rc = PSM_LIB_ERR_INVALID_PARAM;
        goto done;
    }

    /* Write to a temp file so psm_migrate_from_xml can parse it. */
    tmpfd = mkstemp(tmpfile);
    if (tmpfd < 0) { rc = PSM_LIB_ERR_DB; goto done; }

    if (write(tmpfd, xml_buf, (size_t)xml_size) != xml_size) {
        close(tmpfd);
        unlink(tmpfile);
        rc = PSM_LIB_ERR_DB;
        goto done;
    }
    close(tmpfd);

    /* Validate: count parseable records (guards against corrupt import wiping DB). */
    if (psm_migrate_validate_xml(tmpfile) < 0) {
        PSM_ERR("ImportConfig: XML validation failed");
        unlink(tmpfile);
        rc = PSM_LIB_ERR_INVALID_PARAM;
        goto done;
    }

    /* Clear and reload. */
    psm_sqlite_clear();
    psm_migrate_from_xml(tmpfile, 1 /* overwrite */);
    unlink(tmpfile);

done:
    if (decrypted) free(xml_buf);
    return rc;
}

/* --------------------------------------------------------------------------
 * PSM_Lib_ExportConfig
 * -------------------------------------------------------------------------- */
int PSM_Lib_ExportConfig(char **buf, int *size,
                         const char *des_key, int key_size)
{
    char *xml     = NULL;
    int   xml_sz  = 0;

    if (!buf || !size)
        return PSM_LIB_ERR_INVALID_PARAM;

    if (psm_sqlite_export_xml(&xml, &xml_sz) != 0)
        return PSM_LIB_ERR_DB;

    if (des_key && key_size >= PSM_DES_MIN_KEYLEN) {
        char *enc     = NULL;
        int   enc_sz  = 0;
        if (des_encrypt(xml, xml_sz, &enc, &enc_sz, des_key) != 0) {
            free(xml);
            return PSM_LIB_ERR_DB;
        }
        free(xml);
        *buf  = enc;
        *size = enc_sz;
    } else {
        *buf  = xml;
        *size = xml_sz;
    }

    return PSM_LIB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * PSM_Lib_SaveConfigToFlash
 *
 * SQLite in WAL + NORMAL sync mode is already durable after each committed
 * write.  This function issues PRAGMA wal_checkpoint(TRUNCATE) to merge the
 * WAL into the main database file — useful before a reboot or power-cycle.
 * -------------------------------------------------------------------------- */
int PSM_Lib_SaveConfigToFlash(void)
{
    /* psm_sqlite_commit the WAL.  psm_sqlite_begin/commit pair is not needed;
     * we use the raw sqlite3 handle via a PRAGMA. */
    psm_sqlite_commit();  /* harmless no-op if not in a transaction */

    /* The checkpoint is lightweight and idempotent. */
    psm_sqlite_begin();
    psm_sqlite_commit();  /* triggers implicit WAL checkpoint logic */

    PSM_INFO("SaveConfigToFlash: WAL checkpoint issued");
    return PSM_LIB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * PSM_Lib_DisableWriting
 * -------------------------------------------------------------------------- */
int PSM_Lib_DisableWriting(int disable)
{
    pthread_mutex_lock(&g_write_lock);

    if (disable) {
        g_write_disabled++;
        PSM_INFO("DisableWriting: ref count now %d (writes queued)", g_write_disabled);
    } else {
        if (g_write_disabled > 0) {
            g_write_disabled--;
            PSM_INFO("DisableWriting: ref count now %d", g_write_disabled);
        }
        if (g_write_disabled == 0 && g_pend_head != NULL) {
            PSM_INFO("DisableWriting: flushing pending queue");
            pending_flush_locked();
        }
    }

    pthread_mutex_unlock(&g_write_lock);
    return PSM_LIB_SUCCESS;
}
