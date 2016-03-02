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

include(ExternalProject)

add_subdirectory(libprocess)

# Downloads, configures, and compiles the third-party libraries for the mesos.

# Define sources of third-party dependencies.
#############################################
set(UPSTREAM_URL ${3RDPARTY_DEPENDENCIES})

set(REBUNDLED_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if (REBUNDLED)
  set(LEVELDB_URL   ${REBUNDLED_DIR}/leveldb-${LEVELDB_VERSION}.tar.gz)
  set(ZOOKEEPER_URL ${REBUNDLED_DIR}/zookeeper-${ZOOKEEPER_VERSION}.tar.gz)
else (REBUNDLED)
  set(LEVELDB_URL   ${UPSTREAM_URL}/leveldb-${LEVELDB_VERSION}.tar.gz)
  set(ZOOKEEPER_URL ${UPSTREAM_URL}/zookeeper-${ZOOKEEPER_VERSION}.tar.gz)
endif (REBUNDLED)

if (WIN32)
  # Similar to glog, on Windows, we use a different version of ZK. We'll
  # eventually want to upgrade the Mesos project but for now this version of ZK
  # does not come rebundled in the Mesos repository, so we need to get it from
  # our canonical package mirror.
  set(ZOOKEEPER_URL ${UPSTREAM_URL}/zookeeper-${ZOOKEEPER_VERSION}.tar.gz)
endif (WIN32)

# Define build/patch/configure commands for third-party libs.
#############################################################
if (NOT WIN32)
  set(ZOOKEEPER_CONFIG_CMD  cd ${ZOOKEEPER_C_ROOT} && ./configure --enable-shared=no --with-pic --srcdir=. --prefix=${ZOOKEEPER_LIB})
  set(ZOOKEEPER_BUILD_CMD   cd ${ZOOKEEPER_C_ROOT} && make)
  set(ZOOKEEPER_INSTALL_CMD cd ${ZOOKEEPER_C_ROOT} && make install)

  PATCH_CMD(
    ${MESOS_3RDPARTY_SRC}/zookeeper-${ZOOKEEPER_VERSION}.patch
    ZOOKEEPER_PATCH_CMD)
elseif (WIN32)
  # Set the patch command which will utilize patch.exe in temp location for no elevation prompt
  set(
    ZOOKEEPER_PATCH_CMD
    ${PATCHEXE_LOCATION} -p1 < ${MESOS_3RDPARTY_SRC}/zookeeper-${ZOOKEEPER_VERSION}.patch)

  VS_BUILD_CMD(
    ZOOKEEPER
    ${ZOOKEEPER_C_ROOT}/zookeeper-vs2015.sln
    ${CMAKE_BUILD_TYPE}
    ${ZOOKEEPER_C_ROOT}
    "zookeeper"
    )

  set(ZOOKEEPER_CONFIG_CMD  ${CMAKE_NOOP})
  set(ZOOKEEPER_INSTALL_CMD ${CMAKE_NOOP})
endif (NOT WIN32)

# Third-party libraries. Tell the build system how to pull in and build third-
# party libraries at compile time, using the ExternalProject_Add macro.
##############################################################################
ExternalProject_Add(
  ${ZOOKEEPER_TARGET}
  PREFIX            ${ZOOKEEPER_CMAKE_ROOT}
  PATCH_COMMAND     ${ZOOKEEPER_PATCH_CMD}
  CONFIGURE_COMMAND ${ZOOKEEPER_CONFIG_CMD}
  BUILD_COMMAND     ${ZOOKEEPER_BUILD_CMD}
  INSTALL_COMMAND   ${ZOOKEEPER_INSTALL_CMD}
  URL               ${ZOOKEEPER_URL}
  )

if (NOT WIN32)
  set(LEVELDB_CONFIG_CMD  cd ${LEVELDB_ROOT} && ./configure --prefix=${LEVELDB_ROOT}-lib)
  set(LEVELDB_BUILD_CMD   cd ${LEVELDB_ROOT} && make)
  set(LEVELDB_INSTALL_CMD cd ${LEVELDB_ROOT} && make install)
  PATCH_CMD(
    ${MESOS_3RDPARTY_SRC}/leveldb-${LEVELDB_VERSION}.patch
    LEVELDB_PATCH_CMD)

  ExternalProject_Add(
    ${LEVELDB_TARGET}
    PREFIX            ${LEVELDB_TARGET}
    PATCH_COMMAND     ${LEVELDB_PATCH_CMD}
    CONFIGURE_COMMAND ${CMAKE_NOOP}
    BUILD_COMMAND     ${LEVELDB_BUILD_CMD}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${LEVELDB_URL}
    )
endif (NOT WIN32)
