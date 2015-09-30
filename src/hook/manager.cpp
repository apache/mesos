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

#include <mutex>
#include <string>
#include <vector>

#include <mesos/hook.hpp>

#include <mesos/module/hook.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "hook/manager.hpp"
#include "module/manager.hpp"

using std::map;
using std::string;
using std::vector;

using mesos::modules::ModuleManager;

namespace mesos {
namespace internal {

static std::mutex mutex;
static hashmap<string, Hook*> availableHooks;


Try<Nothing> HookManager::initialize(const string& hookList)
{
  synchronized (mutex) {
    const vector<string> hooks = strings::split(hookList, ",");
    foreach (const string& hook, hooks) {
      if (availableHooks.contains(hook)) {
        return Error("Hook module '" + hook + "' already loaded");
      }

      if (!ModuleManager::contains<Hook>(hook)) {
        return Error("No hook module named '" + hook + "' available");
      }

      // Let's create an instance of the hook module.
      Try<Hook*> module = ModuleManager::create<Hook>(hook);
      if (module.isError()) {
        return Error(
            "Failed to instantiate hook module '" + hook + "': " +
            module.error());
      }

      // Add the hook module to the list of available hooks.
      availableHooks[hook] = module.get();
    }
  }

  return Nothing();
}


Try<Nothing> HookManager::unload(const std::string& hookName)
{
  synchronized (mutex) {
    if (!availableHooks.contains(hookName)) {
      return Error(
          "Error unloading hook module '" + hookName + "': module not loaded");
    }

    // Now remove the hook from the list of available hooks.
    availableHooks.erase(hookName);
  }

  return Nothing();
}


bool HookManager::hooksAvailable()
{
  synchronized (mutex) {
    return !availableHooks.empty();
  }
}


Labels HookManager::masterLaunchTaskLabelDecorator(
    const TaskInfo& taskInfo,
    const FrameworkInfo& frameworkInfo,
    const SlaveInfo& slaveInfo)
{
  synchronized (mutex) {
    // We need a mutable copy of the task info and set the new
    // labels after each hook invocation. Otherwise, the last hook
    // will be the only effective hook setting the labels.
    TaskInfo taskInfo_ = taskInfo;

    foreachpair (const string& name, Hook* hook, availableHooks) {
      const Result<Labels> result =
        hook->masterLaunchTaskLabelDecorator(
            taskInfo_,
            frameworkInfo,
            slaveInfo);

      // NOTE: If the hook returns None(), the task labels won't be
      // changed.
      if (result.isSome()) {
        taskInfo_.mutable_labels()->CopyFrom(result.get());
      } else if (result.isError()) {
        LOG(WARNING) << "Master label decorator hook failed for module '"
                    << name << "': " << result.error();
      }
    }

    return taskInfo_.labels();
  }
}


void HookManager::masterSlaveLostHook(const SlaveInfo& slaveInfo)
{
  foreachpair (const string& name, Hook* hook, availableHooks) {
    Try<Nothing> result = hook->masterSlaveLostHook(slaveInfo);
    if (result.isError()) {
      LOG(WARNING) << "Master slave-lost hook failed for module '"
                   << name << "': " << result.error();
    }
  }
}


Labels HookManager::slaveRunTaskLabelDecorator(
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const FrameworkInfo& frameworkInfo,
    const SlaveInfo& slaveInfo)
{
  synchronized (mutex) {
    TaskInfo taskInfo_ = taskInfo;

    foreachpair (const string& name, Hook* hook, availableHooks) {
      const Result<Labels> result = hook->slaveRunTaskLabelDecorator(
          taskInfo_, executorInfo, frameworkInfo, slaveInfo);

      // NOTE: If the hook returns None(), the task labels won't be
      // changed.
      if (result.isSome()) {
        taskInfo_.mutable_labels()->CopyFrom(result.get());
      } else if (result.isError()) {
        LOG(WARNING) << "Slave label decorator hook failed for module '"
                    << name << "': " << result.error();
      }
    }

    return taskInfo_.labels();
  }
}


Environment HookManager::slaveExecutorEnvironmentDecorator(
    ExecutorInfo executorInfo)
{
  synchronized (mutex) {
    foreachpair (const string& name, Hook* hook, availableHooks) {
      const Result<Environment> result =
        hook->slaveExecutorEnvironmentDecorator(executorInfo);

      // NOTE: If the hook returns None(), the environment won't be
      // changed.
      if (result.isSome()) {
        executorInfo.mutable_command()->mutable_environment()->CopyFrom(
            result.get());
      } else if (result.isError()) {
        LOG(WARNING) << "Slave environment decorator hook failed for module '"
                    << name << "': " << result.error();
      }
    }

    return executorInfo.command().environment();
  }
}


void HookManager::slavePreLaunchDockerHook(
    const ContainerInfo& containerInfo,
    const CommandInfo& commandInfo,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& name,
    const string& sandboxDirectory,
    const string& mappedDirectory,
    const Option<Resources>& resources,
    const Option<map<string, string>>& env)
{
  foreachpair (const string& name, Hook* hook, availableHooks) {
    Try<Nothing> result =
      hook->slavePreLaunchDockerHook(
          containerInfo,
          commandInfo,
          taskInfo,
          executorInfo,
          name,
          sandboxDirectory,
          mappedDirectory,
          resources,
          env);
    if (result.isError()) {
      LOG(WARNING) << "Slave pre launch docker hook failed for module '"
                   << name << "': " << result.error();
    }
  }
}


void HookManager::slaveRemoveExecutorHook(
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo)
{
  foreachpair (const string& name, Hook* hook, availableHooks) {
    const Try<Nothing> result =
      hook->slaveRemoveExecutorHook(frameworkInfo, executorInfo);
    if (result.isError()) {
      LOG(WARNING) << "Slave remove executor hook failed for module '"
                   << name << "': " << result.error();
    }
  }
}


TaskStatus HookManager::slaveTaskStatusDecorator(
    const FrameworkID& frameworkId,
    TaskStatus status)
{
  synchronized (mutex) {
    foreachpair (const string& name, Hook* hook, availableHooks) {
      const Result<TaskStatus> result =
        hook->slaveTaskStatusDecorator(frameworkId, status);

      // NOTE: Labels/ContainerStatus remain unchanged if the hook returns
      // None().
      if (result.isSome()) {
        if (result.get().has_labels()) {
          status.mutable_labels()->CopyFrom(result.get().labels());
        }

        if (result.get().has_container_status()) {
          status.mutable_container_status()->CopyFrom(
              result.get().container_status());
        }
      } else if (result.isError()) {
        LOG(WARNING) << "Slave TaskStatus decorator hook failed for "
                     << "module '" << name << "': " << result.error();
      }
    }

    return status;
  }
}

} // namespace internal {
} // namespace mesos {
