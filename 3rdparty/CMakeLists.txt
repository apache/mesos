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

if (BUILD_SHARED_LIBS)
  set(LIBRARY_LINKAGE SHARED)
  set(LIBRARY_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})
else ()
  set(LIBRARY_LINKAGE STATIC)
  set(LIBRARY_SUFFIX ${CMAKE_STATIC_LIBRARY_SUFFIX})
endif ()


# Define sources of third-party dependencies.
#############################################
if (REBUNDLED)
  set(FETCH_URL ${CMAKE_CURRENT_SOURCE_DIR})
else ()
  set(FETCH_URL ${3RDPARTY_DEPENDENCIES})
endif ()


# DEPENDENCIES FOR THE PROCESS LIBRARY AND STOUT.
#
# Downloads, configures, and compiles the third-party libraries for the process
# library (located in `3rdparty/`, i.e. not the `mesos-3rdparty` repository).
###############################################################################
set(BOOST_URL           ${FETCH_URL}/boost-${BOOST_VERSION}.tar.gz)
set(CONCURRENTQUEUE_URL ${FETCH_URL}/concurrentqueue-${CONCURRENTQUEUE_VERSION}.tar.gz)
set(ELFIO_URL           ${FETCH_URL}/elfio-${ELFIO_VERSION}.tar.gz)
set(GLOG_URL            ${FETCH_URL}/glog-${GLOG_VERSION}.tar.gz)
set(HTTP_PARSER_URL     ${FETCH_URL}/http-parser-${HTTP_PARSER_VERSION}.tar.gz)
set(PICOJSON_URL        ${FETCH_URL}/picojson-${PICOJSON_VERSION}.tar.gz)
set(NVML_URL            ${FETCH_URL}/nvml-${NVML_VERSION}.tar.gz)
set(LIBEV_URL           ${FETCH_URL}/libev-${LIBEV_VERSION}.tar.gz)

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

  # NOTE: The Windows version of Glog is patched and only available in the
  # 3rdparty repo, not the local repo.
  set(GLOG_URL   ${3RDPARTY_DEPENDENCIES}/glog-${GLOG_VERSION}.tar.gz)
endif ()

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

  -DBUILD_SHARED_LIBS=${BUILD_SHARED_LIBS}
  )

