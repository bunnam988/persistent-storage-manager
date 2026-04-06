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
#                                              -*- Autoconf -*-
# Process this file with autoconf to produce configure script.
#

AC_PREREQ([2.65])
AC_INIT([CcspPsm], [1.0], [BUG-REPORT-ADDRESS])
AM_INIT_AUTOMAKE([subdir-objects])
LT_INIT

AC_ARG_ENABLE([unitTestDockerSupport],
             AS_HELP_STRING([--enable-unitTestDockerSupport],[enable docker support for unit testing (default is no)]),
             [
              UNIT_TEST_DOCKER_SUPPORT=""
               case "${enableval}" in
                yes) UNIT_TEST_DOCKER_SUPPORT=true
                     m4_if(m4_sysval,[0],[AC_CONFIG_FILES([source/test/Makefile])]);;
                no) UNIT_TEST_DOCKER_SUPPORT=false;;
                 *) AC_MSG_ERROR([bad value ${enableval} for --enable-unitTestDockerSupport ]);;
               esac
             ],
             [echo "Docker support is disabled"])
AM_CONDITIONAL([UNIT_TEST_DOCKER_SUPPORT], [test x$UNIT_TEST_DOCKER_SUPPORT = xtrue])

AC_PREFIX_DEFAULT(`pwd`)
AC_ENABLE_SHARED
AC_DISABLE_STATIC

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AC_PROG_CC
AC_PROG_CXX
AC_PROG_INSTALL
AM_PROG_CC_C_O
AM_PROG_LIBTOOL(libtool)

SYSTEMD_CFLAGS=" "
SYSTEMD_LDFLAGS=" "

AC_ARG_ENABLE([notify],
        AS_HELP_STRING([--enable-notify],[enable systemd notify]),
        [
          case "${enableval}" in
           yes) SYSTEMD_CFLAGS="-DENABLE_SD_NOTIFY"
                SYSTEMD_LDFLAGS="-lsystemd" ;;
           no) AC_MSG_ERROR([systemd notify is disabled]) ;;
          *) AC_MSG_ERROR([bad value ${enableval} for --enable-notify ]) ;;
           esac
           ],
         [echo "systemd notify is disabled."])

AC_SUBST(SYSTEMD_CFLAGS)
AC_SUBST(SYSTEMD_LDFLAGS)

AC_ARG_ENABLE([mountutils],
        AS_HELP_STRING([--enable-mountutils],[enables mountutils replacement (default is no)]),
        [
          case "${enableval}" in
           yes) IS_LIBRDKCONFIG_ENABLED=true
                LIBRDKCONFIG_FLAG=" -DLIBRDKCONFIG_BUILD ";;
           no)  IS_LIBRDKCONFIG_ENABLED=false ;;
          *) AC_MSG_ERROR([bad value ${enableval} for --enable-mountutils]) ;;
           esac
           ],
        [echo "mountutils is disabled"])
AM_CONDITIONAL([IS_LIBRDKCONFIG_ENABLED], [test x$IS_LIBRDKCONFIG_ENABLED = xtrue])
AC_SUBST(LIBRDKCONFIG_FLAG)

# ---------------------------------------------------------------------------
# --enable-serverless: build PSM as an in-process SQLite-backed library
#   instead of the RBus/D-Bus daemon.
# ---------------------------------------------------------------------------
PSM_SERVERLESS_ENABLED=false
SQLITE3_CFLAGS=""
SQLITE3_LIBS=""

AC_ARG_ENABLE([serverless],
    AS_HELP_STRING([--enable-serverless],
        [build PSM as an in-process SQLite library (no RBus/D-Bus daemon)]),
    [
      case "${enableval}" in
        yes) PSM_SERVERLESS_ENABLED=true ;;
        no)  PSM_SERVERLESS_ENABLED=false ;;
        *)   AC_MSG_ERROR([bad value ${enableval} for --enable-serverless]) ;;
      esac
    ],
    [echo "serverless library build is disabled"])
AM_CONDITIONAL([PSM_SERVERLESS], [test x$PSM_SERVERLESS_ENABLED = xtrue])

if test x$PSM_SERVERLESS_ENABLED = xtrue; then
    # SQLite3: prefer system pkg-config, fall back to bundled amalgamation.
    PKG_CHECK_MODULES([SQLITE3], [sqlite3],
        [HAVE_SYSTEM_SQLITE3=yes
         AC_MSG_NOTICE([Using system sqlite3])],
        [HAVE_SYSTEM_SQLITE3=no
         AC_MSG_WARN([sqlite3 not found via pkg-config; using bundled amalgamation])])
    AM_CONDITIONAL([USE_SYSTEM_SQLITE3],
        [test x$HAVE_SYSTEM_SQLITE3 = xyes])

    # zlib is required for .gz config file decompression.
    AC_CHECK_LIB([z], [gzopen], [],
        [AC_MSG_ERROR([zlib not found (required for --enable-serverless)])])

    # OpenSSL is required for DES import/export.
    PKG_CHECK_MODULES([OPENSSL], [openssl],
        [AC_MSG_NOTICE([Using system OpenSSL])],
        [AC_MSG_ERROR([OpenSSL not found (required for --enable-serverless)])])
else
    AM_CONDITIONAL([USE_SYSTEM_SQLITE3], [false])
fi

AC_SUBST(SQLITE3_CFLAGS)
AC_SUBST(SQLITE3_LIBS)
AC_SUBST(OPENSSL_CFLAGS)
AC_SUBST(OPENSSL_LIBS)

# Checks for header files.
AC_CHECK_HEADERS([stdlib.h string.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_INLINE

# Checks for library functions.
AC_FUNC_MALLOC

AC_CONFIG_FILES(
	source/Makefile
	Makefile
)

AC_SUBST(UNIT_TEST_DOCKER_SUPPORT)

AC_OUTPUT

