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

# CONTAINERIZER TESTS.
######################
set(CONTAINERIZER_MEMORY_TESTS_SRC
  ${CONTAINERIZER_MEMORY_TESTS_SRC}
  ${MESOS_SRC_DIR}/tests/flags.cpp
  ${MESOS_SRC_DIR}/tests/utils.cpp
  memory_test_helper_main.cpp
  memory_test_helper.cpp
  )

# INCLUDE DIRECTIVES FOR CONTAINERIZER TEST BINARY (generates, e.g., -I/path/to/thing
# on Linux).
#############################################################################
include_directories(${CONTAINERIZER_TEST_INCLUDE_DIRS})

# LINKING LIBRARIES BY DIRECTORY (might generate, e.g., -L/path/to/thing on
# Linux).
###########################################################################
link_directories(${CONTAINERIZER_TEST_LIB_DIRS})

# THE CONTAINERIZER TEST EXECUTABLE (generates, e.g., stout_tests, etc., on Linux).
###########################################################################
add_executable(${CONTAINERIZER_MEMORY_TESTS_TARGET} ${CONTAINERIZER_MEMORY_TESTS_SRC})

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${CONTAINERIZER_MEMORY_TESTS_TARGET} ${CONTAINERIZER_TEST_LIBS})

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${CONTAINERIZER_MEMORY_TESTS_TARGET} ${CONTAINERIZER_TEST_DEPENDENCIES})

# ADD TEST TARGET (runs when you do, e.g., `make check`).
#########################################################
add_test(NAME MesosContainerizerMemoryTests COMMAND ./${CONTAINERIZER_MEMORY_TESTS_TARGET})
