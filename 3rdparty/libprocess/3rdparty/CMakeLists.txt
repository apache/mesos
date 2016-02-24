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
set(UPSTREAM_URL https://github.com/3rdparty/mesos-3rdparty/raw/master)
set(REBUNDLED_DIR ${CMAKE_CURRENT_SOURCE_DIR})
if (REBUNDLED)
  set(BOOST_URL       ${REBUNDLED_DIR}/boost-${BOOST_VERSION}.tar.gz)
  set(GLOG_URL        ${REBUNDLED_DIR}/glog-${GLOG_VERSION}.tar.gz)
  set(PICOJSON_URL    ${REBUNDLED_DIR}/picojson-${PICOJSON_VERSION}.tar.gz)
  set(HTTP_PARSER_URL ${REBUNDLED_DIR}/ry-http-parser-${HTTP_PARSER_VERSION}.tar.gz)
  set(LIBEV_URL       ${REBUNDLED_DIR}/libev-${LIBEV_VERSION}.tar.gz)
else (REBUNDLED)
  set(BOOST_URL       ${UPSTREAM_URL}/boost-${BOOST_VERSION}.tar.gz)
  set(GLOG_URL        ${UPSTREAM_URL}/glog-${GLOG_VERSION}.tar.gz)
  set(PICOJSON_URL    ${UPSTREAM_URL}/picojson-${PICOJSON_VERSION}.tar.gz)
  set(HTTP_PARSER_URL ${UPSTREAM_URL}/ry-http-parser-${HTTP_PARSER_VERSION}.tar.gz)
  set(LIBEV_URL       ${UPSTREAM_URL}/libev-${LIBEV_VERSION}.tar.gz)
endif (REBUNDLED)

# NOTE: libevent doesn't come rebundled, so this URL is always the same. But,
# it's only downloaded if `ENABLE_LIBEVENT` is set.
set(LIBEVENT_URL ${UPSTREAM_URL}/libevent-release-${LIBEVENT_VERSION}.tar.gz)

if (WIN32)
  # TODO(hausdorff): (MESOS-3394) Upgrade Windows to use glog v0.3.5, as that
  # release will contain fixes that will allow us to build glog on Windows.
  #
  # NOTE: hausdorff has ported glog v0.3.4 to support compilation with MSVC
  # 1900 (it works on MSVC 1800, but not 1900). The patch has been submitted
  # to the glog project which you can see in pull request #43[1].
  #
  # Note also that this tarball actually points directly at the branch the
  # PR is issued from, and uses the GitHub REST API to get a tarball of it.
  # We think this is stictly better than pulling a tarball out of 3rdparty, or
  # running `patch` on a tarball version of the vanilla 0.3.4 release, because
  # it means that this code will automatically be up-to-date, and to transition
  # to the next release of glog (when this will presumably be fixed), we need
  # only change this line.
  #
  # Finally, for our third-party libraries, MSVC 1800 and 1900 are not
  # linker-compatible, so it's important for us to be able to build glog
  # (as well as all the other third-party dependencies) on MSVC 1900.
  #
  # [1] https://github.com/google/glog/pull/43
  set(GLOG_URL     https://github.com/hausdorff/glog/tarball/glog-${GLOG_VERSION}-msvc1900)
  set(CURL_URL     ${UPSTREAM_URL}/curl-static-${CURL_VERSION}.tar.gz)
  set(LIBAPR_URL   ${UPSTREAM_URL}/libapr-${LIBAPR_VERSION}.tar.gz)
endif (WIN32)

# Define build/patch/configure commands for third-party libs.
#############################################################
# NOTE: (fix for MESOS-3250) A few third-party libraries (libev, gmock) do not
# have `make install` commands available, so below we have to add our own
# "install" commands.
#
# The reason is: if we do not, we get runtime library load problems on OS X. In
# particular, `dydl` will look for these libraries at the prefix we passed to
# `configure` (or in `/usr/local` if we did not pass a prefix in), but since
# they don't have a `make install` step, they never get placed in the prefix
# folder.
#
# Our solution is to:
#   (1) make a lib directory inside the Mesos folder for each of the libraries
#       that has no install step, and
#   (2) copy all such libraries into their respective directories.
#
# (Note that step (1) is not only convenient, but important: make will add a
# `lib` to the end of your prefix path when linking, and since the built
# libraries end up in a `.libs` folder, it's not enough to simply pass the
# build directory into `configure` as a prefix; so if we're going to move the
# libraries, we might as well move them to a library folder.)
if (NOT WIN32)
  set(GLOG_CONFIG_CMD  ${GLOG_ROOT}/src/../configure --prefix=${GLOG_LIB_ROOT})
  set(GLOG_BUILD_CMD   make)
  set(GLOG_INSTALL_CMD make install)
  # Patch glog to deal with a problem that appears when compiling on clang
  # under the C++11 standard. cf. MESOS-860, MESOS-966.
  PATCH_CMD(${PROCESS_3RD_SRC}/glog-0.3.3.patch GLOG_PATCH_CMD)

  # NOTE: `libev` is "installed" into a lib directory, see "NOTE: (fix for
  # MESOS-3250)" comment above for explanation.
  set(LIBEV_CONFIG_CMD  ${LIBEV_ROOT}/configure --prefix=${LIBEV_ROOT}-lib)
  set(LIBEV_BUILD_CMD   make)
  set(LIBEV_INSTALL_CMD mkdir -p ${LIBEV_LIB_ROOT} && cp -r ${LIBEV_ROOT}-build/.libs/. ${LIBEV_LIB_ROOT})
  # Patch libev to keep it from reaping child processes.
  PATCH_CMD(${PROCESS_3RD_SRC}/libev-4.15.patch LIBEV_PATCH_CMD)

  set(LIBEVENT_INSTALL_CMD mkdir -p ${LIBEVENT_LIB_ROOT} && cp -r ${LIBEVENT_ROOT}-build/lib/. ${LIBEVENT_LIB_DIR} && cp -r ${LIBEVENT_ROOT}-build/include/. ${LIBEVENT_INCLUDE_DIR} && cp -r ${LIBEVENT_ROOT}/include/. ${LIBEVENT_INCLUDE_DIR})
elseif (WIN32)
  set(GLOG_PATCH_CMD   ${CMAKE_NOOP})
  set(GLOG_CONFIG_CMD  ${CMAKE_NOOP})
  set(GLOG_INSTALL_CMD ${CMAKE_NOOP})
  VS_BUILD_CMD(
      GLOG
      ${GLOG_ROOT}/google-glog.sln
      ${CMAKE_BUILD_TYPE}
      "libglog")

  set(LIBEVENT_INSTALL_CMD ${CMAKE_NOOP})

  set(LIBAPR_INSTALL_CMD ${CMAKE_NOOP})

  set(LIBEVENT_C_FLAGS "/MTd /Zi")
  set(LIBEVENT_CMAKE_ARGS
    ${LIBEVENT_CMAKE_ARGS}
    -DCMAKE_C_FLAGS_DEBUG=${LIBEVENT_C_FLAGS}
    -DCMAKE_C_FLAGS_RELEASE=${LIBEVENT_C_FLAGS}
    -DEVENT__DISABLE_OPENSSL=TRUE
    )
endif (NOT WIN32)

set(RY_UPDATE_CMD ${CMAKE_COMMAND} -E copy ${PROCESS_3RD_SRC}/http-parser/CMakeLists.txt.template ${HTTP_PARSER_ROOT}/CMakeLists.txt)

set(LIBEVENT_CMAKE_ARGS
  ${LIBEVENT_CMAKE_ARGS}
  -LH
  -DEVENT__HAVE_SYS_STAT_H=1
  -DBUILD_SHARED_LIBS=${BUILD_SHARED_LIBS}
  )

# Third-party libraries. Tell the build system how to pull in and build third-
# party libraries at compile time, using the ExternalProject_Add macro.
##############################################################################
ExternalProject_Add(
  ${BOOST_TARGET}
  PREFIX            ${BOOST_CMAKE_ROOT}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${BOOST_URL}
  )

ExternalProject_Add(
  ${GLOG_TARGET}
  PREFIX            ${GLOG_CMAKE_ROOT}
  PATCH_COMMAND     ${GLOG_PATCH_CMD}
  CONFIGURE_COMMAND ${GLOG_CONFIG_CMD}
  BUILD_COMMAND     ${GLOG_BUILD_CMD}
  INSTALL_COMMAND   ${GLOG_INSTALL_CMD}
  URL               ${GLOG_URL}
  DOWNLOAD_NAME     glog-${GLOG_VERSION}.tar.gz
  )

ExternalProject_Add(
  ${PICOJSON_TARGET}
  PREFIX            ${PICOJSON_CMAKE_ROOT}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${PICOJSON_URL}
  )

ExternalProject_Add(
  ${HTTP_PARSER_TARGET}
  PREFIX            ${HTTP_PARSER_CMAKE_ROOT}
  UPDATE_COMMAND    ${RY_UPDATE_CMD}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${HTTP_PARSER_URL}
  )

if (NOT ENABLE_LIBEVENT)
  ExternalProject_Add(
    ${LIBEV_TARGET}
    PREFIX            ${LIBEV_CMAKE_ROOT}
    PATCH_COMMAND     ${LIBEV_PATCH_CMD}
    CONFIGURE_COMMAND ${LIBEV_CONFIG_CMD}
    BUILD_COMMAND     ${LIBEV_BUILD_CMD}
    INSTALL_COMMAND   ${LIBEV_INSTALL_CMD}
    URL               ${LIBEV_URL}
    )
elseif (ENABLE_LIBEVENT)
  ExternalProject_Add(
    ${LIBEVENT_TARGET}
    PREFIX          ${LIBEVENT_CMAKE_ROOT}
    CMAKE_ARGS      ${LIBEVENT_CMAKE_ARGS}
    INSTALL_COMMAND ${LIBEVENT_INSTALL_CMD}
    URL             ${LIBEVENT_URL}
    )
endif (NOT ENABLE_LIBEVENT)

if (WIN32)
  ExternalProject_Add(
    ${LIBAPR_TARGET}
    PREFIX          ${LIBAPR_CMAKE_ROOT}
    INSTALL_COMMAND ${LIBAPR_INSTALL_CMD}
    URL             ${LIBAPR_URL}
    )
endif (WIN32)

# WINDOWS THIRD-PARTY LIBRARIES. Windows has no package manager, so we download
# them here.
###############################################################################
if (WIN32)
  # TODO(hausdorff): maybe try to incorporate this into findpackage for Windows
  ExternalProject_Add(
    ${CURL_TARGET}
    PREFIX            ${CURL_CMAKE_ROOT}
    PATCH_COMMAND     ${CMAKE_NOOP}
    CONFIGURE_COMMAND ${CMAKE_NOOP}
    BUILD_COMMAND     ${CMAKE_NOOP}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${CURL_URL}
  )
endif (WIN32)

# THIRD-PARTY LIBRARIES FOR TESTS.
##################################
if (REBUNDLED)
  set(GMOCK_URL    ${REBUNDLED_DIR}/gmock-${GMOCK_VERSION}.tar.gz)
  set(PROTOBUF_URL ${REBUNDLED_DIR}/protobuf-${PROTOBUF_VERSION}.tar.gz)
else (REBUNDLED)
  set(GMOCK_URL    ${UPSTREAM_URL}/gmock-${GMOCK_VERSION}.tar.gz)
  set(PROTOBUF_URL ${UPSTREAM_URL}/protobuf-${PROTOBUF_VERSION}.tar.gz)
endif (REBUNDLED)

if (WIN32)
  # TODO(hausdorff): (MESOS-3453) this is a patched version of the protobuf
  # library that compiles on Windows. We need to either send this as a PR back
  # to the protobuf project, or we need to apply these changes to our existing
  # protobuf tarball in the patch step.
  set(PROTOBUF_URL https://github.com/haosdent/3rdparty-packages/raw/master/protobuf-${PROTOBUF_VERSION}.tar.gz)
endif (WIN32)

# NOTE: `gmock` is "installed" into a lib directory, see "NOTE: (fix for
# MESOS-3250)" comment above for explanation.
if (NOT WIN32)
  set(GMOCK_INSTALL_CMD mkdir -p ${GMOCK_ROOT}-lib/lib && cp -r ${GMOCK_ROOT}-build/. ${GMOCK_ROOT}-lib/lib && cp -r ${GMOCK_ROOT}-build/gtest/. ${GMOCK_ROOT}-lib/lib)
elseif (WIN32)
  set(GMOCK_INSTALL_CMD ${CMAKE_NOOP})
endif (NOT WIN32)

if (NOT WIN32)
  set(PROTOBUF_PATCH_CMD   ${CMAKE_NOOP})
  set(PROTOBUF_CONFIG_CMD  ${PROTOBUF_ROOT}/src/../configure --prefix=${PROTOBUF_LIB_ROOT})
  set(PROTOBUF_BUILD_CMD   make)
  set(PROTOBUF_INSTALL_CMD make install)
elseif (WIN32)
  set(PROTOBUF_PATCH_CMD   ${CMAKE_NOOP})
  set(PROTOBUF_CONFIG_CMD  ${CMAKE_NOOP})
  set(PROTOBUF_INSTALL_CMD ${CMAKE_NOOP})
  VS_BUILD_CMD(
      PROTOBUF
      ${PROTOBUF_ROOT}/vsprojects/protobuf.sln
      ${CMAKE_BUILD_TYPE}
      "libprotobuf libprotoc protoc")
endif (NOT WIN32)

# NOTE: An implicit consequence of the following code is that on non-Windows
# platforms, gmock and gtest are assumed to be CMake projects, and are thus
# configured and built using default CMake commands. The reason is that on
# non-Windows platforms, we choose to set `GMOCK_CONFIG_CMD` and
# `GMOCK_BUILD_CMD` with stub commands, which cause CMake to "fall back" to
# trying to build them with CMake.
ExternalProject_Add(
  ${GMOCK_TARGET}
  PREFIX            ${GMOCK_CMAKE_ROOT}
  CMAKE_ARGS        -DBUILD_SHARED_LIBS=FALSE
  CONFIGURE_COMMAND ${GMOCK_CONFIG_CMD}
  BUILD_COMMAND     ${GMOCK_BUILD_CMD}
  INSTALL_COMMAND   ${GMOCK_INSTALL_CMD}
  URL               ${GMOCK_URL}
  )

ExternalProject_Add(
  ${PROTOBUF_TARGET}
  PREFIX            ${PROTOBUF_CMAKE_ROOT}
  PATCH_COMMAND     ${PROTOBUF_PATCH_CMD}
  CONFIGURE_COMMAND ${PROTOBUF_CONFIG_CMD}
  BUILD_COMMAND     ${PROTOBUF_BUILD_CMD}
  INSTALL_COMMAND   ${PROTOBUF_INSTALL_CMD}
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
