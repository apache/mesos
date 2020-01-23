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

#include <deque>
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
#include <process/future.hpp>
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

#include "checks/checker.hpp"
#include "checks/checks_runtime.hpp"
#include "checks/health_checker.hpp"

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "logging/logging.hpp"

using mesos::executor::Call;
using mesos::executor::Event;

using mesos::v1::executor::Mesos;

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::Timer;
using process::UPID;

using process::http::Connection;
using process::http::Request;
using process::http::Response;
using process::http::URL;

using std::cerr;
using std::cout;
using std::deque;
using std::endl;
using std::list;
using std::queue;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

constexpr char MESOS_CONTAINER_IP[] = "MESOS_CONTAINER_IP";
constexpr char MESOS_ALLOCATION_ROLE[] = "MESOS_ALLOCATION_ROLE";


class DefaultExecutor : public ProtobufProcess<DefaultExecutor>
{
private:
  // Represents a child container. This is defined here since
  // C++ does not allow forward declaring nested classes.
  struct Container
  {
    ContainerID containerId;
    TaskInfo taskInfo;
    TaskGroupInfo taskGroup; // Task group of the child container.

    Option<TaskStatus> lastTaskStatus;

    // Checker for the container.
    Option<Owned<checks::Checker>> checker;

    // Health checker for the container.
    Option<Owned<checks::HealthChecker>> healthChecker;

    // Connection used for waiting on the child container. It is possible
    // that a container is active but a connection for sending the
    // `WAIT_NESTED_CONTAINER` call has not been established yet.
    Option<Connection> waiting;

    // Error returned by the agent while trying to launch the container.
    Option<string> launchError;

    // TODO(bennoe): Create a real state machine instead of adding
    // more and more ad-hoc boolean values.

    // Indicates whether the child container has been launched.
    bool launched;

    // Indicates whether a status update acknowledgement
    // has been received for any status update.
    bool acknowledged;

    // Set to true if the child container is in the process of being killed.
    bool killing;

    // Set to true if the task group is in the process of being killed.
    bool killingTaskGroup;

    // Set to true if the task has exceeded its max completion timeout.
    bool killedByCompletionTimeout;

    Option<Timer> maxCompletionTimer;
  };

public:
  DefaultExecutor(
      const FrameworkID& _frameworkId,
      const ExecutorID& _executorId,
      const ::URL& _agent,
      const string& _sandboxDirectory,
      const string& _launcherDirectory,
      const Option<string>& _authorizationHeader)
    : ProcessBase(process::ID::generate("default-executor")),
      state(DISCONNECTED),
      contentType(ContentType::PROTOBUF),
      shuttingDown(false),
      unhealthy(false),
      frameworkInfo(None()),
      executorContainerId(None()),
      frameworkId(_frameworkId),
      executorId(_executorId),
      agent(_agent),
      sandboxDirectory(_sandboxDirectory),
      launcherDirectory(_launcherDirectory),
      authorizationHeader(_authorizationHeader) {}

  ~DefaultExecutor() override = default;

  void connected()
  {
    state = CONNECTED;
    connectionId = id::UUID::random();

    doReliableRegistration();
  }

