// Licensed to the Apache Software Foundation (ASF) under one
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

#include <list>
#include <queue>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/executor/executor.hpp>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/mesos.hpp>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/fs.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/status_utils.hpp"

#include "health-check/health_checker.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

using mesos::executor::Call;
using mesos::executor::Event;

using mesos::v1::executor::Mesos;

using process::Clock;
using process::Future;
using process::Owned;
using process::UPID;

using process::http::Connection;
using process::http::Request;
using process::http::Response;
using process::http::URL;

using std::list;
using std::queue;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

class DefaultExecutor : public ProtobufProcess<DefaultExecutor>
{
public:
  DefaultExecutor(
      const FrameworkID& _frameworkId,
      const ExecutorID& _executorId,
      const ::URL& _agent,
      const string& _sandboxDirectory)
    : ProcessBase(process::ID::generate("default-executor")),
      state(DISCONNECTED),
      contentType(ContentType::PROTOBUF),
      launched(false),
      shuttingDown(false),
      unhealthy(false),
      frameworkInfo(None()),
      executorContainerId(None()),
      frameworkId(_frameworkId),
      executorId(_executorId),
      agent(_agent),
      sandboxDirectory(_sandboxDirectory) {}

  virtual ~DefaultExecutor() = default;

  void connected()
  {
    state = CONNECTED;
    connectionId = UUID::random();

    doReliableRegistration();
  }

  void disconnected()
  {
    LOG(INFO) << "Disconnected from agent";

    state = DISCONNECTED;
    connectionId = None();

    // Disconnect all active connections used for waiting on child
    // containers.
    foreach (Connection connection, waiting.values()) {
      connection.disconnect();
    }

    waiting.clear();
  }

  void received(const Event& event)
  {
    LOG(INFO) << "Received " << event.type() << " event";

    switch (event.type()) {
      case Event::SUBSCRIBED: {
        LOG(INFO) << "Subscribed executor on "
                  << event.subscribed().slave_info().hostname();

        frameworkInfo = event.subscribed().framework_info();
        state = SUBSCRIBED;

        CHECK(event.subscribed().has_container_id());
        executorContainerId = event.subscribed().container_id();

        // It is possible that the agent process had failed after we
        // had launched the child containers. We can resume waiting on the
        // child containers again.
        if (launched) {
          wait();
        }

        break;
      }

      case Event::LAUNCH: {
        LOG(ERROR) << "LAUNCH event is not supported";
        // Shut down because this is unexpected; `LAUNCH` event
        // should never go to the default executor.
        shutdown();
        break;
      }

      case Event::LAUNCH_GROUP: {
        launchGroup(event.launch_group().task_group());
        break;
      }

      case Event::KILL: {
        killTask(event.kill().task_id());
        break;
      }

      case Event::ACKNOWLEDGED: {
        // Remove the corresponding update.
        updates.erase(UUID::fromBytes(event.acknowledged().uuid()).get());

        // Remove the corresponding task.
        tasks.erase(event.acknowledged().task_id());
        break;
      }

      case Event::SHUTDOWN: {
        shutdown();
        break;
      }

      case Event::MESSAGE: {
        break;
      }

      case Event::ERROR: {
        LOG(ERROR) << "Error: " << event.error().message();
        break;
      }

      case Event::UNKNOWN: {
        LOG(WARNING) << "Received an UNKNOWN event and ignored";
        break;
      }
    }
  }

protected:
  virtual void initialize()
  {
    install<TaskHealthStatus>(
        &Self::taskHealthUpdated,
        &TaskHealthStatus::task_id,
        &TaskHealthStatus::healthy,
        &TaskHealthStatus::kill_task);

    mesos.reset(new Mesos(
        contentType,
        defer(self(), &Self::connected),
        defer(self(), &Self::disconnected),
        defer(self(), [this](queue<v1::executor::Event> events) {
          while(!events.empty()) {
            const v1::executor::Event& event = events.front();
            received(devolve(event));

            events.pop();
          }
        })));
  }

  void doReliableRegistration()
  {
    if (state == SUBSCRIBED || state == DISCONNECTED) {
      return;
    }

    Call call;
    call.set_type(Call::SUBSCRIBE);

    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    // Send all unacknowledged updates.
    foreach (const Call::Update& update, updates.values()) {
      subscribe->add_unacknowledged_updates()->MergeFrom(update);
    }

    // Send the unacknowledged tasks.
    foreach (const TaskInfo& task, tasks.values()) {
      subscribe->add_unacknowledged_tasks()->MergeFrom(task);
    }

    mesos->send(evolve(call));

    delay(Seconds(1), self(), &Self::doReliableRegistration);
  }

