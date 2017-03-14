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

#include <glog/logging.h>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/authorizer/acls.hpp>

#include <stout/check.hpp>
#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

using namespace mesos;

using std::string;
using std::vector;

const int32_t CPUS_PER_TASK = 1;
const int32_t MEM_PER_TASK = 128;

// The framework reserves resources to run at most one task at a time
// on each agent; the resources are reserved when they are offered to
// the framework for the first time, and are unreserved when all tasks
// are done. The framework terminates if any task fails.
class DynamicReservationScheduler : public Scheduler
{
public:
  DynamicReservationScheduler(
      const string& _command,
      const string& _role,
      const string& _principal)
    : command(_command),
      role(_role),
      tasksLaunched(0),
      tasksFinished(0),
      totalTasks(5),
      principal(_principal)
  {
    reservationInfo.set_principal(principal);

    taskResources = Resources::parse(
        "cpus:" + stringify(CPUS_PER_TASK) +
        ";mem:" + stringify(MEM_PER_TASK)).get();

    taskResources.allocate(role);

    // The task will run on reserved resources.
    Try<Resources> flattened = taskResources.flatten(role, reservationInfo);
    CHECK_SOME(flattened);
    taskResources = flattened.get();
  }

  virtual ~DynamicReservationScheduler() {}

  virtual void registered(SchedulerDriver*,
                          const FrameworkID&,
                          const MasterInfo&)
  {
    LOG(INFO) << "Registered!";
  }

  virtual void reregistered(SchedulerDriver*, const MasterInfo& masterInfo) {}

  virtual void disconnected(SchedulerDriver* driver) {}

  virtual void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers)
  {
    foreach (const Offer& offer, offers) {
      LOG(INFO) << "Received offer " << offer.id() << " with "
                << offer.resources();

      // If the framework got this offer for the first time, the state is
      // `State::INIT`; framework will reserve it (sending RESERVE operation
      // to master) in this loop.
      if (!states.contains(offer.slave_id())) {
        // If all tasks were launched, do not reserve more resources; wait
        // for them to finish and unreserve resources.
        if (tasksLaunched == totalTasks) {
          continue;
        }

        states[offer.slave_id()] = State::INIT;
      }

      const State state = states[offer.slave_id()];

      Filters filters;
      filters.set_refuse_seconds(0);

      switch (state) {
        case State::INIT: {
          // Framework reserves resources from this offer for only one task;
          // the task'll be dispatched when reserved resources are re-offered
          // to this framework.
          Resources resources = offer.resources();
          Offer::Operation reserve = RESERVE(taskResources);

          Try<Resources> apply = resources.apply(reserve);
          if (apply.isError()) {
            LOG(INFO) << "Failed to reserve resources for task in offer "
                      << stringify(offer.id()) << ": " << apply.error();
            break;
          }

          driver->acceptOffers({offer.id()}, {reserve}, filters);
          states[offer.slave_id()] = State::RESERVING;
          break;
        }
        case State::RESERVING: {
          Resources resources = offer.resources();
          Resources reserved = resources.reserved(role);
          if (!reserved.contains(taskResources)) {
            break;
          }
          states[offer.slave_id()] = State::RESERVED;

          // We fallthrough here to save an offer cycle.
        }
        case State::RESERVED: {
          Resources resources = offer.resources();
          Resources reserved = resources.reserved(role);

          CHECK(reserved.contains(taskResources));

          // If all tasks were launched, unreserve those resources.
          if (tasksLaunched == totalTasks) {
            driver->acceptOffers(
                {offer.id()}, {UNRESERVE(taskResources)}, filters);
            states[offer.slave_id()] = State::UNRESERVING;
            break;
          }

          // Framework dispatches task on the reserved resources.
          CHECK(tasksLaunched < totalTasks);

          // Launch tasks on reserved resources.
          const string& taskId = stringify(tasksLaunched++);
          LOG(INFO) << "Launching task " << taskId << " using offer "
                    << offer.id();
          TaskInfo task;
          task.set_name("Task " + taskId + ": " + command);
          task.mutable_task_id()->set_value(taskId);
          task.mutable_slave_id()->MergeFrom(offer.slave_id());
          task.mutable_command()->set_shell(true);
          task.mutable_command()->set_value(command);
          task.mutable_resources()->MergeFrom(taskResources);
          driver->launchTasks(offer.id(), {task}, filters);
          states[offer.slave_id()] = State::TASK_RUNNING;
          break;
        }
        case State::TASK_RUNNING:
          LOG(INFO) << "The task on " << offer.slave_id()
                    << " is running, waiting for task done";
          break;
        case State::UNRESERVING: {
          Resources resources = offer.resources();
          Resources reserved = resources.reserved(role);
          if (!reserved.contains(taskResources)) {
            states[offer.slave_id()] = State::UNRESERVED;
          }
          break;
        }
        case State::UNRESERVED:
          // If state of slave is UNRESERVED, ignore it. The driver is stopped
          // when all tasks are done and all resources are unreserved.
          break;
      }
    }

    // If all tasks were done and all resources were unreserved,
    // stop the driver.
    if (tasksFinished == totalTasks) {
      // If all resources were unreserved, stop the driver.
      foreachvalue (const State& state, states) {
        if (state != State::UNRESERVED) {
          return;
        }
      }

      driver->stop();
    }
  }

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) {}

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    const string& taskId = status.task_id().value();

    if (status.state() == TASK_FINISHED) {
      ++tasksFinished;
      // Mark state of slave as RESERVED, so other tasks can reuse it.
      CHECK(states[status.slave_id()] == State::TASK_RUNNING);
      states[status.slave_id()] = State::RESERVED;
      LOG(INFO) << "Task " << taskId << " is finished at agent "
                << status.slave_id();
    } else {
      LOG(INFO) << "Task " << taskId << " is in state " << status.state();
    }

    if (status.state() == TASK_LOST ||
        status.state() == TASK_KILLED ||
        status.state() == TASK_FAILED) {
      LOG(INFO) << "Aborting because task " << taskId
                << " is in unexpected state " << status.state()
                << " with reason " << status.reason()
                << " from source " << status.source()
                << " with message '" << status.message() << "'";
      driver->abort();
    }

    if (tasksFinished == totalTasks) {
      LOG(INFO) << "All tasks done, waiting for unreserving resources";
    }
  }

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const ExecutorID& executorId,
                                const SlaveID& slaveId,
                                const string& data) {}

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& slaveId) {}

  virtual void executorLost(SchedulerDriver* driver,
                            const ExecutorID& executorId,
                            const SlaveID& slaveId,
                            int status) {}

  virtual void error(SchedulerDriver* driver, const string& message)
  {
    LOG(ERROR) << message;
  }

