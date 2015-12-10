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
# **NOTE:** I (darroyo) have copied a *lot* of stuff in this
# file from code I found at in the FindAPR.cmake[1] project. The
# original version[2] of this file was also released under the
# Apache 2.0 license, and licensed to the ASF, making it fully
# compatible with the Mesos project.
#  [1] 3rdparty/libprocess/3rdparty/stout/cmake/FindApr.cmake
#  [2] https://github.com/akumuli/Akumuli/blob/master/cmake/FindAPR.cmake

# This module helps to find the cURL library.
#
# USAGE: to use this module, add the following line to your CMake project:
#   find_package(Curl)
#
# To make linking to cURL simple, this module defines:
#   * CURL_FOUND            (if false, linking build will fail)
#   * CURL_INCLUDE_DIR      (defineswhere to find apr.h, etc.)
#   * CURL_LIBS             (binaries for cURL and friends)
#
# Also defined, but not for general use are:
#   * CURL_LIB        (where to find the cURL library)

# CONFIGURE THE CURL SEARCH. Specify what we're looking for, and which
# directories we're going to look through to find them.
#############################################################################
set(POSSIBLE_CURL_INCLUDE_DIRS
  ${POSSIBLE_CURL_INCLUDE_DIRS}
  /usr/include/curl
  )

find_path(CURL_INCLUDE_DIR curl.h ${POSSIBLE_CURL_INCLUDE_DIRS})

# SEARCH FOR CURL LIBRARIES.
############################
set(POSSIBLE_CURL_LIB_DIRS
  ${POSSIBLE_CURL_LIB_DIRS}
  /usr/lib
  /usr/lib/x86_64-linux-gnu
  /usr/local/lib
  )

find_library(
  CURL_LIB
  NAMES curl
  PATHS ${POSSIBLE_CURL_LIB_DIRS}
  )

# Did we find the include directory?
string(
  COMPARE NOTEQUAL
  "CURL_INCLUDE_DIR-NOTFOUND"
  ${CURL_INCLUDE_DIR} # Value set to CURL_INCLUDE_DIR-NOTFOUND if not found.
  CURL_INCLUDE_DIR_FOUND
  )

# Did we find the library?
string(
  COMPARE NOTEQUAL
  "CURL_LIB-NOTFOUND"
  ${CURL_LIB} # Value set to `CURL_LIB-NOTFOUND` if not found.
  CURL_LIB_FOUND
  )

# cURL is considered "found" if we've both an include directory and a cURL
# binary.
if ("${CURL_LIB_FOUND}" AND "${CURL_INCLUDE_DIR_FOUND}")
  set(CURL_LIBS ${CURL_LIB})
  set(CURL_FOUND 1)
else ("${CURL_LIB_FOUND}" AND "${CURL_INCLUDE_DIR_FOUND}")
  set(CURL_FOUND 0)
endif ("${CURL_LIB_FOUND}" AND "${CURL_INCLUDE_DIR_FOUND}")

# Results.
if (CURL_FOUND)
  if (NOT Curl_FIND_QUIETLY)
    message(STATUS "Found cURL headers: ${CURL_INCLUDE_DIR}")
    message(STATUS "Found cURL library: ${CURL_LIBS}")
  endif (NOT Curl_FIND_QUIETLY)
else (CURL_FOUND)
  if (Curl_FIND_REQUIRED)
    message(FATAL_ERROR "Could not find cURL library")
  endif (Curl_FIND_REQUIRED)
endif (CURL_FOUND)

# (Deprecated declarations.)
set(NATIVE_CURL_INCLUDE_PATH ${CURL_INCLUDE_DIR})
get_filename_component(NATIVE_CURL_LIB_PATH ${CURL_LIB} PATH)
