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

find_package(Apr REQUIRED)
find_package(Svn REQUIRED)

# DEFINE PROCESS LIBRARY DEPENDENCIES. Tells the process library build targets
# download/configure/build all third-party libraries before attempting to build.
################################################################################
set(STOUT_TEST_DEPENDENCIES
  ${GMOCK_TARGET}
  ${GTEST_TARGET}
  ${PROTOBUF_TARGET}
  )

# DEFINE THIRD-PARTY INCLUDE DIRECTORIES. Tells compiler toolchain where to get
# headers for our third party libs (e.g., -I/path/to/glog on Linux)..
###############################################################################
set(STOUT_TEST_INCLUDE_DIRS
  ${STOUT_TEST_INCLUDE_DIRS}
  ${STOUT}/include
  ${BOOST_ROOT}
  ${PICOJSON_ROOT}
  ${APR_INCLUDE_DIR}
  ${SVN_INCLUDE_DIR}
  ${GMOCK_ROOT}/include
  ${GTEST_SRC}/include
  ${PROTOBUF_LIB}/include
  src
  )

if (WIN32)
  set(STOUT_TEST_INCLUDE_DIRS
    ${STOUT_TEST_INCLUDE_DIRS}
    ${GLOG_ROOT}/src/windows
    )
else (WIN32)
  set(STOUT_TEST_INCLUDE_DIRS
    ${STOUT_TEST_INCLUDE_DIRS}
    ${GLOG_LIB}/include
    )
endif (WIN32)

# DEFINE THIRD-PARTY LIB INSTALL DIRECTORIES. Used to tell the compiler
# toolchain where to find our third party libs (e.g., -L/path/to/glog on
# Linux).
########################################################################
set(STOUT_TEST_LIB_DIRS
  ${STOUT_TEST_LIB_DIRS}
  ${GLOG_LIB}/lib
  ${APR_LIBS}
  ${SVN_LIBS}
  ${GMOCK_ROOT}-build/lib/.libs
  ${GMOCK_ROOT}-build/gtest/lib/.libs
  ${PROTOBUF_LIB}/lib
  )

# DEFINE THIRD-PARTY LIBS. Used to generate flags that the linker uses to
# include our third-party libs (e.g., -lglog on Linux).
#########################################################################
set(STOUT_TEST_LIBS
  ${STOUT_TEST_LIBS}
  ${CMAKE_THREAD_LIBS_INIT}
  glog
  dl
  apr-1
  protobuf
  gtest
  gmock
  ${SVN_LIBS}
  )

if (NOT WIN32)
  find_library(LIBRT_LIBRARIES rt REQUIRED)

  set(STOUT_TEST_LIBS
    ${STOUT_TEST_LIBS}
    ${LIBRT_LIBRARIES}
    )
endif (NOT WIN32)
