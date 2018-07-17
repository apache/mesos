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

#ifndef __CHECKER_PROCESS_HPP__
#define __CHECKER_PROCESS_HPP__

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/stopwatch.hpp>
#include <stout/try.hpp>
#include <stout/variant.hpp>

#include "checks/checks_runtime.hpp"
#include "checks/checks_types.hpp"

namespace mesos {
namespace internal {
namespace checks {

#ifdef __WINDOWS__
constexpr char DOCKER_HEALTH_CHECK_IMAGE[] = "mesos/windows-health-check";
#endif // __WINDOWS__

class CheckerProcess : public ProtobufProcess<CheckerProcess>
{
public:
  // TODO(gkleiman): Instead of passing an optional scheme as a parameter,
  // consider introducing a global `TLSInfo` protobuf and using it in HTTP
  // checks. See MESOS-7356.
  //
  // TODO(qianzhang): Once we figure out how the IPv4/IPv6 should be supported
  // in the health check API (i.e., the `CheckInfo` protobuf message), we may
  // consider to remove the ipv6 parameter field which is a temporary solution
  // for now.
  CheckerProcess(
      const CheckInfo& checkInfo,
      const std::string& launcherDir,
      const lambda::function<void(const Try<CheckStatusInfo>&)>& _callback,
      const TaskID& _taskId,
      const std::string& _name,
      Variant<runtime::Plain, runtime::Docker, runtime::Nested> _runtime,
      const Option<std::string>& scheme,
      bool ipv6 = false);

  void pause();
  void resume();

  ~CheckerProcess() override {}

protected:
  void initialize() override;
  void finalize() override;

private:
  void performCheck();
  void scheduleNext(const Duration& duration);
  void processCheckResult(
      const Stopwatch& stopwatch,
      const Result<CheckStatusInfo>& result);

  process::Future<int> commandCheck(
      const check::Command& cmd,
      const runtime::Plain& plain);

  process::Future<int> dockerCommandCheck(
      const check::Command& cmd,
      const runtime::Docker& docker);

  process::Future<int> nestedCommandCheck(
      const check::Command& cmd,
      const runtime::Nested& nested);
  void _nestedCommandCheck(
      std::shared_ptr<process::Promise<int>> promise,
      check::Command cmd,
      runtime::Nested nested);
  void __nestedCommandCheck(
      std::shared_ptr<process::Promise<int>> promise,
      process::http::Connection connection,
      check::Command cmd,
      runtime::Nested nested);
  void ___nestedCommandCheck(
      std::shared_ptr<process::Promise<int>> promise,
      const ContainerID& checkContainerId,
      const process::http::Response& launchResponse,
      runtime::Nested nested);

  void nestedCommandCheckFailure(
      std::shared_ptr<process::Promise<int>> promise,
      process::http::Connection connection,
      const ContainerID& checkContainerId,
      std::shared_ptr<bool> checkTimedOut,
      const std::string& failure,
      runtime::Nested nested);

  process::Future<Option<int>> waitNestedContainer(
      const ContainerID& containerId,
      runtime::Nested nested);
  process::Future<Option<int>> _waitNestedContainer(
      const ContainerID& containerId,
      const process::http::Response& httpResponse);

  void processCommandCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<int>& future);

  process::Future<int> httpCheck(
      const check::Http& http,
      const Option<runtime::Plain>& plain);
  process::Future<int> _httpCheck(
      const std::vector<std::string>& cmdArgv,
      const Option<runtime::Plain>& plain);
  process::Future<int> __httpCheck(
      const std::tuple<process::Future<Option<int>>,
                       process::Future<std::string>,
                       process::Future<std::string>>& t);
  void processHttpCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<int>& future);

  // The docker HTTP health check is only performed differently on Windows.
#ifdef __WINDOWS__
  process::Future<int> dockerHttpCheck(
      const check::Http& http,
      const runtime::Docker& docker);
#endif // __WINDOWS__

  process::Future<bool> tcpCheck(
      const check::Tcp& tcp,
      const Option<runtime::Plain>& plain);
  process::Future<bool> _tcpCheck(
      const std::vector<std::string>& cmdArgv,
      const Option<runtime::Plain>& plain);
  process::Future<bool> __tcpCheck(
      const std::tuple<process::Future<Option<int>>,
                       process::Future<std::string>,
                       process::Future<std::string>>& t);
  void processTcpCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<bool>& future);

  // The docker TCP health check is only performed differently on Windows.
#ifdef __WINDOWS__
  process::Future<bool> dockerTcpCheck(
      const check::Tcp& tcp,
      const runtime::Docker& docker);
#endif // __WINDOWS__

  const lambda::function<void(const Try<CheckStatusInfo>&)> updateCallback;
  const TaskID taskId;
  const std::string name;
  const Variant<runtime::Plain, runtime::Docker,  runtime::Nested> runtime;

  // The `CheckerProcess` constructor parses the given `CheckInfo` struct and
  // prepares this `Variant`.
  const Variant<check::Command, check::Http, check::Tcp> check;

  Duration checkDelay;
  Duration checkInterval;
  Duration checkTimeout;

  bool paused;

  // Contains the ID of the most recently terminated nested container
  // that was used to perform a COMMAND check.
  Option<ContainerID> previousCheckContainerId;
};

} // namespace checks {
} // namespace internal {
} // namespace mesos {

#endif // __CHECKER_PROCESS_HPP__
