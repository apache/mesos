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
if ("${LEVELDB_ROOT_DIR}" STREQUAL "")
  # NOTE: If this fails, stderr is ignored, and the output variable is empty.
  # This has no deleterious effect on our path search.
  execute_process(
    COMMAND brew --prefix leveldb
    OUTPUT_VARIABLE LEVELDB_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  set(POSSIBLE_LEVELDB_INCLUDE_DIRS "")
  set(POSSIBLE_LEVELDB_LIB_DIRS "")

  if (NOT "${LEVELDB_PREFIX}" STREQUAL "")
    list(APPEND POSSIBLE_LEVELDB_INCLUDE_DIRS ${LEVELDB_PREFIX}/include)
    list(APPEND POSSIBLE_LEVELDB_LIB_DIRS ${LEVELDB_PREFIX}/lib)
  endif()

  list(
    APPEND POSSIBLE_LEVELDB_INCLUDE_DIRS
    /usr/include
    /usr/local/include)

  list(
    APPEND POSSIBLE_LEVELDB_LIB_DIRS
    /usr/lib
    /usr/local/lib)
else()
  set(POSSIBLE_LEVELDB_INCLUDE_DIRS ${LEVELDB_ROOT_DIR}/include)
  set(POSSIBLE_LEVELDB_LIB_DIRS ${LEVELDB_ROOT_DIR}/lib)
endif()

set(LEVELDB_LIBRARY_NAMES leveldb)

FIND_PACKAGE_HELPER(LEVELDB leveldb/db.h)
