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

include(FindPackageHelper)

# TODO(tillt): Consider moving "_ROOT_DIR" logic into FindPackageHelper.
if ("${LIBEVENT_ROOT_DIR}" STREQUAL "")
  # NOTE: If this fails, stderr is ignored, and the output variable is empty.
  # This has no deleterious effect on our path search.
  execute_process(
    COMMAND brew --prefix libevent
    OUTPUT_VARIABLE LIBEVENT_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  set(POSSIBLE_LIBEVENT_INCLUDE_DIRS "")
  set(POSSIBLE_LIBEVENT_LIB_DIRS "")

  if (NOT "${LIBEVENT_PREFIX}" STREQUAL "")
    list(APPEND POSSIBLE_LIBEVENT_INCLUDE_DIRS ${LIBEVENT_PREFIX}/include)
    list(APPEND POSSIBLE_LIBEVENT_LIB_DIRS ${LIBEVENT_PREFIX}/lib)
  endif()

  list(
    APPEND POSSIBLE_LIBEVENT_INCLUDE_DIRS
    /usr/include/libevent
    /usr/local/include/libevent)

  list(
    APPEND POSSIBLE_LIBEVENT_LIB_DIRS
    /usr/lib
    /usr/local/lib)
else()
  set(POSSIBLE_LIBEVENT_INCLUDE_DIRS ${LIBEVENT_ROOT_DIR}/include)
  set(POSSIBLE_LIBEVENT_LIB_DIRS ${LIBEVENT_ROOT_DIR}/lib)
endif()

set(LIBEVENT_LIBRARY_NAMES event_core event_pthreads)

if (ENABLE_SSL)
  list(APPEND LIBEVENT_LIBRARY_NAMES event_openssl)
endif()

FIND_PACKAGE_HELPER(LIBEVENT event.h)
