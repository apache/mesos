#include <nexus_sched.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>

#include <boost/lexical_cast.hpp>
#include <boost/unordered_map.hpp>

#include "foreach.hpp"

using namespace std;
using namespace nexus;

using boost::lexical_cast;


struct Task {
  double launchTime;
  double duration;
  bool launched;
  bool finished;

  Task(double launchTime_, double duration_)
    : launchTime(launchTime_), duration(duration_),
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
  string executor;
  double taskLen;
  int threadsPerTask;
  int64_t memToRequest;
  int64_t memToHog;
  string scheduleFile;
  vector<Task> tasks;
  time_t startTime;
  int tasksLaunched;
  int tasksFinished;

public:
  MyScheduler(const string& executor_, const string& scheduleFile_,
      int threadsPerTask_, int64_t memToRequest_, int64_t memToHog_)
    : executor(executor_), scheduleFile(scheduleFile_),
      threadsPerTask(threadsPerTask_),
      memToRequest(memToRequest_), memToHog(memToHog_),
      tasksLaunched(0), tasksFinished(0)
  {
    ifstream in(scheduleFile.c_str());
    double launchTime;
    double duration;
    while (in >> launchTime >> duration) {
      tasks.push_back(Task(launchTime, duration));
    }
    in.close();
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
    cout << "Registered!" << endl;
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
      if ((tasksLaunched < tasks.size()) && (cpus >= 1 && mem >= memToRequest) &&
          curTime >= tasks[tasksLaunched].launchTime) {
        TaskID tid = tasksLaunched++;
        cout << "Launcing task " << tid << " on " << offer.host << endl;
        map<string, string> params;
        params["cpus"] = "1";
        params["mem"] = lexical_cast<string>(memToRequest);
        ostringstream arg;
        arg << memToHog << " " << tasks[tid].duration << " " << threadsPerTask;
        TaskDescription desc(tid, offer.slaveId, "task", params, arg.str());
        toLaunch.push_back(desc);
      }
    }
    map<string, string> params;
    d->replyToOffer(id, toLaunch, params);
  }

  virtual void statusUpdate(SchedulerDriver* d, const TaskStatus& status) {
    cout << "Task " << status.taskId << " is in state " << status.state << endl;
    if (status.state == TASK_FINISHED || status.state == TASK_FAILED ||
        status.state == TASK_KILLED || status.state == TASK_LOST) {
      tasks[status.taskId].finished = true;
      tasksFinished++;
      if (tasksFinished == tasks.size()) {
        d->stop();
      }
    }
  }
};


int main(int argc, char ** argv) {
  if (argc != 6) {
    cerr << "Usage: " << argv[0]
         << " <master> <schedule_file> <threads_per_task>"
         << " <MB_to_request> <MB_per_task>" << endl;
    return -1;
  }
  char cwd[512];
  getcwd(cwd, sizeof(cwd));
  string executor = string(cwd) + "/memhog-executor";
  MyScheduler sched(executor,
                    argv[2],
                    lexical_cast<int>(argv[3]),
                    lexical_cast<int64_t>(argv[4]) * 1024 * 1024,
                    lexical_cast<int64_t>(argv[5]) * 1024 * 1024);
  NexusSchedulerDriver driver(&sched, argv[1]);
  driver.run();
  return 0;
}
