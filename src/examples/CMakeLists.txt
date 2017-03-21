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


# Example framework and executor sources.
#########################################
set(BALLOON_EXECUTOR_SRC              balloon_executor.cpp)
set(BALLOON_FRAMEWORK_SRC             balloon_framework.cpp)
set(DISK_FULL_FRAMEWORK_SRC           disk_full_framework.cpp)
set(DOCKER_NO_EXECUTOR_FRAMEWORK_SRC  docker_no_executor_framework.cpp)
set(DYNAMIC_RESERVATION_FRAMEWORK_SRC dynamic_reservation_framework.cpp)
set(LOAD_GENERATOR_FRAMEWORK_SRC      load_generator_framework.cpp)
set(LONG_LIVED_EXECUTOR_SRC           long_lived_executor.cpp)
set(LONG_LIVED_FRAMEWORK_SRC          long_lived_framework.cpp)
set(NO_EXECUTOR_FRAMEWORK_SRC         no_executor_framework.cpp)
set(PERSISTENT_VOLUME_FRAMEWORK_SRC   persistent_volume_framework.cpp)
set(TEST_EXECUTOR_SRC                 test_executor.cpp)
set(TEST_FRAMEWORK_SRC                test_framework.cpp)
set(TEST_HTTP_EXECUTOR_SRC            test_http_executor.cpp)
set(TEST_HTTP_FRAMEWORK_SRC           test_http_framework.cpp)


# Build the test modules.
#########################
# NOTE: Modules are not supported on Windows.
if (NOT WIN32)
  add_library(${TEST_ALLOCATOR}          SHARED EXCLUDE_FROM_ALL ${TEST_ALLOCATOR_SRC})
  add_library(${TEST_ANONYMOUS}          SHARED EXCLUDE_FROM_ALL ${TEST_ANONYMOUS_SRC})
  add_library(${TEST_AUTHENTICATION}     SHARED EXCLUDE_FROM_ALL ${TEST_AUTHENTICATION_SRC})
  add_library(${TEST_AUTHORIZER}         SHARED EXCLUDE_FROM_ALL ${TEST_AUTHORIZER_SRC})
  add_library(${TEST_CONTAINER_LOGGER}   SHARED EXCLUDE_FROM_ALL ${TEST_CONTAINER_LOGGER_SRC})
  add_library(${TEST_EXAMPLEMODULE}      SHARED EXCLUDE_FROM_ALL ${TEST_EXAMPLEMODULE_SRC})
  add_library(${TEST_HOOK}               SHARED EXCLUDE_FROM_ALL ${TEST_HOOK_SRC})
  add_library(${TEST_HTTPAUTHENTICATOR}  SHARED EXCLUDE_FROM_ALL ${TEST_HTTPAUTHENTICATOR_SRC})
  add_library(${TEST_ISOLATOR}           SHARED EXCLUDE_FROM_ALL ${TEST_ISOLATOR_SRC})
  add_library(${TEST_MASTER_CONTENDER}   SHARED EXCLUDE_FROM_ALL ${TEST_MASTER_CONTENDER_SRC})
  add_library(${TEST_MASTER_DETECTOR}    SHARED EXCLUDE_FROM_ALL ${TEST_MASTER_DETECTOR_SRC})
  add_library(${TEST_QOS_CONTROLLER}     SHARED EXCLUDE_FROM_ALL ${TEST_QOS_CONTROLLER_SRC})
  add_library(${TEST_RESOURCE_ESTIMATOR} SHARED EXCLUDE_FROM_ALL ${TEST_RESOURCE_ESTIMATOR_SRC})
endif (NOT WIN32)


# Build the example frameworks and executors.
#############################################
# TODO(josephw): The scheduler driver is current not built on Windows.
if (NOT WIN32)
  add_executable(${BALLOON_EXECUTOR}              ${BALLOON_EXECUTOR_SRC})
  add_executable(${BALLOON_FRAMEWORK}             ${BALLOON_FRAMEWORK_SRC})
  add_executable(${DISK_FULL_FRAMEWORK}           ${DISK_FULL_FRAMEWORK_SRC})
  add_executable(${DOCKER_NO_EXECUTOR_FRAMEWORK}  ${DOCKER_NO_EXECUTOR_FRAMEWORK_SRC})
  add_executable(${DYNAMIC_RESERVATION_FRAMEWORK} ${DYNAMIC_RESERVATION_FRAMEWORK_SRC})
  add_executable(${LOAD_GENERATOR_FRAMEWORK}      ${LOAD_GENERATOR_FRAMEWORK_SRC})
  add_executable(${LONG_LIVED_EXECUTOR}           ${LONG_LIVED_EXECUTOR_SRC})
  add_executable(${LONG_LIVED_FRAMEWORK}          ${LONG_LIVED_FRAMEWORK_SRC})
  add_executable(${NO_EXECUTOR_FRAMEWORK}         ${NO_EXECUTOR_FRAMEWORK_SRC})
  add_executable(${PERSISTENT_VOLUME_FRAMEWORK}   ${PERSISTENT_VOLUME_FRAMEWORK_SRC})
  add_executable(${TEST_EXECUTOR}                 ${TEST_EXECUTOR_SRC})
  add_executable(${TEST_FRAMEWORK}                ${TEST_FRAMEWORK_SRC})
  add_executable(${TEST_HTTP_EXECUTOR}            ${TEST_HTTP_EXECUTOR_SRC})
  add_executable(${TEST_HTTP_FRAMEWORK}           ${TEST_HTTP_FRAMEWORK_SRC})
