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

function(ADD_TEST_MODULE MODULE SOURCES)
  if (MESOS_INSTALL_TESTS)
    add_library(${MODULE} SHARED ${SOURCES})
    install(
      TARGETS ${MODULE}
      RUNTIME DESTINATION ${MESOS_INSTALL_RUNTIME}
      LIBRARY DESTINATION ${MESOS_INSTALL_LIBRARIES})
  else()
    add_library(${MODULE} SHARED EXCLUDE_FROM_ALL ${SOURCES})
  endif()
endfunction()

function(ADD_TEST_EXECUTABLE EXECUTABLE SOURCES)
  add_executable(${EXECUTABLE} ${SOURCES})
  if (MESOS_INSTALL_TESTS)
    install(TARGETS ${EXECUTABLE} RUNTIME DESTINATION ${MESOS_INSTALL_RUNTIME})
  endif()
endfunction()

if (NOT WIN32)
  # Build the test modules.
  #########################
  # NOTE: Modules are not supported on Windows.
  ADD_TEST_MODULE(testallocator          test_allocator_module.cpp)
  ADD_TEST_MODULE(testanonymous          test_anonymous_module.cpp)
  ADD_TEST_MODULE(testauthentication     test_authentication_modules.cpp)
  ADD_TEST_MODULE(testauthorizer         test_authorizer_module.cpp)
  ADD_TEST_MODULE(testcontainer_logger   test_container_logger_module.cpp)
  ADD_TEST_MODULE(examplemodule          example_module_impl.cpp)
  ADD_TEST_MODULE(testhook               test_hook_module.cpp)
  ADD_TEST_MODULE(testhttpauthenticator  test_http_authenticator_module.cpp)
  ADD_TEST_MODULE(testisolator           test_isolator_module.cpp)
  ADD_TEST_MODULE(testmastercontender    test_master_contender_module.cpp)
  ADD_TEST_MODULE(testmasterdetector     test_master_detector_module.cpp)
  ADD_TEST_MODULE(testqos_controller     test_qos_controller_module.cpp)
  ADD_TEST_MODULE(testresource_estimator test_resource_estimator_module.cpp)

  # Build the example frameworks and executors.
  #############################################
  # TODO(josephw): The scheduler driver is current not built on Windows.
  ADD_TEST_EXECUTABLE(balloon-executor              balloon_executor.cpp)
  ADD_TEST_EXECUTABLE(balloon-framework             balloon_framework.cpp)
  ADD_TEST_EXECUTABLE(disk-full-framework           disk_full_framework.cpp)
  ADD_TEST_EXECUTABLE(docker-no-executor-framework  docker_no_executor_framework.cpp)
  ADD_TEST_EXECUTABLE(dynamic-reservation-framework dynamic_reservation_framework.cpp)
  ADD_TEST_EXECUTABLE(inverse-offer-framework       inverse_offer_framework.cpp)
  ADD_TEST_EXECUTABLE(load-generator-framework      load_generator_framework.cpp)
  ADD_TEST_EXECUTABLE(long-lived-executor           long_lived_executor.cpp)
  ADD_TEST_EXECUTABLE(long-lived-framework          long_lived_framework.cpp)
  ADD_TEST_EXECUTABLE(operation-feedback-framework  operation_feedback_framework.cpp)
  ADD_TEST_EXECUTABLE(no-executor-framework         no_executor_framework.cpp)
  ADD_TEST_EXECUTABLE(persistent-volume-framework   persistent_volume_framework.cpp)
  ADD_TEST_EXECUTABLE(test-executor                 test_executor.cpp)
  ADD_TEST_EXECUTABLE(test-framework                test_framework.cpp)
  ADD_TEST_EXECUTABLE(test-http-executor            test_http_executor.cpp)
  ADD_TEST_EXECUTABLE(test-http-framework           test_http_framework.cpp)
  ADD_TEST_EXECUTABLE(test-csi-user-framework       test_csi_user_framework.cpp)

  # TODO(chhsiao): The test CSI plugin is Linux only for now.
  if (LINUX)
    ADD_TEST_EXECUTABLE(test-csi-plugin test_csi_plugin.cpp)
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
  target_link_libraries(operation-feedback-framework  PRIVATE mesos)
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
