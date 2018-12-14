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

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/exit.hpp>

#include "executor/v0_v1executor.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

using std::function;
using std::queue;
using std::string;

using mesos::internal::devolve;
using mesos::internal::evolve;

namespace mesos {
namespace v1 {
namespace executor {

// The process (below) is responsible for ensuring synchronized access between
// callbacks received from the driver and calls invoked by the adapter.
class V0ToV1AdapterProcess : public process::Process<V0ToV1AdapterProcess>
{
public:
  V0ToV1AdapterProcess(
      const function<void(void)>& connected,
      const function<void(void)>& disconnected,
      const function<void(const queue<Event>&)>& received)
    : ProcessBase(process::ID::generate("v0-to-v1-adapter")),
      callbacks {connected, disconnected, received},
      connected(false),
      subscribeCall(false) {}

  ~V0ToV1AdapterProcess() override = default;

  void registered(
      const mesos::ExecutorInfo& _executorInfo,
      const mesos::FrameworkInfo& _frameworkInfo,
      const mesos::SlaveInfo& slaveInfo)
  {
    if (!connected) {
      callbacks.connected();
      connected = true;
    }

    // We need these copies to populate the fields in `Event::Subscribed` upon
    // receiving a `reregistered()` callback later.
    executorInfo = _executorInfo;
    frameworkInfo = _frameworkInfo;

    Event event;
    event.set_type(Event::SUBSCRIBED);

    Event::Subscribed* subscribed = event.mutable_subscribed();

    subscribed->mutable_executor_info()->CopyFrom(evolve(executorInfo.get()));
    subscribed->mutable_framework_info()->CopyFrom(evolve(frameworkInfo.get()));
    subscribed->mutable_agent_info()->CopyFrom(evolve(slaveInfo));

    received(event);
  }

  void reregistered(
      const mesos::SlaveInfo& slaveInfo)
  {
    CHECK_SOME(frameworkInfo);
    CHECK_SOME(executorInfo);

    // For compatibility with the v1 interface, invoke the `disconnected`
    // callback before the `connected` callback upon executor re-registration.
    // This is needed because the driver does not invoke this callback upon
    // disconnection from the agent.
    callbacks.disconnected();
    callbacks.connected();
    connected = true;

    Event event;
    event.set_type(Event::SUBSCRIBED);

    Event::Subscribed* subscribed = event.mutable_subscribed();

    subscribed->mutable_executor_info()->CopyFrom(evolve(executorInfo.get()));
    subscribed->mutable_framework_info()->CopyFrom(evolve(frameworkInfo.get()));
    subscribed->mutable_agent_info()->CopyFrom(evolve(slaveInfo));

    received(event);
  }

  // TODO(anand): This method is currently not invoked by the
  // `MesosExecutorDriver`.
  void disconnected() {}

  void killTask(const mesos::TaskID& taskId)
  {
    // Logically an executor cannot receive any response from an agent if it
    // is not connected. Since we have received `killTask`, we assume we are
    // connected and trigger the `connected` callback to enable event delivery.
    // This satisfies the invariant of the v1 interface that an executor can
    // receive an event only after successfully connecting with the agent.
    if (!connected) {
      LOG(INFO) << "Implicitly connecting the executor to kill a task";
      callbacks.connected();
      connected = true;
    }

    Event event;
    event.set_type(Event::KILL);

    Event::Kill* kill = event.mutable_kill();

    kill->mutable_task_id()->CopyFrom(evolve(taskId));

    received(event);
  }

  void launchTask(const mesos::TaskInfo& task)
  {
    Event event;
    event.set_type(Event::LAUNCH);

    Event::Launch* launch = event.mutable_launch();

    launch->mutable_task()->CopyFrom(evolve(task));

    received(event);
  }

  void frameworkMessage(const string& data)
  {
    Event event;
    event.set_type(Event::MESSAGE);

    Event::Message* message = event.mutable_message();

    message->set_data(data);

    received(event);
  }

  void shutdown()
  {
    // Logically an executor cannot receive any response from an agent if it
    // is not connected. Since we have received `shutdown`, we assume we are
    // connected and trigger the `connected` callback to enable event delivery.
    // This satisfies the invariant of the v1 interface that an executor can
    // receive an event only after successfully connecting with the agent.
    if (!connected) {
      LOG(INFO) << "Implicitly connecting the executor to shut it down";
      callbacks.connected();
      connected = true;
    }

    Event event;
    event.set_type(Event::SHUTDOWN);

    received(event);
  }

