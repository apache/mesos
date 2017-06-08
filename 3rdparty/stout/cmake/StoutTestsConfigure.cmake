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
# any file includes this file. There are a few important consequence of this:
#
#   * This file MUST be included before the third-party dependencies like gmock
#     are configured/built/downloaded (if you're doing this). If this code isn't
#     run first, then we won't know (e.g.) what folders to unpack the code to.
#   * This file ONLY defines and exports variables for third-party dependencies
#     that are required by the test suite, but are not a dependency that
#     libprocess core takes. That is, this file handles the gmock dependency,
#     but not the glog dependency (which the process library itself takes a
#     dependency on).
#   * This file and the config file for the libprocess "core" dependencies
#     (e.g. glog, boost, etc.) are separate so that we can export the variables
#     for the core dependencies (e.g., where to find the .so/.dll files) without
#     having to also export the variables for test-only dependencies.

set(
  STOUT_TESTS_TARGET stout-tests
  CACHE STRING "Target we use to refer to tests for the stout library")

# COMPILER CONFIGURATION.
#########################
EXTERNAL("googletest" ${GOOGLETEST_VERSION} "${MESOS_3RDPARTY_BIN}")

# Convenience variables for include directories of third-party dependencies.
set(GMOCK_INCLUDE_DIR ${GOOGLETEST_ROOT}/googlemock/include)
set(GTEST_INCLUDE_DIR ${GOOGLETEST_ROOT}/googletest/include)

# Convenience variables for `lib` directories of built third-party dependencies.
if (WIN32)
  set(GMOCK_LIB_DIR ${GOOGLETEST_ROOT}-build/googlemock)
  set(GTEST_LIB_DIR ${GOOGLETEST_ROOT}-build/googlemock/gtest)
else ()
  set(GMOCK_LIB_DIR ${GOOGLETEST_ROOT}-lib/lib)
  set(GTEST_LIB_DIR ${GOOGLETEST_ROOT}-lib/lib/gtest)
endif ()

# Convenience variables for "lflags", the symbols we pass to CMake to generate
# things like `-L/path/to/glog` or `-lglog`.
set(GMOCK_LFLAG gmock)
set(GTEST_LFLAG gtest)

# DEFINE PROCESS LIBRARY DEPENDENCIES. Tells the process library build targets
# download/configure/build all third-party libraries before attempting to build.
################################################################################
set(STOUT_TEST_DEPENDENCIES
  ${STOUT_TEST_DEPENDENCIES}
  ${STOUT_DEPENDENCIES}
  ${GOOGLETEST_TARGET}
  )

# DEFINE THIRD-PARTY INCLUDE DIRECTORIES. Tells compiler toolchain where to get
# headers for our third party libs (e.g., -I/path/to/glog on Linux)..
###############################################################################
set(STOUT_TEST_3RDPARTY_INCLUDE_DIRS
  ${STOUT_TEST_3RDPARTY_INCLUDE_DIRS}
  ${STOUT_3RDPARTY_INCLUDE_DIRS}
  ${GMOCK_INCLUDE_DIR}
  ${GTEST_INCLUDE_DIR}
  )

# DEFINE THIRD-PARTY LIB INSTALL DIRECTORIES. Used to tell the compiler
# toolchain where to find our third party libs (e.g., -L/path/to/glog on
# Linux).
########################################################################
set(STOUT_TEST_LIB_DIRS
  ${STOUT_TEST_LIB_DIRS}
  ${STOUT_LIB_DIRS}
  ${GMOCK_LIB_DIR}
  ${GTEST_LIB_DIR}
  )

# DEFINE THIRD-PARTY LIBS. Used to generate flags that the linker uses to
# include our third-party libs (e.g., -lglog on Linux).
#########################################################################
set(STOUT_TEST_LIBS
  ${STOUT_TEST_LIBS}
  ${STOUT_LIBS}
  ${GMOCK_LFLAG}
  )

if (NOT WIN32)
  set(STOUT_TEST_LIBS
    ${STOUT_TEST_LIBS}
    ${GTEST_LFLAG}
    )
endif ()
