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

###############################################################
# EXPORTS VARIABLES NEEDED TO LINK TO THIRD-PARTY LIBS. These are used
# throughout the Mesos project.
#
# THIS INCLUDES things like:
#   * "Public interface" stuff, like which headers we need in order to link to
#     libprocess.
#   * Where to look to find built libraries.
#   * Version information of the third-party libraries in use.
#
# This DOES NOT include:
#   * Where to find include headers for tests -- the rest of Mesos does not
#     need this information.
#   * Any information about how to build these libraries. That's in
#     libprocess/CMakeLists.txt
#   * Information required to build third-party libraries, wuch as what source
#     files we need to compile libprocess.
#   * Build commands actually used to compile (e.g.) libprocess.
#
# RATIONALE: Autoconf makes linking to third party dependencies as simple as
# pointing at the underlying makefile. In CMake, this is harder because we do
# not assume there are Makefiles at all. Thus, it is useful to export variables
# with things like which header files you need to include to link to third
# party libraries, and where in the directory tree you need to look to get the
# actual libraries.

set(PROCESS_PACKAGE_VERSION 0.0.1)
set(PROCESS_PACKAGE_SOVERSION 0)
set(PROCESS_TARGET process-${PROCESS_PACKAGE_VERSION})

# DEFINE DIRECTORY STRUCTURE FOR THIRD-PARTY LIBS.
##################################################
set(PROCESS_3RD_SRC ${CMAKE_SOURCE_DIR}/3rdparty/libprocess/3rdparty/)
set(PROCESS_3RD_BIN ${CMAKE_BINARY_DIR}/3rdparty/libprocess/3rdparty/)

set(STOUT ${PROCESS_3RD_SRC}/stout)

EXTERNAL("boost"       "1.53.0"  "${PROCESS_3RD_BIN}")
EXTERNAL("glog"        "0.3.3"   "${PROCESS_3RD_BIN}")
EXTERNAL("picojson"    "4f93734" "${PROCESS_3RD_BIN}")
EXTERNAL("http_parser" "1c3624a" "${PROCESS_3RD_BIN}")
EXTERNAL("libev"       "4.15"    "${PROCESS_3RD_BIN}")

set(GLOG_LIB ${GLOG_ROOT}-lib/lib)

# DEFINE PROCESS LIBRARY DEPENDENCIES. Tells the process library build targets
# download/configure/build all third-party libraries before attempting to build.
################################################################################
set(PROCESS_DEPENDENCIES
  ${PROCESS_DEPENDENCIES}
  ${BOOST_TARGET}
  ${GLOG_TARGET}
  ${PICOJSON_TARGET}
  ${HTTP_PARSER_TARGET}
  ${LIBEV_TARGET}
  )

# DEFINE THIRD-PARTY INCLUDE DIRECTORIES. Tells compiler toolchain where to get
# headers for our third party libs (e.g., -I/path/to/glog on Linux).
###############################################################################
set(PROCESS_INCLUDE_DIRS
  ${PROCESS_INCLUDE_DIRS}
  ${PROCESS_3RD_SRC}/../include
  ${STOUT}/include
  ${BOOST_ROOT}
  ${LIBEV_ROOT}
  ${PICOJSON_ROOT}
  )

if (WIN32)
  set(PROCESS_INCLUDE_DIRS
    ${PROCESS_INCLUDE_DIRS}
    ${GLOG_ROOT}/src/windows
    )
else (WIN32)
  set(PROCESS_INCLUDE_DIRS
    ${PROCESS_INCLUDE_DIRS}
    ${GLOG_LIB}/include
    )
endif (WIN32)

set(PROCESS_INCLUDE_DIRS
  ${PROCESS_INCLUDE_DIRS}
  ${HTTP_PARSER_ROOT}
  )

if (HAS_GPERFTOOLS)
  set(PROCESS_INCLUDE_DIRS
    ${PROCESS_INCLUDE_DIRS}
    ${GPERFTOOLS}/src
    )
endif (HAS_GPERFTOOLS)

# DEFINE THIRD-PARTY LIB INSTALL DIRECTORIES. Used to tell the compiler
# toolchain where to find our third party libs (e.g., -L/path/to/glog on
# Linux).
########################################################################
set(PROCESS_LIB_DIRS
  ${PROCESS_LIB_DIRS}
  ${GLOG_LIB}/lib
  ${LIBEV_ROOT}-build/.libs
  ${HTTP_PARSER_ROOT}-build
  )

# DEFINE THIRD-PARTY LIBS. Used to generate flags that the linker uses to
# include our third-party libs (e.g., -lglog on Linux).
#########################################################################
find_package(Threads REQUIRED)

set(PROCESS_LIBS
  ${PROCESS_LIBS}
  ${PROCESS_TARGET}
  glog
  ev
  http_parser
  ${CMAKE_THREAD_LIBS_INIT}
  )

if (NOT WIN32)
  find_package(ZLIB REQUIRED)

  # TODO(hausdorff): The `LINUX` flag comes from MesosConfigure; when
  # we port the bootstrap script to CMake, we should also copy this
  # logic into .cmake files in the Stout and Process libraries'
  # folders individually.
  if (LINUX)
    set(PROCESS_LIBS ${PROCESS_LIBS} rt)
  endif (LINUX)

  set(PROCESS_LIBS
    ${PROCESS_LIBS}
    ${ZLIB_LIBRARIES}
    )
endif (NOT WIN32)
