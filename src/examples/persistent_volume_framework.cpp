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

#include <stdlib.h>

#include <sstream>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/authorizer/acls.hpp>

#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include "common/status_utils.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

using namespace mesos;
using namespace mesos::internal;

using std::cerr;
using std::cout;
using std::endl;
using std::ostringstream;
using std::string;
using std::vector;


// TODO(jieyu): Currently, persistent volume is only allowed for
// reserved resources.
static Resources SHARD_INITIAL_RESOURCES(const string& role)
{
  return Resources::parse("cpus:0.1;mem:32;disk:16", role).get();
}


static Resource SHARD_PERSISTENT_VOLUME(
    const string& role,
    const string& persistenceId,
    const string& containerPath)
{
  Volume volume;
  volume.set_container_path(containerPath);
  volume.set_mode(Volume::RW);

  Resource::DiskInfo info;
  info.mutable_persistence()->set_id(persistenceId);
  info.mutable_volume()->CopyFrom(volume);

  Resource resource = Resources::parse("disk", "8", role).get();
  resource.mutable_disk()->CopyFrom(info);

  return resource;
}


static Offer::Operation CREATE(const Resources& volumes)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::CREATE);
  operation.mutable_create()->mutable_volumes()->CopyFrom(volumes);
  return operation;
}


static Offer::Operation LAUNCH(const vector<TaskInfo>& tasks)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH);

  foreach (const TaskInfo& task, tasks) {
    operation.mutable_launch()->add_task_infos()->CopyFrom(task);
  }

  return operation;
}


// The framework launches a task on each registered slave using a
// persistent volume. It restarts the task once the previous one on
// the slave finishes. The framework terminates once the number of
// tasks launched on each slave reaches a limit.
class PersistentVolumeScheduler : public Scheduler
{
public:
  PersistentVolumeScheduler(
      const FrameworkInfo& _frameworkInfo,
      size_t numShards,
      size_t tasksPerShard)
    : frameworkInfo(_frameworkInfo)
  {
    for (size_t i = 0; i < numShards; i++) {
      shards.push_back(Shard(
          "shard-" + stringify(i),
          frameworkInfo.role(),
          tasksPerShard));
    }
  }

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

      Resources offered = offer.resources();

      // The operation we will perform on the offer.
      vector<Offer::Operation> operations;

      foreach (Shard& shard, shards) {
        switch (shard.state) {
          case Shard::INIT:
            if (offered.contains(shard.resources)) {
              Resource volume = SHARD_PERSISTENT_VOLUME(
                  frameworkInfo.role(),
                  UUID::random().toString(),
                  "volume");

              Try<Resources> resources = shard.resources.apply(CREATE(volume));
              CHECK_SOME(resources);

              TaskInfo task;
              task.set_name(shard.name);
              task.mutable_task_id()->set_value(UUID::random().toString());
              task.mutable_slave_id()->CopyFrom(offer.slave_id());
              task.mutable_resources()->CopyFrom(resources.get());
              task.mutable_command()->set_value("touch volume/persisted");

              // Update the shard.
              shard.state = Shard::STAGING;
              shard.taskId = task.task_id();
              shard.volume.id = volume.disk().persistence().id();
              shard.volume.slave = offer.slave_id().value();
              shard.resources = resources.get();
              shard.launched++;

              operations.push_back(CREATE(volume));
              operations.push_back(LAUNCH({task}));

              resources = offered.apply(vector<Offer::Operation>{
                  CREATE(volume),
                  LAUNCH({task})});

              CHECK_SOME(resources);
              offered = resources.get();
            }
            break;
          case Shard::WAITING:
            if (offered.contains(shard.resources)) {
              CHECK_EQ(shard.volume.slave, offer.slave_id().value());

              TaskInfo task;
              task.set_name(shard.name);
              task.mutable_task_id()->set_value(UUID::random().toString());
              task.mutable_slave_id()->CopyFrom(offer.slave_id());
              task.mutable_resources()->CopyFrom(shard.resources);
              task.mutable_command()->set_value("test -f volume/persisted");

              // Update the shard.
              shard.state = Shard::STAGING;
              shard.taskId = task.task_id();
              shard.launched++;

              operations.push_back(LAUNCH({task}));
            }
            break;
          case Shard::STAGING:
          case Shard::RUNNING:
          case Shard::DONE:
            // Ignore the offer.
            break;
          default:
            LOG(ERROR) << "Unexpected shard state: " << shard.state;
            driver->abort();
            break;
        }
      }

      driver->acceptOffers({offer.id()}, operations);
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
    LOG(INFO) << "Task '" << status.task_id() << "' is in state "
              << status.state();

