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

set(
  CONTAINERIZER_MEMORY_TESTS_TARGET mesos-containerizer-memory_test
  CACHE STRING "Target we use to refer to tests for mesos containerizer tests"
  )

set(
  ACTIVE_USER_TEST_HELPER_TARGET active-user-test-helper
  CACHE STRING "Test helper target required to run tests with a user."
  )

if (LINUX)
  set(
    SETNS_TEST_HELPER_TARGET setns-test-helper
    CACHE STRING "Test helper target that allows changing the test to its parent namespace."
    )
endif (LINUX)

# COMPILER CONFIGURATION.
#########################
if (WIN32)
  STRING(REGEX REPLACE "/" "\\\\" CURRENT_CMAKE_SOURCE_DIR ${CMAKE_SOURCE_DIR})
  STRING(REGEX REPLACE "/" "\\\\" CURRENT_CMAKE_BUILD_DIR ${CMAKE_BINARY_DIR})
else (WIN32)
  set(CURRENT_CMAKE_SOURCE_DIR ${CMAKE_SOURCE_DIR})
  set(CURRENT_CMAKE_BUILD_DIR ${CMAKE_BINARY_DIR})
endif (WIN32)

add_definitions(-DSOURCE_DIR="${CURRENT_CMAKE_SOURCE_DIR}")
add_definitions(-DBUILD_DIR="${CURRENT_CMAKE_BUILD_DIR}")

add_definitions(-DPKGLIBEXECDIR="${PKG_LIBEXEC_INSTALL_DIR}")
add_definitions(-DTESTLIBEXECDIR="${TEST_LIB_EXEC_DIR}")
add_definitions(-DPKGMODULEDIR="${PKG_MODULE_DIR}")
add_definitions(-DSBINDIR="${S_BIN_DIR}")

# DIRECTORY STRUCTURE FOR THIRD-PARTY LIBS REQUIRED FOR TEST INFRASTRUCTURE.
############################################################################
EXTERNAL("gmock" ${GMOCK_VERSION} "${MESOS_3RDPARTY_BIN}")

set(GTEST_SRC          ${GMOCK_ROOT}/gtest)
set(GPERFTOOLS_VERSION 2.0)
set(GPERFTOOLS         ${MESOS_3RDPARTY_BIN}/gperftools-${GPERFTOOLS_VERSION})

# Convenience variables for include directories of third-party dependencies.
set(GMOCK_INCLUDE_DIR ${GMOCK_ROOT}/include)
set(GTEST_INCLUDE_DIR ${GTEST_SRC}/include)

# Convenience variables for `lib` directories of built third-party dependencies.
if (WIN32)
  set(GMOCK_LIB_DIR ${GMOCK_ROOT}-build/${CMAKE_BUILD_TYPE})
  set(GTEST_LIB_DIR ${GMOCK_ROOT}-build/gtest/${CMAKE_BUILD_TYPE})
else (WIN32)
  set(GMOCK_LIB_DIR ${GMOCK_ROOT}-lib/lib/)
  # TODO(hausdorff): Figure out why this path is different from the
  # `ProcessTestsConfigure` equivalent.
  set(GTEST_LIB_DIR ${GMOCK_ROOT}-build/gtest/)
endif (WIN32)

# Convenience variables for "lflags", the symbols we pass to CMake to generate
# things like `-L/path/to/glog` or `-lglog`.
#set(GMOCK_LFLAG gmock)
set(GTEST_LFLAG gtest)

# DEFINE PROCESS LIBRARY DEPENDENCIES. Tells the process library build targets
# download/configure/build all third-party libraries before attempting to build.
################################################################################
set(CONTAINERIZER_TEST_DEPENDENCIES
  ${CONTAINERIZER_TEST_DEPENDENCIES}
  ${MESOS_TARGET}
  ${GMOCK_TARGET}
  )

# DEFINE THIRD-PARTY INCLUDE DIRECTORIES. Tells compiler toolchain where to get
# headers for our third party libs (e.g., -I/path/to/glog on Linux)..
###############################################################################
set(CONTAINERIZER_TEST_INCLUDE_DIRS
  ${CONTAINERIZER_TEST_INCLUDE_DIRS}
  ${GMOCK_INCLUDE_DIR}
  ${GTEST_INCLUDE_DIR}
  )

# DEFINE THIRD-PARTY LIB INSTALL DIRECTORIES. Used to tell the compiler
# toolchain where to find our third party libs (e.g., -L/path/to/glog on
# Linux).
########################################################################
set(CONTAINERIZER_TEST_LIB_DIRS
  ${CONTAINERIZER_TEST_LIB_DIRS}
  ${GTEST_LIB_DIR}
  )

# DEFINE THIRD-PARTY LIBS. Used to generate flags that the linker uses to
# include our third-party libs (e.g., -lglog on Linux).
#########################################################################
set(CONTAINERIZER_TEST_LIBS
  ${CONTAINERIZER_TEST_LIBS}
  ${MESOS_TARGET}
  ${PROCESS_TARGET}
  ${GTEST_LFLAG}
  )
