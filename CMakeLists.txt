# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# THE MESOS PROJECT.
####################
cmake_minimum_required(VERSION 2.8)

project(Mesos)
set(MESOS_MAJOR_VERSION 1)
set(MESOS_MINOR_VERSION 1)
set(MESOS_PATCH_VERSION 0)
set(PACKAGE_VERSION
  ${MESOS_MAJOR_VERSION}.${MESOS_MINOR_VERSION}.${MESOS_PATCH_VERSION})

set(MESOS_PACKAGE_VERSION ${PACKAGE_VERSION})
set(MESOS_PACKAGE_SOVERSION 0)

# MESOS BUILD CONFIGURATION OPTIONS.
####################################
option(_DEBUG "Set default build configuration to debug" ON)
option(VERBOSE "Enable verbose CMake statements and compilation output" TRUE)
option(
  REBUNDLED
  "Build dependencies from tar.gz files in 3rdparty folder instead of downloading"
  TRUE
  )
option(
  ENABLE_LIBEVENT
  "Use libevent instead of default libev as the core event loop implementation"
  FALSE
  )
set(CMAKE_VERBOSE_MAKEFILE ${VERBOSE})
set(
  3RDPARTY_DEPENDENCIES "https://github.com/3rdparty/mesos-3rdparty/raw/master"
  CACHE STRING "Pass in a URL or filesystem path that points at a mirror of the canonical third-party dependency repository (https://github.com/3rdparty/mesos-3rdparty); this will cause Mesos to build against the third-party tarballs we find in the mirror, instead of going to the Internet or building against the rebundled tarballs in the Mesos repository.\n\nThis is especially useful for Windows builds, because building on MSVC 1900 requires newer versions of ZK, glog, and libevent, than the ones rebundled in the Mesos repository, and so in that case we normally would have to go to the Internet to get them. Using this flag, however, means we can avoid this.\nNOTE: If you use this in conjunction with the `REBUNDLED` flag, then we will only go to this repository when we would go to the Internet normally; on most Unix builds, this will mean you never go to the Internet, but on Windows, it means you will in the case of glog, ZK, and libevent. To build purely against the mirror, do not use the `REBUNDLED` flag."
  )

if (REBUNDLED AND ENABLE_LIBEVENT)
  message(
    WARNING
    "Both `ENABLE_LIBEVENT` and `REBUNDLED` (set to TRUE by default) flags "
    "have been set. But, libevent does not come rebundled in Mesos, so it must "
    "be downloaded."
    )
endif (REBUNDLED AND ENABLE_LIBEVENT)

if (WIN32 AND REBUNDLED)
  message(
    WARNING
    "The current supported version of ZK does not compile on Windows, and does "
    "not come rebundled in the Mesos repository. It must be downloaded from "
    "the Internet, even though the `REBUNDLED` flag was set."
    )
endif (WIN32 AND REBUNDLED)

if (WIN32 AND (NOT ENABLE_LIBEVENT))
  message(
    FATAL_ERROR
    "Windows builds of Mesos currently do not support libev, the default event "
    "loop used by Mesos. To opt into using libevent, pass "
    "`-DENABLE_LIBEVENT=1` as an argument when you run CMake. NOTE: although "
    "the plan is to eventually transition to libevent, it is still in "
    "experimental support, and the code path is much less well-exercised."
    )
endif (WIN32 AND (NOT ENABLE_LIBEVENT))

# CMAKE MODULE SETUP.
#####################
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/3rdparty/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/3rdparty/libprocess/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/3rdparty/stout/cmake)
list(
  APPEND
  CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/3rdparty/libprocess/cmake/macros)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/src/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/src/master/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/src/slave/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/src/tests/cmake)

# Macros.
include(Common)
include(External)
include(PatchCommand)
include(Versions)
include(VsBuildCommand)

# Configuration.
include(MesosConfigure)

# SUBDIRECTORIES.
#################
add_subdirectory(3rdparty)

add_subdirectory(src)

add_custom_target(
  check ${STOUT_TESTS_TARGET}
  COMMAND ${PROCESS_TESTS_TARGET}
  DEPENDS ${PROCESS_TESTS_TARGET} ${STOUT_TESTS_TARGET}
  )
