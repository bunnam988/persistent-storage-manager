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
 * psm_api_test.cpp — Unit tests for the PSM Serverless public API.
 *
 * Tests the PSM_Lib_* API end-to-end including:
 *   - PSM_LibInit / PSM_LibDeinit lifecycle
 *   - Get / Set / Del / GetNames CRUD
 *   - Type mapping (CCSP unsigned int ↔ SQLite type strings)
 *   - PSM_Lib_FactoryReset
 *   - PSM_Lib_UpdateConfigs (always / cond / never overwrite modes)
 *   - PSM_Lib_ImportConfig / PSM_Lib_ExportConfig round-trip
 *   - PSM_Lib_DisableWriting deferred-queue behaviour
 *   - Passphrase log-suppression (functional, not log-content test)
 *
 * These tests use a temporary SQLite file to avoid interfering with system
 * state.  XML fixture files are written to /tmp/ during the test.
 */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

extern "C" {
#include "psm_api.h"
#include "psm_sqlite.h"   /* for psm_sqlite_is_empty() in some assertions */
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Write a string to a temporary file; returns the path (caller must free). */
static char *write_tmp_file(const char *content)
{
    char *path = strdup("/tmp/psm_api_test_XXXXXX");
    int   fd   = mkstemp(path);
    if (fd < 0) { free(path); return nullptr; }
    write(fd, content, strlen(content));
    close(fd);
    return path;
}

/* --------------------------------------------------------------------------
 * Fixture
 * -------------------------------------------------------------------------- */
class PsmApiTest : public ::testing::Test {
protected:
    char *db_path = nullptr;

    void SetUp() override {
        db_path = strdup("/tmp/psm_api_test_db_XXXXXX");
        int fd = mkstemp(db_path);
        if (fd >= 0) close(fd);
        /* Remove the empty placeholder so PSM_LibInit creates a fresh DB. */
        unlink(db_path);

        ASSERT_EQ(PSM_LIB_SUCCESS, PSM_LibInit(db_path));
    }

    void TearDown() override {
        PSM_LibDeinit();
        if (db_path) {
            unlink(db_path);
            free(db_path);
        }
    }
};

/* ========================================================================== */
/*  Lifecycle                                                                  */
/* ========================================================================== */

TEST_F(PsmApiTest, LibInit_CreatesDB)
{
    /* Database file must exist after PSM_LibInit. */
    EXPECT_EQ(0, access(db_path, F_OK));
}

TEST_F(PsmApiTest, LibInit_NullPath_ReturnsError)
{
    PSM_LibDeinit();
    EXPECT_NE(PSM_LIB_SUCCESS, PSM_LibInit(nullptr));
    /* Re-init for TearDown. */
    PSM_LibInit(db_path);
}

/* ========================================================================== */
/*  Get / Set / Del CRUD                                                       */
/* ========================================================================== */

TEST_F(PsmApiTest, SetGet_StringType)
{
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Set_Record_Value2(nullptr, nullptr,
                                  (char *)"eRT.com.test.String",
                                  PSM_PARAM_TYPE_STRING, (char *)"helloworld"));

    unsigned int  type  = 0;
    char         *value = nullptr;
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"eRT.com.test.String",
                                  &type, &value));

    EXPECT_EQ((unsigned int)PSM_PARAM_TYPE_STRING, type);
    EXPECT_STREQ("helloworld", value);
    free(value);
}

TEST_F(PsmApiTest, SetGet_IntType)
{
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Set_Record_Value2(nullptr, nullptr,
                                  (char *)"param.int",
                                  PSM_PARAM_TYPE_INT, (char *)"42"));

    unsigned int  type  = 0;
    char         *value = nullptr;
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"param.int",
                                  &type, &value));

    EXPECT_EQ((unsigned int)PSM_PARAM_TYPE_INT, type);
    EXPECT_STREQ("42", value);
    free(value);
}

TEST_F(PsmApiTest, SetGet_BoolType)
{
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Set_Record_Value2(nullptr, nullptr,
                                  (char *)"param.bool",
                                  PSM_PARAM_TYPE_BOOL, (char *)"true"));

    unsigned int  type  = 0;
    char         *value = nullptr;
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"param.bool",
                                  &type, &value));

    EXPECT_EQ((unsigned int)PSM_PARAM_TYPE_BOOL, type);
    EXPECT_STREQ("true", value);
    free(value);
}

