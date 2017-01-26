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

#include <string>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>
#include <mesos/type_utils.hpp>

#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

using namespace mesos;
using namespace mesos::internal;

using std::string;
using std::vector;

using mesos::Resources;


static Offer::Operation LAUNCH(const vector<TaskInfo>& tasks)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH);

  foreach (const TaskInfo& task, tasks) {
    operation.mutable_launch()->add_task_infos()->CopyFrom(task);
  }

  return operation;
}


class NoExecutorScheduler : public Scheduler
{
public:
  NoExecutorScheduler(
      const FrameworkInfo& _frameworkInfo,
      const string& _command,
      const Resources& _taskResources,
      const Option<size_t>& _totalTasks)
    : frameworkInfo(_frameworkInfo),
      command(_command),
      taskResources(_taskResources),
      tasksLaunched(0u),
      tasksFinished(0u),
      tasksTerminated(0u),
      totalTasks(_totalTasks) {}

  virtual void registered(
      SchedulerDriver* driver,
      const FrameworkID& frameworkId,
      const MasterInfo& masterInfo)
  {
    LOG(INFO) << "Registered with master " << masterInfo
              << " and got framework ID " << frameworkId;

    frameworkInfo.mutable_id()->CopyFrom(frameworkId);
  }

  virtual void reregistered(
      SchedulerDriver* driver,
      const MasterInfo& masterInfo)
  {
    LOG(INFO) << "Reregistered with master " << masterInfo;
  }

  virtual void disconnected(
      SchedulerDriver* driver)
  {
    LOG(INFO) << "Disconnected!";
  }

  virtual void resourceOffers(
      SchedulerDriver* driver,
      const vector<Offer>& offers)
  {
    foreach (const Offer& offer, offers) {
      LOG(INFO) << "Received offer " << offer.id() << " from agent "
                << offer.slave_id() << " (" << offer.hostname() << ") "
                << "with " << offer.resources();

      Resources remaining = offer.resources();
      vector<TaskInfo> tasks;

      while (remaining.contains(taskResources) &&
             (totalTasks.isNone() || tasksLaunched < totalTasks.get())) {
        TaskInfo task;
        task.mutable_task_id()->set_value(stringify(tasksLaunched));
        task.set_name(command);
        task.mutable_slave_id()->CopyFrom(offer.slave_id());
        task.mutable_resources()->CopyFrom(taskResources);
        task.mutable_command()->set_shell(true);
        task.mutable_command()->set_value(command);

        remaining -= taskResources;

        tasks.push_back(task);
        activeTasks.insert(task.task_id());
        tasksLaunched++;
      }

      driver->acceptOffers({offer.id()}, {LAUNCH(tasks)});
    }
  }

  virtual void offerRescinded(
      SchedulerDriver* driver,
      const OfferID& offerId)
  {
    LOG(INFO) << "Offer " << offerId << " has been rescinded";
  }

