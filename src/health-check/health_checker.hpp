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

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/time.hpp>

#include <stout/nothing.hpp>

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace health {

// Forward declarations.
class HealthCheckerProcess;

class HealthChecker
{
public:
  static Try<process::Owned<HealthChecker>> create(
      const HealthCheck& check,
      const process::UPID& executor,
      const TaskID& taskID);

  ~HealthChecker();

  process::Future<Nothing> healthCheck();

private:
  explicit HealthChecker(process::Owned<HealthCheckerProcess> process);

  process::Owned<HealthCheckerProcess> process;
};


class HealthCheckerProcess : public ProtobufProcess<HealthCheckerProcess>
{
public:
  HealthCheckerProcess(
      const HealthCheck& _check,
      const process::UPID& _executor,
      const TaskID& _taskID);

  virtual ~HealthCheckerProcess() {}

  process::Future<Nothing> healthCheck();

private:
  void failure(const std::string& message);
  void success();

  void _healthCheck();

  void _commandHealthCheck();
  void _httpHealthCheck();
  void _tcpHealthCheck();

  void reschedule();

  process::Promise<Nothing> promise;
  HealthCheck check;
  bool initializing;
  process::UPID executor;
  TaskID taskID;
  uint32_t consecutiveFailures;
  process::Time startTime;
};


namespace validation {

Option<Error> healthCheck(const HealthCheck& check);

} // namespace validation {

} // namespace health {
} // namespace internal {
} // namespace mesos {

#endif // __HEALTH_CHECKER_HPP__
