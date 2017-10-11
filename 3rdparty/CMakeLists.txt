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
set(CONCURRENTQUEUE_URL ${FETCH_URL}/concurrentqueue-${CONCURRENTQUEUE_VERSION}.tar.gz)
set(ELFIO_URL           ${FETCH_URL}/elfio-${ELFIO_VERSION}.tar.gz)
set(GLOG_URL            ${FETCH_URL}/glog-${GLOG_VERSION}.tar.gz)
set(GOOGLETEST_URL      ${FETCH_URL}/googletest-release-${GOOGLETEST_VERSION}.tar.gz)
set(HTTP_PARSER_URL     ${FETCH_URL}/http-parser-${HTTP_PARSER_VERSION}.tar.gz)
set(PICOJSON_URL        ${FETCH_URL}/picojson-${PICOJSON_VERSION}.tar.gz)
set(PROTOBUF_URL        ${FETCH_URL}/protobuf-${PROTOBUF_VERSION}.tar.gz)
set(LEVELDB_URL         ${FETCH_URL}/leveldb-${LEVELDB_VERSION}.tar.gz)
set(LIBEV_URL           ${FETCH_URL}/libev-${LIBEV_VERSION}.tar.gz)
set(NVML_URL            ${FETCH_URL}/nvml-${NVML_VERSION}.tar.gz)
set(ZOOKEEPER_URL       ${FETCH_URL}/zookeeper-${ZOOKEEPER_VERSION}.tar.gz)

# NOTE: libevent doesn't come rebundled, so this URL is always the same. But,
# it's only downloaded if `ENABLE_LIBEVENT` is set.
set(LIBEVENT_URL ${3RDPARTY_DEPENDENCIES}/libevent-release-${LIBEVENT_VERSION}.tar.gz)

