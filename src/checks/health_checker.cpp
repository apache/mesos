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
#include <stout/variant.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/killtree.hpp>

#include "checks/checker_process.hpp"
#include "checks/checks_runtime.hpp"
#include "checks/checks_types.hpp"

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
    Variant<runtime::Plain, runtime::Docker, runtime::Nested> runtime)
{
  // Validate the 'HealthCheck' protobuf.
  Option<Error> error = common::validation::validateHealthCheck(healthCheck);
  if (error.isSome()) {
    return error.get();
  }

  return Owned<HealthChecker>(
      new HealthChecker(
          healthCheck,
          launcherDir,
          callback,
          taskId,
          std::move(runtime)));
}


HealthChecker::HealthChecker(
      const HealthCheck& _healthCheck,
      const string& launcherDir,
      const lambda::function<void(const TaskHealthStatus&)>& _callback,
      const TaskID& _taskId,
      Variant<runtime::Plain, runtime::Docker, runtime::Nested> runtime)
  : healthCheck(_healthCheck),
    callback(_callback),
    taskId(_taskId),
    name(HealthCheck::Type_Name(healthCheck.type()) + " health check"),
    startTime(Clock::now()),
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

  bool ipv6 = false;
  if ((healthCheck.type() == HealthCheck::HTTP &&
       healthCheck.http().protocol() == NetworkInfo::IPv6) ||
      (healthCheck.type() == HealthCheck::TCP &&
       healthCheck.tcp().protocol() == NetworkInfo::IPv6)) {
    ipv6 = true;
  }

  process.reset(
      new CheckerProcess(
          toCheckInfo(_healthCheck),
          launcherDir,
          std::bind(&HealthChecker::processCheckResult, this, lambda::_1),
          _taskId,
          name,
          std::move(runtime),
          scheme,
          ipv6));

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

} // namespace checks {
} // namespace internal {
} // namespace mesos {
