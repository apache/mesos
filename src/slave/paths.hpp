// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __SLAVE_PATHS_HPP__
#define __SLAVE_PATHS_HPP__

#include <list>
#include <string>

#include <mesos/mesos.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace paths {


// The slave leverages the file system for a number of purposes:
//
//   (1) For executor sandboxes and disk volumes.
//
//   (2) For checkpointing metadata in order to support "slave
//       recovery". That is, allow the slave to restart without
//       affecting the tasks / executors.
//
//   (3) To detect reboots (by checkpointing the boot id), in which
//       case everything has died so no recovery should be attempted.
//
//   (4) For checkpointing resources that persist across slaves.
//       This includes things like persistent volumes and dynamic
//       reservations.
//
//   (5) For provisioning root filesystems for containers.
//
//   (6) For CSI plugins to preserve data that persist across slaves.
//
// The file system layout is as follows:
//
//   root ('--work_dir' flag)
//   |-- containers
//   |   |-- <container_id> (sandbox)
//   |-- slaves
//   |   |-- latest (symlink)
//   |   |-- <slave_id>
//   |       |-- frameworks
//   |           |-- <framework_id>
//   |               |-- executors
//   |                   |-- <executor_id>
//   |                       |-- runs
//   |                           |-- latest (symlink)
//   |                           |-- <container_id> (sandbox)
//   |-- meta
//   |   |-- boot_id
//   |   |-- resources
//   |   |   |-- resources.info
//   |   |   |-- resources.target
//   |   |   |-- resources_and_operations.state
//   |   |-- slaves
//   |   |   |-- latest (symlink)
//   |   |   |-- <slave_id>
//   |   |       |-- slave.info
//   |   |       |-- drain.config
//   |   |       |-- operations
//   |   |       |   |-- <operation_uuid>
//   |   |       |       |-- operation.updates
//   |   |       |-- resource_providers
//   |   |       |   |-- <type>
//   |   |       |       |-- <name>
//   |   |       |           |-- latest (symlink)
//   |   |       |           |-- <resource_provider_id>
//   |   |       |               |-- resource_provider.state
//   |   |       |               |-- operations
//   |   |       |                   |-- <operation_uuid>
//   |   |       |                       |-- operation.updates
//   |   |       |-- frameworks
//   |   |           |-- <framework_id>
//   |   |               |-- framework.info
//   |   |               |-- framework.pid
//   |   |               |-- executors
//   |   |                   |-- <executor_id>
//   |   |                       |-- executor.info
//   |   |                       |-- runs
//   |   |                           |-- latest (symlink)
//   |   |                           |-- <container_id> (sandbox)
//   |   |                               |-- executor.sentinel (if completed)
//   |   |                               |-- pids
//   |   |                               |   |-- forked.pid
//   |   |                               |   |-- libprocess.pid
//   |   |                               |-- tasks
//   |   |                                   |-- <task_id>
//   |   |                                       |-- task.info
//   |   |                                       |-- task.updates
//   |   |-- volume_gid_manager
//   |       |-- volume_gids
//   |-- volumes
//   |   |-- roles
//   |       |-- <role>
//   |           |-- <persistence_id> (persistent volume)
//   |-- provisioner
//   |-- csi


struct ExecutorRunPath
{
  SlaveID slaveId;
  FrameworkID frameworkId;
  ExecutorID executorId;
  ContainerID containerId;
};


Try<ExecutorRunPath> parseExecutorRunPath(
    const std::string& rootDir,
    const std::string& dir);


const char LATEST_SYMLINK[] = "latest";

// Helpers for obtaining paths in the layout.
// NOTE: The parameter names should adhere to the following convention:
//
//   (1) Use `workDir` if the helper expects the `--work_dir` flag.
//
//   (2) Use `metaDir` if the helper expects the meta directory.
//
//   (3) Use `rootDir` only if the helper is to be reused.
//
// TODO(chhsiao): Clean up the parameter names to follow the convention.

std::string getMetaRootDir(const std::string& rootDir);


std::string getSandboxRootDir(const std::string& rootDir);


std::string getProvisionerDir(const std::string& rootDir);


std::string getCsiRootDir(const std::string& workDir);


std::string getLatestSlavePath(const std::string& rootDir);


std::string getBootIdPath(const std::string& rootDir);


std::string getSlaveInfoPath(
    const std::string& rootDir,
    const SlaveID& slaveId);


std::string getSlavePath(
    const std::string& rootDir,
    const SlaveID& slaveId);


Try<std::list<std::string>> getContainerPaths(
    const std::string& rootDir);


std::string getContainerPath(
    const std::string& rootDir,
    const ContainerID& containerId);


Try<std::list<std::string>> getFrameworkPaths(
    const std::string& rootDir,
    const SlaveID& slaveId);


