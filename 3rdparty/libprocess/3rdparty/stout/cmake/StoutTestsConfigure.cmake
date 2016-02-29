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
  STOUT_TESTS_TARGET stout_tests
  CACHE STRING "Target we use to refer to tests for the stout library")

# COMPILER CONFIGURATION.
#########################
if (APPLE)
  # GTEST on OSX needs its own tr1 tuple.
  add_definitions(-DGTEST_USE_OWN_TR1_TUPLE=1 -DGTEST_LANG_CXX11)
endif (APPLE)

# DEFINE PROCESS LIBRARY DEPENDENCIES. Tells the process library build targets
# download/configure/build all third-party libraries before attempting to build.
################################################################################
set(STOUT_TEST_DEPENDENCIES
  ${STOUT_TEST_DEPENDENCIES}
  ${STOUT_DEPENDENCIES}
  ${GMOCK_TARGET}
  ${GTEST_TARGET}
  )

# DEFINE THIRD-PARTY INCLUDE DIRECTORIES. Tells compiler toolchain where to get
# headers for our third party libs (e.g., -I/path/to/glog on Linux)..
###############################################################################
set(STOUT_TEST_INCLUDE_DIRS
  ${STOUT_TEST_INCLUDE_DIRS}
  ${STOUT_INCLUDE_DIRS}
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
endif (NOT WIN32)
