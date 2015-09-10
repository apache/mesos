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

# Downloads, configures, and compiles the third-party libraries for the process
# library (i.e., 3rdparty/libprocess/3rdparty).

# Define sources of third-party dependencies.
#############################################
set(REBUNDLED_DIR ${CMAKE_CURRENT_SOURCE_DIR})
if (REBUNDLED)
  set(BOOST_URL       ${REBUNDLED_DIR}/boost-${BOOST_VERSION}.tar.gz)
  set(GLOG_URL        ${REBUNDLED_DIR}/glog-${GLOG_VERSION}.tar.gz)
  set(PICOJSON_URL    ${REBUNDLED_DIR}/picojson-${PICOJSON_VERSION}.tar.gz)
  set(HTTP_PARSER_URL ${REBUNDLED_DIR}/ry-http-parser-${HTTP_PARSER_VERSION}.tar.gz)
  set(LIBEV_URL       ${REBUNDLED_DIR}/libev-${LIBEV_VERSION}.tar.gz)
else (REBUNDLED)
  # TODO(hausdorff): (MESOS-3395) depends on the github mirror, should remove
  # when possible.
  set(BOOST_URL       https://github.com/apache/mesos/raw/master/3rdparty/libprocess/3rdparty/boost-${BOOST_VERSION}.tar.gz)
  set(GLOG_URL        https://google-glog.googlecode.com/files/glog-${GLOG_VERSION}.tar.gz)
  # NOTE: This url can't be versioned like the others, because it depends on
  #       specific commit, and isn't in our upstream.
  set(PICOJSON_URL    https://github.com/kazuho/picojson/tarball/4f93734ade33ea0f5e5b4de35fc6b2c324a8dca6)
  # TODO(hausdorff): (MESOS-3395) depends on the github mirror, should remove
  # when possible.
  set(HTTP_PARSER_URL https://github.com/apache/mesos/raw/master/3rdparty/libprocess/3rdparty/ry-http-parser-${HTTP_PARSER_VERSION}.tar.gz)
  # TODO(hausdorff): (MESOS-3395) depends on the github mirror, should remove
  # when possible.
  set(LIBEV_URL       https://github.com/apache/mesos/raw/master/3rdparty/libprocess/3rdparty/libev-${LIBEV_VERSION}.tar.gz)
endif (REBUNDLED)

if (WIN32)
  # TODO(hausdorff): (MESOS-3394) points at a random tarball in my
  # (hausdorff's) repository. We need a more permanent solution for this
  # eventually.
  set(CURL_URL https://github.com/hausdorff/3rdparty/raw/master/curl-static-${CURL_VERSION}.tar.gz)
endif (WIN32)

# Define build/patch/configure commands for third-party libs.
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

# Third-party libraries. Tell the build system how to pull in and build third-
# party libraries at compile time, using the ExternalProject_Add macro.
##############################################################################
# NOTE: On ExternalProject_Add calls in the next section, we pass some
# quoted values into `ExternalProject_Add`. We do this because some of these
# projects can't (or shouldn't) be built from the command line (for various
# reasons), and we need to get creative about what values we pass in to make
# sure these operations are no-ops.
#
# There are two flavors of work-around here. The first we use to avoid building
# third-party projects (like Boost) that never need to be built for Mesos. This
# looks like this:
#
#     BUILD_COMMAND ""
#
# In most of the major releases of CMake, if we don't set parameters like
# `BUILD_COMMAND`, we will trigger the default behavior of that parameter;
# usually, CMake attempts to configure/build/install/etc. the third-party
# project as if it were a CMake (or perhaps make) project. Since these projects
# are not CMake projects, they will error out. This work-around lets us avoid
# that fate altogether.
#
# The second work-around allows us to build a project except on Windows. It
# looks like this:
#
#     BUILD_COMMAND "${GLOG_BUILD_CMD}"
#
# The problem is that there is currently no way to build glog on Windows from
# the command line. But, if GLOG_BUILD_CMD is not set (or if it's an empty
# list), then we run into the same problem as last time; CMake sees that
# `BUILD_COMMAND` is unset, and will default to the "normal" behavior of trying
# to configure/build/install the project as if it was a CMake project. (Since
# it is not, it will of course error out on Windows.)
#
# The work-around is to add quotes to the outside of the variable. In this case,
# ${GLOG_BUILD_CMD} will expand to unset, which will cause us to pass the empty
# string as a parameter to the function. This of course is different from not
# setting the parameter value at all, and hence the work-around works.
ExternalProject_Add(
  ${BOOST_TARGET}
  PREFIX            ${BOOST_CMAKE_ROOT}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  URL               ${BOOST_URL}
  )

ExternalProject_Add(
  ${GLOG_TARGET}
  PREFIX            ${GLOG_CMAKE_ROOT}
  PATCH_COMMAND     "${GLOG_PATCH_CMD}"
  CONFIGURE_COMMAND "${GLOG_CONFIG_CMD}"
  BUILD_COMMAND     "${GLOG_BUILD_CMD}"
  INSTALL_COMMAND   "${GLOG_INSTALL_CMD}"
  URL               ${GLOG_URL}
  DOWNLOAD_NAME     glog-${GLOG_VERSION}.tar.gz
  )

ExternalProject_Add(
  ${PICOJSON_TARGET}
  PREFIX            ${PICOJSON_CMAKE_ROOT}
  DOWNLOAD_NAME     ${PICOJSON_TARGET}.tar.gz
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  URL               ${PICOJSON_URL}
  )

ExternalProject_Add(
  ${HTTP_PARSER_TARGET}
  PREFIX            ${HTTP_PARSER_CMAKE_ROOT}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     "${RY_BUILD_CMD}"
  INSTALL_COMMAND   "${RY_INSTALL_CMD}"
  URL               ${HTTP_PARSER_URL}
  )

ExternalProject_Add(
  ${LIBEV_TARGET}
  PREFIX            ${LIBEV_CMAKE_ROOT}
  PATCH_COMMAND     "${LIBEV_PATCH_CMD}"
  CONFIGURE_COMMAND "${LIBEV_CONFIG_CMD}"
  BUILD_COMMAND     "${LIBEV_BUILD_CMD}"
  INSTALL_COMMAND   ""
  URL               ${LIBEV_URL}
  )

# WINDOWS THIRD-PARTY LIBRARIES. Windows has no package manager, so we download
# them here.
###############################################################################
if (WIN32)
  # TODO(hausdorff): maybe try to incorporate this into findpackage for Windows
  ExternalProject_Add(
    ${CURL_TARGET}
    PREFIX            ${CURL_CMAKE_ROOT}
    PATCH_COMMAND     ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     ""
    INSTALL_COMMAND   ""
    URL               ${CURL_URL}
  )
endif (WIN32)

# THIRD-PARTY LIBRARIES FOR TESTS.
##################################
if (REBUNDLED)
  set(GMOCK_URL    ${REBUNDLED_DIR}/gmock-${GMOCK_VERSION}.tar.gz)
  set(PROTOBUF_URL ${REBUNDLED_DIR}/protobuf-${PROTOBUF_VERSION}.tar.gz)
else (REBUNDLED)
  # TODO(hausdorff): (MESOS-3395) depends on the github mirror, should remove
  # when possible.
  set(GMOCK_URL    https://github.com/apache/mesos/raw/master/3rdparty/libprocess/3rdparty/gmock-${GMOCK_VERSION}.tar.gz)
  # TODO(hausdorff): (MESOS-3395) depends on the github mirror, should remove
  # when possible.
  set(PROTOBUF_URL https://github.com/hausdorff/mesos/raw/test_cmake/3rdparty/libprocess/3rdparty/protobuf-${PROTOBUF_VERSION}.tar.gz)
endif (REBUNDLED)

if (APPLE)
  # GTEST on OSX needs its own tr1 tuple.
  set(GMOCK_BUILD_CMD  make CPPFLAGS=-DGTEST_USE_OWN_TR1_TUPLE -DGTEST_LANG_CXX11)
  set(GMOCK_CONFIG_CMD ${GMOCK_ROOT}/configure --prefix=${GMOCK_ROOT}-lib/lib)
elseif (NOT WIN32)
  set(GMOCK_CONFIG_CMD ${GMOCK_ROOT}/configure --prefix=${GMOCK_ROOT}-lib/lib)
  set(GMOCK_BUILD_CMD  make)
endif (APPLE)

if (NOT WIN32)
  set(PROTOBUF_CONFIG_CMD  ${PROTOBUF_ROOT}/src/../configure --prefix=${PROTOBUF_LIB})
  set(PROTOBUF_BUILD_CMD   make)
  set(PROTOBUF_INSTALL_CMD make install)
endif (NOT WIN32)

ExternalProject_Add(
  # See note about third-party libraries above to understand quirky actual
  # parameters passed in here.
  ${GMOCK_TARGET}
  PREFIX            ${GMOCK_CMAKE_ROOT}
  CONFIGURE_COMMAND "${GMOCK_CONFIG_CMD}"
  BUILD_COMMAND     "${GMOCK_BUILD_CMD}"
  INSTALL_COMMAND   ""
  URL               ${GMOCK_URL}
  )

ExternalProject_Add(
  # See note about third-party libraries above to understand quirky actual
  # parameters passed in here.
  ${PROTOBUF_TARGET}
  PREFIX            ${PROTOBUF_CMAKE_ROOT}
  PATCH_COMMAND     "${PROTOBUF_PATCH_CMD}"
  CONFIGURE_COMMAND "${PROTOBUF_CONFIG_CMD}"
  BUILD_COMMAND     "${PROTOBUF_BUILD_CMD}"
  INSTALL_COMMAND   "${PROTOBUF_INSTALL_CMD}"
  URL               ${PROTOBUF_URL}
  )

# STOUT TESTING INFRASTRUCTURE.
###############################
list(
  APPEND CMAKE_MODULE_PATH
  ${CMAKE_SOURCE_DIR}/3rdparty/libprocess/3rdparty/stout/cmake
  )
include(StoutTestsConfigure)
add_subdirectory(stout/tests)
