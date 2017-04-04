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
#include <stout/uuid.hpp>

#include <stout/os/environment.hpp>
#include <stout/os/killtree.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"
#include "common/validation.hpp"

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

constexpr char DEFAULT_HTTP_SCHEME[] = "http";

// Use '127.0.0.1' instead of 'localhost', because the host
// file in some container images may not contain 'localhost'.
constexpr char DEFAULT_DOMAIN[] = "127.0.0.1";


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
          LOG(FATAL) << "Failed to enter the " << ns << " namespace of task"
                     << " (pid: " << taskPid.get() << "): " << setns.error();
        }

        VLOG(1) << "Entered the " << ns << " namespace of task"
                << " (pid: " << taskPid.get() << ") successfully";
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
      const string& _launcherDir,
      const lambda::function<void(const CheckStatusInfo&)>& _callback,
      const TaskID& _taskId,
      const Option<pid_t>& _taskPid,
      const vector<string>& _namespaces,
      const Option<ContainerID>& _taskContainerId,
      const Option<http::URL>& _agentURL,
      const Option<string>& _authorizationHeader,
      bool _commandCheckViaAgent);

  void pause();
  void resume();

  virtual ~CheckerProcess() {}

protected:
  void initialize() override;
  void finalize() override;

private:
  void performCheck();
  void scheduleNext(const Duration& duration);
  void processCheckResult(
      const Stopwatch& stopwatch,
      const Option<CheckStatusInfo>& result);

  Future<int> commandCheck();

  Future<int> nestedCommandCheck();
  void _nestedCommandCheck(shared_ptr<Promise<int>> promise);
  void __nestedCommandCheck(
      shared_ptr<Promise<int>> promise,
      http::Connection connection);
  void ___nestedCommandCheck(
      shared_ptr<Promise<int>> promise,
      const ContainerID& checkContainerId,
      const http::Response& launchResponse);

  void nestedCommandCheckFailure(
      shared_ptr<Promise<int>> promise,
      http::Connection connection,
      ContainerID checkContainerId,
      shared_ptr<bool> checkTimedOut,
      const string& failure);

  Future<Option<int>> waitNestedContainer(const ContainerID& containerId);
  Future<Option<int>> _waitNestedContainer(
      const ContainerID& containerId,
      const http::Response& httpResponse);

  void processCommandCheckResult(
      const Stopwatch& stopwatch,
      const Future<int>& future);

  Future<int> httpCheck();
  Future<int> _httpCheck(
      const tuple<Future<Option<int>>, Future<string>, Future<string>>& t);
  void processHttpCheckResult(
      const Stopwatch& stopwatch,
      const Future<int>& future);

  Future<bool> tcpCheck();
  Future<bool> _tcpCheck(
      const tuple<Future<Option<int>>, Future<string>, Future<string>>& t);
  void processTcpCheckResult(
      const Stopwatch& stopwatch,
      const Future<bool>& future);

  const CheckInfo check;
  Duration checkDelay;
  Duration checkInterval;
  Duration checkTimeout;

  // Contains the binary for TCP checks.
  const string launcherDir;

  const lambda::function<void(const CheckStatusInfo&)> updateCallback;
  const TaskID taskId;
  const Option<pid_t> taskPid;
  const vector<string> namespaces;
  const Option<ContainerID> taskContainerId;
  const Option<http::URL> agentURL;
  const Option<std::string> authorizationHeader;
  const bool commandCheckViaAgent;

  Option<lambda::function<pid_t(const lambda::function<int()>&)>> clone;

  CheckStatusInfo previousCheckStatus;
  bool paused;

  // Contains the ID of the most recently terminated nested container
  // that was used to perform a COMMAND check.
  Option<ContainerID> previousCheckContainerId;
};


Try<Owned<Checker>> Checker::create(
    const CheckInfo& check,
    const string& launcherDir,
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
      launcherDir,
      callback,
      taskId,
      taskPid,
      namespaces,
      None(),
      None(),
      None(),
      false));

  return Owned<Checker>(new Checker(process));
}


