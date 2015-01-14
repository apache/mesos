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

#ifndef __HOOK_HOOK_HPP__
#define __HOOK_HOOK_HPP__

#include <mesos/mesos.hpp>

#include <stout/result.hpp>

#include "master/master.hpp"

namespace mesos {
namespace internal {

class Hook
{
public:
  virtual ~Hook() {};

  // This label decorator hook is called from within master during
  // the launchTask routine.  A module implementing the hook creates
  // and returns a set of labels.  These labels are then merged with
  // the task labels and passed on to the slave/executor.
  virtual Result<Labels> masterLaunchTaskLabelDecorator(
      const TaskInfo& taskInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo)
  {
    return None();
  }
};

} // namespace internal {
} // namespace mesos {

#endif // __HOOK_HOOK_HPP__
