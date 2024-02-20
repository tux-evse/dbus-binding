###########################################################################
# Copyright (C) 2015-2024 "IoT.bzh"
# Author: José Bollo <jose.bollo@iot.bzh>
#
# $RP_BEGIN_LICENSE$
# Commercial License Usage
#  Licensees holding valid commercial IoT.bzh licenses may use this file in
#  accordance with the commercial license agreement provided with the
#  Software or, alternatively, in accordance with the terms contained in
#  a written agreement between you and The IoT.bzh Company. For licensing terms
#  and conditions see https://www.iot.bzh/terms-conditions. For further
#  information use the contact form at https://www.iot.bzh/contact.
#
# GNU General Public License Usage
#  Alternatively, this file may be used under the terms of the GNU General
#  Public license version 3. This license is as published by the Free Software
#  Foundation and appearing in the file LICENSE.GPLv3 included in the packaging
#  of this file. Please review the following information to ensure the GNU
#  General Public License requirements will be met
#  https://www.gnu.org/licenses/gpl-3.0.html.
# $RP_END_LICENSE$
###########################################################################

cmake_minimum_required(VERSION 3.15)

project(dbus-binding
        VERSION 0.0
        DESCRIPTION "Connection to DBus binding"
        HOMEPAGE_URL "https://github.com/redpesk-samples/dbus-binding"
        LANGUAGES C)

include(FindPkgConfig)
include(GNUInstallDirs)
include(CTest)

pkg_check_modules(DEPS REQUIRED afb-binding>=4 libsystemd>=222 json-c)

pkg_get_variable(VSCRIPT afb-binding version_script)

set(DEFBUS SYSTEM CACHE STRING "default bus, either SYSTEM or USER")
set(AFM_APP_DIR ${CMAKE_INSTALL_PREFIX}/redpesk CACHE PATH "Application directory")

add_library(dbus-binding MODULE src/dbus-binding.c src/dbus-jsonc.c)
target_compile_definitions(dbus-binding PRIVATE DEFAULT_BUSNAME=BUSNAME_${DEFBUS} VERSION="${PROJECT_VERSION}")
target_compile_options(dbus-binding PRIVATE ${DEPS_CFLAGS})
target_include_directories(dbus-binding PRIVATE ${DEPS_INCLUDE_DIRS})
target_link_libraries(dbus-binding ${DEPS_LDFLAGS} libpcscd-glue.so)

set_target_properties(dbus-binding PROPERTIES PREFIX "" LINK_FLAGS "-Wl,--version-script=${VSCRIPT}")


set(DEST ${AFM_APP_DIR}/dbus-binding)
install(TARGETS dbus-binding
        LIBRARY DESTINATION ${DEST}/lib)

