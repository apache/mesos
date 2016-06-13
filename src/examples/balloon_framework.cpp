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

#include <assert.h>
#include <stdlib.h>

#include <sys/param.h>

#include <iostream>
#include <string>
#include <vector>

#include <mesos/scheduler.hpp>

#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include "common/protobuf_utils.hpp"

#include "examples/utils.hpp"

using namespace mesos;
using namespace mesos::internal;

using std::cout;
using std::endl;
using std::string;

// The amount of memory in MB the executor itself takes.
const static size_t EXECUTOR_MEMORY_MB = 64;


class BalloonScheduler : public Scheduler
{
public:
  BalloonScheduler(
      const ExecutorInfo& _executor,
      size_t _balloonLimit)
    : executor(_executor),
      balloonLimit(_balloonLimit),
      taskLaunched(false) {}

  virtual ~BalloonScheduler() {}

  virtual void registered(
      SchedulerDriver*,
      const FrameworkID& frameworkId,
      const MasterInfo&)
  {
    LOG(INFO) << "Registered with framework ID: " << frameworkId;
  }

  virtual void reregistered(SchedulerDriver*, const MasterInfo& masterInfo)
  {
    LOG(INFO) << "Reregistered";
  }

  virtual void disconnected(SchedulerDriver* driver)
  {
    LOG(INFO) << "Disconnected";
  }

  virtual void resourceOffers(
      SchedulerDriver* driver,
      const std::vector<Offer>& offers)
  {
    LOG(INFO) << "Resource offers received";

    for (size_t i = 0; i < offers.size(); i++) {
      const Offer& offer = offers[i];

      // We just launch one task.
      if (!taskLaunched) {
        double mem = getScalarResource(offer, "mem");
        assert(mem > EXECUTOR_MEMORY_MB);

        std::vector<TaskInfo> tasks;
        LOG(INFO) << "Starting the task";

        TaskInfo task;
        task.set_name("Balloon Task");
        task.mutable_task_id()->set_value("1");
        task.mutable_slave_id()->MergeFrom(offer.slave_id());
        task.mutable_executor()->MergeFrom(executor);
        task.set_data(stringify<size_t>(balloonLimit));

        // Use up all the memory from the offer.
        Resource* resource;
        resource = task.add_resources();
        resource->set_name("mem");
        resource->set_type(Value::SCALAR);
        resource->mutable_scalar()->set_value(mem - EXECUTOR_MEMORY_MB);

        // And all the CPU.
        double cpus = getScalarResource(offer, "cpus");
        resource = task.add_resources();
        resource->set_name("cpus");
        resource->set_type(Value::SCALAR);
        resource->mutable_scalar()->set_value(cpus);

        tasks.push_back(task);
        driver->launchTasks(offer.id(), tasks);

        taskLaunched = true;
      }
    }
  }

  virtual void offerRescinded(
      SchedulerDriver* driver,
      const OfferID& offerId)
  {
    LOG(INFO) << "Offer rescinded";
  }

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    LOG(INFO)
      << "Task " << status.task_id() << " in state "
      << TaskState_Name(status.state())
      << ", Source: " << status.source()
      << ", Reason: " << status.reason()
      << (status.has_message() ? ", Message: " + status.message() : "");

    if (protobuf::isTerminalState(status.state())) {
      // NOTE: We expect TASK_FAILED here. The abort here ensures the shell
      // script invoking this test, considers the test result as 'PASS'.
      if (status.state() == TASK_FAILED) {
        driver->abort();
      } else {
        driver->stop();
      }
    }
  }

  virtual void frameworkMessage(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      const string& data)
  {
    LOG(INFO) << "Framework message: " << data;
  }

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& slaveId)
  {
    LOG(INFO) << "Agent lost: " << slaveId;
  }

  virtual void executorLost(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      int status)
  {
    LOG(INFO) << "Executor '" << executorId << "' lost on agent: " << slaveId;
  }

  virtual void error(SchedulerDriver* driver, const string& message)
  {
    LOG(INFO) << "Error message: " << message;
  }

private:
  const ExecutorInfo executor;
  const size_t balloonLimit;
  bool taskLaunched;
};


int main(int argc, char** argv)
{
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0]
              << " <master> <balloon limit in MB>" << std::endl;
    return -1;
  }

  // Verify the balloon limit.
  Try<size_t> limit = numify<size_t>(argv[2]);
  if (limit.isError()) {
    std::cerr << "Balloon limit is not a valid number" << std::endl;
    return -1;
  }

  if (limit.get() < EXECUTOR_MEMORY_MB) {
    std::cerr << "Please use a balloon limit bigger than "
              << EXECUTOR_MEMORY_MB << " MB" << std::endl;
  }

  // Find this executable's directory to locate executor.
  string uri;
  Option<string> value = os::getenv("MESOS_BUILD_DIR");
  if (value.isSome()) {
    uri = path::join(value.get(), "src", "balloon-executor");
  } else {
    uri = path::join(
        os::realpath(Path(argv[0]).dirname()).get(),
        "balloon-executor");
  }

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value(uri);
  executor.set_name("Balloon Executor");
  executor.set_source("balloon_test");

  Resource* mem = executor.add_resources();
  mem->set_name("mem");
  mem->set_type(Value::SCALAR);
  mem->mutable_scalar()->set_value(EXECUTOR_MEMORY_MB);

  BalloonScheduler scheduler(executor, limit.get());

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_name("Balloon Framework (C++)");

  value = os::getenv("MESOS_CHECKPOINT");
  if (value.isSome()) {
    framework.set_checkpoint(
        numify<bool>(value.get()).get());
  }

  MesosSchedulerDriver* driver;
  value = os::getenv("MESOS_AUTHENTICATE_FRAMEWORKS");
  if (value.isSome()) {
    cout << "Enabling authentication for the framework" << endl;

    value = os::getenv("DEFAULT_PRINCIPAL");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting authentication principal in the environment";
    }

    Credential credential;
    credential.set_principal(value.get());

    framework.set_principal(value.get());

    value = os::getenv("DEFAULT_SECRET");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting authentication secret in the environment";
    }

    credential.set_secret(value.get());

    driver = new MesosSchedulerDriver(
        &scheduler, framework, argv[1], credential);
  } else {
    framework.set_principal("balloon-framework-cpp");

    driver = new MesosSchedulerDriver(
        &scheduler, framework, argv[1]);
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
