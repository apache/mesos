#include <mesos_sched.hpp>

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

#include "foreach.hpp"

using namespace std;
using namespace mesos;

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
  MyScheduler(const string& uri_, const string& scheduleFile,
              int threadsPerTask_)
    : uri(uri_), threadsPerTask(threadsPerTask_),
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

  virtual string getFrameworkName(SchedulerDriver*)
  {
    return "Memory hog";
  }

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*)
  {
    ExecutorInfo executor;
    executor.set_uri(uri);
    return executor;
  }

  virtual void registered(SchedulerDriver*, const FrameworkID&)
  {
    cout << "Registered!" << endl;
    startTime = time(0);
  }

  virtual void resourceOffer(SchedulerDriver* driver,
                             const OfferID& offerId,
                             const vector<SlaveOffer>& offers)
  {
    time_t now = time(0);
    double curTime = difftime(now, startTime);
    vector<TaskDescription> toLaunch;
    foreach (const SlaveOffer &offer, offers) {
      // Lookup resources we care about.
      // TODO(benh): It would be nice to ultimately have some helper
      // functions for looking up resources.
      int32_t cpus = 0;
      int32_t mem = 0;

      for (int i = 0; i < offer.params().param_size(); i++) {
        if (offer.params().param(i).key() == "cpus") {
          cpus = lexical_cast<int32_t>(offer.params().param(i).value());
        } else if (offer.params().param(i).key() == "mem") {
          mem = lexical_cast<int32_t>(offer.params().param(i).value());
        }
      }

      // Launch tasks.
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
        *task.mutable_slave_id() = offer.slave_id();

        Params* params = task.mutable_params();

        Param* param;
        param = params->add_param();
        param->set_key("cpus");
        param->set_value("1");

        param = params->add_param();
        param->set_key("mem");
        param->set_value(lexical_cast<string>(tasks[taskId].memToRequest));

        ostringstream data;
        data << tasks[taskId].memToHog << " " << tasks[taskId].duration
            << " " << threadsPerTask;
        task.set_data(data.str());

        toLaunch.push_back(task);
      }
    }

    driver->replyToOffer(offerId, toLaunch);
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
                                const FrameworkMessage& message) {}

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& sid) {}

  virtual void error(SchedulerDriver* driver, int code,
                     const std::string& message) {}


  vector<Task> tasks;
  int successfulTasks;

private:
  string uri;
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
  string executor = string(buf) + "/memhog-executor";
  MyScheduler sched(executor, argv[2], 1);
  MesosSchedulerDriver driver(&sched, argv[1]);
  driver.run();
  return (sched.successfulTasks == sched.tasks.size()) ? 0 : 1;
}
