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

# PROCESS TESTS.
################
set(PROCESS_TESTS_SRC
  main.cpp
  after_tests.cpp
  collect_tests.cpp
  count_down_latch_tests.cpp
  decoder_tests.cpp
  encoder_tests.cpp
  future_tests.cpp
  http_tests.cpp
  limiter_tests.cpp
  loop_tests.cpp
  metrics_tests.cpp
  mutex_tests.cpp
  owned_tests.cpp
  process_tests.cpp
  queue_tests.cpp
  sequence_tests.cpp
  shared_tests.cpp
  socket_tests.cpp
  statistics_tests.cpp
  subprocess_tests.cpp
  system_tests.cpp
  time_tests.cpp
  timeseries_tests.cpp)

if (NOT WIN32)
  list(APPEND PROCESS_TESTS_SRC
    io_tests.cpp
    reap_tests.cpp)
endif ()

if (ENABLE_SSL)
  list(APPEND PROCESS_TESTS_SRC
    jwt_tests.cpp
    ssl_tests.cpp)
endif ()

add_library(process-interface INTERFACE)
target_link_libraries(process-interface INTERFACE process googletest)
target_include_directories(process-interface INTERFACE ..)

add_executable(libprocess-tests EXCLUDE_FROM_ALL ${PROCESS_TESTS_SRC})
target_link_libraries(libprocess-tests PRIVATE process-interface)

target_compile_definitions(
  libprocess-tests PRIVATE
  BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}")

add_executable(test-linkee EXCLUDE_FROM_ALL test_linkee.cpp)
target_link_libraries(test-linkee PRIVATE process-interface)
add_dependencies(libprocess-tests test-linkee)

if (ENABLE_SSL)
  add_executable(ssl-client EXCLUDE_FROM_ALL ssl_client.cpp)
  target_link_libraries(ssl-client PRIVATE process-interface)
  add_dependencies(libprocess-tests ssl-client)
endif ()

add_test(NAME ProcessTests COMMAND libprocess-tests)
