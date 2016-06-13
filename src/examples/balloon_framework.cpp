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

#include <glog/logging.h>

#include <iostream>
#include <string>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <stout/bytes.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

using namespace mesos;
using namespace mesos::internal;

using std::string;

const double CPUS_PER_TASK = 0.1;

const double CPUS_PER_EXECUTOR = 0.1;
const int32_t MEM_PER_EXECUTOR = 64;

class Flags : public flags::FlagsBase
{
public:
  Flags()
  {
    add(&master,
        "master",
        "Master to connect to.");

    add(&task_memory_usage_limit,
        "task_memory_usage_limit",
        None(),
        "Maximum size, in bytes, of the task's memory usage.\n"
        "The task will attempt to occupy memory up until this limit.",
        static_cast<const Bytes*>(nullptr),
        [](const Bytes& value) -> Option<Error> {
          if (value.megabytes() < MEM_PER_EXECUTOR) {
            return Error(
                "Please use a --task_memory_usage_limit greater than " +
                stringify(MEM_PER_EXECUTOR) + " MB");
          }

          return None();
        });

    add(&task_memory,
        "task_memory",
        "How much memory the framework will require per task.\n"
        "If not specified, the task(s) will use all available memory in\n"
        "applicable offers.");

    add(&build_dir,
        "build_dir",
        "The build directory of Mesos. If set, the framework will assume\n"
        "that the executor, framework, and agent(s) all live on the same\n"
        "machine.");

    add(&executor_uri,
        "executor_uri",
        "URI the fetcher should use to get the executor.");

    add(&executor_command,
        "executor_command",
        "The command that should be used to start the executor.\n"
        "This will override the value set by `--build_dir`.");

    add(&checkpoint,
        "checkpoint",
        "Whether this framework should be checkpointed.\n",
        false);

    add(&long_running,
        "long_running",
        "Whether this framework should launch tasks repeatedly\n"
        "or exit after finishing a single task.",
        false);
  }

  string master;
  Bytes task_memory_usage_limit;
  Bytes task_memory;

  // Flags for specifying the executor binary.
  Option<string> build_dir;
  Option<string> executor_uri;
  Option<string> executor_command;

  bool checkpoint;
  bool long_running;
};


// This scheduler starts a single executor and task which gradually
// increases its memory footprint up to a limit.  Depending on the
// resource limits set for the container, the framework expects the
// executor to either finish successfully or be OOM-killed.
class BalloonScheduler : public Scheduler
{
public:
  BalloonScheduler(
      const ExecutorInfo& _executor,
      const Flags& _flags)
    : executor(_executor),
      flags(_flags),
      taskActive(false),
      tasksLaunched(0) {}

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
    static const Resources TASK_RESOURCES = Resources::parse(
        "cpus:" + stringify(CPUS_PER_TASK) +
        ";mem:" + stringify(flags.task_memory.megabytes())).get();

    static const Resources EXECUTOR_RESOURCES = Resources(executor.resources());

    LOG(INFO) << "Resource offers received";

    foreach (const Offer& offer, offers) {
      Resources resources(offer.resources());

      // If there is an active task, or if the offer is not
      // big enough, reject the offer.
      if (taskActive || !resources.flatten().contains(
            TASK_RESOURCES + EXECUTOR_RESOURCES)) {
        Filters filters;
        filters.set_refuse_seconds(600);

        driver->declineOffer(offer.id(), filters);
        continue;
      }

      int taskId = tasksLaunched++;

      LOG(INFO) << "Starting task " << taskId;

      TaskInfo task;
      task.set_name("Balloon Task");
      task.mutable_task_id()->set_value(stringify(taskId));
      task.mutable_slave_id()->MergeFrom(offer.slave_id());
      task.mutable_resources()->CopyFrom(TASK_RESOURCES);
      task.set_data(stringify(flags.task_memory_usage_limit));

      task.mutable_executor()->CopyFrom(executor);
      task.mutable_executor()->mutable_executor_id()
        ->set_value(stringify(taskId));

      driver->launchTasks(offer.id(), {task});

      taskActive = true;
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

    if (!flags.long_running) {
      if (status.state() == TASK_FAILED &&
          status.reason() == TaskStatus::REASON_CONTAINER_LIMITATION_MEMORY) {
        // NOTE: We expect TASK_FAILED when this scheduler is launched by the
        // balloon_framework_test.sh shell script. The abort here ensures the
        // script considers the test result as "PASS".
        driver->abort();
      } else if (status.state() == TASK_FAILED ||
          status.state() == TASK_FINISHED ||
          status.state() == TASK_KILLED ||
          status.state() == TASK_LOST ||
          status.state() == TASK_ERROR) {
        driver->stop();
      }
    }

    // TODO(josephw): Add some metrics for some cases below.
    switch (status.state()) {
      case TASK_FINISHED:
      case TASK_FAILED:
      case TASK_KILLED:
      case TASK_LOST:
      case TASK_ERROR:
        taskActive = false;
        break;
      default:
        break;
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
  const Flags flags;
  bool taskActive;
  int tasksLaunched;
};


int main(int argc, char** argv)
{
  Flags flags;
  Try<flags::Warnings> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    EXIT(EXIT_FAILURE) << flags.usage(load.error());
  }

  const Resources resources = Resources::parse(
      "cpus:" + stringify(CPUS_PER_EXECUTOR) +
      ";mem:" + stringify(MEM_PER_EXECUTOR)).get();

  ExecutorInfo executor;
  executor.mutable_resources()->CopyFrom(resources);
  executor.set_name("Balloon Executor");
  executor.set_source("balloon_test");

  // Determine the command to run the executor based on three possibilities:
  //   1) `--executor_command` was set, which overrides the below cases.
  //   2) We are in the Mesos build directory, so the targeted executable
  //      is actually a libtool wrapper script.
  //   3) We have not detected the Mesos build directory, so assume the
  //      executor is in the same directory as the framework.
  string command;

  // Find this executable's directory to locate executor.
  if (flags.executor_command.isSome()) {
    command = flags.executor_command.get();
  } else if (flags.build_dir.isSome()) {
    command = path::join(
        flags.build_dir.get(), "src", "balloon-executor");
  } else {
    command = path::join(
        os::realpath(Path(argv[0]).dirname()).get(),
        "balloon-executor");
  }

  executor.mutable_command()->set_value(command);

  // Copy `--executor_uri` into the command.
  if (flags.executor_uri.isSome()) {
    mesos::CommandInfo::URI* uri = executor.mutable_command()->add_uris();
    uri->set_value(flags.executor_uri.get());
    uri->set_executable(true);
  }

  BalloonScheduler scheduler(executor, flags);

  // Log any flag warnings (after logging is initialized by the scheduler).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  FrameworkInfo framework;
  framework.set_user(os::user().get());
  framework.set_name("Balloon Framework (C++)");
  framework.set_checkpoint(flags.checkpoint);

  MesosSchedulerDriver* driver;

  // TODO(josephw): Refactor these into a common set of flags.
  Option<string> value = os::getenv("MESOS_AUTHENTICATE_FRAMEWORKS");
  if (value.isSome()) {
    LOG(INFO) << "Enabling authentication for the framework";

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
        &scheduler, framework, flags.master, credential);
  } else {
    framework.set_principal("balloon-framework-cpp");

    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master);
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
