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

#ifndef __MESOS_HOOK_HPP__
#define __MESOS_HOOK_HPP__

#include <map>
#include <string>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

namespace mesos {

class Hook
{
public:
  virtual ~Hook() {};

  // This label decorator hook is called from within master during
  // the launchTask routine. A module implementing the hook creates
  // and returns a set of labels. These labels overwrite the existing
  // labels on the task info.
  virtual Result<Labels> masterLaunchTaskLabelDecorator(
      const TaskInfo& taskInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    return None();
  }

  // This label decorator hook is called from within the slave when
  // receiving a run task request from the master. A module
  // implementing the hook creates and returns a set of labels. These
  // labels overwrite the existing labels on the task info.
  virtual Result<Labels> slaveRunTaskLabelDecorator(
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    return None();
  }

  // This environment decorator hook is called from within slave when
  // launching a new executor. A module implementing the hook creates
  // and returns a set of environment variables. These environment
  // variables then become part of the executor's environment.
  // Ideally, a hook module will also look at the exiting environment
  // variables in executorInfo and extend the values as needed in case
  // of a conflict.
  virtual Result<Environment> slaveExecutorEnvironmentDecorator(
      const ExecutorInfo& executorInfo)
  {
    return None();
  }

  // This hook is called from within slave before docker is launched.
  // A typical module implementing the hook will perform some settings
  // as required.
  virtual Try<Nothing> slavePreLaunchDockerHook(
      const ContainerInfo& containerInfo,
      const CommandInfo& commandInfo,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& name,
      const std::string& sandboxDirectory,
      const std::string& mappedDirectory,
      const Option<Resources>& resources,
      const Option<std::map<std::string, std::string>>& env)
  {
    return Nothing();
  }

  // This hook is called from within slave when an executor is being
  // removed. A typical module implementing the hook will perform some
  // cleanup as required.
  virtual Try<Nothing> slaveRemoveExecutorHook(
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo)
  {
    return Nothing();
  }

  // This hook is called from within slave when it receives a status update from
  // the executor. A module implementing the hook creates and returns a
  // TaskStatus with a set of labels and container_status. These labels and
  // container status overwrite the existing labels on the TaskStatus. Remaining
  // fields from the returned TaskStatus are discarded.
  virtual Result<TaskStatus> slaveTaskStatusDecorator(
      const FrameworkID& frameworkId,
      const TaskStatus& status)
  {
    return None();
  }
};

} // namespace mesos {

#endif // __MESOS_HOOK_HPP__
