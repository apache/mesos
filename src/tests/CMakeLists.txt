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

# Sources for a test helper binary.
###################################
set(TEST_HELPER_SRC
  active_user_test_helper.cpp
  flags.cpp
  http_server_test_helper.cpp
  resources_utils.cpp
  test_helper_main.cpp
  utils.cpp)

if (NOT WIN32)
  list(APPEND TEST_HELPER_SRC
    kill_policy_test_helper.cpp
    containerizer/memory_test_helper.cpp)
endif ()

if (LINUX)
  list(APPEND TEST_HELPER_SRC
    containerizer/capabilities_test_helper.cpp
    containerizer/setns_test_helper.cpp)
endif ()


# Test utilities.
#################
set(MESOS_TESTS_UTILS_SRC
  active_user_test_helper.cpp
  allocator.cpp
  cluster.cpp
  containerizer.cpp
  environment.cpp
  flags.cpp
  http_server_test_helper.cpp
  main.cpp
  master/mock_master_api_subscriber.cpp
  mesos.cpp
  mock_docker.cpp
  mock_fetcher.cpp
  mock_registrar.cpp
  mock_slave.cpp
  module.cpp
  resources_utils.cpp
  utils.cpp
  containerizer/launcher.cpp)

if (NOT WIN32)
  list(APPEND MESOS_TESTS_UTILS_SRC
    kill_policy_test_helper.cpp
    mock_csi_plugin.cpp
    script.cpp
    containerizer/memory_test_helper.cpp)
endif ()

if (LINUX)
  list(APPEND MESOS_TESTS_UTILS_SRC
    containerizer/capabilities_test_helper.cpp
    containerizer/setns_test_helper.cpp)
endif ()


# All the test sources.
#######################
set(MESOS_TESTS_SRC
  ${MESOS_TESTS_UTILS_SRC}
  agent_container_api_tests.cpp
  agent_operation_feedback_tests.cpp
  anonymous_tests.cpp
  api_tests.cpp
  attributes_tests.cpp
  authentication_tests.cpp
  authorization_tests.cpp
  check_tests.cpp
  command_executor_tests.cpp
  common_validation_tests.cpp
  consumption_metrics_tests.cpp
  container_daemon_tests.cpp
  cram_md5_authentication_tests.cpp
  credentials_tests.cpp
  default_executor_tests.cpp
  exception_tests.cpp
  executor_http_api_tests.cpp
  fault_tolerance_tests.cpp
  fetcher_tests.cpp
  files_tests.cpp
  gc_tests.cpp
  hdfs_tests.cpp
  health_check_tests.cpp
  hierarchical_allocator_benchmarks.cpp
  hierarchical_allocator_tests.cpp
  hook_tests.cpp
  http_authentication_tests.cpp
  http_fault_tolerance_tests.cpp
  logging_tests.cpp
  master_draining_tests.cpp
  master_load_tests.cpp
  master_maintenance_tests.cpp
  master_slave_reconciliation_tests.cpp
  master/offer_constraints_filter_tests.cpp
  master/update_framework_tests.cpp
  metrics_tests.cpp
  operation_reconciliation_tests.cpp
  operation_status_update_manager_tests.cpp
  partition_tests.cpp
  paths_tests.cpp
  protobuf_io_tests.cpp
  rate_limiting_tests.cpp
  resource_offers_tests.cpp
  resource_provider_manager_tests.cpp
  resource_provider_validation_tests.cpp
  resource_quantities_tests.cpp
  resources_tests.cpp
  role_tests.cpp
  scheduler_driver_tests.cpp
  scheduler_event_call_tests.cpp
  scheduler_http_api_tests.cpp
  scheduler_tests.cpp
  slave_authorization_tests.cpp
  slave_compatibility_tests.cpp
  slave_tests.cpp
  slave_recovery_tests.cpp
  slave_validation_tests.cpp
  sorter_tests.cpp
  task_status_update_manager_tests.cpp
  uri_tests.cpp
  uri_fetcher_tests.cpp
  values_tests.cpp
  zookeeper_url_tests.cpp)

if (HAS_JAVA)
  list(APPEND MESOS_TESTS_SRC
    group_tests.cpp
    registrar_zookeeper_tests.cpp
    zookeeper.cpp
    zookeeper_test_server.cpp
    zookeeper_tests.cpp)
endif ()

if (ENABLE_SSL)
  list(APPEND MESOS_TESTS_SRC
    secret_generator_tests.cpp)
endif ()

list(APPEND MESOS_TESTS_SRC
  common/future_tracker_tests.cpp
  common/http_tests.cpp
  common/http_tests.pb.cc
  common/recordio_tests.cpp
  common/type_utils_tests.cpp)

