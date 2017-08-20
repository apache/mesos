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

# GENERATE PUBLIC AND INTERNAL PROTOBUFS.
#########################################
include(MesosProtobuf)

# Build the protobuf structs.
PROTOC_TO_INCLUDE_DIR(ACLS                  mesos/authorizer/acls)
PROTOC_TO_INCLUDE_DIR(AGENT                 mesos/agent/agent)
PROTOC_TO_INCLUDE_DIR(ALLOCATOR             mesos/allocator/allocator)
PROTOC_TO_INCLUDE_DIR(APPC_SPEC             mesos/appc/spec)
PROTOC_TO_INCLUDE_DIR(AUTHENTICATION        mesos/authentication/authentication)
PROTOC_TO_INCLUDE_DIR(AUTHORIZATION         mesos/authorizer/authorizer)
PROTOC_TO_INCLUDE_DIR(CONTAINERIZER         mesos/slave/containerizer)
PROTOC_TO_INCLUDE_DIR(DOCKER_SPEC           mesos/docker/spec)
PROTOC_TO_INCLUDE_DIR(DOCKER_V1             mesos/docker/v1)
PROTOC_TO_INCLUDE_DIR(DOCKER_V2             mesos/docker/v2)
PROTOC_TO_INCLUDE_DIR(EXECUTOR              mesos/executor/executor)
PROTOC_TO_INCLUDE_DIR(FETCHER               mesos/fetcher/fetcher)
PROTOC_TO_INCLUDE_DIR(HOOK                  mesos/module/hook)
PROTOC_TO_INCLUDE_DIR(MAINTENANCE           mesos/maintenance/maintenance)
PROTOC_TO_INCLUDE_DIR(MASTER                mesos/master/master)
PROTOC_TO_INCLUDE_DIR(MESOS                 mesos/mesos)
PROTOC_TO_INCLUDE_DIR(MODULE                mesos/module/module)
PROTOC_TO_INCLUDE_DIR(OVERSUBSCRIPTION      mesos/slave/oversubscription)
PROTOC_TO_INCLUDE_DIR(OCI_SPEC              mesos/oci/spec)
PROTOC_TO_INCLUDE_DIR(QUOTA                 mesos/quota/quota)
PROTOC_TO_INCLUDE_DIR(RESOURCE_PROVIDER     mesos/resource_provider/resource_provider)
PROTOC_TO_INCLUDE_DIR(SCHEDULER             mesos/scheduler/scheduler)
PROTOC_TO_INCLUDE_DIR(STATE                 mesos/state/state)
PROTOC_TO_INCLUDE_DIR(URI                   mesos/uri/uri)
PROTOC_TO_INCLUDE_DIR(V1_AGENT              mesos/v1/agent/agent)
PROTOC_TO_INCLUDE_DIR(V1_ALLOCATOR          mesos/v1/allocator/allocator)
PROTOC_TO_INCLUDE_DIR(V1_EXECUTOR           mesos/v1/executor/executor)
PROTOC_TO_INCLUDE_DIR(V1_MAINTENANCE        mesos/v1/maintenance/maintenance)
PROTOC_TO_INCLUDE_DIR(V1_MASTER             mesos/v1/master/master)
PROTOC_TO_INCLUDE_DIR(V1_MESOS              mesos/v1/mesos)
PROTOC_TO_INCLUDE_DIR(V1_QUOTA              mesos/v1/quota/quota)
PROTOC_TO_INCLUDE_DIR(V1_RESOURCE_PROVIDER  mesos/v1/resource_provider/resource_provider)
PROTOC_TO_INCLUDE_DIR(V1_SCHEDULER          mesos/v1/scheduler/scheduler)