std::string getFrameworkPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId);


std::string getFrameworkPidPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId);


std::string getFrameworkInfoPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId);


Try<std::list<std::string>> getExecutorPaths(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId);


std::string getExecutorPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId);


std::string getExecutorInfoPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId);


Try<std::list<std::string>> getExecutorRunPaths(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId);


std::string getExecutorRunPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId);


std::string getExecutorGeneratedForCommandTaskPath(
  const std::string& rootDir,
  const SlaveID& slaveId,
  const FrameworkID& frameworkId,
  const ExecutorID& executorId);


std::string getExecutorHttpMarkerPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId);


std::string getExecutorSentinelPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId);


// Returns the "virtual" path used to expose the executor's sandbox
// via the /files endpoints: `/frameworks/FID/executors/EID/latest`.
std::string getExecutorVirtualPath(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId);


std::string getExecutorLatestRunPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId);


std::string getLibprocessPidPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId);


std::string getForkedPidPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId);


std::string getContainerRootfsPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId);


Try<std::list<std::string>> getTaskPaths(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId);


std::string getTaskPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId);


std::string getTaskInfoPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId);


std::string getTaskUpdatesPath(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const TaskID& taskId);


std::string getResourceProviderRegistryPath(
    const std::string& rootDir,
    const SlaveID& slaveId);


Try<std::list<std::string>> getResourceProviderPaths(
    const std::string& metaDir,
    const SlaveID& slaveId);


std::string getResourceProviderPath(
    const std::string& metaDir,
    const SlaveID& slaveId,
    const std::string& resourceProviderType,
    const std::string& resourceProviderName,
    const ResourceProviderID& resourceProviderId);


std::string getResourceProviderStatePath(
    const std::string& metaDir,
    const SlaveID& slaveId,
    const std::string& resourceProviderType,
    const std::string& resourceProviderName,
    const ResourceProviderID& resourceProviderId);


std::string getLatestResourceProviderPath(
    const std::string& metaDir,
    const SlaveID& slaveId,
    const std::string& resourceProviderType,
    const std::string& resourceProviderName);


Try<std::list<std::string>> getOperationPaths(
    const std::string& rootDir);


// Returns all the directories in which status update streams for operations
// affecting agent default resources are stored.
Try<std::list<std::string>> getSlaveOperationPaths(
    const std::string& metaDir,
    const SlaveID& slaveId);


std::string getOperationPath(
    const std::string& rootDir,
    const id::UUID& operationUuid);


// Returns the path of the directory in which the status update stream for a
// given operation affecting agent default resources is stored.
std::string getSlaveOperationPath(
    const std::string& metaDir,
    const SlaveID& slaveId,
    const id::UUID& operationUuid);


Try<id::UUID> parseOperationPath(
    const std::string& rootDir,
    const std::string& dir);


// Extracts the operation UUID from the path of a directory in which the status
// update stream for an operation affecting agent default resources is stored.
Try<id::UUID> parseSlaveOperationPath(
    const std::string& metaDir,
    const SlaveID& slaveId,
    const std::string& dir);


std::string getOperationUpdatesPath(
    const std::string& rootDir,
    const id::UUID& operationUuid);


// Returns the path of the file to which the status update stream for a given
// operation affecting agent default resources is stored.
std::string getSlaveOperationUpdatesPath(
    const std::string& metaDir,
    const SlaveID& slaveId,
    const id::UUID& operationUuid);


std::string getResourceStatePath(
    const std::string& rootDir);


std::string getResourceStateTargetPath(
    const std::string& rootDir);


std::string getResourcesInfoPath(
    const std::string& rootDir);


std::string getResourcesTargetPath(
    const std::string& rootDir);


std::string getDrainConfigPath(
    const std::string& metaDir,
    const SlaveID& slaveId);


Try<std::list<std::string>> getPersistentVolumePaths(
    const std::string& workDir);


std::string getPersistentVolumePath(
    const std::string& workDir,
    const std::string& role,
    const std::string& persistenceId);


std::string getPersistentVolumePath(
    const std::string& workDir,
    const Resource& resource);


std::string getVolumeGidsPath(const std::string& rootDir);


Try<std::string> createExecutorDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const Option<std::string>& user = None());


Try<Nothing> createSandboxDirectory(
    const std::string& directory,
    const Option<std::string>& user);


std::string createSlaveDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId);


std::string createResourceProviderDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const std::string& resourceProviderType,
    const std::string& resourceProviderName,
    const ResourceProviderID& resourceProviderId);


extern const char LIBPROCESS_PID_FILE[];
extern const char HTTP_MARKER_FILE[];

} // namespace paths {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_PATHS_HPP__
