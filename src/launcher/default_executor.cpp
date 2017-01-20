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

#include <iostream>
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

#include <stout/flags.hpp>
#include <stout/fs.hpp>
#include <stout/lambda.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

#include "checks/health_checker.hpp"

#include "common/http.hpp"
#include "common/status_utils.hpp"

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

using std::cerr;
using std::cout;
using std::endl;
using std::list;
using std::queue;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

class DefaultExecutor : public ProtobufProcess<DefaultExecutor>
{
private:
  // Represents a child container. This is defined here since
  // C++ does not allow forward declaring nested classes.
  struct Container
  {
    ContainerID containerId;
    TaskID taskId;
    TaskGroupInfo taskGroup; // Task group of the child container.

    // Connection used for waiting on the child container. It is possible
    // that a container is active but a connection for sending the
    // `WAIT_NESTED_CONTAINER` call has not been established yet.
    Option<Connection> waiting;

    // Set to true if the child container is in the process of being killed.
    bool killing;

    // Set to true if the task group is in the process of being killed.
    bool killingTaskGroup;
  };

public:
  DefaultExecutor(
      const FrameworkID& _frameworkId,
      const ExecutorID& _executorId,
      const ::URL& _agent,
      const string& _sandboxDirectory,
      const string& _launcherDirectory)
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
      sandboxDirectory(_sandboxDirectory),
      launcherDirectory(_launcherDirectory) {}

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

    // Disconnect all active connections used for
    // waiting on child containers.
    foreachvalue (Owned<Container> container, containers) {
      if (container->waiting.isSome()) {
        container->waiting->disconnect();
        container->waiting = None();
      }
    }
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
          wait(containers.keys());
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
        unacknowledgedUpdates.erase(
            UUID::fromBytes(event.acknowledged().uuid()).get());

        // Remove the corresponding task.
        unacknowledgedTasks.erase(event.acknowledged().task_id());
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
    foreachvalue (const Call::Update& update, unacknowledgedUpdates) {
      subscribe->add_unacknowledged_updates()->MergeFrom(update);
    }

    // Send the unacknowledged tasks.
    foreachvalue (const TaskInfo& task, unacknowledgedTasks) {
      subscribe->add_unacknowledged_tasks()->MergeFrom(task);
    }

    mesos->send(evolve(call));