PROTOC_TO_SRC_DIR(INTERNAL_FLAGS                messages/flags)
PROTOC_TO_SRC_DIR(INTERNAL_LOG                  messages/log)
PROTOC_TO_SRC_DIR(INTERNAL_MESSAGES             messages/messages)
PROTOC_TO_SRC_DIR(INTERNAL_STATE                messages/state)
PROTOC_TO_SRC_DIR(ISOLATOR_CNI_SPEC             slave/containerizer/mesos/isolators/network/cni/spec)
PROTOC_TO_SRC_DIR(ISOLATOR_DOCKER_VOLUME_STATE  slave/containerizer/mesos/isolators/docker/volume/state)
PROTOC_TO_SRC_DIR(PROVISIONER_DOCKER            slave/containerizer/mesos/provisioner/docker/message)
PROTOC_TO_SRC_DIR(REGISTRY                      master/registry)

set(PUBLIC_PROTOBUF_SRC
  ${ACLS_PROTO_CC}
  ${AGENT_PROTO_CC}
  ${ALLOCATOR_PROTO_CC}
  ${APPC_SPEC_PROTO_CC}
  ${AUTHENTICATION_PROTO_CC}
  ${AUTHORIZATION_PROTO_CC}
  ${CONTAINERIZER_PROTO_CC}
  ${DOCKER_SPEC_PROTO_CC}
  ${DOCKER_V1_PROTO_CC}
  ${DOCKER_V2_PROTO_CC}
  ${EXECUTOR_PROTO_CC}
  ${FETCHER_PROTO_CC}
  ${HOOK_PROTO_CC}
  ${MAINTENANCE_PROTO_CC}
  ${MASTER_PROTO_CC}
  ${MESOS_PROTO_CC}
  ${MODULE_PROTO_CC}
  ${OCI_SPEC_PROTO_CC}
  ${OVERSUBSCRIPTION_PROTO_CC}
  ${QUOTA_PROTO_CC}
  ${RESOURCE_PROVIDER_PROTO_CC}
  ${SCHEDULER_PROTO_CC}
  ${STATE_PROTO_CC}
  ${URI_PROTO_CC}
  ${V1_AGENT_PROTO_CC}
  ${V1_ALLOCATOR_PROTO_CC}
  ${V1_EXECUTOR_PROTO_CC}
  ${V1_MAINTENANCE_PROTO_CC}
  ${V1_MASTER_PROTO_CC}
  ${V1_MESOS_PROTO_CC}
  ${V1_QUOTA_PROTO_CC}
  ${V1_RESOURCE_PROVIDER_PROTO_CC}
  ${V1_SCHEDULER_PROTO_CC}
  )

set(INTERNAL_PROTOBUF_SRC
  ${INTERNAL_FLAGS_PROTO_CC}
  ${INTERNAL_LOG_PROTO_CC}
  ${INTERNAL_MESSAGES_PROTO_CC}
  ${INTERNAL_STATE_PROTO_CC}
  ${ISOLATOR_CNI_SPEC_PROTO_CC}
  ${ISOLATOR_DOCKER_VOLUME_STATE_PROTO_CC}
  ${PROVISIONER_DOCKER_PROTO_CC}
  ${REGISTRY_PROTO_CC}
  )


# BUILD PROTOBUFS.
##################
include_directories(
  ${MESOS_PROTOBUF_HEADER_INCLUDE_DIRS}
  ${PROTOBUF_INCLUDE_DIR})

link_directories(${PROTOBUF_LIB_DIR})

add_library(
  ${MESOS_PROTOBUF_TARGET}
  ${MESOS_DEFAULT_LIBRARY_LINKAGE}
  ${PUBLIC_PROTOBUF_SRC}
  ${INTERNAL_PROTOBUF_SRC}
  )

add_dependencies(${MESOS_PROTOBUF_TARGET} ${PROTOBUF_TARGET})

target_link_libraries(${MESOS_PROTOBUF_TARGET} ${PROTOBUF_LFLAG})

if (WIN32)
  # Disable all compiler warnings on Protocol Buffers source.
  set_target_properties(
    ${MESOS_PROTOBUF_TARGET}
    PROPERTIES COMPILE_FLAGS "/w")
endif ()


