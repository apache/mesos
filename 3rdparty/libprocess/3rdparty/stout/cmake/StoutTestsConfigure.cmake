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

if (NOT WIN32)
  # TODO(hausdorff): (cf. MESOS-3181) Add support for attempting to find these
  # packages on Windows, and then if that fails, use CMake macros/functions to
  # download, configure, and build them.
  #
  # WHY: Windows does not have a good package manager, so getting some of our
  # dependencies can be really annoying in some circumstances. For this reason,
  # we should support a CMake-based "distribution channel", even though this
  # way is sure to be more hackish.
  find_package(Apr REQUIRED)
  find_package(Svn REQUIRED)
endif (NOT WIN32)

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
  ${BOOST_TARGET}
  ${GLOG_TARGET}
  ${GMOCK_TARGET}
  ${GTEST_TARGET}
  ${PROTOBUF_TARGET}
  )

if (WIN32)
  set(STOUT_TEST_DEPENDENCIES ${STOUT_TEST_DEPENDENCIES} ${CURL_TARGET})
endif (WIN32)

# DEFINE THIRD-PARTY INCLUDE DIRECTORIES. Tells compiler toolchain where to get
# headers for our third party libs (e.g., -I/path/to/glog on Linux)..
###############################################################################
set(STOUT_TEST_INCLUDE_DIRS
  ${STOUT_TEST_INCLUDE_DIRS}
  ${STOUT_INCLUDE_DIR}
  ${BOOST_INCLUDE_DIR}
  ${PICOJSON_INCLUDE_DIR}
  ${APR_INCLUDE_DIR}
  ${SVN_INCLUDE_DIR}
  ${GMOCK_INCLUDE_DIR}
  ${GTEST_INCLUDE_DIR}
  ${PROTOBUF_INCLUDE_DIR}
  src
  ${GLOG_INCLUDE_DIR}
  )

if (WIN32)
  set(STOUT_TEST_INCLUDE_DIRS ${STOUT_TEST_INCLUDE_DIRS} ${CURL_INCLUDE_DIR})
endif (WIN32)

# DEFINE THIRD-PARTY LIB INSTALL DIRECTORIES. Used to tell the compiler
# toolchain where to find our third party libs (e.g., -L/path/to/glog on
# Linux).
########################################################################
set(STOUT_TEST_LIB_DIRS
  ${STOUT_TEST_LIB_DIRS}
  ${APR_LIBS}
  ${SVN_LIBS}
  ${GMOCK_LIB_DIR}
  ${GMOCK_ROOT}-build/gtest/lib/.libs
  ${GLOG_LIB_DIR}
  ${PROTOBUF_LIB_DIR}
  )

if (WIN32)
  set(STOUT_TEST_LIB_DIRS ${STOUT_TEST_LIB_DIRS} ${CURL_LIB_DIR})
endif (WIN32)

# DEFINE THIRD-PARTY LIBS. Used to generate flags that the linker uses to
# include our third-party libs (e.g., -lglog on Linux).
#########################################################################
set(STOUT_TEST_LIBS
  ${STOUT_TEST_LIBS}
  ${CMAKE_THREAD_LIBS_INIT}
  ${GMOCK_LFLAG}
  ${SVN_LIBS}
  ${GLOG_LFLAG}
  ${PROTOBUF_LFLAG}
  )

if (WIN32)
  set(STOUT_TEST_LIBS ${STOUT_TEST_LIBS} ${CURL_LFLAG})
else (WIN32)
  set(STOUT_TEST_LIBS
    ${STOUT_TEST_LIBS}
    ${GTEST_LFLAG}
    dl
    apr-1
    )
endif (WIN32)

# TODO(hausdorff): The `LINUX` flag comes from MesosConfigure; when we
# port the bootstrap script to CMake, we should also copy this logic
# into .cmake files in the Stout and Process libraries' folders
# individually.
if (LINUX)
  set(STOUT_TEST_LIBS ${STOUT_TEST_LIBS} rt)
endif (LINUX)