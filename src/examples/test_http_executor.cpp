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

#include <iostream>
#include <queue>
#include <string>

#include <mesos/http.hpp>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/mesos.hpp>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/exit.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/uuid.hpp>

using std::cout;
using std::endl;
using std::queue;
using std::string;

using mesos::v1::ExecutorID;
using mesos::v1::FrameworkID;
using mesos::v1::TaskID;
using mesos::v1::TaskInfo;
using mesos::v1::TaskState;
using mesos::v1::TaskStatus;

using mesos::v1::executor::Call;
using mesos::v1::executor::Event;
using mesos::v1::executor::Mesos;

using process::spawn;
using process::wait;


class TestExecutor: public process::Process<TestExecutor>
{
public:
  TestExecutor(const FrameworkID& _frameworkId, const ExecutorID& _executorId)
    : frameworkId(_frameworkId),
      executorId(_executorId),
      state(DISCONNECTED) {}

  void connected()
  {
    state = CONNECTED;

    doReliableRegistration();
  }

  void doReliableRegistration()
  {
    if (state == SUBSCRIBED || state == DISCONNECTED) {
      return;
    }

    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();

    // Send all unacknowledged updates.
    foreachvalue (const Call::Update& update, updates) {
      subscribe->add_unacknowledged_updates()->MergeFrom(update);
    }

    // Send all unacknowledged tasks.
    foreachvalue (const TaskInfo& task, tasks) {
      subscribe->add_unacknowledged_tasks()->MergeFrom(task);
    }

    mesos->send(call);

    process::delay(Seconds(1), self(), &Self::doReliableRegistration);
  }

  void disconnected()
  {
    state = DISCONNECTED;
  }

  void sendStatusUpdate(const TaskInfo& task, const TaskState& state)
  {
    id::UUID uuid = id::UUID::random();

    TaskStatus status;
    status.mutable_task_id()->CopyFrom(task.task_id());
    status.mutable_executor_id()->CopyFrom(executorId);
    status.set_state(state);
    status.set_source(TaskStatus::SOURCE_EXECUTOR);
    status.set_uuid(uuid.toBytes());

    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    call.set_type(Call::UPDATE);

    call.mutable_update()->mutable_status()->CopyFrom(status);

    // Capture the status update.
    updates[uuid] = call.update();

    mesos->send(call);
  }

  void received(queue<Event> events)
  {
    while (!events.empty()) {
      Event event = events.front();
      events.pop();

      switch (event.type()) {
        case Event::SUBSCRIBED: {
          cout << "Received a SUBSCRIBED event" << endl;

          state = SUBSCRIBED;
          break;
        }

        case Event::LAUNCH: {
          const TaskInfo& task = event.launch().task();
          tasks[task.task_id()] = task;

          cout << "Starting task " << task.task_id().value() << endl;

          sendStatusUpdate(task, TaskState::TASK_RUNNING);

          // This is where one would perform the requested task.

          cout << "Finishing task " << task.task_id().value() << endl;

          sendStatusUpdate(task, TaskState::TASK_FINISHED);
          break;
        }

        case Event::LAUNCH_GROUP: {
          cout << "Received a LAUNCH_GROUP event";
          // TODO(vinod): Implement this.
          break;
        }

        case Event::KILL: {
          cout << "Received a KILL event" << endl;
          break;
        }

        case Event::ACKNOWLEDGED: {
          cout << "Received an ACKNOWLEDGED event" << endl;

          // Remove the corresponding update.
          updates.erase(id::UUID::fromBytes(event.acknowledged().uuid()).get());

          // Remove the corresponding task.
          tasks.erase(event.acknowledged().task_id());
          break;
        }

        case Event::MESSAGE: {
          cout << "Received a MESSAGE event" << endl;
          break;
        }

        case Event::SHUTDOWN: {
          cout << "Received a SHUTDOWN event" << endl;
          break;
        }

        case Event::ERROR: {
          cout << "Received an ERROR event" << endl;
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
  }

protected:
  void initialize() override
  {
    // We initialize the library here to ensure that callbacks are only invoked
    // after the process has spawned.
    mesos.reset(new Mesos(
        mesos::ContentType::PROTOBUF,
        process::defer(self(), &Self::connected),
        process::defer(self(), &Self::disconnected),
        process::defer(self(), &Self::received, lambda::_1)));
  }

private:
  const FrameworkID frameworkId;
  const ExecutorID executorId;
  process::Owned<Mesos> mesos;
  enum State
  {
    CONNECTED,
    DISCONNECTED,
    SUBSCRIBED
  } state;

  LinkedHashMap<id::UUID, Call::Update> updates; // Unacknowledged updates.
  LinkedHashMap<TaskID, TaskInfo> tasks; // Unacknowledged tasks.
};


int main()
{
  FrameworkID frameworkId;
  ExecutorID executorId;

  Option<string> value;

  value = os::getenv("MESOS_FRAMEWORK_ID");
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

  process::Owned<TestExecutor> executor(
      new TestExecutor(frameworkId, executorId));

  process::spawn(executor.get());
  process::wait(executor.get());

  return EXIT_SUCCESS;
}