# Configure Mesos files.
########################
configure_file(
  ${MESOS_PUBLIC_INCLUDE_DIR}/mesos/version.hpp.in
  ${MESOS_BIN_INCLUDE_DIR}/mesos/version.hpp
  )

# SOURCE FILES FOR THE MESOS LIBRARY.
#####################################
set(AGENT_SRC
  ${AGENT_SRC}
  slave/constants.cpp
  slave/container_logger.cpp
  slave/flags.cpp
  slave/gc.cpp
  slave/http.cpp
  slave/metrics.cpp
  slave/paths.cpp
  slave/qos_controller.cpp
  slave/qos_controllers/noop.cpp
  slave/resource_estimator.cpp
  slave/slave.cpp
  slave/state.cpp
  slave/status_update_manager.cpp
  slave/validation.cpp
  slave/container_loggers/sandbox.cpp
  slave/containerizer/composing.cpp
  slave/containerizer/containerizer.cpp
  slave/containerizer/docker.cpp
  slave/containerizer/fetcher.cpp
  slave/containerizer/mesos/containerizer.cpp
  slave/containerizer/mesos/isolator.cpp
  slave/containerizer/mesos/launch.cpp
  slave/containerizer/mesos/launcher.cpp
  slave/containerizer/mesos/mount.cpp
  slave/containerizer/mesos/paths.cpp
  slave/containerizer/mesos/utils.cpp
  slave/containerizer/mesos/io/switchboard.cpp
  slave/containerizer/mesos/provisioner/backend.cpp
  slave/containerizer/mesos/provisioner/paths.cpp
  slave/containerizer/mesos/provisioner/provisioner.cpp
  slave/containerizer/mesos/provisioner/store.cpp
  slave/containerizer/mesos/provisioner/utils.cpp
  slave/containerizer/mesos/provisioner/appc/cache.cpp
  slave/containerizer/mesos/provisioner/appc/fetcher.cpp
  slave/containerizer/mesos/provisioner/appc/paths.cpp
  slave/containerizer/mesos/provisioner/appc/store.cpp
  slave/containerizer/mesos/provisioner/backends/copy.cpp
  slave/containerizer/mesos/provisioner/docker/local_puller.cpp
  slave/containerizer/mesos/provisioner/docker/metadata_manager.cpp
  slave/containerizer/mesos/provisioner/docker/paths.cpp
  slave/containerizer/mesos/provisioner/docker/puller.cpp
  slave/containerizer/mesos/provisioner/docker/registry_puller.cpp
  slave/containerizer/mesos/provisioner/docker/store.cpp
  slave/resource_estimators/noop.cpp
  )

if (NOT WIN32)
  set(AGENT_SRC
    ${AGENT_SRC}
    slave/containerizer/mesos/isolators/environment_secret.cpp
    slave/containerizer/mesos/isolators/docker/volume/driver.cpp
    slave/containerizer/mesos/isolators/docker/volume/paths.cpp
    slave/containerizer/mesos/isolators/filesystem/posix.cpp
    slave/containerizer/mesos/isolators/network/cni/paths.cpp
    slave/containerizer/mesos/isolators/network/cni/spec.cpp
    slave/containerizer/mesos/isolators/posix/disk.cpp
    slave/containerizer/mesos/isolators/posix/rlimits.cpp
    slave/containerizer/mesos/isolators/volume/sandbox_path.cpp
    )
endif ()

set(APPC_SRC
  appc/spec.cpp
  )

set(AUTHENTICATION_SRC
  authentication/http/basic_authenticator_factory.cpp
  authentication/http/combined_authenticator.cpp
  )

if (NOT WIN32)
  set(AUTHENTICATION_SRC
    ${AUTHENTICATION_SRC}
    authentication/cram_md5/authenticatee.cpp
    authentication/cram_md5/authenticator.cpp
    authentication/cram_md5/auxprop.cpp
    )
endif ()

if (ENABLE_SSL)
  set(AUTHENTICATION_SRC
    ${AUTHENTICATION_SRC}
    authentication/executor/jwt_secret_generator.cpp
    )
endif ()

