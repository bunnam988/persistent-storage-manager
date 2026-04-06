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
 * psm_sqlite_test.cpp — Unit tests for the PSM SQLite storage engine.
 *
 * Tests the psm_sqlite_* functions directly against a real (in-memory or
 * temporary-file) SQLite database.  No mocks required.
 */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

extern "C" {
#include "psm_sqlite.h"
}

/* --------------------------------------------------------------------------
 * Fixture: opens a fresh in-memory database before each test and closes it
 * after.  Using ":memory:" avoids touching the filesystem and ensures
 * complete test isolation.
 * -------------------------------------------------------------------------- */
class PsmSqliteTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(0, psm_sqlite_open(":memory:"));
    }

    void TearDown() override {
        psm_sqlite_close();
    }
};

/* ========================================================================== */
/*  Basic open / close                                                         */
/* ========================================================================== */

TEST_F(PsmSqliteTest, OpenClose_Idempotent)
{
    /* Second open on the same connection should not crash (close first). */
    psm_sqlite_close();
    EXPECT_EQ(0, psm_sqlite_open(":memory:"));
}

TEST_F(PsmSqliteTest, IsEmpty_OnFreshDB)
{
    EXPECT_EQ(1, psm_sqlite_is_empty());
}

/* ========================================================================== */
/*  Set / Get round-trip                                                       */
/* ========================================================================== */

TEST_F(PsmSqliteTest, SetGet_BasicString)
{
    ASSERT_EQ(0, psm_sqlite_set("eRT.com.test.Key1", "astr", NULL, "hello"));

    char *type = NULL, *ctype = NULL, *value = NULL;
    ASSERT_EQ(0, psm_sqlite_get("eRT.com.test.Key1", &type, &ctype, &value));

    EXPECT_STREQ("astr",  type);
    EXPECT_EQ(nullptr,    ctype);
    EXPECT_STREQ("hello", value);

    free(type); free(ctype); free(value);
}

TEST_F(PsmSqliteTest, SetGet_WithContentType)
{
    ASSERT_EQ(0, psm_sqlite_set("param.int.val", "sint", "int", "42"));

    char *type = NULL, *ctype = NULL, *value = NULL;
    ASSERT_EQ(0, psm_sqlite_get("param.int.val", &type, &ctype, &value));

    EXPECT_STREQ("sint", type);
    EXPECT_STREQ("int",  ctype);
    EXPECT_STREQ("42",   value);

    free(type); free(ctype); free(value);
}

TEST_F(PsmSqliteTest, SetGet_NullValue)
{
    EXPECT_EQ(0, psm_sqlite_set("param.null", "astr", NULL, NULL));

    char *value = NULL;
    ASSERT_EQ(0, psm_sqlite_get("param.null", NULL, NULL, &value));
    /* NULL stored as empty string on retrieval */
    free(value);
}

TEST_F(PsmSqliteTest, Get_NotFound_Returns1)
{
    char *value = NULL;
    int rc = psm_sqlite_get("does.not.exist", NULL, NULL, &value);
    EXPECT_EQ(1, rc);
    EXPECT_EQ(nullptr, value);
}

/* ========================================================================== */
/*  Write-no-op optimisation (RDKB-24884)                                     */
/* ========================================================================== */

TEST_F(PsmSqliteTest, Set_SkipsWriteOnIdenticalValue)
{
    ASSERT_EQ(0, psm_sqlite_set("opt.key", "astr", NULL, "same"));
    /* Setting the same value a second time must succeed without error. */
    EXPECT_EQ(0, psm_sqlite_set("opt.key", "astr", NULL, "same"));

    char *value = NULL;
    ASSERT_EQ(0, psm_sqlite_get("opt.key", NULL, NULL, &value));
    EXPECT_STREQ("same", value);
    free(value);
}

TEST_F(PsmSqliteTest, Set_OverwriteWithNewValue)
{
    ASSERT_EQ(0, psm_sqlite_set("upd.key", "astr", NULL, "old"));
    ASSERT_EQ(0, psm_sqlite_set("upd.key", "astr", NULL, "new"));

    char *value = NULL;
    ASSERT_EQ(0, psm_sqlite_get("upd.key", NULL, NULL, &value));
    EXPECT_STREQ("new", value);
    free(value);
}

