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

# URLs for each downloaded or bundled third-party dependency.
#############################################################
if (REBUNDLED)
  set(FETCH_URL ${CMAKE_CURRENT_SOURCE_DIR})
else ()
  set(FETCH_URL ${3RDPARTY_DEPENDENCIES})
endif ()

# TODO(andschwa): Move each URL to the dependency itself.
set(BOOST_URL           ${FETCH_URL}/boost-${BOOST_VERSION}.tar.gz)
set(BZIP2_URL           ${FETCH_URL}/bzip2-${BZIP2_VERSION}.tar.gz)
set(CONCURRENTQUEUE_URL ${FETCH_URL}/concurrentqueue-${CONCURRENTQUEUE_VERSION}.tar.gz)
set(CSI_V0_URL          ${FETCH_URL}/csi-${CSI_V0_VERSION}.tar.gz)
set(CSI_V1_URL          ${FETCH_URL}/csi-${CSI_V1_VERSION}.tar.gz)
set(ELFIO_URL           ${FETCH_URL}/elfio-${ELFIO_VERSION}.tar.gz)
set(GLOG_URL            ${FETCH_URL}/glog-${GLOG_VERSION}.tar.gz)
set(GOOGLETEST_URL      ${FETCH_URL}/googletest-release-${GOOGLETEST_VERSION}.tar.gz)
set(GRPC_URL            ${FETCH_URL}/grpc-${GRPC_VERSION}.tar.gz)
set(HTTP_PARSER_URL     ${FETCH_URL}/http-parser-${HTTP_PARSER_VERSION}.tar.gz)
set(JEMALLOC_URL        ${FETCH_URL}/jemalloc-${JEMALLOC_VERSION}.tar.gz)
set(LEVELDB_URL         ${FETCH_URL}/leveldb-${LEVELDB_VERSION}.tar.gz)
set(LIBARCHIVE_URL      ${FETCH_URL}/libarchive-${LIBARCHIVE_VERSION}.tar.gz)
set(LIBEV_URL           ${FETCH_URL}/libev-${LIBEV_VERSION}.tar.gz)
set(LIBSECCOMP_URL      ${FETCH_URL}/libseccomp-${LIBSECCOMP_VERSION}.tar.gz)
set(NVML_URL            ${FETCH_URL}/nvml-${NVML_VERSION}.tar.gz)
set(PICOJSON_URL        ${FETCH_URL}/picojson-${PICOJSON_VERSION}.tar.gz)
set(PROTOBUF_URL        ${FETCH_URL}/protobuf-${PROTOBUF_VERSION}.tar.gz)
set(RAPIDJSON_URL       ${FETCH_URL}/rapidjson-${RAPIDJSON_VERSION}.tar.gz)
set(RE2_URL             ${FETCH_URL}/re2-${RE2_VERSION}.tar.gz)
set(XZ_URL              ${FETCH_URL}/xz-${XZ_VERSION}-modified.tar.gz)
set(ZOOKEEPER_URL       ${FETCH_URL}/zookeeper-${ZOOKEEPER_VERSION}.tar.gz)

# NOTE: libevent doesn't come rebundled, so this URL is always the same. But,
# it's only downloaded if `ENABLE_LIBEVENT` is set.
set(LIBEVENT_URL ${3RDPARTY_DEPENDENCIES}/libevent-release-${LIBEVENT_VERSION}.tar.gz)

if (WIN32)
  # NOTE: These dependencies are only rebundled on Windows because they
  # are available as installable packages on Linux; so they live
  # exclusively in the 3rdparty repo.
  set(CURL_URL    ${3RDPARTY_DEPENDENCIES}/curl-${CURL_VERSION}.tar.gz)
  set(LIBAPR_URL  ${3RDPARTY_DEPENDENCIES}/libapr-${LIBAPR_VERSION}.tar.gz)
  set(ZLIB_URL    ${3RDPARTY_DEPENDENCIES}/zlib-${ZLIB_VERSION}.tar.gz)
  set(SASL2_URL   ${3RDPARTY_DEPENDENCIES}/cyrus-sasl-${SASL2_VERSION}.tar.gz)
  set(WCLAYER_URL ${3RDPARTY_DEPENDENCIES}/wclayer.exe)
endif ()

if (NOT WIN32)
  # On platforms like FreeBSD where gmake is not the default, we need
  # to look for gmake explicitly to make sure LevelDB can build.
  # TODO(dforsyth): Remove this once LevelDB is upgraded to a version
  # that uses CMake. See MESOS-8757.
  find_program(
    MAKE_PROGRAM
    NAMES gmake make
    DOCS "External 'make' executable.")

  if (NOT MAKE_PROGRAM)
    message(FATAL_ERROR "Could not find an external 'make' executable.")
  endif ()
endif ()

# Helper utilities for dealing with third-party dependencies.
#############################################################
# ExternalProject: Create custom targets to build projects in external trees.
# https://cmake.org/cmake/help/latest/module/ExternalProject.html
include(ExternalProject)

# This adds our FindApr and FindSvn packages.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/stout/cmake)

# This adds our `EXTERNAL()` and `PATCH_CMD()` functions.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(External)
include(PatchCommand)

# Variables for current specified linkage.
# NOTE: Not all third-party dependencies will respect this.
if (BUILD_SHARED_LIBS)
  set(LIBRARY_LINKAGE SHARED)
  set(LIBRARY_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})
else ()
  set(LIBRARY_LINKAGE STATIC)
  set(LIBRARY_SUFFIX ${CMAKE_STATIC_LIBRARY_SUFFIX})
endif ()

if (WIN32 AND BUILD_SHARED_LIBS)
  set(3RDPARTY_LIBS_INSTALL_DIR ${MESOS_INSTALL_RUNTIME})
else()
  set(3RDPARTY_LIBS_INSTALL_DIR ${MESOS_INSTALL_LIBRARIES})
endif()

set(LIBPROCESS_INSTALL_RUNTIME ${MESOS_INSTALL_RUNTIME})
set(LIBPROCESS_INSTALL_LIBRARIES ${MESOS_INSTALL_LIBRARIES})

# Sets a variable CMAKE_NOOP as noop operation.
#
# NOTE: This is especially important when building third-party libraries on
# Windows; the default behavior of `ExternalProject` is to try to assume that
# third-party libraries can be configured/built/installed with CMake, so in
# cases where this isn't true, we have to "trick" CMake into skipping those
# steps by giving it a noop command to run instead.
set(CMAKE_NOOP ${CMAKE_COMMAND} -E echo)

# This `CMAKE_FORWARD_ARGS` variable is sent as the `CMAKE_ARGS` argument to
# the `ExternalProject_Add` macro (along with any per-project arguments), and
# is used when the external project is configured as a CMake project.
# If either the `CONFIGURE_COMMAND` or `BUILD_COMMAND` arguments of
# `ExternalProject_Add` are used, then the `CMAKE_ARGS` argument will be
# ignored.
#
# NOTE: The CMAKE_GENERATOR_TOOLSET is impliticly set by `ExternalProject_Add`,
# and cannot be included twice.
list(APPEND CMAKE_FORWARD_ARGS
  # TODO(andschwa): Set the CMAKE_GENERATOR explicitly as an argmuent to
  # `ExternalProject_Add`.
  -G${CMAKE_GENERATOR}
  -DCMAKE_POSITION_INDEPENDENT_CODE=${CMAKE_POSITION_INDEPENDENT_CODE}
  -DBUILD_SHARED_LIBS=${BUILD_SHARED_LIBS})

