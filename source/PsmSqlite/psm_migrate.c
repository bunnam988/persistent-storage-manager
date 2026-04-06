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
 * psm_migrate.c — PSM migration engine.
 *
 * This file ports the XML-loading, partner-defaults, and firmware-upgrade merge
 * logic originally spread across ssp_cfmif.c into the serverless library.
 *
 * Key entry points:
 *   psm_migrate_init()          – full first-boot migration sequence
 *   psm_migrate_from_xml()      – XML / .gz file → SQLite
 *   psm_migrate_validate_xml()  – count valid records without inserting
 *   psm_migrate_update_configs()– firmware-upgrade "always/cond/never" merge
 */

#define _GNU_SOURCE  /* for strndup, strchrnul */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>
#include <syscfg/syscfg.h>
#include <cJSON.h>

#include "psm_sqlite.h"
#include "psm_migrate.h"
#include "psm_hal_apis.h"
#include "psm_properties.h"

/* --------------------------------------------------------------------------
 * Logging
 * -------------------------------------------------------------------------- */
#define MIG_DBG(fmt, ...)  fprintf(stderr, "[PSM-Migrate] " fmt "\n", ##__VA_ARGS__)
#define MIG_ERR(fmt, ...)  fprintf(stderr, "[PSM-Migrate] ERROR: " fmt "\n", ##__VA_ARGS__)

/* --------------------------------------------------------------------------
 * Well-known paths (mirrors ssp_cfmif.c constants)
 * -------------------------------------------------------------------------- */
#define PSM_CUR_XML_PATH    "/tmp/bbhm_cur_cfg.xml"
#define PSM_BAK_XML_PATH    "/nvram/bbhm_bak_cfg.xml"

#define PARTNER_APPLY_FILE           "/nvram/.apply_partner_defaults"
#define PARTNER_MIGRATE_PSM          "/tmp/.apply_partner_defaults_psm"
#define PARTNER_MIGRATE_NEW_MEMBER   "/tmp/.apply_partner_defaults_new_psm_member"
#define BOOTSTRAP_INFO_FILE          "/opt/secure/bootstrap.json"

/* --------------------------------------------------------------------------
 * Overwrite mode enum (from new-firmware-defaults XML attribute)
 * -------------------------------------------------------------------------- */
typedef enum {
    OW_ALWAYS,
    OW_COND,
    OW_NEVER,
} psm_overwrite_t;

/* --------------------------------------------------------------------------
 * Internal parsed Record representation
 * -------------------------------------------------------------------------- */
typedef struct {
    char            name[2048];
    char            type[64];
    char            ctype[64];
    char            value[2048];
    psm_overwrite_t overwrite;
} psm_xml_rec_t;

/* --------------------------------------------------------------------------
 * strip_quotes — remove leading/trailing double-quotes in place
 * -------------------------------------------------------------------------- */
static char *strip_quotes(char *s)
{
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        return s + 1;
    }
    return s;
}

/* --------------------------------------------------------------------------
 * parse_record_line
 *
 * Parse a single XML text line that may contain a <Record .../> or
 * <Record ...>value</Record> element.  Extracts name, type, contentType,
 * overwrite, and value into *rec.
 *
 * Returns 0 on success, -1 if the line does not contain a valid Record.
 * -------------------------------------------------------------------------- */