private:
  string command;
  string role;
  int tasksLaunched;
  int tasksFinished;
  int totalTasks;
  string principal;


  //              | Next State    | Action
  // ------------------------------------------
  // INIT         | RESERVING     | Transfer to RESERVING after sending
  //              |               | reserve operation to master.
  // ------------------------------------------
  // RESERVING    | RESERVED      | Transfer to RESERVED when got reserved
  //              |               | resources from master.
  // ------------------------------------------
  // RESERVED     | TASK_RUNNING  | Transfer to TASK_RUNNING after launching
  //              |               | on reserved resources.
  //              | UNRESERVING   | Transfer to UNRESERVING if all tasks are
  //              |               | launched; sending unreserve operation to
  //              |               | master.
  // ------------------------------------------
  // TASK_RUNNING | RESERVED      | Transfer to RESERVED if the running task
  //              |               | done; the resource is reused by other
  //              |               | tasks.
  // ------------------------------------------
  // UNRESERVING  | UNRESERVED    | Transfer to UNRESERVED if the offer did
  //              |               | not include reserved resources.
  // ------------------------------------------
  // UNRESERVED   | -             | If all resources are unreserved, stop
  //              |               | the driver.

  enum State {
    INIT,         // The framework received the offer for the first time.
    RESERVING,    // The framework sent the RESERVE request to master.
    RESERVED,     // The framework got reserved resources from master.
    TASK_RUNNING, // The task was dispatched to master.
    UNRESERVING,  // The framework sent the UNRESERVE request to master.
    UNRESERVED,   // The resources were unreserved.
  };

  hashmap<SlaveID, State> states;
  Resource::ReservationInfo reservationInfo;
  Resources taskResources;

  Offer::Operation RESERVE(const Resources& resources)
  {
    Offer::Operation operation;
    operation.set_type(Offer::Operation::RESERVE);
    operation.mutable_reserve()->mutable_resources()->CopyFrom(resources);
    return operation;
  }

  Offer::Operation UNRESERVE(const Resources& resources)
  {
    Offer::Operation operation;
    operation.set_type(Offer::Operation::UNRESERVE);
    operation.mutable_unreserve()->mutable_resources()->CopyFrom(resources);
    return operation;
  }
};


class Flags : public virtual flags::FlagsBase
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

    add(&Flags::role,
        "role",
        "Role to use when registering");

    add(&Flags::principal,
        "principal",
        "The principal used to identify this framework",
        "test");

    add(&Flags::command,
        "command",
        "The command to run for each task.",
        "echo hello");
  }

  Option<string> master;
  Option<string> role;
  string principal;
  string command;
};

int main(int argc, char** argv)
{
  Flags flags;

  Try<flags::Warnings> load = flags.load(None(), argc, argv);
  if (load.isError()) {
    EXIT(EXIT_FAILURE) << flags.usage(load.error());
  } else if (flags.master.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("Missing --master");
  } else if (flags.role.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("Missing --role");
  } else if (flags.role.get() == "*") {
    EXIT(EXIT_FAILURE)
      << flags.usage("Role is incorrect; the default '*' role cannot be used");
  }

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  FrameworkInfo framework;
  framework.set_user(""); // Mesos'll fill in the current user.
  framework.set_name("Dynamic Reservation Framework (C++)");
  framework.set_role(flags.role.get());
  framework.set_principal(flags.principal);

  DynamicReservationScheduler scheduler(
      flags.command,
      flags.role.get(),
      flags.principal);

  if (flags.master.get() == "local") {
    // Configure master.
    os::setenv("MESOS_AUTHENTICATE_FRAMEWORKS", "false");

    ACLs acls;
    ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_roles()->add_values(flags.role.get());
    os::setenv("MESOS_ACLS", stringify(JSON::protobuf(acls)));
  }

  MesosSchedulerDriver* driver = new MesosSchedulerDriver(
      &scheduler,
      framework,
      flags.master.get());

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
