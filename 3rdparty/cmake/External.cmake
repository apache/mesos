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

###############################################################
# EXTERNAL defines a few variables that make it easy for us to track the
# directory structure of a dependency. In particular, if our library's name is
# boost, we will define the following variables:
#
#   BOOST_TARGET     (a target folder name to put dep in e.g., `boost-1.53.0`)
#   BOOST_CMAKE_ROOT (where to have CMake put the uncompressed source, e.g.,
#                     `build/3rdparty/boost-1.53.0`)
#   BOOST_ROOT       (where the code goes in various stages of build, e.g.,
#                     `build/.../boost-1.53.0/src`, which might contain folders
#                     `build-1.53.0-build,` `-lib`, and so on, for each build step
#                     that dependency has)
function(EXTERNAL
  LIB_NAME
  LIB_VERSION
  BIN_ROOT)

  string(TOUPPER ${LIB_NAME} LIB_NAME_UPPER)

  # Names of variables we will set in this function.
  set(TARGET_VAR     ${LIB_NAME_UPPER}_TARGET)     # e.g., BOOST_TARGET
  set(CMAKE_ROOT_VAR ${LIB_NAME_UPPER}_CMAKE_ROOT) # e.g., BOOST_CMAKE_ROOT
  set(ROOT_VAR       ${LIB_NAME_UPPER}_ROOT)       # e.g., BOOST_ROOT

  # Generate data that we will put in the above variables.
  # NOTE: bundled packages are extracted into the BIN_ROOT, which is why we're
  #       pointing the source root into BIN_ROOT rather than SRC_ROOT.
  set(TARGET_DATA     ${LIB_NAME}-${LIB_VERSION})
  set(CMAKE_ROOT_DATA ${BIN_ROOT}/${TARGET_DATA})
  set(ROOT_DATA       ${CMAKE_ROOT_DATA}/src/${TARGET_DATA})

  # Finally, export the above variables to the parent scope.
  set(${TARGET_VAR}     ${TARGET_DATA}     PARENT_SCOPE)
  set(${CMAKE_ROOT_VAR} ${CMAKE_ROOT_DATA} PARENT_SCOPE)
  set(${ROOT_VAR}       ${ROOT_DATA}       PARENT_SCOPE)
endfunction()