/* ========================================================================== */
/*  Delete                                                                     */
/* ========================================================================== */

TEST_F(PsmSqliteTest, Delete_ExistingRecord)
{
    ASSERT_EQ(0, psm_sqlite_set("del.key", "astr", NULL, "v"));
    ASSERT_EQ(0, psm_sqlite_delete("del.key"));

    char *value = NULL;
    EXPECT_EQ(1, psm_sqlite_get("del.key", NULL, NULL, &value));
    free(value);
}

TEST_F(PsmSqliteTest, Delete_NonExistentRecord_Succeeds)
{
    /* Deleting a key that does not exist must not return an error. */
    EXPECT_EQ(0, psm_sqlite_delete("ghost.key"));
}

/* ========================================================================== */
/*  IsEmpty                                                                   */
/* ========================================================================== */

TEST_F(PsmSqliteTest, IsEmpty_AfterInsert)
{
    ASSERT_EQ(0, psm_sqlite_set("k", "astr", NULL, "v"));
    EXPECT_EQ(0, psm_sqlite_is_empty());
}

TEST_F(PsmSqliteTest, IsEmpty_AfterClear)
{
    ASSERT_EQ(0, psm_sqlite_set("k", "astr", NULL, "v"));
    ASSERT_EQ(0, psm_sqlite_clear());
    EXPECT_EQ(1, psm_sqlite_is_empty());
}

/* ========================================================================== */
/*  GetNames — prefix query                                                   */
/* ========================================================================== */