static int parse_record_line(const char *line, psm_xml_rec_t *rec)
{
    char  work[4096];
    char *t_start, *t_end, *v_start;
    char *tok, *delim, *sp;

    snprintf(work, sizeof(work), "%s", line);

    if ((t_start = strstr(work, "<Record")) == NULL)
        return -1;
    t_start += strlen("<Record");

    if ((t_end = strchr(t_start, '>')) == NULL)
        return -1;

    *t_end = '\0';

    if (t_end > work && t_end[-1] == '/') {
        /* Self-closing: <Record .../> — no value */
        t_end[-1] = '\0';
        v_start   = NULL;
    } else {
        v_start = t_end + 1;
        char *v_end = strstr(v_start, "</Record>");
        if (v_end == NULL)
            return -1;
        *v_end = '\0';
    }

    /* Initialise the record with safe defaults. */
    memset(rec, 0, sizeof(*rec));
    rec->overwrite = OW_NEVER;

    delim = " \t\r\n";
    for (; (tok = strtok_r(t_start, delim, &sp)) != NULL; t_start = NULL) {
        if (strncmp(tok, "name=", 5) == 0) {
            char *v = strip_quotes(tok + 5);
            snprintf(rec->name, sizeof(rec->name), "%s", v);
        } else if (strncmp(tok, "type=", 5) == 0) {
            char *v = strip_quotes(tok + 5);
            snprintf(rec->type, sizeof(rec->type), "%s", v);
        } else if (strncmp(tok, "contentType=", 12) == 0) {
            char *v = strip_quotes(tok + 12);
            snprintf(rec->ctype, sizeof(rec->ctype), "%s", v);
        } else if (strncmp(tok, "overwrite=", 10) == 0) {
            char *v = strip_quotes(tok + 10);
            if (strcmp(v, "always") == 0)
                rec->overwrite = OW_ALWAYS;
            else if (strcmp(v, "cond") == 0)
                rec->overwrite = OW_COND;
            else
                rec->overwrite = OW_NEVER;
        }
    }

    if (rec->name[0] == '\0' || rec->type[0] == '\0')
        return -1;

    if (v_start)
        snprintf(rec->value, sizeof(rec->value), "%s", v_start);

    return 0;
}

/* --------------------------------------------------------------------------
 * Callback type used by parse_xml_file
 * -------------------------------------------------------------------------- */
typedef void (*rec_cb_t)(const psm_xml_rec_t *rec, void *ctx);

/* --------------------------------------------------------------------------
 * parse_xml_file
 *
 * Open an XML (plain or .gz) file and call callback for each valid Record.
 * Returns 0 on success, -1 if the file could not be opened.
 * -------------------------------------------------------------------------- */
