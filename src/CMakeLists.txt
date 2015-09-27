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

# COMPILE PROTOBUF STRUCTS REQUIRED TO BUILD MESOS.
###################################################
include(MesosProtobuf)

# Build the protobuf structs.
PROTOC_TO_INCLUDE_DIR(MESOS            mesos/mesos)
PROTOC_TO_INCLUDE_DIR(V1_MESOS         mesos/v1/mesos)
PROTOC_TO_INCLUDE_DIR(AUTHENTICATION   mesos/authentication/authentication)
PROTOC_TO_INCLUDE_DIR(AUTHORIZATION    mesos/authorizer/authorizer)
PROTOC_TO_INCLUDE_DIR(CONTAINERIZER    mesos/containerizer/containerizer)
PROTOC_TO_INCLUDE_DIR(EXECUTOR         mesos/executor/executor)
PROTOC_TO_INCLUDE_DIR(V1_EXECUTOR      mesos/v1/executor/executor)
PROTOC_TO_INCLUDE_DIR(FETCHER          mesos/fetcher/fetcher)
PROTOC_TO_INCLUDE_DIR(MAINTENANCE      mesos/maintenance/maintenance)
PROTOC_TO_INCLUDE_DIR(ALLOCATOR        mesos/master/allocator)
PROTOC_TO_INCLUDE_DIR(MODULE           mesos/module/module)
PROTOC_TO_INCLUDE_DIR(SCHEDULER        mesos/scheduler/scheduler)
PROTOC_TO_INCLUDE_DIR(V1_SCHEDULER     mesos/v1/scheduler/scheduler)
PROTOC_TO_INCLUDE_DIR(ISOLATOR         mesos/slave/isolator)
PROTOC_TO_INCLUDE_DIR(OVERSUBSCRIPTION mesos/slave/oversubscription)

PROTOC_TO_SRC_DIR(MESSAGES messages/messages)
PROTOC_TO_SRC_DIR(FLAGS    messages/flags)

set(MESOS_PROTOBUF_SRC
  ${MESOS_PROTO_CC}
  ${V1_MESOS_PROTO_CC}
  ${AUTHENTICATION_PROTO_CC}
  ${AUTHORIZATION_PROTO_CC}
  ${CONTAINERIZER_PROTO_CC}
  ${EXECUTOR_PROTO_CC}
  ${V1_EXECUTOR_PROTO_CC}
  ${FETCHER_PROTO_CC}
  ${MAINTENANCE_PROTO_CC}
  ${ALLOCATOR_PROTO_CC}
  ${MODULE_PROTO_CC}
  ${SCHEDULER_PROTO_CC}
  ${V1_SCHEDULER_PROTO_CC}
  ${MESSAGES_PROTO_CC}
  ${FLAGS_PROTO_CC}
  ${ISOLATOR_PROTO_CC}
  ${OVERSUBSCRIPTION_PROTO_CC}
  )

# Configure Mesos files.
########################
configure_file(
  ${MESOS_PUBLIC_INCLUDE_DIR}/mesos/version.hpp.in
  ${MESOS_BIN_INCLUDE_DIR}/mesos/version.hpp
  )

# SOURCE FILES FOR THE MESOS LIBRARY.
#####################################
set(AGENT_SRC
  slave/constants.cpp
  slave/gc.cpp
  slave/flags.cpp
  slave/http.cpp
  slave/metrics.cpp
  slave/monitor.cpp
  slave/paths.cpp
  slave/qos_controller.cpp
  slave/qos_controllers/noop.cpp
  slave/resource_estimator.cpp
  slave/slave.cpp
  slave/state.cpp
  slave/status_update_manager.cpp
  slave/containerizer/containerizer.cpp
  slave/containerizer/composing.cpp
  slave/containerizer/composing.hpp
  slave/containerizer/docker.cpp
  slave/containerizer/docker.hpp
  slave/containerizer/external_containerizer.cpp
  slave/containerizer/fetcher.cpp
  slave/containerizer/isolator.cpp
  slave/containerizer/isolators/filesystem/posix.cpp
  slave/containerizer/isolators/posix/disk.cpp
  slave/containerizer/launcher.cpp
  slave/containerizer/mesos/containerizer.cpp
  slave/containerizer/mesos/launch.cpp
  slave/containerizer/provisioner/paths.cpp
  slave/containerizer/provisioner/provisioner.cpp
  slave/containerizer/provisioner/store.cpp
  slave/containerizer/provisioner/appc/paths.cpp
  slave/containerizer/provisioner/appc/spec.cpp
  slave/containerizer/provisioner/appc/store.cpp
  slave/containerizer/provisioner/backend.cpp
  slave/containerizer/provisioner/backends/copy.cpp
  slave/containerizer/provisioner/docker/registry_client.cpp
  slave/containerizer/provisioner/docker/token_manager.cpp
  slave/resource_estimators/noop.cpp
  )

set(MESOS_SRC
  ${MESOS_SRC}
  ${AGENT_SRC}
  ${MESOS_PROTOBUF_SRC}
  )

# INCLUDE DIRECTIVES FOR MESOS LIBRARY (generates, e.g., -I/path/to/thing
# on Linux).
#########################################################################
include_directories(${AGENT_INCLUDE_DIRS})

# THE MESOS LIBRARY (generates, e.g., libmesos.so, etc., on Linux).
###################################################################
add_library(${MESOS_TARGET} ${MESOS_SRC})
set_target_properties(
  ${MESOS_TARGET} PROPERTIES
  VERSION ${MESOS_PACKAGE_VERSION}
  SOVERSION ${MESOS_PACKAGE_SOVERSION}
  )
add_dependencies(${MESOS_TARGET} ${AGENT_DEPENDENCIES})