set(AUTHORIZER_SRC
  authorizer/acls.cpp
  authorizer/authorizer.cpp
  authorizer/local/authorizer.cpp
  )

set(COMMON_SRC
  common/attributes.cpp
  common/build.cpp
  common/command_utils.cpp
  common/http.cpp
  common/protobuf_utils.cpp
  common/resources.cpp
  common/resources_utils.cpp
  common/roles.cpp
  common/type_utils.cpp
  common/validation.cpp
  common/values.cpp
  )

set(DOCKER_SRC
  docker/docker.cpp
  docker/spec.cpp
  )

set(EXECUTOR_SRC
  exec/exec.cpp
  executor/executor.cpp
  executor/v0_v1executor.cpp
  )

set(FILES_SRC
  files/files.cpp
  )

set(HDFS_SRC
  ${HDFS_SRC}
  hdfs/hdfs.cpp
  )

set(HEALTH_CHECK_SRC
  checks/checker.cpp
  checks/checker_process.cpp
  checks/health_checker.cpp
  )

set(INTERNAL_SRC
  internal/devolve.cpp
  internal/evolve.cpp
  )

set(LINUX_SRC
  ${LINUX_SRC}
  linux/capabilities.cpp
  linux/cgroups.cpp
  linux/fs.cpp
  linux/ldcache.cpp
  linux/ldd.cpp
  linux/perf.cpp
  linux/systemd.cpp
  slave/containerizer/mesos/linux_launcher.cpp
  slave/containerizer/mesos/isolators/appc/runtime.cpp
  slave/containerizer/mesos/isolators/cgroups/cgroups.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystem.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/blkio.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/cpu.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/cpuacct.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/cpuset.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/devices.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/hugetlb.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/memory.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/net_cls.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/net_prio.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/perf_event.cpp
  slave/containerizer/mesos/isolators/cgroups/subsystems/pids.cpp
  slave/containerizer/mesos/isolators/docker/runtime.cpp
  slave/containerizer/mesos/isolators/docker/volume/isolator.cpp
  slave/containerizer/mesos/isolators/filesystem/linux.cpp
  slave/containerizer/mesos/isolators/filesystem/shared.cpp
  slave/containerizer/mesos/isolators/gpu/allocator.cpp
  slave/containerizer/mesos/isolators/gpu/isolator.cpp
  slave/containerizer/mesos/isolators/gpu/nvml.cpp
  slave/containerizer/mesos/isolators/gpu/volume.cpp
  slave/containerizer/mesos/isolators/linux/capabilities.cpp
  slave/containerizer/mesos/isolators/namespaces/ipc.cpp
  slave/containerizer/mesos/isolators/namespaces/pid.cpp
  slave/containerizer/mesos/isolators/network/cni/cni.cpp
  slave/containerizer/mesos/isolators/volume/host_path.cpp
  slave/containerizer/mesos/isolators/volume/image.cpp
  slave/containerizer/mesos/isolators/volume/secret.cpp
  slave/containerizer/mesos/provisioner/backends/aufs.cpp
  slave/containerizer/mesos/provisioner/backends/bind.cpp
  slave/containerizer/mesos/provisioner/backends/overlay.cpp
  )

set(LOCAL_SRC
  local/local.cpp
  )

set(LOG_SRC
  ${LOG_SRC}
  log/catchup.cpp
  log/consensus.cpp
  log/coordinator.cpp
  log/leveldb.cpp
  log/log.cpp
  log/main.cpp
  log/metrics.cpp
  log/recover.cpp
  log/replica.cpp
  log/tool/benchmark.cpp
  log/tool/initialize.cpp
  log/tool/read.cpp
  log/tool/replica.cpp
  )

set(LOGGING_SRC
  logging/flags.cpp
  logging/logging.cpp
  )