  void disconnected()
  {
    LOG(INFO) << "Disconnected from agent";

    state = DISCONNECTED;
    connectionId = None();

    // Disconnect all active connections used for
    // waiting on child containers.
    foreachvalue (Owned<Container>& container, containers) {
      if (container->waiting.isSome()) {
        container->waiting->disconnect();
        container->waiting = None();
      }
    }

    // Pause all checks and health checks.
    foreachvalue (Owned<Container>& container, containers) {
      if (container->checker.isSome()) {
        container->checker->get()->pause();
      }

      if (container->healthChecker.isSome()) {
        container->healthChecker->get()->pause();
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
        if (!containers.empty()) {
          wait(containers.keys());
        }

        // Resume all checks and health checks.
        foreachvalue (Owned<Container>& container, containers) {
          if (container->checker.isSome()) {
            container->checker->get()->resume();
          }

          if (container->healthChecker.isSome()) {
            container->healthChecker->get()->resume();
          }
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
        Option<KillPolicy> killPolicy = event.kill().has_kill_policy()
          ? Option<KillPolicy>(event.kill().kill_policy())
          : None();

        killTask(event.kill().task_id(), killPolicy);
        break;
      }

      case Event::ACKNOWLEDGED: {
        const id::UUID uuid =
          id::UUID::fromBytes(event.acknowledged().uuid()).get();

        if (!unacknowledgedUpdates.contains(uuid)) {
          LOG(WARNING) << "Received acknowledgement " << uuid
                       << " for unknown status update";
          return;
        }

        // Remove the corresponding update.
        unacknowledgedUpdates.erase(uuid);

        // Mark the corresponding task as acknowledged. An acknowledgement
        // may be received after the task has already been removed from
        // `containers`.
        const TaskID taskId = event.acknowledged().task_id();
        if (containers.contains(taskId)) {
          containers.at(taskId)->acknowledged = true;
        }

        // Terminate the executor if all status updates have been acknowledged
        // by the agent and no running containers left.
        if (containers.empty() && unacknowledgedUpdates.empty()) {
          terminate(self());
        }

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

      case Event::HEARTBEAT: {
        break;
      }

      case Event::UNKNOWN: {
        LOG(WARNING) << "Received an UNKNOWN event and ignored";
        break;
      }
    }
  }

protected:
  void initialize() override
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

    // Send all unacknowledged tasks. We don't send tasks whose container
    // didn't launch yet, because the agent will learn about once it launches.
    // We also don't send unacknowledged terminated (and hence already removed
    // from `containers`) tasks, because for such tasks `WAIT_NESTED_CONTAINER`
    // call has already succeeded, meaning the agent knows about the tasks and
    // corresponding containers.
    foreachvalue (const Owned<Container>& container, containers) {
      if (container->launched && !container->acknowledged) {
        subscribe->add_unacknowledged_tasks()->MergeFrom(container->taskInfo);
      }
    }

    mesos->send(evolve(call));

    delay(Seconds(1), self(), &Self::doReliableRegistration);
  }

  void launchGroup(const TaskGroupInfo& taskGroup)
  {
    CHECK_EQ(SUBSCRIBED, state);

    process::http::connect(agent)
      .onAny(defer(self(), &Self::_launchGroup, taskGroup, lambda::_1));
  }

  void _launchGroup(
      const TaskGroupInfo& taskGroup,
      const Future<Connection>& connection)
  {
    if (shuttingDown) {
      LOG(WARNING) << "Ignoring the launch group operation as the "
                   << "executor is shutting down";
      return;
    }

    if (!connection.isReady()) {
      LOG(WARNING) << "Unable to establish connection with the agent to "
                   << "complete the launch group operation: "
                   << (connection.isFailed() ? connection.failure()
                                             : "discarded");
      dropTaskGroup(taskGroup);

      // Shutdown the executor if all the active child containers have
      // terminated.
      if (containers.empty()) {
        _shutdown();
      }

      return;
    }

    // It is possible that the agent process failed after the connection was
    // established. Drop the task group if this happens.
    if (state == DISCONNECTED || state == CONNECTED) {
      LOG(WARNING) << "Unable to complete the launch group operation "
                   << "as the executor is in state " << state;
      dropTaskGroup(taskGroup);

      // Shutdown the executor if all the active child containers have
      // terminated.
      if (containers.empty()) {
        _shutdown();
      }
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(executorContainerId);

    // Determine the container IP in order to set `MESOS_CONTAINER_IP`
    // environment variable for each of the tasks being launched.
    // Libprocess has already determined the IP address associated
    // with this container network namespace in `process::initialize`
    // and hence we can just use the IP assigned to the PID of this
    // process as the IP address of the container.
    //
    // TODO(asridharan): This won't work when the framework sets the
    // `LIBPROCESS_ADVERTISE_IP` which will end up overriding the IP
    // address learnt during `process::initialize`, either through
    // `LIBPROCESS_IP` or through hostname resolution. The correct
    // approach would be to learn the allocated IP address directly
    // from the agent and not rely on the resolution logic implemented
    // in `process::initialize`.
    Environment::Variable containerIP;
    containerIP.set_name(MESOS_CONTAINER_IP);
    containerIP.set_value(stringify(self().address.ip));

    LOG(INFO) << "Setting 'MESOS_CONTAINER_IP' to: " << containerIP.value();

    Environment::Variable allocationRole;
    allocationRole.set_name(MESOS_ALLOCATION_ROLE);

    vector<ContainerID> containerIds;
    vector<Future<Response>> responses;

    foreach (const TaskInfo& task, taskGroup.tasks()) {
      ContainerID containerId;
      containerId.set_value(id::UUID::random().toString());
      containerId.mutable_parent()->CopyFrom(executorContainerId.get());

      containerIds.push_back(containerId);

      containers[task.task_id()] = Owned<Container>(new Container{
        containerId,
        task,
        taskGroup,
        None(),
        None(),
        None(),
        None(),
        None(),
        false,
        false,
        false,
        false});

      // Send out the initial TASK_STARTING update.
      const TaskStatus status = createTaskStatus(task.task_id(), TASK_STARTING);
      forward(status);

      agent::Call call;
      call.set_type(agent::Call::LAUNCH_CONTAINER);

      agent::Call::LaunchContainer* launch = call.mutable_launch_container();
      launch->mutable_container_id()->CopyFrom(containerId);

      if (task.has_command()) {
        launch->mutable_command()->CopyFrom(task.command());
      }

      if (task.has_container()) {
        launch->mutable_container()->CopyFrom(task.container());
      }

      launch->mutable_resources()->CopyFrom(task.resources());

      if (!task.limits().empty()) {
        *launch->mutable_limits() = task.limits();
      }

      // Currently any disk resources used by the task are mounted
      // on the top level container. As a workaround, we set up the
      // volume mapping allowing child containers to share the volumes
      // from their parent containers sandbox.
      //
      // TODO(qianzhang): Mount the disk resources on the task containers
      // directly and then remove this workaround.
      foreach (const Resource& resource, task.resources()) {
        // Ignore if there are no disk resources or if the
        // disk resources did not specify a volume mapping.
        if (!resource.has_disk() || !resource.disk().has_volume()) {
          continue;
        }

        // Set `ContainerInfo.type` to 'MESOS' if the task did
        // not specify a container.
        if (!task.has_container()) {
          launch->mutable_container()->set_type(ContainerInfo::MESOS);
        }

        const Volume& executorVolume = resource.disk().volume();

        Volume* taskVolume = launch->mutable_container()->add_volumes();
        taskVolume->set_mode(executorVolume.mode());
        taskVolume->set_container_path(executorVolume.container_path());

        Volume::Source* source = taskVolume->mutable_source();
        source->set_type(Volume::Source::SANDBOX_PATH);

        Volume::Source::SandboxPath* sandboxPath =
          source->mutable_sandbox_path();

        sandboxPath->set_type(Volume::Source::SandboxPath::PARENT);
        sandboxPath->set_path(executorVolume.container_path());
      }

      // Set the `MESOS_CONTAINER_IP` environment variable for the task.
      //
      // TODO(asridharan): Document this API for consumption by tasks
      // in the Mesos CNI and default-executor documentation.
      CommandInfo *command = launch->mutable_command();
      command->mutable_environment()->add_variables()->CopyFrom(containerIP);

      // Set the `MESOS_ALLOCATION_ROLE` environment variable for the task.
      // Please note that tasks are not allowed to mix resources allocated
      // to different roles, see MESOS-6636.
      allocationRole.set_value(
          task.resources().begin()->allocation_info().role());

      command->mutable_environment()->add_variables()->CopyFrom(allocationRole);

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
      const vector<ContainerID>& containerIds,
      const Connection& connection,
      const Future<vector<Response>>& responses)
  {
    if (shuttingDown) {
      LOG(WARNING) << "Ignoring the launch group operation as the "
                   << "executor is shutting down";
      return;
    }

    // This could happen if the agent process failed while the child
    // containers were being launched. Shutdown the executor if this
    // happens.
    if (!responses.isReady()) {
      LOG(ERROR) << "Unable to receive a response from the agent for "
                 << "the LAUNCH_CONTAINER call: "
                 << (responses.isFailed() ? responses.failure() : "discarded");
      _shutdown();
      return;
    }

    CHECK_EQ(containerIds.size(), (size_t) taskGroup.tasks().size());
    CHECK_EQ(containerIds.size(), responses->size());

    int index = 0;
    auto responseIterator = responses->begin();
    foreach (const ContainerID& containerId, containerIds) {
      const TaskInfo& task = taskGroup.tasks().Get(index++);
      const TaskID& taskId = task.task_id();
      const Response& response = *(responseIterator++);

      CHECK(containers.contains(taskId));
      Container* container = containers.at(taskId).get();

      // Check if we received a 200 OK response for the
      // `LAUNCH_CONTAINER` call. Skip the rest of the container
      // initialization if this is not the case.
      if (response.code != process::http::Status::OK) {
        LOG(ERROR) << "Received '" << response.status << "' (" << response.body
                   << ") while launching child container " << containerId
                   << " of task '" << taskId << "'";
        container->launchError = response.body;
        continue;
      }

      container->launched = true;

      const checks::runtime::Nested nestedRuntime{
        containerId, agent, authorizationHeader};

      if (task.has_check()) {
        Try<Owned<checks::Checker>> checker =
          checks::Checker::create(
              task.check(),
              launcherDirectory,
              defer(self(), &Self::taskCheckUpdated, taskId, lambda::_1),
              taskId,
              nestedRuntime);

        if (checker.isError()) {
          // TODO(anand): Should we send a TASK_FAILED instead?
          LOG(ERROR) << "Failed to create checker: " << checker.error();
          _shutdown();
          return;
        }

        container->checker = checker.get();
      }

      if (task.has_health_check()) {
        Try<Owned<checks::HealthChecker>> healthChecker =
          checks::HealthChecker::create(
              task.health_check(),
              launcherDirectory,
              defer(self(), &Self::taskHealthUpdated, lambda::_1),
              taskId,
              nestedRuntime);

        if (healthChecker.isError()) {
          // TODO(anand): Should we send a TASK_FAILED instead?
          LOG(ERROR) << "Failed to create health checker: "
                     << healthChecker.error();
          _shutdown();
          return;
        }

        container->healthChecker = healthChecker.get();
      }

      // Setup timer for max_completion_time.
      if (task.max_completion_time().nanoseconds() > 0) {
        Duration duration =
          Nanoseconds(task.max_completion_time().nanoseconds());

        container->maxCompletionTimer = delay(
            duration, self(), &Self::maxCompletion, task.task_id(), duration);
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

      forward(createTaskStatus(task.task_id(), TASK_RUNNING));
    }

    auto taskIds = [&taskGroup]() {
      vector<TaskID> taskIds_;
      foreach (const TaskInfo& task, taskGroup.tasks()) {
        taskIds_.push_back(task.task_id());
      }
      return taskIds_;
    };

    LOG(INFO)
      << "Finished launching tasks "
      << stringify(taskIds()) << " in child containers "
      << stringify(containerIds);

    if (state == SUBSCRIBED) {
      // `wait()` requires the executor to be subscribed.
      //
      // Upon subscription, `received()` will call `wait()` on all containers,
      // so it is safe to skip it here if we are not subscribed.
      wait(taskIds());
    } else {
      LOG(INFO) << "Skipped waiting on child containers of tasks "
                << stringify(taskIds()) << " until the connection "
                << "to the agent is reestablished";
    }
  }

  void wait(const vector<TaskID>& taskIds)
  {
    CHECK_EQ(SUBSCRIBED, state);
    CHECK(!containers.empty());
    CHECK_SOME(connectionId);

    LOG(INFO) << "Waiting on child containers of tasks " << stringify(taskIds);

    vector<Future<Connection>> connections;
    for (size_t i = 0; i < taskIds.size(); i++) {
      connections.push_back(process::http::connect(agent));
    }

    process::collect(connections)
      .onAny(defer(
          self(), &Self::_wait, lambda::_1, taskIds, connectionId.get()));
  }

  void _wait(
      const Future<vector<Connection>>& _connections,
      const vector<TaskID>& taskIds,
      const id::UUID& _connectionId)
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
      _shutdown();
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);
    CHECK_SOME(connectionId);

    deque<Connection> connections(_connections->begin(), _connections->end());

    CHECK_EQ(taskIds.size(), connections.size());
    foreach (const TaskID& taskId, taskIds) {
      __wait(connectionId.get(), connections.front(), taskId);
      connections.pop_front();
    }
  }

  void __wait(
      const id::UUID& _connectionId,
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

    Container* container = containers.at(taskId).get();

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
      const id::UUID& _connectionId,
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

    Container* container = containers.at(taskId).get();

    CHECK_SOME(container->waiting);

    auto retry_ = [this, container]() mutable {
      container->waiting->disconnect();
      container->waiting = None();
      retry(connectionId.get(), container->taskInfo.task_id());
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

    // Shutdown the executor if the agent responded to the
    // `WAIT_NESTED_CONTAINER` call with an error. Note that several race
    // conditions can cause a 404 NOT FOUND response, which shouldn't be
    // treated as an error.
    if (response->code != process::http::Status::NOT_FOUND &&
        response->code != process::http::Status::OK) {
      LOG(ERROR) << "Received '" << response->status << "' ("
                 << response->body << ") waiting on child container "
                 << container->containerId << " of task '" << taskId << "'";
      _shutdown();
      return;
    }

    // If the task is checked, pause the associated checker to avoid
    // sending check updates after a terminal status update.
    if (container->checker.isSome()) {
      CHECK_NOTNULL(container->checker->get());
      container->checker->get()->pause();
      container->checker = None();
    }

    // If the task is health checked, pause the associated health checker
    // to avoid sending health updates after a terminal status update.
    if (container->healthChecker.isSome()) {
      CHECK_NOTNULL(container->healthChecker->get());
      container->healthChecker->get()->pause();
      container->healthChecker = None();
    }

    TaskState taskState;
    Option<string> message;
    Option<TaskStatus::Reason> reason;
    Option<TaskResourceLimitation> limitation;

    if (response->code == process::http::Status::NOT_FOUND) {
      // The agent can respond with 404 NOT FOUND due to a failed container
      // launch or due to a race condition.

      if (container->killing) {
        // Send TASK_KILLED if the task was killed as a result of
        // `killTask()` or `shutdown()`.
        taskState = TASK_KILLED;
      } else if (container->launchError.isSome()) {
        // Send TASK_FAILED if we know that `LAUNCH_CONTAINER` returned
        // an error.
        taskState = TASK_FAILED;
        message = container->launchError;
      } else {
        // We don't know exactly why `WAIT_NESTED_CONTAINER` returned 404 NOT
        // FOUND, so we'll assume that the task failed.
        taskState = TASK_FAILED;
        message = "Unable to retrieve command's termination information";
      }
    } else {
      Try<agent::Response> waitResponse =
        deserialize<agent::Response>(contentType, response->body);
      CHECK_SOME(waitResponse);

      if (!waitResponse->wait_nested_container().has_exit_status()) {
        taskState = TASK_FAILED;

        if (container->launchError.isSome()) {
          message = container->launchError;
        } else {
          message = "Command terminated with unknown status";
        }
      } else {
        int status = waitResponse->wait_nested_container().exit_status();

        CHECK(WIFEXITED(status) || WIFSIGNALED(status))
          << "Unexpected wait status " << status;

        if (container->killedByCompletionTimeout) {
          taskState = TASK_FAILED;
          reason = TaskStatus::REASON_MAX_COMPLETION_TIME_REACHED;
        } else if (container->killing) {
          // Send TASK_KILLED if the task was killed as a result of
          // `killTask()` or `shutdown()`.
          taskState = TASK_KILLED;
        } else if (WSUCCEEDED(status)) {
          taskState = TASK_FINISHED;
        } else {
          taskState = TASK_FAILED;
        }

        message = "Command " + WSTRINGIFY(status);
      }

      // Note that we always prefer the task state and reason from the
      // agent response over what we can determine ourselves because
      // in general, the agent has more specific information about why
      // the container exited (e.g. this might be a container resource
      // limitation).
      if (waitResponse->wait_nested_container().has_state()) {
        taskState = waitResponse->wait_nested_container().state();
      }

      if (waitResponse->wait_nested_container().has_reason()) {
        reason = waitResponse->wait_nested_container().reason();
      }

      if (waitResponse->wait_nested_container().has_message()) {
        if (message.isSome()) {
          message->append(
              ": " +  waitResponse->wait_nested_container().message());
        } else {
          message = waitResponse->wait_nested_container().message();
        }
      }

      if (waitResponse->wait_nested_container().has_limitation()) {
        limitation = waitResponse->wait_nested_container().limitation();
      }
    }

    TaskStatus taskStatus = createTaskStatus(
        taskId,
        taskState,
        reason,
        message,
        limitation);

    // Indicate that a task has been unhealthy upon termination.
    //
    // TODO(gkleiman): We should do this if this task or another task that
    // belongs to the same task group is unhealthy. See MESOS-8543.
    if (unhealthy) {
      // TODO(abudnik): Consider specifying appropriate status update reason,
      // saying that the task was killed due to a failing health check.
      taskStatus.set_healthy(false);
    }

    forward(taskStatus);

    LOG(INFO)
      << "Child container " << container->containerId << " of task '"
      << taskId << "' completed in state " << stringify(taskState)
      << (message.isSome() ? ": " + message.get() : "");

    // The default restart policy for a task group is to kill all the
    // remaining child containers if one of them terminated with a
    // non-zero exit code.
    if (!shuttingDown && !container->killingTaskGroup &&
        (taskState == TASK_FAILED || taskState == TASK_KILLED)) {
      // Needed for logging.
      auto taskIds = [container]() {
        vector<TaskID> taskIds_;
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
        const TaskID& taskId_ = task.task_id();

        // Ignore if it's the same task that triggered this callback or
        // if the task is no longer active.
        if (taskId_ == container->taskInfo.task_id() ||
            !containers.contains(taskId_)) {
          continue;
        }

        Container* container_ = containers.at(taskId_).get();
        container_->killingTaskGroup = true;

        // Ignore if the task is already being killed. This can happen
        // when the scheduler tries to kill multiple tasks in the task
        // group simultaneously and then one of the tasks is killed
        // while the other tasks are still being killed, see MESOS-8051.
        if (container_->killing) {
          continue;
        }

        kill(container_);
      }
    }

    CHECK(containers.contains(taskId));
    containers.erase(taskId);

    // Shutdown the executor if all the active child containers have terminated.
    if (containers.empty()) {
      _shutdown();
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

    if (containers.empty()) {
      _shutdown();
      return;
    }

    // It is possible that the executor library injected the shutdown event
    // upon a disconnection with the agent for non-checkpointed
    // frameworks or after recovery timeout for checkpointed frameworks.
    // This could also happen when the executor is connected but the agent
    // asked it to shutdown because it didn't subscribe in time.
    if (state == CONNECTED || state == DISCONNECTED) {
      _shutdown();
      return;
    }

    CHECK_EQ(SUBSCRIBED, state);

    vector<Future<Nothing>> killResponses;
    foreachvalue (const Owned<Container>& container, containers) {
      // It is possible that we received a `killTask()` request
      // from the scheduler before and are waiting on the `waited()`
      // callback to be invoked for the child container.
      if (container->killing) {
        continue;
      }

      killResponses.push_back(kill(container.get()));
    }

    // It is possible that the agent process can fail while we are
    // killing child containers. We fail fast if this happens.
    collect(killResponses)
      .onAny(defer(
          self(),
          [this](const Future<vector<Nothing>>& future) {
        if (future.isReady()) {
          return;
        }

        LOG(ERROR)
          << "Unable to complete the operation of killing "
          << "child containers: "
          << (future.isFailed() ? future.failure() : "discarded");

        _shutdown();
      }));
  }

  void _shutdown()
  {
    if (unacknowledgedUpdates.empty()) {
      terminate(self());
    } else {
      // This is a fail safe in case the agent doesn't send an ACK for
      // a status update for some reason.
      const Duration duration = Seconds(60);

      LOG(INFO) << "Terminating after " << duration;

      delay(duration, self(), &Self::__shutdown);
    }
  }

  void __shutdown()
  {
    terminate(self());
  }

  Future<Nothing> kill(
      Container* container,
      const Option<Duration>& _gracePeriod = None())
  {
    if (!container->launched) {
      // We can get here if we're killing a task group for which multiple
      // containers failed to launch.
      return Nothing();
    }

    if (container->maxCompletionTimer.isSome()) {
      Clock::cancel(container->maxCompletionTimer.get());
      container->maxCompletionTimer = None();
    }

    CHECK(!container->killing);
    container->killing = true;

    // If the task is checked, pause the associated checker.
    //
    // TODO(alexr): Once we support `TASK_KILLING` in this executor,
    // consider continuing checking the task after sending `TASK_KILLING`.
    if (container->checker.isSome()) {
      CHECK_NOTNULL(container->checker->get());
      container->checker->get()->pause();
      container->checker = None();
    }

    // If the task is health checked, pause the associated health checker.
    //
    // TODO(alexr): Once we support `TASK_KILLING` in this executor,
    // consider health checking the task after sending `TASK_KILLING`.
    if (container->healthChecker.isSome()) {
      CHECK_NOTNULL(container->healthChecker->get());
      container->healthChecker->get()->pause();
      container->healthChecker = None();
    }

    const TaskID& taskId = container->taskInfo.task_id();

    LOG(INFO)
      << "Killing task " << taskId << " running in child container"
      << " " << container->containerId << " with SIGTERM signal";

    // Default grace period is set to 3s.
    Duration gracePeriod = Seconds(3);

    if (_gracePeriod.isSome()) {
      gracePeriod = _gracePeriod.get();
    }

    LOG(INFO) << "Scheduling escalation to SIGKILL in " << gracePeriod
              << " from now";

    const ContainerID& containerId = container->containerId;

    delay(gracePeriod,
          self(),
          &Self::escalated,
          containerId,
          container->taskInfo.task_id(),
          gracePeriod);

    // Send a 'TASK_KILLING' update if the framework can handle it.
    CHECK_SOME(frameworkInfo);

    if (!container->killedByCompletionTimeout &&
        protobuf::frameworkHasCapability(
            frameworkInfo.get(),
            FrameworkInfo::Capability::TASK_KILLING_STATE)) {
      TaskStatus status = createTaskStatus(taskId, TASK_KILLING);
      forward(status);
    }

    // Ideally we should detect and act on this kill's failure, and perform the
    // following actions only once the kill is successful:
    //
    // 1) Stop (health) checking.
    // 2) Send a `TASK_KILLING` task status update.
    // 3) Schedule the kill escalation.
    // 4) Set `container->killing` to `true`
    //
    // If the kill fails or times out, we could do one of the following options:
    //
    // 1) Automatically retry the kill (MESOS-8726).
    // 2) Let the scheduler request another kill.
    return kill(containerId, SIGTERM);
  }

  Future<Nothing> kill(const ContainerID& containerId, int signal)
  {
    agent::Call call;
    call.set_type(agent::Call::KILL_NESTED_CONTAINER);

    agent::Call::KillNestedContainer* kill =
      call.mutable_kill_nested_container();

    kill->mutable_container_id()->CopyFrom(containerId);
    kill->set_signal(signal);

    return post(None(), call)
      .then([=](const Response& response) -> Future<Nothing> {
        if (response.code != process::http::Status::OK) {
          return Failure(
              stringify("The agent failed to send signal") +
              " " + strsignal(signal) + " (" + stringify(signal) + ")" +
              " to the container " + stringify(containerId) +
              ": " + response.body);
        }

        return Nothing();
      });
  }

  void escalated(
      const ContainerID& containerId,
      const TaskID& taskId,
      const Duration& timeout)
  {
    // It might be possible that the container is already terminated.
    // If that happens, don't bother escalating to SIGKILL.
    if (!containers.contains(taskId)) {
      LOG(WARNING)
        << "Ignoring escalation to SIGKILL since the task '" << taskId
        << "' running in child container " << containerId << " has"
        << " already terminated";
      return;
    }

    LOG(INFO)
      << "Task '" << taskId << "' running in child container " << containerId
      << " did not terminate after " << timeout << ", sending SIGKILL"
      << " to the container";

    kill(containerId, SIGKILL)
      .onFailed(defer(self(), [=](const string& failure) {
        const Duration duration = Seconds(1);

        LOG(WARNING)
          << "Escalation to SIGKILL the task '" << taskId
          << "' running in child container " << containerId
          << " failed: " << failure << "; Retrying in " << duration;

        process::delay(
            duration, self(), &Self::escalated, containerId, taskId, timeout);

        return;
      }));
  }

  void killTask(
      const TaskID& taskId,
      const Option<KillPolicy>& killPolicy = None())
  {
    if (shuttingDown) {
      LOG(WARNING) << "Ignoring kill for task '" << taskId
                   << "' since the executor is shutting down";
      return;
    }

    // TODO(anand): Add support for adjusting the remaining grace period if
    // we receive another kill request while a task is being killed but has
    // not terminated yet. See similar comments in the command executor
    // for more context. See MESOS-8557 for more details.

    LOG(INFO) << "Received kill for task '" << taskId << "'";

    if (!containers.contains(taskId)) {
      LOG(WARNING) << "Ignoring kill for task '" << taskId
                   << "' as it is no longer active";
      return;
    }

    Container* container = containers.at(taskId).get();
    if (container->killing) {
      LOG(WARNING) << "Ignoring kill for task '" << taskId
                   << "' as it is in the process of getting killed";
      return;
    }

    Option<Duration> gracePeriod = None();

    // Kill policy provided in the `Kill` event takes precedence
    // over kill policy specified when the task was launched.
    if (killPolicy.isSome() && killPolicy->has_grace_period()) {
      gracePeriod = Nanoseconds(killPolicy->grace_period().nanoseconds());
    } else if (container->taskInfo.has_kill_policy() &&
               container->taskInfo.kill_policy().has_grace_period()) {
      gracePeriod = Nanoseconds(
          container->taskInfo.kill_policy().grace_period().nanoseconds());
    }

    const ContainerID& containerId = container->containerId;
    kill(container, gracePeriod)
      .onFailed(defer(self(), [=](const string& failure) {
        LOG(WARNING) << "Failed to kill the task '" << taskId
                     << "' running in child container " << containerId << ": "
                     << failure;
      }));
  }

  void maxCompletion(const TaskID& taskId, const Duration& duration)
  {
    if (!containers.contains(taskId)) {
      return;
    }

    LOG(INFO) << "Killing task " << taskId
              << " which exceeded its maximum completion time of " << duration;

    Container* container = containers.at(taskId).get();
    container->maxCompletionTimer = None();
    container->killedByCompletionTimeout = true;
    // Use a zero grace period to kill the container.
    kill(container, Duration::zero());
  }

  void taskCheckUpdated(
      const TaskID& taskId,
      const CheckStatusInfo& checkStatus)
  {
    // If the checked container has already been waited on,
    // ignore the check update. This prevents us from sending
    // `TASK_RUNNING` after a terminal status update.
    if (!containers.contains(taskId)) {
      VLOG(1) << "Received check update for terminated task"
              << " '" << taskId << "'; ignoring";
      return;
    }

    // If the checked container has already been asked to terminate,
    // ignore the check update.
    //
    // TODO(alexr): Once we support `TASK_KILLING` in this executor,
    // consider sending check updates after sending `TASK_KILLING`.
    if (containers.at(taskId)->checker.isNone()) {
      VLOG(1) << "Received check update for terminating task"
              << " '" << taskId << "'; ignoring";
      return;
    }

    LOG(INFO) << "Received check update '" << checkStatus
              << "' for task '" << taskId << "'";

    // Use the previous task status to preserve all attached information.
    // We always send a `TASK_RUNNING` right after the task is launched.
    CHECK_SOME(containers.at(taskId)->lastTaskStatus);
    const TaskStatus status = protobuf::createTaskStatus(
        containers.at(taskId)->lastTaskStatus.get(),
        id::UUID::random(),
        Clock::now().secs(),
        None(),
        None(),
        None(),
        TaskStatus::REASON_TASK_CHECK_STATUS_UPDATED,
        None(),
        None(),
        checkStatus);

    forward(status);
  }

  void taskHealthUpdated(const TaskHealthStatus& healthStatus)
  {
    if (state == DISCONNECTED) {
      VLOG(1) << "Ignoring task health update for task"
              << " '" << healthStatus.task_id() << "',"
              << " because the executor is not connected to the agent";
      return;
    }

    // If the health checked container has already been waited on,
    // ignore the health update. This prevents us from sending
    // `TASK_RUNNING` after a terminal status update.
    if (!containers.contains(healthStatus.task_id())) {
      VLOG(1) << "Received task health update for terminated task"
              << " '" << healthStatus.task_id() << "'; ignoring";
      return;
    }

    // If the health checked container has already been asked to
    // terminate, ignore the health update.
    //
    // TODO(alexr): Once we support `TASK_KILLING` in this executor,
    // consider sending health updates after sending `TASK_KILLING`.
    if (containers.at(healthStatus.task_id())->healthChecker.isNone()) {
      VLOG(1) << "Received task health update for terminating task"
              << " '" << healthStatus.task_id() << "'; ignoring";
      return;
    }

    LOG(INFO) << "Received task health update for task"
              << " '" << healthStatus.task_id() << "', task is "
              << (healthStatus.healthy() ? "healthy" : "not healthy");

    // Use the previous task status to preserve all attached information.
    // We always send a `TASK_RUNNING` right after the task is launched.
    CHECK_SOME(containers.at(healthStatus.task_id())->lastTaskStatus);
    const TaskStatus status = protobuf::createTaskStatus(
        containers.at(healthStatus.task_id())->lastTaskStatus.get(),
        id::UUID::random(),
        Clock::now().secs(),
        None(),
        None(),
        None(),
        TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
        None(),
        healthStatus.healthy());

    forward(status);

    if (healthStatus.kill_task()) {
      unhealthy = true;
      killTask(healthStatus.task_id());
    }
  }

private:
  // Use this helper to create a status update from scratch, i.e., without
  // previously attached extra information like `data` or `check_status`.
  TaskStatus createTaskStatus(
      const TaskID& taskId,
      const TaskState& state,
      const Option<TaskStatus::Reason>& reason = None(),
      const Option<string>& message = None(),
      const Option<TaskResourceLimitation>& limitation = None())
  {
    TaskStatus status = protobuf::createTaskStatus(
        taskId,
        state,
        id::UUID::random(),
        Clock::now().secs());

    status.mutable_executor_id()->CopyFrom(executorId);
    status.set_source(TaskStatus::SOURCE_EXECUTOR);

    if (reason.isSome()) {
      status.set_reason(reason.get());
    }

    if (message.isSome()) {
      status.set_message(message.get());
    }

    if (limitation.isSome()) {
      status.mutable_limitation()->CopyFrom(limitation.get());
    }

    CHECK(containers.contains(taskId));
    const Container* container = containers.at(taskId).get();

    // TODO(alexr): Augment health information in a way similar to
    // `CheckStatusInfo`. See MESOS-6417 for more details.

    // If a check for the task has been defined, `check_status` field in each
    // task status must be set to a valid `CheckStatusInfo` message even if
    // there is no check status available yet.
    if (container->taskInfo.has_check()) {
      CheckStatusInfo checkStatusInfo;
      checkStatusInfo.set_type(container->taskInfo.check().type());
      switch (container->taskInfo.check().type()) {
        case CheckInfo::COMMAND: {
          checkStatusInfo.mutable_command();
          break;
        }
        case CheckInfo::HTTP: {
          checkStatusInfo.mutable_http();
          break;
        }
        case CheckInfo::TCP: {
          checkStatusInfo.mutable_tcp();
          break;
        }
        case CheckInfo::UNKNOWN: {
          LOG(FATAL) << "UNKNOWN check type is invalid";
          break;
        }
      }

      status.mutable_check_status()->CopyFrom(checkStatusInfo);
    }

    // Fill the container ID associated with this task.
    ContainerStatus* containerStatus = status.mutable_container_status();
    containerStatus->mutable_container_id()->CopyFrom(container->containerId);

    return status;
  }

  void forward(const TaskStatus& status)
  {
    Call call;
    call.set_type(Call::UPDATE);

    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    call.mutable_update()->mutable_status()->CopyFrom(status);

    // Capture the status update.
    unacknowledgedUpdates[id::UUID::fromBytes(status.uuid()).get()] =
      call.update();

    // Overwrite the last task status.
    CHECK(containers.contains(status.task_id()));
    containers.at(status.task_id())->lastTaskStatus = status;

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

    if (authorizationHeader.isSome()) {
      request.headers["Authorization"] = authorizationHeader.get();
    }

    // Only pipeline requests when there is an active connection.
    if (connection.isSome()) {
      request.keepAlive = true;
    }

    return connection.isSome() ? connection->send(request)
                               : process::http::request(request);
  }

  void retry(const id::UUID& _connectionId, const TaskID& taskId)
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
      const id::UUID& _connectionId,
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

    const Container* container = containers.at(taskId).get();

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

  void dropTaskGroup(const TaskGroupInfo& taskGroup)
  {
    TaskState taskState =
      protobuf::frameworkHasCapability(
          frameworkInfo.get(), FrameworkInfo::Capability::PARTITION_AWARE)
        ? TASK_DROPPED
        : TASK_LOST;

    foreach (const TaskInfo& task, taskGroup.tasks()) {
      forward(createTaskStatus(task.task_id(), taskState));
    }
  }

  enum State
  {
    CONNECTED,
    DISCONNECTED,
    SUBSCRIBED
  } state;

  const ContentType contentType;
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
  const Option<string> authorizationHeader;

  LinkedHashMap<id::UUID, Call::Update> unacknowledgedUpdates;

  // Child containers.
  LinkedHashMap<TaskID, Owned<Container>> containers;

  // There can be multiple simulataneous ongoing (re-)connection attempts
  // with the agent for waiting on child containers. This helps us in
  // uniquely identifying the current connection and ignoring
  // the stale instance. We initialize this to a new value upon receiving
  // a `connected()` callback.
  Option<id::UUID> connectionId;
};

} // namespace internal {
} // namespace mesos {


class Flags : public virtual mesos::internal::logging::Flags
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
  mesos::FrameworkID frameworkId;
  mesos::ExecutorID executorId;
  string scheme = "http"; // Default scheme.
  ::URL agent;
  string sandboxDirectory;
  Flags flags;

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

  mesos::internal::logging::initialize(argv[0], true, flags); // Catch signals.

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
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

#ifndef __WINDOWS__
  value = os::getenv("MESOS_DOMAIN_SOCKET");
  if (value.isSome()) {
    // The previous value of `scheme` can be ignored here, since we do not
    // use https over unix domain sockets anyways.
    scheme = "http+unix";
    std::string path = value.get();

    if (path.size() >= 108) {
      std::string cwd = os::getcwd();
      VLOG(1) << "Path " << path << " too long, shortening it by using"
              << " the relative path to " << cwd;

      Try<std::string> relative = path::relative(path, cwd);
      if (relative.isError()) {
        EXIT(EXIT_FAILURE)
          << "Couldnt compute relative path of socket location: "
          << relative.error();
      }
      path = "./" + *relative;
    }

    // This should not happen, because the socket is supposed to
    // be in `$MESOS_SANDBOX/agent.sock`, so the relative path should
    // always be `./agent.sock`.
    if (path.size() >= 108) {
      EXIT(EXIT_FAILURE)
        << "Cannot use domain sockets for communication as requested: "
        << "Path " << path << " is longer than 108 characters";
    }

    agent = ::URL(
        scheme,
        path,
        upid.id + "/api/v1");
  } else
#endif // __WINDOWS__
  {
    agent = ::URL(
        scheme,
        upid.address.ip,
        upid.address.port,
        upid.id + "/api/v1");
  }

  LOG(INFO) << "Using URL " << agent << " for the streaming endpoint";

  process::initialize();

  value = os::getenv("MESOS_SANDBOX");
  if (value.isNone()) {
    EXIT(EXIT_FAILURE)
      << "Expecting 'MESOS_SANDBOX' to be set in the environment";
  }
  sandboxDirectory = value.get();

  Option<string> authorizationHeader;
  value = os::getenv("MESOS_EXECUTOR_AUTHENTICATION_TOKEN");
  if (value.isSome()) {
    authorizationHeader = "Bearer " + value.get();
  }

  Owned<mesos::internal::DefaultExecutor> executor(
      new mesos::internal::DefaultExecutor(
          frameworkId,
          executorId,
          agent,
          sandboxDirectory,
          flags.launcher_dir,
          authorizationHeader));

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