static int parse_xml_file(const char *path, rec_cb_t callback, void *ctx)
{
    char        line[4096];
    psm_xml_rec_t rec;
    int         is_gz = 0;
    const char *ext   = strrchr(path, '.');

    if (ext && strcmp(ext, ".gz") == 0)
        is_gz = 1;

    if (is_gz) {
        gzFile gz = gzopen(path, "rb");
        if (!gz) {
            MIG_ERR("gzopen '%s' failed", path);
            return -1;
        }
        while (gzgets(gz, line, sizeof(line)) != NULL) {
            if (parse_record_line(line, &rec) == 0)
                callback(&rec, ctx);
        }
        gzclose(gz);
    } else {
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            MIG_ERR("fopen '%s' failed", path);
            return -1;
        }
        while (fgets(line, sizeof(line), fp) != NULL) {
            if (parse_record_line(line, &rec) == 0)
                callback(&rec, ctx);
        }
        fclose(fp);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Callback context for psm_migrate_from_xml
 * -------------------------------------------------------------------------- */
typedef struct {
    int overwrite;
    int count;
} insert_ctx_t;

static void insert_cb(const psm_xml_rec_t *rec, void *ctx)
{
    insert_ctx_t *c = (insert_ctx_t *)ctx;

    if (c->overwrite) {
        psm_sqlite_set(rec->name,
                       rec->type,
                       rec->ctype[0] ? rec->ctype : NULL,
                       rec->value);
    } else {
        /* INSERT OR IGNORE: only write when the key is absent. */
        char *existing = NULL;
        if (psm_sqlite_get(rec->name, NULL, NULL, &existing) != 0) {
            psm_sqlite_set(rec->name,
                           rec->type,
                           rec->ctype[0] ? rec->ctype : NULL,
                           rec->value);
        }
        free(existing);
    }
    c->count++;
}

/* --------------------------------------------------------------------------
 * psm_migrate_from_xml (public)
 * -------------------------------------------------------------------------- */
int psm_migrate_from_xml(const char *xml_path, int overwrite)
{
    insert_ctx_t ctx = { .overwrite = overwrite, .count = 0 };
    int rc;

    if (!xml_path || access(xml_path, F_OK) != 0) {
        MIG_DBG("'%s' not accessible, skipping", xml_path ? xml_path : "(null)");
        return -1;
    }

    MIG_DBG("migrating '%s' (overwrite=%d)", xml_path, overwrite);

    psm_sqlite_begin();
    rc = parse_xml_file(xml_path, insert_cb, &ctx);
    if (rc == 0)
        psm_sqlite_commit();
    else
        psm_sqlite_rollback();

    MIG_DBG("migrated %d records from '%s'", ctx.count, xml_path);
    return rc;
}

/* --------------------------------------------------------------------------
 * Callback context for psm_migrate_validate_xml
 * -------------------------------------------------------------------------- */
static void count_cb(const psm_xml_rec_t *rec, void *ctx)
{
    (void)rec;
    (*(int *)ctx)++;
}

/* --------------------------------------------------------------------------
 * psm_migrate_validate_xml (public)
 * -------------------------------------------------------------------------- */
int psm_migrate_validate_xml(const char *xml_path)
{
    int count = 0;

    if (!xml_path || access(xml_path, F_OK) != 0)
        return -1;

    parse_xml_file(xml_path, count_cb, &count);
    return count;
}

/* --------------------------------------------------------------------------
 * migrate_partner_params_syscfg
 *
 * Ports Psm_GetCustomPartnersParams() + import_custom_partners_params() from
 * ssp_cfmif.c.  Reads syscfg keys, maps them to PSM record names, inserts
 * overwriting any existing values, then unsets the consumed syscfg keys.
 * -------------------------------------------------------------------------- */
static void migrate_partner_params_syscfg(void)
{
    char value_buf[128] = {0};
    int  ret;

/* Helper macro: read one syscfg key, insert into SQLite overwriting, unset. */
#define PARTNER_KEY(skey, psm_name) do {                                           \
    memset(value_buf, 0, sizeof(value_buf));                                       \
    ret = syscfg_get(NULL, (skey), value_buf, sizeof(value_buf));                  \
    if (ret == 0 && value_buf[0] != '\0') {                                        \
        psm_sqlite_set((psm_name), "astr", NULL, value_buf);                       \
        syscfg_unset(NULL, (skey));                                                \
        MIG_DBG("partner param: '%s' = '%s'", (psm_name), value_buf);             \
    }                                                                              \
} while (0)

/* Helper for bool-valued keys that need case conversion (true→TRUE). */
#define PARTNER_BOOL_KEY(skey, psm_name) do {                                      \
    memset(value_buf, 0, sizeof(value_buf));                                       \
    ret = syscfg_get(NULL, (skey), value_buf, sizeof(value_buf));                  \
    if (ret == 0 && value_buf[0] != '\0') {                                        \
        const char *bv = (strncmp(value_buf,"true",4)==0) ? "TRUE" : "FALSE";      \
        psm_sqlite_set((psm_name), "astr", NULL, bv);                              \
        syscfg_unset(NULL, (skey));                                                \
        MIG_DBG("partner bool: '%s' = '%s'", (psm_name), bv);                     \
    }                                                                              \
} while (0)

    PARTNER_KEY("WiFiRegionCode",
        "eRT.com.cisco.spvtg.ccsp.tr181pa.Device.WiFi."
        "X_RDKCENTRAL-COM_Syndication.WiFiRegion.Code");
    PARTNER_KEY("TR69CertLocation",
        "dmsb.device.deviceinfo.X_RDKCENTRAL-COM_Syndication.TR69CertLocation");
    PARTNER_KEY("XHS_SSIDprefix",
        "Device.DeviceInfo.X_RDKCENTRAL-COM_Syndication.HomeSec.SSIDprefix");
    PARTNER_KEY("lan_ipaddr",     "dmsb.l3net.4.V4Addr");
    PARTNER_KEY("lan_netmask",    "dmsb.l3net.4.V4SubnetMask");
    PARTNER_KEY("WEBCONFIG_INIT_URL",   "Device.X_RDK_WebConfig.URL");
    PARTNER_KEY("TELEMETRY_INIT_URL",
        "Device.X_RDK_WebConfig.SupplementaryServiceUrls.Telemetry");
    PARTNER_KEY("MQTT_INIT_URL",         "Device.X_RDK_MQTT.BrokerURL");
    PARTNER_KEY("MQTT_INIT_LOCATIONID",  "Device.X_RDK_MQTT.LocationID");
    PARTNER_KEY("MQTT_INIT_PORT",        "Device.X_RDK_MQTT.Port");
    PARTNER_KEY("WEBPA_SERVER_URL",   "Device.X_RDKCENTRAL-COM_Webpa.Server.URL");
    PARTNER_KEY("TOKEN_SERVER_URL",   "Device.X_RDKCENTRAL-COM_Webpa.TokenServer.URL");
    PARTNER_KEY("DNS_TEXT_URL",       "Device.X_RDKCENTRAL-COM_Webpa.DNSText.URL");

