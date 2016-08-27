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
# This file exports variables needed ot link to third-party libs. These are
# used throughout the Mesos project.
#
# This includes things like:
#   * Components defining the public interface, like which headers we need in
#     order to link to libprocess.
#   * Where to look to find built libraries.
#   * Version information of the third-party libraries in use.
#
# This does not include:
#   * Where to find include headers for tests -- the rest of Mesos does not
#     need this information.
#   * Any information about how to build these libraries. That's in
#     libprocess/CMakeLists.txt
#   * Information required to build third-party libraries, wuch as what source
#     files we need to compile libprocess.
#   * Build commands actually used to compile (e.g.) libprocess.
#
# Rationale: Autoconf makes linking to third party dependencies as simple as
# pointing at the underlying makefile. In CMake, this is harder because we do
# not assume there are Makefiles at all. Thus, it is useful to export variables
# with things like which header files you need to include to link to third
# party libraries, and where in the directory tree you need to look to get the
# actual libraries.

set(PROCESS_PACKAGE_VERSION 0.0.1)
set(PROCESS_PACKAGE_SOVERSION 0)
set(PROCESS_TARGET process-${PROCESS_PACKAGE_VERSION})

# Define process library dependencies. Tells the process library build targets
# download/configure/build all third-party libraries before attempting to build.
################################################################################
set(PROCESS_DEPENDENCIES
  ${PROCESS_DEPENDENCIES}
  ${STOUT_DEPENDENCIES}
  ${HTTP_PARSER_TARGET}
  )

if (NOT ENABLE_LIBEVENT)
  set(PROCESS_DEPENDENCIES ${PROCESS_DEPENDENCIES} ${LIBEV_TARGET})
elseif (ENABLE_LIBEVENT)
  set(PROCESS_DEPENDENCIES ${PROCESS_DEPENDENCIES} ${LIBEVENT_TARGET})
endif (NOT ENABLE_LIBEVENT)

if (WIN32)
  set(PROCESS_DEPENDENCIES
    ${PROCESS_DEPENDENCIES}
    ${GZIP_TARGET}
    )
endif (WIN32)

# Define third-party include directories. Tells compiler toolchain where to get
# headers for our third party libs (e.g., -I/path/to/glog on Linux).
###############################################################################
set(PROCESS_INCLUDE_DIRS
  ${PROCESS_INCLUDE_DIRS}
  ${STOUT_INCLUDE_DIRS}
  ${PROCESS_INCLUDE_DIR}
  ${HTTP_PARSER_INCLUDE_DIR}
  )

if (NOT ENABLE_LIBEVENT)
  set(PROCESS_INCLUDE_DIRS ${PROCESS_INCLUDE_DIRS} ${LIBEV_INCLUDE_DIR})
elseif (ENABLE_LIBEVENT)
  set(PROCESS_INCLUDE_DIRS ${PROCESS_INCLUDE_DIRS} ${LIBEVENT_INCLUDE_DIR})
endif (NOT ENABLE_LIBEVENT)

if (HAS_GPERFTOOLS)
  set(PROCESS_INCLUDE_DIRS ${PROCESS_INCLUDE_DIRS} ${GPERFTOOLS_INCLUDE_DIR})
endif (HAS_GPERFTOOLS)

if (WIN32)
  set(PROCESS_INCLUDE_DIRS
    ${PROCESS_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIR}
  )
endif (WIN32)

# Define third-party lib install directories. Used to tell the compiler
# toolchain where to find our third party libs (e.g., -L/path/to/glog on
# Linux).
########################################################################
set(PROCESS_LIB_DIRS
  ${PROCESS_LIB_DIRS}
  ${STOUT_LIB_DIRS}
  ${HTTP_PARSER_LIB_DIR}
  )

if (NOT ENABLE_LIBEVENT)
  set(PROCESS_LIB_DIRS ${PROCESS_LIB_DIRS} ${LIBEV_LIB_DIR})
elseif (ENABLE_LIBEVENT)
  set(PROCESS_LIB_DIRS ${PROCESS_LIB_DIRS} ${LIBEVENT_LIB_DIR})
endif (NOT ENABLE_LIBEVENT)

if (WIN32)
  set(PROCESS_LIB_DIRS
    ${PROCESS_LIB_DIRS}
    ${ZLIB_LIB_DIR}
    )
endif (WIN32)

# Define third-party libs. Used to generate flags that the linker uses to
# include our third-party libs (e.g., -lglog on Linux).
#########################################################################
find_package(Threads REQUIRED)

set(PROCESS_LIBS
  ${PROCESS_LIBS}
  ${STOUT_LIBS}
  ${HTTP_PARSER_LFLAG}
  )

if (WIN32)
  set(PROCESS_LIBS
    ${PROCESS_LIBS}
    userenv
    )
endif (WIN32)

if (NOT ENABLE_LIBEVENT)
  set(PROCESS_LIBS ${PROCESS_LIBS} ${LIBEV_LFLAG})
elseif (ENABLE_LIBEVENT)
  set(PROCESS_LIBS ${PROCESS_LIBS} ${LIBEVENT_LFLAG})
endif (NOT ENABLE_LIBEVENT)

if (NOT WIN32)
  find_package(ZLIB REQUIRED)

  # TODO(hausdorff): (MESOS-3396) The `LINUX` flag comes from MesosConfigure;
  # when we port the bootstrap script to CMake, we should also copy this
  # logic into .cmake files in the Stout and Process libraries'
  # folders individually.
  if (LINUX)
    set(PROCESS_LIBS ${PROCESS_LIBS} rt)
  endif (LINUX)

  set(PROCESS_LIBS
    ${PROCESS_LIBS}
    ${ZLIB_LIBRARIES}
    pthread
    )
endif (NOT WIN32)