endif (NOT WIN32)


# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
if (NOT WIN32)
  target_link_libraries(${TEST_ALLOCATOR}          ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_ANONYMOUS}          ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_AUTHENTICATION}     ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_AUTHORIZER}         ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_CONTAINER_LOGGER}   ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_EXAMPLEMODULE}      ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_HOOK}               ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_HTTPAUTHENTICATOR}  ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_ISOLATOR}           ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_MASTER_CONTENDER}   ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_MASTER_DETECTOR}    ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_QOS_CONTROLLER}     ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_RESOURCE_ESTIMATOR} ${MESOS_LIBS_TARGET})

  target_link_libraries(${BALLOON_EXECUTOR}              ${MESOS_LIBS_TARGET})
  target_link_libraries(${BALLOON_FRAMEWORK}             ${MESOS_LIBS_TARGET})
  target_link_libraries(${DISK_FULL_FRAMEWORK}           ${MESOS_LIBS_TARGET})
  target_link_libraries(${DOCKER_NO_EXECUTOR_FRAMEWORK}  ${MESOS_LIBS_TARGET})
  target_link_libraries(${DYNAMIC_RESERVATION_FRAMEWORK} ${MESOS_LIBS_TARGET})
  target_link_libraries(${LOAD_GENERATOR_FRAMEWORK}      ${MESOS_LIBS_TARGET})
  target_link_libraries(${LONG_LIVED_EXECUTOR}           ${MESOS_LIBS_TARGET})
  target_link_libraries(${LONG_LIVED_FRAMEWORK}          ${MESOS_LIBS_TARGET})
  target_link_libraries(${NO_EXECUTOR_FRAMEWORK}         ${MESOS_LIBS_TARGET})
  target_link_libraries(${PERSISTENT_VOLUME_FRAMEWORK}   ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_EXECUTOR}                 ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_FRAMEWORK}                ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_HTTP_EXECUTOR}            ${MESOS_LIBS_TARGET})
  target_link_libraries(${TEST_HTTP_FRAMEWORK}           ${MESOS_LIBS_TARGET})
endif (NOT WIN32)


# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
if (NOT WIN32)
  add_dependencies(${TEST_ALLOCATOR}          ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_ANONYMOUS}          ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_AUTHENTICATION}     ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_AUTHORIZER}         ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_CONTAINER_LOGGER}   ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_EXAMPLEMODULE}      ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_HOOK}               ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_HTTPAUTHENTICATOR}  ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_ISOLATOR}           ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_MASTER_CONTENDER}   ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_MASTER_DETECTOR}    ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_QOS_CONTROLLER}     ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_RESOURCE_ESTIMATOR} ${MESOS_LIBS_TARGET})

  add_dependencies(${BALLOON_EXECUTOR}              ${MESOS_LIBS_TARGET})
  add_dependencies(${BALLOON_FRAMEWORK}             ${MESOS_LIBS_TARGET})
  add_dependencies(${DISK_FULL_FRAMEWORK}           ${MESOS_LIBS_TARGET})
  add_dependencies(${DOCKER_NO_EXECUTOR_FRAMEWORK}  ${MESOS_LIBS_TARGET})
  add_dependencies(${DYNAMIC_RESERVATION_FRAMEWORK} ${MESOS_LIBS_TARGET})
  add_dependencies(${LOAD_GENERATOR_FRAMEWORK}      ${MESOS_LIBS_TARGET})
  add_dependencies(${LONG_LIVED_EXECUTOR}           ${MESOS_LIBS_TARGET})
  add_dependencies(${LONG_LIVED_FRAMEWORK}          ${MESOS_LIBS_TARGET})
  add_dependencies(${NO_EXECUTOR_FRAMEWORK}         ${MESOS_LIBS_TARGET})
  add_dependencies(${PERSISTENT_VOLUME_FRAMEWORK}   ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_EXECUTOR}                 ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_FRAMEWORK}                ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_HTTP_EXECUTOR}            ${MESOS_LIBS_TARGET})
  add_dependencies(${TEST_HTTP_FRAMEWORK}           ${MESOS_LIBS_TARGET})
endif (NOT WIN32)