#if defined(_RDKB_GLOBAL_PRODUCT_REQ_)
    /* LAN IPv6 ULA — two PSM params share the same syscfg key */
    memset(value_buf, 0, sizeof(value_buf));
    ret = syscfg_get(NULL, "LANULASupport", value_buf, sizeof(value_buf));
    if (ret == 0 && value_buf[0] != '\0') {
        const char *bv = (strncmp(value_buf, "true", 4) == 0) ? "TRUE" : "FALSE";
        psm_sqlite_set("dmsb.lanmanagemententry.lanulaenable",  "astr", NULL, bv);
        psm_sqlite_set("dmsb.lanmanagemententry.lanipv6enable", "astr", NULL, bv);
        MIG_DBG("partner bool: lanulaenable/lanipv6enable = '%s'", bv);
        /* NOTE: LANULASupport is intentionally NOT unset here; matches original. */
    }

    PARTNER_BOOL_KEY("BackupWanDnsSupport",
        "dmsb.wanmanager.BackupWanDnsSupport");
    PARTNER_BOOL_KEY("IPv6EUI64FormatSupport",
        "dmsb.wanmanager.IPv6EUI64FormatSupport");
    PARTNER_BOOL_KEY("ConfigureWANIPv6OnLANBridgeSupport",
        "dmsb.wanmanager.ConfigureWANIPv6OnLANBridgeSupport");
    PARTNER_BOOL_KEY("UseWANMACForManagementServices",
        "dmsb.wanmanager.UseWANMACForManagementServices");
    PARTNER_KEY("ConnectivityCheckType",
        "dmsb.wanmanager.if.1.VirtualInterface.1.IP.ConnectivityCheckType");
    PARTNER_BOOL_KEY("InterfaceVLANMarkingSupport",
        "dmsb.wanmanager.InterfaceVLANMarkingSupport");
#endif /* _RDKB_GLOBAL_PRODUCT_REQ_ */

    syscfg_commit();

#undef PARTNER_KEY
#undef PARTNER_BOOL_KEY
}

/* --------------------------------------------------------------------------
 * Partner param table — mirrors parm_present_table[] from ssp_cfmif.c.
 * Used to detect which params are missing before reading bootstrap.json.
 * -------------------------------------------------------------------------- */
typedef struct {
    const char *psm_name;
    const char *partner_name;   /* JSON key under the PartnerID object */
} partner_param_t;

