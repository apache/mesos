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

namespace mesos {
namespace internal {
namespace checks {

// Forward declarations.
class CheckerProcess;

class Checker
{
public:
  /**
   * Attempts to create a `Checker` object. In case of success, checking
   * starts immediately after initialization.
   *
   * If the check is a COMMAND check, the checker will fork a process, enter
   * the task's namespaces, and execute the commmand.
   *
   * @param check The protobuf message definition of a check.
   * @param launcherDir A directory where Mesos helper binaries are located.
   *     Executor must have access to this directory for TCP checks.
   * @param callback A callback `Checker` uses to send check status updates
   *     to its owner (usually an executor).
   * @param taskId The TaskID of the target task.
   * @param taskPid The target task's pid used to enter the specified
   *     namespaces.
   * @param namespaces The namespaces to enter prior to performing the check.
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
      const Option<pid_t>& taskPid,
      const std::vector<std::string>& namespaces);

  /**
   * Attempts to create a `Checker` object. In case of success, checking
   * starts immediately after initialization.
   *
   * If the check is a COMMAND check, the checker will delegate the execution
   * of the check to the Mesos agent via the `LaunchNestedContainerSession`
   * API call.
   *
   * @param check The protobuf message definition of a check.
   * @param launcherDir A directory where Mesos helper binaries are located.
   *     Executor must have access to this directory for TCP checks.
   * @param callback A callback `Checker` uses to send check status updates
   *     to its owner (usually an executor).
   * @param taskId The TaskID of the target task.
   * @param taskContainerId The ContainerID of the target task.
   * @param agentURL The URL of the agent.
   * @param authorizationHeader The authorization header the checker should use
   *     to authenticate with the agent operator API.
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
      const ContainerID& taskContainerId,
      const process::http::URL& agentURL,
      const Option<std::string>& authorizationHeader);

  ~Checker();

  // Not copyable, not assignable.
  Checker(const Checker&) = delete;
  Checker& operator=(const Checker&) = delete;

  // Idempotent helpers for pausing and resuming checking.
  void pause();
  void resume();

private:
  explicit Checker(process::Owned<CheckerProcess> process);

  process::Owned<CheckerProcess> process;
};

namespace validation {

// TODO(alexr): A better place for these functions would be something like
// "mesos_validation.cpp", since they validate API protobufs which are not
// solely related to this library.
Option<Error> checkInfo(const CheckInfo& checkInfo);
Option<Error> checkStatusInfo(const CheckStatusInfo& checkStatusInfo);

} // namespace validation {

} // namespace checks {
} // namespace internal {
} // namespace mesos {

#endif // __CHECKER_HPP__
