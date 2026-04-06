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
 * psm_api.h — PSM Serverless Library Public API
 *
 * Drop-in replacement for ccsp_psm_helper.h when PSM runs as an in-process
 * library backed by SQLite (--enable-serverless build).
 *
 * Consumer migration:
 *   1. Replace  #include <ccsp_psm_helper.h>
 *      with     #include <psm_api.h>
 *   2. Add PSM_LibInit("/nvram/psm.db") at process startup.
 *   3. Add PSM_LibDeinit() at process shutdown.
 *   4. Update link line: add -lpsm, drop -lrbus (if only used for PSM).
 *   All PSM_Get/Set/Del call sites remain unchanged (resolved by #defines below).
 */

#ifndef PSM_API_H
#define PSM_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Return codes  (0 = success; negative = error)
 * -------------------------------------------------------------------------- */
#define PSM_LIB_SUCCESS              0
#define PSM_LIB_ERR_INVALID_PARAM   -1
#define PSM_LIB_ERR_NOT_FOUND       -2
#define PSM_LIB_ERR_DB              -3
#define PSM_LIB_ERR_NOMEM           -4
#define PSM_LIB_ERR_NOT_INIT        -5

/* --------------------------------------------------------------------------
 * CCSP parameter type codes
 * These match the ccsp_base_api.h enum values used by ccsp_psm_helper callers.
 * -------------------------------------------------------------------------- */
#define PSM_PARAM_TYPE_STRING        0   /* ccsp_string   → stored as "astr"          */
#define PSM_PARAM_TYPE_INT           1   /* ccsp_int      → stored as "sint"          */
#define PSM_PARAM_TYPE_UINT          2   /* ccsp_unsignedInt → stored as "uint"       */
#define PSM_PARAM_TYPE_BOOL          3   /* ccsp_boolean  → stored as "bool"          */
#define PSM_PARAM_TYPE_DATETIME      4   /* ccsp_dateTime → stored as "astr"/datetime */
#define PSM_PARAM_TYPE_BASE64        5   /* ccsp_base64   → stored as "bstr"          */

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * PSM_LibInit - open (or create) the PSM SQLite database.
 *
 * If the database is newly created or empty this function automatically
 * migrates data using the following priority order:
 *   1. /tmp/bbhm_cur_cfg.xml       (current runtime config)
 *   2. /nvram/bbhm_bak_cfg.xml     (backup, if cur not found)
 *   3. /psm/config/psm_def_cfg.xml.gz  (factory default)
 * After loading the XML it applies partner defaults from syscfg,
 * merges missing entries from /opt/secure/bootstrap.json, and injects
 * HAL-provided WiFi factory defaults.
 *
 * @param db_path  Absolute path to the SQLite DB file, e.g. "/nvram/psm.db"
 * @return PSM_LIB_SUCCESS on success, negative error code on failure.
 */
int PSM_LibInit(const char *db_path);

/**
 * PSM_LibDeinit - flush any pending writes and close the database.
 */
void PSM_LibDeinit(void);

/* --------------------------------------------------------------------------
 * Core CRUD
 *
 * The 'bus_handle' parameter is unused; pass NULL.  It exists solely for
 * source-level compatibility with callers of ccsp_psm_helper.h.
 * -------------------------------------------------------------------------- */

/**
 * PSM_Lib_Get_Record_Value2 - retrieve a PSM record value by name.
 *
 * @param bus_handle        Unused; pass NULL.
 * @param subsystem_prefix  Prefix prepended to pRecordName (may be NULL/"").
 * @param pRecordName       Record key name.
 * @param pulRecordType     OUTPUT: CCSP type code (PSM_PARAM_TYPE_*). May be NULL.
 * @param ppRecordValue     OUTPUT: heap-allocated value string. Caller must free().
 * @return PSM_LIB_SUCCESS, PSM_LIB_ERR_NOT_FOUND, or other error code.
 */
int PSM_Lib_Get_Record_Value2(
        void          *bus_handle,
        char          *subsystem_prefix,
        char          *pRecordName,
        unsigned int  *pulRecordType,
        char         **ppRecordValue);

/**
 * PSM_Lib_Set_Record_Value2 - create or update a PSM record.
 *
 * @param bus_handle        Unused; pass NULL.
 * @param subsystem_prefix  Prefix (may be NULL/"").
 * @param pRecordName       Record key name.
 * @param ulRecordType      CCSP type code (PSM_PARAM_TYPE_*).
 * @param pRecordValue      Value string.
 * @return PSM_LIB_SUCCESS or error code.
 */
int PSM_Lib_Set_Record_Value2(
        void          *bus_handle,
        char          *subsystem_prefix,
        char          *pRecordName,
        unsigned int   ulRecordType,
        char          *pRecordValue);

/**
 * PSM_Lib_Del_Record - delete a PSM record.
 *
 * @param bus_handle        Unused; pass NULL.
 * @param subsystem_prefix  Prefix (may be NULL/"").
 * @param pRecordName       Record key name to delete.
 * @return PSM_LIB_SUCCESS or error code.
 */
int PSM_Lib_Del_Record(
        void  *bus_handle,
        char  *subsystem_prefix,
        char  *pRecordName);

/**
 * PSM_Lib_Get_Record_Names2 - list record names matching a prefix.
 *
 * @param bus_handle        Unused; pass NULL.
 * @param subsystem_prefix  Prefix (may be NULL/"").
 * @param pRecordName       Name prefix to search.
 * @param ulNextLevel       Non-zero: return only direct children
 *                          (names without an additional '.' after the prefix).
 * @param pulInstanceNumber OUTPUT: count of returned names.
 * @param ppInstanceList    OUTPUT: heap-allocated array of heap-allocated strings.
 *                          Caller must free() each string and the array.
 * @return PSM_LIB_SUCCESS or error code.
 */
int PSM_Lib_Get_Record_Names2(
        void    *bus_handle,
        char    *subsystem_prefix,
        char    *pRecordName,
        int      ulNextLevel,
        int     *pulInstanceNumber,
        char  ***ppInstanceList);

/* --------------------------------------------------------------------------
 * Extended operations
 * -------------------------------------------------------------------------- */

/**
 * PSM_Lib_FactoryReset - wipe all records and reload from the default XML.
 *
 * Calls PsmHal_RestoreFactoryDefaults() after clearing the database.
 *
 * @param def_xml_path  Path to the default XML/gz config.  Pass NULL to use
 *                      the built-in path (/psm/config/psm_def_cfg.xml.gz).
 * @return PSM_LIB_SUCCESS or error code.
 */
int PSM_Lib_FactoryReset(const char *def_xml_path);

/**
 * PSM_Lib_UpdateConfigs - apply new firmware default records to the database.
 *
 * Respects per-record 'overwrite' attributes in the new default XML:
 *   "always" - overwrite the current value unconditionally.
 *   "cond"   - overwrite only if the current value equals the OLD default.
 *   "never"  - leave the current value untouched.
 * Records present in the new defaults but absent from the database are inserted.
 *
 * @param new_def_xml_path  Path to the new firmware default XML (plain or .gz).
 * @return PSM_LIB_SUCCESS or error code.
 */
int PSM_Lib_UpdateConfigs(const char *new_def_xml_path);

/**
 * PSM_Lib_ImportConfig - import an (optionally DES-encrypted) XML config.
 *
 * If des_key is non-NULL and key_size >= 16, the buffer is DES-CBC decrypted
 * (first 8 bytes = key, next 8 bytes = IV) before parsing.
 * The XML structure is validated before the database is modified.
 *
 * @param buf       Buffer containing the XML (or encrypted XML).
 * @param size      Buffer size in bytes.
 * @param des_key   Optional DES key+IV buffer.  Pass NULL to skip decryption.
 * @param key_size  Length of des_key.
 * @return PSM_LIB_SUCCESS or error code.
 */
int PSM_Lib_ImportConfig(const char *buf, int size,
                         const char *des_key, int key_size);

/**
 * PSM_Lib_ExportConfig - serialize the database to an XML buffer.
 *
 * If des_key is non-NULL the output buffer is DES-CBC encrypted.
 * The caller must free(*buf) after use.
 *
 * @param buf       OUTPUT: heap-allocated XML (or encrypted) buffer.
 * @param size      OUTPUT: buffer size in bytes.
 * @param des_key   Optional DES key+IV.  Pass NULL to skip encryption.
 * @param key_size  Length of des_key.
 * @return PSM_LIB_SUCCESS or error code.
 */
int PSM_Lib_ExportConfig(char **buf, int *size,
                         const char *des_key, int key_size);

/**
 * PSM_Lib_SaveConfigToFlash - ensure data is durably written.
 *
 * With SQLite in WAL+NORMAL mode, data is already durable after each write.
 * This function issues PRAGMA wal_checkpoint(TRUNCATE) to merge the WAL into
 * the main database file, which is useful before a power-cycle or reboot.
 *
 * @return PSM_LIB_SUCCESS always.
 */
int PSM_Lib_SaveConfigToFlash(void);

/**
 * PSM_Lib_DisableWriting - pause or resume immediate database writes.
 *
 * When writing is disabled (internal reference count > 0), calls to
 * PSM_Lib_Set_Record_Value2 and PSM_Lib_Del_Record queue changes in memory.
 * When the reference count returns to 0 the pending queue is flushed.
 *
 * @param disable  1 = increment ref count (disable writing).
 *                 0 = decrement ref count (re-enable; flushes if count == 0).
 * @return PSM_LIB_SUCCESS or error code.
 */
int PSM_Lib_DisableWriting(int disable);

/* --------------------------------------------------------------------------
 * Source-level compatibility macros
 *
 * Callers that previously included <ccsp_psm_helper.h> and used the
 * PSM_Get_Record_Value2 / PSM_Set_Record_Value2 / PSM_Del_Record /
 * PSM_Get_Record_Names2 functions require zero call-site changes; simply
 * replace the header include with this file.
 * -------------------------------------------------------------------------- */
#define PSM_Get_Record_Value2   PSM_Lib_Get_Record_Value2
#define PSM_Set_Record_Value2   PSM_Lib_Set_Record_Value2
#define PSM_Del_Record          PSM_Lib_Del_Record
#define PSM_Get_Record_Names2   PSM_Lib_Get_Record_Names2

#ifdef __cplusplus
}
#endif

#endif /* PSM_API_H */
