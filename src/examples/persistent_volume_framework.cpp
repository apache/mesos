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
#include <stout/hashset.hpp>
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
  Resources allocation =
    Resources::parse("cpus:0.1;mem:32;disk:16", role).get();

  allocation.allocate(role);

  return allocation;
}


static Resource SHARD_PERSISTENT_VOLUME(
    const string& role,
    const string& persistenceId,
    const string& containerPath,
    const string& principal,
    bool isShared)
{
  Volume volume;
  volume.set_container_path(containerPath);
  volume.set_mode(Volume::RW);

  Resource::DiskInfo info;
  info.mutable_persistence()->set_id(persistenceId);
  info.mutable_persistence()->set_principal(principal);
  info.mutable_volume()->CopyFrom(volume);

  Resource resource = Resources::parse("disk", "8", role).get();
  resource.mutable_disk()->CopyFrom(info);
  resource.mutable_allocation_info()->set_role(role);

  if (isShared) {
    resource.mutable_shared();
  }

  return resource;
}


static Offer::Operation CREATE(const Resources& volumes)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::CREATE);
  operation.mutable_create()->mutable_volumes()->CopyFrom(volumes);
  return operation;
}


static Offer::Operation DESTROY(const Resources& volumes)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::DESTROY);
  operation.mutable_destroy()->mutable_volumes()->CopyFrom(volumes);
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


