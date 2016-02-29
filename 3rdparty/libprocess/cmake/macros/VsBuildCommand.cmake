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

##############################################################
# VS_BUILD_CMD generates a build command for project which have Visual Studio
# Solution File(.sln). For example, when we want to build GLOG through Visual
# Studio in command line, we would run this command. It would define
# ${LIB_NAME_UPPER}_BUILD_CMD with build command finally.
function(
    VS_BUILD_CMD
    LIB_NAME
    SOLUTION_FILE
    BUILD_CONFIGURATION
    PROJECT_ROOT
    PROJECT_NAMES)

  string(TOUPPER ${LIB_NAME} LIB_NAME_UPPER)

  set(VS_BUILD_SCRIPT
    ${CMAKE_SOURCE_DIR}/3rdparty/libprocess/cmake/macros/VsBuildCommand.bat)
  set(BUILD_CMD_VAR ${LIB_NAME_UPPER}_BUILD_CMD)

  # Generate the build command with ${VS_BUILD_SCRIPT}.
  ## 1. Convert the path to Windows style.
  file(
    TO_NATIVE_PATH
    "${VS_BUILD_SCRIPT} ${SOLUTION_FILE} ${BUILD_CONFIGURATION} ${PROJECT_ROOT} ${PROJECT_NAMES}"
    BUILD_CMD_STRING)
  ## 2. Convert the command to list so that CMake could handle it correctly.
  string(REPLACE " " ";" BUILD_CMD ${BUILD_CMD_STRING})

  set(${BUILD_CMD_VAR} ${BUILD_CMD} PARENT_SCOPE)
endfunction()