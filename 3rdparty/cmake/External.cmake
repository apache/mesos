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
#   BOOST_VERSION    (e.g., 1.53.0)
#   BOOST_TARGET     (a target folder name to put dep in e.g., boost-1.53.0)
#   BOOST_CMAKE_ROOT (where to have CMake put the uncompressed source, e.g.,
#                     build/3rdparty/boost-1.53.0)
#   BOOST_ROOT       (where the code goes in various stages of build, e.g.,
#                     build/.../boost-1.53.0/src, which might contain folders
#                     build-1.53.0-build, -lib, and so on, for each build step
#                     that dependency has)
function(EXTERNAL
  LIB_NAME
  LIB_VERSION
  BIN_ROOT)

  string(TOUPPER ${LIB_NAME} LIB_NAME_UPPER)

  # Names of variables we will set in this function.
  set(VERSION_VAR    ${LIB_NAME_UPPER}_VERSION)    # e.g., BOOST_VERSION
  set(TARGET_VAR     ${LIB_NAME_UPPER}_TARGET)     # e.g., BOOST_TARGET
  set(CMAKE_ROOT_VAR ${LIB_NAME_UPPER}_CMAKE_ROOT) # e.g., BOOST_CMAKE_ROOT
  set(ROOT_VAR       ${LIB_NAME_UPPER}_ROOT)       # e.g., BOOST_ROOT

  # Generate data that we will put in the above variables.
  # NOTE: bundled packages are untar'd into the BIN_ROOT, which is why we're
  #       pointing the source root into BIN_ROOT rather than SRC_ROOT.
  # TODO(hausdorff): SRC_DATA doesn't work for HTTP, LIBEV, GMOCK, or GTEST.
  set(VERSION_DATA    ${LIB_VERSION})
  set(TARGET_DATA     ${LIB_NAME}-${VERSION_DATA})
  set(CMAKE_ROOT_DATA ${BIN_ROOT}/${TARGET_DATA})
  set(ROOT_DATA       ${CMAKE_ROOT_DATA}/src/${TARGET_DATA})

  # Finally, EXPORT THE ABOVE VARIABLES. We take the data variables we just
  # defined, and export them to variables in the parent scope.
  #
  # NOTE: The "export" step is different from the "define the data vars" step
  #       because an expression like ${VERSION_VAR} will evaluate to
  #       something like "BOOST_VERSION", not something like "1.53.0". That
  #       is: to get the version in the parent scope we would do something
  #       like ${BOOST_VERSION}, which might evaluate to something like
  #       "1.53.0". So in this function, if you wanted to generate (e.g.) the
  #       target variable, it is not sufficient to write
  #       "${LIB_NAME}-${VERSION_VAR}", because this would result in
  #       something like "boost-BOOST_VERSION" when what we really wanted was
  #       "boost-1.53.0". Hence, these two steps are different.
  set(${VERSION_VAR}    # e.g., 1.53.0
    ${VERSION_DATA}
    PARENT_SCOPE)

  set(${TARGET_VAR}     # e.g., boost-1.53.0
    ${TARGET_DATA}
    PARENT_SCOPE)

  set(${CMAKE_ROOT_VAR} # e.g., build/3rdparty/boost-1.53.0
    ${CMAKE_ROOT_DATA}
    PARENT_SCOPE)

  set(${ROOT_VAR}       # e.g., build/.../boost-1.53.0/src
    ${ROOT_DATA}
    PARENT_SCOPE)
endfunction()