  void launchGroup(const TaskGroupInfo& taskGroup)
  {
    CHECK_EQ(SUBSCRIBED, state);

    if (launched) {
      LOG(WARNING) << "Ignoring the launch operation since a task group "
                   << "has been already launched";

      foreach (const TaskInfo& task, taskGroup.tasks()) {
        update(
            task.task_id(),
            TASK_FAILED,
            "Attempted to run multiple task groups using a "
            "\"default\" executor");
      }
      return;
    }

    launched = true;

    process::http::connect(agent)
      .onAny(defer(self(), &Self::_launchGroup, taskGroup, lambda::_1));
  }

  void _launchGroup(
      const TaskGroupInfo& taskGroup,
      const Future<Connection>& connection)
  {
    if (shuttingDown) {
      LOG(WARNING) << "Ignoring the launch operation as the "
                   << "executor is shutting down";
      return;
    }

    if (!connection.isReady()) {
      LOG(ERROR)
        << "Unable to establish connection with the agent: "
        << (connection.isFailed() ? connection.failure() : "discarded");
      __shutdown();
      return;
    }

    // It is possible that the agent process failed after the connection was
    // established. Shutdown the executor if this happens.
    if (state == DISCONNECTED || state == CONNECTED) {
      LOG(ERROR) << "Unable to complete the launch operation "
                 << "as the executor is in state " << state;
      __shutdown();
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(executorContainerId);

    list<ContainerID> pending;
    list<Future<Response>> responses;

    foreach (const TaskInfo& task, taskGroup.tasks()) {
      ContainerID containerId;
      containerId.set_value(UUID::random().toString());
      containerId.mutable_parent()->CopyFrom(executorContainerId.get());

      pending.push_back(containerId);

      agent::Call call;
      call.set_type(agent::Call::LAUNCH_NESTED_CONTAINER);

      agent::Call::LaunchNestedContainer* launch =
        call.mutable_launch_nested_container();

      launch->mutable_container_id()->CopyFrom(containerId);

      if (task.has_command()) {
        launch->mutable_command()->CopyFrom(task.command());
      }

      if (task.has_container()) {
        launch->mutable_container()->CopyFrom(task.container());
      }

      responses.push_back(post(connection.get(), call));
    }

    process::collect(responses)
      .onAny(defer(self(),
                   &Self::__launchGroup,
                   taskGroup,
                   pending,
                   connection.get(),
                   lambda::_1));
  }

  void __launchGroup(
      const TaskGroupInfo& taskGroup,
      list<ContainerID> pending,
      const Connection& connection,
      const Future<list<Response>>& responses)
  {
    if (shuttingDown) {
      LOG(WARNING) << "Ignoring the launch operation as the "
                   << "executor is shutting down";
      return;
    }

    // This could happen if the agent process failed while the child
    // containers were being launched. Shutdown the executor if this
    // happens.
    if (!responses.isReady()) {
      LOG(ERROR) << "Unable to receive a response from the agent for "
                 << "the LAUNCH_NESTED_CONTAINER call: "
                 << (responses.isFailed() ? responses.failure() : "discarded");
      __shutdown();
      return;
    }

    // Check if we received a 200 OK response for all the
    // `LAUNCH_NESTED_CONTAINER` calls. Shutdown the executor
    // if this is not the case.
    foreach (const Response& response, responses.get()) {
      if (response.code != process::http::Status::OK) {
        LOG(ERROR) << "Received '" << response.status << "' ("
                   << response.body << ") while launching child container";
        __shutdown();
        return;
      }
    }

    // This could happen if the agent process failed after the child
    // containers were launched. Shutdown the executor if this happens.
    if (state == DISCONNECTED || state == CONNECTED) {
      LOG(ERROR) << "Unable to complete the operation of launching child "
                 << "containers as the executor is in state " << state;
      __shutdown();
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK(launched);

    foreach (const TaskInfo& task, taskGroup.tasks()) {
      const TaskID& taskId = task.task_id();
      ContainerID containerId = pending.front();

      tasks[taskId] = task;
      containers[containerId] = taskId;

      pending.pop_front();

      if (task.has_health_check()) {
        // TODO(anand): Add support for command health checks.
        CHECK_NE(HealthCheck::COMMAND, task.health_check().type())
          << "Command health checks are not supported yet";

        Try<Owned<health::HealthChecker>> _checker =
          health::HealthChecker::create(
              task.health_check(),
              self(),
              taskId,
              None(),
              vector<string>());

        if (_checker.isError()) {
          // TODO(anand): Should we send a TASK_FAILED instead?
          LOG(ERROR) << "Failed to create health checker: "
                     << _checker.error();
          __shutdown();
          return;
        }

        Owned<health::HealthChecker> checker = _checker.get();

        checker->healthCheck()
          .onAny(defer(self(), [this, taskId](const Future<Nothing>& future) {
            if (!future.isReady()) {
              LOG(ERROR)
                << "Health check for task '" << taskId << "' failed due to: "
                << (future.isFailed() ? future.failure() : "discarded");

              __shutdown();
            }
          }));

        checkers.push_back(checker);
      }

      // Currently, the Mesos agent does not expose the mapping from
      // `ContainerID` to `TaskID` for nested containers.
      // In order for the Web UI to access the task sandbox, we create
      // a symbolic link from 'tasks/taskId' -> 'containers/containerId'.
      const string TASKS_DIRECTORY = "tasks";
      const string CONTAINERS_DIRECTORY = "containers";

      Try<Nothing> mkdir = os::mkdir(TASKS_DIRECTORY);
      if (mkdir.isError()) {
        LOG(FATAL) << "Unable to create task directory: " << mkdir.error();
      }

      Try<Nothing> symlink = fs::symlink(
          path::join(sandboxDirectory,
                     CONTAINERS_DIRECTORY,
                     containerId.value()),
          path::join(TASKS_DIRECTORY, taskId.value()));

      if (symlink.isError()) {
        LOG(FATAL) << "Unable to create symbolic link for container "
                   << containerId << " of task '" << taskId << "' due to: "
                   << symlink.error();
      }
    }

    // Send a TASK_RUNNING status update now that the task group has
    // been successfully launched.
    foreach (const TaskInfo& task, taskGroup.tasks()) {
      update(task.task_id(), TASK_RUNNING);
    }

    LOG(INFO)
      << "Successfully launched child containers "
      << stringify(containers.keys()) << " for tasks "
      << stringify(containers.values());

    wait();
  }

  void wait()
  {
    CHECK_EQ(SUBSCRIBED, state);
    CHECK(launched);
    CHECK_SOME(connectionId);

    list<Future<Connection>> connections;
    for (size_t i = 0; i < containers.size(); i++) {
      connections.push_back(process::http::connect(agent));
    }

    process::collect(connections)
      .onAny(defer(self(), &Self::_wait, lambda::_1, connectionId.get()));
  }

  void _wait(
      const Future<list<Connection>>& _connections,
      const UUID& _connectionId)
  {
    // It is possible that the agent process failed in the interim.
    // We would resume waiting on the child containers once we
    // subscribe again with the agent.
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring the wait operation from stale connection";
      return;
    }

    if (!_connections.isReady()) {
      LOG(ERROR)
        << "Unable to establish connection with the agent: "
        << (_connections.isFailed() ? _connections.failure() : "discarded");
      __shutdown();
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(connectionId);

    list<Connection> connections = _connections.get();
    CHECK_EQ(containers.size(), connections.size());

    foreach (const ContainerID& containerId, containers.keys()) {
      CHECK(!waiting.contains(containerId));

      __wait(connectionId.get(),
             connections.front(),
             containers[containerId],
             containerId);

      connections.pop_front();
    }
  }

  void __wait(
      const UUID& _connectionId,
      const Connection& connection,
      const TaskID& taskId,
      const ContainerID& containerId)
  {
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring the wait operation from a stale connection";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(connectionId);
    CHECK(!waiting.contains(containerId));

    LOG(INFO) << "Waiting for child container " << containerId
              << " of task '" << taskId << "'";

    waiting.put(containerId, connection);

    agent::Call call;
    call.set_type(agent::Call::WAIT_NESTED_CONTAINER);

    agent::Call::WaitNestedContainer* containerWait =
      call.mutable_wait_nested_container();

    containerWait->mutable_container_id()->CopyFrom(containerId);

    Future<Response> response = post(connection, call);
    response
      .onAny(defer(self(),
                   &Self::waited,
                   connectionId.get(),
                   connection,
                   taskId,
                   containerId,
                   lambda::_1));
  }

  void waited(
      const UUID& _connectionId,
      Connection connection,
      const TaskID& taskId,
      const ContainerID& containerId,
      const Future<Response>& response)
  {
    // It is possible that this callback executed after the agent process
    // failed in the interim. We can resume waiting on the child containers
    // once we subscribe again with the agent.
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring the waited callback from a stale connection";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK(waiting.contains(containerId));
    CHECK(waiting.get(containerId) == connection);

    auto retry_ = [this, connection, taskId, containerId]() mutable {
      connection.disconnect();
      waiting.erase(containerId);
      retry(connectionId.get(), taskId, containerId);
    };

    // It is possible that the response failed due to a network blip
    // rather than the agent process failing. In that case, reestablish
    // the connection.
    if (!response.isReady()) {
      LOG(ERROR)
        << "Connection for waiting on child container "
        << containerId << " of task '" << taskId << "' interrupted: "
        << (response.isFailed() ? response.failure() : "discarded");
      retry_();
      return;
    }

    // It is possible that the agent was still recovering when we
    // subscribed again after an agent process failure and started to
    // wait for the child container. In that case, reestablish
    // the connection.
    if (response->code == process::http::Status::SERVICE_UNAVAILABLE) {
      LOG(WARNING) << "Received '" << response->status << "' ("
                   << response->body << ") waiting on child container "
                   << containerId << " of task '" << taskId << "'";
      retry_();
      return;
    }

    // Check if we receive a 200 OK response for the `WAIT_NESTED_CONTAINER`
    // calls. Shutdown the executor otherwise.
    if (response->code != process::http::Status::OK) {
      LOG(ERROR) << "Received '" << response->status << "' ("
                 << response->body << ") waiting on child container "
                 << containerId << " of task '" << taskId << "'";
      __shutdown();
      return;
    }

    Try<agent::Response> waitResponse =
      deserialize<agent::Response>(contentType, response->body);
    CHECK_SOME(waitResponse);

    TaskState taskState;
    Option<string> message;

    Option<int> status = waitResponse->wait_nested_container().exit_status();

    if (status.isNone()) {
      taskState = TASK_FAILED;
    } else {
      CHECK(WIFEXITED(status.get()) || WIFSIGNALED(status.get()))
        << status.get();

      if (WIFEXITED(status.get()) && WEXITSTATUS(status.get()) == 0) {
        taskState = TASK_FINISHED;
      } else if (shuttingDown) {
        // Send TASK_KILLED if the task was killed as a result of
        // `killTask()` or `shutdown()`.
        taskState = TASK_KILLED;
      } else {
        taskState = TASK_FAILED;
      }

      message = "Command " + WSTRINGIFY(status.get());
    }

    if (unhealthy) {
      update(taskId, taskState, message, false);
    } else {
      update(taskId, taskState, message, None());
    }

    LOG(INFO) << "Successfully waited for child container " << containerId
              << " of task '" << taskId << "'"
              << " in state " << stringify(taskState);

    CHECK(containers.contains(containerId));
    containers.erase(containerId);

    // The default restart policy for a task group is to kill all the
    // remaining child containers if one of them terminated with a
    // non-zero exit code. Ignore if a shutdown is in progress.
    if (!shuttingDown && taskState == TASK_FAILED) {
      LOG(ERROR)
        << "Child container " << containerId << " terminated with status "
        << (status.isSome() ? WSTRINGIFY(status.get()) : "unknown");
      shutdown();
      return;
    }

    // Shutdown the executor if all the active child containers have terminated.
    if (containers.empty()) {
      __shutdown();
    }
  }

  void shutdown()
  {
    if (shuttingDown) {
      LOG(WARNING) << "Ignoring shutdown since it is in progress";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);

    LOG(INFO) << "Shutting down";

    shuttingDown = true;

    if (!launched) {
      __shutdown();
      return;
    }

    process::http::connect(agent)
      .onAny(defer(self(), &Self::_shutdown, lambda::_1));
  }

  void _shutdown(const Future<Connection>& connection)
  {
    if (!connection.isReady()) {
      LOG(ERROR)
        << "Unable to establish connection with the agent: "
        << (connection.isFailed() ? connection.failure() : "discarded");
      __shutdown();
      return;
    }

    // It is possible that the agent process failed before we could
    // kill the child containers.
    if (state == DISCONNECTED || state == CONNECTED) {
      LOG(ERROR) << "Unable to kill child containers as the "
                 << "executor is in state " << state;
      __shutdown();
      return;
    }

    list<Future<Nothing>> killing;
    foreach (const ContainerID& containerId, containers.keys()) {
      killing.push_back(kill(connection.get(), containerId));
    }

    // It is possible that the agent process can fail while we are
    // killing child containers. We fail fast if this happens. We
    // capture `connection` to ensure that the connection is not
    // disconnected before the responses are complete.
    collect(killing)
      .onAny(defer(
          self(),
          [this, connection](const Future<list<Nothing>>& future) {
        if (future.isReady()) {
          return;
        }

        LOG(ERROR)
          << "Unable to complete the operation of killing "
          << "child containers: "
          << (future.isFailed() ? future.failure() : "discarded");

        __shutdown();
      }));
  }

  void __shutdown()
  {
    const Duration duration = Seconds(1);

    LOG(INFO) << "Terminating after " << duration;

    // TODO(qianzhang): Remove this hack since the executor now receives
    // acknowledgements for status updates. The executor can terminate
    // after it receives an ACK for a terminal status update.
    os::sleep(duration);
    terminate(self());
  }

  Future<Nothing> kill(Connection connection, const ContainerID& containerId)
  {
    CHECK_EQ(SUBSCRIBED, state);
    CHECK(containers.contains(containerId));

    LOG(INFO) << "Killing child container " << containerId;

    agent::Call call;
    call.set_type(agent::Call::KILL_NESTED_CONTAINER);

    agent::Call::KillNestedContainer* kill =
      call.mutable_kill_nested_container();

    kill->mutable_container_id()->CopyFrom(containerId);

    return post(connection, call)
      .then([](const Response& /* response */) {
        return Nothing();
      });
  }

  void killTask(const TaskID& taskId)
  {
    if (shuttingDown) {
      LOG(WARNING) << "Ignoring kill for task '" << taskId
                   << "' since the executor is shutting down";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);

    // TODO(anand): Add support for handling kill policies.

    LOG(INFO) << "Received kill for task '" << taskId << "'";

    bool found = false;
    foreach (const TaskID& taskId_, containers.values()) {
      if (taskId_ == taskId) {
        found = true;
        break;
      }
    }

    if (!found) {
      LOG(WARNING) << "Ignoring kill for task '" << taskId
                   << "' as it is no longer active";
      return;
    }

    shutdown();
  }

  void taskHealthUpdated(
      const TaskID& taskId,
      bool healthy,
      bool initiateTaskKill)
  {
    LOG(INFO) << "Received task health update for task '" << taskId
              << "', task is "
              << (healthy ? "healthy" : "not healthy");

    update(taskId, TASK_RUNNING, None(), healthy);

    if (initiateTaskKill) {
      unhealthy = true;
      killTask(taskId);
    }
  }

private:
  void update(
      const TaskID& taskId,
      const TaskState& state,
      const Option<string>& message = None(),
      const Option<bool>& healthy = None())
  {
    UUID uuid = UUID::random();

    TaskStatus status;
    status.mutable_task_id()->CopyFrom(taskId);
    status.mutable_executor_id()->CopyFrom(executorId);

    status.set_state(state);
    status.set_source(TaskStatus::SOURCE_EXECUTOR);
    status.set_uuid(uuid.toBytes());
    status.set_timestamp(Clock::now().secs());

    if (message.isSome()) {
      status.set_message(message.get());
    }

    if (healthy.isSome()) {
      status.set_healthy(healthy.get());
    }

    Call call;
    call.set_type(Call::UPDATE);

    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    call.mutable_update()->mutable_status()->CopyFrom(status);

    // Capture the status update.
    updates[uuid] = call.update();

    mesos->send(evolve(call));
  }

  Future<Response> post(Connection connection, const agent::Call& call)
  {
    ::Request request;
    request.method = "POST";
    request.url = agent;
    request.body = serialize(contentType, evolve(call));
    request.keepAlive = true;
    request.headers = {{"Accept", stringify(contentType)},
                       {"Content-Type", stringify(contentType)}};

    return connection.send(request);
  }

  void retry(
      const UUID& _connectionId,
      const TaskID& taskId,
      const ContainerID& containerId)
  {
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring retry attempt from a stale connection";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);

    process::http::connect(agent)
      .onAny(defer(self(),
                   &Self::_retry,
                   lambda::_1,
                   connectionId.get(),
                   taskId,
                   containerId));
  }

  void _retry(
      const Future<Connection>& connection,
      const UUID& _connectionId,
      const TaskID& taskId,
      const ContainerID& containerId)
  {
    const Duration duration = Seconds(1);

    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring retry attempt from a stale connection";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(connectionId);

    if (!connection.isReady()) {
      LOG(ERROR)
        << "Unable to establish connection with the agent ("
        << (connection.isFailed() ? connection.failure() : "discarded")
        << ") for waiting on child container " << containerId
        << " of task '" << taskId << "'; Retrying again in " << duration;

      process::delay(
          duration,
          self(),
          &Self::retry,
          connectionId.get(),
          taskId,
          containerId);

      return;
    }

    LOG(INFO)
      << "Established connection to wait for child container " << containerId
      << " of task '" << taskId << "'; Retrying the WAIT_NESTED_CONTAINER call "
      << "in " << duration;

    // It is possible that we were able to reestablish the connection
    // but the agent might still be recovering. To avoid the vicious
    // cycle i.e., the `WAIT_NESTED_CONTAINER` call failing immediately
    // with a '503 SERVICE UNAVAILABLE' followed by retrying establishing
    // the connection again, we wait before making the call.
    process::delay(
        duration,
        self(),
        &Self::__wait,
        connectionId.get(),
        connection.get(),
        taskId,
        containerId);
  }

  enum State
  {
    CONNECTED,
    DISCONNECTED,
    SUBSCRIBED
  } state;

  const ContentType contentType;
  bool launched;
  bool shuttingDown;
  bool unhealthy; // Set to true if any of the tasks are reported unhealthy.
  Option<FrameworkInfo> frameworkInfo;
  Option<ContainerID> executorContainerId;
  const FrameworkID frameworkId;
  const ExecutorID executorId;
  Owned<Mesos> mesos;
  const ::URL agent; // Agent API URL.
  const string sandboxDirectory;
  LinkedHashMap<UUID, Call::Update> updates; // Unacknowledged updates.
  LinkedHashMap<TaskID, TaskInfo> tasks; // Unacknowledged tasks.

  // TODO(anand): Consider creating a `Container` struct to manage
  // information about an active container and its waiting connection.

  LinkedHashMap<ContainerID, TaskID> containers; // Active child containers.

  // Connections used for waiting on child containers. A child container
  // can be active and present in `containers` but not present
  // in `waiting` if a connection for sending the `WAIT_NESTED_CONTAINER`
  // call has not been established yet.
  hashmap<ContainerID, Connection> waiting;

  // There can be multiple simulataneous ongoing (re-)connection attempts
  // with the agent for waiting on child containers. This helps us in
  // uniquely identifying the current connection and ignoring
  // the stale instance. We initialize this to a new value upon receiving
  // a `connected()` callback.
  Option<UUID> connectionId;

  list<Owned<health::HealthChecker>> checkers; // Health checkers.
};

} // namespace internal {
} // namespace mesos {