set(MASTER_SRC
  ${MASTER_SRC}
  master/flags.cpp
  master/http.cpp
  master/maintenance.cpp
  master/master.cpp
  master/metrics.cpp
  master/quota.cpp
  master/quota_handler.cpp
  master/registrar.cpp
  master/weights.cpp
  master/weights_handler.cpp
  master/validation.cpp
  master/allocator/allocator.cpp
  master/allocator/mesos/hierarchical.cpp
  master/allocator/mesos/metrics.cpp
  master/allocator/sorter/drf/metrics.cpp
  master/allocator/sorter/drf/sorter.cpp
  master/contender/contender.cpp
  master/contender/standalone.cpp
  master/contender/zookeeper.cpp
  master/detector/detector.cpp
  master/detector/standalone.cpp
  master/detector/zookeeper.cpp
  )

set(MESSAGES_SRC
  ${MESSAGES_SRC}
  messages/messages.cpp
  )

set(MODULE_SRC
  hook/manager.cpp
  module/manager.cpp
  )

set(OCI_SRC
  oci/spec.cpp
  )

set(POSIX_SRC
  posix/rlimits.cpp
  )

set(RESOURCE_PROVIDER_SRC
  resource_provider/daemon.cpp
  resource_provider/detector.cpp
  resource_provider/driver.cpp
  resource_provider/local.cpp
  resource_provider/manager.cpp
  resource_provider/validation.cpp
  resource_provider/storage/provider.cpp
  )

set(SCHEDULER_SRC
  sched/sched.cpp
  scheduler/scheduler.cpp
  )

set(SECRET_SRC
  secret/resolver.cpp
  )

set(STATE_SRC
  ${STATE_SRC}
  state/in_memory.cpp
  )

if (NOT WIN32)
  set(STATE_SRC
    ${STATE_SRC}
    state/leveldb.cpp
    state/log.cpp
    )
endif ()

set(URI_SRC
  uri/fetcher.cpp
  uri/utils.cpp
  uri/fetchers/copy.cpp
  uri/fetchers/curl.cpp
  )

if (NOT WIN32)
  set(URI_SRC
    ${URI_SRC}
    uri/fetchers/docker.cpp
    uri/fetchers/hadoop.cpp
    )
endif ()

set(USAGE_SRC
  usage/usage.cpp
  )

set(V1_SRC
  v1/attributes.cpp
  v1/mesos.cpp
  v1/resources.cpp
  v1/values.cpp
  )

set(VERSION_SRC
  version/version.cpp
  )

set(WATCHER_SRC
  watcher/whitelist_watcher.cpp
  )

set(WIN32_SRC
  ${WIN32_SRC}
  slave/containerizer/mesos/isolators/environment_secret.cpp
  slave/containerizer/mesos/isolators/filesystem/posix.cpp
  slave/containerizer/mesos/isolators/filesystem/windows.cpp
  )

set(ZOOKEEPER_SRC
  zookeeper/authentication.cpp
  zookeeper/contender.cpp
  zookeeper/detector.cpp
  zookeeper/zookeeper.cpp
  zookeeper/group.cpp
  )

set(MESOS_SRC
  ${MESOS_SRC}
  ${AGENT_SRC}
  ${APPC_SRC}
  ${AUTHENTICATION_SRC}
  ${AUTHORIZER_SRC}
  ${COMMON_SRC}
  ${DOCKER_SRC}
  ${EXECUTOR_SRC}
  ${FILES_SRC}
  ${HEALTH_CHECK_SRC}
  ${INTERNAL_SRC}
  ${LOCAL_SRC}
  ${LOGGING_SRC}
  ${MASTER_SRC}
  ${MESSAGES_SRC}
  ${MODULE_SRC}
  ${OCI_SRC}
  ${RESOURCE_PROVIDER_SRC}
  ${SCHEDULER_SRC}
  ${SECRET_SRC}
  ${STATE_SRC}
  ${URI_SRC}
  ${USAGE_SRC}
  ${V1_SRC}
  ${VERSION_SRC}
  ${WATCHER_SRC}
  ${ZOOKEEPER_SRC}
  )

