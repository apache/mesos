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
# Defines the variables useful for tests, and exports them to the scope of
# whatever file includes this file. There are a few important consequence of this:
#
#   * This file MUST be included before the third-party dependencies like gmock
#     are configured/built/downloaded (if you're doing this). If this code isn't
#     run first, then we won't know (e.g.) what folders to unpack the code to.
#   * This file ONLY defines and exports variables for third-party dependencies
#     that are required by the test suite, but are not a dependency that
#     libprocess core takes. That is, this file handles the gmock dependency,
#     but not the glog dependency (which the process library itself takes a
#     dependency on).
#   * This file and the config file for the libprocess "core" dependencies (e.g.,
#     glog, boost, etc.) so that we can export the variables for the core
#     dependencies (e.g., where to find the .so/.dll files) without having to also
#     export the variables for the dependencies that only the test package has.

set(
  PROCESS_TESTS_TARGET process_tests
  CACHE STRING "Target we use to refer to tests for the process library")

# DIRECTORY STRUCTURE FOR THIRD-PARTY LIBS REQUIRED FOR TEST INFRASTRUCTURE.
############################################################################
EXTERNAL("gmock" ${GMOCK_VERSION} "${PROCESS_3RD_BIN}")

set(GTEST_SRC          ${GMOCK_ROOT}/gtest)
set(GPERFTOOLS_VERSION 2.0)
set(GPERFTOOLS         ${PROCESS_3RD_BIN}/gperftools-${GPERFTOOLS_VERSION})

# Convenience variables for include directories of third-party dependencies.
set(GMOCK_INCLUDE_DIR ${GMOCK_ROOT}/include)
set(GTEST_INCLUDE_DIR ${GTEST_SRC}/include)

# Convenience variables for `lib` directories of built third-party dependencies.
if (WIN32)
  set(GMOCK_LIB_DIR ${GMOCK_ROOT}-build/${CMAKE_BUILD_TYPE})
  set(GTEST_LIB_DIR ${GMOCK_ROOT}-build/gtest/${CMAKE_BUILD_TYPE})
else (WIN32)
  set(GMOCK_LIB_DIR ${GMOCK_ROOT}-lib/lib/)
  set(GTEST_LIB_DIR ${GMOCK_ROOT}-build/gtest/lib/.libs)
endif (WIN32)

# Convenience variables for "lflags", the symbols we pass to CMake to generate
# things like `-L/path/to/glog` or `-lglog`.
set(GMOCK_LFLAG gmock)
set(GTEST_LFLAG gtest)

# COMPILER CONFIGURATION.
#########################
if (APPLE)
  # GTEST on OSX needs its own tr1 tuple.
  add_definitions(-DGTEST_USE_OWN_TR1_TUPLE=1 -DGTEST_LANG_CXX11)
endif (APPLE)

# DEFINE PROCESS TEST LIBRARY DEPENDENCIES. Tells the process library build
# tests target download/configure/build all third-party libraries before
# attempting to build.
###########################################################################
set(PROCESS_TEST_DEPENDENCIES
  ${PROCESS_TEST_DEPENDENCIES}
  ${PROCESS_DEPENDENCIES}
  ${PROCESS_TARGET}
  ${GMOCK_TARGET}
  )

# DEFINE THIRD-PARTY INCLUDE DIRECTORIES. Tells compiler toolchain where to get
# headers for our third party libs (e.g., -I/path/to/glog on Linux).
###############################################################################
set(PROCESS_TEST_INCLUDE_DIRS
  ${PROCESS_TEST_INCLUDE_DIRS}
  ../   # includes, e.g., decoder.hpp
  ${PROCESS_INCLUDE_DIRS}
  ${GMOCK_INCLUDE_DIR}
  ${GTEST_INCLUDE_DIR}
  src
  )

# DEFINE THIRD-PARTY LIB INSTALL DIRECTORIES. Used to tell the compiler
# toolchain where to find our third party libs (e.g., -L/path/to/glog on
# Linux).
########################################################################
set(PROCESS_TEST_LIB_DIRS
  ${PROCESS_TEST_LIB_DIRS}
  ${PROCESS_LIB_DIRS}
  ${CMAKE_CURRENT_BINARY_DIR}/.. # libprocess directory.

  ${GMOCK_LIB_DIR}
  ${GTEST_LIB_DIR}
  )

# DEFINE THIRD-PARTY LIBS. Used to generate flags that the linker uses to
# include our third-party libs (e.g., -lglog on Linux).
#########################################################################
set(PROCESS_TEST_LIBS
  ${PROCESS_TEST_LIBS}
  ${PROCESS_TARGET}
  ${PROCESS_LIBS}
  ${GMOCK_LFLAG}
  ${GTEST_LFLAG}
  )