static const partner_param_t s_partner_tbl[] = {
    { "dmsb.device.deviceinfo.X_RDKCENTRAL-COM_Syndication.TR69CertLocation",
      "Device.DeviceInfo.X_RDKCENTRAL-COM_Syndication.TR69CertLocation" },
    { "eRT.com.cisco.spvtg.ccsp.tr181pa.Device.WiFi."
      "X_RDKCENTRAL-COM_Syndication.WiFiRegion.Code",
      "Device.WiFi.X_RDKCENTRAL-COM_Syndication.WiFiRegion.Code" },
    { "Device.DeviceInfo.X_RDKCENTRAL-COM_Syndication.HomeSec.SSIDprefix",
      "Device.DeviceInfo.X_RDKCENTRAL-COM_Syndication.HomeSec.SSIDprefix" },
    { "Device.X_RDK_WebConfig.URL",
      "Device.X_RDK_WebConfig.URL" },
    { "Device.X_RDK_WebConfig.SupplementaryServiceUrls.Telemetry",
      "Device.X_RDK_WebConfig.SupplementaryServiceUrls.Telemetry" },
    { "Device.X_RDK_MQTT.BrokerURL",
      "Device.X_RDK_MQTT.BrokerURL" },
    { "Device.X_RDK_MQTT.LocationID",
      "Device.X_RDK_MQTT.LocationID" },
    { "Device.X_RDK_MQTT.Port",
      "Device.X_RDK_MQTT.Port" },
    { "dmsb.l3net.4.V4Addr",
      "Device.DeviceInfo.X_RDKCENTRAL-COM_Syndication."
      "RDKB_UIBranding.DefaultAdminIP" },
    { "dmsb.l3net.4.V4SubnetMask",
      "Device.DeviceInfo.X_RDKCENTRAL-COM_Syndication."
      "RDKB_UIBranding.DefaultLocalIPv4SubnetRange" },
    { "Device.X_RDKCENTRAL-COM_Webpa.Server.URL",
      "Device.X_RDKCENTRAL-COM_Webpa.Server.URL" },
    { "Device.X_RDKCENTRAL-COM_Webpa.TokenServer.URL",
      "Device.X_RDKCENTRAL-COM_Webpa.TokenServer.URL" },
    { "Device.X_RDKCENTRAL-COM_Webpa.DNSText.URL",
      "Device.X_RDKCENTRAL-COM_Webpa.DNSText.URL" },
#if defined(_RDKB_GLOBAL_PRODUCT_REQ_)
    { "dmsb.lanmanagemententry.lanulaenable",
      "Device.X_RDK_Features.LANIPv6ULA" },
    { "dmsb.lanmanagemententry.lanipv6enable",
      "Device.X_RDK_Features.LANIPv6ULA" },
    { "dmsb.wanmanager.BackupWanDnsSupport",
      "Device.X_RDK_Features.BackupWanDns" },
    { "dmsb.wanmanager.IPv6EUI64FormatSupport",
      "Device.X_RDK_Features.IPv6EUI64FormatSupport" },
    { "dmsb.wanmanager.ConfigureWANIPv6OnLANBridgeSupport",
      "Device.X_RDK_Features.ConfigureWANIPv6OnLANBridgeSupport" },
    { "dmsb.wanmanager.UseWANMACForManagementServices",
      "Device.X_RDK_Features.UseWANMACForManagementServices" },
    { "dmsb.wanmanager.if.1.VirtualInterface.1.IP.ConnectivityCheckType",
      "Device.X_RDK_Features.WANConnectivityCheckType" },
    { "dmsb.wanmanager.InterfaceVLANMarkingSupport",
      "Device.X_RDK_Features.InterfaceVLANMarkingSupport" },
#endif
};
#define PARTNER_TBL_SZ  (sizeof(s_partner_tbl) / sizeof(s_partner_tbl[0]))

/* --------------------------------------------------------------------------
 * migrate_bootstrap_json
 *
 * Ports merge_missing_Partner_params() from ssp_cfmif.c.
 * For any partner param that is missing from SQLite, look it up in
 * /opt/secure/bootstrap.json under the active PartnerID and insert it
 * (no overwrite of records that already exist).
 * -------------------------------------------------------------------------- */