if (NOT WIN32)
  set(MESOS_SRC
    ${MESOS_SRC}
    ${HDFS_SRC}
    ${LOG_SRC}
    ${POSIX_SRC}
    )
endif ()

# Include source for linux build.
if (LINUX)
  set(MESOS_SRC ${MESOS_SRC} ${LINUX_SRC})
endif ()

# Include source for Windows build.
if (WIN32)
  set(MESOS_SRC ${MESOS_SRC} ${WIN32_SRC})
endif ()

# INCLUDE DIRECTIVES FOR MESOS LIBRARY (generates, e.g., -I/path/to/thing
# on Linux).
#########################################################################
include_directories(SYSTEM ${AGENT_3RDPARTY_INCLUDE_DIRS})
include_directories(${AGENT_INCLUDE_DIRS})

# Generate all binaries in the same folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src)
if (WIN32)
  SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/src)
  SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/src)
endif ()

# Generate all libraries in the same folder to conform with the Automake build.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/.libs)
if (WIN32)
  SET(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/src/.libs)
  SET(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/src/.libs)
endif ()

# LINKING LIBRARIES BY DIRECTORY (might generate, e.g., -L/path/to/thing on
# Linux).
###########################################################################
link_directories(${AGENT_LIB_DIRS})

# THE MESOS LIBRARY (generates, e.g., libmesos.so, etc., on Linux).
###################################################################
add_library(${MESOS_LIBS_TARGET} ${MESOS_DEFAULT_LIBRARY_LINKAGE} ${MESOS_SRC})
set_target_properties(
  ${MESOS_LIBS_TARGET} PROPERTIES
  VERSION ${MESOS_PACKAGE_VERSION}
  SOVERSION ${MESOS_PACKAGE_SOVERSION}
  )

if (ENABLE_PRECOMPILED_HEADERS)
  # These exluded headers either cause namespace resolution issues or
  # are problematic for incremental builds.
  set_property(DIRECTORY
    PROPERTY COTIRE_PREFIX_HEADER_IGNORE_PATH
      "${CMAKE_SOURCE_DIR}/include/mesos/authentication/http/basic_authenticator_factory.hpp"
      "${CMAKE_SOURCE_DIR}/include/mesos/authentication/http/combined_authenticator.hpp"
      "${CMAKE_SOURCE_DIR}/src/sched/constants.hpp"
      "${CMAKE_SOURCE_DIR}/src/sched/flags.hpp"
      "${CMAKE_BINARY_DIR}/src/common/build_config.hpp")

  # For now, we include agent headers, because the Windows build currently
  # does not build all of the Mesos master (only enough for testing the agent).
  set_property(DIRECTORY
    PROPERTY COTIRE_PREFIX_HEADER_INCLUDE_PATH
      "${AGENT_INCLUDE_DIRS};${AGENT_3RDPARTY_INCLUDE_DIRS}")

  cotire(${MESOS_LIBS_TARGET})
endif ()

# ADD LIBRARY DEPENDENCIES (tells CMake what to compile/build first).
####################################################################
add_dependencies(${MESOS_TARGET} ${MESOS_LIBS_TARGET})
add_dependencies(
  ${MESOS_LIBS_TARGET}
  ${MESOS_PROTOBUF_TARGET}
  ${AGENT_DEPENDENCIES}
  )

# ADD LINKER FLAGS (generates, e.g., -lglog on Linux).
######################################################
target_link_libraries(${MESOS_LIBS_TARGET} ${MESOS_PROTOBUF_TARGET} ${AGENT_LIBS})

# BUILD THE MESOS EXECUTABLES.
##############################
add_subdirectory(checks/)
add_subdirectory(cli/)
add_subdirectory(docker/)
add_subdirectory(examples/)
add_subdirectory(launcher/)
add_subdirectory(local/)
add_subdirectory(log/)
add_subdirectory(master/)
add_subdirectory(slave/)
add_subdirectory(slave/containerizer/mesos/)
add_subdirectory(usage/)

# BUILD THE MESOS TESTS.
#########################################################
add_subdirectory(tests/)
