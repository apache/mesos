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
#include <string>

#include <boost/lexical_cast.hpp>

#include <mesos/scheduler.hpp>

#include <mesos/authorizer/acls.hpp>

#include <stout/exit.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>

#include "examples/flags.hpp"

#include "logging/logging.hpp"

using namespace mesos;

using boost::lexical_cast;

using std::string;
using std::vector;

const int32_t CPUS_PER_TASK = 1;
const int32_t MEM_PER_TASK = 32;

constexpr char FRAMEWORK_NAME[] = "Docker No Executor Framework (C++)";


class DockerNoExecutorScheduler : public Scheduler
{
public:
  DockerNoExecutorScheduler()
    : tasksLaunched(0), tasksFinished(0), totalTasks(5) {}

  ~DockerNoExecutorScheduler() override {}

  void registered(SchedulerDriver*,
                          const FrameworkID&,
                          const MasterInfo&) override
  {
    LOG(INFO) << "Registered!";
  }

  void reregistered(SchedulerDriver*, const MasterInfo& masterInfo) override {}

  void disconnected(SchedulerDriver* driver) override {}

  void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers) override
  {
    LOG(INFO) << ".";

    for (size_t i = 0; i < offers.size(); i++) {
      const Offer& offer = offers[i];

      // Lookup resources we care about.
      // TODO(benh): It would be nice to ultimately have some helper
      // functions for looking up resources.
      double cpus = 0;
      double mem = 0;

      for (int i = 0; i < offer.resources_size(); i++) {
        const Resource& resource = offer.resources(i);
        if (resource.name() == "cpus" &&
            resource.type() == Value::SCALAR) {
          cpus = resource.scalar().value();
        } else if (resource.name() == "mem" &&
                   resource.type() == Value::SCALAR) {
          mem = resource.scalar().value();
        }
      }

      // Launch tasks.
      vector<TaskInfo> tasks;
      while (tasksLaunched < totalTasks &&
             cpus >= CPUS_PER_TASK &&
             mem >= MEM_PER_TASK) {
        int taskId = tasksLaunched++;

        LOG(INFO) << "Starting task " << taskId << " on " << offer.hostname();

        TaskInfo task;
        task.set_name("Task " + lexical_cast<string>(taskId));
        task.mutable_task_id()->set_value(lexical_cast<string>(taskId));
        task.mutable_slave_id()->MergeFrom(offer.slave_id());
        task.mutable_command()->set_value("echo hello");

        // Use Docker to run the task.
        ContainerInfo containerInfo;
        containerInfo.set_type(ContainerInfo::DOCKER);

        ContainerInfo::DockerInfo dockerInfo;
        dockerInfo.set_image("alpine");

        containerInfo.mutable_docker()->CopyFrom(dockerInfo);
        task.mutable_container()->CopyFrom(containerInfo);

        Resource* resource;

        resource = task.add_resources();
        resource->set_name("cpus");
        resource->set_type(Value::SCALAR);
        resource->mutable_scalar()->set_value(CPUS_PER_TASK);

        resource = task.add_resources();
        resource->set_name("mem");
        resource->set_type(Value::SCALAR);
        resource->mutable_scalar()->set_value(MEM_PER_TASK);

        tasks.push_back(task);

        cpus -= CPUS_PER_TASK;
        mem -= MEM_PER_TASK;
      }

      driver->launchTasks(offer.id(), tasks);
    }
  }

  void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) override {}

  void statusUpdate(SchedulerDriver* driver, const TaskStatus& status) override
  {
    int taskId = lexical_cast<int>(status.task_id().value());

    LOG(INFO) << "Task " << taskId << " is in state " << status.state();

    if (status.state() == TASK_FINISHED) {
      tasksFinished++;
    }

    if (tasksFinished == totalTasks) {
      driver->stop();
    }
  }

  void frameworkMessage(SchedulerDriver* driver,
                                const ExecutorID& executorId,
                                const SlaveID& slaveId,
                                const string& data) override {}

  void slaveLost(SchedulerDriver* driver, const SlaveID& slaveId) override {}

  void executorLost(SchedulerDriver* driver,
                            const ExecutorID& executorId,
                            const SlaveID& slaveId,
                            int status) override {}

  void error(SchedulerDriver* driver, const string& message) override {}

private:
  int tasksLaunched;
  int tasksFinished;
  int totalTasks;
};


class Flags : public virtual mesos::internal::examples::Flags {};


int main(int argc, char** argv)
{
  Flags flags;
  Try<flags::Warnings> load = flags.load("MESOS_EXAMPLE_", argc, argv);

  if (flags.help) {
    std::cout << flags.usage() << std::endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    std::cerr << flags.usage(load.error()) << std::endl;
    return EXIT_FAILURE;
  }

  internal::logging::initialize(argv[0], false);

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_principal(flags.principal);
  framework.set_name(FRAMEWORK_NAME);
  framework.set_checkpoint(flags.checkpoint);
  framework.add_roles(flags.role);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  if (flags.master == "local") {
    // Configure master.
    os::setenv("MESOS_ROLES", flags.role);
    os::setenv("MESOS_AUTHENTICATE_FRAMEWORKS", stringify(flags.authenticate));

    ACLs acls;
    ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_roles()->add_values("*");
    os::setenv("MESOS_ACLS", stringify(JSON::protobuf(acls)));
  }

  DockerNoExecutorScheduler scheduler;

  MesosSchedulerDriver* driver;

  if (flags.authenticate) {
    LOG(INFO) << "Enabling authentication for the framework";

    Credential credential;
    credential.set_principal(flags.principal);
    if (flags.secret.isSome()) {
      credential.set_secret(flags.secret.get());
    }

    driver = new MesosSchedulerDriver(
        &scheduler,
        framework,
        argv[1],
        credential);
  } else {
    driver = new MesosSchedulerDriver(
        &scheduler,
        framework,
        argv[1]);
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