Try<Owned<Checker>> Checker::create(
    const CheckInfo& check,
    const string& launcherDir,
    const lambda::function<void(const CheckStatusInfo&)>& callback,
    const TaskID& taskId,
    const ContainerID& taskContainerId,
    const http::URL& agentURL,
    const Option<string>& authorizationHeader)
{
  // Validate the `CheckInfo` protobuf.
  Option<Error> error = validation::checkInfo(check);
  if (error.isSome()) {
    return error.get();
  }

  Owned<CheckerProcess> process(new CheckerProcess(
      check,
      launcherDir,
      callback,
      taskId,
      None(),
      {},
      taskContainerId,
      agentURL,
      authorizationHeader,
      true));

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


void Checker::pause()
{
  dispatch(process.get(), &CheckerProcess::pause);
}


void Checker::resume()
{
  dispatch(process.get(), &CheckerProcess::resume);
}


CheckerProcess::CheckerProcess(
    const CheckInfo& _check,
    const string& _launcherDir,
    const lambda::function<void(const CheckStatusInfo&)>& _callback,
    const TaskID& _taskId,
    const Option<pid_t>& _taskPid,
    const vector<string>& _namespaces,
    const Option<ContainerID>& _taskContainerId,
    const Option<http::URL>& _agentURL,
    const Option<std::string>& _authorizationHeader,
    bool _commandCheckViaAgent)
  : ProcessBase(process::ID::generate("checker")),
    check(_check),
    launcherDir(_launcherDir),
    updateCallback(_callback),
    taskId(_taskId),
    taskPid(_taskPid),
    namespaces(_namespaces),
    taskContainerId(_taskContainerId),
    agentURL(_agentURL),
    authorizationHeader(_authorizationHeader),
    commandCheckViaAgent(_commandCheckViaAgent),
    paused(false)
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
    case CheckInfo::TCP: {
      previousCheckStatus.mutable_tcp();
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
  VLOG(1) << "Check configuration for task '" << taskId << "':"
          << " '" << jsonify(JSON::Protobuf(check)) << "'";

  scheduleNext(checkDelay);
}


void CheckerProcess::finalize()
{
  LOG(INFO) << "Checking for task '" << taskId << "' stopped";
}


void CheckerProcess::performCheck()
{
  if (paused) {
    return;
  }

  Stopwatch stopwatch;
  stopwatch.start();

  switch (check.type()) {
    case CheckInfo::COMMAND: {
      Future<int> future = commandCheckViaAgent ? nestedCommandCheck()
                                                : commandCheck();
      future.onAny(defer(
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
    case CheckInfo::TCP: {
      tcpCheck().onAny(defer(
          self(),
          &Self::processTcpCheckResult, stopwatch, lambda::_1));
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
  CHECK(!paused);

  VLOG(1) << "Scheduling check for task '" << taskId << "' in " << duration;

  delay(duration, self(), &Self::performCheck);
}


void CheckerProcess::pause()
{
  if (!paused) {
    VLOG(1) << "Checking for task '" << taskId << "' paused";

    paused = true;
  }
}


void CheckerProcess::resume()
{
  if (paused) {
    VLOG(1) << "Checking for task '" << taskId << "' resumed";

    paused = false;

    // Schedule a check immediately.
    scheduleNext(Duration::zero());
  }
}

void CheckerProcess::processCheckResult(
    const Stopwatch& stopwatch,
    const Option<CheckStatusInfo>& result)
{
  // `Checker` might have been paused while performing the check.
  if (paused) {
    LOG(INFO) << "Ignoring " << check.type() << " check result for"
              << " task '" << taskId << "': checking is paused";
    return;
  }

  // `result` should be some if it was possible to perform the check,
  // and empty if there was a transient error.
  if (result.isSome()) {
    VLOG(1) << "Performed " << check.type() << " check"
            << " for task '" << taskId << "' in " << stopwatch.elapsed();

    // Trigger the callback if check info changes.
    if (result.get() != previousCheckStatus) {
      // We assume this is a local send, i.e., the checker library is not used
      // in a binary external to the executor and hence can not exit before
      // the data is sent to the executor.
      updateCallback(result.get());
      previousCheckStatus = result.get();
    }
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
    VLOG(1) << "Launching COMMAND check '" << command.value() << "'"
            << " for task '" << taskId << "'";

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

    VLOG(1) << "Launching COMMAND check [" << command.value() << ", "
            << strings::join(", ", argv) << "] for task '" << taskId << "'";

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
        VLOG(1) << "Killing the COMMAND check process '" << commandPid
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


Future<int> CheckerProcess::nestedCommandCheck()
{
  CHECK_EQ(CheckInfo::COMMAND, check.type());
  CHECK(check.has_command());
  CHECK_SOME(taskContainerId);
  CHECK_SOME(agentURL);

  VLOG(1) << "Launching COMMAND check for task '" << taskId << "'";

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

    agent::Call::RemoveNestedContainer* removeContainer =
      call.mutable_remove_nested_container();

    removeContainer->mutable_container_id()->CopyFrom(
        previousCheckContainerId.get());

    http::Request request;
    request.method = "POST";
    request.url = agentURL.get();
    request.body = serialize(ContentType::PROTOBUF, evolve(call));
    request.headers = {{"Accept", stringify(ContentType::PROTOBUF)},
                       {"Content-Type", stringify(ContentType::PROTOBUF)}};

    if (authorizationHeader.isSome()) {
      request.headers["Authorization"] = authorizationHeader.get();
    }

    http::request(request, false)
      .onFailed(defer(self(),
                      [this, promise](const string& failure) {
        LOG(WARNING) << "Connection to remove the nested container '"
                     << previousCheckContainerId.get()
                     << "' used for the COMMAND check for task '"
                     << taskId << "' failed: " << failure;

        // Something went wrong while sending the request, we treat this
        // as a transient failure and discard the promise.
        promise->discard();
      }))
      .onReady(defer(self(), [this, promise](const http::Response& response) {
        if (response.code != http::Status::OK) {
          // The agent was unable to remove the check container, we
          // treat this as a transient failure and discard the promise.
          LOG(WARNING) << "Received '" << response.status << "' ("
                       << response.body << ") while removing the nested"
                       << " container '" << previousCheckContainerId.get()
                       << "' used for the COMMAND check for task '"
                       << taskId << "'";

          promise->discard();
        }

        previousCheckContainerId = None();
        _nestedCommandCheck(promise);
      }));
  } else {
    _nestedCommandCheck(promise);
  }

  return promise->future();
}


void CheckerProcess::_nestedCommandCheck(shared_ptr<Promise<int>> promise)
{
  // TODO(alexr): Use a lambda named capture for
  // this cached value once it is available.
  const TaskID _taskId = taskId;

  http::connect(agentURL.get())
    .onFailed(defer(self(), [_taskId, promise](const string& failure) {
      LOG(WARNING) << "Unable to establish connection with the agent to launch"
                   << " COMMAND check for task '" << _taskId << "'"
                   << ": " << failure;

      // We treat this as a transient failure.
      promise->discard();
    }))
    .onReady(defer(self(), &Self::__nestedCommandCheck, promise, lambda::_1));
}


void CheckerProcess::__nestedCommandCheck(
    shared_ptr<Promise<int>> promise,
    http::Connection connection)
{
  ContainerID checkContainerId;
  checkContainerId.set_value("check-" + UUID::random().toString());
  checkContainerId.mutable_parent()->CopyFrom(taskContainerId.get());

  previousCheckContainerId = checkContainerId;

  CommandInfo command(check.command().command());

  agent::Call call;
  call.set_type(agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  agent::Call::LaunchNestedContainerSession* launch =
    call.mutable_launch_nested_container_session();

  launch->mutable_container_id()->CopyFrom(checkContainerId);
  launch->mutable_command()->CopyFrom(command);

  http::Request request;
  request.method = "POST";
  request.url = agentURL.get();
  request.body = serialize(ContentType::PROTOBUF, evolve(call));
  request.headers = {{"Accept", stringify(ContentType::RECORDIO)},
                     {"Message-Accept", stringify(ContentType::PROTOBUF)},
                     {"Content-Type", stringify(ContentType::PROTOBUF)}};

  if (authorizationHeader.isSome()) {
    request.headers["Authorization"] = authorizationHeader.get();
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
                    lambda::_1))
    .onReady(defer(self(),
                   &Self::___nestedCommandCheck,
                   promise,
                   checkContainerId,
                   lambda::_1));
}


void CheckerProcess::___nestedCommandCheck(
    shared_ptr<Promise<int>> promise,
    const ContainerID& checkContainerId,
    const http::Response& launchResponse)
{
  if (launchResponse.code != http::Status::OK) {
    // The agent was unable to launch the check container,
    // we treat this as a transient failure.
    LOG(WARNING) << "Received '" << launchResponse.status << "' ("
                 << launchResponse.body << ") while launching COMMAND check"
                 << " for task '" << taskId << "'";

    promise->discard();
    return;
  }

  waitNestedContainer(checkContainerId)
    .onFailed([promise](const string& failure) {
      promise->fail(
          "Unable to get the exit code: " + failure);
    })
    .onReady([promise](const Option<int>& status) -> void {
      if (status.isNone()) {
        promise->fail("Unable to get the exit code");
      // TODO(gkleiman): Make sure that the following block works on Windows.
      } else if (WIFSIGNALED(status.get()) &&
                 WTERMSIG(status.get()) == SIGKILL) {
        // The check container was signaled, probably because the task
        // finished while the check was still in-flight, so we discard
        // the result.
        promise->discard();
      } else {
        promise->set(status.get());
      }
    });
}


void CheckerProcess::nestedCommandCheckFailure(
    shared_ptr<Promise<int>> promise,
    http::Connection connection,
    ContainerID checkContainerId,
    shared_ptr<bool> checkTimedOut,
    const string& failure)
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
    waitNestedContainer(checkContainerId)
      .onAny([failure, promise](const Future<Option<int>>&) {
        // We assume that once `WaitNestedContainer` returns,
        // irrespective of whether the response contains a failure, the
        // container will be in a terminal state, and that it will be
        // possible to remove it.
        //
        // This means that we don't need to retry the
        // `WaitNestedContainer` call.
        promise->fail(failure);
      });
  } else {
    // The agent was not able to complete the request, discarding the
    // promise signals the checker that it should retry the check.
    //
    // This will allow us to recover from a blip. The executor will
    // pause the checker when it detects that the agent is not
    // available.
    LOG(WARNING) << "Connection to the agent to launch COMMAND check"
                 << " for task '" << taskId << "' failed: " << failure;

    promise->discard();
  }
}


Future<Option<int>> CheckerProcess::waitNestedContainer(
    const ContainerID& containerId)
{
  agent::Call call;
  call.set_type(agent::Call::WAIT_NESTED_CONTAINER);

  agent::Call::WaitNestedContainer* containerWait =
    call.mutable_wait_nested_container();

  containerWait->mutable_container_id()->CopyFrom(containerId);

  http::Request request;
  request.method = "POST";
  request.url = agentURL.get();
  request.body = serialize(ContentType::PROTOBUF, evolve(call));
  request.headers = {{"Accept", stringify(ContentType::PROTOBUF)},
                     {"Content-Type", stringify(ContentType::PROTOBUF)}};

  if (authorizationHeader.isSome()) {
    request.headers["Authorization"] = authorizationHeader.get();
  }

  return http::request(request, false)
    .repair([containerId](const Future<http::Response>& future) {
      return Failure(
          "Connection to wait for check container '" +
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
        ") while waiting on check container '" + stringify(containerId) + "'");
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
  Option<CheckStatusInfo> result;

  // On Posix, `future` corresponds to termination information in the
  // `stat_loc` area. On Windows, `status` is obtained via calling the
  // `GetExitCodeProcess()` function.
  //
  // TODO(alexr): Ensure `WEXITSTATUS` family macros are no-op on Windows,
  // see MESOS-7242.
  if (future.isReady() && WIFEXITED(future.get())) {
    const int exitCode = WEXITSTATUS(future.get());
    VLOG(1) << check.type() << " check for task '" << taskId << "'"
            << " returned: " << exitCode;

    CheckStatusInfo checkStatusInfo;
    checkStatusInfo.set_type(check.type());
    checkStatusInfo.mutable_command()->set_exit_code(
        static_cast<int32_t>(exitCode));

    result = checkStatusInfo;
  } else if (future.isDiscarded()) {
    // Check's status is currently not available due to a transient error,
    // e.g., due to the agent failover, no `CheckStatusInfo` message should
    // be sent to the callback.
    LOG(INFO) << check.type() << " check for task '" << taskId << "' discarded";

    result = None();
  } else {
    // Check's status is currently not available, which may indicate a change
    // that should be reported as an empty `CheckStatusInfo.Command` message.
    LOG(WARNING) << check.type() << " check for task '" << taskId << "'"
                 << " failed: " << future.failure();

    CheckStatusInfo checkStatusInfo;
    checkStatusInfo.set_type(check.type());
    checkStatusInfo.mutable_command();

    result = checkStatusInfo;
  }

  processCheckResult(stopwatch, result);
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

  VLOG(1) << "Launching HTTP check '" << url << "' for task '" << taskId << "'";

  const vector<string> argv = {
    HTTP_CHECK_COMMAND,
    "-s",                 // Don't show progress meter or error messages.
    "-S",                 // Makes curl show an error message if it fails.
    "-L",                 // Follows HTTP 3xx redirects.
    "-k",                 // Ignores SSL validation when scheme is https.
    "-w", "%{http_code}", // Displays HTTP response code on stdout.
    "-o", os::DEV_NULL,   // Ignores output.
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
                << " for task '" << _taskId << "'";

        os::killtree(curlPid, SIGKILL);
      }

      return Failure(
          string(HTTP_CHECK_COMMAND) + " timed out after " +
          stringify(timeout));
    })
    .then(defer(self(), &Self::_httpCheck, lambda::_1));
}


Future<int> CheckerProcess::_httpCheck(
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
    const Future<string>& error = std::get<2>(t);
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

  const Future<string>& output = std::get<1>(t);
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
    const Future<int>& future)
{
  CheckStatusInfo result;
  result.set_type(check.type());

  if (future.isReady()) {
    VLOG(1) << check.type() << " check for task '"
            << taskId << "' returned: " << future.get();

    result.mutable_http()->set_status_code(static_cast<uint32_t>(future.get()));
  } else {
    // Check's status is currently not available, which may indicate a change
    // that should be reported as an empty `CheckStatusInfo.Http` message.
    LOG(WARNING) << check.type() << " check for task '" << taskId << "' failed:"
                 << " " << (future.isFailed() ? future.failure() : "discarded");

    result.mutable_http();
  }

  processCheckResult(stopwatch, result);
}


Future<bool> CheckerProcess::tcpCheck()
{
  CHECK_EQ(CheckInfo::TCP, check.type());
  CHECK(check.has_tcp());

  // TCP_CHECK_COMMAND should be reachable.
  CHECK(os::exists(launcherDir));

  const CheckInfo::Tcp& tcp = check.tcp();

  VLOG(1) << "Launching TCP check for task '" << taskId << "' at port "
          << tcp.port();

  const string command = path::join(launcherDir, TCP_CHECK_COMMAND);

  const vector<string> argv = {
    command,
    "--ip=" + stringify(DEFAULT_DOMAIN),
    "--port=" + stringify(tcp.port())
  };

  // TODO(alexr): Consider launching the helper binary once per task lifetime,
  // see MESOS-6766.
  Try<Subprocess> s = subprocess(
      command,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      clone);

  if (s.isError()) {
    return Failure(
        "Failed to create the " + command + " subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once they are available.
  pid_t commandPid = s->pid();
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(
        timeout,
        [timeout, commandPid, _taskId](Future<tuple<Future<Option<int>>,
                                                    Future<string>,
                                                    Future<string>>> future)
    {
      future.discard();

      if (commandPid != -1) {
        // Cleanup the TCP_CHECK_COMMAND process.
        VLOG(1) << "Killing the TCP check process " << commandPid
                << " for task '" << _taskId << "'";

        os::killtree(commandPid, SIGKILL);
      }

      return Failure(
          string(TCP_CHECK_COMMAND) + " timed out after " + stringify(timeout));
    })
    .then(defer(self(), &Self::_tcpCheck, lambda::_1));
}


Future<bool> CheckerProcess::_tcpCheck(
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
    VLOG(1) << string(TCP_CHECK_COMMAND) << ": " << commandOutput.get();
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
  CheckStatusInfo result;
  result.set_type(check.type());

  if (future.isReady()) {
    VLOG(1) << check.type() << " check for task '"
            << taskId << "' returned: " << stringify(future.get());

    result.mutable_tcp()->set_succeeded(future.get());
  } else {
    // Check's status is currently not available, which may indicate a change
    // that should be reported as an empty `CheckStatusInfo.Tcp` message.
    LOG(WARNING) << check.type() << " check for task '" << taskId << "' failed:"
                 << " " << (future.isFailed() ? future.failure() : "discarded");

    result.mutable_tcp();
  }

  processCheckResult(stopwatch, result);
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
        return Error("Expecting 'command' to be set for COMMAND check");
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
            "The path '" + http.path() + "' of HTTP check must start with '/'");
      }

      break;
    }
    case CheckInfo::TCP: {
      if (!checkInfo.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP check");
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
            "Expecting 'command' to be set for COMMAND check's status");
      }
      break;
    }
    case CheckInfo::HTTP: {
      if (!checkStatusInfo.has_http()) {
        return Error("Expecting 'http' to be set for HTTP check's status");
      }
      break;
    }
    case CheckInfo::TCP: {
      if (!checkStatusInfo.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP check's status");
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