  virtual void statusUpdate(
      SchedulerDriver* driver,
      const TaskStatus& status)
  {
    if (!activeTasks.contains(status.task_id())) {
      LOG(WARNING) << "Unknown task '" << status.task_id() << "'"
                   << " is in state " << status.state();
      return;
    }

    if (status.state() == TASK_LOST ||
        status.state() == TASK_KILLED ||
        status.state() == TASK_FAILED) {
      LOG(ERROR) << "Task '" << status.task_id() << "'"
                 << " is in unexpected state " << status.state()
                 << (status.has_reason()
                     ? " with reason " + stringify(status.reason()) : "")
                 << " from source " << status.source()
                 << " with message '" << status.message() << "'";
    } else {
      LOG(INFO) << "Task '" << status.task_id() << "'"
                << " is in state " << status.state();
    }

    if (internal::protobuf::isTerminalState(status.state())) {
      if (status.state() == TASK_FINISHED) {
        tasksFinished++;
      }

      tasksTerminated++;
      activeTasks.erase(status.task_id());
    }

    if (totalTasks.isSome() && tasksTerminated == totalTasks.get()) {
      if (tasksTerminated - tasksFinished > 0) {
        EXIT(EXIT_FAILURE)
          << "Failed to complete successfully: "
          << stringify(tasksTerminated - tasksFinished)
          << " of " << stringify(totalTasks.get()) << " terminated abnormally";
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
    LOG(FATAL) << "Received framework message from executor '" << executorId
               << "' on agent " << slaveId << ": '" << data << "'";
  }

  virtual void slaveLost(
      SchedulerDriver* driver,
      const SlaveID& slaveId)
  {
    LOG(INFO) << "Lost agent " << slaveId;
  }

  virtual void executorLost(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      int status)
  {
    LOG(INFO) << "Lost executor '" << executorId << "' on agent "
              << slaveId << ", " << WSTRINGIFY(status);
  }

  virtual void error(
      SchedulerDriver* driver,
      const string& message)
  {
    LOG(ERROR) << message;
  }

private:
  FrameworkInfo frameworkInfo;

  string command;
  Resources taskResources;

  hashset<TaskID> activeTasks;
  size_t tasksLaunched;
  size_t tasksFinished;
  size_t tasksTerminated;

  Option<size_t> totalTasks;
};


class Flags : public virtual logging::Flags
{
public:
  Flags()
  {
    add(&Flags::master,
        "master",
        "The master to connect to. May be one of:\n"
        "  master@addr:port (The PID of the master)\n"
        "  zk://host1:port1,host2:port2,.../path\n"
        "  zk://username:password@host1:port1,host2:port2,.../path\n"
        "  file://path/to/file (where file contains one of the above)");

    add(&Flags::checkpoint,
        "checkpoint",
        "Whether to enable checkpointing (true by default).",
        true);

    add(&Flags::principal,
        "principal",
        "To enable authentication, both --principal and --secret\n"
        "must be supplied.");

    add(&Flags::secret,
        "secret",
        "To enable authentication, both --principal and --secret\n"
        "must be supplied.");

    add(&Flags::command,
        "command",
        "The command to run for each task.",
        "echo hello");

    add(&Flags::task_resources,
        "task_resources",
        "The resources that the task uses.",
        "cpus:0.1;mem:32;disk:32");

    // TODO(bmahler): We need to take a separate flag for
    // revocable resources because there is no support yet
    // for specifying revocable resources in a resource string.
    add(&Flags::task_revocable_resources,
        "task_revocable_resources",
        "The revocable resources that the task uses.");

    add(&Flags::num_tasks,
        "num_tasks",
        "Optionally, the number of tasks to run to completion before exiting.\n"
        "If unset, as many tasks as possible will be launched.");
  }

  Option<string> master;
  bool checkpoint;
  Option<string> principal;
  Option<string> secret;
  string command;
  string task_resources;
  Option<string> task_revocable_resources;
  Option<size_t> num_tasks;
};


int main(int argc, char** argv)
{
  Flags flags;

  Try<flags::Warnings> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    EXIT(EXIT_FAILURE)
      << flags.usage(load.error());
  }

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  if (flags.help) {
    EXIT(EXIT_SUCCESS)
      << flags.usage();
  }

  if (flags.master.isNone()) {
    EXIT(EXIT_FAILURE)
      << flags.usage("Missing required option --master");
  }

  if (flags.principal.isSome() != flags.secret.isSome()) {
    EXIT(EXIT_FAILURE)
      << flags.usage("Both --principal and --secret are required"
                     " to enable authentication");
  }

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_name("No Executor Framework");
  framework.set_checkpoint(flags.checkpoint);

  if (flags.task_revocable_resources.isSome()) {
    framework.add_capabilities()->set_type(
        FrameworkInfo::Capability::REVOCABLE_RESOURCES);
  }

  if (flags.principal.isSome()) {
    framework.set_principal(flags.principal.get());
  }

  Try<Resources> resources =
    Resources::parse(flags.task_resources);

  if (resources.isError()) {
    EXIT(EXIT_FAILURE)
      << flags.usage("Invalid --task_resources: " +
                     resources.error());
  }

  Resources taskResources = resources.get();

  if (flags.task_revocable_resources.isSome()) {
    Try<Resources> revocableResources =
      Resources::parse(flags.task_revocable_resources.get());

    if (revocableResources.isError()) {
      EXIT(EXIT_FAILURE)
        << flags.usage("Invalid --task_revocable_resources: " +
                       revocableResources.error());
    }

    foreach (Resource revocable, revocableResources.get()) {
      revocable.mutable_revocable();
      taskResources += revocable;
    }
  }

  taskResources.allocate(framework.role());

  logging::initialize(argv[0], flags, true); // Catch signals.

  NoExecutorScheduler scheduler(
      framework,
      flags.command,
      taskResources,
      flags.num_tasks);

  MesosSchedulerDriver* driver;

  if (flags.principal.isSome() && flags.secret.isSome()) {
    Credential credential;
    credential.set_principal(flags.principal.get());
    credential.set_secret(flags.secret.get());

    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master.get(), credential);
  } else {
    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master.get());
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
