##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2015 RDK Management
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
if UNIT_TEST_DOCKER_SUPPORT
SUBDIRS = test
else

ACLOCAL_AMFLAGS = -I m4

# ---------------------------------------------------------------------------
# Serverless library build  (./configure --enable-serverless)
# Produces:  libpsm.so / libpsm.a
# Does NOT build the PsmSsp daemon.
# ---------------------------------------------------------------------------
if PSM_SERVERLESS

lib_LTLIBRARIES = libpsm.la

libpsm_la_CPPFLAGS = \
    -I$(top_srcdir)/source/include \
    -I$(top_srcdir)/source/PsmSqlite

libpsm_la_CFLAGS = \
    -DPSM_SERVERLESS \
    $(OPENSSL_CFLAGS)

if USE_SYSTEM_SQLITE3
libpsm_la_CFLAGS  += $(SQLITE3_CFLAGS)
else
libpsm_la_CFLAGS  += -DPSM_BUNDLED_SQLITE3
endif

libpsm_la_SOURCES = \
    PsmSqlite/psm_sqlite.c \
    PsmSqlite/psm_migrate.c \
    PsmApi/psm_api.c \
    Ssp/psm_hal_apis.c

if !USE_SYSTEM_SQLITE3
# When sqlite3 is not found on the system, include the amalgamation.
# Place sqlite3.c and sqlite3.h in source/PsmSqlite/ before building.
libpsm_la_SOURCES += PsmSqlite/sqlite3.c
endif

libpsm_la_LDFLAGS = \
    -version-info 1:0:0 \
    -lz \
    -lpthread \
    -lsyscfg \
    -lcjson \
    $(OPENSSL_LIBS)

if USE_SYSTEM_SQLITE3
libpsm_la_LDFLAGS += $(SQLITE3_LIBS)
endif

if IS_LIBRDKCONFIG_ENABLED
libpsm_la_CFLAGS  += $(LIBRDKCONFIG_FLAG)
libpsm_la_LDFLAGS += -lrdkconfig
endif

# Install the public header so consumers can #include <psm_api.h>
pkginclude_HEADERS = include/psm_api.h

else
# ---------------------------------------------------------------------------
# Original daemon build  (./configure  — default)
# Produces:  PsmSsp binary
# ---------------------------------------------------------------------------

bin_PROGRAMS = PsmSsp

PsmSsp_CPPFLAGS = \
    -I$(top_srcdir)/source/DslhManagementServer \
    -I$(top_srcdir)/source/include

PsmSsp_CFLAGS = $(SYSTEMD_CFLAGS)

PsmSsp_SOURCES = \
    Ssp/ssp_main.c \
    Ssp/ssp_rbus.c \
    Ssp/ssp_dbus.c \
    Ssp/ssp_cfmif.c \
    Ssp/psm_hal_apis.c \
    PsmFileLoader/psm_flo_interface.c \
    PsmFileLoader/psm_flo_control.c \
    PsmFileLoader/psm_flo_parse.c \
    PsmFileLoader/psm_flo_operation.c \
    PsmFileLoader/psm_flo_states.c \
    PsmFileLoader/psm_flo_base.c \
    PsmSysRegistry/psm_sysro_states.c \
    PsmSysRegistry/psm_sysro_interface.c \
    PsmSysRegistry/psm_sysro_storage.c \
    PsmSysRegistry/psm_sysro_sysramif.c \
    PsmSysRegistry/psm_sysro_base.c \
    PsmSysRegistry/psm_sysro_cfmif.c \
    PsmSysRegistry/psm_sysro_operation.c \
    PsmSysRegistry/psm_sysro_control.c

PsmSsp_LDFLAGS = -lccsp_common $(SYSTEMD_LDFLAGS) -lsyscfg -lutapi -lutctx -lprivilege -lrbus -lcjson -lsecure_wrapper -lpthread

if IS_LIBRDKCONFIG_ENABLED
PsmSsp_CFLAGS  += $(LIBRDKCONFIG_FLAG)
PsmSsp_LDFLAGS += -lrdkconfig
endif

endif # PSM_SERVERLESS
endif # UNIT_TEST_DOCKER_SUPPORT
