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

# Test module sources.
######################
set(TEST_ALLOCATOR_SRC          test_allocator_module.cpp)
set(TEST_ANONYMOUS_SRC          test_anonymous_module.cpp)
set(TEST_AUTHENTICATION_SRC     test_authentication_modules.cpp)
set(TEST_AUTHORIZER_SRC         test_authorizer_module.cpp)
set(TEST_CONTAINER_LOGGER_SRC   test_container_logger_module.cpp)
set(TEST_EXAMPLEMODULE_SRC      example_module_impl.cpp)
set(TEST_HOOK_SRC               test_hook_module.cpp)
set(TEST_HTTPAUTHENTICATOR_SRC  test_http_authenticator_module.cpp)
set(TEST_ISOLATOR_SRC           test_isolator_module.cpp)
set(TEST_MASTER_CONTENDER_SRC   test_master_contender_module.cpp)
set(TEST_MASTER_DETECTOR_SRC    test_master_detector_module.cpp)
set(TEST_QOS_CONTROLLER_SRC     test_qos_controller_module.cpp)
set(TEST_RESOURCE_ESTIMATOR_SRC test_resource_estimator_module.cpp)


# Build the test modules.
#########################
# NOTE: Modules are not supported on Windows.
if (NOT WIN32)
  add_library(${TEST_ALLOCATOR}          SHARED ${TEST_ALLOCATOR_SRC})
  add_library(${TEST_ANONYMOUS}          SHARED ${TEST_ANONYMOUS_SRC})
  add_library(${TEST_AUTHENTICATION}     SHARED ${TEST_AUTHENTICATION_SRC})
  add_library(${TEST_AUTHORIZER}         SHARED ${TEST_AUTHORIZER_SRC})
  add_library(${TEST_CONTAINER_LOGGER}   SHARED ${TEST_CONTAINER_LOGGER_SRC})
  add_library(${TEST_EXAMPLEMODULE}      SHARED ${TEST_EXAMPLEMODULE_SRC})
  add_library(${TEST_HOOK}               SHARED ${TEST_HOOK_SRC})
  add_library(${TEST_HTTPAUTHENTICATOR}  SHARED ${TEST_HTTPAUTHENTICATOR_SRC})
  add_library(${TEST_ISOLATOR}           SHARED ${TEST_ISOLATOR_SRC})
  add_library(${TEST_MASTER_CONTENDER}   SHARED ${TEST_MASTER_CONTENDER_SRC})
  add_library(${TEST_MASTER_DETECTOR}    SHARED ${TEST_MASTER_DETECTOR_SRC})
  add_library(${TEST_QOS_CONTROLLER}     SHARED ${TEST_QOS_CONTROLLER_SRC})
  add_library(${TEST_RESOURCE_ESTIMATOR} SHARED ${TEST_RESOURCE_ESTIMATOR_SRC})
endif (NOT WIN32)


# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
if (NOT WIN32)
  target_link_libraries(${TEST_ALLOCATOR}          ${MESOS_TARGET})
  target_link_libraries(${TEST_ANONYMOUS}          ${MESOS_TARGET})
  target_link_libraries(${TEST_AUTHENTICATION}     ${MESOS_TARGET})
  target_link_libraries(${TEST_AUTHORIZER}         ${MESOS_TARGET})
  target_link_libraries(${TEST_CONTAINER_LOGGER}   ${MESOS_TARGET})
  target_link_libraries(${TEST_EXAMPLEMODULE}      ${MESOS_TARGET})
  target_link_libraries(${TEST_HOOK}               ${MESOS_TARGET})
  target_link_libraries(${TEST_HTTPAUTHENTICATOR}  ${MESOS_TARGET})
  target_link_libraries(${TEST_ISOLATOR}           ${MESOS_TARGET})
  target_link_libraries(${TEST_MASTER_CONTENDER}   ${MESOS_TARGET})
  target_link_libraries(${TEST_MASTER_DETECTOR}    ${MESOS_TARGET})
  target_link_libraries(${TEST_QOS_CONTROLLER}     ${MESOS_TARGET})
  target_link_libraries(${TEST_RESOURCE_ESTIMATOR} ${MESOS_TARGET})
endif (NOT WIN32)


# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
if (NOT WIN32)
  add_dependencies(${TEST_ALLOCATOR}          ${MESOS_TARGET})
  add_dependencies(${TEST_ANONYMOUS}          ${MESOS_TARGET})
  add_dependencies(${TEST_AUTHENTICATION}     ${MESOS_TARGET})
  add_dependencies(${TEST_AUTHORIZER}         ${MESOS_TARGET})
  add_dependencies(${TEST_CONTAINER_LOGGER}   ${MESOS_TARGET})
  add_dependencies(${TEST_EXAMPLEMODULE}      ${MESOS_TARGET})
  add_dependencies(${TEST_HOOK}               ${MESOS_TARGET})
  add_dependencies(${TEST_HTTPAUTHENTICATOR}  ${MESOS_TARGET})
  add_dependencies(${TEST_ISOLATOR}           ${MESOS_TARGET})
  add_dependencies(${TEST_MASTER_CONTENDER}   ${MESOS_TARGET})
  add_dependencies(${TEST_MASTER_DETECTOR}    ${MESOS_TARGET})
  add_dependencies(${TEST_QOS_CONTROLLER}     ${MESOS_TARGET})
  add_dependencies(${TEST_RESOURCE_ESTIMATOR} ${MESOS_TARGET})
endif (NOT WIN32)