    foreach (Shard& shard, shards) {
      if (shard.taskId == status.task_id()) {
        switch (status.state()) {
          case TASK_RUNNING:
            shard.state = Shard::RUNNING;
            break;
          case TASK_FINISHED:
            if (shard.launched >= shard.tasks) {
              shard.state = Shard::DONE;
            } else {
              shard.state = Shard::WAITING;
            }
            break;
          case TASK_STAGING:
          case TASK_STARTING:
            // Ignore the status update.
            break;
          default:
            LOG(ERROR) << "Unexpected task state " << status.state()
                       << " for task '" << status.task_id() << "'";
            driver->abort();
            break;
        }

        break;
      }
    }

    // Check the terminal condition.
    bool terminal = true;
    foreach (const Shard& shard, shards) {
      if (shard.state != Shard::DONE) {
        terminal = false;
        break;
      }
    }

    if (terminal) {
      driver->stop();
    }
  }

  virtual void frameworkMessage(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      const string& data)
  {
    LOG(INFO) << "Received framework message from executor '" << executorId
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
  struct Shard
  {
    enum State
    {
      INIT = 0,   // The shard hasn't been launched yet.
      STAGING,    // The shard has been launched.
      RUNNING,    // The shard is running.
      WAITING,    // The shard is waiting to be re-launched.
      DONE,       // The shard has finished all tasks.

      // TODO(jieyu): Add another state so that we can track the
      // destroy of the volume once all tasks finish.
    };

    // The persistent volume associated with this shard.
    struct Volume
    {
      // The persistence ID.
      string id;

      // An identifier used to uniquely identify a slave (even across
      // reboot). In the test, we use the slave ID since slaves will not
      // be rebooted. Note that we cannot use hostname as the identifier
      // in a local cluster because all slaves share the same hostname.
      string slave;
    };

    Shard(const string& _name, const string& role, size_t _tasks)
      : name(_name),
        state(INIT),
        resources(SHARD_INITIAL_RESOURCES(role)),
        launched(0),
        tasks(_tasks) {}

    string name;
    State state;          // The current state of this shard.
    TaskID taskId;        // The ID of the current task.
    Volume volume;        // The persistent volume associated with the shard.
    Resources resources;  // Resources required to launch the shard.
    size_t launched;      // How many tasks this shard has launched.
    size_t tasks;         // How many tasks this shard should launch.
  };

  FrameworkInfo frameworkInfo;
  vector<Shard> shards;
};


class Flags : public logging::Flags
{
public:
  Flags()
  {
    add(&master,
        "master",
        "The master to connect to. May be one of:\n"
        "  master@addr:port (The PID of the master)\n"
        "  zk://host1:port1,host2:port2,.../path\n"
        "  zk://username:password@host1:port1,host2:port2,.../path\n"
        "  file://path/to/file (where file contains one of the above)");

    add(&role,
        "role",
        "Role to use when registering",
        "test");

    add(&principal,
        "principal",
        "The principal used to identify this framework",
        "test");

    add(&num_shards,
        "num_shards",
        "The number of shards the framework will run.",
        3);

    add(&tasks_per_shard,
        "tasks_per_shard",
        "The number of tasks should be launched per shard.",
        3);
  }

  Option<string> master;
  string role;
  string principal;
  size_t num_shards;
  size_t tasks_per_shard;
};


int main(int argc, char** argv)
{
  Flags flags;

  Try<flags::Warnings> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (flags.master.isNone()) {
    cerr << flags.usage("Missing required option --master") << endl;
    return EXIT_FAILURE;
  }

  logging::initialize(argv[0], flags, true); // Catch signals.

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_name("Persistent Volume Framework (C++)");
  framework.set_role(flags.role);
  framework.set_checkpoint(true);
  framework.set_principal(flags.principal);

  if (flags.master.get() == "local") {
    // Configure master.
    os::setenv("MESOS_ROLES", flags.role);
    os::setenv("MESOS_AUTHENTICATE_FRAMEWORKS", "false");

    ACLs acls;
    ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_roles()->add_values(flags.role);

    os::setenv("MESOS_ACLS", stringify(JSON::protobuf(acls)));

    // Configure slave.
    os::setenv("MESOS_DEFAULT_ROLE", flags.role);
  }

  PersistentVolumeScheduler scheduler(
      framework,
      flags.num_shards,
      flags.tasks_per_shard);

  MesosSchedulerDriver* driver = new MesosSchedulerDriver(
      &scheduler,
      framework,
      flags.master.get());

  int status = driver->run() == DRIVER_STOPPED ? EXIT_SUCCESS : EXIT_FAILURE;

  driver->stop();
  delete driver;
  return status;
}
