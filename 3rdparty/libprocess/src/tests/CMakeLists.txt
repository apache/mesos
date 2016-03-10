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
  ${PROCESS_TESTS_SRC}
  collect_tests.cpp
  decoder_tests.cpp
  encoder_tests.cpp
  future_tests.cpp
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

# INCLUDE DIRECTIVES FOR PROCESS TEST BINARY (generates, e.g., -I/path/to/thing
# on Linux).
###############################################################################
include_directories(${PROCESS_TEST_INCLUDE_DIRS})

# LINKING LIBRARIES BY DIRECTORY (might generate, e.g., -L/path/to/thing on
# Linux).
###########################################################################
link_directories(${PROCESS_TEST_LIB_DIRS})

# THE PROCESS TEST EXECUTABLE (generates, e.g., process_tests, etc., on Linux).
###############################################################################
add_executable(${PROCESS_TESTS_TARGET} ${PROCESS_TESTS_SRC})

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${PROCESS_TESTS_TARGET} ${PROCESS_TEST_LIBS})

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${PROCESS_TESTS_TARGET} ${PROCESS_TEST_DEPENDENCIES})

# ADD TEST TARGET (runs when you do, e.g., `make check`).
#########################################################
add_test(NAME ProcessTests COMMAND ./${PROCESS_TESTS_TARGET})
