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

#include "checks/checker.hpp"

#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/io.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>
#include <process/time.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/jsonify.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/stopwatch.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/environment.hpp>
#include <stout/os/killtree.hpp>

#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"
#include "common/validation.hpp"

#ifdef __linux__
#include "linux/ns.hpp"
#endif

using process::Failure;
using process::Future;
using process::Owned;
using process::Subprocess;

using std::map;
using std::string;
using std::tuple;
using std::vector;

namespace mesos {
namespace internal {
namespace checks {

#ifndef __WINDOWS__
constexpr char HTTP_CHECK_COMMAND[] = "curl";
#else
constexpr char HTTP_CHECK_COMMAND[] = "curl.exe";
#endif // __WINDOWS__

static const string DEFAULT_HTTP_SCHEME = "http";

// Use '127.0.0.1' instead of 'localhost', because the host
// file in some container images may not contain 'localhost'.
static const string DEFAULT_DOMAIN = "127.0.0.1";


#ifdef __linux__
// TODO(alexr): Instead of defining this ad-hoc clone function, provide a
// general solution for entring namespace in child processes, see MESOS-6184.
static pid_t cloneWithSetns(
    const lambda::function<int()>& func,
    const Option<pid_t>& taskPid,
    const vector<string>& namespaces)
{
  return process::defaultClone([=]() -> int {
    if (taskPid.isSome()) {
      foreach (const string& ns, namespaces) {
        Try<Nothing> setns = ns::setns(taskPid.get(), ns);
        if (setns.isError()) {
          // This effectively aborts the check.
          LOG(FATAL) << "Failed to enter the " << ns << " namespace of "
                     << "task (pid: '" << taskPid.get() << "'): "
                     << setns.error();
        }

        VLOG(1) << "Entered the " << ns << " namespace of "
                << "task (pid: '" << taskPid.get() << "') successfully";
      }
    }

    return func();
  });
}
#endif


class CheckerProcess : public ProtobufProcess<CheckerProcess>
{
public:
  CheckerProcess(
      const CheckInfo& _check,
      const lambda::function<void(const CheckStatusInfo&)>& _callback,
      const TaskID& _taskId,
      const Option<pid_t>& _taskPid,
      const std::vector<std::string>& _namespaces);

  virtual ~CheckerProcess() {}

protected:
  void initialize() override;
  void finalize() override;

private:
  void performCheck();
  void scheduleNext(const Duration& duration);
  void processCheckResult(
      const Stopwatch& stopwatch,
      const CheckStatusInfo& result);

  process::Future<int> commandCheck();
  void processCommandCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<int>& result);

  process::Future<int> httpCheck();
  process::Future<int> _httpCheck(
      const std::tuple<
          process::Future<Option<int>>,
          process::Future<std::string>,
          process::Future<std::string>>& t);
  void processHttpCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<int>& result);

  const CheckInfo check;
  Duration checkDelay;
  Duration checkInterval;
  Duration checkTimeout;

  const lambda::function<void(const CheckStatusInfo&)> updateCallback;
  const TaskID taskId;
  const Option<pid_t> taskPid;
  const std::vector<std::string> namespaces;
  Option<lambda::function<pid_t(const lambda::function<int()>&)>> clone;

