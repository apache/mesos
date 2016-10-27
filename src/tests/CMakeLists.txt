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

include(MesosTestsConfigure)

######################
set(TEST_HELPER_SRC
  ${TEST_HELPER_SRC}
  active_user_test_helper.cpp
  flags.cpp
  health_check_test_helper.cpp
  resources_utils.cpp
  test_helper_main.cpp
  utils.cpp
  containerizer/memory_test_helper.cpp
  )

if (LINUX)
  set(TEST_HELPER_SRC
    ${TEST_HELPER_SRC}
    containerizer/capabilities_test_helper.cpp
    containerizer/setns_test_helper.cpp
    )
endif (LINUX)

# TODO(hausdorff): Add the other tests here.

# INCLUDE DIRECTIVES (generates, e.g., -I/path/to/thing on Linux).
##################################################################
include_directories(${TEST_HELPER_INCLUDE_DIRS})

# LINKING LIBRARIES (might generate, e.g., -L/path/to/thing on Linux).
######################################################################
link_directories(${TEST_HELPER_LIB_DIRS})

# THE TEST AND HELPER EXECUTABLEs (generates, e.g., stout_tests, etc., on Linux).
#################################$###############################################
add_executable(${TEST_HELPER_TARGET} ${TEST_HELPER_SRC})

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${TEST_HELPER_TARGET} ${TEST_HELPER_LIBS})

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${TEST_HELPER_TARGET} ${TEST_HELPER_DEPENDENCIES})
