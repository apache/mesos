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

option(DISABLE_PARALLEL_TEST_EXECUTION "Do not execute tests in parallel" OFF)
if (DISABLE_PARALLEL_TEST_EXECUTION)
  unset(TEST_DRIVER CACHE)
else ()
  set(TEST_DRIVER
    "${PROJECT_SOURCE_DIR}/support/mesos-gtest-runner.py" CACHE STRING
    "GTest driver to use")
  mark_as_advanced(TEST_DRIVER)
endif ()

# CONFIGURE COMPILER.
#####################
include(CompilationConfigure)

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
  ${MESOS_SRC_DIR})

# Make directories that generated Mesos code goes into.
add_custom_target(
  make_bin_include_dir ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MESOS_BIN_INCLUDE_DIR})

add_custom_target(
  make_bin_src_dir ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MESOS_BIN_SRC_DIR})

add_custom_target(
  make_bin_java_dir ALL
  DEPENDS make_bin_src_dir
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MESOS_BIN_SRC_DIR}/java/generated)

add_custom_target(
  make_bin_jni_dir ALL
  DEPENDS make_bin_src_dir
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MESOS_BIN_SRC_DIR}/java/jni)

# MESOS SCRIPT CONFIGURATION.
#############################
# Define variables required to configure these scripts,
# and also the Java build script `mesos.pom.in`.
set(abs_top_srcdir "${CMAKE_SOURCE_DIR}")
set(abs_top_builddir "${CMAKE_BINARY_DIR}")

if (NOT WIN32)
  # Create build bin/ directory. We will place configured scripts here.
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

  # Define the variables required to configure these scripts.
  set(VERSION "${PACKAGE_VERSION}")

  # Find all scripts to configure. The scripts are in the bin/ directory, and
  # end with the `.in` suffix. For example `mesos.sh.in`.
  file(GLOB_RECURSE BIN_FILES "${CMAKE_SOURCE_DIR}/bin/*.in")
  foreach (BIN_FILE ${BIN_FILES})
    # Strip the `.in` suffix off. This will be the script's name in the build
    # bin/ directory. For example, `mesos.sh.in` -> `mesos.sh`.
    string(REGEX MATCH "(.*).in$" MATCH ${BIN_FILE})
    get_filename_component(OUTPUT_BIN_FILE "${CMAKE_MATCH_1}" NAME)

    configure_file(
      ${BIN_FILE}
      "${CMAKE_BINARY_DIR}/bin/${OUTPUT_BIN_FILE}"
      @ONLY)
  endforeach ()
endif ()
