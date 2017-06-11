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

# STOUT TESTS.
##############
set(STOUT_ROOT_TESTS_SRC
  ${STOUT_ROOT_TESTS_SRC}
  base64_tests.cpp
  bits_tests.cpp
  bytes_tests.cpp
  cache_tests.cpp
  duration_tests.cpp
  dynamiclibrary_tests.cpp
  error_tests.cpp
  flags_tests.cpp
  gzip_tests.cpp
  hashmap_tests.cpp
  hashset_tests.cpp
  interval_tests.cpp
  ip_tests.cpp
  json_tests.cpp
  jsonify_tests.cpp
  linkedhashmap_tests.cpp
  mac_tests.cpp
  main.cpp
  multimap_tests.cpp
  none_tests.cpp
  numify_tests.cpp
  option_tests.cpp
  os_tests.cpp
  protobuf_tests.pb.h
  protobuf_tests.proto
  recordio_tests.cpp
  result_tests.cpp
  some_tests.cpp
  strings_tests.cpp
  subcommand_tests.cpp
  try_tests.cpp
  uuid_tests.cpp
  variant_tests.cpp
  version_tests.cpp
  )

if (NOT WIN32)
  set(STOUT_ROOT_TESTS_SRC
    ${STOUT_ROOT_TESTS_SRC}
    path_tests.cpp
    protobuf_tests.cpp
    protobuf_tests.pb.cc
    svn_tests.cpp
    )
endif (NOT WIN32)

set(STOUT_OS_TESTS_SRC
  ${STOUT_OS_TESTS_SRC}
  os/env_tests.cpp
  os/filesystem_tests.cpp
  os/process_tests.cpp
  os/rmdir_tests.cpp
  os/socket_tests.cpp
  os/strerror_tests.cpp
  os/systems_tests.cpp
  )

if (NOT WIN32)
  set(STOUT_OS_TESTS_SRC
    ${STOUT_OS_TESTS_SRC}
    os/sendfile_tests.cpp
    os/signals_tests.cpp
    )
endif (NOT WIN32)

if (LINUX)
  set(STOUT_ROOT_TESTS_SRC ${STOUT_ROOT_TESTS_SRC} proc_tests.cpp)
endif (LINUX)

set(STOUT_TESTS_SRC
  ${STOUT_TESTS_SRC}
  ${STOUT_HEADERS}
  ${STOUT_ROOT_TESTS_SRC}
  ${STOUT_OS_TESTS_SRC}
  )

# INCLUDE DIRECTIVES FOR STOUT TEST BINARY (generates, e.g., -I/path/to/thing
# on Linux).
#############################################################################
include_directories(SYSTEM ${STOUT_TEST_3RDPARTY_INCLUDE_DIRS})
include_directories(${STOUT_INCLUDE_DIRS})

# LINKING LIBRARIES BY DIRECTORY (might generate, e.g., -L/path/to/thing on
# Linux).
###########################################################################
link_directories(${STOUT_TEST_LIB_DIRS})

# THE STOUT TEST EXECUTABLE (generates, e.g., stout-tests, etc., on Linux).
###########################################################################
add_executable(${STOUT_TESTS_TARGET} EXCLUDE_FROM_ALL ${STOUT_TESTS_SRC})

GROUP_STOUT_HEADERS()

GROUP_SOURCE(
  "tests"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "*.[ch]*")

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${STOUT_TESTS_TARGET} ${STOUT_TEST_LIBS})

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${STOUT_TESTS_TARGET} ${STOUT_TEST_DEPENDENCIES})

# ADD TEST TARGET (runs when you do, e.g., `make check`).
#########################################################
add_test(NAME StoutTests COMMAND ${STOUT_TESTS_TARGET})
