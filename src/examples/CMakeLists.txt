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

if (NOT WIN32)
  # Build the test modules.
  #########################
  # NOTE: Modules are not supported on Windows.
  add_library(testallocator          SHARED EXCLUDE_FROM_ALL test_allocator_module.cpp)
  add_library(testanonymous          SHARED EXCLUDE_FROM_ALL test_anonymous_module.cpp)
  add_library(testauthentication     SHARED EXCLUDE_FROM_ALL test_authentication_modules.cpp)
  add_library(testauthorizer         SHARED EXCLUDE_FROM_ALL test_authorizer_module.cpp)
  add_library(testcontainer_logger   SHARED EXCLUDE_FROM_ALL test_container_logger_module.cpp)
  add_library(examplemodule          SHARED EXCLUDE_FROM_ALL example_module_impl.cpp)
  add_library(testhook               SHARED EXCLUDE_FROM_ALL test_hook_module.cpp)
  add_library(testhttpauthenticator  SHARED EXCLUDE_FROM_ALL test_http_authenticator_module.cpp)
  add_library(testisolator           SHARED EXCLUDE_FROM_ALL test_isolator_module.cpp)
  add_library(testmastercontender    SHARED EXCLUDE_FROM_ALL test_master_contender_module.cpp)
  add_library(testmasterdetector     SHARED EXCLUDE_FROM_ALL test_master_detector_module.cpp)
  add_library(testqos_controller     SHARED EXCLUDE_FROM_ALL test_qos_controller_module.cpp)
  add_library(testresource_estimator SHARED EXCLUDE_FROM_ALL test_resource_estimator_module.cpp)


  # Build the example frameworks and executors.
  #############################################
  # TODO(josephw): The scheduler driver is current not built on Windows.
  add_executable(balloon-executor              balloon_executor.cpp)
  add_executable(balloon-framework             balloon_framework.cpp)
  add_executable(disk-full-framework           disk_full_framework.cpp)
  add_executable(docker-no-executor-framework  docker_no_executor_framework.cpp)
  add_executable(dynamic-reservation-framework dynamic_reservation_framework.cpp)
  add_executable(inverse-offer-framework       inverse_offer_framework.cpp)
  add_executable(load-generator-framework      load_generator_framework.cpp)
  add_executable(long-lived-executor           long_lived_executor.cpp)
  add_executable(long-lived-framework          long_lived_framework.cpp)
  add_executable(no-executor-framework         no_executor_framework.cpp)
  add_executable(persistent-volume-framework   persistent_volume_framework.cpp)
  add_executable(test-executor                 test_executor.cpp)
  add_executable(test-framework                test_framework.cpp)
  add_executable(test-http-executor            test_http_executor.cpp)
  add_executable(test-http-framework           test_http_framework.cpp)
  add_executable(test-csi-user-framework       test_csi_user_framework.cpp)

  # TODO(chhsiao): The test CSI plugin is Linux only for now.
  if (LINUX)
    add_executable(test-csi-plugin test_csi_plugin.cpp)
  endif ()

  # NOTE: Do not replace this with `link_libraries()`. While it may result in
  # less code, it is deprecated and relies on side effects instead of
  # explicitness.
  target_link_libraries(testallocator          PRIVATE mesos)
  target_link_libraries(testanonymous          PRIVATE mesos)
  target_link_libraries(testauthentication     PRIVATE mesos)
  target_link_libraries(testauthorizer         PRIVATE mesos)
  target_link_libraries(testcontainer_logger   PRIVATE mesos)
  target_link_libraries(examplemodule          PRIVATE mesos)
  target_link_libraries(testhook               PRIVATE mesos)
  target_link_libraries(testhttpauthenticator  PRIVATE mesos)
  target_link_libraries(testisolator           PRIVATE mesos)
  target_link_libraries(testmastercontender    PRIVATE mesos)
  target_link_libraries(testmasterdetector     PRIVATE mesos)
  target_link_libraries(testqos_controller     PRIVATE mesos)
  target_link_libraries(testresource_estimator PRIVATE mesos)

  target_link_libraries(balloon-executor              PRIVATE mesos)
  target_link_libraries(balloon-framework             PRIVATE mesos)
  target_link_libraries(disk-full-framework           PRIVATE mesos)
  target_link_libraries(docker-no-executor-framework  PRIVATE mesos)
  target_link_libraries(dynamic-reservation-framework PRIVATE mesos)
  target_link_libraries(inverse-offer-framework       PRIVATE mesos)
  target_link_libraries(load-generator-framework      PRIVATE mesos)
  target_link_libraries(long-lived-executor           PRIVATE mesos)
  target_link_libraries(long-lived-framework          PRIVATE mesos)
  target_link_libraries(no-executor-framework         PRIVATE mesos)
  target_link_libraries(persistent-volume-framework   PRIVATE mesos)
  target_link_libraries(test-executor                 PRIVATE mesos)
  target_link_libraries(test-framework                PRIVATE mesos)
  target_link_libraries(test-http-executor            PRIVATE mesos)
  target_link_libraries(test-http-framework           PRIVATE mesos)
  target_link_libraries(test-csi-user-framework       PRIVATE mesos)

  # TODO(chhsiao): The test CSI plugin is Linux only for now.
  if (LINUX)
    # NOTE: We explicitly add `grpc` here since the test CSI plugin uses the
    # server-side gRPC library, which might not be linked in mesos.
    target_link_libraries(test-csi-plugin PRIVATE mesos grpc)
  endif ()
endif ()
