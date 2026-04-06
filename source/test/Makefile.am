##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2020 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################
AM_CFLAGS = -fPIC -fno-exceptions -Os -Wall -Wextra

AM_CXXFLAGS = -std=c++11

ACLOCAL_AMFLAGS = -I m4

bin_PROGRAMS = PsmSsp_gtest.bin

PsmSsp_gtest_bin_CPPFLAGS = -I$(PKG_CONFIG_SYSROOT_DIR)$(includedir)/gtest \
                            -I${top_srcdir}/gtest/include \
                            -I$(top_srcdir)/../CcspCommonLibrary/source/cosa/include/ \
                            -I$(top_srcdir)/../CcspCommonLibrary/source/ccsp/components/include/ \
                            -I$(PKG_CONFIG_SYSROOT_DIR)$(includedir)/ccsp \
                            -I${PKG_CONFIG_SYSROOT_DIR}$(includedir)/libsafec \
                            -I${top_srcdir}/source \
                            -I${top_srcdir}/source/include \
                            -I${top_srcdir}/source/PsmSysRegistry \
                            -I${top_srcdir}/source/PsmFileLoader

PsmSsp_gtest_bin_SOURCES = gtest_main.cpp \
                           $(top_srcdir)/source/Ssp/psm_hal_apis.c \
                           $(top_srcdir)/source/Ssp/ssp_cfmif.c \
                           $(top_srcdir)/source/PsmSysRegistry/psm_sysro_base.c \
                           $(top_srcdir)/source/PsmSysRegistry/psm_sysro_interface.c \
                           $(top_srcdir)/source/PsmSysRegistry/psm_sysro_storage.c \
                           $(top_srcdir)/source/PsmSysRegistry/psm_sysro_sysramif.c \
                           $(top_srcdir)/source/PsmSysRegistry/psm_sysro_cfmif.c \
                           $(top_srcdir)/source/PsmSysRegistry/psm_sysro_operation.c \
                           $(top_srcdir)/source/PsmSysRegistry/psm_sysro_control.c \
                           $(top_srcdir)/source/PsmSysRegistry/psm_sysro_states.c \
                           $(top_srcdir)/source/PsmFileLoader/psm_flo_interface.c \
                           $(top_srcdir)/source/PsmFileLoader/psm_flo_control.c \
                           $(top_srcdir)/source/PsmFileLoader/psm_flo_parse.c \
                           $(top_srcdir)/source/PsmFileLoader/psm_flo_operation.c \
                           $(top_srcdir)/source/PsmFileLoader/psm_flo_states.c \
                           $(top_srcdir)/source/PsmFileLoader/psm_flo_base.c \
                           psm_flo_base_test.cpp \
                           psm_flo_operation_test.cpp \
                           psm_flo_interface_test.cpp \
                           psm_flo_states_test.cpp \
                           psm_flo_control_test.cpp \
                           psm_flo_parse_test.cpp \
                           psm_sysro_base_test.cpp \
                           psm_sysro_interface_test.cpp \
                           psm_sysro_sysramif_test.cpp \
                           psm_sysro_control_test.cpp \
                           psm_sysro_operation_test.cpp \
                           psm_sysro_states_test.cpp \
                           psm_sysro_storage_test.cpp \
                           psm_sysro_cfmif_test.cpp \
                           psm_mock.cpp

PsmSsp_gtest_bin_LDFLAGS = -lgtest -lgmock -lgcov -lcjson -lpthread

PsmSsp_gtest_bin_LDADD = $(HOME)/usr/local/lib/libmock_ccsp_common.la \
                             $(HOME)/usr/local/lib/libmock_safec_lib.la \
                             $(HOME)/usr/local/lib/libmock_syscfg.la \
                             $(HOME)/usr/local/lib/libmock_file_io.la \
                             $(HOME)/usr/local/lib/libmock_cjson.la

# ---------------------------------------------------------------------------
# Serverless library unit tests
# Tests the psm_sqlite_* and PSM_Lib_* functions against a real SQLite DB.
# Build with:  ./configure --enable-unitTestDockerSupport --enable-serverless
# ---------------------------------------------------------------------------
if PSM_SERVERLESS

bin_PROGRAMS += PsmSqlite_gtest.bin

PsmSqlite_gtest_bin_CPPFLAGS = \
    -I$(PKG_CONFIG_SYSROOT_DIR)$(includedir)/gtest \
    -I${top_srcdir}/gtest/include \
    -I${top_srcdir}/source/include \
    -I${top_srcdir}/source/PsmSqlite

PsmSqlite_gtest_bin_CFLAGS   = $(SQLITE3_CFLAGS) $(OPENSSL_CFLAGS)
PsmSqlite_gtest_bin_CXXFLAGS = -std=c++11 $(SQLITE3_CFLAGS) $(OPENSSL_CFLAGS)

PsmSqlite_gtest_bin_SOURCES = \
    gtest_main.cpp \
    psm_sqlite_test.cpp \
    psm_api_test.cpp \
    $(top_srcdir)/source/PsmSqlite/psm_sqlite.c \
    $(top_srcdir)/source/PsmSqlite/psm_migrate.c \
    $(top_srcdir)/source/PsmApi/psm_api.c \
    $(top_srcdir)/source/Ssp/psm_hal_apis.c

PsmSqlite_gtest_bin_LDFLAGS = \
    -lgtest \
    -lgcov \
    -lz \
    -lpthread \
    -lsyscfg \
    -lcjson \
    $(SQLITE3_LIBS) \
    $(OPENSSL_LIBS) \
    --coverage

endif # PSM_SERVERLESS

# Adding Code Coverage Flags
AM_CFLAGS += -fprofile-arcs -ftest-coverage
AM_CXXFLAGS += -fprofile-arcs -ftest-coverage
PsmSsp_gtest_bin_LDFLAGS += --coverage