    delay(Seconds(1), self(), &Self::doReliableRegistration);
  }

  void launchGroup(const TaskGroupInfo& taskGroup)
  {
    CHECK_EQ(SUBSCRIBED, state);

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

    list<ContainerID> containerIds;
    list<Future<Response>> responses;

    foreach (const TaskInfo& task, taskGroup.tasks()) {
      ContainerID containerId;
      containerId.set_value(UUID::random().toString());
      containerId.mutable_parent()->CopyFrom(executorContainerId.get());

      containerIds.push_back(containerId);

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
                   containerIds,
                   connection.get(),
                   lambda::_1));
  }

  void __launchGroup(
      const TaskGroupInfo& taskGroup,
      const list<ContainerID>& containerIds,
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
    CHECK_EQ(containerIds.size(), (size_t) taskGroup.tasks().size());

    size_t index = 0;
    foreach (const ContainerID& containerId, containerIds) {
      const TaskInfo& task = taskGroup.tasks().Get(index++);
      const TaskID& taskId = task.task_id();

      unacknowledgedTasks[taskId] = task;
      containers[taskId] = Owned<Container>(
          new Container {containerId, taskId, taskGroup, None(), false, false});

      if (task.has_health_check()) {
        // TODO(anand): Add support for command health checks.
        CHECK_NE(HealthCheck::COMMAND, task.health_check().type())
          << "Command health checks are not supported yet";

        Try<Owned<checks::HealthChecker>> _checker =
          checks::HealthChecker::create(
              task.health_check(),
              launcherDirectory,
              defer(self(), &Self::taskHealthUpdated, lambda::_1),
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

        checkers[taskId] = _checker.get();
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

    auto taskIds = [&taskGroup]() {
      list<TaskID> taskIds_;
      foreach (const TaskInfo& task, taskGroup.tasks()) {
        taskIds_.push_back(task.task_id());
      }
      return taskIds_;
    };

    LOG(INFO)
      << "Successfully launched tasks "
      << stringify(taskIds()) << " in child containers "
      << stringify(containerIds);

    wait(taskIds());
  }

  void wait(const list<TaskID>& taskIds)
  {
    CHECK_EQ(SUBSCRIBED, state);
    CHECK(launched);
    CHECK_SOME(connectionId);

    list<Future<Connection>> connections;
    for (size_t i = 0; i < taskIds.size(); i++) {
      connections.push_back(process::http::connect(agent));
    }

    process::collect(connections)
      .onAny(defer(
          self(), &Self::_wait, lambda::_1, taskIds, connectionId.get()));
  }

  void _wait(
      const Future<list<Connection>>& _connections,
      const list<TaskID>& taskIds,
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

    CHECK_EQ(taskIds.size(), connections.size());
    foreach (const TaskID& taskId, taskIds) {
      __wait(connectionId.get(), connections.front(), taskId);
      connections.pop_front();
    }
  }

  void __wait(
      const UUID& _connectionId,
      const Connection& connection,
      const TaskID& taskId)
  {
    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring the wait operation from a stale connection";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(connectionId);
    CHECK(containers.contains(taskId));

    Owned<Container> container = containers.at(taskId);

    LOG(INFO) << "Waiting for child container " << container->containerId
              << " of task '" << taskId << "'";

    CHECK_NONE(container->waiting);
    container->waiting = connection;

    agent::Call call;
    call.set_type(agent::Call::WAIT_NESTED_CONTAINER);

    agent::Call::WaitNestedContainer* containerWait =
      call.mutable_wait_nested_container();

    containerWait->mutable_container_id()->CopyFrom(container->containerId);

    Future<Response> response = post(connection, call);
    response
      .onAny(defer(self(),
                   &Self::waited,
                   connectionId.get(),
                   taskId,
                   lambda::_1));
  }

  void waited(
      const UUID& _connectionId,
      const TaskID& taskId,
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
    CHECK(containers.contains(taskId));

    Owned<Container> container = containers.at(taskId);

    CHECK_SOME(container->waiting);

    auto retry_ = [this, container]() mutable {
      container->waiting->disconnect();
      container->waiting = None();
      retry(connectionId.get(), container->taskId);
    };

    // It is possible that the response failed due to a network blip
    // rather than the agent process failing. In that case, reestablish
    // the connection.
    if (!response.isReady()) {
      LOG(ERROR)
        << "Connection for waiting on child container "
        << container->containerId << " of task '" << taskId << "' interrupted: "
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
                   << container->containerId << " of task '" << taskId << "'";
      retry_();
      return;
    }

    // Check if we receive a 200 OK response for the `WAIT_NESTED_CONTAINER`
    // calls. Shutdown the executor otherwise.
    if (response->code != process::http::Status::OK) {
      LOG(ERROR) << "Received '" << response->status << "' ("
                 << response->body << ") waiting on child container "
                 << container->containerId << " of task '" << taskId << "'";
      __shutdown();
      return;
    }

    Try<agent::Response> waitResponse =
      deserialize<agent::Response>(contentType, response->body);
    CHECK_SOME(waitResponse);

    // If the task has been health checked, stop the associated checker.
    //
    // TODO(alexr): Once we support `TASK_KILLING` in this executor, health
    // checking should be stopped right before sending the `TASK_KILLING`
    // update to avoid subsequent `TASK_RUNNING` updates.
    if (checkers.contains(taskId)) {
      CHECK_NOTNULL(checkers.at(taskId).get());
      checkers.at(taskId)->stop();
      checkers.erase(taskId);
    }

    TaskState taskState;
    Option<string> message;

    Option<int> status = waitResponse->wait_nested_container().exit_status();

    if (status.isNone()) {
      taskState = TASK_FAILED;
    } else {
      CHECK(WIFEXITED(status.get()) || WIFSIGNALED(status.get()))
        << "Unexpected wait status " << status.get();

      if (WSUCCEEDED(status.get())) {
        taskState = TASK_FINISHED;
      } else if (container->killing) {
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

    CHECK(containers.contains(taskId));
    containers.erase(taskId);

    LOG(INFO)
      << "Child container " << container->containerId << " of task '" << taskId
      << "' in state " << stringify(taskState) << " terminated with status "
      << (status.isSome() ? WSTRINGIFY(status.get()) : "unknown");

    // Shutdown the executor if all the active child containers have terminated.
    if (containers.empty()) {
      __shutdown();
    }

    // Ignore if the executor is already in the process of shutting down.
    if (shuttingDown) {
      return;
    }

    // Ignore if this task group is already in the process of being killed.
    if (container->killingTaskGroup) {
      return;
    }

    // The default restart policy for a task group is to kill all the
    // remaining child containers if one of them terminated with a
    // non-zero exit code.
    if (taskState == TASK_FAILED || taskState == TASK_KILLED) {
      // Needed for logging.
      auto taskIds = [container]() {
        list<TaskID> taskIds_;
        foreach (const TaskInfo& task, container->taskGroup.tasks()) {
          taskIds_.push_back(task.task_id());
        }
        return taskIds_;
      };

      // Kill all the other active containers
      // belonging to this task group.
      LOG(INFO) << "Killing task group containing tasks "
                << stringify(taskIds());

      container->killingTaskGroup = true;
      foreach (const TaskInfo& task, container->taskGroup.tasks()) {
        const TaskID& taskId = task.task_id();
        if (taskId == container->taskId) {
          continue;
        }

        CHECK(containers.contains(taskId));
        Owned<Container> container_ = containers.at(taskId);

        container_->killingTaskGroup = true;

        kill(container_);
      }
    }
  }

  void shutdown()
  {
    if (shuttingDown) {
      LOG(WARNING) << "Ignoring shutdown since it is in progress";
      return;
    }

    LOG(INFO) << "Shutting down";

    shuttingDown = true;

    // Stop health checking all tasks because we are shutting down.
    foreach (const Owned<checks::HealthChecker>& checker, checkers.values()) {
      checker->stop();
    }
    checkers.clear();

    if (!launched) {
      __shutdown();
      return;
    }

    // It is possible that the executor library injected the shutdown event
    // upon a disconnection with the agent for non-checkpointed
    // frameworks or after recovery timeout for checkpointed frameworks.
    // This could also happen when the executor is connected but the agent
    // asked it to shutdown because it didn't subscribe in time.
    if (state == CONNECTED || state == DISCONNECTED) {
      __shutdown();
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);

    list<Future<Nothing>> killResponses;
    foreachvalue (const Owned<Container>& container, containers) {
      // It is possible that we received a `killTask()` request
      // from the scheduler before and are waiting on the `waited()`
      // callback to be invoked for the child container.
      if (container->killing) {
        continue;
      }

      killResponses.push_back(kill(container));
    }

    // It is possible that the agent process can fail while we are
    // killing child containers. We fail fast if this happens.
    collect(killResponses)
      .onAny(defer(
          self(),
          [this](const Future<list<Nothing>>& future) {
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

  Future<Nothing> kill(Owned<Container> container)
  {
    CHECK_EQ(SUBSCRIBED, state);

    CHECK(!container->killing);
    container->killing = true;

    LOG(INFO) << "Killing child container " << container->containerId;

    agent::Call call;
    call.set_type(agent::Call::KILL_NESTED_CONTAINER);

    agent::Call::KillNestedContainer* kill =
      call.mutable_kill_nested_container();

    kill->mutable_container_id()->CopyFrom(container->containerId);

    return post(None(), call)
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

    if (!containers.contains(taskId)) {
      LOG(WARNING) << "Ignoring kill for task '" << taskId
                   << "' as it is no longer active";
      return;
    }

    const Owned<Container>& container = containers.at(taskId);
    if (container->killing) {
      LOG(WARNING) << "Ignoring kill for task '" << taskId
                   << "' as it is in the process of getting killed";
      return;
    }

    kill(container);
  }

  void taskHealthUpdated(const TaskHealthStatus& healthStatus)
  {
    // This prevents us from sending `TASK_RUNNING` after a terminal status
    // update, because we may receive an update from a health check scheduled
    // before the task has been waited on.
    if (!checkers.contains(healthStatus.task_id())) {
      return;
    }

    LOG(INFO) << "Received task health update for task"
              << " '" << healthStatus.task_id() << "', task is "
              << (healthStatus.healthy() ? "healthy" : "not healthy");

    update(
        healthStatus.task_id(), TASK_RUNNING, None(), healthStatus.healthy());

    if (healthStatus.kill_task()) {
      unhealthy = true;
      killTask(healthStatus.task_id());
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

    // Fill the container ID associated with this task.
    CHECK(containers.contains(taskId));
    const Owned<Container>& container = containers.at(taskId);

    ContainerStatus* containerStatus = status.mutable_container_status();
    containerStatus->mutable_container_id()->CopyFrom(container->containerId);

    Call call;
    call.set_type(Call::UPDATE);

    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    call.mutable_update()->mutable_status()->CopyFrom(status);

    // Capture the status update.
    unacknowledgedUpdates[uuid] = call.update();

    mesos->send(evolve(call));
  }

  Future<Response> post(
      Option<Connection> connection,
      const agent::Call& call)
  {
    ::Request request;
    request.method = "POST";
    request.url = agent;
    request.body = serialize(contentType, evolve(call));
    request.headers = {{"Accept", stringify(contentType)},
                       {"Content-Type", stringify(contentType)}};

    // Only pipeline requests when there is an active connection.
    if (connection.isSome()) {
      request.keepAlive = true;
    }

    return connection.isSome() ? connection->send(request)
                               : process::http::request(request);
  }

  void retry(const UUID& _connectionId, const TaskID& taskId)
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
                   taskId));
  }

  void _retry(
      const Future<Connection>& connection,
      const UUID& _connectionId,
      const TaskID& taskId)
  {
    const Duration duration = Seconds(1);

    if (connectionId != _connectionId) {
      VLOG(1) << "Ignoring retry attempt from a stale connection";
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(connectionId);
    CHECK(containers.contains(taskId));

    const Owned<Container>& container = containers.at(taskId);

    if (!connection.isReady()) {
      LOG(ERROR)
        << "Unable to establish connection with the agent ("
        << (connection.isFailed() ? connection.failure() : "discarded")
        << ") for waiting on child container " << container->containerId
        << " of task '" << taskId << "'; Retrying again in " << duration;

      process::delay(
          duration, self(), &Self::retry, connectionId.get(), taskId);

      return;
    }

    LOG(INFO)
      << "Established connection to wait for child container "
      << container->containerId << " of task '" << taskId
      << "'; Retrying the WAIT_NESTED_CONTAINER call " << "in " << duration;

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
        taskId);
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
  const string launcherDirectory;

  LinkedHashMap<UUID, Call::Update> unacknowledgedUpdates;

  // A task is considered unacknowledged if no status update
  // acknowledgements have been received for it yet.
  LinkedHashMap<TaskID, TaskInfo> unacknowledgedTasks;

  // Active child containers.
  LinkedHashMap<TaskID, Owned<Container>> containers;

  // There can be multiple simulataneous ongoing (re-)connection attempts
  // with the agent for waiting on child containers. This helps us in
  // uniquely identifying the current connection and ignoring
  // the stale instance. We initialize this to a new value upon receiving
  // a `connected()` callback.
  Option<UUID> connectionId;

  // TODO(anand): Move the health checker information to the `Container` struct.
  hashmap<TaskID, Owned<checks::HealthChecker>> checkers; // Health checkers.
};

} // namespace internal {
} // namespace mesos {


class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::launcher_dir,
        "launcher_dir",
        "Directory path of Mesos binaries.",
        PKGLIBEXECDIR);
  }

  string launcher_dir;
};


int main(int argc, char** argv)
{
  process::initialize();

  Flags flags;
  mesos::FrameworkID frameworkId;
  mesos::ExecutorID executorId;
  string scheme = "http"; // Default scheme.
  ::URL agent;
  string sandboxDirectory;

  // Load flags from command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

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

#ifdef USE_SSL_SOCKET
  // TODO(gkleiman): Update this once the deprecation cycle is over (see
  // MESOS-6492).
  value = os::getenv("SSL_ENABLED");
  if (value.isNone()) {
    value = os::getenv("LIBPROCESS_SSL_ENABLED");
  }

  if (value.isSome() && (value.get() == "1" || value.get() == "true")) {
    scheme = "https";
  }
#endif

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
          sandboxDirectory,
          flags.launcher_dir));

  process::spawn(executor.get());
  process::wait(executor.get());

  // NOTE: We need to delete the executor before we call `process::finalize`
  // because the executor will try to terminate and wait on a libprocess
  // actor in the executor's destructor.
  executor.reset();

  // NOTE: We need to finalize libprocess, on Windows especially,
  // as any binary that uses the networking stack on Windows must
  // also clean up the networking stack before exiting.
  process::finalize(true);
  return EXIT_SUCCESS;
}
