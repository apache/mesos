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
include(CheckCXXCompilerFlag)

# SYSTEM CONFIGURATION INFORMATION.
###################################
string(TOUPPER "${CMAKE_SYSTEM_NAME}"      OS_NAME)
string(TOUPPER "${CMAKE_SYSTEM_VERSION}"   OS_VER)
string(TOUPPER "${CMAKE_SYSTEM_PROCESSOR}" SYS_ARCH)

# CMAKE CONFIGURE LOGO.
#######################
message(STATUS "************************************************************")
message(STATUS "********* Beginning Mesos CMake configuration step *********")
message(STATUS "************************************************************")
message(STATUS "INSTALLATION PREFIX: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "MACHINE SPECS:")
message(STATUS "    Hostname: ${HOSTNAME}")
message(STATUS "    OS:       ${OS_NAME}(${OS_VER})")
message(STATUS "    Arch:     ${SYS_ARCH}")
message(STATUS "    BitMode:  ${BIT_MODE}")
message(STATUS "    BuildID:  ${BUILDID}")
message(STATUS "************************************************************")

# SET UP TESTING INFRASTRUCTURE.
################################
enable_testing()

# CONFIGURE COMPILATION.
########################
string(COMPARE EQUAL ${CMAKE_SYSTEM_NAME} "Linux" LINUX)

if (_DEBUG)
  set(CMAKE_BUILD_TYPE Debug)
endif (_DEBUG)

# Make sure C++ 11 features we need are supported. This is split into two
# cases: Windows and "other platforms".
#   * For "other platforms", we simply check if the C++11 flags work
#   * For Windows, it looks like (1) C++11 is enabled by default on MSVC 1900 or
#     later, and (2) C++11 is totally broken for 1800 or earlier (i.e., Mesos
#     will not compile on MSVC pre-1900). So, when in Windows, we just check the
#     MSVC version, and don't try to check or pass in C++11 flags at all.
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
if (WIN32)
  # Windows case first.

  # We don't support compilation against mingw headers (which, e.g., Clang on
  # Windows does at this point), because this is likely to cost us more effort
  # to support than it will be worth at least in the short term.
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES MSVC)
    message(
      WARNING
      "Mesos does not support compiling on Windows with "
      "${CMAKE_CXX_COMPILER_ID}. Please use MSVC.")
  endif (NOT CMAKE_CXX_COMPILER_ID MATCHES MSVC)

  # MSVC 1900 supports C++11; earliser versions don't. So, warn if you try to
  # use anything else.
  if (${MSVC_VERSION} LESS 1900)
    message(
      WARNING
      "Mesos does not support compiling on MSVC versions earlier than 1900. "
      "Please use MSVC 1900 (included with Visual Studio 2015 or later).")
  endif (${MSVC_VERSION} LESS 1900)

  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
elseif (COMPILER_SUPPORTS_CXX11)
  # Finally, on non-Windows platforms, we must check that the current compiler
  # supports C++11.
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
else (WIN32)
  message(
    FATAL_ERROR
    "The compiler ${CMAKE_CXX_COMPILER} does not support the `-std=c++11` "
    "flag. Please use a different C++ compiler.")
endif (WIN32)

# Convenience flags to simplify Windows support in C++ source.
if (MSVC)
  add_definitions(-DMESOS_MSVC)
endif (MSVC)

# Compiler constants required for third-party libs.
if (WIN32)
  # Windows-specific workaround for a glog issue documented here[1].
  # Basically, Windows.h and glog/logging.h both define ERROR. Since we don't
  # need the Windows ERROR, we can use this flag to avoid defining it at all.
  # Unlike the other fix (defining GLOG_NO_ABBREVIATED_SEVERITIES), this fix
  # is guaranteed to require no changes to the original Mesos code. See also
  # the note in the code itself[2].
  #
  # [1] https://google-glog.googlecode.com/svn/trunk/doc/glog.html#windows
  # [2] https://code.google.com/p/google-glog/source/browse/trunk/src/windows/glog/logging.h?r=113
  add_definitions(-DNOGDI)
  add_definitions(-DNOMINMAX)
endif (WIN32)

# THIRD-PARTY CONFIGURATION.
############################
# NOTE: The third-party configuration variables exported here are used
# throughout the project, so it's important that this config script goes here.
include(ProcessConfigure)
