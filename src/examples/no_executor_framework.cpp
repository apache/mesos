/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <libgen.h>

#include <iostream>
#include <string>

#include <boost/lexical_cast.hpp>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <stout/check.hpp>
#include <stout/exit.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include "common/type_utils.hpp"

using namespace mesos;

using boost::lexical_cast;

using std::cout;
using std::cerr;
using std::endl;
using std::flush;
using std::string;
using std::vector;

using mesos::Resources;

const int32_t CPUS_PER_TASK = 1;
const int32_t MEM_PER_TASK = 128;

class NoExecutorScheduler : public Scheduler
{
public:
  NoExecutorScheduler()
    : tasksLaunched(0), tasksFinished(0), totalTasks(5) {}

  virtual ~NoExecutorScheduler() {}

  virtual void registered(SchedulerDriver*,
                          const FrameworkID&,
                          const MasterInfo&)
  {
    cout << "Registered!" << endl;
  }

  virtual void reregistered(SchedulerDriver*, const MasterInfo& masterInfo) {}

  virtual void disconnected(SchedulerDriver* driver) {}

  virtual void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers)
  {
    foreach (const Offer& offer, offers) {
      cout << "Received offer " << offer.id() << " with " << offer.resources()
           << endl;

      static const Resources TASK_RESOURCES = Resources::parse(
          "cpus:" + stringify(CPUS_PER_TASK) +
          ";mem:" + stringify(MEM_PER_TASK)).get();

      Resources remaining = offer.resources();

      // Launch tasks.
      vector<TaskInfo> tasks;
      while (tasksLaunched < totalTasks &&
             remaining.flatten().contains(TASK_RESOURCES)) {
        int taskId = tasksLaunched++;

        cout << "Launching task " << taskId << " using offer "
             << offer.id() << endl;

        TaskInfo task;
        task.set_name("Task " + lexical_cast<string>(taskId));
        task.mutable_task_id()->set_value(lexical_cast<string>(taskId));
        task.mutable_slave_id()->MergeFrom(offer.slave_id());
        task.mutable_command()->set_value("echo hello");

        Option<Resources> resources = remaining.find(TASK_RESOURCES);
        CHECK_SOME(resources);
        task.mutable_resources()->MergeFrom(resources.get());
        remaining -= resources.get();

        tasks.push_back(task);
      }

      driver->launchTasks(offer.id(), tasks);
    }
  }

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) {}

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    int taskId = lexical_cast<int>(status.task_id().value());

    cout << "Task " << taskId << " is in state " << status.state() << endl;

    if (status.state() == TASK_FINISHED)
      tasksFinished++;

    if (status.state() == TASK_LOST ||
        status.state() == TASK_KILLED ||
        status.state() == TASK_FAILED) {
      cout << "Aborting because task " << taskId
           << " is in unexpected state " << status.state()
           << " with reason " << status.reason()
           << " from source " << status.source()
           << " with message '" << status.message() << "'"
           << endl;
      driver->abort();
    }

    if (tasksFinished == totalTasks)
      driver->stop();
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

  virtual void error(SchedulerDriver* driver, const string& message) {}

private:
  int tasksLaunched;
  int tasksFinished;
  int totalTasks;
};


int main(int argc, char** argv)
{
  if (argc != 2) {
    cerr << "Usage: " << argv[0] << " <master>" << endl;
    return -1;
  }

  NoExecutorScheduler scheduler;

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_name("No Executor Framework (C++)");

  if (os::hasenv("MESOS_CHECKPOINT")) {
    framework.set_checkpoint(
        numify<bool>(os::getenv("MESOS_CHECKPOINT")).get());
  }

  MesosSchedulerDriver* driver;
  if (os::hasenv("MESOS_AUTHENTICATE")) {
    cout << "Enabling authentication for the framework" << endl;

    if (!os::hasenv("DEFAULT_PRINCIPAL")) {
      EXIT(1) << "Expecting authentication principal in the environment";
    }

    if (!os::hasenv("DEFAULT_SECRET")) {
      EXIT(1) << "Expecting authentication secret in the environment";
    }

    Credential credential;
    credential.set_principal(getenv("DEFAULT_PRINCIPAL"));
    credential.set_secret(getenv("DEFAULT_SECRET"));

    framework.set_principal(getenv("DEFAULT_PRINCIPAL"));

    driver = new MesosSchedulerDriver(
        &scheduler, framework, argv[1], credential);
  } else {
    framework.set_principal("no-executor-framework-cpp");

    driver = new MesosSchedulerDriver(
        &scheduler, framework, argv[1]);
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
