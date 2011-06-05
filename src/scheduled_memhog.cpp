#include <nexus_sched.hpp>

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

#include <glog/logging.h>

#include "foreach.hpp"

using namespace std;
using namespace nexus;

using boost::lexical_cast;


struct Task {
  double launchTime;
  double duration;
  bool launched;
  bool finished;
  int64_t memToRequest;
  int64_t memToHog;

  Task(double launchTime_, double duration_,
       int64_t memToRequest_, int64_t memToHog_)
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
  string executor;
  double taskLen;
  int threadsPerTask;
  string scheduleFile;
  vector<Task> tasks;
  time_t startTime;
  int tasksLaunched;
  int tasksFinished;
  int successfulTasks;

  MyScheduler(const string& executor_, const string& scheduleFile_,
              int threadsPerTask_)
    : executor(executor_), scheduleFile(scheduleFile_),
      threadsPerTask(threadsPerTask_),
      tasksLaunched(0), tasksFinished(0), successfulTasks(0)
  {
    ifstream in(scheduleFile.c_str());
    double launchTime;
    double duration;
    int64_t memToRequest;
    int64_t memToHog;
    while (in >> launchTime >> duration >> memToRequest >> memToHog) {
      memToRequest *= (1024 * 1024);
      memToHog *= (1024 * 1024);
      tasks.push_back(Task(launchTime, duration, memToRequest, memToHog));
    }
    in.close();
    LOG(INFO) << "Loaded " << tasks.size() << " tasks";
    if (tasks.size() == 0) {
      cerr << "Schedule file contained no tasks!" << endl;
      exit(1);
    }
    // Sort tasks by start time
    TaskComparator comp;
    sort(tasks.begin(), tasks.end(), comp);
  }

  virtual ~MyScheduler() {}

  virtual string getFrameworkName(SchedulerDriver*) {
    return "Memory hog";
  }

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo(executor, "");
  }

  virtual void registered(SchedulerDriver*, FrameworkID fid) {
    LOG(INFO) << "Registered!";
    startTime = time(0);
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    time_t now = time(0);
    double curTime = difftime(now, startTime);
    vector<TaskDescription> toLaunch;
    foreach (const SlaveOffer &offer, offers) {
      // This is kind of ugly because operator[] isn't a const function
      int32_t cpus = lexical_cast<int32_t>(offer.params.find("cpus")->second);
      int64_t mem = lexical_cast<int64_t>(offer.params.find("mem")->second);
      if (tasksLaunched < tasks.size() &&
          cpus >= 1 &&
          curTime >= tasks[tasksLaunched].launchTime &&
          mem >= tasks[tasksLaunched].memToRequest)
      {
        TaskID tid = tasksLaunched++;
        LOG(INFO) << "Launcing task " << tid << " on " << offer.host;
        map<string, string> params;
        params["cpus"] = "1";
        params["mem"] = lexical_cast<string>(tasks[tid].memToRequest);
        ostringstream arg;
        arg << tasks[tid].memToHog << " " << tasks[tid].duration
            << " " << threadsPerTask;
        TaskDescription desc(tid, offer.slaveId, "task", params, arg.str());
        toLaunch.push_back(desc);
      }
    }
    map<string, string> params;
    d->replyToOffer(id, toLaunch, params);
  }

  virtual void statusUpdate(SchedulerDriver* d, const TaskStatus& status) {
    LOG(INFO) << "Task " << status.taskId << " is in state " << status.state;
    if (status.state == TASK_FINISHED || status.state == TASK_FAILED ||
        status.state == TASK_KILLED || status.state == TASK_LOST) {
      tasks[status.taskId].finished = true;
      tasksFinished++;
      if (status.state == TASK_FINISHED) {
        successfulTasks++;
      }
      if (tasksFinished == tasks.size()) {
        d->stop();
      }
    }
  }
};


int main(int argc, char ** argv) {
  if (argc != 3) {
    cerr << "Usage: " << argv[0] << " <master> <schedule_file>";
    return -1;
  }
  char buf[4096];
  realpath(dirname(argv[0]), buf);
  string executor = string(buf) + "/memhog-executor";
  MyScheduler sched(executor, argv[2], 1);
  NexusSchedulerDriver driver(&sched, argv[1]);
  driver.run();
  return (sched.successfulTasks == sched.tasks.size()) ? 0 : 1;
}
