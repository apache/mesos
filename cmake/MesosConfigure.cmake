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

# CONFIGURE COMPILER.
#####################
include(CompilationConfigure)

# THIRD-PARTY CONFIGURATION.
############################
# NOTE: The third-party configuration variables exported here are used
# throughout the project, so it's important that this config script goes here.
include(Mesos3rdpartyConfigure)
include(Process3rdpartyConfigure)

# Generate a batch script that will build Mesos. Any project referencing Mesos
# can then build it by calling this script.
if (WIN32)
  VS_BUILD_CMD(
      MESOS
      ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.sln
      ${CMAKE_BUILD_TYPE}
      ""
      "")

  string(REPLACE ";" " " MESOS_BUILD_CMD "${MESOS_BUILD_CMD}")
  file(WRITE ${CMAKE_BINARY_DIR}/make.bat ${MESOS_BUILD_CMD})
endif (WIN32)

# DEFINE DIRECTORY STRUCTURE MESOS PROJECT.
###########################################
set(MESOS_SRC_DIR     ${CMAKE_SOURCE_DIR}/src)
set(MESOS_BIN         ${CMAKE_BINARY_DIR})
set(MESOS_BIN_SRC_DIR ${MESOS_BIN}/src)

# Convenience variables for include directories of third-party dependencies.
set(MESOS_PUBLIC_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
set(MESOS_BIN_INCLUDE_DIR    ${CMAKE_BINARY_DIR}/include)

# Make directories that generated Mesos code goes into.
add_custom_target(
  make_bin_include_dir ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MESOS_BIN_INCLUDE_DIR})

add_custom_target(
  make_bin_src_dir ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MESOS_BIN_SRC_DIR})

# CONFIGURE AGENT.
##################
include(SlaveConfigure)

# CONFIGURE MASTER.
##################
include(MasterConfigure)

# MESOS LIBRARY CONFIGURATION.
##############################
set(MESOS_TARGET mesos-${MESOS_PACKAGE_VERSION})
set(MESOS_PROTOBUF_TARGET mesos-protobufs
    CACHE STRING "Library of protobuf definitions used by Mesos"
    )
