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

# FIND_PACKAGE_HELPER is a convenience function that is intended for use
# in conjunction with the `find_package` CMake command.  Our usage of
# `find_package(<package>)` will search for files named `Find<package>.cmake`
# in our configured CMake include directories and load those files as modules.
#
# The `Find<package>.cmake` files should call this helper function only after
# defining a set of variables:
#   * POSSIBLE_${PACKAGE_NAME}_INCLUDE_DIRS
#     A list of possible locations to find the `HEADER_FILE`.
#   * POSSIBLE_${PACKAGE_NAME}_LIB_DIRS
#     A list of possible locations to find the libraries (below).
#   * ${PACKAGE_NAME}_LIBRARY_NAMES
#     A list of all the library names to search for.
#
# Upon returning, this function will set the following variables in
# the caller's scope:
#   * ${PACKAGE_NAME}_INCLUDE_DIR - Defines where to find header files.
#   * ${PACKAGE_NAME}_LIBS        - Defines the name of library files.
function(FIND_PACKAGE_HELPER PACKAGE_NAME HEADER_FILE)
  # Return early if the package has already been found.
  if (${PACKAGE_NAME}_FOUND)
    return()
  endif ()

  # Put the return variables in a known state (empty).
  unset(${PACKAGE_NAME}_INCLUDE_DIR)
  unset(${PACKAGE_NAME}_LIBS)

  # Search for the header file in the possible include directories.
  find_path(
    ${PACKAGE_NAME}_INCLUDE_DIR
    ${HEADER_FILE}
    HINTS ${POSSIBLE_${PACKAGE_NAME}_INCLUDE_DIRS})

  # Check if the header file was found.
  string(
    COMPARE NOTEQUAL
    "${PACKAGE_NAME}_INCLUDE_DIR-NOTFOUND"
    ${${PACKAGE_NAME}_INCLUDE_DIR}

    # Output variable.
    ${PACKAGE_NAME}_INCLUDE_DIR_FOUND)

  # Error out if the header is not found.
  if (NOT ${${PACKAGE_NAME}_INCLUDE_DIR_FOUND})
    message(FATAL_ERROR "Could not find ${PACKAGE_NAME} header ${HEADER_FILE}")
  endif ()

  # Search for all the libraries in the possible library directories.
  foreach (LIBRARY_NAME ${${PACKAGE_NAME}_LIBRARY_NAMES})
    # NOTE: `find_library` will set the output variable in the cache,
    # meaning that subsequent calls to `find_library` with the same
    # output variable give the same result. Therefore, we unset the
    # cached variable each time before calling `find_library`.
    unset(${PACKAGE_NAME}_LIB_PATH CACHE)

    find_library(
      ${PACKAGE_NAME}_LIB_PATH
      NAMES ${LIBRARY_NAME}
      PATHS ${POSSIBLE_${PACKAGE_NAME}_LIB_DIRS}
      NO_DEFAULT_PATH)

    # NOTE: We call this again to search the default paths,
    # but it will only do so if it wasn't found above.
    # This is recommended by the CMake documentation.
    find_library(
      ${PACKAGE_NAME}_LIB_PATH
      NAMES ${LIBRARY_NAME})

    # Check if the library was found.
    string(
      COMPARE NOTEQUAL
      "${PACKAGE_NAME}_LIB_PATH-NOTFOUND"
      ${${PACKAGE_NAME}_LIB_PATH}

      # Output variable.
      ${PACKAGE_NAME}_LIB_PATH_FOUND)

    # Error out if any of the libraries are not found.
    if (NOT ${${PACKAGE_NAME}_LIB_PATH_FOUND})
      message(FATAL_ERROR "Could not find ${PACKAGE_NAME} library ${LIBRARY_NAME}")
    endif ()

    list(APPEND ${PACKAGE_NAME}_LIBS ${${PACKAGE_NAME}_LIB_PATH})
  endforeach ()

  set(${PACKAGE_NAME}_FOUND 1 PARENT_SCOPE)
  message(STATUS "Found ${PACKAGE_NAME} library: ${${PACKAGE_NAME}_LIBS}")
  message(STATUS "Found ${PACKAGE_NAME} header: ${${PACKAGE_NAME}_INCLUDE_DIR}")

  # Export the return variables.
  set(${PACKAGE_NAME}_LIBS ${${PACKAGE_NAME}_LIBS} PARENT_SCOPE)
  set(${PACKAGE_NAME}_INCLUDE_DIR ${${PACKAGE_NAME}_INCLUDE_DIR} PARENT_SCOPE)
endfunction()