static void migrate_bootstrap_json(void)
{
    FILE   *fp;
    long    len;
    char   *data       = NULL;
    cJSON  *root       = NULL;
    char    partner_id[64] = {0};
    unsigned int i;
    int    *present;
    int     any_missing = 0;

    /* Build presence map. */
    present = calloc(PARTNER_TBL_SZ, sizeof(int));
    if (!present)
        return;

    for (i = 0; i < PARTNER_TBL_SZ; i++) {
        char *val = NULL;
        if (psm_sqlite_get(s_partner_tbl[i].psm_name, NULL, NULL, &val) == 0) {
            present[i] = 1;
            free(val);
        } else {
            any_missing = 1;
        }
    }

    if (!any_missing) {
        free(present);
        return;
    }

    /* Load bootstrap.json */
    fp = fopen(BOOTSTRAP_INFO_FILE, "r");
    if (!fp) {
        MIG_DBG("'%s' not found, skipping bootstrap merge", BOOTSTRAP_INFO_FILE);
        free(present);
        return;
    }

    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len > 0) {
        data = malloc((size_t)len + 1);
        if (!data) { fclose(fp); free(present); return; }
        if ((long)fread(data, 1, (size_t)len, fp) != len) {
            MIG_ERR("fread bootstrap.json truncated");
            fclose(fp); free(data); free(present);
            return;
        }
        data[len] = '\0';
    }
    fclose(fp);

    if (!data) { free(present); return; }

    root = cJSON_Parse(data);
    free(data);
    if (!root) {
        MIG_ERR("cJSON_Parse bootstrap.json failed");
        free(present);
        return;
    }

    syscfg_get(NULL, "PartnerID", partner_id, sizeof(partner_id));

    cJSON *partner_obj = cJSON_GetObjectItem(root, partner_id);
    if (partner_obj) {
        for (i = 0; i < PARTNER_TBL_SZ; i++) {
            if (present[i])
                continue;  /* already in DB */

            cJSON *param_obj = cJSON_GetObjectItem(partner_obj,
                                                   s_partner_tbl[i].partner_name);
            if (!param_obj) continue;

            cJSON *active_val = cJSON_GetObjectItem(param_obj, "ActiveValue");
            if (!active_val || !active_val->valuestring) continue;

            /* Insert only when not already present (no overwrite). */
            char *existing = NULL;
            if (psm_sqlite_get(s_partner_tbl[i].psm_name,
                               NULL, NULL, &existing) != 0) {
                psm_sqlite_set(s_partner_tbl[i].psm_name, "astr", NULL,
                               active_val->valuestring);
                MIG_DBG("bootstrap: inserted '%s'", s_partner_tbl[i].psm_name);
            }
            free(existing);
        }
    }

    cJSON_Delete(root);
    free(present);
}

/* --------------------------------------------------------------------------
 * psm_migrate_init (public)
 * -------------------------------------------------------------------------- */
int psm_migrate_init(void)
{
    char def_path[256];
    int  migrated = 0;

    /*
     * 3-tier fallback: cur → bak → def
     * Use INSERT OR IGNORE (overwrite=0) so later steps can fill gaps.
     */
    if (access(PSM_CUR_XML_PATH, F_OK) == 0) {
        if (psm_migrate_from_xml(PSM_CUR_XML_PATH, 0) == 0)
            migrated = 1;
    }
    if (!migrated && access(PSM_BAK_XML_PATH, F_OK) == 0) {
        if (psm_migrate_from_xml(PSM_BAK_XML_PATH, 0) == 0)
            migrated = 1;
    }

    /* Always fill any gaps from the factory default. */
    snprintf(def_path, sizeof(def_path), "%s%s",
             PSM_DEF_SYS_FILE_PATH, PSM_DEF_DEF_FILE_NAME);
    psm_migrate_from_xml(def_path, 0);   /* INSERT OR IGNORE */

    /*
     * Partner / syscfg migration:
     * triggered by sentinel files or when there is no cur/bak XML at all
     * (genuine first boot).
     */
    int need_partner = 0;

    if (access(PARTNER_MIGRATE_PSM, F_OK) == 0) {
        need_partner = 1;
        MIG_DBG("deleting sentinel %s", PARTNER_MIGRATE_PSM);
        unlink(PARTNER_MIGRATE_PSM);
    }
    if (access(PARTNER_MIGRATE_NEW_MEMBER, F_OK) == 0) {
        need_partner = 1;
        MIG_DBG("deleting sentinel %s", PARTNER_MIGRATE_NEW_MEMBER);
        unlink(PARTNER_MIGRATE_NEW_MEMBER);
    }
    if (!migrated)
        need_partner = 1;  /* first boot — no cur/bak existed */

    if (need_partner) {
        if (access(PARTNER_APPLY_FILE, F_OK) == 0) {
            MIG_DBG("deleting sentinel %s", PARTNER_APPLY_FILE);
            unlink(PARTNER_APPLY_FILE);
        }
        migrate_partner_params_syscfg();
    }

    /* Bootstrap.json partner param merge (fills gaps only). */
    migrate_bootstrap_json();

    /* HAL custom WiFi SSID/passphrase defaults (INSERT OR IGNORE). */
    PsmHalParam_t *hal_params = NULL;
    int            hal_cnt    = 0;

    if (PsmHal_GetCustomParams(&hal_params, &hal_cnt) == 0
            && hal_params != NULL && hal_cnt > 0) {
        psm_sqlite_begin();
        for (int i = 0; i < hal_cnt; i++) {
            if (!hal_params[i].name || !hal_params[i].name[0])
                continue;
            char *existing = NULL;
            if (psm_sqlite_get(hal_params[i].name, NULL, NULL, &existing) != 0) {
                psm_sqlite_set(hal_params[i].name, "astr", NULL,
                               hal_params[i].value);
            }
            free(existing);
        }
        psm_sqlite_commit();
        free(hal_params);
    }

    return 0;
}