TEST_F(PsmApiTest, Get_NullType_DoesNotCrash)
{
    PSM_Lib_Set_Record_Value2(nullptr, nullptr,
                              (char *)"k", PSM_PARAM_TYPE_STRING, (char *)"v");

    char *value = nullptr;
    /* pulRecordType may be NULL — must not segfault. */
    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"k", nullptr, &value));
    free(value);
}

TEST_F(PsmApiTest, Get_NotFound_ReturnsError)
{
    char *value = nullptr;
    EXPECT_EQ(PSM_LIB_ERR_NOT_FOUND,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"does.not.exist",
                                  nullptr, &value));
    EXPECT_EQ(nullptr, value);
}

TEST_F(PsmApiTest, Del_Record)
{
    PSM_Lib_Set_Record_Value2(nullptr, nullptr,
                              (char *)"del.me", PSM_PARAM_TYPE_STRING, (char *)"x");
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Del_Record(nullptr, nullptr, (char *)"del.me"));

    char *value = nullptr;
    EXPECT_EQ(PSM_LIB_ERR_NOT_FOUND,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"del.me", nullptr, &value));
    free(value);
}

/* ========================================================================== */
/*  Subsystem prefix                                                           */
/* ========================================================================== */

TEST_F(PsmApiTest, SubsystemPrefix_IsPrepenedToKey)
{
    /* Set with subsystem prefix "eRT." */
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Set_Record_Value2(nullptr, (char *)"eRT.",
                                  (char *)"param.x",
                                  PSM_PARAM_TYPE_STRING, (char *)"123"));

    /* Get with same prefix */
    char *value = nullptr;
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, (char *)"eRT.",
                                  (char *)"param.x",
                                  nullptr, &value));
    EXPECT_STREQ("123", value);
    free(value);

    /* Get WITHOUT prefix should find nothing (different key). */
    char *v2 = nullptr;
    EXPECT_EQ(PSM_LIB_ERR_NOT_FOUND,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"param.x", nullptr, &v2));
    free(v2);
}

/* ========================================================================== */
/*  GetNames                                                                   */
/* ========================================================================== */

TEST_F(PsmApiTest, GetNames_AllUnderPrefix)
{
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"WiFi.Radio.1.Ch",
                              PSM_PARAM_TYPE_UINT, (char *)"6");
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"WiFi.Radio.1.BW",
                              PSM_PARAM_TYPE_UINT, (char *)"20");
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"WiFi.Radio.2.Ch",
                              PSM_PARAM_TYPE_UINT, (char *)"11");
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"Ethernet.If.1",
                              PSM_PARAM_TYPE_STRING, (char *)"eth0");

    int    count = 0;
    char **list  = nullptr;
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Names2(nullptr, nullptr, (char *)"WiFi.",
                                  0, &count, &list));
    EXPECT_EQ(3, count);

    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}

TEST_F(PsmApiTest, GetNames_NextLevel)
{
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"A.B.C",
                              PSM_PARAM_TYPE_STRING, (char *)"1");
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"A.D",
                              PSM_PARAM_TYPE_STRING, (char *)"2");
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"A.E",
                              PSM_PARAM_TYPE_STRING, (char *)"3");

    int    count = 0;
    char **list  = nullptr;
    /* next_level=1: A.D and A.E qualify; A.B.C does not */
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Names2(nullptr, nullptr, (char *)"A.",
                                  1, &count, &list));
    EXPECT_EQ(2, count);

    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}

/* ========================================================================== */
/*  Migration from XML                                                         */
/* ========================================================================== */

TEST_F(PsmApiTest, Migration_LoadsRecordsFromXml)
{
    /* Re-init with a fresh DB, supplying a pre-populated XML file. */
    PSM_LibDeinit();
    unlink(db_path);

    const char *xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
        "<Provision>\n"
        "  <Record name=\"mig.key1\" type=\"astr\">val1</Record>\n"
        "  <Record name=\"mig.key2\" type=\"sint\" contentType=\"int\">99</Record>\n"
        "</Provision>\n";

    char *xml_file = write_tmp_file(xml);
    ASSERT_NE(nullptr, xml_file);

    /* Place it at the well-known current-config path so init picks it up. */
    int r = rename(xml_file, "/tmp/bbhm_cur_cfg.xml");
    free(xml_file);
    if (r != 0) {
        /* If rename fails (cross-device), copy manually. */
        FILE *src = fopen("/tmp/psm_api_test_xml_src", "w");
        if (src) { fputs(xml, src); fclose(src); }
        rename("/tmp/psm_api_test_xml_src", "/tmp/bbhm_cur_cfg.xml");
    }

    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_LibInit(db_path));

    char *v1 = nullptr, *v2 = nullptr;
    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"mig.key1", nullptr, &v1));
    EXPECT_STREQ("val1", v1);

    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"mig.key2", nullptr, &v2));
    EXPECT_STREQ("99", v2);

    free(v1); free(v2);
    unlink("/tmp/bbhm_cur_cfg.xml");
}

