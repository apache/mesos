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

# Downloads, configures, and compiles the third party libraries for the process
# library (i.e., 3rdparty/libprocess/3rdparty).

# DEFINE SOURCES OF THIRD_PARTY DEPENDENCIES.
#############################################
set(REBUNDLED_DIR ${CMAKE_CURRENT_SOURCE_DIR})
if (REBUNDLED)
  set(BOOST_URL       ${REBUNDLED_DIR}/boost-${BOOST_VERSION}.tar.gz)
  set(GLOG_URL        ${REBUNDLED_DIR}/glog-${GLOG_VERSION}.tar.gz)
  set(PICOJSON_URL    ${REBUNDLED_DIR}/picojson-${PICOJSON_VERSION}.tar.gz)
  set(HTTP_PARSER_URL ${REBUNDLED_DIR}/ry-http-parser-${HTTP_PARSER_VERSION}.tar.gz)
  set(LIBEV_URL       ${REBUNDLED_DIR}/libev-${LIBEV_VERSION}.tar.gz)
else (REBUNDLED)
  # TODO(hausdorff): depends on the github mirror, should remove when possible.
  set(BOOST_URL       https://github.com/apache/mesos/raw/master/3rdparty/libprocess/3rdparty/boost-${BOOST_VERSION}.tar.gz)
  set(GLOG_URL        https://google-glog.googlecode.com/files/glog-${GLOG_VERSION}.tar.gz)
  # NOTE: This url can't be versioned like the others, because it depends on
  #       specific commit, and isn't in our upstream.
  set(PICOJSON_URL    https://github.com/kazuho/picojson/tarball/4f93734ade33ea0f5e5b4de35fc6b2c324a8dca6)
  # TODO(hausdorff): depends on the github mirror, should remove when possible.
  set(HTTP_PARSER_URL https://github.com/apache/mesos/raw/master/3rdparty/libprocess/3rdparty/ry-http-parser-${HTTP_PARSER_VERSION}.tar.gz)
  # TODO(hausdorff): depends on the github mirror, should remove when possible.
  set(LIBEV_URL       https://github.com/apache/mesos/raw/master/3rdparty/libprocess/3rdparty/libev-${LIBEV_VERSION}.tar.gz)
endif (REBUNDLED)

# DEFINE BUILD/PATCH/CONFIGURE COMMANDS FOR THIRD-PARTY LIBS.
#############################################################
if (NOT WIN32)
  set(GLOG_CONFIG_CMD  ${GLOG_ROOT}/src/../configure --prefix=${GLOG_LIB})
  set(GLOG_BUILD_CMD   make)
  set(GLOG_INSTALL_CMD make install)
  # Patch glog to deal with a problem that appears when compiling on clang
  # under the C++11 standard. cf. MESOS-860, MESOS-966.
  PATCH_CMD(${PROCESS_3RD_SRC}/glog-0.3.3.patch GLOG_PATCH_CMD)

  set(RY_BUILD_CMD   make -C ${HTTP_PARSER_ROOT})
  set(RY_INSTALL_CMD ar -rv libhttp_parser.a ${HTTP_PARSER_ROOT}/http_parser_g.o)

  set(LIBEV_CONFIG_CMD ${LIBEV_ROOT}/configure)
  set(LIBEV_BUILD_CMD  make)
  # Patch libev to keep it from reaping child processes.
  PATCH_CMD(${PROCESS_3RD_SRC}/libev-4.15.patch LIBEV_PATCH_CMD)
endif (NOT WIN32)

# THIRD-PARTY LIBRARIES. Tell the build system how to pull in and build third-
# party libraries at compile time, using the ExternalProject_Add macro.
##############################################################################
ExternalProject_Add(
  ${BOOST_TARGET}
  PREFIX            ${BOOST_CMAKE_ROOT}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  URL               "${BOOST_URL}"
  )

ExternalProject_Add(
  ${GLOG_TARGET}
  PREFIX            ${GLOG_CMAKE_ROOT}
  # Quotes are important in the following commands. Building glog on WIN32
  # must be done in Visual Studio, and in that case, these commands must be
  # nops. But if you take out the quote marks, CMake will treat this as an
  # empty build command, and will attempt to build glog as a CMake project.
  PATCH_COMMAND     "${GLOG_PATCH_CMD}"
  CONFIGURE_COMMAND "${GLOG_CONFIG_CMD}"
  BUILD_COMMAND     "${GLOG_BUILD_CMD}"
  INSTALL_COMMAND   "${GLOG_INSTALL_CMD}"
  URL               "${GLOG_URL}"
  )

ExternalProject_Add(
  ${PICOJSON_TARGET}
  PREFIX            ${PICOJSON_CMAKE_ROOT}
  DOWNLOAD_NAME     ${PICOJSON_TARGET}.tar.gz
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  URL               "${PICOJSON_URL}"
  )

ExternalProject_Add(
  ${HTTP_PARSER_TARGET}
  PREFIX            ${HTTP_PARSER_CMAKE_ROOT}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     "${RY_BUILD_CMD}"
  INSTALL_COMMAND   "${RY_INSTALL_CMD}"
  URL               "${HTTP_PARSER_URL}"
  )

ExternalProject_Add(
  ${LIBEV_TARGET}
  PREFIX            ${LIBEV_CMAKE_ROOT}
  PATCH_COMMAND     "${LIBEV_PATCH_CMD}"
  CONFIGURE_COMMAND "${LIBEV_CONFIG_CMD}"
  BUILD_COMMAND     "${LIBEV_BUILD_CMD}"
  INSTALL_COMMAND   ""
  URL               "${LIBEV_URL}"
  )

# THIRD-PARTY LIBRARIES FOR TESTS.
##################################
if (REBUNDLED)
  set(GMOCK_URL       ${REBUNDLED_DIR}/gmock-${GMOCK_VERSION}.tar.gz)
else (REBUNDLED)
  # TODO(hausdorff): depends on the github mirror, should remove when possible.
  set(GMOCK_URL       https://github.com/apache/mesos/raw/master/3rdparty/libprocess/3rdparty/gmock-${GMOCK_VERSION}.tar.gz)
endif (REBUNDLED)

if (NOT WIN32)
  set(GMOCK_CONFIG_CMD  ${GMOCK_ROOT}/configure --prefix=${GMOCK_ROOT}-lib/lib)
  set(GMOCK_BUILD_CMD   make)
endif (NOT WIN32)

ExternalProject_Add(
  ${GMOCK_TARGET}
  PREFIX            ${GMOCK_CMAKE_ROOT}
  CONFIGURE_COMMAND "${GMOCK_CONFIG_CMD}"
  BUILD_COMMAND     "${GMOCK_BUILD_CMD}"
  INSTALL_COMMAND   ""
  URL               "${GMOCK_URL}"
  )
