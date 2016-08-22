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
if (LINUX)
  set(SETNS_TEST_HELPER_SRC
    ${SETNS_TEST_HELPER_SRC}
    setns_test_helper_main.cpp
    setns_test_helper.cpp
    )
endif (LINUX)

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
if (LINUX)
  add_executable(${SETNS_TEST_HELPER_TARGET} ${SETNS_TEST_HELPER_SRC})
endif (LINUX)

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
if (LINUX)
  target_link_libraries(${SETNS_TEST_HELPER_TARGET} ${CONTAINERIZER_TEST_LIBS})
endif (LINUX)

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
if (LINUX)
  add_dependencies(${SETNS_TEST_HELPER_TARGET} ${CONTAINERIZER_TEST_DEPENDENCIES})
endif (LINUX)

# ADD TEST TARGET (runs when you do, e.g., `make check`).
#########################################################
