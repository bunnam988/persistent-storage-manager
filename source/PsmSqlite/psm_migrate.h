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
 * psm_migrate.h — PSM migration engine (internal header).
 *
 * Handles:
 *   - One-time XML → SQLite bootstrap on first boot (empty database)
 *   - Partner default parameters from syscfg
 *   - bootstrap.json partner param merge
 *   - HAL custom WiFi param injection
 *   - Firmware upgrade config merge (always / cond / never)
 */

#ifndef PSM_MIGRATE_H
#define PSM_MIGRATE_H

/* Default factory config file location (mirrors psm_properties.h values,
 * but without pulling in the ANSC framework headers). */
#ifndef PSM_DEF_SYS_FILE_PATH
#define PSM_DEF_SYS_FILE_PATH   "/psm/config/"
#endif
#ifndef PSM_DEF_DEF_FILE_NAME
#define PSM_DEF_DEF_FILE_NAME   "psm_def_cfg.xml.gz"
#endif

/**
 * psm_migrate_init - run the full first-boot migration sequence.
 *
 * Called from PSM_LibInit() when the database is empty.  What it does:
 *   1. psm_migrate_from_xml()       – XML file 3-tier fallback (cur→bak→def)
 *   2. Fill any gaps from the default XML (INSERT OR IGNORE)
 *   3. migrate_partner_params_syscfg() – consume syscfg keys, insert overwriting
 *   4. migrate_bootstrap_json()      – /opt/secure/bootstrap.json merge
 *   5. PsmHal_GetCustomParams()      – inject HAL WiFi factory defaults
 *
 * @return 0 on success, -1 on failure.
 */
int psm_migrate_init(void);

/**
 * psm_migrate_from_xml - parse an XML config file and insert records.
 *
 * Supports both plain XML and gzip-compressed (.gz) files.
 * When overwrite == 0, existing database records are preserved (INSERT OR IGNORE).
 * When overwrite == 1, existing database records are replaced (INSERT OR REPLACE).
 *
 * @param xml_path  Absolute path to the XML or .gz file.
 * @param overwrite 0 = keep existing, 1 = replace existing.
 * @return 0 on success, -1 if the file could not be opened/read.
 */
int psm_migrate_from_xml(const char *xml_path, int overwrite);

/**
 * psm_migrate_validate_xml - count valid Record elements in an XML file.
 *
 * Used by ImportConfig to verify the buffer before wiping the database.
 * Returns the number of successfully parsed Record elements (>= 0), or
 * -1 if the file could not be opened.
 */
int psm_migrate_validate_xml(const char *xml_path);

/**
 * psm_migrate_update_configs - firmware-upgrade record merge.
 *
 * Applies records from new_def_xml_path to the current database, respecting
 * per-record 'overwrite' attributes ("always", "cond", "never").
 * Records present in the new defaults but absent from the database are added.
 * After merging, the standard default XML path is updated with the new defaults.
 *
 * @param new_def_xml_path  Path to the new firmware default XML (plain or .gz).
 * @return 0 on success, -1 on failure.
 */
int psm_migrate_update_configs(const char *new_def_xml_path);

#endif /* PSM_MIGRATE_H */
