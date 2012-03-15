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

#include <mesos/scheduler.hpp>

using namespace mesos;

using boost::lexical_cast;

using std::cout;
using std::cerr;
using std::endl;
using std::flush;
using std::string;
using std::vector;

const int32_t CPUS_PER_TASK = 1;
const int32_t MEM_PER_TASK = 32;

class NoExecutorScheduler : public Scheduler
{
public:
  NoExecutorScheduler()
    : tasksLaunched(0), tasksFinished(0), totalTasks(5) {}

  virtual ~NoExecutorScheduler() {}

  virtual void registered(SchedulerDriver*, const FrameworkID&)
  {
    cout << "Registered!" << endl;
  }

  virtual void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers)
  {
    cout << "." << flush;
    for (int i = 0; i < offers.size(); i++) {
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
      vector<TaskDescription> tasks;
      while (tasksLaunched < totalTasks &&
             cpus >= CPUS_PER_TASK &&
             mem >= MEM_PER_TASK) {
        int taskId = tasksLaunched++;

        cout << "Starting task " << taskId << " on "
             << offer.hostname() << endl;

        TaskDescription task;
        task.set_name("Task " + lexical_cast<string>(taskId));
        task.mutable_task_id()->set_value(lexical_cast<string>(taskId));
        task.mutable_slave_id()->MergeFrom(offer.slave_id());
        task.mutable_command()->set_value("sleep 1");

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

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) {}

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    int taskId = lexical_cast<int>(status.task_id().value());

    cout << "Task " << taskId << " is in state " << status.state() << endl;

    if (status.state() == TASK_FINISHED)
      tasksFinished++;

    if (tasksFinished == totalTasks)
      driver->stop();
  }

  virtual void frameworkMessage(SchedulerDriver* driver,
				const SlaveID& slaveId,
				const ExecutorID& executorId,
                                const string& data) {}

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& sid) {}
  virtual void error(SchedulerDriver* driver, int code, const string& message) {}

private:
  int tasksLaunched;
  int tasksFinished;
  int totalTasks;
};


int main(int argc, char** argv)
{
  if (argc != 2) {
    cerr << "Usage: " << argv[0] << " <masterPid>" << endl;
    return -1;
  }

  NoExecutorScheduler scheduler;

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value("exit 1");

  MesosSchedulerDriver driver(
      &scheduler, "C++ Test Framework", executor, argv[1]);

  return driver.run() == DRIVER_STOPPED ? 0 : 1;
}