/* ==========================================================================
 * psm_migrate_update_configs — firmware-upgrade record merge
 * ==========================================================================
 *
 * Ports ssp_CfmUpdateConfigs() without the ANSC XML DOM dependency.
 *
 * Algorithm:
 *   1. Load OLD defaults into a lightweight in-memory hash map.
 *   2. Parse NEW defaults line by line.
 *   3. For each new record apply the overwrite attribute:
 *        always → always update SQLite
 *        cond   → update only if current DB value equals old default
 *        never  → leave untouched
 *        absent → insert the new default value
 *   4. Copy new defaults to the standard default-config path so that the
 *      next firmware upgrade can use them as the "old" baseline.
 * ========================================================================== */

/* --- Simple hash map to hold old-default name→value pairs --- */
#define OD_BUCKETS  512u

typedef struct od_entry_s {
    char              *name;
    char              *value;
    struct od_entry_s *next;
} od_entry_t;

static od_entry_t *od_map[OD_BUCKETS];

static unsigned int od_hash(const char *s)
{
    unsigned int h = 5381u;
    int c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + (unsigned int)c;
    return h % OD_BUCKETS;
}

static void od_insert(const char *name, const char *value)
{
    unsigned int  idx = od_hash(name);
    od_entry_t   *e   = malloc(sizeof(*e));
    if (!e) return;
    e->name  = strdup(name);
    e->value = value ? strdup(value) : strdup("");
    e->next  = od_map[idx];
    od_map[idx] = e;
}

static const char *od_lookup(const char *name)
{
    unsigned int idx = od_hash(name);
    for (od_entry_t *e = od_map[idx]; e; e = e->next)
        if (strcmp(e->name, name) == 0)
            return e->value;
    return NULL;
}

static void od_clear(void)
{
    for (unsigned int i = 0; i < OD_BUCKETS; i++) {
        od_entry_t *e = od_map[i];
        while (e) {
            od_entry_t *nx = e->next;
            free(e->name);
            free(e->value);
            free(e);
            e = nx;
        }
        od_map[i] = NULL;
    }
}

/* Callback that fills od_map from a parsed XML file. */
static void load_od_cb(const psm_xml_rec_t *rec, void *ctx)
{
    (void)ctx;
    od_insert(rec->name, rec->value);
}

/* --------------------------------------------------------------------------
 * copy_file — copy src to dst (plain binary copy, handles both .gz and plain)
 * -------------------------------------------------------------------------- */
