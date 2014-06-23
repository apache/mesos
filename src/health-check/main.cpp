/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <signal.h>
#include <stdio.h>
#include <iostream>
#include <string>
#include <string.h>
#include <unistd.h>

#include <mesos/mesos.hpp>

#include <process/pid.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>
#include <stout/os.hpp>
#include <stout/option.hpp>
#include <stout/flags.hpp>
#include <stout/protobuf.hpp>
#include <stout/json.hpp>

#include "common/status_utils.hpp"

#include "messages/messages.hpp"

using namespace mesos;

using std::cout;
using std::cerr;
using std::endl;
using std::string;
using std::map;

using process::UPID;

namespace mesos {
namespace internal {

using namespace process;

using namespace mesos::internal::status;

class HealthCheckerProcess : public ProtobufProcess<HealthCheckerProcess>
{
public:
  HealthCheckerProcess(
    const HealthCheck& _check,
    const UPID& _executor,
    const TaskID& _taskID)
    : check(_check),
      initializing(true),
      executor(_executor),
      taskID(_taskID),
      consecutiveFailures(0) {}

  virtual ~HealthCheckerProcess() {}

  Future<Nothing> healthCheck()
  {
    VLOG(2) << "Health checks starting in "
      << Seconds(check.delay_seconds()) << ", grace period "
      << Seconds(check.grace_period_seconds());

    startTime = Clock::now();

    delay(Seconds(check.delay_seconds()), self(), &Self::_healthCheck);
    return promise.future();
  }

private:
  void failure(const string& message)
  {
    if (check.grace_period_seconds() > 0 &&
        (Clock::now() - startTime).secs() <= check.grace_period_seconds()) {
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
      promise.fail(message);
    } else {
      reschedule();
    }
  }

  void success()
  {
    VLOG(1) << "Check passed";
    if (initializing) {
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
    } else if (check.has_command()) {
      const CommandInfo& command = check.command();

      map<string, string> environment;

      foreach (const Environment_Variable& variable,
               command.environment().variables()) {
        environment[variable.name()] = variable.value();
      }

      VLOG(2) << "Launching health command: " << command.value();

      Try<Subprocess> external =
        process::subprocess(
          command.value(),
          Subprocess::PIPE(),
          Subprocess::FD(STDERR_FILENO),
          Subprocess::FD(STDERR_FILENO),
          None(),
          environment);

      if (external.isError()) {
        promise.fail("Error creating subprocess for healthcheck");
      } else {
        Future<Option<int> > status = external.get().status();
        status.await(Seconds(check.timeout_seconds()));

        if (status.isFailed()) {
          promise.fail("Shell command check failed with status: " +
                        status.failure());
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
    } else {
      promise.fail("No check found in health check");
    }
  }

  void reschedule()
  {
    VLOG(1) << "Rescheduling health check in "
      << Seconds(check.interval_seconds());

    delay(Seconds(check.interval_seconds()), self(), &Self::_healthCheck);
  }

  Promise<Nothing> promise;
  HealthCheck check;
  bool initializing;
  UPID executor;
  TaskID taskID;
  uint32_t consecutiveFailures;
  process::Time startTime;
};

} // namespace internal {
} // namespace mesos {


class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::health_check_json,
        "health_check_json",
        "JSON describing health check to perform");

    add(&Flags::executor,
        "executor",
        "Executor UPID to send health check messages to");

    add(&Flags::task_id,
        "task_id",
        "Task ID that this health check process is checking");
  }

  Option<std::string> health_check_json;
  Option<UPID> executor;
  Option<std::string> task_id;
};


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << os::basename(argv0).get() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  Flags flags;

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  Try<Nothing> load = flags.load(None(), argc, argv);

  if (load.isError()) {
    LOG(WARNING) << load.error();
    usage(argv[0], flags);
    return -1;
  }

  if (help) {
    usage(argv[0], flags);
    return 0;
  }

  if (flags.health_check_json.isNone()) {
    LOG(WARNING) << "Expected JSON with health check description";
    usage(argv[0], flags);
    return 0;
  }

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(flags.health_check_json.get());
  if (parse.isError()) {
    LOG(WARNING) << "JSON parse error: " << parse.error();
    usage(argv[0], flags);
    return 0;
  }

  if (flags.executor.isNone()) {
    LOG(WARNING) << "Expected UPID for health check";
    usage(argv[0], flags);
    return 0;
  }

  Try<HealthCheck> check = protobuf::parse<HealthCheck>(parse.get());
  if (check.isError()) {
    LOG(WARNING) << "JSON error: " << check.error();
    usage(argv[0], flags);
    return 0;
  }

  if (check.get().has_http() && check.get().has_command()) {
    LOG(WARNING) << "Both HTTP and Command check passed in";
    return -1;
  }

  if (!check.get().has_http() && !check.get().has_command()) {
    LOG(WARNING) << "No health check found";
    return -1;
  }

  if (flags.task_id.isNone()) {
    LOG(WARNING) << "TaskID error: " << check.error();
    usage(argv[0], flags);
    return 0;
  }

  TaskID taskID;
  taskID.set_value(flags.task_id.get());

  internal::HealthCheckerProcess process(
    check.get(),
    flags.executor.get(),
    taskID);

  process::spawn(&process);

  process::Future<Nothing> checking =
    process::dispatch(
      process, &internal::HealthCheckerProcess::healthCheck);

  checking.await();

  process::terminate(process);
  process::wait(process);

  if (checking.isFailed()) {
    LOG(WARNING) << "Health check failed " << checking.failure();
    return 1;
  }

  return 0;
}
