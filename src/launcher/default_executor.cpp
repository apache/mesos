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
#include <queue>
#include <string>

#include <mesos/mesos.hpp>

#include <mesos/executor/executor.hpp>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/mesos.hpp>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/linkedhashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

using mesos::executor::Call;
using mesos::executor::Event;

using mesos::v1::executor::Mesos;

using process::Owned;

using std::cout;
using std::cerr;
using std::endl;
using std::queue;
using std::string;

namespace mesos {
namespace internal {

class DefaultExecutor : public process::Process<DefaultExecutor>
{
public:
  DefaultExecutor(
      const FrameworkID& _frameworkId,
      const ExecutorID& _executorId)
    : ProcessBase(process::ID::generate("default-executor")),
      state(DISCONNECTED),
      launched(false),
      frameworkInfo(None()),
      frameworkId(_frameworkId),
      executorId(_executorId) {}

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
        break;
      }

      case Event::LAUNCH: {
        cerr << "LAUNCH event is not supported" << endl;
        // Shut down because this is unexpected; `LAUNCH` event
        // should never go to the default executor.
        //
        // TODO(anand): Invoke `shutdown()`.
        break;
      }

      case Event::LAUNCH_GROUP: {
        launchGroup(event.launch_group().task_group());
        break;
      }

      case Event::KILL: {
        // TODO(anand): Implement this.
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
        // TODO(anand): Implement this.
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
        ContentType::PROTOBUF,
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

  void launchGroup(const TaskGroupInfo& _taskGroup)
  {
    CHECK_EQ(SUBSCRIBED, state);

    if (launched) {
      foreach (const TaskInfo& task, _taskGroup.tasks()) {
        update(
            task.task_id(),
            TASK_FAILED,
            "Attempted to run multiple task groups using a "
            "\"default\" executor");
      }
      return;
    }

    launched = true;

    foreach (const TaskInfo& task, _taskGroup.tasks()) {
      tasks[task.task_id()] = task;
    }

    // Send a TASK_RUNNING status update followed immediately by a
    // TASK_FINISHED update.
    //
    // TODO(anand): Eventually, we need to invoke the `LAUNCH_CONTAINER`
    // call via the Agent API.
    foreach (const TaskInfo& task, _taskGroup.tasks()) {
      update(task.task_id(), TASK_RUNNING);
    }

    foreach (const TaskInfo& task, _taskGroup.tasks()) {
      update(task.task_id(), TASK_FINISHED);
    }

    // TODO(qianzhang): Remove this hack since the executor now receives
    // acknowledgements for status updates. The executor can terminate
    // after it receives an ACK for a terminal status update.
    os::sleep(Seconds(1));
    terminate(self());
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

    if (message.isSome()) {
      status.set_message(message.get());
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

  enum State
  {
    CONNECTED,
    DISCONNECTED,
    SUBSCRIBED
  } state;

  bool launched;
  Option<FrameworkInfo> frameworkInfo;
  const FrameworkID frameworkId;
  const ExecutorID executorId;
  Owned<Mesos> mesos;
  LinkedHashMap<UUID, Call::Update> updates; // Unacknowledged updates.
  LinkedHashMap<TaskID, TaskInfo> tasks; // Unacknowledged tasks.
};

} // namespace internal {
} // namespace mesos {


int main(int argc, char** argv)
{
  mesos::FrameworkID frameworkId;
  mesos::ExecutorID executorId;

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

  Owned<mesos::internal::DefaultExecutor> executor(
      new mesos::internal::DefaultExecutor(
          frameworkId,
          executorId));

  process::spawn(executor.get());
  process::wait(executor.get());

  return EXIT_SUCCESS;
}
