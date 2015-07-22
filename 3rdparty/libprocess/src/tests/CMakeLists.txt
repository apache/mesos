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

set(PROCESS_TESTS_SRC
  ${PROCESS_TESTS_SRC}
  decoder_tests.cpp
  encoder_tests.cpp
  http_tests.cpp
  io_tests.cpp
  limiter_tests.cpp
  main.cpp
  mutex_tests.cpp
  metrics_tests.cpp
  owned_tests.cpp
  process_tests.cpp
  queue_tests.cpp
  reap_tests.cpp
  sequence_tests.cpp
  shared_tests.cpp
  statistics_tests.cpp
  subprocess_tests.cpp
  system_tests.cpp
  timeseries_tests.cpp
  time_tests.cpp
  )

include_directories(
  ../   # includes, e.g., decoder.hpp
  ${PROCESS_INCLUDE_DIRS}
  ${GMOCK_ROOT}/include
  ${GTEST_SRC}/include
  src
  )

link_directories(
  ${PROCESS_LIB_DIRS}
  ${GMOCK_ROOT}-build/lib/.libs
  ${GMOCK_ROOT}-build/gtest/lib/.libs
  )

add_executable(tests ${PROCESS_TESTS_SRC})
target_link_libraries(tests ${PROCESS_LIBS} gmock gtest)
add_dependencies(tests ${PROCESS_TEST_DEPENDENCIES})
add_test(NAME ProcessTests COMMAND ./tests)