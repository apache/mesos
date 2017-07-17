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

include(StoutTestsConfigure)
include(MesosTestsConfigure)

# Sources for a test helper binary.
###################################
set(TEST_HELPER_SRC
  ${TEST_HELPER_SRC}
  active_user_test_helper.cpp
  flags.cpp
  http_server_test_helper.cpp
  resources_utils.cpp
  test_helper_main.cpp
  utils.cpp
  )

if (NOT WIN32)
  set(TEST_HELPER_SRC
    ${TEST_HELPER_SRC}
    kill_policy_test_helper.cpp
    containerizer/memory_test_helper.cpp
    )
endif ()

if (LINUX)
  set(TEST_HELPER_SRC
    ${TEST_HELPER_SRC}
    containerizer/capabilities_test_helper.cpp
    containerizer/setns_test_helper.cpp
    )
endif ()

# Test utilities.
#################
set(MESOS_TESTS_UTILS_SRC
  ${MESOS_TESTS_UTILS_SRC}
  active_user_test_helper.cpp
  cluster.cpp
  containerizer.cpp
  environment.cpp
  flags.cpp
  http_server_test_helper.cpp
  main.cpp
  mesos.cpp
  mock_docker.cpp
  mock_fetcher.cpp
  mock_registrar.cpp
  mock_slave.cpp
  module.cpp
  resources_utils.cpp
  utils.cpp
  containerizer/launcher.cpp
  )

if (NOT WIN32)
  set(MESOS_TESTS_UTILS_SRC
    ${MESOS_TESTS_UTILS_SRC}
    kill_policy_test_helper.cpp
    script.cpp
    containerizer/memory_test_helper.cpp
    )
endif ()

if (LINUX)
  set(MESOS_TESTS_UTILS_SRC
    ${MESOS_TESTS_UTILS_SRC}
    containerizer/capabilities_test_helper.cpp
    containerizer/setns_test_helper.cpp
    )
endif ()

# All the test sources.
#######################
set(MESOS_TESTS_SRC
  ${MESOS_TESTS_SRC}
  ${MESOS_TESTS_UTILS_SRC}
  anonymous_tests.cpp
  api_tests.cpp
  attributes_tests.cpp
  authentication_tests.cpp
  check_tests.cpp
  command_executor_tests.cpp
  credentials_tests.cpp
  default_executor_tests.cpp
  exception_tests.cpp
  executor_http_api_tests.cpp
  fault_tolerance_tests.cpp
  fetcher_tests.cpp
  files_tests.cpp
  gc_tests.cpp
  health_check_tests.cpp
  hierarchical_allocator_tests.cpp
  hook_tests.cpp
  http_authentication_tests.cpp
  http_fault_tolerance_tests.cpp
  master_maintenance_tests.cpp
  master_slave_reconciliation_tests.cpp
  partition_tests.cpp
  paths_tests.cpp
  protobuf_io_tests.cpp
  rate_limiting_tests.cpp
  resource_offers_tests.cpp
  resource_provider_http_api_tests.cpp
  resources_tests.cpp
  role_tests.cpp
  scheduler_driver_tests.cpp
  scheduler_event_call_tests.cpp
  scheduler_http_api_tests.cpp
  scheduler_tests.cpp
  slave_tests.cpp
  slave_validation_tests.cpp
  sorter_tests.cpp
  status_update_manager_tests.cpp
  uri_tests.cpp
  uri_fetcher_tests.cpp
  values_tests.cpp
  zookeeper_url_tests.cpp
  )

if (ENABLE_SSL)
  set(MESOS_TESTS_SRC
    ${MESOS_TESTS_SRC}
    secret_generator_tests.cpp
    )
endif ()

set(MESOS_TESTS_SRC
  ${MESOS_TESTS_SRC}
  common/http_tests.cpp
  common/recordio_tests.cpp
  common/type_utils_tests.cpp
  )

set(MESOS_TESTS_SRC
  ${MESOS_TESTS_SRC}
  containerizer/containerizer_tests.cpp
  containerizer/docker_tests.cpp
  )

if (NOT WIN32)
  set(MESOS_TESTS_SRC
    ${MESOS_TESTS_SRC}
    authorization_tests.cpp
    container_logger_tests.cpp
    cram_md5_authentication_tests.cpp
    disk_quota_tests.cpp
    dynamic_weights_tests.cpp
    examples_tests.cpp
    fetcher_cache_tests.cpp
    hdfs_tests.cpp
    log_tests.cpp
    logging_tests.cpp
    master_allocator_tests.cpp
    master_authorization_tests.cpp
    master_contender_detector_tests.cpp
    master_quota_tests.cpp
    master_tests.cpp
    master_validation_tests.cpp
    metrics_tests.cpp
    module_tests.cpp
    oversubscription_tests.cpp
    persistent_volume_endpoints_tests.cpp
    persistent_volume_tests.cpp
    protobuf_utils_tests.cpp
    reconciliation_tests.cpp
    registrar_tests.cpp
    reservation_endpoints_tests.cpp
    reservation_tests.cpp
    slave_authorization_tests.cpp
    slave_recovery_tests.cpp
    state_tests.cpp
    teardown_tests.cpp
    upgrade_tests.cpp
    )

  set(MESOS_TESTS_SRC
    ${MESOS_TESTS_SRC}
    common/command_utils_tests.cpp
    )

  set(MESOS_TESTS_SRC
    ${MESOS_TESTS_SRC}
    containerizer/appc_spec_tests.cpp
    containerizer/composing_containerizer_tests.cpp
    containerizer/cpu_isolator_tests.cpp
    containerizer/docker_containerizer_tests.cpp
    containerizer/docker_spec_tests.cpp
    containerizer/environment_secret_isolator_tests.cpp
    containerizer/io_switchboard_tests.cpp
    containerizer/isolator_tests.cpp
    containerizer/memory_isolator_tests.cpp
    containerizer/mesos_containerizer_paths_tests.cpp
    containerizer/mesos_containerizer_tests.cpp
    containerizer/oci_spec_tests.cpp
    containerizer/posix_rlimits_isolator_tests.cpp
    containerizer/provisioner_appc_tests.cpp
    containerizer/provisioner_backend_tests.cpp
    containerizer/provisioner_docker_tests.cpp
    containerizer/provisioner_paths_tests.cpp
    containerizer/volume_sandbox_path_isolator_tests.cpp
    )
