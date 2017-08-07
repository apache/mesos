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

#include "checks/health_checker.hpp"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <iostream>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/agent/agent.hpp>

#include <process/collect.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/killtree.hpp>

#include "checks/checker_process.hpp"

#include "common/http.hpp"
#include "common/status_utils.hpp"
#include "common/validation.hpp"

#include "internal/evolve.hpp"

#ifdef __linux__
#include "linux/ns.hpp"
#endif

using process::delay;
using process::dispatch;
using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::Promise;
using process::Subprocess;
using process::Time;

using process::http::Connection;
using process::http::Response;

using std::map;
using std::shared_ptr;
using std::string;
using std::tuple;
using std::vector;

namespace mesos {
namespace internal {
namespace checks {

static CheckInfo toCheckInfo(const HealthCheck& healthCheck)
{
  CheckInfo check;

  check.set_delay_seconds(healthCheck.delay_seconds());
  check.set_interval_seconds(healthCheck.interval_seconds());
  check.set_timeout_seconds(healthCheck.timeout_seconds());

  switch (healthCheck.type()) {
    case HealthCheck::COMMAND: {
      check.set_type(CheckInfo::COMMAND);

      check.mutable_command()->mutable_command()->CopyFrom(
          healthCheck.command());

      break;
    }
    case HealthCheck::HTTP: {
      check.set_type(CheckInfo::HTTP);

      CheckInfo::Http* http = check.mutable_http();
      http->set_port(healthCheck.http().port());
      http->set_path(healthCheck.http().path());

      break;
    }
    case HealthCheck::TCP: {
      check.set_type(CheckInfo::TCP);

      check.mutable_tcp()->set_port(healthCheck.tcp().port());

      break;
    }
    case HealthCheck::UNKNOWN: {
      check.set_type(CheckInfo::UNKNOWN);

      break;
    }
  }

  return check;
}


static Try<Nothing> interpretCheckStatusInfo(const CheckStatusInfo& result)
{
  switch (result.type()) {
    case CheckInfo::COMMAND: {
      const int exitCode = result.command().exit_code();
      if (exitCode != 0) {
        return Error("Command " + WSTRINGIFY(exitCode));
      }

      break;
    }
    case CheckInfo::HTTP: {
      const int statusCode = result.http().status_code();
      if (statusCode < process::http::Status::OK ||
          statusCode >= process::http::Status::BAD_REQUEST) {
        return Error(
            "Unexpected HTTP response code: " +
            process::http::Status::string(statusCode));
      }

      break;
    }
    case CheckInfo::TCP: {
      if (!result.tcp().succeeded()) {
        return Error("TCP connection failed");
      }

      break;
    }
    case CheckInfo::UNKNOWN: {
      break;
    }
  }

  return Nothing();
}


Try<Owned<HealthChecker>> HealthChecker::create(
    const HealthCheck& healthCheck,
    const string& launcherDir,
    const lambda::function<void(const TaskHealthStatus&)>& callback,
    const TaskID& taskId,
    const Option<pid_t>& taskPid,
    const vector<string>& namespaces)
{
  // Validate the 'HealthCheck' protobuf.
  Option<Error> error = validation::healthCheck(healthCheck);
  if (error.isSome()) {
    return error.get();
  }

  return Owned<HealthChecker>(
      new HealthChecker(
          healthCheck,
          taskId,
          callback,
          launcherDir,
          taskPid,
          namespaces,
          None(),
          None(),
          None(),
          false));
}


Try<Owned<HealthChecker>> HealthChecker::create(
    const HealthCheck& healthCheck,
    const string& launcherDir,
    const lambda::function<void(const TaskHealthStatus&)>& callback,
    const TaskID& taskId,
    const ContainerID& taskContainerId,
    const process::http::URL& agentURL,
    const Option<string>& authorizationHeader)
{
  // Validate the 'HealthCheck' protobuf.
  Option<Error> error = validation::healthCheck(healthCheck);
  if (error.isSome()) {
    return error.get();
  }

  return Owned<HealthChecker>(
      new HealthChecker(
          healthCheck,
          taskId,
          callback,
          launcherDir,
          None(),
          {},
          taskContainerId,
          agentURL,
          authorizationHeader,
          true));
}


HealthChecker::HealthChecker(
      const HealthCheck& _healthCheck,
      const TaskID& _taskId,
      const lambda::function<void(const TaskHealthStatus&)>& _callback,
      const std::string& launcherDir,
      const Option<pid_t>& taskPid,
      const std::vector<std::string>& namespaces,
      const Option<ContainerID>& taskContainerId,
      const Option<process::http::URL>& agentURL,
      const Option<std::string>& authorizationHeader,
      bool commandCheckViaAgent)
  : healthCheck(_healthCheck),
    callback(_callback),
    name(HealthCheck::Type_Name(healthCheck.type()) + " health check"),
    startTime(Clock::now()),
    taskId(_taskId),
    consecutiveFailures(0),
    initializing(true)
{
  VLOG(1) << "Health check configuration for task '" << taskId << "':"
          << " '" << jsonify(JSON::Protobuf(healthCheck)) << "'";

  Try<Duration> create = Duration::create(healthCheck.grace_period_seconds());
  CHECK_SOME(create);
  checkGracePeriod = create.get();

  Option<string> scheme;
  if (healthCheck.type() == HealthCheck::HTTP &&
      healthCheck.http().has_scheme()) {
    scheme = healthCheck.http().scheme();
  }

  process.reset(
      new CheckerProcess(
          toCheckInfo(_healthCheck),
          launcherDir,
          std::bind(&HealthChecker::processCheckResult, this, lambda::_1),
          _taskId,
          taskPid,
          namespaces,
          taskContainerId,
          agentURL,
          authorizationHeader,
          scheme,
          name,
          commandCheckViaAgent));

  spawn(process.get());
}


HealthChecker::~HealthChecker()
{
  terminate(process.get());
  wait(process.get());
}


void HealthChecker::pause()
{
  dispatch(process.get(), &CheckerProcess::pause);
}


void HealthChecker::resume()
{
  dispatch(process.get(), &CheckerProcess::resume);
}


void HealthChecker::processCheckResult(const Try<CheckStatusInfo>& result)
{
  if (result.isError()) {
    // The error is with the underlying check.
    LOG(WARNING) << name << " for task '" << taskId << "'" << " failed: "
                 << result.error();

    failure();
    return;
  }

  Try<Nothing> healthCheckResult = interpretCheckStatusInfo(result.get());
  if (healthCheckResult.isError()) {
    // The underlying check succeeded, but its result is interpreted as failure.
    LOG(WARNING) << name << " for task '" << taskId << "'" << " failed: "
                 << healthCheckResult.error();

    failure();
    return;
  }

  success();
}


void HealthChecker::failure()
{
  if (initializing &&
      checkGracePeriod.secs() > 0 &&
      (Clock::now() - startTime) <= checkGracePeriod) {
    LOG(INFO) << "Ignoring failure of " << name << " for task '" << taskId
              << "': still in grace period";
    return;
  }

  consecutiveFailures++;
  LOG(WARNING) << name << " for task '" << taskId << "' failed "
               << consecutiveFailures << " times consecutively";

  bool killTask = consecutiveFailures >= healthCheck.consecutive_failures();

  TaskHealthStatus taskHealthStatus;
  taskHealthStatus.set_healthy(false);
  taskHealthStatus.set_consecutive_failures(consecutiveFailures);
  taskHealthStatus.set_kill_task(killTask);
  taskHealthStatus.mutable_task_id()->CopyFrom(taskId);

  // We assume this is a local send, i.e. the health checker library
  // is not used in a binary external to the executor and hence can
  // not exit before the data is sent to the executor.
  callback(taskHealthStatus);
}


void HealthChecker::success()
{
  VLOG(1) << name << " for task '" << taskId << "' passed";

  // Send a healthy status update on the first success,
  // and on the first success following failure(s).
  if (initializing || consecutiveFailures > 0) {
    TaskHealthStatus taskHealthStatus;
    taskHealthStatus.set_healthy(true);
    taskHealthStatus.mutable_task_id()->CopyFrom(taskId);
    callback(taskHealthStatus);
    initializing = false;
  }

  consecutiveFailures = 0;
}


namespace validation {

Option<Error> healthCheck(const HealthCheck& healthCheck)
{
  if (!healthCheck.has_type()) {
    return Error("HealthCheck must specify 'type'");
  }

  switch (healthCheck.type()) {
    case HealthCheck::COMMAND: {
      if (!healthCheck.has_command()) {
        return Error("Expecting 'command' to be set for COMMAND health check");
      }

      const CommandInfo& command = healthCheck.command();

      if (!command.has_value()) {
        string commandType =
          (command.shell() ? "'shell command'" : "'executable path'");

        return Error("Command health check must contain " + commandType);
      }

      Option<Error> error =
        common::validation::validateCommandInfo(command);
      if (error.isSome()) {
        return Error(
            "Health check's `CommandInfo` is invalid: " + error->message);
      }

      // TODO(alexr): Make sure irrelevant fields, e.g., `uris` are not set.

      break;
    }
    case HealthCheck::HTTP: {
      if (!healthCheck.has_http()) {
        return Error("Expecting 'http' to be set for HTTP health check");
      }

      const HealthCheck::HTTPCheckInfo& http = healthCheck.http();

      if (http.has_scheme() &&
          http.scheme() != "http" &&
          http.scheme() != "https") {
        return Error(
            "Unsupported HTTP health check scheme: '" + http.scheme() + "'");
      }

      if (http.has_path() && !strings::startsWith(http.path(), '/')) {
        return Error(
            "The path '" + http.path() +
            "' of HTTP health check must start with '/'");
      }

      break;
    }
    case HealthCheck::TCP: {
      if (!healthCheck.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP health check");
      }

      break;
    }
    case HealthCheck::UNKNOWN: {
      return Error(
          "'" + HealthCheck::Type_Name(healthCheck.type()) + "'"
          " is not a valid health check type");
    }
  }

  if (healthCheck.has_delay_seconds() && healthCheck.delay_seconds() < 0.0) {
    return Error("Expecting 'delay_seconds' to be non-negative");
  }

  if (healthCheck.has_grace_period_seconds() &&
      healthCheck.grace_period_seconds() < 0.0) {
    return Error("Expecting 'grace_period_seconds' to be non-negative");
  }

  if (healthCheck.has_interval_seconds() &&
      healthCheck.interval_seconds() < 0.0) {
    return Error("Expecting 'interval_seconds' to be non-negative");
  }

  if (healthCheck.has_timeout_seconds() &&
      healthCheck.timeout_seconds() < 0.0) {
    return Error("Expecting 'timeout_seconds' to be non-negative");
  }

  return None();
}

} // namespace validation {

} // namespace checks {
} // namespace internal {
} // namespace mesos {