# This only matters for single-configuration generators.
# E.g. Makefile, but not Visual Studio.
if (NOT "${CMAKE_BUILD_TYPE}" STREQUAL "")
  list(APPEND CMAKE_FORWARD_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
endif ()

foreach (lang C CXX)
  list(APPEND CMAKE_${lang}_FORWARD_ARGS
    ${CMAKE_FORWARD_ARGS}
    -DCMAKE_${lang}_COMPILER=${CMAKE_${lang}_COMPILER}
    -DCMAKE_${lang}_COMPILER_LAUNCHER=${CMAKE_${lang}_COMPILER_LAUNCHER}
    -DCMAKE_${lang}_FLAGS=${CMAKE_${lang}_FLAGS})

  foreach (config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    list(APPEND CMAKE_${lang}_FORWARD_ARGS
      -DCMAKE_${lang}_FLAGS_${config}=${CMAKE_${lang}_FLAGS_${config}})
  endforeach ()
endforeach ()

# For 3rdparty dependencies that use CMake to find and build against
# OpenSSL, we forward any user-supplied options for the `FindOpenSSL` module.
if (ENABLE_SSL)
  if (OPENSSL_ROOT_DIR)
    list(APPEND CMAKE_SSL_FORWARD_ARGS
      -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR})
  endif ()

  if (OPENSSL_USE_STATIC_LIBS)
    list(APPEND CMAKE_SSL_FORWARD_ARGS
      -DOPENSSL_USE_STATIC_LIBS=${OPENSSL_USE_STATIC_LIBS})
  endif ()

  if (OPENSSL_MSVC_STATIC_RT)
    list(APPEND CMAKE_SSL_FORWARD_ARGS
      -DOPENSSL_MSVC_STATIC_RT=${OPENSSL_MSVC_STATIC_RT})
  endif ()
endif ()

# This function works around a CMake issue with setting include directories of
# imported libraries built with `ExternalProject_Add`.
# https://gitlab.kitware.com/cmake/cmake/issues/15052
function(MAKE_INCLUDE_DIR TARGET)
  get_target_property(DIR ${TARGET} INTERFACE_INCLUDE_DIRECTORIES)
  file(MAKE_DIRECTORY ${DIR})
endfunction()

# This function works around a CMake issue with the Ninja generator where it
# does not understand imported libraries, and instead needs `BUILD_BYPRODUCTS`
# explicitly set.
# https://cmake.org/pipermail/cmake/2015-April/060234.html
function(GET_BYPRODUCTS TARGET)
  string(TOUPPER ${TARGET} NAME)
  if (CMAKE_GENERATOR MATCHES "Ninja")
    get_target_property(BYPRODUCTS ${TARGET} IMPORTED_LOCATION)
    set(${NAME}_BYPRODUCTS ${BYPRODUCTS} PARENT_SCOPE)
  else ()
    # Make this function a no-op when not using Ninja.
    set(${NAME}_BYPRODUCTS "" PARENT_SCOPE)
  endif()
endfunction()


# Third-party libraries. Adds CMake imported or interface targets for each
# dependency. Bundled dependencies are built with `ExternalProject_Add`,
# otherwise `find_package()` is used.
##########################################################################

# Boost: C++ Libraries.
# http://www.boost.org
#######################
if ("${BOOST_ROOT_DIR}" STREQUAL "")
  EXTERNAL(boost ${BOOST_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
  add_library(boost INTERFACE)
  add_dependencies(boost ${BOOST_TARGET})
  if (CMAKE_CXX_COMPILER_ID MATCHES GNU OR CMAKE_CXX_COMPILER_ID MATCHES Clang)
    # Headers including Boost 1.65.0 fail to compile with GCC 7.2 and
    # CLang 3.6 without `-Wno-unused-local-typedefs`.
    # TODO(andschwa): Remove this when Boost has a resolution.
    target_compile_options(boost INTERFACE -Wno-unused-local-typedefs)
  endif ()
  target_include_directories(boost INTERFACE ${BOOST_ROOT})

  # Patch Boost to avoid repeated "Unknown compiler warnings" on Windows.
  PATCH_CMD(BOOST_PATCH_CMD boost-${BOOST_VERSION}.patch)

  ExternalProject_Add(
    ${BOOST_TARGET}
    PREFIX            ${BOOST_CMAKE_ROOT}
    PATCH_COMMAND     ${BOOST_PATCH_CMD}
    CONFIGURE_COMMAND ${CMAKE_NOOP}
    BUILD_COMMAND     ${CMAKE_NOOP}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${BOOST_URL}
    URL_HASH          ${BOOST_HASH})

  if (ENABLE_INSTALL_MODULE_DEPENDENCIES)
    install(
      DIRECTORY ${BOOST_ROOT}/
      DESTINATION ${MESOS_INSTALL_HEADERS})
  endif ()
else ()
  add_library(boost INTERFACE)
  target_include_directories(boost INTERFACE ${BOOST_ROOT_DIR}/include)
endif ()


# moodycamel::ConcurrentQueue: An industrial-strength lock-free queue.
# https://github.com/cameron314/concurrentqueue
######################################################################
EXTERNAL(concurrentqueue ${CONCURRENTQUEUE_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(concurrentqueue INTERFACE)
add_dependencies(concurrentqueue ${CONCURRENTQUEUE_TARGET})
target_include_directories(concurrentqueue INTERFACE ${CONCURRENTQUEUE_ROOT})

ExternalProject_Add(
  ${CONCURRENTQUEUE_TARGET}
  PREFIX            ${CONCURRENTQUEUE_CMAKE_ROOT}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${CONCURRENTQUEUE_URL}
  URL_HASH          ${CONCURRENTQUEUE_HASH})


if (WIN32)
  # Cyrus SASL: Authentication protocol/mechanism interface (Based on IETF RFC 4422)
  # https://cyrusimap.org/sasl/
  ##################################################################################
  EXTERNAL(sasl2 ${SASL2_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
  add_library(sasl2 STATIC IMPORTED GLOBAL)
  add_dependencies(sasl2 ${SASL2_TARGET})

  set_target_properties(
    sasl2 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${SASL2_ROOT}-build/include
    INTERFACE_COMPILE_DEFINITIONS LIBSASL_EXPORTS=1)

  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      sasl2 PROPERTIES
      IMPORTED_LOCATION_DEBUG ${SASL2_ROOT}-build/Debug/libsasl2${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${SASL2_ROOT}-build/Release/libsasl2${LIBRARY_SUFFIX})
  else ()
    set_target_properties(
      sasl2 PROPERTIES
      IMPORTED_LOCATION ${SASL2_ROOT}-build/libsasl2${LIBRARY_SUFFIX})
  endif ()

  # Patch SASL to include a minimal CMake build system which encompasses
  # only the parts that Mesos relies on (CRAM MD5).
  PATCH_CMD(SASL2_PATCH_CMD cyrus-sasl-${SASL2_VERSION}.patch)

  MAKE_INCLUDE_DIR(sasl2)
  GET_BYPRODUCTS(sasl2)

  ExternalProject_Add(
    ${SASL2_TARGET}
    PREFIX            ${SASL2_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${SASL2_BYPRODUCTS}
    PATCH_COMMAND     ${SASL2_PATCH_CMD}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${SASL2_URL}
    URL_HASH          ${SASL2_HASH})

  install(FILES $<TARGET_FILE:sasl2> DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})
else ()
  find_library(SASL2_LIB sasl2)

  if (NOT SASL2_LIB)
    message(FATAL_ERROR "Could not find sasl2 dependency.")
  endif ()

  add_library(sasl2 SHARED IMPORTED GLOBAL)
  set_property(TARGET sasl2 PROPERTY IMPORTED_LOCATION ${SASL2_LIB})
endif ()


# CSI: Container Storage Interface Specification.
# https://github.com/container-storage-interface/spec
#####################################################
EXTERNAL(csi_v0 ${CSI_V0_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(csi_v0 INTERFACE)
add_dependencies(csi_v0 ${CSI_V0_TARGET})
target_include_directories(csi_v0 INTERFACE ${CSI_V0_ROOT})

EXTERNAL(csi_v1 ${CSI_V1_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(csi_v1 INTERFACE)
add_dependencies(csi_v1 ${CSI_V1_TARGET})
target_include_directories(csi_v1 INTERFACE ${CSI_V1_ROOT})

# NOTE: To support multiple CSI versions, we move the CSI proto files to
# version-qualified paths so `protoc` can find them.
set(CSI_V0_CONFIG_CMD
  ${CMAKE_COMMAND} -E make_directory ${CSI_V0_ROOT}/csi/v0 &&
  ${CMAKE_COMMAND} -E rename ${CSI_V0_ROOT}/csi.proto ${CSI_V0_ROOT}/csi/v0/csi.proto)

set(CSI_V1_CONFIG_CMD
  ${CMAKE_COMMAND} -E make_directory ${CSI_V1_ROOT}/csi/v1 &&
  ${CMAKE_COMMAND} -E rename ${CSI_V1_ROOT}/csi.proto ${CSI_V1_ROOT}/csi/v1/csi.proto)

ExternalProject_Add(
  ${CSI_V0_TARGET}
  PREFIX            ${CSI_V0_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${CSI_V0_ROOT}/csi/v0/csi.proto
  CONFIGURE_COMMAND ${CSI_V0_CONFIG_CMD}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${CSI_V0_URL}
  URL_HASH          ${CSI_V0_HASH})

ExternalProject_Add(
  ${CSI_V1_TARGET}
  PREFIX            ${CSI_V1_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${CSI_V1_ROOT}/csi/v1/csi.proto
  CONFIGURE_COMMAND ${CSI_V1_CONFIG_CMD}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${CSI_V1_URL}
  URL_HASH          ${CSI_V1_HASH})


# ELFIO: library for reading and generating ELF files.
# http://elfio.sourceforge.net
######################################################
EXTERNAL(elfio ${ELFIO_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(elfio INTERFACE)
add_dependencies(elfio ${ELFIO_TARGET})
target_include_directories(elfio INTERFACE ${ELFIO_ROOT})

ExternalProject_Add(
  ${ELFIO_TARGET}
  PREFIX            ${ELFIO_CMAKE_ROOT}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${ELFIO_URL}
  URL_HASH          ${ELFIO_HASH})


# glog: Google Logging Library.
# https://github.com/google/glog
################################
EXTERNAL(glog ${GLOG_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(glog ${LIBRARY_LINKAGE} IMPORTED GLOBAL)
add_dependencies(glog ${GLOG_TARGET})

# Patch glog to deal with a problem that appears when compiling on clang
# under the C++11 standard. cf. MESOS-860, MESOS-966.
# On Windows, patch it to enable stack tracing.
PATCH_CMD(GLOG_PATCH_CMD glog-${GLOG_VERSION}.patch)

set(GLOG_LIBRARY_NAME libglog)

if (WIN32)
  # NOTE: Windows-specific workaround for a glog issue documented here[1].
  # Basically, Windows.h and glog/logging.h both define ERROR. Since we don't
  # need the Windows ERROR, we can use this flag to avoid defining it at all.
  # Unlike the other fix (defining GLOG_NO_ABBREVIATED_SEVERITIES), this fix
  # is guaranteed to require no changes to the original Mesos code. See also
  # the note in the code itself[2].
  #
  # [1] https://htmlpreview.github.io/?https://github.com/google/glog/blob/master/doc/glog.html#windows
  # [2] https://github.com/google/glog/blob/f012836db187d5897d4adaaf621b4d53ae4865da/src/windows/glog/logging.h#L965
  set(GLOG_COMPILE_DEFINITIONS NOGDI NOMINMAX)
  if (NOT BUILD_SHARED_LIBS)
    list(APPEND GLOG_COMPILE_DEFINITIONS GOOGLE_GLOG_DLL_DECL=)
  endif ()

  set_target_properties(
    glog PROPERTIES
    # TODO(andschwa): Remove this when glog is updated.
    IMPORTED_LINK_INTERFACE_LIBRARIES DbgHelp
    INTERFACE_COMPILE_DEFINITIONS "${GLOG_COMPILE_DEFINITIONS}")

endif ()

set(GLOG_INSTALL_DIR ${GLOG_ROOT}-install)

if (CMAKE_GENERATOR MATCHES "Visual Studio")
  set_target_properties(
    glog PROPERTIES
    IMPORTED_LOCATION_DEBUG ${GLOG_INSTALL_DIR}/lib/glogd${LIBRARY_SUFFIX}
    IMPORTED_IMPLIB_DEBUG ${GLOG_INSTALL_DIR}/lib/glogd${CMAKE_IMPORT_LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${GLOG_INSTALL_DIR}/lib/glog${LIBRARY_SUFFIX}
    IMPORTED_IMPLIB_RELEASE ${GLOG_INSTALL_DIR}/lib/glog${CMAKE_IMPORT_LIBRARY_SUFFIX}
  )
else ()
  # This is for single-configuration generators such as GNU Make.
  if (CMAKE_BUILD_TYPE MATCHES Debug)
    set(GLOG_SUFFIX d)
  endif ()

  set_target_properties(
    glog PROPERTIES
    IMPORTED_LOCATION ${GLOG_INSTALL_DIR}/lib/libglog${GLOG_SUFFIX}${LIBRARY_SUFFIX}
    IMPORTED_IMPLIB ${GLOG_INSTALL_DIR}/lib/libglog${GLOG_SUFFIX}${CMAKE_IMPORT_LIBRARY_SUFFIX}
  )
endif ()

set_target_properties(
  glog PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ${GLOG_INSTALL_DIR}/include
)

set(
  GLOG_CMAKE_ARGS
  ${CMAKE_CXX_FORWARD_ARGS}
  -DBUILD_TESTING=OFF
  -DCMAKE_INSTALL_BINDIR=${GLOG_INSTALL_DIR}/bin
  -DCMAKE_INSTALL_INCLUDEDIR=${GLOG_INSTALL_DIR}/include
  -DCMAKE_INSTALL_LIBDIR=${GLOG_INSTALL_DIR}/lib
  -DWITH_GFLAGS=OFF
)

MAKE_INCLUDE_DIR(glog)
GET_BYPRODUCTS(glog)

ExternalProject_Add(
  ${GLOG_TARGET}
  PREFIX            ${GLOG_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${GLOG_BYPRODUCTS}
  PATCH_COMMAND     ${GLOG_PATCH_CMD}
  CMAKE_ARGS        ${GLOG_CMAKE_ARGS}
  INSTALL_DIR       ${GLOG_INSTALL_DIR}
  URL               ${GLOG_URL}
  URL_HASH          ${GLOG_HASH})

if (WIN32 AND BUILD_SHARED_LIBS)
  # NOTE: glog's install step places DLLs into ${GLOG_INSTALL_DIR}/bin .
  install(
    DIRECTORY ${GLOG_INSTALL_DIR}/bin/
    DESTINATION ${MESOS_INSTALL_RUNTIME})
endif()

install(
  DIRECTORY ${GLOG_INSTALL_DIR}/lib/
  DESTINATION ${MESOS_INSTALL_LIBRARIES})

if (ENABLE_INSTALL_MODULE_DEPENDENCIES)
  install(
    DIRECTORY ${GLOG_INSTALL_DIR}/include/glog
    DESTINATION ${MESOS_INSTALL_HEADERS})
endif ()

# re2: non-backtracking regular expression library by Google
# https://github.com/google/re2
################################
EXTERNAL(re2 ${RE2_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(re2 ${LIBRARY_LINKAGE} IMPORTED GLOBAL)
add_dependencies(re2 ${RE2_TARGET})

# NOTE: Mesos build uses re2 installed into a prefix (similarly to glog).
set(RE2_INSTALL_DIR ${RE2_ROOT}-install)

set(
  RE2_CMAKE_ARGS
  ${CMAKE_CXX_FORWARD_ARGS}
  -DBUILD_TESTING=OFF
  -DCMAKE_INSTALL_BINDIR=${RE2_INSTALL_DIR}/bin
  -DCMAKE_INSTALL_INCLUDEDIR=${RE2_INSTALL_DIR}/include
  -DCMAKE_INSTALL_LIBDIR=${RE2_INSTALL_DIR}/lib
)

if (WIN32)
  if (BUILD_SHARED_LIBS)
    set(RE2_IMPORTED_LOCATION_DIR ${RE2_INSTALL_DIR}/bin)
  else()
    set(RE2_IMPORTED_LOCATION_DIR ${RE2_INSTALL_DIR}/lib)
  endif()

  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    # TODO(asekretenko): Make sure that these locations are correct on
    # MSVC Windows both in debug and release modes; adjust if necessary.
    set_target_properties(
      re2 PROPERTIES
      IMPORTED_LOCATION_DEBUG ${RE2_IMPORTED_LOCATION_DIR}/re2${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${RE2_IMPORTED_LOCATION_DIR}/re2${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_DEBUG ${RE2_INSTALL_DIR}/lib/re2${CMAKE_IMPORT_LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_RELEASE ${RE2_INSTALL_DIR}/lib/re2${CMAKE_IMPORT_LIBRARY_SUFFIX}
    )
  else ()
    # TODO(asekretenko): Make sure that these locations are correct on
    # non-MSVC Windows; adjust if necessary.
    set_target_properties(
      re2 PROPERTIES
      IMPORTED_LOCATION ${RE2_IMPORTED_LOCATION_DIR}/libre2${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB ${RE2_INSTALL_DIR}/lib/libre2${CMAKE_IMPORT_LIBRARY_SUFFIX}
    )
  endif()
else()
  set_target_properties(
    re2 PROPERTIES
    IMPORTED_LOCATION ${RE2_INSTALL_DIR}/lib/libre2${LIBRARY_SUFFIX}
  )
endif ()

set_target_properties(
  re2 PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ${RE2_INSTALL_DIR}/include
)

MAKE_INCLUDE_DIR(re2)
GET_BYPRODUCTS(re2)

ExternalProject_Add(
  ${RE2_TARGET}
  PREFIX            ${RE2_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${RE2_BYPRODUCTS}
  CMAKE_ARGS        ${RE2_CMAKE_ARGS}
  INSTALL_DIR       ${RE2_INSTALL_DIR}
  URL               ${RE2_URL}
  URL_HASH          ${RE2_HASH})

if (WIN32 AND BUILD_SHARED_LIBS)
  # TODO(asekretenko): Make sure that this location is correct on
  # Windows; adjust if necessary.
  install(
    DIRECTORY ${RE2_INSTALL_DIR}/bin/
    DESTINATION ${MESOS_INSTALL_RUNTIME})
endif()

install(
  DIRECTORY ${RE2_INSTALL_DIR}/lib/
  DESTINATION ${MESOS_INSTALL_LIBRARIES})

# PicoJSON: JSON parser / serializer.
# https://github.com/kazuho/picojson
#####################################
EXTERNAL(picojson ${PICOJSON_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(picojson INTERFACE)
add_dependencies(picojson ${PICOJSON_TARGET})
target_include_directories(picojson INTERFACE ${PICOJSON_ROOT})

# NOTE: PicoJson requires __STDC_FORMAT_MACROS to be defined before importing
# 'inttypes.h'.  Since other libraries may also import this header, it must
# be globally defined so that PicoJSON has access to the macros, regardless
# of the order of inclusion.
target_compile_definitions(
  picojson INTERFACE
  __STDC_FORMAT_MACROS)

PATCH_CMD(PICOJSON_PATCH_CMD picojson-${PICOJSON_VERSION}.patch)

ExternalProject_Add(
  ${PICOJSON_TARGET}
  PREFIX            ${PICOJSON_CMAKE_ROOT}
  PATCH_COMMAND     ${PICOJSON_PATCH_CMD}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${PICOJSON_URL}
  URL_HASH          ${PICOJSON_HASH})

if (ENABLE_INSTALL_MODULE_DEPENDENCIES)
  install(
    FILES ${PICOJSON_CMAKE_ROOT}/src/picojson-${PICOJSON_VERSION}/picojson.h
    DESTINATION ${MESOS_INSTALL_HEADERS})
endif ()

# RapidJSON: JSON parser / serializer.
# https://github.com/Tencent/rapidjson
#####################################
EXTERNAL(rapidjson ${RAPIDJSON_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(rapidjson INTERFACE)
add_dependencies(rapidjson ${RAPIDJSON_TARGET})

target_include_directories(
    rapidjson INTERFACE
    ${RAPIDJSON_ROOT}/include)

PATCH_CMD(RAPIDJSON_PATCH_CMD rapidjson-${RAPIDJSON_VERSION}.patch)

ExternalProject_Add(
  ${RAPIDJSON_TARGET}
  PREFIX            ${RAPIDJSON_CMAKE_ROOT}
  PATCH_COMMAND     ${RAPIDJSON_PATCH_CMD}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${RAPIDJSON_URL}
  URL_HASH          ${RAPIDJSON_HASH})

if (ENABLE_INSTALL_MODULE_DEPENDENCIES)
  install(
    DIRECTORY ${RAPIDJSON_CMAKE_ROOT}/src/rapidjson-${RAPIDJSON_VERSION}/include/rapidjson
    DESTINATION ${MESOS_INSTALL_HEADERS})
endif ()

if (ENABLE_NVML)
  # NVML: NVIDIA Management Library.
  # https://developer.nvidia.com/nvidia-management-library-nvml
  #############################################################
  EXTERNAL(nvml ${NVML_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
  add_library(nvml INTERFACE)
  add_dependencies(nvml ${NVML_TARGET})
  target_include_directories(nvml INTERFACE ${NVML_ROOT})

  ExternalProject_Add(
    ${NVML_TARGET}
    PREFIX            ${NVML_CMAKE_ROOT}
    CONFIGURE_COMMAND ${CMAKE_NOOP}
    BUILD_COMMAND     ${CMAKE_NOOP}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${NVML_URL}
    URL_HASH          ${NVML_HASH})
endif ()


# HTTP Parser: HTTP request/response parser for C.
# https://github.com/nodejs/http-parser
##################################################
# TODO: Remove `GLOBAL` when `Process3rdpartyConfigure` is removed.
EXTERNAL(http_parser ${HTTP_PARSER_VERSION} ${CMAKE_CURRENT_BINARY_DIR})

# NOTE: http-parser is built as a static library unconditionally.
add_library(http_parser STATIC IMPORTED GLOBAL)
add_dependencies(http_parser ${HTTP_PARSER_TARGET})

set_target_properties(
  http_parser PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ${HTTP_PARSER_ROOT})

if (WIN32)
  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      http_parser PROPERTIES
      IMPORTED_LOCATION_DEBUG ${HTTP_PARSER_ROOT}-build/Debug/http_parser${CMAKE_STATIC_LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${HTTP_PARSER_ROOT}-build/Release/http_parser${CMAKE_STATIC_LIBRARY_SUFFIX})
  else ()
    set_target_properties(
      http_parser PROPERTIES
      IMPORTED_LOCATION ${HTTP_PARSER_ROOT}-build/http_parser${CMAKE_STATIC_LIBRARY_SUFFIX})
  endif ()
else ()
  set_target_properties(
    http_parser PROPERTIES
    IMPORTED_LOCATION ${HTTP_PARSER_ROOT}-build/libhttp_parser${CMAKE_STATIC_LIBRARY_SUFFIX})
endif ()

# NOTE: This is used to provide a CMake build for http-parser. We can't just use
# `add_library(http_parser ...)` because `ExternalProject_Add` extracts the
# tarball at build time, and `add_library` is a configuration time step.
set(HTTP_PARSER_PATCH_CMD
  ${CMAKE_COMMAND} -E copy
  ${CMAKE_CURRENT_SOURCE_DIR}/http-parser/CMakeLists.txt.template
  ${HTTP_PARSER_ROOT}/CMakeLists.txt)

MAKE_INCLUDE_DIR(http_parser)
GET_BYPRODUCTS(http_parser)

ExternalProject_Add(
  ${HTTP_PARSER_TARGET}
  PREFIX            ${HTTP_PARSER_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${HTTP_PARSER_BYPRODUCTS}
  PATCH_COMMAND     ${HTTP_PARSER_PATCH_CMD}
  CMAKE_ARGS        ${CMAKE_CXX_FORWARD_ARGS}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${HTTP_PARSER_URL}
  URL_HASH          ${HTTP_PARSER_HASH})


if (ENABLE_LIBEVENT)
  # libevent: An event notification library.
  # http://libevent.org/
  ##########################################
  if (NOT UNBUNDLED_LIBEVENT)
    EXTERNAL(libevent ${LIBEVENT_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
    add_library(libevent ${LIBRARY_LINKAGE} IMPORTED)
    add_dependencies(libevent ${LIBEVENT_TARGET})

    set_target_properties(
      libevent PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBEVENT_ROOT}/include;${LIBEVENT_ROOT}-build/include")

    if (WIN32)
      if (CMAKE_GENERATOR MATCHES "Visual Studio")
        set_target_properties(
          libevent PROPERTIES
          IMPORTED_LOCATION_DEBUG ${LIBEVENT_ROOT}-build/lib/Debug/event${LIBRARY_SUFFIX}
          IMPORTED_LOCATION_RELEASE ${LIBEVENT_ROOT}-build/lib/Release/event${LIBRARY_SUFFIX})
      else ()
        set_target_properties(
          libevent PROPERTIES
          IMPORTED_LOCATION ${LIBEVENT_ROOT}-build/lib/event${LIBRARY_SUFFIX})
      endif ()
    else ()
      set_target_properties(
        libevent PROPERTIES
        IMPORTED_LOCATION ${LIBEVENT_ROOT}-build/lib/libevent${LIBRARY_SUFFIX})
    endif ()

    set(LIBEVENT_CMAKE_FORWARD_ARGS
      ${CMAKE_C_FORWARD_ARGS}
      ${CMAKE_SSL_FORWARD_ARGS}
      # NOTE: Libevent does not respect the BUILD_SHARED_LIBS global flag.
      -DEVENT__BUILD_SHARED_LIBRARIES=${BUILD_SHARED_LIBS}
      -DEVENT__DISABLE_OPENSSL=$<NOT:$<BOOL:${ENABLE_SSL}>>
      -DEVENT__DISABLE_BENCHMARK=ON
      -DEVENT__DISABLE_REGRESS=ON
      -DEVENT__DISABLE_SAMPLES=ON
      -DEVENT__DISABLE_TESTS=ON)

    if (CMAKE_C_COMPILER_ID MATCHES GNU OR CMAKE_C_COMPILER_ID MATCHES Clang)
      list(APPEND LIBEVENT_CMAKE_FORWARD_ARGS -DCMAKE_C_FLAGS=-fPIC)
    endif ()

    MAKE_INCLUDE_DIR(libevent)
    GET_BYPRODUCTS(libevent)

    ExternalProject_Add(
      ${LIBEVENT_TARGET}
      PREFIX            ${LIBEVENT_CMAKE_ROOT}
      BUILD_BYPRODUCTS  ${LIBEVENT_BYPRODUCTS}
      CMAKE_ARGS        ${LIBEVENT_CMAKE_FORWARD_ARGS}
      INSTALL_COMMAND   ${CMAKE_NOOP}
      URL               ${LIBEVENT_URL}
      URL_HASH          ${LIBEVENT_HASH})

    install(
      FILES $<TARGET_FILE:libevent>
      DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})

  else ()
    find_package(LIBEVENT REQUIRED)
    add_library(libevent INTERFACE)

    foreach (lib ${LIBEVENT_LIBS})
      get_filename_component(LIBEVENT_LIB ${lib} NAME_WE)
      add_library(${LIBEVENT_LIB} SHARED IMPORTED)
      set_target_properties(${LIBEVENT_LIB} PROPERTIES
        IMPORTED_LOCATION ${lib}
        INTERFACE_INCLUDE_DIRECTORIES ${LIBEVENT_INCLUDE_DIR})
      target_link_libraries(libevent INTERFACE ${LIBEVENT_LIB})
    endforeach ()
  endif ()
elseif (NOT WIN32) # Windows defaults to `libwinio`, a native implementation.
  # libev: Full-featured high-performance event loop.
  # https://github.com/enki/libev
  ###################################################
  EXTERNAL(libev ${LIBEV_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
  add_library(libev SHARED IMPORTED)
  add_dependencies(libev ${LIBEV_TARGET})

  set_target_properties(
    libev PROPERTIES
    IMPORTED_LOCATION ${LIBEV_ROOT}-build/lib/libev${CMAKE_SHARED_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBEV_ROOT}-build/include)

  # Patch libev to keep it from reaping child processes.
  PATCH_CMD(LIBEV_PATCH_CMD libev-${LIBEV_VERSION}.patch)

  MAKE_INCLUDE_DIR(libev)
  GET_BYPRODUCTS(libev)

  ExternalProject_Add(
    ${LIBEV_TARGET}
    PREFIX            ${LIBEV_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${LIBEV_BYPRODUCTS}
    PATCH_COMMAND     ${LIBEV_PATCH_CMD}
    CONFIGURE_COMMAND ${LIBEV_ROOT}/configure --prefix=${LIBEV_ROOT}-build
    BUILD_COMMAND     ${MAKE_PROGRAM}
    INSTALL_COMMAND   ${MAKE_PROGRAM} install
    URL               ${LIBEV_URL}
    URL_HASH          ${LIBEV_HASH})

  install(
    DIRECTORY ${LIBEV_ROOT}-build/lib/
    DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})
endif ()


# libseccomp: library, which provides a front-end for generating seccomp filters.
# https://github.com/seccomp/libseccomp
##################################################
if (ENABLE_SECCOMP_ISOLATOR)
  if (NOT UNBUNDLED_LIBSECCOMP)
    EXTERNAL(libseccomp ${LIBSECCOMP_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
    add_library(libseccomp STATIC IMPORTED GLOBAL)
    add_dependencies(libseccomp ${LIBSECCOMP_TARGET})

    set_target_properties(
      libseccomp PROPERTIES
      IMPORTED_LOCATION ${LIBSECCOMP_ROOT}-build/src/.libs/libseccomp.a
      INTERFACE_INCLUDE_DIRECTORIES "${LIBSECCOMP_ROOT}/include")

    set(
      LIBSECCOMP_CONFIGURE_COMMAND
      ${LIBSECCOMP_ROOT}/configure --disable-shared --prefix=${LIBSECCOMP_ROOT}-build)

    MAKE_INCLUDE_DIR(libseccomp)
    GET_BYPRODUCTS(libseccomp)

    ExternalProject_Add(
      ${LIBSECCOMP_TARGET}
      PREFIX            ${LIBSECCOMP_CMAKE_ROOT}
      BUILD_BYPRODUCTS  ${LIBSECCOMP_BYPRODUCTS}
      CONFIGURE_COMMAND ${LIBSECCOMP_CONFIGURE_COMMAND}
      INSTALL_COMMAND   ${CMAKE_NOOP}
      URL               ${LIBSECCOMP_URL}
      URL_HASH          ${LIBSECCOMP_HASH})

    install(
      FILES $<TARGET_FILE:libseccomp>
      DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})

  else ()
    find_package(LIBSECCOMP REQUIRED)
    add_library(libseccomp SHARED IMPORTED GLOBAL)
    set_target_properties(libseccomp PROPERTIES
      IMPORTED_LOCATION ${LIBSECCOMP_LIBS}
      INTERFACE_INCLUDE_DIRECTORIES ${LIBSECCOMP_INCLUDE_DIR})
  endif ()
endif ()


# APR: The Apache Portable Runtime Project.
# https://apr.apache.org
###########################################
if (WIN32)
  EXTERNAL(libapr ${LIBAPR_VERSION} ${CMAKE_CURRENT_BINARY_DIR})

  # NOTE: libapr-1 is always a static library on Windows.
  add_library(apr STATIC IMPORTED)
  add_dependencies(apr ${LIBAPR_TARGET})

  set_target_properties(
    apr PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LIBAPR_ROOT}/include;${LIBAPR_ROOT}-build")

  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      apr PROPERTIES
      IMPORTED_LOCATION_DEBUG ${LIBAPR_ROOT}-build/Debug/libapr-1${CMAKE_STATIC_LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${LIBAPR_ROOT}-build/Release/libapr-1${CMAKE_STATIC_LIBRARY_SUFFIX})
  else ()
    set_target_properties(
      apr PROPERTIES
      IMPORTED_LOCATION ${LIBAPR_ROOT}-build/libapr-1${CMAKE_STATIC_LIBRARY_SUFFIX})
  endif ()

  MAKE_INCLUDE_DIR(apr)
  GET_BYPRODUCTS(apr)

  ExternalProject_Add(
    ${LIBAPR_TARGET}
    PREFIX            ${LIBAPR_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${APR_BYPRODUCTS}
    CMAKE_ARGS        ${CMAKE_C_FORWARD_ARGS}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${LIBAPR_URL}
    URL_HASH          ${LIBAPR_HASH})

  install(FILES $<TARGET_FILE:apr> DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})
else ()
  find_package(APR REQUIRED)
  add_library(apr SHARED IMPORTED)

  set_target_properties(
    apr PROPERTIES
    IMPORTED_LOCATION ${APR_LIBS}
    INTERFACE_INCLUDE_DIRECTORIES ${APR_INCLUDE_DIR})
endif ()


# Subversion: Enterprise-class centralized version control for the masses.
# https://subversion.apache.org
##########################################################################
if (NOT WIN32)
  # NOTE: This does not use `FindSubversion` because it finds the `svn`
  # executable, but we're looking for the library.
  find_package(SVN REQUIRED)
  add_library(svn INTERFACE)
  target_include_directories(svn INTERFACE ${SVN_INCLUDE_DIR})

  # `FindSvn` returns finds three separate libraries.
  foreach (lib ${SVN_LIBS})
    get_filename_component(SVN_LIB ${lib} NAME_WE)
    add_library(${SVN_LIB} SHARED IMPORTED)
    set_target_properties(${SVN_LIB} PROPERTIES IMPORTED_LOCATION ${lib})
    target_link_libraries(svn INTERFACE ${SVN_LIB})
  endforeach ()
endif ()


# cURL: Command line tool and library for transferring data with URLs.
# https://curl.haxx.se
######################################################################
if (WIN32)
  EXTERNAL(curl ${CURL_VERSION} ${CMAKE_CURRENT_BINARY_DIR})

  # This is named `libcurl` to not conflict with the executable.
  add_library(libcurl ${LIBRARY_LINKAGE} IMPORTED GLOBAL)
  add_dependencies(libcurl ${CURL_TARGET})

  # Contrary to Linux, the CMake build deploys `curl.exe` on Windows.
  #
  # NOTE: The dependency on `CURL_TARGET` is introduced by the generator
  # expression in `add_custom_command` below. Introducing it here via
  # `add_dependencies` creates a cycle.
  add_executable(curl IMPORTED GLOBAL)

  set(CURL_CMAKE_FORWARD_ARGS
    ${CMAKE_C_FORWARD_ARGS}
    -DCMAKE_USE_WINSSL=ON
    -DCURL_DISABLE_LDAP=ON
    -DBUILD_TESTING=OFF)

  # NOTE: cURL does not respect BUILD_SHARED_LIBS.
  if (NOT BUILD_SHARED_LIBS)
    # This is both a CMake argument and a pre-processor definition.
    list(APPEND CURL_CMAKE_FORWARD_ARGS -DCURL_STATICLIB=ON)

    set_target_properties(
      libcurl PROPERTIES
      INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB)
  endif ()

  set_target_properties(
    libcurl PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${CURL_ROOT}/include
    INTERFACE_LINK_LIBRARIES crypt32)

  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      libcurl PROPERTIES
      IMPORTED_LOCATION_DEBUG ${CURL_ROOT}-build/lib/Debug/libcurl-d${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${CURL_ROOT}-build/lib/Release/libcurl${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_DEBUG ${CURL_ROOT}-build/lib/Debug/libcurl_imp${CMAKE_IMPORT_LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_RELEASE ${CURL_ROOT}-build/lib/Release/libcurl_imp${CMAKE_IMPORT_LIBRARY_SUFFIX})

    set_target_properties(
      curl PROPERTIES
      IMPORTED_LOCATION_DEBUG ${CURL_ROOT}-build/src/Debug/curl.exe
      IMPORTED_LOCATION_RELEASE ${CURL_ROOT}-build/src/Release/curl.exe)
  else ()
    # This is for single-configuration generators such as Ninja.
    if (CMAKE_BUILD_TYPE MATCHES Debug)
      set(CURL_SUFFIX "-d")
    endif ()

    set_target_properties(
      libcurl PROPERTIES
      IMPORTED_LOCATION ${CURL_ROOT}-build/lib/libcurl${CURL_SUFFIX}${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB ${CURL_ROOT}-build/lib/libcurl_imp${CMAKE_IMPORT_LIBRARY_SUFFIX})

    set_target_properties(
      curl PROPERTIES
      IMPORTED_LOCATION ${CURL_ROOT}-build/src/curl.exe)
  endif ()

  MAKE_INCLUDE_DIR(libcurl)
  GET_BYPRODUCTS(libcurl)
  GET_BYPRODUCTS(curl)

  ExternalProject_Add(
    ${CURL_TARGET}
    PREFIX            ${CURL_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${LIBCURL_BYPRODUCTS};${CURL_BYPRODUCTS}
    PATCH_COMMAND     ${CMAKE_NOOP}
    CMAKE_ARGS        ${CURL_CMAKE_FORWARD_ARGS}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${CURL_URL}
    URL_HASH          ${CURL_HASH})

  # This copies the file `curl.exe` from the `3rdparty` build folder to
  # `build/src`, next to the other produced executables. This is necessary for
  # code that shells out to cURL on Windows (in particular, for running Mesos
  # tests without running cmake install).
  ExternalProject_Add_Step(${CURL_TARGET} copy
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:curl> ${CMAKE_BINARY_DIR}/src/curl.exe
    DEPENDEES build)

  install(FILES $<TARGET_FILE:libcurl> DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})
  install(FILES $<TARGET_FILE:curl> DESTINATION ${MESOS_INSTALL_RUNTIME})
else ()
  if ("${CURL_ROOT_DIR}" STREQUAL "")
    find_package(CURL REQUIRED)
  else ()
    set(POSSIBLE_CURL_INCLUDE_DIRS ${CURL_ROOT_DIR}/include)
    set(POSSIBLE_CURL_LIB_DIRS ${CURL_ROOT_DIR}/lib)

    set(CURL_LIBRARY_NAMES curl)

    FIND_PACKAGE_HELPER(CURL curl/curl.h)
    SET(CURL_INCLUDE_DIRS ${CURL_INCLUDE_DIR})
    SET(CURL_LIBRARIES ${CURL_LIBS})
  endif ()

  add_library(libcurl SHARED IMPORTED)

  set_target_properties(
    libcurl PROPERTIES
    IMPORTED_LOCATION ${CURL_LIBRARIES}
    INTERFACE_INCLUDE_DIRECTORIES ${CURL_INCLUDE_DIRS})
endif ()


# wclayer: Command line tool for manipulating Windows Container
# storage layers.
# https://github.com/Microsoft/hcsshim/blob/master/cmd/wclayer/wclayer.go
#########################################################################
if (WIN32)
  EXTERNAL(wclayer WIP ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(wclayer IMPORTED GLOBAL)
  add_dependencies(wclayer ${WCLAYER_TARGET})

  # TODO(liazha): Change this to wherever you need it placed.
  set(WCLAYER_PATH ${CMAKE_BINARY_DIR}/src/wclayer.exe)
  set_target_properties(
    wclayer PROPERTIES
    IMPORTED_LOCATION ${WCLAYER_PATH})

  set(WCLAYER_INSTALL_CMD ${CMAKE_COMMAND} -E copy ${WCLAYER_CMAKE_ROOT}/src/wclayer.exe ${WCLAYER_PATH})

  GET_BYPRODUCTS(wclayer)

  ExternalProject_Add(
    ${WCLAYER_TARGET}
    PREFIX              ${WCLAYER_CMAKE_ROOT}
    BUILD_BYPRODUCTS    ${WCLAYER_BYPRODUCTS}
    CONFIGURE_COMMAND   ${CMAKE_NOOP}
    BUILD_COMMAND       ${CMAKE_NOOP}
    INSTALL_COMMAND     ${WCLAYER_INSTALL_CMD}
    DOWNLOAD_NO_EXTRACT TRUE
    URL                 ${WCLAYER_URL})

  install(FILES $<TARGET_FILE:wclayer> DESTINATION ${MESOS_INSTALL_RUNTIME})
endif ()

# bzip2: A high-quality data compressor.
# http://www.bzip.org
########################################
if (WIN32)
  EXTERNAL(bzip2 ${BZIP2_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
  add_library(bzip2 STATIC IMPORTED)
  add_dependencies(bzip2 ${BZIP2_TARGET})

  set_target_properties(
    bzip2 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${BZIP2_ROOT};${BZIP2_ROOT}-build")

  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      bzip2 PROPERTIES
      IMPORTED_LOCATION_DEBUG ${BZIP2_ROOT}-build/Debug/bzip2${BZIP2_SHARED}${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${BZIP2_ROOT}-build/Release/bzip2${BZIP2_SHARED}${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_DEBUG ${BZIP2_ROOT}-build/Debug/bzip2${CMAKE_IMPORT_LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_RELEASE ${BZIP2_ROOT}-build/Release/bzip2${CMAKE_IMPORT_LIBRARY_SUFFIX})
  else ()
    set_target_properties(
      bzip2 PROPERTIES
      IMPORTED_LOCATION ${BZIP2_ROOT}-build/bzip2${BZIP2_SHARED}${LIBRARY_SUFFIX})
  endif ()

  PATCH_CMD(BZIP2_PATCH_CMD bzip2-${BZIP2_VERSION}.patch)

  MAKE_INCLUDE_DIR(bzip2)
  GET_BYPRODUCTS(bzip2)

  set(BZIP2_CMAKE_ARGS
    ${CMAKE_FORWARD_ARGS}
    ${CMAKE_C_FORWARD_ARGS}
    -DBZIP2_INSTALL_DIR=${BZIP2_ROOT}-lib)

  ExternalProject_Add(
    ${BZIP2_TARGET}
    PREFIX            ${BZIP2_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${BZIP2_BYPRODUCTS}
    PATCH_COMMAND     ${BZIP2_PATCH_CMD}
    CMAKE_ARGS        ${BZIP2_CMAKE_ARGS}
    INSTALL_DIR       ${BZIP2_ROOT}-lib
    URL               ${BZIP2_URL}
    URL_HASH          ${BZIP2_HASH})

  install(FILES $<TARGET_FILE:bzip2> DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})
endif ()

# xz: A Massively Spiffy Yet Delicately Unobtrusive Compression Library.
# https://tukaani.org/xz/
########################################################################
if (WIN32)
  EXTERNAL(xz ${XZ_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
  add_library(xz STATIC IMPORTED)
  add_dependencies(xz ${XZ_TARGET})

  set_target_properties(
    xz PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${XZ_ROOT};${XZ_ROOT}-build")


  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      xz PROPERTIES
      IMPORTED_LOCATION_DEBUG ${XZ_ROOT}-build/Debug/lzma${XZ_SHARED}${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${XZ_ROOT}-build/Release/lzma${XZ_SHARED}${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_DEBUG ${XZ_ROOT}-build/Debug/lzma${CMAKE_IMPORT_LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_RELEASE ${XZ_ROOT}-build/Release/lzma${CMAKE_IMPORT_LIBRARY_SUFFIX})
  else ()
    set_target_properties(
      xz PROPERTIES
      IMPORTED_LOCATION ${XZ_ROOT}-build/lzma${CMAKE_IMPORT_LIBRARY_SUFFIX})
  endif ()

  PATCH_CMD(XZ_PATCH_CMD xz-${XZ_VERSION}.patch)

  MAKE_INCLUDE_DIR(xz)
  GET_BYPRODUCTS(xz)

  set(XZ_CMAKE_ARGS
    ${CMAKE_FORWARD_ARGS}
    ${CMAKE_C_FORWARD_ARGS}
    -DXZ_INSTALL_DIR=${XZ_ROOT}-lib)

  ExternalProject_Add(
    ${XZ_TARGET}
    PREFIX            ${XZ_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${XZ_BYPRODUCTS}
    PATCH_COMMAND     ${XZ_PATCH_CMD}
    CMAKE_ARGS        ${XZ_CMAKE_ARGS}
    INSTALL_DIR       ${XZ_ROOT}-lib
    URL               ${XZ_URL}
    URL_HASH          ${XZ_HASH})
endif ()

# zlib: A Massively Spiffy Yet Delicately Unobtrusive Compression Library.
# https://zlib.net
##########################################################################
if (WIN32)
  EXTERNAL(zlib ${ZLIB_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
  add_library(zlib ${LIBRARY_LINKAGE} IMPORTED)
  add_dependencies(zlib ${ZLIB_TARGET})

  # Zlib generates different libraries depending on the linkage
  # and configuration.  i.e.:
  #   * For a static Debug build: `zlibstaticd.lib`.
  #   * For a shared Release build: `zlib1.lib`.
  if (NOT BUILD_SHARED_LIBS)
    set(ZLIB_STATIC static)
  else ()
    set(ZLIB_SHARED 1)
  endif ()

  # NOTE: We install Zlib under `${ZLIB_ROOT}-lib` so CMake's `FindZLIB` module
  # can find all required headers and library files.
  set(ZLIB_CMAKE_ARGS
    ${CMAKE_C_FORWARD_ARGS}
    -DCMAKE_INSTALL_PREFIX=${ZLIB_ROOT}-lib)

  set_target_properties(
    zlib PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_ROOT}-lib/include)

  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      zlib PROPERTIES
      IMPORTED_LOCATION_DEBUG ${ZLIB_ROOT}-lib/lib/zlib${ZLIB_STATIC}d${ZLIB_SHARED}${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${ZLIB_ROOT}-lib/lib/zlib${ZLIB_STATIC}${ZLIB_SHARED}${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_DEBUG ${ZLIB_ROOT}-lib/lib/zlib${ZLIB_STATIC}d${CMAKE_IMPORT_LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_RELEASE ${ZLIB_ROOT}-lib/lib/zlib${ZLIB_STATIC}${CMAKE_IMPORT_LIBRARY_SUFFIX})
  else ()
    # This is for single-configuration generators such as Ninja.
    if (CMAKE_BUILD_TYPE MATCHES Debug)
      set(ZLIB_SUFFIX "d")
    endif ()

    set_target_properties(
      zlib PROPERTIES
      IMPORTED_LOCATION ${ZLIB_ROOT}-lib/lib/zlib${ZLIB_STATIC}${ZLIB_SUFFIX}${ZLIB_SHARED}${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB ${ZLIB_ROOT}-lib/lib/zlib${ZLIB_STATIC}${ZLIB_SUFFIX}${CMAKE_IMPORT_LIBRARY_SUFFIX})
  endif ()

  MAKE_INCLUDE_DIR(zlib)
  GET_BYPRODUCTS(zlib)

  ExternalProject_Add(
    ${ZLIB_TARGET}
    PREFIX            ${ZLIB_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${ZLIB_BYPRODUCTS}
    CMAKE_ARGS        ${ZLIB_CMAKE_ARGS}
    INSTALL_DIR       ${ZLIB_ROOT}-lib
    URL               ${ZLIB_URL}
    URL_HASH          ${ZLIB_HASH})

  install(FILES $<TARGET_FILE:zlib> DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})
else ()
  find_package(ZLIB REQUIRED)
  add_library(zlib SHARED IMPORTED)

  set_target_properties(
    zlib PROPERTIES
    IMPORTED_LOCATION ${ZLIB_LIBRARIES}
    INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_INCLUDE_DIRS})
endif ()

set_target_properties(
  zlib PROPERTIES
  INTERFACE_COMPILE_DEFINITIONS HAVE_LIBZ)

# libarchive: Multi-format archive and compression library.
# https://github.com/libarchive/libarchive
###########################################################
if (NOT UNBUNDLED_LIBARCHIVE)
  EXTERNAL(libarchive ${LIBARCHIVE_VERSION} ${CMAKE_CURRENT_BINARY_DIR})

  add_library(libarchive STATIC IMPORTED GLOBAL)
  add_dependencies(libarchive ${LIBARCHIVE_TARGET})

  set(LIBARCHIVE_CMAKE_ARGS
    ${CMAKE_FORWARD_ARGS}
    ${CMAKE_C_FORWARD_ARGS}
    -DENABLE_ACL=OFF
    -DENABLE_CNG=OFF
    -DENABLE_CPIO=OFF
    -DENABLE_EXPAT=OFF
    -DENABLE_ICONV=OFF
    -DENABLE_LibGCC=OFF
    -DENABLE_LIBXML2=OFF
    -DENABLE_LZO=OFF
    -DENABLE_NETTLE=OFF
    -DENABLE_OPENSSL=OFF
    -DENABLE_PCREPOSIX=OFF
    -DENABLE_TEST=OFF
    -DCMAKE_PREFIX_PATH=${ZLIB_ROOT}-lib@@${BZIP2_ROOT}-lib@@${XZ_ROOT}-lib
    -DCMAKE_INSTALL_PREFIX=${LIBARCHIVE_ROOT}-build)

  PATCH_CMD(LIBARCHIVE_PATCH_CMD libarchive-${LIBARCHIVE_VERSION}.patch)

  # NOTE: On Windows, libarchive is linked against several compression
  # libraries included in the build chain, such as bzip2, xz, and zlib.
  # On other platforms, libarchive links to zlib only, as bzip2 and xz
  # are not in the build chain.
  if (WIN32)
    set_target_properties(
      libarchive PROPERTIES
      INTERFACE_COMPILE_DEFINITIONS LIBARCHIVE_STATIC
      # NOTE: The install step avoids the need for separate DEBUG and
      # RELEASE paths.
      IMPORTED_LOCATION ${LIBARCHIVE_ROOT}-build/lib/archive_static${LIBRARY_SUFFIX}
      INTERFACE_INCLUDE_DIRECTORIES ${LIBARCHIVE_ROOT}-build/include
      INTERFACE_LINK_LIBRARIES "bzip2;xz;zlib")
    set(LIBARCHIVE_DEPENDS bzip2 xz zlib)

    # Make libarchive link against the same zlib the rest of the build
    # links to. This is necessary because the zlib project
    # unconditionally builds both shared and static libraries, and we
    # need to be consistent about which is linked.
    list(APPEND LIBARCHIVE_CMAKE_ARGS
      -DZLIB_LIBRARY=$<TARGET_FILE:zlib>)
  else ()
    list(APPEND LIBARCHIVE_CMAKE_ARGS
      -DENABLE_BZip2=OFF
      -DENABLE_LZMA=OFF)

    set_target_properties(
      libarchive PROPERTIES
      IMPORTED_LOCATION ${LIBARCHIVE_ROOT}-build/lib/libarchive${CMAKE_STATIC_LIBRARY_SUFFIX}
      INTERFACE_INCLUDE_DIRECTORIES ${LIBARCHIVE_ROOT}-build/include
      INTERFACE_LINK_LIBRARIES "zlib")
    set(LIBARCHIVE_DEPENDS zlib)
  endif ()

  MAKE_INCLUDE_DIR(libarchive)
  GET_BYPRODUCTS(libarchive)

  ExternalProject_Add(
    ${LIBARCHIVE_TARGET}
    DEPENDS           ${LIBARCHIVE_DEPENDS}
    PREFIX            ${LIBARCHIVE_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${LIBARCHIVE_BYPRODUCTS}
    PATCH_COMMAND     ${LIBARCHIVE_PATCH_CMD}
    LIST_SEPARATOR    @@
    CMAKE_ARGS        ${LIBARCHIVE_CMAKE_ARGS}
    URL               ${LIBARCHIVE_URL}
    URL_HASH          ${LIBARCHIVE_HASH})
else ()
  find_package(LIBARCHIVE REQUIRED)
  add_library(libarchive SHARED IMPORTED GLOBAL)
  set_target_properties(libarchive PROPERTIES
    IMPORTED_LOCATION ${LIBARCHIVE_LIBS}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBARCHIVE_INCLUDE_DIR})
endif ()

# Google Test: Google's C++ test framework (GoogleTest and GoogleMock).
# https://github.com/google/googletest
#######################################################################
EXTERNAL(googletest ${GOOGLETEST_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(googletest INTERFACE)
add_dependencies(googletest ${GOOGLETEST_TARGET})
target_link_libraries(googletest INTERFACE gmock gtest)

# Note that Google Test is always built with static libraries because of the
# following open issue when using shared libraries, on both Windows and Linux:
# https://github.com/google/googletest/issues/930
add_library(gmock STATIC IMPORTED GLOBAL)
add_library(gtest STATIC IMPORTED GLOBAL)

set_target_properties(
  gmock PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ${GOOGLETEST_ROOT}/googlemock/include)

set_target_properties(
  gtest PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ${GOOGLETEST_ROOT}/googletest/include)

set(GOOGLETEST_CMAKE_FORWARD_ARGS ${CMAKE_CXX_FORWARD_ARGS})

if (WIN32)
  set(GOOGLETEST_COMPILE_DEFINITIONS
    # Silence deprecation warning in the interface of Google Test.
    _SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING
    # Build in C++11 mode.
    GTEST_LANG_CXX11=1)

  set_target_properties(
    gtest PROPERTIES
    INTERFACE_COMPILE_DEFINITIONS "${GOOGLETEST_COMPILE_DEFINITIONS}")

  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      gmock PROPERTIES
      IMPORTED_LOCATION_DEBUG ${GOOGLETEST_ROOT}-build/googlemock/Debug/gmock${CMAKE_STATIC_LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${GOOGLETEST_ROOT}-build/googlemock/Release/gmock${CMAKE_STATIC_LIBRARY_SUFFIX})

    set_target_properties(
      gtest PROPERTIES
      IMPORTED_LOCATION_DEBUG ${GOOGLETEST_ROOT}-build/googlemock/gtest/Debug/gtest${CMAKE_STATIC_LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${GOOGLETEST_ROOT}-build/googlemock/gtest/Release/gtest${CMAKE_STATIC_LIBRARY_SUFFIX})
  else ()
    set_target_properties(
      gmock PROPERTIES
      IMPORTED_LOCATION ${GOOGLETEST_ROOT}-build/googlemock/gmock${CMAKE_STATIC_LIBRARY_SUFFIX})

    set_target_properties(
      gtest PROPERTIES
      IMPORTED_LOCATION ${GOOGLETEST_ROOT}-build/googlemock/gtest/gtest${CMAKE_STATIC_LIBRARY_SUFFIX})
  endif ()

  # Silence new deprecation warning in Visual Studio 15.5.
  # NOTE: This has been patched upstream, but we don't patch Google Test.
  # https://github.com/google/googletest/issues/1111
  list(APPEND GOOGLETEST_CMAKE_FORWARD_ARGS
    -DCMAKE_CXX_FLAGS=/D_SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING)
else ()
  set_target_properties(
    gmock PROPERTIES
    IMPORTED_LOCATION ${GOOGLETEST_ROOT}-build/googlemock/libgmock${CMAKE_STATIC_LIBRARY_SUFFIX})

  set_target_properties(
    gtest PROPERTIES
    IMPORTED_LOCATION ${GOOGLETEST_ROOT}-build/googlemock/gtest/libgtest${CMAKE_STATIC_LIBRARY_SUFFIX})
endif ()

PATCH_CMD(GOOGLETEST_PATCH_CMD googletest-release-${GOOGLETEST_VERSION}.patch)

MAKE_INCLUDE_DIR(gmock)
MAKE_INCLUDE_DIR(gtest)
GET_BYPRODUCTS(gmock)
GET_BYPRODUCTS(gtest)

# Unconditionally build static libraries.
list(APPEND GOOGLETEST_CMAKE_FORWARD_ARGS -DBUILD_SHARED_LIBS=OFF)

# But also link to the CRT dynamically.
list(APPEND GOOGLETEST_CMAKE_FORWARD_ARGS
  -Dgtest_force_shared_crt=ON)

ExternalProject_Add(
  ${GOOGLETEST_TARGET}
  PREFIX            ${GOOGLETEST_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${GMOCK_BYPRODUCTS};${GTEST_BYPRODUCTS}
  PATCH_COMMAND     ${GOOGLETEST_PATCH_CMD}
  CMAKE_ARGS        ${GOOGLETEST_CMAKE_FORWARD_ARGS}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${GOOGLETEST_URL}
  URL_HASH          ${GOOGLETEST_HASH})


# Protocol Buffers: Google's data interchange format.
# https://developers.google.com/protocol-buffers/
#####################################################
EXTERNAL(protobuf ${PROTOBUF_VERSION} ${CMAKE_CURRENT_BINARY_DIR})

# TODO(andschwa): Move protobufs so these don't have to be GLOBAL.
add_library(protobuf ${LIBRARY_LINKAGE} IMPORTED GLOBAL)
add_library(libprotoc ${LIBRARY_LINKAGE} IMPORTED GLOBAL)

add_dependencies(protobuf ${PROTOBUF_TARGET})
add_executable(protoc IMPORTED GLOBAL)
add_dependencies(protoc ${PROTOBUF_TARGET})

set_target_properties(
  protobuf PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ${PROTOBUF_ROOT}/src)

set(PROTOBUF_CMAKE_FORWARD_ARGS ${CMAKE_CXX_FORWARD_ARGS}
  -Dprotobuf_BUILD_TESTS=OFF)

if (WIN32)
  PATCH_CMD(PROTOBUF_PATCH_CMD protobuf-${PROTOBUF_VERSION}.patch)

  # Link to the CRT dynamically.
  list(APPEND PROTOBUF_CMAKE_FORWARD_ARGS
    -Dprotobuf_MSVC_STATIC_RUNTIME=OFF)

  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      protobuf PROPERTIES
      IMPORTED_LOCATION_DEBUG ${PROTOBUF_ROOT}-build/Debug/libprotobufd${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${PROTOBUF_ROOT}-build/Release/libprotobuf${LIBRARY_SUFFIX})

    set_target_properties(
      protoc PROPERTIES
      IMPORTED_LOCATION_DEBUG ${PROTOBUF_ROOT}-build/Debug/protoc.exe
      IMPORTED_LOCATION_RELEASE ${PROTOBUF_ROOT}-build/Release/protoc.exe)
  else ()
    # This is for single-configuration generators such as Ninja.
    if (CMAKE_BUILD_TYPE MATCHES Debug)
      set(PROTOBUF_SUFFIX "d")
    endif ()

    set_target_properties(
      protobuf PROPERTIES
      IMPORTED_LOCATION ${PROTOBUF_ROOT}-build/libprotobuf${PROTOBUF_SUFFIX}${LIBRARY_SUFFIX})

    set_target_properties(
      protoc PROPERTIES
      IMPORTED_LOCATION ${PROTOBUF_ROOT}-build/protoc.exe)
  endif ()
else ()
  # This is for single-configuration generators such as GNU Make.
  if (CMAKE_BUILD_TYPE MATCHES Debug)
    set(PROTOBUF_SUFFIX d)
  endif ()

  set_target_properties(
    protobuf PROPERTIES
    IMPORTED_LOCATION ${PROTOBUF_ROOT}-build/libprotobuf${PROTOBUF_SUFFIX}${LIBRARY_SUFFIX})

  set_target_properties(
    libprotoc PROPERTIES
    IMPORTED_LOCATION ${PROTOBUF_ROOT}-build/libprotoc${PROTOBUF_SUFFIX}${LIBRARY_SUFFIX})

  set_target_properties(
    protoc PROPERTIES
    IMPORTED_LOCATION ${PROTOBUF_ROOT}-build/protoc)
endif ()

MAKE_INCLUDE_DIR(protobuf)
GET_BYPRODUCTS(protobuf)

ExternalProject_Add(
  ${PROTOBUF_TARGET}
  PREFIX            ${PROTOBUF_CMAKE_ROOT}
  PATCH_COMMAND     ${PROTOBUF_PATCH_CMD}
  BUILD_BYPRODUCTS  ${PROTOBUF_BYPRODUCTS}
  SOURCE_SUBDIR     cmake
  CMAKE_ARGS        ${PROTOBUF_CMAKE_FORWARD_ARGS}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${PROTOBUF_URL}
  URL_HASH          ${PROTOBUF_HASH})

install(FILES $<TARGET_FILE:protobuf> DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})
install(FILES $<TARGET_FILE:libprotoc> DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})
install(PROGRAMS $<TARGET_FILE:protoc> DESTINATION ${MESOS_INSTALL_RUNTIME})

if (ENABLE_INSTALL_MODULE_DEPENDENCIES)
  install(
    DIRECTORY ${PROTOBUF_ROOT}/src/google
    DESTINATION ${MESOS_INSTALL_HEADERS}
    FILES_MATCHING PATTERN "*.h")
endif ()

# gRPC: Google's high performance, open-source universal RPC framework.
# https://grpc.io/
#######################################################################
EXTERNAL(grpc ${GRPC_VERSION} ${CMAKE_CURRENT_BINARY_DIR})

add_library(libgpr ${LIBRARY_LINKAGE} IMPORTED)
add_dependencies(libgpr ${GRPC_TARGET})
add_library(libgrpc ${LIBRARY_LINKAGE} IMPORTED)
add_dependencies(libgrpc ${GRPC_TARGET})
add_library(libgrpc++ ${LIBRARY_LINKAGE} IMPORTED)
add_dependencies(libgrpc++ ${GRPC_TARGET})

add_library(grpc INTERFACE)
target_link_libraries(grpc INTERFACE libgrpc++ libgrpc libgpr)

# TODO(chhsiao): Move grpc so these don't have to be GLOBAL.
add_executable(grpc_cpp_plugin IMPORTED GLOBAL)
add_dependencies(grpc_cpp_plugin ${GRPC_TARGET})

set(GRPC_CMAKE_ARGS
  ${CMAKE_C_FORWARD_ARGS}
  ${CMAKE_CXX_FORWARD_ARGS}
  -DCMAKE_PREFIX_PATH=${PROTOBUF_ROOT}-build
  -DgRPC_PROTOBUF_PROVIDER=package
  -DgRPC_PROTOBUF_PACKAGE_TYPE=CONFIG
  -DgRPC_ZLIB_PROVIDER=package)

if (ENABLE_SSL)
  set(GRPC_VARIANT "")
  list(APPEND GRPC_CMAKE_ARGS -DgRPC_SSL_PROVIDER=package ${CMAKE_SSL_FORWARD_ARGS})
else ()
  set(GRPC_VARIANT "_unsecure")
  list(APPEND GRPC_CMAKE_ARGS -DgRPC_SSL_PROVIDER=none)
endif ()

set(GRPC_BUILD_CMD
  ${CMAKE_COMMAND} --build . --config $<CONFIG> --target gpr &&
  ${CMAKE_COMMAND} --build . --config $<CONFIG> --target grpc${GRPC_VARIANT} &&
  ${CMAKE_COMMAND} --build . --config $<CONFIG> --target grpc++${GRPC_VARIANT} &&
  ${CMAKE_COMMAND} --build . --config $<CONFIG> --target grpc_cpp_plugin)

set_target_properties(
  grpc PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ${GRPC_ROOT}/include)

PATCH_CMD(GRPC_PATCH_CMD grpc-${GRPC_VERSION}.patch)

if (WIN32)
  list(APPEND GRPC_CMAKE_ARGS -DZLIB_ROOT=${ZLIB_ROOT}-lib)

  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      libgpr PROPERTIES
      IMPORTED_LOCATION_DEBUG ${GRPC_ROOT}-build/Debug/gpr${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${GRPC_ROOT}-build/Release/gpr${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_DEBUG ${GRPC_ROOT}-build/Debug/gpr${CMAKE_IMPORT_LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_RELEASE ${GRPC_ROOT}-build/Release/gpr${CMAKE_IMPORT_LIBRARY_SUFFIX})

    set_target_properties(
      libgrpc PROPERTIES
      IMPORTED_LOCATION_DEBUG ${GRPC_ROOT}-build/Debug/grpc${GRPC_VARIANT}${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${GRPC_ROOT}-build/Release/grpc${GRPC_VARIANT}${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_DEBUG ${GRPC_ROOT}-build/Debug/grpc${GRPC_VARIANT}${CMAKE_IMPORT_LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_RELEASE ${GRPC_ROOT}-build/Release/grpc${GRPC_VARIANT}${CMAKE_IMPORT_LIBRARY_SUFFIX})

    set_target_properties(
      libgrpc++ PROPERTIES
      IMPORTED_LOCATION_DEBUG ${GRPC_ROOT}-build/Debug/grpc++${GRPC_VARIANT}${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${GRPC_ROOT}-build/Release/grpc++${GRPC_VARIANT}${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_DEBUG ${GRPC_ROOT}-build/Debug/grpc++${GRPC_VARIANT}${CMAKE_IMPORT_LIBRARY_SUFFIX}
      IMPORTED_IMPLIB_RELEASE ${GRPC_ROOT}-build/Release/grpc++${GRPC_VARIANT}${CMAKE_IMPORT_LIBRARY_SUFFIX})

    set_target_properties(
      grpc_cpp_plugin PROPERTIES
      IMPORTED_LOCATION_DEBUG ${GRPC_ROOT}-build/Debug/grpc_cpp_plugin.exe
      IMPORTED_LOCATION_RELEASE ${GRPC_ROOT}-build/Release/grpc_cpp_plugin.exe)
  else ()
    set_target_properties(
      libgpr PROPERTIES
      IMPORTED_LOCATION ${GRPC_ROOT}-build/gpr${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB ${GRPC_ROOT}-build/gpr${CMAKE_IMPORT_LIBRARY_SUFFIX})

    set_target_properties(
      libgrpc PROPERTIES
      IMPORTED_LOCATION ${GRPC_ROOT}-build/grpc${GRPC_VARIANT}${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB ${GRPC_ROOT}-build/grpc${GRPC_VARIANT}${CMAKE_IMPORT_LIBRARY_SUFFIX})

    set_target_properties(
      libgrpc++ PROPERTIES
      IMPORTED_LOCATION ${GRPC_ROOT}-build/grpc++${GRPC_VARIANT}${LIBRARY_SUFFIX}
      IMPORTED_IMPLIB ${GRPC_ROOT}-build/grpc++${GRPC_VARIANT}${CMAKE_IMPORT_LIBRARY_SUFFIX})

    set_target_properties(
      grpc_cpp_plugin PROPERTIES
      IMPORTED_LOCATION ${GRPC_ROOT}-build/grpc_cpp_plugin.exe)
  endif()
else ()
  set_target_properties(
    libgpr PROPERTIES
    IMPORTED_LOCATION ${GRPC_ROOT}-build/libgpr${LIBRARY_SUFFIX})

  set_target_properties(
    libgrpc PROPERTIES
    IMPORTED_LOCATION ${GRPC_ROOT}-build/libgrpc${GRPC_VARIANT}${LIBRARY_SUFFIX})

  set_target_properties(
    libgrpc++ PROPERTIES
    IMPORTED_LOCATION ${GRPC_ROOT}-build/libgrpc++${GRPC_VARIANT}${LIBRARY_SUFFIX})

  set_target_properties(
    grpc_cpp_plugin PROPERTIES
    IMPORTED_LOCATION ${GRPC_ROOT}-build/grpc_cpp_plugin)
endif ()

MAKE_INCLUDE_DIR(grpc)
GET_BYPRODUCTS(libgpr)
GET_BYPRODUCTS(libgrpc)
GET_BYPRODUCTS(libgrpc++)
GET_BYPRODUCTS(grpc_cpp_plugin)

ExternalProject_Add(
  ${GRPC_TARGET}
  DEPENDS          protobuf protoc zlib
  PREFIX           ${GRPC_CMAKE_ROOT}
  PATCH_COMMAND    ${GRPC_PATCH_CMD}
  BUILD_BYPRODUCTS ${LIBGPR_BYPRODUCTS};${LIBGRPC_BYPRODUCTS};${LIBGRPC++_BYPRODUCTS};${GRPC_CPP_PLUGIN_BYPRODUCTS}
  CMAKE_ARGS       ${GRPC_CMAKE_ARGS}
  BUILD_COMMAND    ${GRPC_BUILD_CMD}
  INSTALL_COMMAND  ${CMAKE_NOOP}
  URL              ${GRPC_URL}
  URL_HASH         ${GRPC_HASH})

install(
  FILES $<TARGET_FILE:libgpr> $<TARGET_FILE:libgrpc> $<TARGET_FILE:libgrpc++>
  DESTINATION ${3RDPARTY_LIBS_INSTALL_DIR})



# Jemalloc: General-purpose malloc implementation.
# http://jemalloc.net
##################################################
if (ENABLE_JEMALLOC_ALLOCATOR)
  EXTERNAL(jemalloc ${JEMALLOC_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
  add_library(jemalloc STATIC IMPORTED GLOBAL)
  add_dependencies(jemalloc ${JEMALLOC_TARGET})

  set_target_properties(
    jemalloc PROPERTIES
    IMPORTED_LOCATION ${JEMALLOC_ROOT}-build/lib/libjemalloc_pic${CMAKE_STATIC_LIBRARY_SUFFIX})

  set(
    JEMALLOC_CONFIGURE_COMMAND
    ${JEMALLOC_ROOT}/configure --enable-stats --enable-prof --with-malloc-conf=prof:true,prof_active:false)

  GET_BYPRODUCTS(jemalloc)

  ExternalProject_Add(
    ${JEMALLOC_TARGET}
    PREFIX            ${JEMALLOC_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${JEMALLOC_BYPRODUCTS}
    CONFIGURE_COMMAND ${JEMALLOC_CONFIGURE_COMMAND}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${JEMALLOC_URL}
    URL_HASH          ${JEMALLOC_HASH})
endif ()


# Apache ZooKeeper: C Client Library to ZooKeeper.
# https://zookeeper.apache.org/
##################################################
# TODO(andschwa): Move zookeeper so these don't have to be GLOBAL.
EXTERNAL(zookeeper ${ZOOKEEPER_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(zookeeper STATIC IMPORTED GLOBAL)
add_library(zk-hashtable STATIC IMPORTED GLOBAL)
add_dependencies(zookeeper ${ZOOKEEPER_TARGET})

set(ZOOKEEPER_COMPILE_DEFINITIONS ZOOKEEPER_VERSION="${ZOOKEEPER_VERSION}" USE_STATIC_LIB)
set_target_properties(
  zookeeper PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${ZOOKEEPER_ROOT}/src/c/include;${ZOOKEEPER_ROOT}/src/c/generated"
  INTERFACE_LINK_LIBRARIES zk-hashtable
  INTERFACE_COMPILE_DEFINITIONS "${ZOOKEEPER_COMPILE_DEFINITIONS}")

set(ZOOKEEPER_CMAKE_FORWARD_ARGS ${CMAKE_CXX_FORWARD_ARGS} -DWANT_CPPUNIT=OFF)

PATCH_CMD(ZOOKEEPER_PATCH_CMD zookeeper-${ZOOKEEPER_VERSION}.patch)

if (WIN32)
  if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(
      zookeeper PROPERTIES
      IMPORTED_LOCATION_DEBUG ${ZOOKEEPER_ROOT}-build/Debug/zookeeper${CMAKE_STATIC_LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${ZOOKEEPER_ROOT}-build/Release/zookeeper${CMAKE_STATIC_LIBRARY_SUFFIX})

    set_target_properties(
      zk-hashtable PROPERTIES
      IMPORTED_LOCATION_DEBUG ${ZOOKEEPER_ROOT}-build/Debug/hashtable${CMAKE_STATIC_LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${ZOOKEEPER_ROOT}-build/Release/hashtable${CMAKE_STATIC_LIBRARY_SUFFIX})
  else ()
    set_target_properties(
      zookeeper PROPERTIES
      IMPORTED_LOCATION ${ZOOKEEPER_ROOT}-build/zookeeper${CMAKE_STATIC_LIBRARY_SUFFIX})

    set_target_properties(
      zk-hashtable PROPERTIES
      IMPORTED_LOCATION ${ZOOKEEPER_ROOT}-build/hashtable${CMAKE_STATIC_LIBRARY_SUFFIX})
  endif ()
else ()
  set_target_properties(
    zookeeper PROPERTIES
    IMPORTED_LOCATION ${ZOOKEEPER_ROOT}-build/libzookeeper${CMAKE_STATIC_LIBRARY_SUFFIX})

  set_target_properties(
    zk-hashtable PROPERTIES
    IMPORTED_LOCATION ${ZOOKEEPER_ROOT}-build/libhashtable${CMAKE_STATIC_LIBRARY_SUFFIX})
endif ()

MAKE_INCLUDE_DIR(zookeeper)
GET_BYPRODUCTS(zookeeper)
GET_BYPRODUCTS(zk-hashtable)

ExternalProject_Add(
  ${ZOOKEEPER_TARGET}
  PREFIX            ${ZOOKEEPER_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${ZOOKEEPER_BYPRODUCTS};${ZK-HASHTABLE_BYPRODUCTS}
  PATCH_COMMAND     ${ZOOKEEPER_PATCH_CMD}
  SOURCE_SUBDIR     src/c
  CMAKE_ARGS        ${ZOOKEEPER_CMAKE_FORWARD_ARGS}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${ZOOKEEPER_URL}
  URL_HASH          ${ZOOKEEPER_HASH})

if (ENABLE_INSTALL_MODULE_DEPENDENCIES)
  install(
    FILES
      ${ZOOKEEPER_CMAKE_ROOT}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/generated/zookeeper.jute.h
      ${ZOOKEEPER_CMAKE_ROOT}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/include/config.h
      ${ZOOKEEPER_CMAKE_ROOT}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/include/proto.h
      ${ZOOKEEPER_CMAKE_ROOT}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/include/recordio.h
      ${ZOOKEEPER_CMAKE_ROOT}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/include/winconfig.h
      ${ZOOKEEPER_CMAKE_ROOT}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/include/zookeeper_log.h
      ${ZOOKEEPER_CMAKE_ROOT}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/include/zookeeper_version.h
      ${ZOOKEEPER_CMAKE_ROOT}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/include/zookeeper.h
    DESTINATION ${MESOS_INSTALL_HEADERS}/zookeeper)

  # NOTE: `zookeeper` is always built statically, hence it is always installed
  # into ${MESOS_INSTALL_LIBRARIES}, even on Windows.
  install(FILES $<TARGET_FILE:zookeeper> DESTINATION ${MESOS_INSTALL_LIBRARIES})
  install(FILES $<TARGET_FILE:zk-hashtable> DESTINATION ${MESOS_INSTALL_LIBRARIES})
endif ()

# LevelDB: A fast key-value storage library.
# https://github.com/google/leveldb
############################################
if (NOT WIN32)
  if (NOT UNBUNDLED_LEVELDB)
    # TODO(andschwa): Move leveldb so these don't have to be GLOBAL.
    EXTERNAL(leveldb ${LEVELDB_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
    add_library(leveldb STATIC IMPORTED GLOBAL)
    add_dependencies(leveldb ${LEVELDB_TARGET})

    set_target_properties(
      leveldb PROPERTIES
      IMPORTED_LOCATION ${LEVELDB_ROOT}/out-static/libleveldb${CMAKE_STATIC_LIBRARY_SUFFIX}
      INTERFACE_INCLUDE_DIRECTORIES ${LEVELDB_ROOT}/include)

    PATCH_CMD(LEVELDB_PATCH_CMD leveldb-${LEVELDB_VERSION}.patch)

    MAKE_INCLUDE_DIR(leveldb)
    GET_BYPRODUCTS(leveldb)

    ExternalProject_Add(
      ${LEVELDB_TARGET}
      PREFIX            ${LEVELDB_CMAKE_ROOT}
      BUILD_BYPRODUCTS  ${LEVELDB_BYPRODUCTS}
      PATCH_COMMAND     ${LEVELDB_PATCH_CMD}
      CONFIGURE_COMMAND ${CMAKE_NOOP}
      BUILD_IN_SOURCE   1
      BUILD_COMMAND     ${MAKE_PROGRAM} OPT=-O2\ -DNDEBUG\ -fPIC all
      INSTALL_COMMAND   ${CMAKE_NOOP}
      URL               ${LEVELDB_URL}
      URL_HASH          ${LEVELDB_HASH})
  else ()
    find_package(LEVELDB REQUIRED)
    # NOTE: We are linking leveldb dynamically to prevent us having to
    # satisfy its default dependencies against gperftools' tcmalloc and
    # snappy.
    add_library(leveldb SHARED IMPORTED GLOBAL)
    set_target_properties(leveldb PROPERTIES
      IMPORTED_LOCATION ${LEVELDB_LIBS}
      INTERFACE_INCLUDE_DIRECTORIES ${LEVELDB_INCLUDE_DIR})
  endif ()
endif ()


# In-tree dependencies.
#######################
add_subdirectory(stout)
add_subdirectory(libprocess)
