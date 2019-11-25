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

#include "checks/checker_process.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/agent/agent.hpp>

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
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/jsonify.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/recordio.hpp>
#include <stout/stopwatch.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include <stout/os/environment.hpp>
#include <stout/os/killtree.hpp>

#include "checks/checks_runtime.hpp"
#include "checks/checks_types.hpp"

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "internal/evolve.hpp"

#ifdef __linux__
#include "linux/ns.hpp"
#endif

namespace http = process::http;

using process::Failure;
using process::Future;
using process::Owned;
using process::Promise;
using process::Subprocess;

using std::map;
using std::shared_ptr;
using std::string;
using std::tuple;
using std::vector;

namespace mesos {
namespace internal {
namespace checks {

#ifndef __WINDOWS__
constexpr char HTTP_CHECK_COMMAND[] = "curl";
constexpr char TCP_CHECK_COMMAND[] = "mesos-tcp-connect";
#else
constexpr char HTTP_CHECK_COMMAND[] = "curl.exe";
constexpr char TCP_CHECK_COMMAND[] = "mesos-tcp-connect.exe";
#endif // __WINDOWS__


#ifdef __linux__
// TODO(alexr): Instead of defining this ad-hoc clone function, provide a
// general solution for entering namespace in child processes, see MESOS-6184.
static pid_t cloneWithSetns(
    const lambda::function<int()>& func,
    const Option<pid_t>& taskPid,
    const vector<string>& namespaces)
{
  auto child = [=]() -> int {
    if (taskPid.isSome()) {
      foreach (const string& ns, namespaces) {
        Try<Nothing> setns = ns::setns(taskPid.get(), ns);
        if (setns.isError()) {
          // This effectively aborts the check.
          LOG(FATAL) << "Failed to enter the " << ns << " namespace of task"
                     << " (pid: " << taskPid.get() << "): " << setns.error();
        }

        VLOG(1) << "Entered the " << ns << " namespace of task"
                << " (pid: " << taskPid.get() << ") successfully";
      }
    }

    return func();
  };

  pid_t pid = ::fork();
  if (pid == -1) {
    return -1;
  } else if (pid == 0) {
    // Child.
    ::exit(child());
    UNREACHABLE();
  } else {
    // Parent.
    return pid;
  }
}
#endif


// The return type matches the `clone` function parameter in
// `process::subprocess`.
static Option<lambda::function<pid_t(const lambda::function<int()>&)>>
getCustomCloneFunc(const Option<runtime::Plain>& plain)
{
  if (plain.isNone() || plain->namespaces.empty()) {
    return None();
  }

#ifdef __linux__
  return lambda::bind(
      &cloneWithSetns,
      lambda::_1,
      plain->taskPid,
      plain->namespaces);
#else
  return None();
#endif // __linux__
}


// Reads `ProcessIO::Data` records from a string containing "Record-IO"
// data encoded in protobuf messages, and returns the stdout and stderr.
//
// NOTE: This function ignores any `ProcessIO::Control` records.
//
// TODO(gkleiman): This function is very similar to one in `api_tests.cpp`, we
// should refactor them into a common helper when fixing MESOS-7903.
static Try<tuple<string, string>> decodeProcessIOData(const string& data)
{
  string stdoutReceived;
  string stderrReceived;

  ::recordio::Decoder decoder;

  Try<std::deque<string>> records = decoder.decode(data);

  if (records.isError()) {
    return Error(records.error());
  }

  while (!records->empty()) {
    string record = std::move(records->front());
    records->pop_front();

    Try<v1::agent::ProcessIO> result = deserialize<v1::agent::ProcessIO>(
        ContentType::PROTOBUF, record);

    if (result.isError()) {
      return Error(result.error());
    }

    if (result->data().type() == v1::agent::ProcessIO::Data::STDOUT) {
      stdoutReceived += result->data().data();
    } else if (result->data().type() == v1::agent::ProcessIO::Data::STDERR) {
      stderrReceived += result->data().data();
    }
  }

  return std::make_tuple(stdoutReceived, stderrReceived);
}


static Variant<check::Command, check::Http, check::Tcp> checkInfoToCheck(
    const CheckInfo& checkInfo,
    const string& launcherDir,
    const Option<string>& scheme,
    bool ipv6)
{
  // Use the `CheckInfo` struct to create the right
  // `check::{Command, Http, Tcp}` struct.
  switch (checkInfo.type()) {
    case CheckInfo::COMMAND: {
      return check::Command(checkInfo.command().command());
    }
    case CheckInfo::HTTP: {
      const CheckInfo::Http& http = checkInfo.http();
      return check::Http(
          http.port(),
          http.has_path() ? http.path() : "",
          scheme.getOrElse(check::DEFAULT_HTTP_SCHEME),
          ipv6);
    }
    case CheckInfo::TCP: {
      return check::Tcp(checkInfo.tcp().port(), launcherDir);
    }
    case CheckInfo::UNKNOWN: {
      // TODO(josephw): Add validation at the agent or master level.
      // Without validation, this is considered a mis-configuration of the
      // task (user error).
      LOG(FATAL) << "Received UNKNOWN check type";
    }
  }

  UNREACHABLE();
}


CheckerProcess::CheckerProcess(
    const CheckInfo& checkInfo,
    const string& launcherDir,
    const lambda::function<void(const Try<CheckStatusInfo>&)>& _callback,
    const TaskID& _taskId,
    const string& _name,
    Variant<runtime::Plain, runtime::Docker, runtime::Nested> _runtime,
    const Option<string>& scheme,
    bool ipv6)
  : ProcessBase(process::ID::generate("checker")),
    updateCallback(_callback),
    taskId(_taskId),
    name(_name),
    runtime(std::move(_runtime)),
    check(checkInfoToCheck(checkInfo, launcherDir, scheme, ipv6)),
    paused(false)
{
  Try<Duration> create = Duration::create(checkInfo.delay_seconds());
  CHECK_SOME(create);
  checkDelay = create.get();

  create = Duration::create(checkInfo.interval_seconds());
  CHECK_SOME(create);
  checkInterval = create.get();

  // Zero value means infinite timeout.
  create = Duration::create(checkInfo.timeout_seconds());
  CHECK_SOME(create);
  checkTimeout =
    (create.get() > Duration::zero()) ? create.get() : Duration::max();
}


void CheckerProcess::initialize()
{
  scheduleNext(checkDelay);
}


void CheckerProcess::finalize()
{
  LOG(INFO) << "Stopped " << name << " for task '" << taskId << "'";
}


void CheckerProcess::performCheck()
{
  if (paused) {
    return;
  }

  Stopwatch stopwatch;
  stopwatch.start();

  // There are 3 checks (CMD/HTTP/TCP) for 3 runtimes (PLAIN/DOCKER/NESTED)
  // for 2 different OSes (Linux/Windows), so we have a 3x3x2 matrix of
  // possibilities. Luckily, a lot of the cases have the same implementation,
  // so we don't need to have 18 different code paths.
  //
  // Here is a matrix of all the different implementations with explanations.
  // Note that the format is "Linux/Windows", so the Linux implementation is
  // before Windows if they differ.
  //
  //         | CMD  | HTTP | TCP
  // --------+------+------+-------
  //  Plain  | A*/A |  B   |  C
  //  Docker |  D   | B*/E | C*/F
  //  Nested |  G/- |  B   |  C
  //
  // Explanations:
  // - A, B, C: Standard check launched directly by the library's user, i.e.,
  //   the executor. Specifically, it launches the given command for CMD checks,
  //   `curl` for HTTP checks, and `mesos-tcp-connect` for TCP checks.
  // - A*, B*, C*: On Linux, the proper namespaces will be entered, which are
  //   the optional "mnt" for CMD and the required "net" for Docker HTTP/TCP.
  //   These checks are executed by the library's user, i.e., the executor.
  // - D: Delegate the command to Docker by wrapping the command with
  //   `docker exec` to run in the container's namespaces.
  // - E, F: On Windows, delegate the network checks to Docker by wrapping the
  //   check with the `docker run --network=container:id ...` command to
  //   run the check in the container's namespaces.
  // - G: The library uses Agent API to delegate running the command in a
  //   nested container on Linux.
  // - '-': Not implemented (nested command checks on Windows).
  check.visit(
      [=](const check::Command& cmd) {
        Future<int> future = runtime.visit(
            [=](const runtime::Plain& plain) {
              // Case A* and A
              return commandCheck(cmd, plain);
            },
            [=](const runtime::Docker& docker) {
              // Case D
              return dockerCommandCheck(cmd, docker);
            },
            [=](const runtime::Nested& nested) {
              // Case G
              return nestedCommandCheck(cmd, nested);
            });

        future.onAny(defer(
            self(), &Self::processCommandCheckResult, stopwatch, lambda::_1));
      },
      [=](const check::Http& http) {
        Future<int> future = runtime.visit(
            [=](const runtime::Plain& plain) {
              // Case B
              return httpCheck(http, plain);
            },
            [=](const runtime::Docker& docker) {
#ifdef __WINDOWS__
              // Case E
              return dockerHttpCheck(http, docker);
#else
              // Case B*
              return httpCheck(
                  http, runtime::Plain{docker.namespaces, docker.taskPid});
#endif // __WINDOWS__
            },
            [=](const runtime::Nested&) {
              // Case B
              return httpCheck(http, None());
            });

        future.onAny(defer(
            self(), &Self::processHttpCheckResult, stopwatch, lambda::_1));
      },
      [=](const check::Tcp& tcp) {
        Future<bool> future = runtime.visit(
            [=](const runtime::Plain& plain) {
              // Case C
              return tcpCheck(tcp, plain);
            },
            [=](const runtime::Docker& docker) {
#ifdef __WINDOWS__
              // Case F
              return dockerTcpCheck(tcp, docker);
#else
              // Case C*
              return tcpCheck(
                  tcp, runtime::Plain{docker.namespaces, docker.taskPid});
#endif // __WINDOWS__
            },
            [=](const runtime::Nested&) {
              // Case C
              return tcpCheck(tcp, None());
            });

        future.onAny(
            defer(self(), &Self::processTcpCheckResult, stopwatch, lambda::_1));
      });
}


void CheckerProcess::scheduleNext(const Duration& duration)
{
  CHECK(!paused);

  VLOG(1) << "Scheduling " << name << " for task '" << taskId << "' in "
          << duration;

  delay(duration, self(), &Self::performCheck);
}


void CheckerProcess::pause()
{
  if (!paused) {
    VLOG(1) << "Paused " << name << " for task '" << taskId << "'";

    paused = true;
  }
}


void CheckerProcess::resume()
{
  if (paused) {
    VLOG(1) << "Resumed " << name << " for task '" << taskId << "'";

    paused = false;

    // Schedule a check immediately.
    scheduleNext(Duration::zero());
  }
}


void CheckerProcess::processCheckResult(
    const Stopwatch& stopwatch,
    const Result<CheckStatusInfo>& result)
{
  // `Checker` might have been paused while performing the check.
  if (paused) {
    LOG(INFO) << "Ignoring " << name << " result for"
              << " task '" << taskId << "': checking is paused";
    return;
  }

  // `result` will be:
  //
  // 1. `Some(CheckStatusInfo)` if it was possible to perform the check.
  // 2. An `Error` if the check failed due to a non-transient error,
  //    e.g., timed out.
  // 3. `None` if the check failed due to a transient error - this kind of
  //     failure will be silently ignored.
  if (result.isSome()) {
    // It was possible to perform the check.
    VLOG(1) << "Performed " << name << " for task '" << taskId << "' in "
            << stopwatch.elapsed();

    updateCallback(result.get());
  } else if (result.isError()) {
    // The check failed due to a non-transient error.
    updateCallback(Error(result.error()));
  } else {
    // The check failed due to a transient error.
    LOG(INFO) << name << " for task '" << taskId << "' is not available";
  }

  scheduleNext(checkInterval);
}


Future<int> CheckerProcess::commandCheck(
    const check::Command& cmd,
    const runtime::Plain& plain)
{
  const CommandInfo& command = cmd.info;

  map<string, string> environment = os::environment();

  foreach (const Environment::Variable& variable,
           command.environment().variables()) {
    environment[variable.name()] = variable.value();
  }

  // Launch the subprocess.
  Try<Subprocess> s = Error("Not launched");

  if (command.shell()) {
    // Use the shell variant.
    VLOG(1) << "Launching " << name << " '" << command.value() << "'"
            << " for task '" << taskId << "'";

    s = process::subprocess(
        command.value(),
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        environment,
        getCustomCloneFunc(plain));
  } else {
    // Use the exec variant.
    vector<string> argv(
        std::begin(command.arguments()), std::end(command.arguments()));

    VLOG(1) << "Launching " << name << " [" << command.value() << ", "
            << strings::join(", ", argv) << "] for task '" << taskId << "'";

    s = process::subprocess(
        command.value(),
        argv,
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        nullptr,
        environment,
        getCustomCloneFunc(plain));
  }

  if (s.isError()) {
    return Failure("Failed to create subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once it is available.
  const pid_t commandPid = s->pid();
  const string _name = name;
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return s->status()
    .after(
        timeout,
        [timeout, commandPid, _name, _taskId](Future<Option<int>> future)
    {
      future.discard();

      if (commandPid != -1) {
        // Cleanup the external command process.
        VLOG(1) << "Killing the " << _name << " process '" << commandPid
                << "' for task '" << _taskId << "'";

        os::killtree(commandPid, SIGKILL);
      }

      return Failure("Command timed out after " + stringify(timeout));
    })
    .then([](const Option<int>& exitCode) -> Future<int> {
      if (exitCode.isNone()) {
        return Failure("Failed to reap the command process");
      }

      return exitCode.get();
    });
}


Future<int> CheckerProcess::dockerCommandCheck(
    const check::Command& cmd,
    const runtime::Docker& docker)
{
  // Wrap the original health check command in `docker exec`.
  const CommandInfo& command = cmd.info;

  vector<string> commandArguments;
  commandArguments.push_back(docker.dockerPath);
  commandArguments.push_back("-H");
  commandArguments.push_back(docker.socketName);
  commandArguments.push_back("exec");
  commandArguments.push_back(docker.containerName);

  if (command.shell()) {
    commandArguments.push_back(os::Shell::name);
    commandArguments.push_back(os::Shell::arg1);

    // We don't need to quote the arguments, because we're converting the
    // shell command to the executable form.
    commandArguments.push_back(command.value());
  } else {
    // The format is `docker exec [OPTIONS] CONTAINER COMMAND [ARG...]`, so we
    // have `COMMAND == command.value()` or `COMMAND == command.arguments()[0]`.
    // So, `command.arguments()[0]` can either be executable name or the "real"
    // first argument to the program (`argv[1]`). This matches the logic in
    // `docker->run`.
    if (command.has_value()) {
      commandArguments.push_back(command.value());
    }

    commandArguments.insert(
        commandArguments.end(),
        command.arguments().cbegin(),
        command.arguments().cend());
  }

  CommandInfo dockerCmd(command);
  dockerCmd.set_shell(false);
  dockerCmd.set_value(commandArguments[0]);
  dockerCmd.clear_arguments();
  foreach (const string& argument, commandArguments) {
    dockerCmd.add_arguments(argument);
  }

  return commandCheck(
      check::Command(dockerCmd),
      runtime::Plain{docker.namespaces, docker.taskPid});
}


Future<int> CheckerProcess::nestedCommandCheck(
    const check::Command& cmd,
    const runtime::Nested& nested)
{
  VLOG(1) << "Launching " << name << " for task '" << taskId << "'";

  // We don't want recoverable errors, e.g., the agent responding with
  // HTTP status code 503, to trigger a check failure.
  //
  // The future returned by this method represents the result of a
  // check. It will be set to the exit status of the check command if it
  // succeeded, to a `Failure` if there was a non-transient error, and
  // discarded if there was a transient error.
  auto promise = std::make_shared<Promise<int>>();

  if (previousCheckContainerId.isSome()) {
    agent::Call call;
    call.set_type(agent::Call::REMOVE_NESTED_CONTAINER);

    mesos::ContainerID previousId = previousCheckContainerId.get();

    agent::Call::RemoveNestedContainer* removeContainer =
      call.mutable_remove_nested_container();

    removeContainer->mutable_container_id()->CopyFrom(
        previousCheckContainerId.get());

    http::Request request;
    request.method = "POST";
    request.url = nested.agentURL;
    request.body = serialize(ContentType::PROTOBUF, evolve(call));
    request.headers = {{"Accept", stringify(ContentType::PROTOBUF)},
                       {"Content-Type", stringify(ContentType::PROTOBUF)}};

    if (nested.authorizationHeader.isSome()) {
      request.headers["Authorization"] = nested.authorizationHeader.get();
    }

    http::request(request, false)
      .onFailed(defer(self(),
                      [this, promise, previousId](const string& failure) {
        LOG(WARNING) << "Connection to remove the nested container '"
                     << previousId << "' used for the " << name << " for"
                     << " task '" << taskId << "' failed: " << failure;

        // Something went wrong while sending the request, we treat this
        // as a transient failure and discard the promise.
        promise->discard();
      }))
      .onReady(defer(self(),
                     [this, promise, cmd, nested, previousId]
                     (const http::Response& response) {
        if (response.code != http::Status::OK) {
          // The agent was unable to remove the check container, we
          // treat this as a transient failure and discard the promise.
          LOG(WARNING) << "Received '" << response.status << "' ("
                       << response.body << ") while removing the nested"
                       << " container '" << previousId << "' used for"
                       << " the " << name << " for task '" << taskId << "'";

          promise->discard();
        } else {
          previousCheckContainerId = None();
          _nestedCommandCheck(promise, cmd, nested);
        }
      }));
  } else {
    _nestedCommandCheck(promise, cmd, nested);
  }

  return promise->future();
}


void CheckerProcess::_nestedCommandCheck(
    shared_ptr<Promise<int>> promise,
    check::Command cmd,
    runtime::Nested nested)
{
  // TODO(alexr): Use lambda named captures for
  // these cached values once they are available.
  const TaskID _taskId = taskId;
  const string _name = name;

  http::connect(nested.agentURL)
    .onFailed(defer(self(), [_taskId, _name, promise](const string& failure) {
      LOG(WARNING) << "Unable to establish connection with the agent to launch "
                   << _name << " for task '" << _taskId << "'"
                   << ": " << failure;

      // We treat this as a transient failure.
      promise->discard();
    }))
    .onReady(defer(self(),
                   &Self::__nestedCommandCheck,
                   promise,
                   lambda::_1,
                   cmd,
                   nested));
}


void CheckerProcess::__nestedCommandCheck(
    shared_ptr<Promise<int>> promise,
    http::Connection connection,
    check::Command cmd,
    runtime::Nested nested)
{
  ContainerID checkContainerId;
  checkContainerId.set_value("check-" + id::UUID::random().toString());
  checkContainerId.mutable_parent()->CopyFrom(nested.taskContainerId);

  previousCheckContainerId = checkContainerId;

  CommandInfo command(cmd.info);

  agent::Call call;
  call.set_type(agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  agent::Call::LaunchNestedContainerSession* launch =
    call.mutable_launch_nested_container_session();

  launch->mutable_container_id()->CopyFrom(checkContainerId);
  launch->mutable_command()->CopyFrom(command);

  http::Request request;
  request.method = "POST";
  request.url = nested.agentURL;
  request.body = serialize(ContentType::PROTOBUF, evolve(call));
  request.headers = {{"Accept", stringify(ContentType::RECORDIO)},
                     {"Message-Accept", stringify(ContentType::PROTOBUF)},
                     {"Content-Type", stringify(ContentType::PROTOBUF)}};

  if (nested.authorizationHeader.isSome()) {
    request.headers["Authorization"] = nested.authorizationHeader.get();
  }

  // TODO(alexr): Use a lambda named capture for
  // this cached value once it is available.
  const Duration timeout = checkTimeout;

  auto checkTimedOut = std::make_shared<bool>(false);

  // `LAUNCH_NESTED_CONTAINER_SESSION` returns a streamed response with
  // the output of the container. The agent will close the stream once
  // the container has exited, or kill the container if the client
  // closes the connection.
  //
  // We're calling `Connection::send` with `streamed = false`, so that
  // it returns an HTTP response of type 'BODY' once the entire response
  // is received.
  //
  // This means that this future will not be completed until after the
  // check command has finished or the connection has been closed.
  //
  // TODO(gkleiman): The output of timed-out checks is lost, we'll
  // probably have to call `Connection::send` with `streamed = true`
  // to be able to log it. See MESOS-7903.
  connection.send(request, false)
    .after(checkTimeout,
           defer(self(),
                 [timeout, checkTimedOut](Future<http::Response> future) {
      future.discard();

      *checkTimedOut = true;

      return Failure("Command timed out after " + stringify(timeout));
    }))
    .onFailed(defer(self(),
                    &Self::nestedCommandCheckFailure,
                    promise,
                    connection,
                    checkContainerId,
                    checkTimedOut,
                    lambda::_1,
                    nested))
    .onReady(defer(self(),
                   &Self::___nestedCommandCheck,
                   promise,
                   checkContainerId,
                   lambda::_1,
                   nested));
}


void CheckerProcess::___nestedCommandCheck(
    shared_ptr<Promise<int>> promise,
    const ContainerID& checkContainerId,
    const http::Response& launchResponse,
    runtime::Nested nested)
{
  if (launchResponse.code != http::Status::OK) {
    // The agent was unable to launch the check container,
    // we treat this as a transient failure.
    LOG(WARNING) << "Received '" << launchResponse.status << "' ("
                 << launchResponse.body << ") while launching " << name
                 << " for task '" << taskId << "'";

    // We'll try to remove the container created for the check at the
    // beginning of the next check. In order to prevent a failure, the
    // promise should only be completed once we're sure that the
    // container has terminated.
    waitNestedContainer(checkContainerId, nested)
      .onAny([promise](const Future<Option<int>>&) {
        // We assume that once `WaitNestedContainer` returns,
        // irrespective of whether the response contains a failure, the
        // container will be in a terminal state, and that it will be
        // possible to remove it.
        promise->discard();
    });

    return;
  }

  Try<tuple<string, string>> checkOutput =
    decodeProcessIOData(launchResponse.body);

  if (checkOutput.isError()) {
    LOG(WARNING) << "Failed to decode the output of the " << name
                 << " for task '" << taskId << "': " << checkOutput.error();
  } else {
    string stdoutReceived;
    string stderrReceived;

    tie(stdoutReceived, stderrReceived) = checkOutput.get();

    LOG(INFO) << "Output of the " << name << " for task '" << taskId
              << "' (stdout):" << std::endl << stdoutReceived;

    LOG(INFO) << "Output of the " << name << " for task '" << taskId
              << "' (stderr):" << std::endl << stderrReceived;
  }

  waitNestedContainer(checkContainerId, nested)
    .onFailed([promise](const string& failure) {
      promise->fail(
          "Unable to get the exit code: " + failure);
    })
    .onReady([promise](const Option<int>& status) -> void {
      if (status.isNone()) {
        promise->fail("Unable to get the exit code");
#ifndef __WINDOWS__
      // TODO(akagup): Implement this for Windows. The `WaitNestedContainer`
      // has a `TaskState` field, so we can probably use that for determining
      // if the task failed.
      } else if (WIFSIGNALED(status.get()) &&
                 WTERMSIG(status.get()) == SIGKILL) {
        // The check container was signaled, probably because the task
        // finished while the check was still in-flight, so we discard
        // the result.
        promise->discard();
#endif // __WINDOWS__
      } else {
        promise->set(status.get());
      }
    });
}


void CheckerProcess::nestedCommandCheckFailure(
    shared_ptr<Promise<int>> promise,
    http::Connection connection,
    const ContainerID& checkContainerId,
    shared_ptr<bool> checkTimedOut,
    const string& failure,
    runtime::Nested nested)
{
  if (*checkTimedOut) {
    // The check timed out, closing the connection will make the agent
    // kill the container.
    connection.disconnect();

    // If the check delay interval is zero, we'll try to perform another
    // check right after we finish processing the current timeout.
    //
    // We'll try to remove the container created for the check at the
    // beginning of the next check. In order to prevent a failure, the
    // promise should only be completed once we're sure that the
    // container has terminated.
    waitNestedContainer(checkContainerId, nested)
      .onAny([failure, promise](const Future<Option<int>>&) {
        // We assume that once `WaitNestedContainer` returns,
        // irrespective of whether the response contains a failure, the
        // container will be in a terminal state, and that it will be
        // possible to remove it.
        //
        // This means that we don't need to retry the `WaitNestedContainer`
        // call.
        promise->fail(failure);
      });
  } else {
    // The agent was not able to complete the request, discarding the
    // promise signals the checker that it should retry the check.
    //
    // This will allow us to recover from a blip. The executor will
    // pause the checker when it detects that the agent is not
    // available. Here we do not need to wait the check container since
    // the agent may have been unavailable, and when the agent is back,
    // it will destroy the check container as orphan container, and we
    // will eventually remove it in `nestedCommandCheck()`.
    LOG(WARNING) << "Connection to the agent to launch " << name
                 << " for task '" << taskId << "' failed: " << failure;

    promise->discard();
  }
}


Future<Option<int>> CheckerProcess::waitNestedContainer(
    const ContainerID& containerId,
    runtime::Nested nested)
{
  agent::Call call;
  call.set_type(agent::Call::WAIT_NESTED_CONTAINER);

  agent::Call::WaitNestedContainer* containerWait =
    call.mutable_wait_nested_container();

  containerWait->mutable_container_id()->CopyFrom(containerId);

  http::Request request;
  request.method = "POST";
  request.url = nested.agentURL;
  request.body = serialize(ContentType::PROTOBUF, evolve(call));
  request.headers = {{"Accept", stringify(ContentType::PROTOBUF)},
                     {"Content-Type", stringify(ContentType::PROTOBUF)}};

  if (nested.authorizationHeader.isSome()) {
    request.headers["Authorization"] = nested.authorizationHeader.get();
  }

  // TODO(alexr): Use a lambda named capture for
  // this cached value once it is available.
  const string _name = name;

  return http::request(request, false)
    .repair([containerId, _name](const Future<http::Response>& future) {
      return Failure(
          "Connection to wait for " + _name + " container '" +
          stringify(containerId) + "' failed: " + future.failure());
    })
    .then(defer(self(),
                &Self::_waitNestedContainer, containerId, lambda::_1));
}


Future<Option<int>> CheckerProcess::_waitNestedContainer(
    const ContainerID& containerId,
    const http::Response& httpResponse)
{
  if (httpResponse.code != http::Status::OK) {
    return Failure(
        "Received '" + httpResponse.status + "' (" + httpResponse.body +
        ") while waiting on " + name + " container '" +
        stringify(containerId) + "'");
  }

  Try<agent::Response> response =
    deserialize<agent::Response>(ContentType::PROTOBUF, httpResponse.body);
  CHECK_SOME(response);

  CHECK(response->has_wait_nested_container());

  return (
      response->wait_nested_container().has_exit_status()
        ? Option<int>(response->wait_nested_container().exit_status())
        : Option<int>::none());
}


void CheckerProcess::processCommandCheckResult(
    const Stopwatch& stopwatch,
    const Future<int>& future)
{
  CHECK(!future.isPending());

  Result<CheckStatusInfo> result = None();

  // On Posix, `future` corresponds to termination information in the
  // `stat_loc` area. On Windows, `status` is obtained via calling the
  // `GetExitCodeProcess()` function.
  //
  // TODO(alexr): Ensure `WEXITSTATUS` family macros are no-op on Windows,
  // see MESOS-7242.
  if (future.isReady() && WIFEXITED(future.get())) {
    const int exitCode = WEXITSTATUS(future.get());
    LOG(INFO) << name << " for task '" << taskId << "' returned: " << exitCode;

    CheckStatusInfo checkStatusInfo;
    checkStatusInfo.set_type(CheckInfo::COMMAND);
    checkStatusInfo.mutable_command()->set_exit_code(
        static_cast<int32_t>(exitCode));

    result = Result<CheckStatusInfo>(checkStatusInfo);
  } else if (future.isDiscarded()) {
    // Check's status is currently not available due to a transient error,
    // e.g., due to the agent failover, no `CheckStatusInfo` message should
    // be sent to the callback.
    result = None();
  } else {
    result = Result<CheckStatusInfo>(Error(future.failure()));
  }

  processCheckResult(stopwatch, result);
}

static vector<string> httpCheckCommand(
    const string& command,
    const string& url)
{
  return {
    command,
    "-s",                 // Don't show progress meter or error messages.
    "-S",                 // Makes curl show an error message if it fails.
    "-L",                 // Follows HTTP 3xx redirects.
    "-k",                 // Ignores SSL validation when scheme is https.
    "-w", "%{http_code}", // Displays HTTP response code on stdout.
    "-o", os::DEV_NULL,   // Ignores output.
    "-g",                 // Switches off the "URL globbing parser".
    url
  };
}

Future<int> CheckerProcess::httpCheck(
    const check::Http& http,
    const Option<runtime::Plain>& plain)
{
  const string url =
    http.scheme + "://" + http.domain + ":" + stringify(http.port) + http.path;

  return _httpCheck(httpCheckCommand(HTTP_CHECK_COMMAND, url), plain);
}


Future<int> CheckerProcess::_httpCheck(
    const vector<string>& cmdArgv,
    const Option<runtime::Plain>& plain)
{
  VLOG(1) << "Launching " << name << " with command '"
          << strings::join(" ", cmdArgv) << "' for task '" << taskId << "'";

  // TODO(alexr): Consider launching the helper binary once per task lifetime,
  // see MESOS-6766.
  Try<Subprocess> s = process::subprocess(
      cmdArgv[0],
      cmdArgv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      getCustomCloneFunc(plain));

  if (s.isError()) {
    return Failure(
        "Failed to create the " + string(HTTP_CHECK_COMMAND) +
        " subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once it is available.
  const pid_t curlPid = s->pid();
  const string _name = name;
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(
        timeout,
        [timeout, curlPid, _name, _taskId](Future<tuple<Future<Option<int>>,
                                                        Future<string>,
                                                        Future<string>>> future)
    {
      future.discard();

      if (curlPid != -1) {
        // Cleanup the HTTP_CHECK_COMMAND process.
        VLOG(1) << "Killing the " << _name << " process " << curlPid
                << " for task '" << _taskId << "'";

        os::killtree(curlPid, SIGKILL);
      }

      return Failure(
          string(HTTP_CHECK_COMMAND) + " timed out after " +
          stringify(timeout));
    })
    .then(defer(self(), &Self::__httpCheck, lambda::_1));
}


Future<int> CheckerProcess::__httpCheck(
    const tuple<Future<Option<int>>, Future<string>, Future<string>>& t)
{
  const Future<Option<int>>& status = std::get<0>(t);
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
    const Future<string>& commandError = std::get<2>(t);
    if (!commandError.isReady()) {
      return Failure(
          string(HTTP_CHECK_COMMAND) + " " + WSTRINGIFY(exitCode) +
          "; reading stderr failed: " +
          (commandError.isFailed() ? commandError.failure() : "discarded"));
    }

    return Failure(
        string(HTTP_CHECK_COMMAND) + " " + WSTRINGIFY(exitCode) + ": " +
        commandError.get());
  }

  const Future<string>& commandOutput = std::get<1>(t);
  if (!commandOutput.isReady()) {
    return Failure(
        "Failed to read stdout from " + string(HTTP_CHECK_COMMAND) + ": " +
        (commandOutput.isFailed() ? commandOutput.failure() : "discarded"));
  }

  VLOG(1) << "Output of the " << name << " for task '" << taskId
          << "': " << commandOutput.get();

  // Parse the output and get the HTTP status code.
  Try<int> statusCode = numify<int>(strings::trim(commandOutput.get()));
  if (statusCode.isError()) {
    return Failure(
        "Unexpected output from " + string(HTTP_CHECK_COMMAND) + ": " +
        commandOutput.get());
  }

  return statusCode.get();
}


void CheckerProcess::processHttpCheckResult(
    const Stopwatch& stopwatch,
    const Future<int>& future)
{
  CHECK(!future.isPending());

  Result<CheckStatusInfo> result = None();

  if (future.isReady()) {
    LOG(INFO) << name << " for task '" << taskId << "'"
              << " returned: " << future.get();

    CheckStatusInfo checkStatusInfo;
    checkStatusInfo.set_type(CheckInfo::HTTP);
    checkStatusInfo.mutable_http()->set_status_code(
        static_cast<uint32_t>(future.get()));

    result = Result<CheckStatusInfo>(checkStatusInfo);
  } else if (future.isDiscarded()) {
    // Check's status is currently not available due to a transient error,
    // e.g., due to the agent failover, no `CheckStatusInfo` message should
    // be sent to the callback.
    result = None();
  } else {
    result = Result<CheckStatusInfo>(Error(future.failure()));
  }

  processCheckResult(stopwatch, result);
}


#ifdef __WINDOWS__
static vector<string> dockerNetworkRunCommand(
    const runtime::Docker& docker,
    const string& image)
{
  return {
    docker.dockerPath,
    "-H",
    docker.socketName,
    "run",
    "--rm",
    "--network=container:" + docker.containerName,
    image
  };
}


// On Windows, we can't directly change a process's namespace. So, for the
// Docker HTTP checks, we need to use the network=container feature in
// Docker to run the network check in the right namespace.
Future<int> CheckerProcess::dockerHttpCheck(
    const check::Http& http,
    const runtime::Docker& docker)
{
  vector<string> argv =
    dockerNetworkRunCommand(docker, DOCKER_HEALTH_CHECK_IMAGE);

  const string url = http.scheme + "://" + http.domain + ":" +
                     stringify(http.port) + http.path;

  const vector<string> httpCheckCommandParameters =
    httpCheckCommand(HTTP_CHECK_COMMAND, url);

  argv.insert(
      argv.end(),
      httpCheckCommandParameters.cbegin(),
      httpCheckCommandParameters.cend());

  return _httpCheck(argv, runtime::Plain{docker.namespaces, docker.taskPid});
}
#endif // __WINDOWS__


static vector<string> tcpCommand(
  const string& command,
  const string& domain,
  uint16_t port)
{
  return {
    command,
    "--ip=" + domain,
    "--port=" + stringify(port)
  };
}


Future<bool> CheckerProcess::tcpCheck(
    const check::Tcp& tcp,
    const Option<runtime::Plain>& plain)
{
  const string command = path::join(tcp.launcherDir, TCP_CHECK_COMMAND);

  const vector<string> argv =
    tcpCommand(command, tcp.domain, static_cast<uint16_t>(tcp.port));

  return _tcpCheck(argv, plain);
}

Future<bool> CheckerProcess::_tcpCheck(
    const vector<string>& cmdArgv,
    const Option<runtime::Plain>& plain)
{
  VLOG(1) << "Launching " << name << " for task '" << taskId << "'"
          << " with command '" << strings::join(" ", cmdArgv) << "'";

  // TODO(alexr): Consider launching the helper binary once per task lifetime,
  // see MESOS-6766.
  Try<Subprocess> s = subprocess(
      cmdArgv[0],
      cmdArgv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      getCustomCloneFunc(plain));

  if (s.isError()) {
    return Failure(
        "Failed to create the " + cmdArgv[0] + " subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once they are available.
  pid_t commandPid = s->pid();
  const string _name = name;
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(
        timeout, [timeout, commandPid, _name, _taskId](
            Future<tuple<Future<Option<int>>,
            Future<string>,
            Future<string>>> future)
    {
      future.discard();

      if (commandPid != -1) {
        // Cleanup the TCP_CHECK_COMMAND process.
        VLOG(1) << "Killing the " << _name << " process " << commandPid
                << " for task '" << _taskId << "'";

        os::killtree(commandPid, SIGKILL);
      }

      return Failure(
          string(TCP_CHECK_COMMAND) + " timed out after " + stringify(timeout));
    })
    .then(defer(self(), &Self::__tcpCheck, lambda::_1));
}


Future<bool> CheckerProcess::__tcpCheck(
    const tuple<Future<Option<int>>, Future<string>, Future<string>>& t)
{
  const Future<Option<int>>& status = std::get<0>(t);
  if (!status.isReady()) {
    return Failure(
        "Failed to get the exit status of the " + string(TCP_CHECK_COMMAND) +
        " process: " + (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Failure(
        "Failed to reap the " + string(TCP_CHECK_COMMAND) + " process");
  }

  int exitCode = status->get();

  const Future<string>& commandOutput = std::get<1>(t);
  if (commandOutput.isReady()) {
    VLOG(1) << "Output of the " << name << " for task '" << taskId
            << "': " << commandOutput.get();
  }

  if (exitCode != 0) {
    const Future<string>& commandError = std::get<2>(t);
    if (commandError.isReady()) {
      VLOG(1) << string(TCP_CHECK_COMMAND) << ": " << commandError.get();
    }
  }

  // Non-zero exit code of TCP_CHECK_COMMAND can mean configuration problem
  // (e.g., bad command flag), system error (e.g., a socket cannot be
  // created), or actually a failed connection. We cannot distinguish between
  // these cases, hence treat all of them as connection failure.
  return (exitCode == 0 ? true : false);
}


void CheckerProcess::processTcpCheckResult(
    const Stopwatch& stopwatch,
    const Future<bool>& future)
{
  CHECK(!future.isPending());

  Result<CheckStatusInfo> result = None();

  if (future.isReady()) {
    LOG(INFO) << name << " for task '" << taskId << "'"
              << " returned: " << future.get();

    CheckStatusInfo checkStatusInfo;
    checkStatusInfo.set_type(CheckInfo::TCP);
    checkStatusInfo.mutable_tcp()->set_succeeded(future.get());

    result = Result<CheckStatusInfo>(checkStatusInfo);
  } else if (future.isDiscarded()) {
    // Check's status is currently not available due to a transient error,
    // e.g., due to the agent failover, no `CheckStatusInfo` message should
    // be sent to the callback.
    result = None();
  } else {
    result = Result<CheckStatusInfo>(Error(future.failure()));
  }

  processCheckResult(stopwatch, result);
}


#ifdef __WINDOWS__
// On Windows, we can't directly change a process's namespace. So, for the
// Docker TCP checks, we need to use the network=container feature in
// Docker to run the network check in the right namespace.
Future<bool> CheckerProcess::dockerTcpCheck(
    const check::Tcp& tcp,
    const runtime::Docker& docker)
{
  vector<string> argv =
    dockerNetworkRunCommand(docker, DOCKER_HEALTH_CHECK_IMAGE);

  const vector<string> tcpCheckCommandParameters =
    tcpCommand(TCP_CHECK_COMMAND, tcp.domain, static_cast<uint16_t>(tcp.port));

  argv.insert(
      argv.end(),
      tcpCheckCommandParameters.cbegin(),
      tcpCheckCommandParameters.cend());

  return _tcpCheck(argv, runtime::Plain{docker.namespaces, docker.taskPid});
}
#endif // __WINDOWS__

} // namespace checks {
} // namespace internal {
} // namespace mesos {
