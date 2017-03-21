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

# CMAKE OPTIONS.
################
# NOTE: This silences a warning from CMake policy CMP0042.
# By default, `CMAKE_MACOSX_RPATH` is ON, but CMake will continue to warn
# unless the value is set explicitly in the build files.
set(CMAKE_MACOSX_RPATH ON)

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

if (WIN32)
  set(MESOS_DEFAULT_LIBRARY_LINKAGE "STATIC")
else (WIN32)
  set(MESOS_DEFAULT_LIBRARY_LINKAGE "SHARED")
  set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)
endif (WIN32)

# DEFINE DIRECTORY STRUCTURE MESOS PROJECT.
###########################################
set(MESOS_SRC_DIR     ${CMAKE_SOURCE_DIR}/src)
set(MESOS_BIN         ${CMAKE_BINARY_DIR})
set(MESOS_BIN_SRC_DIR ${MESOS_BIN}/src)

# Convenience variables for include directories of third-party dependencies.
set(MESOS_PUBLIC_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
set(MESOS_BIN_INCLUDE_DIR    ${CMAKE_BINARY_DIR}/include)

set(
  MESOS_PROTOBUF_HEADER_INCLUDE_DIRS
  ${MESOS_BIN_INCLUDE_DIR}
  ${MESOS_BIN_INCLUDE_DIR}/mesos
  ${MESOS_BIN_SRC_DIR}
  ${MESOS_SRC_DIR}
  )

# Make directories that generated Mesos code goes into.
add_custom_target(
  make_bin_include_dir ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MESOS_BIN_INCLUDE_DIR})

add_custom_target(
  make_bin_src_dir ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MESOS_BIN_SRC_DIR})

# CONFIGURE AGENT.
##################
include(AgentConfigure)

# CONFIGURE MASTER.
##################
include(MasterConfigure)

# CONFIGURE EXAMPLE MODULES AND FRAMEWORKS.
###########################################
include(ExamplesConfigure)

# DEFINE MESOS BUILD TARGETS.
#############################
set(
  AGENT_TARGET mesos-agent
  CACHE STRING "Target we use to refer to agent executable")

set(
  DEFAULT_EXECUTOR_TARGET mesos-default-executor
  CACHE STRING "Target for the default executor")

set(
  MESOS_CONTAINERIZER mesos-containerizer
  CACHE STRING "Target for containerizer")

set(
  MESOS_DOCKER_EXECUTOR mesos-docker-executor
  CACHE STRING "Target for docker executor")

set(
  MESOS_EXECUTOR mesos-executor
  CACHE STRING "Target for command executor")

set(
  MESOS_FETCHER mesos-fetcher
  CACHE STRING "Target for fetcher")

if (NOT WIN32)
  set(
    MESOS_IO_SWITCHBOARD mesos-io-switchboard
    CACHE STRING "Target for the IO switchboard")

  set(
    MESOS_CNI_PORT_MAPPER mesos-cni-port-mapper
    CACHE STRING "Target for the CNI port-mapper plugin")
endif (NOT WIN32)

set(
  MESOS_MASTER mesos-master
  CACHE STRING "Target for master")

set(
  MESOS_TCP_CONNECT mesos-tcp-connect
  CACHE STRING "Target for tcp-connect")

set(
  MESOS_USAGE mesos-usage
  CACHE STRING "Target for usage")

# MESOS LIBRARY CONFIGURATION.
##############################
set(
  MESOS_TARGET mesos-and-binaries
  CACHE STRING "Target that includes libmesos and all required binaries")

add_custom_target(${MESOS_TARGET} ALL)

set(MESOS_LIBS_TARGET mesos-${MESOS_PACKAGE_VERSION}
    CACHE STRING "Library of master and agent code")

set(MESOS_PROTOBUF_TARGET mesos-protobufs
    CACHE STRING "Library of protobuf definitions used by Mesos")

# MESOS SCRIPT CONFIGURATION.
#############################
if (NOT WIN32)
  # Create build bin/ directory. We will place configured scripts here.
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tmp")

  # Define the variables required to configure these scripts.
  set(abs_top_srcdir "${CMAKE_SOURCE_DIR}")
  set(abs_top_builddir "${CMAKE_BINARY_DIR}")
  set(VERSION "${PACKAGE_VERSION}")

  # Find all scripts to configure. The scripts are in the bin/ directory, and
  # end with the `.in` suffix. For example `mesos.sh.in`.
  file(GLOB_RECURSE BIN_FILES "${CMAKE_SOURCE_DIR}/bin/*.in")
  foreach (BIN_FILE ${BIN_FILES})
    # Strip the `.in` suffix off. This will be the script's name in the build
    # bin/ directory. For example, `mesos.sh.in` -> `mesos.sh`.
    string(REGEX MATCH "(.*).in$" MATCH ${BIN_FILE})
    get_filename_component(OUTPUT_BIN_FILE "${CMAKE_MATCH_1}" NAME)

    # Configure. Because CMake does not support configuring and setting
    # permissions, we do this in a two-step process: we first configure the
    # file, placing the configured file in a temporary directory, and then we
    # "copy" the configured file to the build `bin` directory, setting the
    # permissions as we go.
    #
    # NOTE: We use the `@ONLY` argument here to avoid trying to substitute
    # the value of `${@}`, which we frequently use in the scripts.
    configure_file(
      ${BIN_FILE}
      "${CMAKE_BINARY_DIR}/bin/tmp/${OUTPUT_BIN_FILE}"
      @ONLY)

    file(COPY "${CMAKE_BINARY_DIR}/bin/tmp/${OUTPUT_BIN_FILE}"
      DESTINATION "${CMAKE_BINARY_DIR}/bin"
      FILE_PERMISSIONS WORLD_EXECUTE OWNER_READ OWNER_WRITE)
  endforeach (BIN_FILE)
  file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/bin/tmp")
endif (NOT WIN32)
