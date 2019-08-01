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
  adaptor_tests.cpp
  archiver_tests.cpp
  base64_tests.cpp
  bits_tests.cpp
  boundedhashmap_tests.cpp
  bytes_tests.cpp
  cache_tests.cpp
  cpp17_tests.cpp
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
  lambda_tests.cpp
  linkedhashmap_tests.cpp
  mac_tests.cpp
  main.cpp
  multimap_tests.cpp
  none_tests.cpp
  numify_tests.cpp
  option_tests.cpp
  os_tests.cpp
  path_tests.cpp
  protobuf_tests.cpp
  protobuf_tests.pb.cc
  protobuf_tests.pb.h
  protobuf_tests.proto
  recordio_tests.cpp
  result_tests.cpp
  some_tests.cpp
  strings_tests.cpp
  subcommand_tests.cpp
  try_tests.cpp
  uri_tests.cpp
  uuid_tests.cpp
  variant_tests.cpp
  version_tests.cpp)

if (NOT WIN32)
  list(APPEND STOUT_ROOT_TESTS_SRC
    svn_tests.cpp)
endif ()

set(STOUT_OS_TESTS_SRC
  os/copyfile_tests.cpp
  os/env_tests.cpp
  os/filesystem_tests.cpp
  os/process_tests.cpp
  os/rmdir_tests.cpp
  os/socket_tests.cpp
  os/strerror_tests.cpp
  os/sendfile_tests.cpp)

if (NOT WIN32)
  list(APPEND STOUT_OS_TESTS_SRC
    # NOTE: There is no intention to port the functions exercised in
    # the following tests to Windows.
    os/signals_tests.cpp
    os/systems_tests.cpp)
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  list(APPEND STOUT_ROOT_TESTS_SRC
    proc_tests.cpp)
endif ()

set(STOUT_TESTS_SRC
  ${STOUT_HEADERS}
  ${STOUT_ROOT_TESTS_SRC}
  ${STOUT_OS_TESTS_SRC})

# THE C++ PROTOCOL BUFFER CODE (protobuf_tests.pb.h and protobuf_tests.pb.cc).
##############################################################################
add_custom_command(
  OUTPUT protobuf_tests.pb.h protobuf_tests.pb.cc
  COMMAND protoc
    -I${CMAKE_CURRENT_SOURCE_DIR}
    --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/protobuf_tests.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protobuf_tests.proto)

add_executable(stout-tests EXCLUDE_FROM_ALL ${STOUT_TESTS_SRC})
target_link_libraries(stout-tests PRIVATE stout googletest)
target_include_directories(stout-tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# ADD TEST TARGET (runs when you do, e.g., `make check`).
#########################################################
add_test(NAME StoutTests COMMAND ${TEST_DRIVER} stout-tests)
