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
# IMPORTANT NOTE: I (hausdorff) have copied a *lot* of stuff in this file from
#                 code I found at in the Akumuli[1] project. The original version
#                 of this file was also released under the Apache 2.0 license, and
#                 licensed to the ASF, making it fully compatible with the Mesos
#                 project.
#  [1] https://github.com/akumuli/Akumuli/blob/master/cmake/FindAPR.cmake

# This module helps to find the Apache Portable Runtime (APR) and APR-Util
# packages.
#
# USAGE: to use this module, add the following line to your CMake project:
#   find_package(Apr)
#
# To make linking to APR simple, this module defines:
#   * APR_FOUND and APRUTIL_FOUND             (if false, linking build will fail)
#   * APR_INCLUDE_DIR and APRUTIL_INCLUDE_DIR (defineswhere to find apr.h, etc.)
#   * APR_LIBS and APRUTIL_LIBS     (binaries for APR and friends)
#
# Also defined, but not for general use are:
#   * APR_LIB and APRUTIL_LIB         (where to find the APR library)

# CONFIGURE THE APR SEARCH. Specify what we're looking for, and which directories
# we're going to look through to find them.
#############################################################################
unset(APRUTIL_LIB)
unset(APRUTIL_INCLUDE_DIR)
unset(APR_LIB)
unset(APR_INCLUDE_DIR)
unset(APR_LIBS)
unset(APR_FOUND)

set(POSSIBLE_APR_INCLUDE_DIRS
  ${POSSIBLE_APR_INCLUDE_DIRS}
  /opt/homebrew/opt/apr/include/apr-1
  /usr/local/include/apr-1
  /usr/local/include/apr-1.0
  /usr/include/apr-1
  /usr/include/apr-1.0
  /usr/local/apr/include/apr-1
  )

set(APR_LIB_NAMES ${APR_LIB_NAMES} apr-1)

set(POSSIBLE_APR_LIB_DIRS
  ${POSSIBLE_APR_LIB_DIRS}
  /usr/lib
  /usr/local/lib
  /usr/local/apr/lib
  /opt/homebrew/opt/apr/lib
  )

# SEARCH FOR APR LIBRARIES.
###########################
find_path(APR_INCLUDE_DIR apr.h ${POSSIBLE_APR_INCLUDE_DIRS})

find_library(
  APR_LIB
  NAMES ${APR_LIB_NAMES}
  PATHS ${POSSIBLE_APR_LIB_DIRS}
  )

# Did we find the include directory?
string(
  COMPARE NOTEQUAL
  "APR_INCLUDE_DIR-NOTFOUND"
  ${APR_INCLUDE_DIR} # Value set to APR_INCLUDE_DIR-NOTFOUND if not found.
  APR_INCLUDE_DIR_FOUND
  )

# Did we find the library?
string(
  COMPARE NOTEQUAL
  "APR_LIB-NOTFOUND"
  ${APR_LIB} # Value set to `APR_LIB-NOTFOUND` if not found.
  APR_LIB_FOUND
  )

# APR is considered "found" if we've both an include directory and an APR binary.
if ("${APR_LIB_FOUND}" AND "${APR_INCLUDE_DIR_FOUND}")
  set(APR_LIBS ${APR_LIB})
  set(APR_FOUND 1)
else ("${APR_LIB_FOUND}" AND "${APR_INCLUDE_DIR_FOUND}")
  set(APR_FOUND 0)
endif ("${APR_LIB_FOUND}" AND "${APR_INCLUDE_DIR_FOUND}")

# Results.
if (APR_FOUND)
  if (NOT Apr_FIND_QUIETLY)
    message(STATUS "Found APR headers: ${APR_INCLUDE_DIR}")
    message(STATUS "Found APR library: ${APR_LIBS}")
  endif (NOT Apr_FIND_QUIETLY)
else (APR_FOUND)
  if (Apr_FIND_REQUIRED)
    message(FATAL_ERROR "Could not find APR library")
  endif (Apr_FIND_REQUIRED)