list(APPEND MESOS_TESTS_SRC
  containerizer/containerizer_tests.cpp
  containerizer/cpu_isolator_tests.cpp
  containerizer/docker_containerizer_tests.cpp
  containerizer/docker_tests.cpp
  containerizer/memory_isolator_tests.cpp)

if (NOT WIN32)
  list(APPEND MESOS_TESTS_SRC
    container_logger_tests.cpp
    csi_client_tests.cpp
    csi_utils_tests.cpp
    disk_profile_adaptor_tests.cpp
    disk_quota_tests.cpp
    dynamic_weights_tests.cpp
    examples_tests.cpp
    fetcher_cache_tests.cpp
    log_tests.cpp
    master_allocator_tests.cpp
    master_authorization_tests.cpp
    master_benchmarks.cpp
    master_contender_detector_tests.cpp
    master_quota_tests.cpp
    master_tests.cpp
    master_validation_tests.cpp
    module_tests.cpp
    oversubscription_tests.cpp
    persistent_volume_endpoints_tests.cpp
    persistent_volume_tests.cpp
    protobuf_utils_tests.cpp
    reconciliation_tests.cpp
    registrar_tests.cpp
    reservation_endpoints_tests.cpp
    reservation_tests.cpp
    state_tests.cpp
    teardown_tests.cpp
    upgrade_tests.cpp
    volume_gid_manager_tests.cpp)

  list(APPEND MESOS_TESTS_SRC
    common/command_utils_tests.cpp)

  list(APPEND MESOS_TESTS_SRC
    containerizer/appc_spec_tests.cpp
    containerizer/composing_containerizer_tests.cpp
    containerizer/docker_spec_tests.cpp
    containerizer/environment_secret_isolator_tests.cpp
    containerizer/io_switchboard_tests.cpp
    containerizer/isolator_tests.cpp
    containerizer/mesos_containerizer_paths_tests.cpp
    containerizer/mesos_containerizer_tests.cpp
    containerizer/oci_spec_tests.cpp
    containerizer/posix_rlimits_isolator_tests.cpp
    containerizer/provisioner_appc_tests.cpp
    containerizer/provisioner_backend_tests.cpp
    containerizer/provisioner_docker_tests.cpp
    containerizer/provisioner_paths_tests.cpp
    containerizer/volume_sandbox_path_isolator_tests.cpp)
endif ()

if (LINUX)
  list(APPEND MESOS_TESTS_SRC
    agent_resource_provider_config_api_tests.cpp
    ldcache_tests.cpp
    ldd_tests.cpp
    storage_local_resource_provider_tests.cpp
    containerizer/capabilities_tests.cpp
    containerizer/cgroups_isolator_tests.cpp
    containerizer/cgroups_tests.cpp
    containerizer/cni_isolator_tests.cpp
    containerizer/docker_volume_isolator_tests.cpp
    containerizer/fs_tests.cpp
    containerizer/linux_capabilities_isolator_tests.cpp
    containerizer/linux_nnp_isolator_tests.cpp
    containerizer/linux_devices_isolator_tests.cpp
    containerizer/linux_filesystem_isolator_tests.cpp
    containerizer/memory_pressure_tests.cpp
    containerizer/nested_mesos_containerizer_tests.cpp
    containerizer/ns_tests.cpp
    containerizer/nvidia_gpu_isolator_tests.cpp
    containerizer/perf_tests.cpp
    containerizer/rootfs.cpp
    containerizer/runtime_isolator_tests.cpp
    containerizer/sched_tests.cpp
    containerizer/volume_csi_isolator_tests.cpp
    containerizer/volume_host_path_isolator_tests.cpp
    containerizer/volume_image_isolator_tests.cpp
    containerizer/volume_secret_isolator_tests.cpp)

  if (ENABLE_XFS_DISK_ISOLATOR)
    list(APPEND MESOS_TESTS_SRC
      containerizer/xfs_quota_tests.cpp)
  endif()

  if (ENABLE_SECCOMP_ISOLATOR)
    list(APPEND MESOS_TESTS_SRC
      containerizer/linux_seccomp_parser_tests.cpp
      containerizer/linux_seccomp_isolator_tests.cpp)
  endif()

  if (ENABLE_LAUNCHER_SEALING)
    list(APPEND MESOS_TESTS_SRC
      containerizer/linux_memfd_tests.cpp)
  endif()

  if (ENABLE_LINUX_ROUTING)
    list(APPEND MESOS_TESTS_SRC
      containerizer/routing_tests.cpp)
  endif ()

  if (ENABLE_NETWORK_PORTS_ISOLATOR)
    list(APPEND MESOS_TESTS_SRC
      containerizer/ports_isolator_tests.cpp)
  endif()

  if (ENABLE_PORT_MAPPING_ISOLATOR)
    list(APPEND MESOS_TESTS_SRC
      containerizer/port_mapping_tests.cpp)
  endif()
