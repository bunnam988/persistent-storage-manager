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

bin_PROGRAMS = PsmSsp

PsmSsp_CPPFLAGS = \
    -I$(top_srcdir)/source/DslhManagementServer \
    -I$(top_srcdir)/source/include

PsmSsp_CFLAGS = $(SYSTEMD_CFLAGS)

PsmSsp_SOURCES = \
    Ssp/ssp_main.c \
    Ssp/ssp_rbus.c \
    Ssp/ssp_dbus.c \
    Ssp/psm_hal_apis.c

# Removed libraries: -lcjson
PsmSsp_LDFLAGS = -lccsp_common $(SYSTEMD_LDFLAGS) -lsyscfg -lutapi -lutctx -lprivilege -lrbus -lsecure_wrapper -lpthread

if IS_LIBRDKCONFIG_ENABLED
PsmSsp_CFLAGS += $(LIBRDKCONFIG_FLAG)
PsmSsp_LDFLAGS += -lrdkconfig
endif
endif