if (WIN32)
  # NOTE: These dependencies are only rebundled on Windows because they
  # are available as installable packages on Linux; so they live
  # exclusively in the 3rdparty repo.
  set(CURL_URL   ${3RDPARTY_DEPENDENCIES}/curl-${CURL_VERSION}.tar.gz)
  set(LIBAPR_URL ${3RDPARTY_DEPENDENCIES}/libapr-${LIBAPR_VERSION}.tar.gz)
  set(ZLIB_URL   ${3RDPARTY_DEPENDENCIES}/zlib-${ZLIB_VERSION}.tar.gz)
  set(SASL2_URL  ${3RDPARTY_DEPENDENCIES}/cyrus-sasl-${SASL2_VERSION}.tar.gz)

  # NOTE: The Windows version of Glog is patched and only available in the
  # 3rdparty repo, not the local repo.
  set(GLOG_URL   ${3RDPARTY_DEPENDENCIES}/glog-${GLOG_VERSION}.tar.gz)
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
  list(APPEND CMAKE_FORWARD_ARGS
    -DCMAKE_${lang}_FLAGS=${CMAKE_${lang}_FLAGS})

  foreach (config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    list(APPEND CMAKE_FORWARD_ARGS
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
  if (WIN32)
    get_target_property(DEBUG ${TARGET} IMPORTED_LOCATION_DEBUG)
    get_target_property(RELEASE ${TARGET} IMPORTED_LOCATION_RELEASE)
    set(BYPRODUCTS ${DEBUG} ${RELEASE})
  else ()
    get_target_property(BYPRODUCTS ${TARGET} IMPORTED_LOCATION)
  endif ()

  string(TOUPPER ${TARGET} NAME)
  set(${NAME}_BYPRODUCTS ${BYPRODUCTS} PARENT_SCOPE)
endfunction()


# Third-party libraries. Adds CMake imported or interface targets for each
# dependency. Bundled dependencies are built with `ExternalProject_Add`,
# otherwise `find_package()` is used.
##########################################################################

# Boost: C++ Libraries.
# http://www.boost.org
#######################
EXTERNAL(boost ${BOOST_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(boost INTERFACE)
add_dependencies(boost ${BOOST_TARGET})
target_include_directories(boost SYSTEM INTERFACE ${BOOST_ROOT})

ExternalProject_Add(
  ${BOOST_TARGET}
  PREFIX            ${BOOST_CMAKE_ROOT}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${BOOST_URL}
  URL_HASH          ${BOOST_HASH})


# moodycamel::ConcurrentQueue: An industrial-strength lock-free queue.
# https://github.com/cameron314/concurrentqueue
######################################################################
EXTERNAL(concurrentqueue ${CONCURRENTQUEUE_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(concurrentqueue INTERFACE)
add_dependencies(concurrentqueue ${CONCURRENTQUEUE_TARGET})
target_include_directories(concurrentqueue SYSTEM INTERFACE ${CONCURRENTQUEUE_ROOT})

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
    IMPORTED_LOCATION_DEBUG ${SASL2_ROOT}-build/Debug/libsasl2${LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${SASL2_ROOT}-build/Release/libsasl2${LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${SASL2_ROOT}-build/include
    INTERFACE_COMPILE_DEFINITIONS LIBSASL_EXPORTS=1)

  # Patch SASL to include a minimal CMake build system which encompasses
  # only the parts that Mesos relies on (CRAM MD5).
  PATCH_CMD(SASL2_PATCH_CMD cyrus-sasl-${SASL2_VERSION}.patch)

  MAKE_INCLUDE_DIR(sasl2)

  ExternalProject_Add(
    ${SASL2_TARGET}
    PREFIX            ${SASL2_CMAKE_ROOT}
    PATCH_COMMAND     ${SASL2_PATCH_CMD}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${SASL2_URL}
    URL_HASH          ${SASL2_HASH})
else ()
  find_library(SASL2_LIB sasl2)

  if (NOT SASL2_LIB)
    message(FATAL_ERROR "Could not find sasl2 dependency.")
  endif ()
endif ()


# ELFIO: library for reading and generating ELF files.
# http://elfio.sourceforge.net
######################################################
EXTERNAL(elfio ${ELFIO_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(elfio INTERFACE)
add_dependencies(elfio ${ELFIO_TARGET})
target_include_directories(elfio SYSTEM INTERFACE ${ELFIO_ROOT})

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

if (WIN32)
  set(GLOG_INSTALL_CMD ${CMAKE_NOOP})

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
    IMPORTED_LOCATION_DEBUG ${GLOG_ROOT}-build/Debug/glog${LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${GLOG_ROOT}-build/Release/glog${LIBRARY_SUFFIX}
    IMPORTED_IMPLIB_DEBUG ${GLOG_ROOT}-build/Debug/glog${CMAKE_IMPORT_LIBRARY_SUFFIX}
    IMPORTED_IMPLIB_RELEASE ${GLOG_ROOT}-build/Release/glog${CMAKE_IMPORT_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${GLOG_ROOT}/src/windows
    # TODO(andschwa): Remove this when glog is updated.
    IMPORTED_LINK_INTERFACE_LIBRARIES DbgHelp
    INTERFACE_COMPILE_DEFINITIONS "${GLOG_COMPILE_DEFINITIONS}")
else ()
  # TODO(andschwa): Build with CMake instead when glog is updated.
  set(GLOG_CONFIG_CMD  ${GLOG_ROOT}/src/../configure --with-pic GTEST_CONFIG=no --prefix=${GLOG_ROOT}-build)
  set(GLOG_BUILD_CMD   make)
  set(GLOG_INSTALL_CMD make install)

  set_target_properties(
    glog PROPERTIES
    IMPORTED_LOCATION ${GLOG_ROOT}-build/lib/libglog${LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${GLOG_ROOT}-build/include)
endif ()

MAKE_INCLUDE_DIR(glog)
GET_BYPRODUCTS(glog)

ExternalProject_Add(
  ${GLOG_TARGET}
  PREFIX            ${GLOG_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${GLOG_BYPRODUCTS}
  PATCH_COMMAND     ${GLOG_PATCH_CMD}
  CMAKE_ARGS        ${CMAKE_FORWARD_ARGS};-DBUILD_TESTING=OFF
  CONFIGURE_COMMAND ${GLOG_CONFIG_CMD}
  BUILD_COMMAND     ${GLOG_BUILD_CMD}
  INSTALL_COMMAND   ${GLOG_INSTALL_CMD}
  URL               ${GLOG_URL}
  URL_HASH          ${GLOG_HASH})


# PicoJSON: JSON parser / serializer.
# https://github.com/kazuho/picojson
#####################################
EXTERNAL(picojson ${PICOJSON_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(picojson INTERFACE)
add_dependencies(picojson ${PICOJSON_TARGET})
target_include_directories(picojson SYSTEM INTERFACE ${PICOJSON_ROOT})

# Enable the INT64 support for PicoJSON.
# NOTE: PicoJson requires __STDC_FORMAT_MACROS to be defined before importing
# 'inttypes.h'.  Since other libraries may also import this header, it must
# be globally defined so that PicoJSON has access to the macros, regardless
# of the order of inclusion.
target_compile_definitions(
  picojson INTERFACE
  PICOJSON_USE_INT64
  __STDC_FORMAT_MACROS)

ExternalProject_Add(
  ${PICOJSON_TARGET}
  PREFIX            ${PICOJSON_CMAKE_ROOT}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${PICOJSON_URL}
  URL_HASH          ${PICOJSON_HASH})


# NVML: NVIDIA Management Library.
# https://developer.nvidia.com/nvidia-management-library-nvml
#############################################################
EXTERNAL(nvml ${NVML_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
add_library(nvml INTERFACE)
add_dependencies(nvml ${NVML_TARGET})
target_include_directories(nvml SYSTEM INTERFACE ${NVML_ROOT})

ExternalProject_Add(
  ${NVML_TARGET}
  PREFIX            ${NVML_CMAKE_ROOT}
  CONFIGURE_COMMAND ${CMAKE_NOOP}
  BUILD_COMMAND     ${CMAKE_NOOP}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${NVML_URL}
  URL_HASH          ${NVML_HASH})


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
  set_target_properties(
    http_parser PROPERTIES
    IMPORTED_LOCATION_DEBUG ${HTTP_PARSER_ROOT}-build/Debug/http_parser${CMAKE_STATIC_LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${HTTP_PARSER_ROOT}-build/Release/http_parser${CMAKE_STATIC_LIBRARY_SUFFIX})
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
  CMAKE_ARGS        ${CMAKE_FORWARD_ARGS}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${HTTP_PARSER_URL}
  URL_HASH          ${HTTP_PARSER_HASH})


if (ENABLE_LIBEVENT)
  # libevent: An event notification library.
  # http://libevent.org/
  ##########################################
  EXTERNAL(libevent ${LIBEVENT_VERSION} ${CMAKE_CURRENT_BINARY_DIR})
  add_library(libevent ${LIBRARY_LINKAGE} IMPORTED)
  add_dependencies(libevent ${LIBEVENT_TARGET})

  set_target_properties(
    libevent PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LIBEVENT_ROOT}/include;${LIBEVENT_ROOT}-build/include")

  if (WIN32)
    set_target_properties(
      libevent PROPERTIES
      IMPORTED_LOCATION_DEBUG ${LIBEVENT_ROOT}-build/lib/Debug/event${LIBRARY_SUFFIX}
      IMPORTED_LOCATION_RELEASE ${LIBEVENT_ROOT}-build/lib/Release/event${LIBRARY_SUFFIX})
  else ()
    set_target_properties(
      libevent PROPERTIES
      IMPORTED_LOCATION ${LIBEVENT_ROOT}-build/lib/libevent${LIBRARY_SUFFIX})
  endif ()

  set(LIBEVENT_CMAKE_ARGS
    ${CMAKE_FORWARD_ARGS}
    ${CMAKE_SSL_FORWARD_ARGS}
    # NOTE: Libevent does not respect the BUILD_SHARED_LIBS global flag.
    -DEVENT__BUILD_SHARED_LIBRARIES=${BUILD_SHARED_LIBS}
    -DEVENT__DISABLE_OPENSSL=$<NOT:$<BOOL:${ENABLE_SSL}>>
    -DCMAKE_C_FLAGS=$<IF:$<PLATFORM_ID:Windows>,"",-fPIC>
    -DEVENT__DISABLE_BENCHMARK=ON
    -DEVENT__DISABLE_REGRESS=ON
    -DEVENT__DISABLE_SAMPLES=ON
    -DEVENT__DISABLE_TESTS=ON)

  MAKE_INCLUDE_DIR(libevent)
  GET_BYPRODUCTS(libevent)

  ExternalProject_Add(
    ${LIBEVENT_TARGET}
    PREFIX            ${LIBEVENT_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${LIBEVENT_BYPRODUCTS}
    CMAKE_ARGS        ${LIBEVENT_CMAKE_ARGS}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${LIBEVENT_URL}
    URL_HASH          ${LIBEVENT_HASH})
else ()
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
    BUILD_COMMAND     make
    INSTALL_COMMAND   make install
    URL               ${LIBEV_URL}
    URL_HASH          ${LIBEV_HASH})
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
    IMPORTED_LOCATION_DEBUG ${LIBAPR_ROOT}-build/Debug/libapr-1${CMAKE_STATIC_LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${LIBAPR_ROOT}-build/Release/libapr-1${CMAKE_STATIC_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES "${LIBAPR_ROOT}/include;${LIBAPR_ROOT}-build")

  MAKE_INCLUDE_DIR(apr)
  GET_BYPRODUCTS(apr)

  ExternalProject_Add(
    ${LIBAPR_TARGET}
    PREFIX            ${LIBAPR_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${APR_BYPRODUCTS}
    CMAKE_ARGS        ${CMAKE_FORWARD_ARGS}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${LIBAPR_URL}
    URL_HASH          ${LIBAPR_HASH})
else ()
  find_package(Apr REQUIRED)
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
  find_package(Svn REQUIRED)
  add_library(svn INTERFACE)
  target_include_directories(svn SYSTEM INTERFACE ${SVN_INCLUDE_DIR})

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
  add_library(curl ${LIBRARY_LINKAGE} IMPORTED GLOBAL)
  add_dependencies(curl ${CURL_TARGET})

  set(CURL_CMAKE_ARGS
    ${CMAKE_FORWARD_ARGS}
    ${CMAKE_SSL_FORWARD_ARGS}
    -DBUILD_CURL_TESTS=OFF
    -DCMAKE_USE_OPENSSL=${ENABLE_SSL})

  # NOTE: cURL does not respect BUILD_SHARED_LIBS.
  if (NOT BUILD_SHARED_LIBS)
    # This is both a CMake argument and a pre-processor definition.
    list(APPEND CURL_CMAKE_ARGS -DCURL_STATICLIB=ON)

    set_target_properties(
      curl PROPERTIES
      INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB)
  endif ()

  set_target_properties(
    curl PROPERTIES
    IMPORTED_LOCATION_DEBUG ${CURL_ROOT}-build/lib/Debug/libcurl${LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${CURL_ROOT}-build/lib/Release/libcurl${LIBRARY_SUFFIX}
    IMPORTED_IMPLIB_DEBUG ${CURL_ROOT}-build/lib/Debug/libcurl_imp${CMAKE_IMPORT_LIBRARY_SUFFIX}
    IMPORTED_IMPLIB_RELEASE ${CURL_ROOT}-build/lib/Release/libcurl_imp${CMAKE_IMPORT_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${CURL_ROOT}/include)

  MAKE_INCLUDE_DIR(curl)
  GET_BYPRODUCTS(curl)

  ExternalProject_Add(
    ${CURL_TARGET}
    PREFIX            ${CURL_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${CURL_BYPRODUCTS}
    PATCH_COMMAND     ${CMAKE_NOOP}
    CMAKE_ARGS        ${CURL_CMAKE_ARGS}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${CURL_URL}
    URL_HASH          ${CURL_HASH})
else ()
  find_package(CURL REQUIRED)
  add_library(curl SHARED IMPORTED)

  set_target_properties(
    curl PROPERTIES
    IMPORTED_LOCATION ${CURL_LIBRARIES}
    INTERFACE_INCLUDE_DIRECTORIES ${CURL_INCLUDE_DIRS})
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

  set_target_properties(
    zlib PROPERTIES
    IMPORTED_LOCATION_DEBUG ${ZLIB_ROOT}-build/Debug/zlib${ZLIB_STATIC}d${ZLIB_SHARED}${LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${ZLIB_ROOT}-build/Release/zlib${ZLIB_STATIC}${ZLIB_SHARED}${LIBRARY_SUFFIX}
    IMPORTED_IMPLIB_DEBUG ${ZLIB_ROOT}-build/Debug/zlib${ZLIB_STATIC}d${CMAKE_IMPORT_LIBRARY_SUFFIX}
    IMPORTED_IMPLIB_RELEASE ${ZLIB_ROOT}-build/Release/zlib${ZLIB_STATIC}${CMAKE_IMPORT_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_ROOT};${ZLIB_ROOT}-build")

  MAKE_INCLUDE_DIR(zlib)
  GET_BYPRODUCTS(zlib)

  ExternalProject_Add(
    ${ZLIB_TARGET}
    PREFIX            ${ZLIB_CMAKE_ROOT}
    BUILD_BYPRODUCTS  ${ZLIB_BYPRODUCTS}
    CMAKE_ARGS        ${CMAKE_FORWARD_ARGS}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${ZLIB_URL}
    URL_HASH          ${ZLIB_HASH})
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

if (WIN32)
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
    IMPORTED_LOCATION ${GOOGLETEST_ROOT}-build/googlemock/libgmock${CMAKE_STATIC_LIBRARY_SUFFIX})

  set_target_properties(
    gtest PROPERTIES
    IMPORTED_LOCATION ${GOOGLETEST_ROOT}-build/googlemock/gtest/libgtest${CMAKE_STATIC_LIBRARY_SUFFIX})
endif ()

MAKE_INCLUDE_DIR(gmock)
MAKE_INCLUDE_DIR(gtest)
GET_BYPRODUCTS(gmock)
GET_BYPRODUCTS(gtest)

set(GOOGLETEST_CMAKE_FORWARD_ARGS ${CMAKE_FORWARD_ARGS})

# Unconditionally build static libraries.
list(APPEND GOOGLETEST_CMAKE_FORWARD_ARGS -DBUILD_SHARED_LIBS=OFF)

# Link to the correct CRT based on other libraries' linkage.
list(APPEND GOOGLETEST_CMAKE_FORWARD_ARGS
  -Dgtest_force_shared_crt=${BUILD_SHARED_LIBS})

ExternalProject_Add(
  ${GOOGLETEST_TARGET}
  PREFIX            ${GOOGLETEST_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${GMOCK_BYPRODUCTS};${GTEST_BYPRODUCTS}
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

add_dependencies(protobuf ${PROTOBUF_TARGET})
add_executable(protoc IMPORTED GLOBAL)
add_dependencies(protoc ${PROTOBUF_TARGET})

set_target_properties(
  protobuf PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ${PROTOBUF_ROOT}/src)

if (WIN32)
  set_target_properties(
    protobuf PROPERTIES
    IMPORTED_LOCATION_DEBUG ${PROTOBUF_ROOT}-build/Debug/libprotobufd${LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${PROTOBUF_ROOT}-build/Release/libprotobuf${LIBRARY_SUFFIX})

  set_target_properties(
    protoc PROPERTIES
    IMPORTED_LOCATION_DEBUG ${PROTOBUF_ROOT}-build/Debug/protoc.exe
    IMPORTED_LOCATION_RELEASE ${PROTOBUF_ROOT}-build/Release/protoc.exe)
else ()
  # This is for single-configuration generators such as GNU Make.
  if (CMAKE_BUILD_TYPE MATCHES DEBUG)
    set(PROTOBUF_SUFFIX d)
  endif ()

  set_target_properties(
    protobuf PROPERTIES
    IMPORTED_LOCATION ${PROTOBUF_ROOT}-build/libprotobuf${PROTOBUF_SUFFIX}${LIBRARY_SUFFIX})

  set_target_properties(
    protoc PROPERTIES
    IMPORTED_LOCATION ${PROTOBUF_ROOT}-build/protoc)
endif ()

MAKE_INCLUDE_DIR(protobuf)
GET_BYPRODUCTS(protobuf)

ExternalProject_Add(
  ${PROTOBUF_TARGET}
  PREFIX            ${PROTOBUF_CMAKE_ROOT}
  BUILD_BYPRODUCTS  ${PROTOBUF_BYPRODUCTS}
  SOURCE_SUBDIR     cmake
  CMAKE_ARGS        ${CMAKE_FORWARD_ARGS};-Dprotobuf_BUILD_TESTS=OFF
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${PROTOBUF_URL}
  URL_HASH          ${PROTOBUF_HASH})


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

PATCH_CMD(ZOOKEEPER_PATCH_CMD zookeeper-${ZOOKEEPER_VERSION}.patch)

if (WIN32)
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
  CMAKE_ARGS        ${CMAKE_FORWARD_ARGS};-DWANT_CPPUNIT=OFF
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${ZOOKEEPER_URL}
  URL_HASH          ${ZOOKEEPER_HASH})


# LevelDB: A fast key-value storage library.
# https://github.com/google/leveldb
############################################
if (NOT WIN32)
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
    BUILD_COMMAND     make OPT=-O2\ -DNDEBUG\ -fPIC all
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${LEVELDB_URL}
    URL_HASH          ${LEVELDB_HASH})
endif ()


# In-tree dependencies.
#######################
add_subdirectory(stout)
add_subdirectory(libprocess)
