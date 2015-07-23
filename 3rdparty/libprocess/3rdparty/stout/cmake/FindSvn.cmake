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
# IMPORTANT NOTE: Much of this file is taken almost-directly from the
#                 FindApr.cmake file in this project. This file is itself taken
#                 from an external source; see comments for details.

# This module helps to find the Subversion packages.
#
# USAGE: to use this module, add the following line to your CMake project:
#   find_package(Svn)
#
# To make linking to Subversion simple, this module defines:
#   * SVN_FOUND       (if false, linking build will fail)
#   * SVN_INCLUDE_DIR (defineswhere to find svn_client.h, etc.)
#   * SVN_LIBS   (binaries for SVN and friends)
#
# Also defined, but not for general use are:
#   * SVN_LIB (where to find the APR library)

# CONFIGURE OUR SEARCH. Specify what we're looking for, and which directories
# we're going to look through to find them.
#############################################################################
set(POSSIBLE_SVN_INCLUDE_DIRECTORIES
  ${POSSIBLE_SVN_INCLUDE_DIRECTORIES}
  /usr/local/include/subversion-1
  /usr/include/subversion-1
  )

set(SVN_LIB_NAMES
  ${SVN_LIB_NAMES}
  svn_client-1
  svn_delta-1
  svn_diff-1
  svn_fs-1
  svn_fs_base-1
  svn_fs_fs-1
  svn_fs_util-1
  svn_ra-1
  svn_ra_local-1
  svn_ra_serf-1
  svn_ra_svn-1
  svn_repos-1
  svn_subr-1
  svn_wc-1
  )

set(POSSIBLE_SVN_LIB_DIRECTORIES
  ${POSSIBLE_SVN_LIB_DIRECTORIES}
  /usr/lib
  /usr/local/lib
  )

# SEARCH FOR SVN LIBRARIES.
###########################
find_path(SVN_INCLUDE_DIR svn_client.h ${POSSIBLE_SVN_INCLUDE_DIRECTORIES})

# Did we find the include directory?
string(
  COMPARE NOTEQUAL
  "SVN_INCLUDE_DIR-NOTFOUND"
  ${SVN_INCLUDE_DIR} # Value set to `SVN_INCLUDE_DIR-NOTFOUND` if not found.
  SVN_INCLUDE_DIR_FOUND
  )

foreach (LIB_NAME ${SVN_LIB_NAMES})
  find_library(
    ${LIB_NAME}_PATH
    NAMES ${LIB_NAME}
    PATHS ${POSSIBLE_SVN_LIB_DIRECTORIES}
    )

  # Did we find the library?
  string(
    COMPARE NOTEQUAL
    "${LIB_NAME}_PATH-NOTFOUND"
    ${${LIB_NAME}_PATH} # Value set to ${${LIB_NAME}_PATH}-NOTFOUND if not found.
    ${LIB_NAME}_PATH_FOUND
    )

  if ("${${LIB_NAME}_PATH_FOUND}")
    message(STATUS "Found SVN lib: ${${LIB_NAME}_PATH}")
    list(APPEND SVN_LIBS ${${LIB_NAME}_PATH})
  else ("${${LIB_NAME}_PATH_FOUND}")
    message(FATAL_ERROR "Could not find SVN library ${${LIB_NAME}_PATH}")
  endif ("${${LIB_NAME}_PATH_FOUND}")
endforeach (LIB_NAME ${SVN_LIB_NAMES})

list(LENGTH SVN_LIB_NAMES NUM_REQD_LIBS)
list(LENGTH SVN_LIBS NUM_FOUND_LIBS)

# Results.
if ((NUM_FOUND_LIBS AND (NUM_REQD_LIBS EQUAL NUM_FOUND_LIBS)) AND
    SVN_INCLUDE_DIR_FOUND)
  if (NOT Svn_FIND_QUIETLY)
    message(STATUS "Found SVN: ${SVN_LIBS}")
  endif(NOT Svn_FIND_QUIETLY)
else ((NUM_FOUND_LIBS AND (NUM_REQD_LIBS EQUAL NUM_FOUND_LIBS)) AND
      SVN_INCLUDE_DIR_FOUND)
  if (Svn_FIND_REQUIRED)
    message(FATAL_ERROR "Could not find SVN library")
  endif (Svn_FIND_REQUIRED)
endif ((NUM_FOUND_LIBS AND (NUM_REQD_LIBS EQUAL NUM_FOUND_LIBS)) AND
       SVN_INCLUDE_DIR_FOUND)

# Export variables.
mark_as_advanced(
  SVN_LIB
  SVN_INCLUDE_DIR
  )