  void error(const string& message)
  {
    // Logically an executor cannot receive any response from an agent if it
    // is not connected. Since we have received `error`, we assume we are
    // connected and trigger the `connected` callback to enable event delivery.
    // This satisfies the invariant of the v1 interface that an executor can
    // receive an event only after successfully connecting with the agent.
    if (!connected) {
      LOG(INFO) << "Implicitly connecting the executor to send an error";
      callbacks.connected();
      connected = true;
    }

    Event event;
    event.set_type(Event::ERROR);

    Event::Error* error = event.mutable_error();

    error->set_message(message);

    received(event);
  }

  void send(ExecutorDriver* driver, const Call& call)
  {
    CHECK_NOTNULL(driver);

    switch (call.type()) {
      case Call::SUBSCRIBE: {
        subscribeCall = true;

        // The driver subscribes implicitly with the agent upon initialization.
        // For compatibility with the v1 interface, send the already enqueued
        // subscribed event upon receiving the subscribe request.
        _received();

        break;
      }

      case Call::UPDATE: {
        driver->sendStatusUpdate(devolve(call.update().status()));
        break;
      }

      case Call::MESSAGE: {
        driver->sendFrameworkMessage(call.message().data());
        break;
      }

      case Call::HEARTBEAT: {
        // NOTE: Heartbeat calls were added to HTTP executors only.
        // There is no equivalent method for PID-based executors.
        break;
      }

      case Call::UNKNOWN: {
        EXIT(EXIT_FAILURE) << "Received an unexpected " << call.type()
                           << " call";
        break;
      }
    }
  }

protected:
  void received(const Event& event)
  {
    // For compatibility with the v1 interface, we only start sending events
    // once the executor has sent the subscribe call.
    if (!subscribeCall) {
      pending.push(event);
      return;
    }

    pending.push(event);

    _received();
  }

  void _received()
  {
    CHECK(subscribeCall);

    callbacks.received(pending);

    pending = queue<Event>();
  }

private:
  struct Callbacks
  {
    const function<void(void)> connected;
    const function<void(void)> disconnected;
    const function<void(const queue<Event>&)> received;
  };

  Callbacks callbacks;
  bool connected;
  bool subscribeCall;
  queue<Event> pending;
  Option<mesos::ExecutorInfo> executorInfo;
  Option<mesos::FrameworkInfo> frameworkInfo;
};


V0ToV1Adapter::V0ToV1Adapter(
    const function<void(void)>& connected,
    const function<void(void)>& disconnected,
    const function<void(const queue<Event>&)>& received)
  : process(new V0ToV1AdapterProcess(connected, disconnected, received)),
    driver(this)
{
  spawn(process.get());
  driver.start();
}


void V0ToV1Adapter::registered(
    ExecutorDriver*,
    const mesos::ExecutorInfo& executorInfo,
    const mesos::FrameworkInfo& frameworkInfo,
    const mesos::SlaveInfo& slaveInfo)
{
  process::dispatch(
      process.get(),
      &V0ToV1AdapterProcess::registered,
      executorInfo,
      frameworkInfo,
      slaveInfo);
}


void V0ToV1Adapter::reregistered(
    ExecutorDriver*,
    const mesos::SlaveInfo& slaveInfo)
{
  process::dispatch(
      process.get(), &V0ToV1AdapterProcess::reregistered, slaveInfo);
}


void V0ToV1Adapter::disconnected(ExecutorDriver*)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::disconnected);
}


void V0ToV1Adapter::killTask(ExecutorDriver*, const mesos::TaskID& taskId)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::killTask, taskId);
}


void V0ToV1Adapter::launchTask(ExecutorDriver*, const mesos::TaskInfo& task)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::launchTask, task);
}


void V0ToV1Adapter::frameworkMessage(ExecutorDriver*, const string& data)
{
  process::dispatch(
      process.get(), &V0ToV1AdapterProcess::frameworkMessage, data);
}


void V0ToV1Adapter::shutdown(ExecutorDriver*)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::shutdown);
}


void V0ToV1Adapter::error(ExecutorDriver*, const string& message)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::error, message);
}


void V0ToV1Adapter::send(const Call& call)
{
  process::dispatch(process.get(), &V0ToV1AdapterProcess::send, &driver, call);
}


V0ToV1Adapter::~V0ToV1Adapter()
{
  driver.stop();

  terminate(process.get());
  wait(process.get());
}

} // namespace executor {
} // namespace v1 {
} // namespace mesos {