# This only matters for single-configuration generators.
# E.g. Makefile, but not Visual Studio.
if (NOT "${CMAKE_BUILD_TYPE}" STREQUAL "")
  list(APPEND CMAKE_FORWARD_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
endif ()

foreach (lang C CXX)
  list(APPEND CMAKE_FORWARD_ARGS
    -DCMAKE_${lang}_FLAGS=${CMAKE_${lang}_FLAGS}
    )

  foreach (config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    list(APPEND CMAKE_FORWARD_ARGS
      -DCMAKE_${lang}_FLAGS_${config}=${CMAKE_${lang}_FLAGS_${config}}
      )
  endforeach ()
endforeach ()


# Third-party libraries. Tell the build system how to pull in and build third-
# party libraries at compile time, using the ExternalProject_Add macro.
##############################################################################

# This function works around a CMake issue with setting include directories of
# imported libraries built with `ExternalProject_Add`.
# https://gitlab.kitware.com/cmake/cmake/issues/15052
function(MAKE_INCLUDE_DIR TARGET)
  get_target_property(DIR ${TARGET} INTERFACE_INCLUDE_DIRECTORIES)
  file(MAKE_DIRECTORY ${DIR})
endfunction()

# Boost: C++ Libraries.
# http://www.boost.org
#######################
EXTERNAL(boost ${BOOST_VERSION} "${MESOS_3RDPARTY_BIN}")
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
EXTERNAL(concurrentqueue ${CONCURRENTQUEUE_VERSION} "${MESOS_3RDPARTY_BIN}")
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


# ELFIO: library for reading and generating ELF files.
# http://elfio.sourceforge.net
######################################################
EXTERNAL(elfio ${ELFIO_VERSION} "${MESOS_3RDPARTY_BIN}")
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
EXTERNAL(glog ${GLOG_VERSION} "${MESOS_3RDPARTY_BIN}")
add_library(glog ${LIBRARY_LINKAGE} IMPORTED)
add_dependencies(glog ${GLOG_TARGET})

if (WIN32)
  set(GLOG_PATCH_CMD ${PATCHEXE_LOCATION} -p1 < ${MESOS_3RDPARTY_SRC}/glog-${GLOG_VERSION}.patch)
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
  set(GLOG_COMPILE_DEFINITIONS GOOGLE_GLOG_DLL_DECL= NOGDI NOMINMAX)

  set_target_properties(
    glog PROPERTIES
    IMPORTED_LOCATION_DEBUG ${GLOG_ROOT}-build/Debug/glog${LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${GLOG_ROOT}-build/Release/glog${LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${GLOG_ROOT}/src/windows
    # TODO(andschwa): Remove this when glog is updated.
    IMPORTED_LINK_INTERFACE_LIBRARIES DbgHelp
    INTERFACE_COMPILE_DEFINITIONS "${GLOG_COMPILE_DEFINITIONS}")
else ()
  # Patch glog to deal with a problem that appears when compiling on clang
  # under the C++11 standard. cf. MESOS-860, MESOS-966.
  patch_cmd(${MESOS_3RDPARTY_SRC}/glog-${GLOG_VERSION}.patch GLOG_PATCH_CMD)

  # TODO(andschwa): Build with CMake instead when glog is updated.
  # NOTE: We only link to the shared library of glog because the static build
  # doesn't have PIC.
  set(GLOG_CONFIG_CMD  ${GLOG_ROOT}/src/../configure GTEST_CONFIG=no --prefix=${GLOG_ROOT}-build)
  set(GLOG_BUILD_CMD   make)
  set(GLOG_INSTALL_CMD make install)
  set_target_properties(
    glog PROPERTIES
    IMPORTED_LOCATION ${GLOG_ROOT}-build/lib/libglog${CMAKE_SHARED_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${GLOG_ROOT}-build/include)
endif ()

MAKE_INCLUDE_DIR(glog)

ExternalProject_Add(
  ${GLOG_TARGET}
  PREFIX            ${GLOG_CMAKE_ROOT}
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
EXTERNAL(picojson ${PICOJSON_VERSION} "${MESOS_3RDPARTY_BIN}")
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
EXTERNAL(nvml ${NVML_VERSION} "${MESOS_3RDPARTY_BIN}")
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
EXTERNAL(http_parser ${HTTP_PARSER_VERSION} "${MESOS_3RDPARTY_BIN}")
add_library(http_parser ${LIBRARY_LINKAGE} IMPORTED GLOBAL)
add_dependencies(http_parser ${HTTP_PARSER_TARGET})
set_target_properties(http_parser PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${HTTP_PARSER_ROOT})

# NOTE: http-parser is always built as a static library.
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
  ${MESOS_3RDPARTY_SRC}/http-parser/CMakeLists.txt.template
  ${HTTP_PARSER_ROOT}/CMakeLists.txt)

MAKE_INCLUDE_DIR(http_parser)

ExternalProject_Add(
  ${HTTP_PARSER_TARGET}
  PREFIX            ${HTTP_PARSER_CMAKE_ROOT}
  PATCH_COMMAND     ${HTTP_PARSER_PATCH_CMD}
  CMAKE_ARGS        ${CMAKE_FORWARD_ARGS}
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${HTTP_PARSER_URL}
  URL_HASH          ${HTTP_PARSER_HASH})


if (ENABLE_LIBEVENT)
  # libevent: An event notification library.
  # http://libevent.org/
  ##########################################
  EXTERNAL(libevent ${LIBEVENT_VERSION} "${MESOS_3RDPARTY_BIN}")
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
    # NOTE: Libevent does not respect the BUILD_SHARED_LIBS global flag.
    ${CMAKE_FORWARD_ARGS}
    -DEVENT__BUILD_SHARED_LIBRARIES=${BUILD_SHARED_LIBS}
    -DEVENT__DISABLE_OPENSSL=$<NOT:$<BOOL:${ENABLE_SSL}>>
    -DCMAKE_C_FLAGS=$<IF:$<PLATFORM_ID:Windows>,"",-fPIC>
    -DEVENT__DISABLE_BENCHMARK=ON
    -DEVENT__DISABLE_REGRESS=ON
    -DEVENT__DISABLE_SAMPLES=ON
    -DEVENT__DISABLE_TESTS=ON)

  if (OPENSSL_ROOT_DIR)
    list(APPEND LIBEVENT_CMAKE_ARGS -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR})
  endif ()

  MAKE_INCLUDE_DIR(libevent)

  ExternalProject_Add(
    ${LIBEVENT_TARGET}
    PREFIX            ${LIBEVENT_CMAKE_ROOT}
    CMAKE_ARGS        ${LIBEVENT_CMAKE_ARGS}
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${LIBEVENT_URL}
    URL_HASH          ${LIBEVENT_HASH})
else ()
  # libev: Full-featured high-performance event loop.
  # https://github.com/enki/libev
  ###################################################
  EXTERNAL(libev ${LIBEV_VERSION} "${MESOS_3RDPARTY_BIN}")
  add_library(libev SHARED IMPORTED)
  add_dependencies(libev ${LIBEV_TARGET})

  set_target_properties(
    libev PROPERTIES
    IMPORTED_LOCATION ${LIBEV_ROOT}-build/lib/libev${CMAKE_SHARED_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBEV_ROOT}-build/include)

  # Patch libev to keep it from reaping child processes.
  patch_cmd(${MESOS_3RDPARTY_SRC}/libev-4.22.patch LIBEV_PATCH_CMD)

  MAKE_INCLUDE_DIR(libev)

  ExternalProject_Add(
    ${LIBEV_TARGET}
    PREFIX            ${LIBEV_CMAKE_ROOT}
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
  EXTERNAL(libapr ${LIBAPR_VERSION} "${MESOS_3RDPARTY_BIN}")
  add_library(apr ${LIBRARY_LINKAGE} IMPORTED)
  add_dependencies(apr ${LIBAPR_TARGET})

  # NOTE: libapr-1 is always a static library on Windows.
  set_target_properties(
    apr PROPERTIES
    IMPORTED_LOCATION_DEBUG ${LIBAPR_ROOT}-build/Debug/libapr-1${CMAKE_STATIC_LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${LIBAPR_ROOT}-build/Release/libapr-1${CMAKE_STATIC_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES "${LIBAPR_ROOT}/include;${LIBAPR_ROOT}-build")

  MAKE_INCLUDE_DIR(apr)

  ExternalProject_Add(
    ${LIBAPR_TARGET}
    PREFIX            ${LIBAPR_CMAKE_ROOT}
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


# curl: Command line tool and library for transferring data with URLs.
# https://curl.haxx.se
######################################################################
if (WIN32)
  EXTERNAL(curl ${CURL_VERSION} "${MESOS_3RDPARTY_BIN}")
  add_library(curl ${LIBRARY_LINKAGE} IMPORTED)
  add_dependencies(curl ${CURL_TARGET})

  set_target_properties(
    curl PROPERTIES
    IMPORTED_LOCATION_DEBUG ${CURL_ROOT}-build/lib/Debug/libcurl${LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${CURL_ROOT}-build/lib/Release/libcurl${LIBRARY_SUFFIX}
    INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB
    INTERFACE_INCLUDE_DIRECTORIES ${CURL_ROOT}/include)

  set(CURL_CMAKE_ARGS ${CMAKE_FORWARD_ARGS} -DBUILD_CURL_TESTS=OFF)

  # NOTE: curl does not respect BUILD_SHARED_LIBS.
  if (NOT BUILD_SHARED_LIBS)
    # This is both a CMake argument, and a pre-processor definition.
    list(APPEND CURL_CMAKE_ARGS -DCURL_STATICLIB=ON)
  endif ()

  MAKE_INCLUDE_DIR(curl)

  ExternalProject_Add(
    ${CURL_TARGET}
    PREFIX            ${CURL_CMAKE_ROOT}
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
  EXTERNAL(zlib ${ZLIB_VERSION} "${MESOS_3RDPARTY_BIN}")
  add_library(zlib ${LIBRARY_LINKAGE} IMPORTED)
  add_dependencies(zlib ${ZLIB_TARGET})

  # Zlib generates different libraries depending on the linkage
  # and configuration.  i.e.:
  #   * For a static Debug build: `zlibstaticd.lib`.
  #   * For a shared Release build: `zlib.lib`.
  if (NOT BUILD_SHARED_LIBS)
    set(ZLIB_STATIC static)
  endif ()

  set_target_properties(
    zlib PROPERTIES
    IMPORTED_LOCATION_DEBUG ${ZLIB_ROOT}-build/Debug/zlib${ZLIB_STATIC}d${LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${ZLIB_ROOT}-build/Release/zlib${ZLIB_STATIC}${LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_ROOT};${ZLIB_ROOT}-build")

  MAKE_INCLUDE_DIR(zlib)

  ExternalProject_Add(
    ${ZLIB_TARGET}
    PREFIX            ${ZLIB_CMAKE_ROOT}
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


# DEPENDENCIES FOR THE PROCESS LIBRARY TESTS AND STOUT TESTS.
#
# Downloads, configures, and compiles the third-party libraries for the process
# library tests (located in `3rdparty/`).
###############################################################################
list(
  APPEND CMAKE_MODULE_PATH
  ${CMAKE_SOURCE_DIR}/3rdparty/stout/cmake)


# Google Test: Google's C++ test framework (GoogleTest and GoogleMock).
# https://github.com/google/googletest
#######################################################################
EXTERNAL(googletest ${GOOGLETEST_VERSION} "${MESOS_3RDPARTY_BIN}")
add_library(googletest INTERFACE)
add_dependencies(googletest ${GOOGLETEST_TARGET})
target_link_libraries(googletest INTERFACE gmock gtest)

add_library(gmock IMPORTED STATIC)
add_library(gtest IMPORTED STATIC)

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

set(GOOGLETEST_URL ${FETCH_URL}/googletest-release-${GOOGLETEST_VERSION}.tar.gz)

ExternalProject_Add(
  ${GOOGLETEST_TARGET}
  PREFIX            ${GOOGLETEST_CMAKE_ROOT}
  # NOTE: googletest cannot be built as a shared library, or the test runner
  # will crash with a double-free.
  CMAKE_ARGS        ${CMAKE_FORWARD_ARGS};-DBUILD_SHARED_LIBS=OFF
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${GOOGLETEST_URL}
  URL_HASH          ${GOOGLETEST_HASH})


# Protocol Buffers: Google's data interchange format.
# https://developers.google.com/protocol-buffers/
#####################################################
EXTERNAL("protobuf" ${PROTOBUF_VERSION} "${MESOS_3RDPARTY_BIN}")

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

set(PROTOBUF_URL ${FETCH_URL}/protobuf-${PROTOBUF_VERSION}.tar.gz)

ExternalProject_Add(
  ${PROTOBUF_TARGET}
  PREFIX            ${PROTOBUF_CMAKE_ROOT}
  SOURCE_SUBDIR     cmake
  CMAKE_ARGS        ${CMAKE_FORWARD_ARGS};-Dprotobuf_BUILD_TESTS=OFF
  INSTALL_COMMAND   ${CMAKE_NOOP}
  URL               ${PROTOBUF_URL}
  URL_HASH          ${PROTOBUF_HASH})


# BUILDING THE STOUT LIBRARY AND ITS TESTS.
#
# Builds, configures, and compiles the stout library and assorted tests.
########################################################################
add_subdirectory(stout)


# BUILDING THE PROCESS LIBRARY AND ITS TESTS.
#
# Builds, configures, and compiles the process library and assorted tests.
##########################################################################
add_subdirectory(libprocess)


# BUILDING THE MESOS DEPENDENCIES.
#
# Downloads, configures, and compiles the third-party libraries for the mesos.
###################################################
set(LEVELDB_URL   ${FETCH_URL}/leveldb-${LEVELDB_VERSION}.tar.gz)
set(ZOOKEEPER_URL ${FETCH_URL}/zookeeper-${ZOOKEEPER_VERSION}.tar.gz)


# Apache ZooKeeper: C Client Library to ZooKeeper.
# https://zookeeper.apache.org/
##################################################
# TODO(andschwa): Move zookeeper so these don't have to be GLOBAL.
EXTERNAL(zookeeper ${ZOOKEEPER_VERSION} "${MESOS_3RDPARTY_BIN}")
add_library(zookeeper STATIC IMPORTED GLOBAL)
add_library(zk-hashtable STATIC IMPORTED GLOBAL)
add_dependencies(zookeeper ${ZOOKEEPER_TARGET})

set(ZOOKEEPER_COMPILE_DEFINITIONS ZOOKEEPER_VERSION="${ZOOKEEPER_VERSION}" USE_STATIC_LIB)
set_target_properties(
  zookeeper PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${ZOOKEEPER_ROOT}/src/c/include;${ZOOKEEPER_ROOT}/src/c/generated"
  INTERFACE_LINK_LIBRARIES zk-hashtable
  INTERFACE_COMPILE_DEFINITIONS "${ZOOKEEPER_COMPILE_DEFINITIONS}")

if (WIN32)
  set_target_properties(
    zookeeper PROPERTIES
    IMPORTED_LOCATION_DEBUG ${ZOOKEEPER_ROOT}-build/Debug/zookeeper${CMAKE_STATIC_LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${ZOOKEEPER_ROOT}-build/Release/zookeeper${CMAKE_STATIC_LIBRARY_SUFFIX})

  set_target_properties(
    zk-hashtable PROPERTIES
    IMPORTED_LOCATION_DEBUG ${ZOOKEEPER_ROOT}-build/Debug/hashtable${CMAKE_STATIC_LIBRARY_SUFFIX}
    IMPORTED_LOCATION_RELEASE ${ZOOKEEPER_ROOT}-build/Release/hashtable${CMAKE_STATIC_LIBRARY_SUFFIX})

  # Set the patch command which will utilize patch.exe in temp location for no elevation prompt
  # NOTE: We do not specify the `--binary` patch option here because the
  # files being modified are extracted with CRLF (Windows) line endings
  # already. The `--binary` option will instead fail to apply the patch.
  set(ZOOKEEPER_PATCH_CMD ${PATCHEXE_LOCATION} -p1 < ${MESOS_3RDPARTY_SRC}/zookeeper-${ZOOKEEPER_VERSION}.patch)
else ()
  set_target_properties(
    zookeeper PROPERTIES
    IMPORTED_LOCATION ${ZOOKEEPER_ROOT}-build/libzookeeper${CMAKE_STATIC_LIBRARY_SUFFIX})

  set_target_properties(
    zk-hashtable PROPERTIES
    IMPORTED_LOCATION ${ZOOKEEPER_ROOT}-build/libhashtable${CMAKE_STATIC_LIBRARY_SUFFIX})

  patch_cmd(${MESOS_3RDPARTY_SRC}/zookeeper-${ZOOKEEPER_VERSION}.patch ZOOKEEPER_PATCH_CMD)
endif ()

MAKE_INCLUDE_DIR(zookeeper)

ExternalProject_Add(
  ${ZOOKEEPER_TARGET}
  PREFIX            ${ZOOKEEPER_CMAKE_ROOT}
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
  EXTERNAL(leveldb ${LEVELDB_VERSION} "${MESOS_3RDPARTY_BIN}")
  add_library(leveldb STATIC IMPORTED GLOBAL)
  add_dependencies(leveldb ${LEVELDB_TARGET})

  set_target_properties(
    leveldb PROPERTIES
    IMPORTED_LOCATION ${LEVELDB_ROOT}/out-static/libleveldb${CMAKE_STATIC_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${LEVELDB_ROOT}/include)

  patch_cmd(${MESOS_3RDPARTY_SRC}/leveldb-${LEVELDB_VERSION}.patch LEVELDB_PATCH_CMD)

  MAKE_INCLUDE_DIR(leveldb)

  ExternalProject_Add(
    ${LEVELDB_TARGET}
    PREFIX            ${LEVELDB_CMAKE_ROOT}
    PATCH_COMMAND     ${LEVELDB_PATCH_CMD}
    CONFIGURE_COMMAND ${CMAKE_NOOP}
    BUILD_IN_SOURCE   1
    BUILD_COMMAND     make OPT=-O2\ -DNDEBUG\ -fPIC all
    INSTALL_COMMAND   ${CMAKE_NOOP}
    URL               ${LEVELDB_URL}
    URL_HASH          ${LEVELDB_HASH})
endif ()
