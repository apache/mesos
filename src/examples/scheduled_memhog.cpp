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
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>

#include <boost/lexical_cast.hpp>
#include <boost/unordered_map.hpp>

#include <mesos/scheduler.hpp>

using namespace mesos;
using namespace std;

using boost::lexical_cast;


struct Task {
  double launchTime;
  double duration;
  bool launched;
  bool finished;
  int memToRequest;
  int memToHog;

  Task(double launchTime_, double duration_,
       int memToRequest_, int memToHog_)
    : launchTime(launchTime_), duration(duration_),
      memToRequest(memToRequest_), memToHog(memToHog_),
      launched(0), finished(0) {}
};


struct TaskComparator
{
  bool operator () (const Task& t1, const Task& t2) {
    return t1.launchTime < t2.launchTime;
  }
};


class MyScheduler : public Scheduler
{
public:
  MyScheduler(const string& scheduleFile, int threadsPerTask_)
    : threadsPerTask(threadsPerTask_),
      tasksLaunched(0), tasksFinished(0), successfulTasks(0)
  {
    ifstream in(scheduleFile.c_str());
    double launchTime;
    double duration;
    int memToRequest;
    int memToHog;
    while (in >> launchTime >> duration >> memToRequest >> memToHog) {
      tasks.push_back(Task(launchTime, duration, memToRequest, memToHog));
    }
    in.close();
    cout << "Loaded " << tasks.size() << " tasks" << endl;
    if (tasks.size() == 0) {
      cerr << "Schedule file contained no tasks!" << endl;
      exit(1);
    }
    // Sort tasks by start time
    TaskComparator comp;
    sort(tasks.begin(), tasks.end(), comp);
  }

  virtual ~MyScheduler() {}

  virtual void registered(SchedulerDriver*, const FrameworkID&)
  {
    cout << "Registered!" << endl;
    startTime = time(0);
  }

  virtual void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers)
  {
    time_t now = time(0);
    double curTime = difftime(now, startTime);
    vector<Offer>::const_iterator iterator = offers.begin();
    for (; iterator != offers.end(); ++iterator) {
      const Offer& offer = *iterator;

      // Lookup resources we care about.
      // TODO(benh): It would be nice to ultimately have some helper
      // functions for looking up resources.
      double cpus = 0;
      double mem = 0;

      for (int i = 0; i < offer.resources_size(); i++) {
        const Resource& resource = offer.resources(i);
        if (resource.name() == "cpus" &&
            resource.type() == Resource::SCALAR) {
          cpus = resource.scalar().value();
        } else if (resource.name() == "mem" &&
                   resource.type() == Resource::SCALAR) {
          mem = resource.scalar().value();
        }
      }

      // Launch tasks.
      vector<TaskDescription> toLaunch;
      if (tasksLaunched < tasks.size() &&
          cpus >= 1 &&
          curTime >= tasks[tasksLaunched].launchTime &&
          mem >= tasks[tasksLaunched].memToRequest)
      {
        int taskId = tasksLaunched++;

        cout << "Starting task " << taskId << " on "
             << offer.hostname() << endl;

        TaskDescription task;
        task.set_name("Task " + lexical_cast<string>(taskId));
        task.mutable_task_id()->set_value(lexical_cast<string>(taskId));
        task.mutable_slave_id()->MergeFrom(offer.slave_id());

        Resource* resource;

        resource = task.add_resources();
        resource->set_name("cpus");
        resource->set_type(Resource::SCALAR);
        resource->mutable_scalar()->set_value(1);

        resource = task.add_resources();
        resource->set_name("mem");
        resource->set_type(Resource::SCALAR);
        resource->mutable_scalar()->set_value(tasks[taskId].memToRequest);

        ostringstream data;
        data << tasks[taskId].memToHog << " " << tasks[taskId].duration
            << " " << threadsPerTask;
        task.set_data(data.str());

        toLaunch.push_back(task);
      }

      driver->launchTasks(offer.id(), toLaunch);
    }
  }

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) {}

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    int taskId = lexical_cast<int>(status.task_id().value());

    cout << "Task " << taskId << " is in state " << status.state() << endl;

    if (status.state() == TASK_FINISHED ||
        status.state() == TASK_FAILED ||
        status.state() == TASK_KILLED ||
        status.state() == TASK_LOST) {
      tasks[taskId].finished = true;
      tasksFinished++;

      if (status.state() == TASK_FINISHED) {
        successfulTasks++;
      }

      if (tasksFinished == tasks.size()) {
        driver->stop();
      }
    }
  }

  virtual void frameworkMessage(SchedulerDriver* driver,
				const SlaveID& slaveId,
				const ExecutorID& executorId,
                                const string& data) {}

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& sid) {}

  virtual void error(SchedulerDriver* driver, int code,
                     const std::string& message) {}


  vector<Task> tasks;
  int successfulTasks;

private:
  double taskLen;
  int threadsPerTask;
  time_t startTime;
  int tasksLaunched;
  int tasksFinished;
};


int main(int argc, char** argv)
{
  if (argc != 3) {
    cerr << "Usage: " << argv[0] << " <master> <schedule_file>";
    return -1;
  }
  char buf[4096];
  realpath(dirname(argv[0]), buf);
  string uri = string(buf) + "/memhog-executor";
  MyScheduler sched(argv[2], 1);
  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.set_uri(uri);
  MesosSchedulerDriver driver(&sched, "Memory hog", executor, argv[1]);
  driver.run();
  return (sched.successfulTasks == sched.tasks.size()) ? 0 : 1;
}