endif (APR_FOUND)

# (Deprecated declarations.)
set(NATIVE_APR_INCLUDE_PATH ${APR_INCLUDE_DIR} )
get_filename_component(NATIVE_APR_LIB_PATH ${APR_LIB} PATH)

# Export libraries variables.
mark_as_advanced(
  APR_LIB
  APR_INCLUDE_DIR
  )

# CONFIGURE THE APR-UTIL SEARCH. Specify what we're looking for, and which
# directories we're going to look through to find them.
#############################################################################
set(POSSIBLE_APRUTIL_INCLUDE_DIRS
  ${POSSIBLE_APRUTIL_INCLUDE_DIRS}
  /opt/homebrew/opt/apr-util/include/apr-1
  /usr/local/include/apr-1
  /usr/local/include/apr-1.0
  /usr/include/apr-1
  /usr/include/apr-1.0
  /usr/local/apr/include/apr-1
  )

set(APRUTIL_LIB_NAMES ${APRUTIL_LIB_NAMES} aprutil-1)

set(POSSIBLE_APRURIL_LIB_DIRS
  ${POSSIBLE_APRURIL_LIB_DIRS}
  /usr/lib
  /usr/local/lib
  /usr/local/apr/lib
  /opt/homebrew/opt/apr-util/lib
  )

# SEARCH FOR APR-UTIL LIBRARIES.
################################
find_path(APRUTIL_INCLUDE_DIR apu.h ${POSSIBLE_APRUTIL_INCLUDE_DIRS})

find_library(
  APRUTIL_LIB
  NAMES ${APRUTIL_LIB_NAMES}
  PATHS ${POSSIBLE_APRUTIL_LIB_DIRS}
  )

# Did we find the include directory?
string(
  COMPARE NOTEQUAL
  "APRUTIL_INCLUDE_DIR-NOTFOUND"
  ${APRUTIL_INCLUDE_DIR} # Value set to APR_INCLUDE_DIR-NOTFOUND if not found.
  APRUTIL_INCLUDE_DIR_FOUND
  )

# Did we find the library?
string(
  COMPARE NOTEQUAL
  "APRUTIL_LIB-NOTFOUND"
  ${APRUTIL_LIB} # Value set to `APR_LIB-NOTFOUND` if not found.
  APRUTIL_LIB_FOUND
  )

# APR-UTIL is "found" if we've both an include directory and an APR-UTIL binary.
if ("${APRUTIL_LIB_FOUND}" AND "${APRUTIL_INCLUDE_DIR_FOUND}")
  set(APRUTIL_LIBS ${APRUTIL_LIB})
  set(APRUTIL_FOUND 1)
else ("${APRUTIL_LIB_FOUND}" AND "${APRUTIL_INCLUDE_DIR_FOUND}")
  set(APRUTIL_FOUND 0)
endif ("${APRUTIL_LIB_FOUND}" AND "${APRUTIL_INCLUDE_DIR_FOUND}")

# Results.
if (APRUTIL_FOUND)
  if (NOT Apr_FIND_QUIETLY)
    message(STATUS "Found APRUTIL headers: ${APRUTIL_INCLUDE_DIR}")
    message(STATUS "Found APRUTIL library: ${APRUTIL_LIBS}")
  endif (NOT Apr_FIND_QUIETLY)
else (APRUTIL_FOUND)
  if (Apr_FIND_REQUIRED)
    message(FATAL_ERROR "Could not find APRUTIL library")
  endif (Apr_FIND_REQUIRED)
endif (APRUTIL_FOUND)

# (Deprecated declarations.)
set(NATIVE_APRUTIL_INCLUDE_PATH ${APRUTIL_INCLUDE_DIR})
get_filename_component(NATIVE_APRUTIL_LIB_PATH ${APRUTIL_LIB} PATH)

# Export libraries variables.
mark_as_advanced(
  APRUTIL_LIB
  APRUTIL_INCLUDE_DIR
  )