endif ()


# THE TEST AND HELPER EXECUTABLES.
##################################
add_library(mesos-tests-interface INTERFACE)
target_link_libraries(
  mesos-tests-interface INTERFACE
  mesos
  googletest)

if (NOT WIN32)
  target_link_libraries(
    mesos-tests-interface INTERFACE
    load_qos_controller
    fixed_resource_estimator
    logrotate_container_logger
    uri_disk_profile_adaptor)
endif ()

target_compile_definitions(
  mesos-tests-interface INTERFACE
  SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  BUILD_DIR="${CMAKE_BINARY_DIR}"
  PKGLIBEXECDIR="${PKG_LIBEXEC_INSTALL_DIR}"
  TESTLIBEXECDIR="${TEST_LIB_EXEC_DIR}"
  PKGMODULEDIR="${PKG_MODULE_DIR}"
  SBINDIR="${S_BIN_DIR}"
  MESOS_INSTALL_TESTS=${MESOS_INSTALL_TESTS})

if(MESOS_INSTALL_TESTS)
  add_executable(mesos-tests ${MESOS_TESTS_SRC})
else()
  add_executable(mesos-tests EXCLUDE_FROM_ALL ${MESOS_TESTS_SRC})
endif()

target_link_libraries(mesos-tests PRIVATE mesos-tests-interface)

target_compile_definitions(
  mesos-tests PRIVATE
  $<$<BOOL:${HAS_JAVA}>:MESOS_HAS_JAVA>)

# Helper to run tests that require a subprocess.
if (MESOS_INSTALL_TESTS)
  add_executable(test-helper ${TEST_HELPER_SRC})
  install(TARGETS test-helper RUNTIME DESTINATION ${MESOS_INSTALL_RUNTIME})
else()
  add_executable(test-helper EXCLUDE_FROM_ALL ${TEST_HELPER_SRC})
endif()

target_link_libraries(test-helper PRIVATE mesos-tests-interface)

# The tests require these binaries.
add_dependencies(mesos-tests test-helper mesos-agent)

if (NOT WIN32)
  # The tests require these binaries.
  # NOTE: These binaries do not yet build on Windows.
  add_dependencies(
    mesos-tests
    mesos-execute
    mesos-local
    mesos-log
    mesos-logrotate-logger
    mesos-master
    mesos-resolve)

  # The tests require all the test modules. These are not directly linked but
  # instead loaded at runtime, hence the manual dependency here.
  # NOTE: Modules are not supported on Windows.
  add_dependencies(
    mesos-tests
    testallocator
    testanonymous
    testauthentication
    testauthorizer
    testcontainer_logger
    examplemodule
    testhook
    testhttpauthenticator
    testisolator
    testmastercontender
    testmasterdetector
    testqos_controller
    testresource_estimator)

  # The tests require all the example frameworks and executors.
  # TODO(josephw): The scheduler driver is currently not built on Windows.
  add_dependencies(
    mesos-tests
    balloon-executor
    balloon-framework
    disk-full-framework
    docker-no-executor-framework
    dynamic-reservation-framework
    inverse-offer-framework
    load-generator-framework
    long-lived-executor
    long-lived-framework
    operation-feedback-framework
    no-executor-framework
    persistent-volume-framework
    test-executor
    test-framework
    test-csi-user-framework
    test-http-executor
    test-http-framework)

  # SLRP-related tests are Linux only and require the test CSI plugin binary.
  if (LINUX)
    add_dependencies(
      mesos-tests
      test-csi-plugin)
  endif ()

  if (NOT ENABLE_NEW_CLI)
    add_dependencies(mesos-tests
      mesos-cli)
  else ()
    add_dependencies(mesos-tests
      cli)
  endif ()
endif ()

if (MESOS_INSTALL_TESTS)
  install(TARGETS mesos-tests RUNTIME DESTINATION ${MESOS_INSTALL_RUNTIME})
endif()

# ADD TEST TARGET (runs when you do, e.g., `make check`).
#########################################################
add_test(NAME MesosTests COMMAND ${TEST_DRIVER} "${CMAKE_BINARY_DIR}/src/mesos-tests")
set_property(TEST MesosTests PROPERTY ENVIRONMENT
  "MESOS_GTEST_RUNNER_FLAGS=--sequential=*ROOT_* $ENV{MESOS_GTEST_RUNNER_FLAGS}")
