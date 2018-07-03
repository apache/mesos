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

#ifndef __CHECKER_HPP__
#define __CHECKER_HPP__

#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/variant.hpp>

#include "checks/checks_runtime.hpp"
#include "checks/checks_types.hpp"

namespace mesos {
namespace internal {
namespace checks {

class CheckerProcess;


class Checker
{
public:
  /**
   * Attempts to create a `Checker` object. In case of success, checking
   * starts immediately after initialization.
   *
   * The check performed is based off the check type and the given runtime.
   *
   * @param check The protobuf message definition of a check.
   * @param launcherDir A directory where Mesos helper binaries are located.
   *     Executor must have access to this directory for TCP checks.
   * @param callback A callback `Checker` uses to send check status updates
   *     to its owner (usually an executor).
   * @param taskId The TaskID of the target task.
   * @param runtime The runtime that launched the task.
   * @return A `Checker` object or an error if `create` fails.
   *
   * @todo A better approach would be to return a stream of updates, e.g.,
   * `process::Stream<CheckStatusInfo>` rather than invoking a callback.
   */
  static Try<process::Owned<Checker>> create(
      const CheckInfo& check,
      const std::string& launcherDir,
      const lambda::function<void(const CheckStatusInfo&)>& callback,
      const TaskID& taskId,
      Variant<runtime::Plain, runtime::Docker, runtime::Nested> runtime);

  ~Checker();

  // Not copyable, not assignable.
  Checker(const Checker&) = delete;
  Checker& operator=(const Checker&) = delete;

  // Idempotent helpers for pausing and resuming checking.
  void pause();
  void resume();

private:
  Checker(
      const CheckInfo& _check,
      const std::string& _launcherDir,
      const lambda::function<void(const CheckStatusInfo&)>& _callback,
      const TaskID& _taskId,
      Variant<runtime::Plain, runtime::Docker, runtime::Nested> _runtime);

  void processCheckResult(const Try<CheckStatusInfo>& result);

  const CheckInfo check;
  const lambda::function<void(const CheckStatusInfo&)> callback;
  const TaskID taskId;
  const std::string name;

  CheckStatusInfo previousCheckStatus;
  process::Owned<CheckerProcess> process;
};

} // namespace checks {
} // namespace internal {
} // namespace mesos {

#endif // __CHECKER_HPP__
