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

#ifndef __MESOS_HOOK_HPP__
#define __MESOS_HOOK_HPP__

#include <map>
#include <string>

#include <mesos/attributes.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/future.hpp>

#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <mesos/module/hook.pb.h>

namespace mesos {

class Hook
{
public:
  virtual ~Hook() {}

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

  // This resource decorator hook is called from within the master during
  // the launchTask routine. An implementation of this hook allows the user
  // to, for example, allocate a default amount of a certain resource if it is
  // not provided by the framework. This hook aims to provide default values for
  // resources that must be accounted. Implementing this hook is useful when one
  // a new resource is introduced and at least one framework does not support it
  // yet. Note that the original resources from the TaskInfo will be overwritten
  // by the ones returned by the hook.
  virtual Result<Resources> masterLaunchTaskResourceDecorator(
      const TaskInfo& task,
      const Resources& slaveResources)
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


  // This hook is called when an Agent is removed i.e. deemed lost by the
  // master. The hook is invoked after all frameworks have been informed about
  // the loss.
  virtual Try<Nothing> masterSlaveLostHook(const SlaveInfo& slaveInfo)
  {
    return Nothing();
  }


  // This environment decorator hook is called from within slave when
  // launching a new executor. A module implementing the hook creates
  // and returns a set of environment variables. These environment
  // variables then become part of the executor's environment.
  // Ideally, a hook module will also look at the existing environment
  // variables in executorInfo and extend the values as needed in case
  // of a conflict.
  virtual Result<Environment> slaveExecutorEnvironmentDecorator(
      const ExecutorInfo& executorInfo)
  {
    return None();
  }

  // This task and executor decorator is called from within the slave after
  // receiving a run task request from the master but before the docker
  // containerizer launches the task. A module implementing the hook can
  // inspect the arguments and return a `Failure` if the task should be
  // rejected with a `TASK_FAILED`.
  // The hook can return a set of environment variables individually for
  // both, the executor and the task. Note that for custom executors,
  // the task environment variables, in case of conflicts, *will*
  // overwrite the executor variables.
  //
  // NOTE: The order of hooks matters for environment variables.
  // If there is a conflict, the hook loaded last will take priority.
  //
  // NOTE: Unlike `slaveExecutorEnvironmentDecorator`, environment variables
  // returned from this hook, in case of conflicts, *will* overwrite
  // environment variables inside the `ExecutorInfo`.
  virtual process::Future<Option<DockerTaskExecutorPrepareInfo>>
    slavePreLaunchDockerTaskExecutorDecorator(
        const Option<TaskInfo>& taskInfo,
        const ExecutorInfo& executorInfo,
        const std::string& containerName,
        const std::string& containerWorkDirectory,
        const std::string& mappedSandboxDirectory,
        const Option<std::map<std::string, std::string>>& env)
  {
    return None();
  }


  // This hook is called from within slave after URIs and container
  // image are fetched. A typical module implementing this hook will
  // perform some operations on the fetched artifacts.
  virtual Try<Nothing> slavePostFetchHook(
      const ContainerID& containerId,
      const std::string& directory)
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

  // This hook is called from within the slave when it initializes. A module
  // implementing the hook creates and returns a Resources object with the new
  // list of resources available on the slave before they are advertised to the
  // master. These new resources overwrite the previous ones in SlaveInfo.
  virtual Result<Resources> slaveResourcesDecorator(
      const SlaveInfo& slaveInfo)
  {
    return None();
  }

  // This hook is called from within the slave when it initializes. A module
  // implementing the hook creates and returns an Attributes object with the
  // new list of attributes for the slave before they are advertised to the
  // master. These new attributes overwrite the previous ones in SlaveInfo.
  virtual Result<Attributes> slaveAttributesDecorator(
      const SlaveInfo& slaveInfo)
  {
    return None();
  }
};

} // namespace mesos {

#endif // __MESOS_HOOK_HPP__