endif ()

if (LINUX)
  set(MESOS_TESTS_SRC
    ${MESOS_TESTS_SRC}
    ldcache_tests.cpp
    ldd_tests.cpp
    containerizer/capabilities_tests.cpp
    containerizer/cgroups_isolator_tests.cpp
    containerizer/cgroups_tests.cpp
    containerizer/cni_isolator_tests.cpp
    containerizer/docker_volume_isolator_tests.cpp
    containerizer/fs_tests.cpp
    containerizer/linux_capabilities_isolator_tests.cpp
    containerizer/linux_filesystem_isolator_tests.cpp
    containerizer/memory_pressure_tests.cpp
    containerizer/nested_mesos_containerizer_tests.cpp
    containerizer/ns_tests.cpp
    containerizer/nvidia_gpu_isolator_tests.cpp
    containerizer/perf_tests.cpp
    containerizer/rootfs.cpp
    containerizer/runtime_isolator_tests.cpp
    containerizer/sched_tests.cpp
    containerizer/volume_image_isolator_tests.cpp
    containerizer/volume_secret_isolator_tests.cpp
    )
endif ()

# INCLUDE DIRECTIVES (generates, e.g., -I/path/to/thing on Linux).
##################################################################
include_directories(SYSTEM ${MESOS_3RDPARTY_TESTS_INCLUDE_DIRS})
include_directories(${MESOS_TESTS_INCLUDE_DIRS})

# LINKING LIBRARIES (might generate, e.g., -L/path/to/thing on Linux).
######################################################################
link_directories(${MESOS_TESTS_LIB_DIRS})

# THE TEST AND HELPER EXECUTABLEs (generates, e.g., stout-tests, etc., on Linux).
#################################$###############################################
add_executable(${MESOS_TESTS_TARGET} EXCLUDE_FROM_ALL ${MESOS_TESTS_SRC})
add_executable(${TEST_HELPER_TARGET} EXCLUDE_FROM_ALL ${TEST_HELPER_SRC})

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${MESOS_TESTS_TARGET} ${MESOS_TESTS_LIBS})
target_link_libraries(${TEST_HELPER_TARGET} ${MESOS_TESTS_LIBS})

# ADD BINARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(
  ${MESOS_TESTS_TARGET}
  ${MESOS_TESTS_DEPENDENCIES}
  ${TEST_HELPER_TARGET}
  )

if (NOT WIN32)
  # The tests require all the test modules.
  # NOTE: Modules are not supported on Windows.
  add_dependencies(
    ${MESOS_TESTS_TARGET}
    ${TEST_ALLOCATOR}
    ${TEST_ANONYMOUS}
    ${TEST_AUTHENTICATION}
    ${TEST_AUTHORIZER}
    ${TEST_CONTAINER_LOGGER}
    ${TEST_EXAMPLEMODULE}
    ${TEST_HOOK}
    ${TEST_HTTPAUTHENTICATOR}
    ${TEST_ISOLATOR}
    ${TEST_MASTER_CONTENDER}
    ${TEST_MASTER_DETECTOR}
    ${TEST_QOS_CONTROLLER}
    ${TEST_RESOURCE_ESTIMATOR}
    )

  # The tests require all the example frameworks and executors.
  # TODO(josephw): The scheduler driver is current not built on Windows.
  add_dependencies(
    ${MESOS_TESTS_TARGET}
    ${BALLOON_EXECUTOR}
    ${BALLOON_FRAMEWORK}
    ${DISK_FULL_FRAMEWORK}
    ${DOCKER_NO_EXECUTOR_FRAMEWORK}
    ${DYNAMIC_RESERVATION_FRAMEWORK}
    ${LOAD_GENERATOR_FRAMEWORK}
    ${LONG_LIVED_EXECUTOR}
    ${LONG_LIVED_FRAMEWORK}
    ${NO_EXECUTOR_FRAMEWORK}
    ${PERSISTENT_VOLUME_FRAMEWORK}
    ${TEST_EXECUTOR}
    ${TEST_FRAMEWORK}
    ${TEST_HTTP_EXECUTOR}
    ${TEST_HTTP_FRAMEWORK}
    )
endif ()

add_dependencies(${TEST_HELPER_TARGET} ${MESOS_TESTS_DEPENDENCIES})

# ADD TEST TARGET (runs when you do, e.g., `make check`).
#########################################################
add_test(NAME MesosTests COMMAND ${MESOS_TESTS_TARGET})