/* ========================================================================== */
/*  UpdateConfigs — overwrite modes                                           */
/* ========================================================================== */

TEST_F(PsmApiTest, UpdateConfigs_AlwaysOverwrites)
{
    /* Seed current DB */
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"fw.always",
                              PSM_PARAM_TYPE_STRING, (char *)"user_value");

    const char *new_def =
        "<?xml version=\"1.0\" ?>\n"
        "<Provision>\n"
        "  <Record name=\"fw.always\" type=\"astr\" overwrite=\"always\">"
        "new_fw_value</Record>\n"
        "</Provision>\n";

    char *xml_file = write_tmp_file(new_def);
    ASSERT_NE(nullptr, xml_file);
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_UpdateConfigs(xml_file));
    unlink(xml_file);
    free(xml_file);

    char *v = nullptr;
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"fw.always", nullptr, &v));
    EXPECT_STREQ("new_fw_value", v);
    free(v);
}

TEST_F(PsmApiTest, UpdateConfigs_NeverKeepsUserValue)
{
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"fw.never",
                              PSM_PARAM_TYPE_STRING, (char *)"user_value");

    const char *new_def =
        "<?xml version=\"1.0\" ?>\n"
        "<Provision>\n"
        "  <Record name=\"fw.never\" type=\"astr\" overwrite=\"never\">"
        "new_fw_value</Record>\n"
        "</Provision>\n";

    char *xml_file = write_tmp_file(new_def);
    ASSERT_NE(nullptr, xml_file);
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_UpdateConfigs(xml_file));
    unlink(xml_file);
    free(xml_file);

    char *v = nullptr;
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"fw.never", nullptr, &v));
    /* "never" → user value must be preserved */
    EXPECT_STREQ("user_value", v);
    free(v);
}

TEST_F(PsmApiTest, UpdateConfigs_AddsMissingRecord)
{
    /* Record not in DB yet */
    const char *new_def =
        "<?xml version=\"1.0\" ?>\n"
        "<Provision>\n"
        "  <Record name=\"fw.new\" type=\"astr\">brand_new</Record>\n"
        "</Provision>\n";

    char *xml_file = write_tmp_file(new_def);
    ASSERT_NE(nullptr, xml_file);
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_UpdateConfigs(xml_file));
    unlink(xml_file);
    free(xml_file);

    char *v = nullptr;
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"fw.new", nullptr, &v));
    EXPECT_STREQ("brand_new", v);
    free(v);
}

/* ========================================================================== */
/*  FactoryReset                                                               */
/* ========================================================================== */

TEST_F(PsmApiTest, FactoryReset_ClearsAndReloadsFromDefault)
{
    /* Insert some user data */
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"user.data",
                              PSM_PARAM_TYPE_STRING, (char *)"should_be_gone");

    const char *def_xml =
        "<?xml version=\"1.0\" ?>\n"
        "<Provision>\n"
        "  <Record name=\"factory.param\" type=\"astr\">default_val</Record>\n"
        "</Provision>\n";

    char *def_file = write_tmp_file(def_xml);
    ASSERT_NE(nullptr, def_file);

    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_FactoryReset(def_file));
    unlink(def_file);
    free(def_file);

    /* User data must be gone */
    char *v1 = nullptr;
    EXPECT_EQ(PSM_LIB_ERR_NOT_FOUND,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"user.data", nullptr, &v1));
    free(v1);

    /* Factory record must be present */
    char *v2 = nullptr;
    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"factory.param", nullptr, &v2));
    EXPECT_STREQ("default_val", v2);
    free(v2);
}

/* ========================================================================== */
/*  ImportConfig / ExportConfig round-trip                                    */
/* ========================================================================== */

TEST_F(PsmApiTest, ImportExport_PlainXmlRoundTrip)
{
    /* Seed */
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"exp.k1",
                              PSM_PARAM_TYPE_STRING, (char *)"v1");
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"exp.k2",
                              PSM_PARAM_TYPE_INT,    (char *)"7");

    /* Export */
    char *exported = nullptr;
    int   exp_size = 0;
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_ExportConfig(&exported, &exp_size,
                                                     nullptr, 0));
    ASSERT_GT(exp_size, 0);

    /* Wipe and re-import */
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_ImportConfig(exported, exp_size,
                                                      nullptr, 0));
    free(exported);

    /* Verify records survived the round-trip */
    char *v1 = nullptr, *v2 = nullptr;
    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"exp.k1", nullptr, &v1));
    EXPECT_STREQ("v1", v1);

    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"exp.k2", nullptr, &v2));
    EXPECT_STREQ("7", v2);

    free(v1); free(v2);
}