// The framework launches a task on each registered agent using a
// (possibly shared) persistent volume. In the case of regular
// persistent volumes, the next task is started once the previous
// one terminates; and in the case of shared persistent volumes,
// tasks can use the same shared volume simultaneously. The
// framework terminates once the number of tasks launched on each
// agent reaches a limit.
class PersistentVolumeScheduler : public Scheduler
{
public:
  PersistentVolumeScheduler(
      const FrameworkInfo& _frameworkInfo,
      size_t numShards,
      size_t numSharedShards,
      size_t tasksPerShard)
    : frameworkInfo(_frameworkInfo)
  {
    // Initialize the shards using regular persistent volume.
    for (size_t i = 0; i < numShards; i++) {
      shards.push_back(Shard(
          "shard-" + stringify(i),
          frameworkInfo.role(),
          tasksPerShard,
          false));
    }

    // Initialize the shards using shared persistent volume.
    for (size_t i = 0; i < numSharedShards; i++) {
      shards.push_back(Shard(
          "shared-shard-" + stringify(i),
          frameworkInfo.role(),
          tasksPerShard,
          true));
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
            CHECK_EQ(0u, shard.launched);

            if (offered.contains(shard.resources)) {
              Resource volume = SHARD_PERSISTENT_VOLUME(
                  frameworkInfo.role(),
                  UUID::random().toString(),
                  "volume",
                  frameworkInfo.principal(),
                  shard.volume.isShared);

              Try<Resources> resources = shard.resources.apply(CREATE(volume));
              CHECK_SOME(resources);

              TaskInfo task;
              task.set_name(shard.name);
              task.mutable_task_id()->set_value(UUID::random().toString());
              task.mutable_slave_id()->CopyFrom(offer.slave_id());
              task.mutable_resources()->CopyFrom(resources.get());

              // TODO(anindya_sinha): Add a flag to allow specifying a
              // custom write command for the consumer task.
              task.mutable_command()->set_value(
                  "echo hello > volume/persisted");

              // Update the shard.
              shard.state = Shard::STAGING;
              shard.taskIds.insert(task.task_id());
              shard.resources = resources.get();
              shard.volume.resource = volume;
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

          case Shard::STAGING:
            CHECK_LE(shard.launched, shard.tasks);

            if (shard.launched == shard.tasks) {
              LOG(INFO) << "All tasks launched, but one or more yet to run";
              break;
            }

            if (offered.contains(shard.resources)) {
              // Set mode to RO for persistent volume resource since we are
              // just reading from the persistent volume.
              CHECK_SOME(shard.volume.resource);

              Resource volume = shard.volume.resource.get();
              Resources taskResources = shard.resources - volume;
              volume.mutable_disk()->mutable_volume()->set_mode(Volume::RO);
              taskResources += volume;

              TaskInfo task;
              task.set_name(shard.name);
              task.mutable_task_id()->set_value(UUID::random().toString());
              task.mutable_slave_id()->CopyFrom(offer.slave_id());
              task.mutable_resources()->CopyFrom(taskResources);

              // The read task tries to access the content written in the
              // persistent volume for up to 15 seconds. This is to handle
              // the scenario where the writer task may not have done the
              // write to the volume even though it was launched earlier.
              // TODO(anindya_sinha): Add a flag to allow specifying a
              // custom read command for the consumer task.
              task.mutable_command()->set_value(R"~(
                  COUNTER=0
                  while [ $COUNTER -lt 15 ]; do
                    cat volume/persisted
                    if [ $? -eq 0 ]; then
                      exit 0
                    fi
                    ((COUNTER++))
                    sleep 1
                  done
                  exit 1
                  )~");

              // Update the shard.
              shard.taskIds.insert(task.task_id());
              shard.launched++;

              // Launch the next instance of this task with the already
              // created volume.
              operations.push_back(LAUNCH({task}));
            }

            break;

          case Shard::TERMINATING:
            CHECK_EQ(shard.terminated, shard.launched);
            CHECK_SOME(shard.volume.resource);

            // Send (or resend) DESTROY of the volume here.
            if (offered.contains(shard.volume.resource.get())) {
              operations.push_back(DESTROY(shard.volume.resource.get()));
            } else {
              shard.state = Shard::DONE;
            }

            break;

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
      if (shard.taskIds.contains(status.task_id())) {
        switch (status.state()) {
          case TASK_RUNNING:
            CHECK(shard.launched <= shard.tasks);
            if (shard.launched == shard.tasks &&
                shard.state == Shard::STAGING) {
              shard.state = Shard::RUNNING;
            }
            break;
          case TASK_FINISHED:
            ++shard.terminated;

            CHECK(shard.terminated <= shard.tasks);
            if (shard.terminated == shard.tasks &&
                shard.state == Shard::RUNNING) {
              shard.state = Shard::TERMINATING;
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
    // States that the state machine for each shard goes through.
    //
    // The state machine per shard runs as follows:
    // 1. The shard is STAGING when it launches a writer task that writes
    //    to the persistent volume.
    // 2. The shard launches one or more reader tasks that read the same file
    //    from the persistent volume to confirm the write operation. Once all
    //    tasks are launched, the shard is RUNNING.
    // 3. When all tasks finish, the shard starts TERMINATING by destroying
    //    the persistent volume.
    // 4. Once the persistent volume is successfully destroyed, the shard
    //    is DONE and terminates.
    //
    // In the case of regular persistent volumes, the tasks run in sequence;
    // whereas in the case of shared persistent volumes, the tasks run in
    // parallel.
    enum State
    {
      INIT = 0,    // The shard is in initialized state.
      STAGING,     // The shard is awaiting offers to launch more tasks.
      RUNNING,     // The shard is running (i.e., all tasks have
                   // successfully launched).
      TERMINATING, // All tasks are finished and needs cleanup.
      DONE,        // The shard has finished cleaning up.
    };

    // The persistent volume associated with this shard.
    struct Volume
    {
      explicit Volume(bool _isShared)
        : isShared(_isShared) {}

      // `Resource` object for this volume.
      Option<Resource> resource;

      // Flag to indicate if this is a regular or shared persistent volume.
      bool isShared;
    };

    Shard(
        const string& _name,
        const string& role,
        size_t _tasks,
        bool isShared)
      : name(_name),
        state(INIT),
        volume(isShared),
        resources(SHARD_INITIAL_RESOURCES(role)),
        launched(0),
        terminated(0),
        tasks(_tasks) {}

    string name;
    State state;             // The current state of this shard.
    hashset<TaskID> taskIds; // The IDs of the tasks running on this shard.
    Volume volume;           // The persistent volume associated with the shard.
    Resources resources;     // Resources required to launch the shard.
    size_t launched;         // How many tasks this shard has launched.
    size_t terminated;       // How many tasks this shard has terminated.
    size_t tasks;            // How many tasks this shard should launch.
  };

  FrameworkInfo frameworkInfo;
  vector<Shard> shards;
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

    add(&Flags::role,
        "role",
        "Role to use when registering",
        "test");

    add(&Flags::principal,
        "principal",
        "The principal used to identify this framework",
        "test");

    add(&Flags::num_shards,
        "num_shards",
        "The number of shards the framework will run using regular volume.",
        2);

    add(&Flags::num_shared_shards,
        "num_shared_shards",
        "The number of shards the framework will run using shared volume.",
        2);

    add(&Flags::tasks_per_shard,
        "tasks_per_shard",
        "The number of tasks should be launched per shard.",
        2);
  }

  Option<string> master;
  string role;
  string principal;
  size_t num_shards;
  size_t num_shared_shards;
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
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  if (flags.master.get() == "local") {
    // Configure master.
    os::setenv("MESOS_ROLES", flags.role);
    os::setenv("MESOS_AUTHENTICATE_FRAMEWORKS", "false");

    ACLs acls;
    ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_roles()->add_values(flags.role);

    os::setenv("MESOS_ACLS", stringify(JSON::protobuf(acls)));

    // Configure agent.
    os::setenv("MESOS_DEFAULT_ROLE", flags.role);
  }

  PersistentVolumeScheduler scheduler(
      framework,
      flags.num_shards,
      flags.num_shared_shards,
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