  CheckStatusInfo previousCheckStatus;
};


Try<Owned<Checker>> Checker::create(
    const CheckInfo& check,
    const lambda::function<void(const CheckStatusInfo&)>& callback,
    const TaskID& taskId,
    const Option<pid_t>& taskPid,
    const vector<string>& namespaces)
{
  // Validate the `CheckInfo` protobuf.
  Option<Error> error = validation::checkInfo(check);
  if (error.isSome()) {
    return error.get();
  }

  Owned<CheckerProcess> process(new CheckerProcess(
      check,
      callback,
      taskId,
      taskPid,
      namespaces));

  return Owned<Checker>(new Checker(process));
}


Checker::Checker(Owned<CheckerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Checker::~Checker()
{
  terminate(process.get());
  wait(process.get());
}


void Checker::stop()
{
  terminate(process.get(), true);
}


CheckerProcess::CheckerProcess(
    const CheckInfo& _check,
    const lambda::function<void(const CheckStatusInfo&)>& _callback,
    const TaskID& _taskId,
    const Option<pid_t>& _taskPid,
    const vector<string>& _namespaces)
  : ProcessBase(process::ID::generate("checker")),
    check(_check),
    updateCallback(_callback),
    taskId(_taskId),
    taskPid(_taskPid),
    namespaces(_namespaces)
{
  Try<Duration> create = Duration::create(check.delay_seconds());
  CHECK_SOME(create);
  checkDelay = create.get();

  create = Duration::create(check.interval_seconds());
  CHECK_SOME(create);
  checkInterval = create.get();

  // Zero value means infinite timeout.
  create = Duration::create(check.timeout_seconds());
  CHECK_SOME(create);
  checkTimeout =
    (create.get() > Duration::zero()) ? create.get() : Duration::max();

  // The first check update should be sent only when a check succeeds,
  // hence we should deduplicate against a corresponding "empty" result.
  previousCheckStatus.set_type(check.type());
  switch (check.type()) {
    case CheckInfo::COMMAND: {
      previousCheckStatus.mutable_command();
      break;
    }

    case CheckInfo::HTTP: {
      previousCheckStatus.mutable_http();
      break;
    }

    case CheckInfo::UNKNOWN: {
      LOG(FATAL) << "Received UNKNOWN check type";
      break;
    }
  }

#ifdef __linux__
  if (!namespaces.empty()) {
    clone = lambda::bind(&cloneWithSetns, lambda::_1, taskPid, namespaces);
  }
#endif
}


void CheckerProcess::initialize()
{
  VLOG(1) << "Check configuration for task " << taskId << ":"
          << " '" << jsonify(JSON::Protobuf(check)) << "'";

  scheduleNext(checkDelay);
}


void CheckerProcess::finalize()
{
  LOG(INFO) << "Checking for task " << taskId << " stopped";
}


void CheckerProcess::performCheck()
{
  Stopwatch stopwatch;
  stopwatch.start();

  switch (check.type()) {
    case CheckInfo::COMMAND: {
      commandCheck().onAny(defer(
          self(),
          &Self::processCommandCheckResult, stopwatch, lambda::_1));
      break;
    }

    case CheckInfo::HTTP: {
      httpCheck().onAny(defer(
          self(),
          &Self::processHttpCheckResult, stopwatch, lambda::_1));
      break;
    }

    case CheckInfo::UNKNOWN: {
      LOG(FATAL) << "Received UNKNOWN check type";
      break;
    }
  }
}


void CheckerProcess::scheduleNext(const Duration& duration)
{
  VLOG(1) << "Scheduling check for task " << taskId << " in " << duration;

  delay(duration, self(), &Self::performCheck);
}


void CheckerProcess::processCheckResult(
    const Stopwatch& stopwatch,
    const CheckStatusInfo& result)
{
  VLOG(1) << "Performed " << check.type() << " check for task " << taskId
          << " in " << stopwatch.elapsed();

  // Trigger the callback if check info changes.
  if (result != previousCheckStatus) {
    // We assume this is a local send, i.e., the checker library is not used
    // in a binary external to the executor and hence can not exit before
    // the data is sent to the executor.
    updateCallback(result);
    previousCheckStatus = result;
  }

  scheduleNext(checkInterval);
}


Future<int> CheckerProcess::commandCheck()
{
  CHECK_EQ(CheckInfo::COMMAND, check.type());
  CHECK(check.has_command());

  const CommandInfo& command = check.command().command();

  map<string, string> environment = os::environment();

  foreach (const Environment::Variable& variable,
           command.environment().variables()) {
    environment[variable.name()] = variable.value();
  }

  // Launch the subprocess.
  Try<Subprocess> s = Error("Not launched");

  if (command.shell()) {
    // Use the shell variant.
    VLOG(1) << "Launching command check '" << command.value() << "'"
            << " for task " << taskId;

    s = process::subprocess(
        command.value(),
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        environment,
        clone);
  } else {
    // Use the exec variant.
    vector<string> argv(
        std::begin(command.arguments()), std::end(command.arguments()));

    VLOG(1) << "Launching command check [" << command.value() << ", "
            << strings::join(", ", argv) << "] for task " << taskId;

    s = process::subprocess(
        command.value(),
        argv,
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        nullptr,
        environment,
        clone);
  }

  if (s.isError()) {
    return Failure("Failed to create subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once it is available.
  const pid_t commandPid = s->pid();
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return s->status()
    .after(
        timeout,
        [timeout, commandPid, _taskId](Future<Option<int>> future) {
      future.discard();

      if (commandPid != -1) {
        // Cleanup the external command process.
        VLOG(1) << "Killing the command check process " << commandPid
                << " for task " << _taskId;

        os::killtree(commandPid, SIGKILL);
      }

      return Failure(
          "Command timed out after " + stringify(timeout) + "; aborting");
    })
    .then([](const Option<int>& exitCode) -> Future<int> {
      if (exitCode.isNone()) {
        return Failure("Failed to reap the command process");
      }

      return exitCode.get();
    });
}


void CheckerProcess::processCommandCheckResult(
    const Stopwatch& stopwatch,
    const Future<int>& result)
{
  CheckStatusInfo checkStatusInfo;
  checkStatusInfo.set_type(check.type());

  // On Posix, `result` corresponds to termination information in the
  // `stat_loc` area. On Windows, `status` is obtained via calling the
  // `GetExitCodeProcess()` function.
  //
  // TODO(alexr): Ensure `WEXITSTATUS` family macros are no-op on Windows,
  // see MESOS-7242.
  if (result.isReady() && WIFEXITED(result.get())) {
    const int exitCode = WEXITSTATUS(result.get());
    VLOG(1) << check.type() << " check for task "
            << taskId << " returned " << exitCode;

    checkStatusInfo.mutable_command()->set_exit_code(
        static_cast<int32_t>(exitCode));
  } else {
    // Check's status is currently not available, which may indicate a change
    // that should be reported as an empty `CheckStatusInfo.Command` message.
    LOG(WARNING) << "Check for task " << taskId << " failed: "
                 << (result.isFailed() ? result.failure() : "discarded");

    checkStatusInfo.mutable_command();
  }

  processCheckResult(stopwatch, checkStatusInfo);
}


Future<int> CheckerProcess::httpCheck()
{
  CHECK_EQ(CheckInfo::HTTP, check.type());
  CHECK(check.has_http());

  const CheckInfo::Http& http = check.http();

  const string scheme = DEFAULT_HTTP_SCHEME;
  const string path = http.has_path() ? http.path() : "";
  const string url = scheme + "://" + DEFAULT_DOMAIN + ":" +
                     stringify(http.port()) + path;

  VLOG(1) << "Launching HTTP check '" << url << "' for task " << taskId;

  const vector<string> argv = {
    HTTP_CHECK_COMMAND,
    "-s",                 // Don't show progress meter or error messages.
    "-S",                 // Makes curl show an error message if it fails.
    "-L",                 // Follows HTTP 3xx redirects.
    "-k",                 // Ignores SSL validation when scheme is https.
    "-w", "%{http_code}", // Displays HTTP response code on stdout.
    "-o", os::DEV_NULL,    // Ignores output.
    url
  };

  // TODO(alexr): Consider launching the helper binary once per task lifetime,
  // see MESOS-6766.
  Try<Subprocess> s = process::subprocess(
      HTTP_CHECK_COMMAND,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      clone);

  if (s.isError()) {
    return Failure(
        "Failed to create the " + string(HTTP_CHECK_COMMAND) +
        " subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once it is available.
  const pid_t curlPid = s->pid();
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(
        timeout,
        [timeout, curlPid, _taskId](Future<tuple<Future<Option<int>>,
                                                 Future<string>,
                                                 Future<string>>> future) {
      future.discard();

      if (curlPid != -1) {
        // Cleanup the HTTP_CHECK_COMMAND process.
        VLOG(1) << "Killing the HTTP check process " << curlPid
                << " for task " << _taskId;

        os::killtree(curlPid, SIGKILL);
      }

      return Failure(
          string(HTTP_CHECK_COMMAND) + " timed out after " +
          stringify(timeout) + "; aborting");
    })
    .then(defer(self(), &Self::_httpCheck, lambda::_1));
}


Future<int> CheckerProcess::_httpCheck(
    const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t)
{
  Future<Option<int>> status = std::get<0>(t);
  if (!status.isReady()) {
    return Failure(
        "Failed to get the exit status of the " + string(HTTP_CHECK_COMMAND) +
        " process: " + (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Failure(
        "Failed to reap the " + string(HTTP_CHECK_COMMAND) + " process");
  }

  int exitCode = status->get();
  if (exitCode != 0) {
    Future<string> error = std::get<2>(t);
    if (!error.isReady()) {
      return Failure(
          string(HTTP_CHECK_COMMAND) + " returned " +
          WSTRINGIFY(exitCode) + "; reading stderr failed: " +
          (error.isFailed() ? error.failure() : "discarded"));
    }

    return Failure(
        string(HTTP_CHECK_COMMAND) + " returned " +
        WSTRINGIFY(exitCode) + ": " + error.get());
  }

  Future<string> output = std::get<1>(t);
  if (!output.isReady()) {
    return Failure(
        "Failed to read stdout from " + string(HTTP_CHECK_COMMAND) + ": " +
        (output.isFailed() ? output.failure() : "discarded"));
  }

  // Parse the output and get the HTTP status code.
  Try<int> statusCode = numify<int>(output.get());
  if (statusCode.isError()) {
    return Failure(
        "Unexpected output from " + string(HTTP_CHECK_COMMAND) + ": " +
        output.get());
  }

  return statusCode.get();
}


void CheckerProcess::processHttpCheckResult(
    const Stopwatch& stopwatch,
    const process::Future<int>& result)
{
  CheckStatusInfo checkStatusInfo;
  checkStatusInfo.set_type(check.type());

  if (result.isReady()) {
    VLOG(1) << check.type() << " check for task "
            << taskId << " returned " << result.get();

    checkStatusInfo.mutable_http()->set_status_code(
        static_cast<uint32_t>(result.get()));
  } else {
    // Check's status is currently not available, which may indicate a change
    // that should be reported as an empty `CheckStatusInfo.Http` message.
    LOG(WARNING) << "Check for task " << taskId << " failed: "
                 << (result.isFailed() ? result.failure() : "discarded");

    checkStatusInfo.mutable_http();
  }

  processCheckResult(stopwatch, checkStatusInfo);
}

namespace validation {

Option<Error> checkInfo(const CheckInfo& checkInfo)
{
  if (!checkInfo.has_type()) {
    return Error("CheckInfo must specify 'type'");
  }

  switch (checkInfo.type()) {
    case CheckInfo::COMMAND: {
      if (!checkInfo.has_command()) {
        return Error("Expecting 'command' to be set for command check");
      }

      const CommandInfo& command = checkInfo.command().command();

      if (!command.has_value()) {
        string commandType =
          (command.shell() ? "'shell command'" : "'executable path'");

        return Error("Command check must contain " + commandType);
      }

      Option<Error> error =
        common::validation::validateCommandInfo(command);
      if (error.isSome()) {
        return Error(
            "Check's `CommandInfo` is invalid: " + error->message);
      }

      // TODO(alexr): Make sure irrelevant fields, e.g., `uris` are not set.

      break;
    }

    case CheckInfo::HTTP: {
      if (!checkInfo.has_http()) {
        return Error("Expecting 'http' to be set for HTTP check");
      }

      const CheckInfo::Http& http = checkInfo.http();

      if (http.has_path() && !strings::startsWith(http.path(), '/')) {
        return Error(
            "The path '" + http.path() +
            "' of HTTP  check must start with '/'");
      }

      break;
    }

    case CheckInfo::UNKNOWN: {
      return Error(
          "'" + CheckInfo::Type_Name(checkInfo.type()) + "'"
          " is not a valid check type");
    }
  }

  if (checkInfo.has_delay_seconds() && checkInfo.delay_seconds() < 0.0) {
    return Error("Expecting 'delay_seconds' to be non-negative");
  }

  if (checkInfo.has_interval_seconds() && checkInfo.interval_seconds() < 0.0) {
    return Error("Expecting 'interval_seconds' to be non-negative");
  }

  if (checkInfo.has_timeout_seconds() && checkInfo.timeout_seconds() < 0.0) {
    return Error("Expecting 'timeout_seconds' to be non-negative");
  }

  return None();
}


Option<Error> checkStatusInfo(const CheckStatusInfo& checkStatusInfo)
{
  if (!checkStatusInfo.has_type()) {
    return Error("CheckStatusInfo must specify 'type'");
  }

  switch (checkStatusInfo.type()) {
    case CheckInfo::COMMAND: {
      if (!checkStatusInfo.has_command()) {
        return Error(
            "Expecting 'command' to be set for command check's status");
      }
      break;
    }

    case CheckInfo::HTTP: {
      if (!checkStatusInfo.has_http()) {
        return Error("Expecting 'http' to be set for HTTP check's status");
      }
      break;
    }

    case CheckInfo::UNKNOWN: {
      return Error(
          "'" + CheckInfo::Type_Name(checkStatusInfo.type()) + "'"
          " is not a valid check's status type");
    }
  }

  return None();
}

} // namespace validation {

} // namespace checks {
} // namespace internal {
} // namespace mesos {
