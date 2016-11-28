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

include(StoutTestsConfigure)

set(
  PROCESS_TESTS_TARGET process_tests
  CACHE STRING "Target we use to refer to tests for the process library")

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
