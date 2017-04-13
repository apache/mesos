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

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/stopwatch.hpp>

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace checks {

// Forward declarations.
class HealthCheckerProcess;

class HealthChecker
{
public:
  /**
   * Attempts to create a `HealthChecker` object. In case of success, health
   * checking starts immediately after initialization.
   *
   * If the check is a command health check, the checker will fork a process,
   * enter the task's namespaces, and execute the commmand.
   *
   * @param check The protobuf message definition of health check.
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
      const HealthCheck& check,
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
   * @param check The protobuf message definition of health check.
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
      const HealthCheck& check,
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
  explicit HealthChecker(process::Owned<HealthCheckerProcess> process);

  process::Owned<HealthCheckerProcess> process;
};


class HealthCheckerProcess : public ProtobufProcess<HealthCheckerProcess>
{
public:
  HealthCheckerProcess(
      const HealthCheck& _check,
      const std::string& _launcherDir,
      const lambda::function<void(const TaskHealthStatus&)>& _callback,
      const TaskID& _taskId,
      const Option<pid_t>& _taskPid,
      const std::vector<std::string>& _namespaces,
      const Option<ContainerID>& _taskContainerId,
      const Option<process::http::URL>& _agentURL,
      const Option<std::string>& authorizationHeader,
      bool _commandCheckViaAgent);

  virtual ~HealthCheckerProcess() {}

  void pause();
  void resume();

protected:
  virtual void initialize() override;

private:
  void failure(const std::string& message);
  void success();

  void performSingleCheck();
  void processCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<Nothing>& future);

  process::Future<Nothing> commandHealthCheck();

  process::Future<Nothing> nestedCommandHealthCheck();

  void _nestedCommandHealthCheck(
      std::shared_ptr<process::Promise<Nothing>> promise);

  void __nestedCommandHealthCheck(
      std::shared_ptr<process::Promise<Nothing>> promise,
      process::http::Connection connection);

  void ___nestedCommandHealthCheck(
      std::shared_ptr<process::Promise<Nothing>> promise,
      const ContainerID& checkContainerId,
      const process::http::Response& launchResponse);

  void nestedCommandHealthCheckFailure(
      std::shared_ptr<process::Promise<Nothing>> promise,
      process::http::Connection connection,
      ContainerID checkContainerId,
      std::shared_ptr<bool> checkTimedOut,
      const std::string& failure);

  /**
   * Waits for a container to be terminated.
   *
   * Waits for a container to be terminated via the Agent's
   * `WaitNestedContainer` API call.
   *
   * @param containerID The `ContainerID` of the container that we want
   * to wait for.
   *
   * @return The exit status as returned by the Agent.
   */
  process::Future<Option<int>> waitNestedContainer(
      const ContainerID& containerId);

  process::Future<Option<int>> _waitNestedContainer(
      const ContainerID& containerId,
      const process::http::Response& httpResponse);

  process::Future<Nothing> httpHealthCheck();

  process::Future<Nothing> _httpHealthCheck(
      const std::tuple<
          process::Future<Option<int>>,
          process::Future<std::string>,
          process::Future<std::string>>& t);

  process::Future<Nothing> tcpHealthCheck();

  process::Future<Nothing> _tcpHealthCheck(
      const std::tuple<
          process::Future<Option<int>>,
          process::Future<std::string>,
          process::Future<std::string>>& t);

  void scheduleNext(const Duration& duration);

  HealthCheck check;
  Duration checkDelay;
  Duration checkInterval;
  Duration checkGracePeriod;
  Duration checkTimeout;

  // Contains a binary for TCP health checks.
  const std::string launcherDir;

  const lambda::function<void(const TaskHealthStatus&)> healthUpdateCallback;
  const TaskID taskId;
  const Option<pid_t> taskPid;
  const std::vector<std::string> namespaces;
  const Option<ContainerID> taskContainerId;
  const Option<process::http::URL> agentURL;
  const Option<std::string> authorizationHeader;
  const bool commandCheckViaAgent;

  Option<lambda::function<pid_t(const lambda::function<int()>&)>> clone;

  uint32_t consecutiveFailures;
  process::Time startTime;
  bool initializing;
  bool paused;

  // Contains the ID of the most recently terminated nested container
  // that was used to perform a COMMAND health check.
  Option<ContainerID> previousCheckContainerId;
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