int main(int argc, char** argv)
{
  mesos::FrameworkID frameworkId;
  mesos::ExecutorID executorId;
  string scheme = "http"; // Default scheme.
  ::URL agent;
  string sandboxDirectory;

  Option<string> value = os::getenv("MESOS_FRAMEWORK_ID");
  if (value.isNone()) {
    EXIT(EXIT_FAILURE)
      << "Expecting 'MESOS_FRAMEWORK_ID' to be set in the environment";
  }
  frameworkId.set_value(value.get());

  value = os::getenv("MESOS_EXECUTOR_ID");
  if (value.isNone()) {
    EXIT(EXIT_FAILURE)
      << "Expecting 'MESOS_EXECUTOR_ID' to be set in the environment";
  }
  executorId.set_value(value.get());

  value = os::getenv("SSL_ENABLED");
  if (value.isSome() && (value.get() == "1" || value.get() == "true")) {
    scheme = "https";
  }

  value = os::getenv("MESOS_SLAVE_PID");
  if (value.isNone()) {
    EXIT(EXIT_FAILURE)
      << "Expecting 'MESOS_SLAVE_PID' to be set in the environment";
  }

  UPID upid(value.get());
  CHECK(upid) << "Failed to parse MESOS_SLAVE_PID '" << value.get() << "'";

  agent = ::URL(
      scheme,
      upid.address.ip,
      upid.address.port,
      upid.id + "/api/v1");

  value = os::getenv("MESOS_SANDBOX");
  if (value.isNone()) {
    EXIT(EXIT_FAILURE)
      << "Expecting 'MESOS_SANDBOX' to be set in the environment";
  }
  sandboxDirectory = value.get();

  Owned<mesos::internal::DefaultExecutor> executor(
      new mesos::internal::DefaultExecutor(
          frameworkId,
          executorId,
          agent,
          sandboxDirectory));

  process::spawn(executor.get());
  process::wait(executor.get());

  return EXIT_SUCCESS;
}
