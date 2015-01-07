/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __SLAVE_PATHS_HPP__
#define __SLAVE_PATHS_HPP__

#include <list>
#include <string>

#include <mesos/mesos.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace paths {

const std::string LATEST_SYMLINK = "latest";

// Helpers for obtaining paths in the layout:

std::string getMetaRootDir(const std::string& rootDir);


std::string getArchiveDir(const std::string& rootDir);


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


std::string createExecutorDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId);


std::string createSlaveDirectory(
    const std::string& rootDir,
    const SlaveID& slaveId);

} // namespace paths {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_PATHS_HPP__
