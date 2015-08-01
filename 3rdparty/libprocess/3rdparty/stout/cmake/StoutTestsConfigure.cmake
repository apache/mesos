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
if (WIN32)
  # Used to #ifdef out (e.g.) some platform-specific parts of Stout. We choose
  # to define a new flag rather than using an existing flag (e.g., `_WIN32`)
  # because we want to give the build system fine-grained control over how what
  # code is #ifdef'd out in the future, and using only flags defined by our
  # build system to control this logic is the clearest and stablest way of
  # doing this.
  add_definitions(-D__WINDOWS__)
elseif (APPLE)
  # GTEST on OSX needs its own tr1 tuple.
  # TODO(dhamon): Update to gmock 1.7 and pass GTEST_LANG_CXX11 when
  # in C++11 mode.
  add_definitions(-DGTEST_USE_OWN_TR1_TUPLE=1)
endif (WIN32)

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
  set(STOUT_TEST_DEPENDENCIES
    ${STOUT_TEST_DEPENDENCIES}
    ${CURL_TARGET}
  )
endif(WIN32)

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
    ${CURL_ROOT}/include
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
  ${APR_LIBS}
  ${SVN_LIBS}
  ${GMOCK_ROOT}-build/lib/.libs
  ${GMOCK_ROOT}-build/gtest/lib/.libs
  )

if (WIN32)
  # TODO(hausdorff): currently these dependencies have to be built out-of-band
  # by opening Visual Studio, building the project, and then building Mesos. We
  # should write batch scripts that will build these dependencies from the
  # command line. (This is one reason why we're linking to the Debug/ folders,
  # which is not a good idea for release builds anyway.)
  set(STOUT_TEST_LIB_DIRS
    ${STOUT_TEST_LIB_DIRS}
    ${GLOG_ROOT}/Debug
    ${GMOCK_ROOT}/msvc/2010/Debug
    ${PROTOBUF_ROOT}/vsprojects/Debug
    ${CURL_ROOT}/lib
    )
else (WIN32)
  set(STOUT_TEST_LIB_DIRS
    ${STOUT_TEST_LIB_DIRS}
    ${GLOG_LIB}/lib
    ${PROTOBUF_LIB}/lib
    )
endif (WIN32)

# DEFINE THIRD-PARTY LIBS. Used to generate flags that the linker uses to
# include our third-party libs (e.g., -lglog on Linux).
#########################################################################
set(STOUT_TEST_LIBS
  ${STOUT_TEST_LIBS}
  ${CMAKE_THREAD_LIBS_INIT}
  gmock
  ${SVN_LIBS}
  )

if (WIN32)
  # Necessary because the lib names for glog and protobuf are generated
  # incorrectly on Windows. That is, on *nix, the glog binary should be (e.g.)
  # libglog.so, and on Windows it should be glog.lib. But on Windows, it's
  # actually libglog.lib. Hence, we have to special case it here because CMake
  # assumes the library names are generated correctly.
  set(STOUT_TEST_LIBS
    ${STOUT_TEST_LIBS}
    libglog
    libprotobuf
    libcurl_a
    )
else (WIN32)
  set(STOUT_TEST_LIBS
    ${STOUT_TEST_LIBS}
    glog
    gtest
    protobuf
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