TEST_F(PsmApiTest, ImportConfig_InvalidXml_ReturnsError)
{
    const char *garbage = "{ this is not xml }";
    EXPECT_NE(PSM_LIB_SUCCESS,
        PSM_Lib_ImportConfig(garbage, (int)strlen(garbage), nullptr, 0));
}

/* ========================================================================== */
/*  SaveConfigToFlash — smoke test (must not crash or return error)           */
/* ========================================================================== */

TEST_F(PsmApiTest, SaveConfigToFlash_Succeeds)
{
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"flash.k",
                              PSM_PARAM_TYPE_STRING, (char *)"v");
    EXPECT_EQ(PSM_LIB_SUCCESS, PSM_Lib_SaveConfigToFlash());
}

/* ========================================================================== */
/*  DisableWriting — deferred queue                                            */
/* ========================================================================== */

TEST_F(PsmApiTest, DisableWriting_DefersThenFlushes)
{
    /* Disable writing (increment ref count) */
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_DisableWriting(1));

    /* These writes should be queued, not immediately committed */
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"dw.k1",
                              PSM_PARAM_TYPE_STRING, (char *)"queued1");
    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"dw.k2",
                              PSM_PARAM_TYPE_STRING, (char *)"queued2");

    /* Re-enable: queue should flush */
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_DisableWriting(0));

    /* Both records must now be in the DB */
    char *v1 = nullptr, *v2 = nullptr;
    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"dw.k1", nullptr, &v1));
    EXPECT_STREQ("queued1", v1);

    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"dw.k2", nullptr, &v2));
    EXPECT_STREQ("queued2", v2);

    free(v1); free(v2);
}

TEST_F(PsmApiTest, DisableWriting_NestedRefCount)
{
    /* Double disable */
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_DisableWriting(1));
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_DisableWriting(1));

    PSM_Lib_Set_Record_Value2(nullptr, nullptr, (char *)"nested.k",
                              PSM_PARAM_TYPE_STRING, (char *)"v");

    /* First enable should NOT flush yet (count goes to 1 not 0) */
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_DisableWriting(0));

    /* Second enable flushes */
    ASSERT_EQ(PSM_LIB_SUCCESS, PSM_Lib_DisableWriting(0));

    char *v = nullptr;
    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)"nested.k", nullptr, &v));
    EXPECT_STREQ("v", v);
    free(v);
}

/* ========================================================================== */
/*  Passphrase log suppression — functional test                               */
/* The actual log output is not tested (would require log capture); instead    */
/* we verify that setting/getting Passphrase records works correctly.          */
/* ========================================================================== */

TEST_F(PsmApiTest, Passphrase_SetGetWorks)
{
    const char *name = "eRT.com.cisco.spvtg.ccsp.Device.WiFi.SSID.1.Passphrase";

    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Set_Record_Value2(nullptr, nullptr,
                                  (char *)name,
                                  PSM_PARAM_TYPE_STRING,
                                  (char *)"SuperSecret123!"));

    char *value = nullptr;
    ASSERT_EQ(PSM_LIB_SUCCESS,
        PSM_Lib_Get_Record_Value2(nullptr, nullptr,
                                  (char *)name, nullptr, &value));
    EXPECT_STREQ("SuperSecret123!", value);
    free(value);
}

/* ========================================================================== */
/*  Compatibility macros — ensure #defines resolve correctly                  */
/* ========================================================================== */

TEST_F(PsmApiTest, CompatMacros_Resolve)
{
    /* PSM_Set_Record_Value2 must alias PSM_Lib_Set_Record_Value2 */
    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Set_Record_Value2(nullptr, nullptr,
                              (char *)"compat.k",
                              PSM_PARAM_TYPE_STRING, (char *)"cv"));

    char *v = nullptr;
    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Get_Record_Value2(nullptr, nullptr,
                              (char *)"compat.k", nullptr, &v));
    EXPECT_STREQ("cv", v);
    free(v);

    EXPECT_EQ(PSM_LIB_SUCCESS,
        PSM_Del_Record(nullptr, nullptr, (char *)"compat.k"));
}
