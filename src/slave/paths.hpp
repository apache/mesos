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

#include <stout/try.hpp>

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
// The file system layout is as follows:
//
//   root ('--work_dir' flag)
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
//   |   |-- slaves
//   |       |-- latest (symlink)
//   |       |-- <slave_id>
//   |           |-- slave.info
//   |           |-- frameworks
//   |               |-- <framework_id>
//   |                   |-- framework.info
//   |                   |-- framework.pid
//   |                   |-- executors
//   |                       |-- <executor_id>
//   |                           |-- executor.info
//   |                           |-- runs
//   |                               |-- latest (symlink)
//   |                               |-- <container_id> (sandbox)
//   |                                   |-- executor.sentinel (if completed)
//   |                                   |-- pids
//   |                                   |   |-- forked.pid
//   |                                   |   |-- libprocess.pid
//   |                                   |-- tasks
//   |                                       |-- <task_id>
//   |                                           |-- task.info
//   |                                           |-- task.updates
//   |-- volumes
//   |   |-- roles
//   |       |-- <role>
//   |           |-- <persistence_id> (persistent volume)
//   |-- provisioner


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

// Helpers for obtaining paths in the layout:

std::string getMetaRootDir(const std::string& rootDir);


std::string getSandboxRootDir(const std::string& rootDir);


std::string getProvisionerDir(const std::string& rootDir);


std::string getLatestSlavePath(const std::string& rootDir);


std::string getBootIdPath(const std::string& rootDir);


std::string getSlaveInfoPath(
    const std::string& rootDir,
    const SlaveID& slaveId);


std::string getSlavePath(
    const std::string& rootDir,
    const SlaveID& slaveId);


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


std::string getResourcesInfoPath(
    const std::string& rootDir);


std::string getResourcesTargetPath(
    const std::string& rootDir);


std::string getPersistentVolumePath(
    const std::string& rootDir,
    const std::string& role,
    const std::string& persistenceId);


std::string getPersistentVolumePath(
    const std::string& rootDir,
    const Resource& resource);


std::string createExecutorDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const Option<std::string>& user = None());


std::string createSlaveDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId);


extern const char LIBPROCESS_PID_FILE[];
extern const char HTTP_MARKER_FILE[];

} // namespace paths {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_PATHS_HPP__