static int copy_file(const char *src, const char *dst)
{
    FILE  *fsrc = fopen(src, "rb");
    FILE  *fdst = fopen(dst, "wb");
    char   buf[4096];
    size_t nr;
    int    ok = 0;

    if (!fsrc || !fdst)
        goto out;

    while ((nr = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        if (fwrite(buf, 1, nr, fdst) != nr)
            goto out;
    }
    ok = 1;

out:
    if (fsrc) fclose(fsrc);
    if (fdst) fclose(fdst);
    return ok ? 0 : -1;
}

/* --------------------------------------------------------------------------
 * psm_migrate_update_configs (public)
 * -------------------------------------------------------------------------- */
int psm_migrate_update_configs(const char *new_def_xml_path)
{
    char  def_path[256];
    int   is_gz = 0;
    const char *ext;
    int   rc    = 0;

    if (!new_def_xml_path || !new_def_xml_path[0]) {
        MIG_ERR("update_configs: new_def_xml_path is NULL or empty");
        return -1;
    }

    /* Detect compression. */
    ext   = strrchr(new_def_xml_path, '.');
    is_gz = (ext && strcmp(ext, ".gz") == 0);

    /* Step 1: Load OLD defaults into od_map for "cond" comparisons. */
    memset(od_map, 0, sizeof(od_map));
    snprintf(def_path, sizeof(def_path), "%s%s",
             PSM_DEF_SYS_FILE_PATH, PSM_DEF_DEF_FILE_NAME);
    parse_xml_file(def_path, load_od_cb, NULL);

    /* Step 2 & 3: Parse NEW defaults and apply to the database. */
    char  line[4096];
    FILE *fp = NULL;
    gzFile gz = NULL;

    if (is_gz) {
        gz = gzopen(new_def_xml_path, "rb");
        if (!gz) { MIG_ERR("gzopen '%s'", new_def_xml_path); rc = -1; goto done; }
    } else {
        fp = fopen(new_def_xml_path, "rb");
        if (!fp) { MIG_ERR("fopen '%s'", new_def_xml_path); rc = -1; goto done; }
    }

    psm_sqlite_begin();

    while (is_gz ? (gzgets(gz, line, sizeof(line)) != NULL)
                 : (fgets(line, sizeof(line), fp)   != NULL)) {

        psm_xml_rec_t rec;
        if (parse_record_line(line, &rec) != 0)
            continue;
        if (rec.name[0] == '\0')
            continue;

        const char *ctype_arg = rec.ctype[0] ? rec.ctype : NULL;
        char       *cur_val   = NULL;
        int         in_db     = psm_sqlite_get(rec.name, NULL, NULL, &cur_val);

        if (in_db != 0) {
            /* Not in DB — always insert the new default. */
            psm_sqlite_set(rec.name, rec.type, ctype_arg, rec.value);
        } else {
            switch (rec.overwrite) {
            case OW_ALWAYS:
                psm_sqlite_set(rec.name, rec.type, ctype_arg, rec.value);
                break;

            case OW_COND: {
                /* Overwrite only if current value equals the OLD default. */
                const char *old_val = od_lookup(rec.name);
                if (old_val && cur_val && strcmp(cur_val, old_val) == 0)
                    psm_sqlite_set(rec.name, rec.type, ctype_arg, rec.value);
                break;
            }

            case OW_NEVER:
            default:
                /* Leave current value untouched. */
                break;
            }
        }
        free(cur_val);
    }

    psm_sqlite_commit();

    /* Step 4: Persist new defaults as the standard default for next upgrade. */
    /* We save the new default XML to the well-known default path so that
     * the next firmware update can use it as the "old" baseline for OW_COND. */
    if (is_gz) {
        /* Decompress new .gz defaults to the plain default path. */
        gzFile  gz2 = gzopen(new_def_xml_path, "rb");
        FILE   *dst = fopen(def_path, "wb");
        if (gz2 && dst) {
            char buf[4096];
            int  n;
            while ((n = gzread(gz2, buf, sizeof(buf))) > 0)
                fwrite(buf, 1, (size_t)n, dst);
        }
        if (gz2) gzclose(gz2);
        if (dst)  fclose(dst);
    } else {
        copy_file(new_def_xml_path, def_path);
    }

done:
    if (gz) gzclose(gz);
    if (fp) fclose(fp);
    od_clear();
    return rc;
}
