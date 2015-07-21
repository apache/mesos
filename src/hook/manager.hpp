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

#ifndef __HOOK_MANAGER_HPP__
#define __HOOK_MANAGER_HPP__

#include <string>

#include <mesos/mesos.hpp>
#include <mesos/hook.hpp>

#include <stout/try.hpp>

namespace mesos {
namespace internal {

class HookManager
{
public:
  static Try<Nothing> initialize(const std::string& hookList);

  // Exposed just for testing so that we can unload a given
  // hook and remove it from the list of available hooks.
  static Try<Nothing> unload(const std::string& hookName);

  static bool hooksAvailable();

  static Labels masterLaunchTaskLabelDecorator(
      const TaskInfo& taskInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo);

  static Labels slaveRunTaskLabelDecorator(
      const TaskInfo& taskInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo);

  static Environment slaveExecutorEnvironmentDecorator(
      ExecutorInfo executorInfo);

  static void slaveRemoveExecutorHook(
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo);

  static Labels slaveTaskStatusLabelDecorator(
      const FrameworkID& frameworkId,
      TaskStatus status);
};

} // namespace internal {
} // namespace mesos {

#endif // __HOOK_MANAGER_HPP__
