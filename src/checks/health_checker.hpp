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

#include "checker_process.hpp"

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace checks {

class HealthChecker
{
public:
  /**
   * Attempts to create a `HealthChecker` object. In case of success, health
   * checking starts immediately after initialization.
   *
   * If the check is a command health check, the checker will fork a process,
   * enter the task's namespaces, and execute the command.
   *
   * @param healthCheck The protobuf message definition of health check.
   * @param launcherDir A directory where Mesos helper binaries are located.
   * @param callback A callback HealthChecker uses to send health status
   *     updates to its owner (usually an executor).
   * @param taskId The TaskID of the target task.
   * @param taskPid The target task's pid used to enter the specified
   *     namespaces.
   * @param namespaces The namespaces to enter prior to performing the health
   *     check.
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
      const Option<pid_t>& taskPid,
      const std::vector<std::string>& namespaces);

  /**
   * Attempts to create a `HealthChecker` object. In case of success, health
   * checking starts immediately after initialization.
   *
   * If the check is a command health check, the checker will delegate the
   * execution of the check to the Mesos agent via the
   * `LaunchNestedContainerSession` API call.
   *
   * @param healthCheck The protobuf message definition of health check.
   * @param launcherDir A directory where Mesos helper binaries are located.
   * @param callback A callback HealthChecker uses to send health status
   *     updates to its owner (usually an executor).
   * @param taskId The TaskID of the target task.
   * @param taskContainerId The ContainerID of the target task.
   * @param agentURL The URL of the agent.
   * @param authorizationHeader The authorization header the health checker
   *     should use to authenticate with the agent operator API.
   * @return A `HealthChecker` object or an error if `create` fails.
   *
   * @todo A better approach would be to return a stream of updates, e.g.,
   * `process::Stream<TaskHealthStatus>` rather than invoking a callback.
   */
  static Try<process::Owned<HealthChecker>> create(
      const HealthCheck& healthCheck,
      const std::string& launcherDir,
      const lambda::function<void(const TaskHealthStatus&)>& callback,
      const TaskID& taskId,
      const ContainerID& taskContainerId,
      const process::http::URL& agentURL,
      const Option<std::string>& authorizationHeader);


  ~HealthChecker();

  // Idempotent helpers for pausing and resuming health checking.
  void pause();
  void resume();

private:
  HealthChecker(
      const HealthCheck& _healthCheck,
      const TaskID& _taskId,
      const lambda::function<void(const TaskHealthStatus&)>& _callback,
      const std::string& launcherDir,
      const Option<pid_t>& taskPid,
      const std::vector<std::string>& namespaces,
      const Option<ContainerID>& taskContainerId,
      const Option<process::http::URL>& agentURL,
      const Option<std::string>& authorizationHeader,
      bool commandCheckViaAgent);

  void processCheckResult(const Try<CheckStatusInfo>& result);
  void failure();
  void success();

  const HealthCheck healthCheck;
  const lambda::function<void(const TaskHealthStatus&)> callback;
  const std::string name;
  const process::Time startTime;
  const TaskID taskId;

  Duration checkGracePeriod;
  uint32_t consecutiveFailures;
  bool initializing;

  process::Owned<CheckerProcess> process;
};


namespace validation {

// TODO(alexr): A better place for this function would be something like
// "mesos_validation.cpp", since it validates API protobuf which is not
// solely related to the health checking library.
Option<Error> healthCheck(const HealthCheck& check);

} // namespace validation {

} // namespace checks {
} // namespace internal {
} // namespace mesos {

#endif // __HEALTH_CHECKER_HPP__