TEST_F(PsmSqliteTest, GetNames_PrefixMatch)
{
    psm_sqlite_set("Device.WiFi.Radio.1.Channel",  "astr", NULL, "6");
    psm_sqlite_set("Device.WiFi.Radio.1.Bandwidth","astr", NULL, "20");
    psm_sqlite_set("Device.WiFi.Radio.2.Channel",  "astr", NULL, "11");
    psm_sqlite_set("Device.Ethernet.Interface.1",  "astr", NULL, "eth0");

    char **names = nullptr;
    int   count  = 0;
    ASSERT_EQ(0, psm_sqlite_get_names("Device.WiFi.", 0, &names, &count));
    EXPECT_EQ(3, count);

    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

TEST_F(PsmSqliteTest, GetNames_NextLevel_OnlyDirectChildren)
{
    psm_sqlite_set("Device.WiFi.Radio",   "astr", NULL, "");
    psm_sqlite_set("Device.WiFi.SSID",    "astr", NULL, "");
    psm_sqlite_set("Device.WiFi.X.Deep",  "astr", NULL, ""); /* should be excluded */
    psm_sqlite_set("Device.Ethernet",     "astr", NULL, ""); /* different branch */

    char **names = nullptr;
    int   count  = 0;
    /* next_level=1: only direct children of "Device.WiFi." — no '.' after prefix */
    ASSERT_EQ(0, psm_sqlite_get_names("Device.WiFi.", 1, &names, &count));

    /* Radio and SSID qualify; X (which has children) does not. */
    EXPECT_EQ(2, count);

    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

TEST_F(PsmSqliteTest, GetNames_EmptyPrefix_ReturnsAll)
{
    psm_sqlite_set("A", "astr", NULL, "1");
    psm_sqlite_set("B", "astr", NULL, "2");
    psm_sqlite_set("C", "astr", NULL, "3");

    char **names = nullptr;
    int   count  = 0;
    ASSERT_EQ(0, psm_sqlite_get_names("", 0, &names, &count));
    EXPECT_EQ(3, count);

    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

TEST_F(PsmSqliteTest, GetNames_NoMatch_ReturnsEmpty)
{
    psm_sqlite_set("Device.WiFi.x", "astr", NULL, "");

    char **names = nullptr;
    int   count  = -1;
    ASSERT_EQ(0, psm_sqlite_get_names("ZZZ.", 0, &names, &count));
    EXPECT_EQ(0, count);
    free(names);
}

/* ========================================================================== */
/*  Transaction — rollback on failure                                         */
/* ========================================================================== */

TEST_F(PsmSqliteTest, Transaction_CommitPreservesData)
{
    psm_sqlite_begin();
    psm_sqlite_set("txn.key1", "astr", NULL, "v1");
    psm_sqlite_set("txn.key2", "astr", NULL, "v2");
    psm_sqlite_commit();

    char *v1 = NULL, *v2 = NULL;
    EXPECT_EQ(0, psm_sqlite_get("txn.key1", NULL, NULL, &v1));
    EXPECT_EQ(0, psm_sqlite_get("txn.key2", NULL, NULL, &v2));
    EXPECT_STREQ("v1", v1);
    EXPECT_STREQ("v2", v2);
    free(v1); free(v2);
}

TEST_F(PsmSqliteTest, Transaction_RollbackDiscardsData)
{
    psm_sqlite_begin();
    psm_sqlite_set("rollback.key", "astr", NULL, "should_not_persist");
    psm_sqlite_rollback();

    char *value = NULL;
    EXPECT_EQ(1, psm_sqlite_get("rollback.key", NULL, NULL, &value));
    free(value);
}

/* ========================================================================== */
/*  Concurrent access (WAL mode)                                              */
/* ========================================================================== */

struct ThreadArgs {
    int thread_id;
    int iterations;
    int errors;
};

static void *writer_thread(void *arg)
{
    ThreadArgs *ta = static_cast<ThreadArgs *>(arg);
    char key[64], val[64];

    for (int i = 0; i < ta->iterations; i++) {
        snprintf(key, sizeof(key), "concurrent.t%d.k%d", ta->thread_id, i);
        snprintf(val, sizeof(val), "value_%d_%d", ta->thread_id, i);
        if (psm_sqlite_set(key, "astr", NULL, val) != 0)
            ta->errors++;
    }
    return nullptr;
}

TEST_F(PsmSqliteTest, Concurrent_TwoWriterThreads)
{
    const int ITERS = 200;

    ThreadArgs ta1 = { 1, ITERS, 0 };
    ThreadArgs ta2 = { 2, ITERS, 0 };

    pthread_t t1, t2;
    ASSERT_EQ(0, pthread_create(&t1, nullptr, writer_thread, &ta1));
    ASSERT_EQ(0, pthread_create(&t2, nullptr, writer_thread, &ta2));
    pthread_join(t1, nullptr);
    pthread_join(t2, nullptr);

    EXPECT_EQ(0, ta1.errors) << "Thread-1 had write errors";
    EXPECT_EQ(0, ta2.errors) << "Thread-2 had write errors";

    /* Total row count must be ITERS * 2 */
    char **names = nullptr;
    int    count = 0;
    ASSERT_EQ(0, psm_sqlite_get_names("concurrent.", 0, &names, &count));
    EXPECT_EQ(ITERS * 2, count);
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* ========================================================================== */
/*  Export XML round-trip                                                     */
/* ========================================================================== */

TEST_F(PsmSqliteTest, ExportXml_ContainsRecords)
{
    psm_sqlite_set("xml.key1", "astr", NULL,   "v1");
    psm_sqlite_set("xml.key2", "sint", "int",  "99");

    char *buf  = nullptr;
    int   size = 0;
    ASSERT_EQ(0, psm_sqlite_export_xml(&buf, &size));
    ASSERT_GT(size, 0);

    EXPECT_NE(nullptr, strstr(buf, "<Provision>"));
    EXPECT_NE(nullptr, strstr(buf, "xml.key1"));
    EXPECT_NE(nullptr, strstr(buf, "xml.key2"));
    EXPECT_NE(nullptr, strstr(buf, "contentType=\"int\""));

    free(buf);
}

TEST_F(PsmSqliteTest, ExportXml_EmptyDB)
{
    char *buf  = nullptr;
    int   size = 0;
    ASSERT_EQ(0, psm_sqlite_export_xml(&buf, &size));
    EXPECT_NE(nullptr, strstr(buf, "<Provision>"));
    EXPECT_NE(nullptr, strstr(buf, "</Provision>"));
    free(buf);
}
