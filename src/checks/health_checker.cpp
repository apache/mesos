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

#ifndef __WINDOWS__
constexpr char TCP_CHECK_COMMAND[] = "mesos-tcp-connect";
constexpr char HTTP_CHECK_COMMAND[] = "curl";
#else
constexpr char TCP_CHECK_COMMAND[] = "mesos-tcp-connect.exe";
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
          // This effectively aborts the health check.
          LOG(FATAL) << "Failed to enter the " << ns << " namespace of task"
                     << " (pid: '" << taskPid.get() << "'): " << setns.error();
        }

        VLOG(1) << "Entered the " << ns << " namespace of task"
                << " (pid: '" << taskPid.get() << "') successfully";
      }
    }

    return func();
  });
}
#endif


Try<Owned<HealthChecker>> HealthChecker::create(
    const HealthCheck& check,
    const string& launcherDir,
    const lambda::function<void(const TaskHealthStatus&)>& callback,
    const TaskID& taskId,
    const Option<pid_t>& taskPid,
    const vector<string>& namespaces)
{
  // Validate the 'HealthCheck' protobuf.
  Option<Error> error = validation::healthCheck(check);
  if (error.isSome()) {
    return error.get();
  }

  Owned<HealthCheckerProcess> process(new HealthCheckerProcess(
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

  return Owned<HealthChecker>(new HealthChecker(process));
}


Try<Owned<HealthChecker>> HealthChecker::create(
    const HealthCheck& check,
    const string& launcherDir,
    const lambda::function<void(const TaskHealthStatus&)>& callback,
    const TaskID& taskId,
    const ContainerID& taskContainerId,
    const process::http::URL& agentURL,
    const Option<string>& authorizationHeader)
{
  // Validate the 'HealthCheck' protobuf.
  Option<Error> error = validation::healthCheck(check);
  if (error.isSome()) {
    return error.get();
  }

  Owned<HealthCheckerProcess> process(new HealthCheckerProcess(
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


void HealthChecker::pause()
{
  dispatch(process.get(), &HealthCheckerProcess::pause);
}


void HealthChecker::resume()
{
  dispatch(process.get(), &HealthCheckerProcess::resume);
}


HealthCheckerProcess::HealthCheckerProcess(
    const HealthCheck& _check,
    const string& _launcherDir,
    const lambda::function<void(const TaskHealthStatus&)>& _callback,
    const TaskID& _taskId,
    const Option<pid_t>& _taskPid,
    const vector<string>& _namespaces,
    const Option<ContainerID>& _taskContainerId,
    const Option<process::http::URL>& _agentURL,
    const Option<string>& _authorizationHeader,
    bool _commandCheckViaAgent)
  : ProcessBase(process::ID::generate("health-checker")),
    check(_check),
    launcherDir(_launcherDir),
    healthUpdateCallback(_callback),
    taskId(_taskId),
    taskPid(_taskPid),
    namespaces(_namespaces),
    taskContainerId(_taskContainerId),
    agentURL(_agentURL),
    authorizationHeader(_authorizationHeader),
    commandCheckViaAgent(_commandCheckViaAgent),
    consecutiveFailures(0),
    initializing(true),
    paused(false)
{
  Try<Duration> create = Duration::create(check.delay_seconds());
  CHECK_SOME(create);
  checkDelay = create.get();

  create = Duration::create(check.interval_seconds());
  CHECK_SOME(create);
  checkInterval = create.get();

  create = Duration::create(check.grace_period_seconds());
  CHECK_SOME(create);
  checkGracePeriod = create.get();

  // Zero value means infinite timeout.
  create = Duration::create(check.timeout_seconds());
  CHECK_SOME(create);
  checkTimeout =
    (create.get() > Duration::zero()) ? create.get() : Duration::max();

#ifdef __linux__
  if (!namespaces.empty()) {
    clone = lambda::bind(&cloneWithSetns, lambda::_1, taskPid, namespaces);
  }
#endif
}


void HealthCheckerProcess::initialize()
{
  VLOG(1) << "Health check configuration for task '" << taskId << "':"
          << " '" << jsonify(JSON::Protobuf(check)) << "'";

  startTime = Clock::now();

  scheduleNext(checkDelay);
}


void HealthCheckerProcess::failure(const string& message)
{
  if (initializing &&
      checkGracePeriod.secs() > 0 &&
      (Clock::now() - startTime) <= checkGracePeriod) {
    LOG(INFO) << "Ignoring failure of "
              << HealthCheck::Type_Name(check.type()) << " health check for"
              << " task '" << taskId << "': still in grace period";
    scheduleNext(checkInterval);
    return;
  }

  consecutiveFailures++;
  LOG(WARNING) << HealthCheck::Type_Name(check.type())
               << " health check for task '" << taskId << "' failed "
               << consecutiveFailures << " times consecutively: " << message;

  bool killTask = consecutiveFailures >= check.consecutive_failures();

  TaskHealthStatus taskHealthStatus;
  taskHealthStatus.set_healthy(false);
  taskHealthStatus.set_consecutive_failures(consecutiveFailures);
  taskHealthStatus.set_kill_task(killTask);
  taskHealthStatus.mutable_task_id()->CopyFrom(taskId);

  // We assume this is a local send, i.e. the health checker library
  // is not used in a binary external to the executor and hence can
  // not exit before the data is sent to the executor.
  healthUpdateCallback(taskHealthStatus);

  // Even if we set the `kill_task` flag, it is an executor who kills the task
  // and honors the flag (or not). We have no control over the task's lifetime,
  // hence we should continue until we are explicitly asked to stop.
  scheduleNext(checkInterval);
}


void HealthCheckerProcess::success()
{
  VLOG(1) << HealthCheck::Type_Name(check.type()) << " health check for task '"
          << taskId << "' passed";

  // Send a healthy status update on the first success,
  // and on the first success following failure(s).
  if (initializing || consecutiveFailures > 0) {
    TaskHealthStatus taskHealthStatus;
    taskHealthStatus.set_healthy(true);
    taskHealthStatus.mutable_task_id()->CopyFrom(taskId);
    healthUpdateCallback(taskHealthStatus);
    initializing = false;
  }

  consecutiveFailures = 0;
  scheduleNext(checkInterval);
}


void HealthCheckerProcess::performSingleCheck()
{
  if (paused) {
    return;
  }

  Future<Nothing> checkResult;

  Stopwatch stopwatch;
  stopwatch.start();

  switch (check.type()) {
    case HealthCheck::COMMAND: {
      checkResult = commandCheckViaAgent ? nestedCommandHealthCheck()
                                         : commandHealthCheck();
      break;
    }

    case HealthCheck::HTTP: {
      checkResult = httpHealthCheck();
      break;
    }

    case HealthCheck::TCP: {
      checkResult = tcpHealthCheck();
      break;
    }

    default: {
      UNREACHABLE();
    }
  }

  checkResult.onAny(defer(
      self(),
      &Self::processCheckResult, stopwatch, lambda::_1));
}


void HealthCheckerProcess::processCheckResult(
    const Stopwatch& stopwatch,
    const Future<Nothing>& future)
{
  // `HealthChecker` might have been paused while performing the check.
  if (paused) {
    LOG(INFO) << "Ignoring " << HealthCheck::Type_Name(check.type())
              << " health check result for task '" << taskId
              << "': health checking is paused";
    return;
  }

  if (future.isDiscarded()) {
    LOG(INFO) << HealthCheck::Type_Name(check.type()) << " health check for"
              << " task '" << taskId << "' discarded";
    scheduleNext(checkInterval);
    return;
  }

  VLOG(1) << "Performed " << HealthCheck::Type_Name(check.type())
          << " health check for task '" << taskId << "' in "
          << stopwatch.elapsed();

  if (future.isReady()) {
    success();
    return;
  }

  string message = HealthCheck::Type_Name(check.type()) +
                   " health check for task '" + stringify(taskId) +
                   "' failed: " + future.failure();

  failure(message);
}


Future<Nothing> HealthCheckerProcess::commandHealthCheck()
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
    VLOG(1) << "Launching COMMAND health check '" << command.value() << "'"
            << " for task '" << taskId << "'";

    external = subprocess(
        command.value(),
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        environment,
        clone);
  } else {
    // Use the exec variant.
    vector<string> argv;
    foreach (const string& arg, command.arguments()) {
      argv.push_back(arg);
    }

    VLOG(1) << "Launching COMMAND health check [" << command.value() << ", "
            << strings::join(", ", argv) << "] for task '" << taskId << "'";

    external = subprocess(
        command.value(),
        argv,
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        nullptr,
        environment,
        clone);
  }

  if (external.isError()) {
    return Failure("Failed to create subprocess: " + external.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once they are available.
  pid_t commandPid = external->pid();
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return external->status()
    .after(
        timeout,
        [timeout, commandPid, _taskId](Future<Option<int>> future) {
      future.discard();

      if (commandPid != -1) {
        // Cleanup the external command process.
        VLOG(1) << "Killing the COMMAND health check process '" << commandPid
                << "' for task '" << _taskId << "'";

        os::killtree(commandPid, SIGKILL);
      }

      return Failure("Command timed out after " + stringify(timeout));
    })
    .then([](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("Failed to reap the command process");
      }

      int statusCode = status.get();
      if (statusCode != 0) {
        return Failure("Command returned: " + WSTRINGIFY(statusCode));
      }

      return Nothing();
    });
}


Future<Nothing> HealthCheckerProcess::nestedCommandHealthCheck()
{
  CHECK_EQ(HealthCheck::COMMAND, check.type());
  CHECK_SOME(taskContainerId);
  CHECK(check.has_command());
  CHECK_SOME(agentURL);

  VLOG(1) << "Launching COMMAND health check for task '" << taskId << "'";

  // We don't want recoverable errors, e.g., the agent responding with
  // HTTP status code 503, to trigger a health check failure.
  //
  // The future returned by this method represents the result of a
  // health check. It will be set to `Nothing` if the check succeeded,
  // to a `Failure` if it failed, and discarded if there was a transient
  // error.
  auto promise = std::make_shared<Promise<Nothing>>();

  if (previousCheckContainerId.isSome()) {
    agent::Call call;
    call.set_type(agent::Call::REMOVE_NESTED_CONTAINER);

    agent::Call::RemoveNestedContainer* removeContainer =
      call.mutable_remove_nested_container();

    removeContainer->mutable_container_id()->CopyFrom(
        previousCheckContainerId.get());

    process::http::Request request;
    request.method = "POST";
    request.url = agentURL.get();
    request.body = serialize(ContentType::PROTOBUF, evolve(call));
    request.headers = {{"Accept", stringify(ContentType::PROTOBUF)},
                       {"Content-Type", stringify(ContentType::PROTOBUF)}};

    if (authorizationHeader.isSome()) {
      request.headers["Authorization"] = authorizationHeader.get();
    }

    process::http::request(request, false)
      .onFailed(defer(self(),
                      [this, promise](const string& failure) {
        LOG(WARNING) << "Connection to remove the nested container '"
                     << previousCheckContainerId.get()
                     << "' used for the COMMAND health check for task '"
                     << taskId << "' failed: " << failure;

        // Something went wrong while sending the request, we treat this
        // as a transient failure and discard the promise.
        promise->discard();
      }))
      .onReady(defer(self(), [this, promise](const Response& response) {
        if (response.code != process::http::Status::OK) {
          // The agent was unable to remove the health check container,
          // we treat this as a transient failure and discard the promise.
          LOG(WARNING) << "Received '" << response.status << "' ("
                       << response.body << ") while removing the nested"
                       << " container '" << previousCheckContainerId.get()
                       << "' used for the COMMAND health check for task '"
                       << taskId << "'";

          promise->discard();
        }

        previousCheckContainerId = None();
        _nestedCommandHealthCheck(promise);
      }));
  } else {
    _nestedCommandHealthCheck(promise);
  }

  return promise->future();
}


void HealthCheckerProcess::_nestedCommandHealthCheck(
    shared_ptr<process::Promise<Nothing>> promise)
{
  // TODO(alexr): Use a lambda named capture for
  // this cached value once it is available.
  const TaskID _taskId = taskId;

  process::http::connect(agentURL.get())
    .onFailed(defer(self(), [_taskId, promise](const string& failure) {
      LOG(WARNING) << "Unable to establish connection with the agent to launch"
                   << " COMMAND health check for task '" << _taskId
                   << "': " << failure;

      // We treat this as a transient failure.
      promise->discard();
    }))
    .onReady(defer(self(),
                   &Self::__nestedCommandHealthCheck, promise, lambda::_1));
}


void HealthCheckerProcess::__nestedCommandHealthCheck(
    shared_ptr<process::Promise<Nothing>> promise,
    Connection connection)
{
  ContainerID checkContainerId;
  checkContainerId.set_value("health-check-" + UUID::random().toString());
  checkContainerId.mutable_parent()->CopyFrom(taskContainerId.get());

  previousCheckContainerId = checkContainerId;

  CommandInfo command(check.command());

  agent::Call call;
  call.set_type(agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  agent::Call::LaunchNestedContainerSession* launch =
    call.mutable_launch_nested_container_session();

  launch->mutable_container_id()->CopyFrom(checkContainerId);
  launch->mutable_command()->CopyFrom(command);

  process::http::Request request;
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
  // health check command has finished or the connection has been
  // closed.
  connection.send(request, false)
    .after(checkTimeout,
           defer(self(), [timeout, checkTimedOut](Future<Response> future) {
      future.discard();

      *checkTimedOut = true;

      return Failure("Command timed out after " + stringify(timeout));
    }))
    .onFailed(defer(self(),
                    &Self::nestedCommandHealthCheckFailure,
                    promise,
                    connection,
                    checkContainerId,
                    checkTimedOut,
                    lambda::_1))
    .onReady(defer(self(),
                   &Self::___nestedCommandHealthCheck,
                   promise,
                   checkContainerId,
                   lambda::_1));
}


void HealthCheckerProcess::___nestedCommandHealthCheck(
    shared_ptr<process::Promise<Nothing>> promise,
    const ContainerID& checkContainerId,
    const Response& launchResponse)
{
  if (launchResponse.code != process::http::Status::OK) {
    // The agent was unable to launch the health check container, we
    // treat this as a transient failure.
    LOG(WARNING) << "Received '" << launchResponse.status << "' ("
                 << launchResponse.body << ") while launching COMMAND health"
                 << " check for task '" << taskId << "'";

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
      } else if (status.get() != 0) {
        promise->fail("Command returned: " + WSTRINGIFY(status.get()));
      } else {
        promise->set(Nothing());
      }
    });
}


void HealthCheckerProcess::nestedCommandHealthCheckFailure(
    shared_ptr<Promise<Nothing>> promise,
    Connection connection,
    ContainerID checkContainerId,
    shared_ptr<bool> checkTimedOut,
    const string& failure)
{
  if (*checkTimedOut) {
    // The health check timed out, closing the connection will make the
    // agent kill the container.
    connection.disconnect();

    // If the health check delay interval is zero, we'll try to perform
    // another health check right after we finish processing the current
    // timeout.
    //
    // We'll try to remove the container created for the health check at
    // the beginning of the next check. In order to prevent a failure,
    // the promise should only be completed once we're sure that the
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
    // promise signals the health checker that it should retry the
    // health check.
    //
    // This will allow us to recover from a blip. The executor will
    // pause the health checker when it detects that the agent is not
    // available.
    LOG(WARNING) << "Connection to the agent to launch COMMAND health check"
                 << " for task '" << taskId << "' failed: " << failure;

    promise->discard();
  }
}


Future<Option<int>> HealthCheckerProcess::waitNestedContainer(
    const ContainerID& containerId)
{
  agent::Call call;
  call.set_type(agent::Call::WAIT_NESTED_CONTAINER);

  agent::Call::WaitNestedContainer* containerWait =
    call.mutable_wait_nested_container();

  containerWait->mutable_container_id()->CopyFrom(containerId);

  process::http::Request request;
  request.method = "POST";
  request.url = agentURL.get();
  request.body = serialize(ContentType::PROTOBUF, evolve(call));
  request.headers = {{"Accept", stringify(ContentType::PROTOBUF)},
                     {"Content-Type", stringify(ContentType::PROTOBUF)}};

  if (authorizationHeader.isSome()) {
    request.headers["Authorization"] = authorizationHeader.get();
  }

  return process::http::request(request, false)
    .repair([containerId](const Future<Response>& future) {
      return Failure(
          "Connection to wait for health check container '" +
          stringify(containerId) + "' failed: " + future.failure());
    })
    .then(defer(self(),
                &Self::_waitNestedContainer, containerId, lambda::_1));
}


Future<Option<int>> HealthCheckerProcess::_waitNestedContainer(
    const ContainerID& containerId,
    const Response& httpResponse)
{
  if (httpResponse.code != process::http::Status::OK) {
    return Failure(
        "Received '" + httpResponse.status + "' (" + httpResponse.body +
        ") while waiting on health check container '" +
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


Future<Nothing> HealthCheckerProcess::httpHealthCheck()
{
  CHECK_EQ(HealthCheck::HTTP, check.type());
  CHECK(check.has_http());

  const HealthCheck::HTTPCheckInfo& http = check.http();

  const string scheme = http.has_scheme() ? http.scheme() : DEFAULT_HTTP_SCHEME;
  const string path = http.has_path() ? http.path() : "";
  const string url = scheme + "://" + DEFAULT_DOMAIN + ":" +
                     stringify(http.port()) + path;

  VLOG(1) << "Launching HTTP health check '" << url << "'"
          << " for task '" << taskId << "'";

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
  Try<Subprocess> s = subprocess(
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
  // these cached values once they are available.
  pid_t curlPid = s->pid();
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
        VLOG(1) << "Killing the HTTP health check process '" << curlPid
                << "' for task '" << _taskId << "'";

        os::killtree(curlPid, SIGKILL);
      }

      return Failure(
          string(HTTP_CHECK_COMMAND) + " timed out after " +
          stringify(timeout));
    })
    .then(defer(self(), &Self::_httpHealthCheck, lambda::_1));
}


Future<Nothing> HealthCheckerProcess::_httpHealthCheck(
    const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t)
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

  int statusCode = status->get();
  if (statusCode != 0) {
    const Future<string>& error = std::get<2>(t);
    if (!error.isReady()) {
      return Failure(
          string(HTTP_CHECK_COMMAND) + " returned " +
          WSTRINGIFY(statusCode) + "; reading stderr failed: " +
          (error.isFailed() ? error.failure() : "discarded"));
    }

    return Failure(
        string(HTTP_CHECK_COMMAND) + " returned " +
        WSTRINGIFY(statusCode) + ": " + error.get());
  }

  const Future<string>& output = std::get<1>(t);
  if (!output.isReady()) {
    return Failure(
        "Failed to read stdout from " + string(HTTP_CHECK_COMMAND) + ": " +
        (output.isFailed() ? output.failure() : "discarded"));
  }

  // Parse the output and get the HTTP response code.
  Try<int> code = numify<int>(output.get());
  if (code.isError()) {
    return Failure(
        "Unexpected output from " + string(HTTP_CHECK_COMMAND) + ": " +
        output.get());
  }

  if (code.get() < process::http::Status::OK ||
      code.get() >= process::http::Status::BAD_REQUEST) {
    return Failure(
        "Unexpected HTTP response code: " +
        process::http::Status::string(code.get()));
  }

  return Nothing();
}


Future<Nothing> HealthCheckerProcess::tcpHealthCheck()
{
  CHECK_EQ(HealthCheck::TCP, check.type());
  CHECK(check.has_tcp());

  // TCP_CHECK_COMMAND should be reachable.
  CHECK(os::exists(launcherDir));

  const HealthCheck::TCPCheckInfo& tcp = check.tcp();

  VLOG(1) << "Launching TCP health check for task '" << taskId << "' at port"
          << tcp.port();

  const string tcpConnectPath = path::join(launcherDir, TCP_CHECK_COMMAND);

  const vector<string> tcpConnectArguments = {
    tcpConnectPath,
    "--ip=" + DEFAULT_DOMAIN,
    "--port=" + stringify(tcp.port())
  };

  // TODO(alexr): Consider launching the helper binary once per task lifetime,
  // see MESOS-6766.
  Try<Subprocess> s = subprocess(
      tcpConnectPath,
      tcpConnectArguments,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      clone);

  if (s.isError()) {
    return Failure(
        "Failed to create the " + string(TCP_CHECK_COMMAND) +
        " subprocess: " + s.error());
  }

  // TODO(alexr): Use lambda named captures for
  // these cached values once they are available.
  pid_t tcpConnectPid = s->pid();
  const Duration timeout = checkTimeout;
  const TaskID _taskId = taskId;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(
        timeout,
        [timeout, tcpConnectPid, _taskId](Future<tuple<Future<Option<int>>,
                                                       Future<string>,
                                                       Future<string>>> future)
    {
      future.discard();

      if (tcpConnectPid != -1) {
        // Cleanup the TCP_CHECK_COMMAND process.
        VLOG(1) << "Killing the TCP health check process " << tcpConnectPid
                << " for task '" << _taskId << "'";

        os::killtree(tcpConnectPid, SIGKILL);
      }

      return Failure(
          string(TCP_CHECK_COMMAND) + " timed out after " + stringify(timeout));
    })
    .then(defer(self(), &Self::_tcpHealthCheck, lambda::_1));
}


Future<Nothing> HealthCheckerProcess::_tcpHealthCheck(
    const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t)
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

  int statusCode = status->get();
  if (statusCode != 0) {
    const Future<string>& error = std::get<2>(t);
    if (!error.isReady()) {
      return Failure(
          string(TCP_CHECK_COMMAND) + " returned " +
          WSTRINGIFY(statusCode) + "; reading stderr failed: " +
          (error.isFailed() ? error.failure() : "discarded"));
    }

    return Failure(
        string(TCP_CHECK_COMMAND) + " returned " +
        WSTRINGIFY(statusCode) + ": " + error.get());
  }

  return Nothing();
}


void HealthCheckerProcess::scheduleNext(const Duration& duration)
{
  CHECK(!paused);

  VLOG(1) << "Scheduling health check for task '" << taskId << "' in "
          << duration;

  delay(duration, self(), &Self::performSingleCheck);
}


void HealthCheckerProcess::pause()
{
  if (!paused) {
    VLOG(1) << "Health checking for task '" << taskId << "' paused";

    paused = true;
  }
}


void HealthCheckerProcess::resume()
{
  if (paused) {
    VLOG(1) << "Health checking for task '" << taskId << "' resumed";

    paused = false;

    // Schedule a health check immediately.
    scheduleNext(Duration::zero());
  }
}


namespace validation {

Option<Error> healthCheck(const HealthCheck& check)
{
  if (!check.has_type()) {
    return Error("HealthCheck must specify 'type'");
  }

  switch (check.type()) {
    case HealthCheck::COMMAND: {
      if (!check.has_command()) {
        return Error("Expecting 'command' to be set for COMMAND health check");
      }

      const CommandInfo& command = check.command();

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

      break;
    }

    case HealthCheck::HTTP: {
      if (!check.has_http()) {
        return Error("Expecting 'http' to be set for HTTP health check");
      }

      const HealthCheck::HTTPCheckInfo& http = check.http();

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
      if (!check.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP health check");
      }

      break;
    }

    case HealthCheck::UNKNOWN: {
      return Error(
          "'" + HealthCheck::Type_Name(check.type()) + "'"
          " is not a valid health check type");
    }
  }

  return None();
}

} // namespace validation {

} // namespace checks {
} // namespace internal {
} // namespace mesos {
