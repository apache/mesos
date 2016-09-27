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

#include <stout/linkedhashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"

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

using std::cout;
using std::cerr;
using std::endl;
using std::list;
using std::queue;
using std::string;

namespace mesos {
namespace internal {

class DefaultExecutor : public process::Process<DefaultExecutor>
{
public:
  DefaultExecutor(
      const FrameworkID& _frameworkId,
      const ExecutorID& _executorId,
      const ::URL& _agent)
    : ProcessBase(process::ID::generate("default-executor")),
      state(DISCONNECTED),
      contentType(ContentType::PROTOBUF),
      launched(false),
      shuttingDown(false),
      frameworkInfo(None()),
      executorContainerId(None()),
      frameworkId(_frameworkId),
      executorId(_executorId),
      agent(_agent) {}

  virtual ~DefaultExecutor() = default;

  void connected()
  {
    state = CONNECTED;

    doReliableRegistration();
  }

  void disconnected()
  {
    state = DISCONNECTED;
  }

  void received(const Event& event)
  {
    cout << "Received " << event.type() << " event" << endl;

    switch (event.type()) {
      case Event::SUBSCRIBED: {
        cout << "Subscribed executor on "
             << event.subscribed().slave_info().hostname() << endl;

        frameworkInfo = event.subscribed().framework_info();
        state = SUBSCRIBED;

        CHECK(event.subscribed().has_container_id());
        executorContainerId = event.subscribed().container_id();

        break;
      }

      case Event::LAUNCH: {
        cerr << "LAUNCH event is not supported" << endl;
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
        cerr << "Error: " << event.error().message() << endl;
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

      tasks[taskId] = task;
      containers[pending.front()] = taskId;

      pending.pop_front();
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

    // TODO(anand): We need to wait for the tasks via the
    // `WAIT_NESTED_CONTAINERS` call.
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

private:
  void update(
      const TaskID& taskId,
      const TaskState& state,
      const Option<string>& message = None())
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

    // TODO(vinod): Implement health checks.
    status.set_healthy(true);

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

  enum State
  {
    CONNECTED,
    DISCONNECTED,
    SUBSCRIBED
  } state;

  const ContentType contentType;
  bool launched;
  bool shuttingDown;
  Option<FrameworkInfo> frameworkInfo;
  Option<ContainerID> executorContainerId;
  const FrameworkID frameworkId;
  const ExecutorID executorId;
  Owned<Mesos> mesos;
  const ::URL agent; // Agent API URL.
  LinkedHashMap<UUID, Call::Update> updates; // Unacknowledged updates.
  LinkedHashMap<TaskID, TaskInfo> tasks; // Unacknowledged tasks.
  LinkedHashMap<ContainerID, TaskID> containers; // Active child containers.
};

} // namespace internal {
} // namespace mesos {


int main(int argc, char** argv)
{
  mesos::FrameworkID frameworkId;
  mesos::ExecutorID executorId;
  string scheme = "http"; // Default scheme.
  ::URL agent;

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

  Owned<mesos::internal::DefaultExecutor> executor(
      new mesos::internal::DefaultExecutor(
          frameworkId,
          executorId,
          agent));

  process::spawn(executor.get());
  process::wait(executor.get());

  return EXIT_SUCCESS;
}
