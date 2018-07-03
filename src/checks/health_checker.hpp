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

#ifndef __HEALTH_CHECKER_HPP__
#define __HEALTH_CHECKER_HPP__

#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/variant.hpp>

#include "checks/checks_runtime.hpp"
#include "checks/checks_types.hpp"

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace checks {

class CheckerProcess;


class HealthChecker
{
public:
  /**
   * Attempts to create a `HealthChecker` object. In case of success, health
   * checking starts immediately after initialization.
   *
   * The check performed is based off the check type and the given runtime.
   *
   * @param healthCheck The protobuf message definition of health check.
   * @param launcherDir A directory where Mesos helper binaries are located.
   * @param callback A callback HealthChecker uses to send health status
   *     updates to its owner (usually an executor).
   * @param taskId The TaskID of the target task.
   * @param runtime The runtime that launched the task.
   * @return A `HealthChecker` object or an error if `create` fails.
   *
   * @todo A better approach would be to return a stream of updates, e.g.,
   * `process::Stream<TaskHealthStatus>` rather than invoking a callback.
   *
   * @todo Consider leveraging `checks::Checker` for checking functionality.
   * This class will then focus on interpreting and acting on the result.
   */
  static Try<process::Owned<HealthChecker>> create(
      const HealthCheck& healthCheck,
      const std::string& launcherDir,
      const lambda::function<void(const TaskHealthStatus&)>& callback,
      const TaskID& taskId,
      Variant<runtime::Plain, runtime::Docker, runtime::Nested> runtime);

  ~HealthChecker();

  // Idempotent helpers for pausing and resuming health checking.
  void pause();
  void resume();

private:
  HealthChecker(
      const HealthCheck& _healthCheck,
      const std::string& launcherDir,
      const lambda::function<void(const TaskHealthStatus&)>& _callback,
      const TaskID& _taskId,
      Variant<runtime::Plain, runtime::Docker, runtime::Nested> runtime);

  void processCheckResult(const Try<CheckStatusInfo>& result);
  void failure();
  void success();

  const HealthCheck healthCheck;
  const lambda::function<void(const TaskHealthStatus&)> callback;
  const TaskID taskId;

  const std::string name;
  const process::Time startTime;

  Duration checkGracePeriod;
  uint32_t consecutiveFailures;
  bool initializing;

  process::Owned<CheckerProcess> process;
};

} // namespace checks {
} // namespace internal {
} // namespace mesos {

#endif // __HEALTH_CHECKER_HPP__
