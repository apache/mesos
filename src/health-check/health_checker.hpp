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

#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <iostream>
#include <string>

#include <mesos/mesos.hpp>

#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include <stout/os/killtree.hpp>

#include "common/status_utils.hpp"

#include "messages/messages.hpp"

using std::cout;
using std::cerr;
using std::endl;
using std::map;
using std::string;

using process::UPID;

namespace mesos {
namespace internal {

// Forward declarations.
class HealthCheckerProcess;

class HealthChecker
{
public:
  static Try<process::Owned<HealthChecker>> create(
      const HealthCheck& check,
      const UPID& executor,
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
      const UPID& _executor,
      const TaskID& _taskID)
    : ProcessBase(process::ID::generate("health-checker")),
      check(_check),
      initializing(true),
      executor(_executor),
      taskID(_taskID),
      consecutiveFailures(0) {}

  virtual ~HealthCheckerProcess() {}

  process::Future<Nothing> healthCheck()
  {
    VLOG(2) << "Health checks starting in "
      << Seconds(check.delay_seconds()) << ", grace period "
      << Seconds(check.grace_period_seconds());

    startTime = process::Clock::now();

    delay(Seconds(check.delay_seconds()), self(), &Self::_healthCheck);
    return promise.future();
  }

private:
  void failure(const string& message)
  {
    if (check.grace_period_seconds() > 0 &&
        (process::Clock::now() - startTime).secs() <=
          check.grace_period_seconds()) {
      LOG(INFO) << "Ignoring failure as health check still in grace period";
      reschedule();
      return;
    }

    consecutiveFailures++;
    VLOG(1) << "#" << consecutiveFailures << " check failed: " << message;

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

  void success()
  {
    VLOG(1) << "Check passed";

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

  void _healthCheck()
  {
    if (check.has_http()) {
      promise.fail("HTTP health check is not supported");
      return;
    }

    if (!check.has_command()) {
      promise.fail("No check found in health check");
      return;
    }

    const CommandInfo& command = check.command();

    map<string, string> environment = os::environment();

    foreach (const Environment::Variable& variable,
             command.environment().variables()) {
      environment[variable.name()] = variable.value();
    }

    // Launch the subprocess.
    Option<Try<process::Subprocess>> external = None();

    if (command.shell()) {
      // Use the shell variant.
      if (!command.has_value()) {
        promise.fail("Shell command is not specified");
        return;
      }

      VLOG(2) << "Launching health command '" << command.value() << "'";

      external = process::subprocess(
          command.value(),
          process::Subprocess::PATH("/dev/null"),
          process::Subprocess::FD(STDERR_FILENO),
          process::Subprocess::FD(STDERR_FILENO),
          process::NO_SETSID,
          environment);
    } else {
      // Use the exec variant.
      if (!command.has_value()) {
        promise.fail("Executable path is not specified");
        return;
      }

      vector<string> argv;
      foreach (const string& arg, command.arguments()) {
        argv.push_back(arg);
      }

      VLOG(2) << "Launching health command [" << command.value() << ", "
              << strings::join(", ", argv) << "]";

      external = process::subprocess(
          command.value(),
          argv,
          process::Subprocess::PATH("/dev/null"),
          process::Subprocess::FD(STDERR_FILENO),
          process::Subprocess::FD(STDERR_FILENO),
          process::NO_SETSID,
          None(),
          environment);
    }

    CHECK_SOME(external);

    if (external.get().isError()) {
      failure("Error creating subprocess for healthcheck: " +
              external.get().error());
      return;
    }

    pid_t commandPid = external.get().get().pid();

    process::Future<Option<int>> status = external.get().get().status();
    status.await(Seconds(check.timeout_seconds()));

    if (!status.isReady()) {
      string msg = "Command check failed with reason: ";
      if (status.isFailed()) {
        msg += "failed with error: " + status.failure();
      } else if (status.isDiscarded()) {
        msg += "status future discarded";
      } else {
        msg += "status still pending after timeout " +
               stringify(Seconds(check.timeout_seconds()));
      }

      if (commandPid != -1) {
        // Cleanup the external command process.
        os::killtree(commandPid, SIGKILL);
        VLOG(1) << "Kill health check command " << commandPid;
      }

      failure(msg);
      return;
    }

    int statusCode = status.get().get();
    if (statusCode != 0) {
      string message = "Health command check " + WSTRINGIFY(statusCode);
      failure(message);
    } else {
      success();
    }
  }

  void reschedule()
  {
    VLOG(1) << "Rescheduling health check in "
      << Seconds(check.interval_seconds());

    delay(Seconds(check.interval_seconds()), self(), &Self::_healthCheck);
  }

  process::Promise<Nothing> promise;
  HealthCheck check;
  bool initializing;
  UPID executor;
  TaskID taskID;
  uint32_t consecutiveFailures;
  process::Time startTime;
};

} // namespace internal {
} // namespace mesos {

#endif // __HEALTH_CHECKER_HPP__
