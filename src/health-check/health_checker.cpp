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

#include "health-check/health_checker.hpp"

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

#include <process/collect.hpp>
#include <process/delay.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/killtree.hpp>

#include "common/status_utils.hpp"

using process::delay;
using process::Clock;
using process::Failure;
using process::Future;
using process::NO_SETSID;
using process::Owned;
using process::Promise;
using process::Subprocess;
using process::Time;
using process::UPID;

using std::map;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace health {

Try<Owned<HealthChecker>> HealthChecker::create(
    const HealthCheck& check,
    const UPID& executor,
    const TaskID& taskID)
{
  // Validate the 'HealthCheck' protobuf.
  Option<Error> error = validation::healthCheck(check);
  if (error.isSome()) {
    return error.get();
  }

  Owned<HealthCheckerProcess> process(new HealthCheckerProcess(
      check,
      executor,
      taskID));

  return Owned<HealthChecker>(new HealthChecker(process));
}


HealthChecker::HealthChecker(
    Owned<HealthCheckerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


HealthChecker::~HealthChecker()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> HealthChecker::healthCheck()
{
  return dispatch(process.get(), &HealthCheckerProcess::healthCheck);
}


HealthCheckerProcess::HealthCheckerProcess(
    const HealthCheck& _check,
    const UPID& _executor,
    const TaskID& _taskID)
  : ProcessBase(process::ID::generate("health-checker")),
    check(_check),
    initializing(true),
    executor(_executor),
    taskID(_taskID),
    consecutiveFailures(0) {}


Future<Nothing> HealthCheckerProcess::healthCheck()
{
  VLOG(1) << "Health check starting in "
          << Seconds(check.delay_seconds()) << ", grace period "
          << Seconds(check.grace_period_seconds());

  startTime = Clock::now();

  delay(Seconds(check.delay_seconds()), self(), &Self::_healthCheck);
  return promise.future();
}


void HealthCheckerProcess::failure(const string& message)
{
  if (check.grace_period_seconds() > 0 &&
      (Clock::now() - startTime).secs() <= check.grace_period_seconds()) {
    LOG(INFO) << "Ignoring failure as health check still in grace period";
    reschedule();
    return;
  }

  consecutiveFailures++;
  LOG(WARNING) << "Health check failed " << consecutiveFailures
               << " times consecutively: " << message;

  bool killTask = consecutiveFailures >= check.consecutive_failures();

  TaskHealthStatus taskHealthStatus;
  taskHealthStatus.set_healthy(false);
  taskHealthStatus.set_consecutive_failures(consecutiveFailures);
  taskHealthStatus.set_kill_task(killTask);
  taskHealthStatus.mutable_task_id()->CopyFrom(taskID);
  send(executor, taskHealthStatus);

  if (killTask) {
    // This is a hack to ensure the message is sent to the
    // executor before we exit the process. Without this,
    // we may exit before libprocess has sent the data over
    // the socket. See MESOS-4111.
    os::sleep(Seconds(1));
    promise.fail(message);
  } else {
    reschedule();
  }
}


void HealthCheckerProcess::success()
{
  VLOG(1) << HealthCheck::Type_Name(check.type()) << " health check passed";

  // Send a healthy status update on the first success,
  // and on the first success following failure(s).
  if (initializing || consecutiveFailures > 0) {
    TaskHealthStatus taskHealthStatus;
    taskHealthStatus.set_healthy(true);
    taskHealthStatus.mutable_task_id()->CopyFrom(taskID);
    send(executor, taskHealthStatus);
    initializing = false;
  }

  consecutiveFailures = 0;
  reschedule();
}


void HealthCheckerProcess::_healthCheck()
{
  Future<Nothing> checkResult;

  switch (check.type()) {
    case HealthCheck::COMMAND: {
      checkResult = _commandHealthCheck();
      break;
    }

    case HealthCheck::HTTP: {
      checkResult = _httpHealthCheck();
      break;
    }

    case HealthCheck::TCP: {
      checkResult = _tcpHealthCheck();
      break;
    }

    default: {
      UNREACHABLE();
    }
  }

  checkResult.onAny(defer(self(), &Self::__healthCheck, lambda::_1));
}


void HealthCheckerProcess::__healthCheck(const Future<Nothing>& future)
{
  if (future.isReady()) {
    success();
    return;
  }

  string message = HealthCheck::Type_Name(check.type()) +
                   " health check failed: " +
                   (future.isFailed() ? future.failure() : "discarded");

  failure(message);
}


Future<Nothing> HealthCheckerProcess::_commandHealthCheck()
{
  CHECK_EQ(HealthCheck::COMMAND, check.type());
  CHECK(check.has_command());

  const CommandInfo& command = check.command();

  map<string, string> environment = os::environment();

  foreach (const Environment::Variable& variable,
           command.environment().variables()) {
    environment[variable.name()] = variable.value();
  }

  // Launch the subprocess.
  Try<Subprocess> external = Error("Not launched");

  if (command.shell()) {
    // Use the shell variant.
    VLOG(1) << "Launching command health check '" << command.value() << "'";

    external = subprocess(
        command.value(),
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        NO_SETSID,
        environment);
  } else {
    // Use the exec variant.
    vector<string> argv;
    foreach (const string& arg, command.arguments()) {
      argv.push_back(arg);
    }

    VLOG(1) << "Launching command health check [" << command.value() << ", "
            << strings::join(", ", argv) << "]";

    external = subprocess(
        command.value(),
        argv,
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        NO_SETSID,
        nullptr,
        environment);
  }

  if (external.isError()) {
    return Failure("Failed to create subprocess: " + external.error());
  }

  pid_t commandPid = external->pid();
  Duration timeout = Seconds(check.timeout_seconds());

  return external->status()
    .after(timeout, [timeout, commandPid](Future<Option<int>> future) {
      future.discard();

      if (commandPid != -1) {
        // Cleanup the external command process.
        VLOG(1) << "Killing the command health check process " << commandPid;

        os::killtree(commandPid, SIGKILL);
      }

      return Failure(
          "Command has not returned after " + stringify(timeout) +
          "; aborting");
    })
    .then([](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("Failed to reap the command process");
      }

      int statusCode = status.get();
      if (statusCode != 0) {
        return Failure("Command returned " + WSTRINGIFY(statusCode));
      }

      return Nothing();
    });
}


Future<Nothing> HealthCheckerProcess::_httpHealthCheck()
{
  CHECK_EQ(HealthCheck::HTTP, check.type());
  CHECK(check.has_http());

  promise.fail("HTTP health check is not supported");

  return Nothing();
}


Future<Nothing> HealthCheckerProcess::_tcpHealthCheck()
{
  CHECK_EQ(HealthCheck::TCP, check.type());
  CHECK(check.has_tcp());

  promise.fail("TCP health check is not supported");

  return Nothing();
}


void HealthCheckerProcess::reschedule()
{
  VLOG(1) << "Rescheduling health check in "
          << Seconds(check.interval_seconds());

  delay(Seconds(check.interval_seconds()), self(), &Self::_healthCheck);
}


namespace validation {

Option<Error> healthCheck(const HealthCheck& check)
{
  if (!check.has_type()) {
    return Error("HealthCheck must specify 'type'");
  }

  if (check.type() == HealthCheck::COMMAND) {
    if (!check.has_command()) {
      return Error("Expecting 'command' to be set for command health check");
    }

    const CommandInfo& command = check.command();

    if (!command.has_value()) {
      string commandType =
        (command.shell() ? "'shell command'" : "'executable path'");

      return Error("Command health check must contain " + commandType);
    }
  } else if (check.type() == HealthCheck::HTTP) {
    if (!check.has_http()) {
      return Error("Expecting 'http' to be set for HTTP health check");
    }

    const HealthCheck::HTTPCheckInfo& http = check.http();

    if (http.has_scheme() &&
        http.scheme() != "http" &&
        http.scheme() != "https") {
      return Error("Unsupported HTTP health check scheme: '" + http.scheme() +
                   "'");
    }

    if (http.has_path() && !strings::startsWith(http.path(), '/')) {
      return Error("The path '" + http.path() + "' of HTTP health check must "
                   "start with '/'");
    }
  } else if (check.type() == HealthCheck::TCP) {
    if (!check.has_tcp()) {
      return Error("Expecting 'tcp' to be set for TCP health check");
    }
  } else {
    return Error("Unsupported health check type: '" +
                 HealthCheck::Type_Name(check.type()) + "'");
  }

  return None();
}

} // namespace validation {

} // namespace health {
} // namespace internal {
} // namespace mesos {
