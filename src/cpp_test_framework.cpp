#include <nexus_sched.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>

#include <boost/lexical_cast.hpp>

#include "foreach.hpp"

using namespace std;
using namespace nexus;
using boost::lexical_cast;

class MyScheduler : public Scheduler
{
  string executor;
  int tasksLaunched;
  int tasksFinished;
  int totalTasks;

public:
  MyScheduler(const string& exec)
    : executor(exec), tasksLaunched(0), tasksFinished(0), totalTasks(5) {}

  virtual ~MyScheduler() {}

  virtual string getFrameworkName(SchedulerDriver*) {
    return "C++ Test Framework";
  }

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    return ExecutorInfo(executor, "");
  }

  virtual void registered(SchedulerDriver*, FrameworkID fid) {
    cout << "Registered!" << endl;
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const std::vector<SlaveOffer>& offers) {
    cout << "." << flush;
    vector<TaskDescription> tasks;
    foreach (const SlaveOffer &offer, offers) {
      // This is kind of ugly because operator[] isn't a const function
      int32_t cpus = lexical_cast<int32_t>(offer.params.find("cpus")->second);
      int64_t mem = lexical_cast<int64_t>(offer.params.find("mem")->second);
      if ((tasksLaunched < totalTasks) && (cpus >= 1 && mem >= 33554432)) {
        TaskID tid = tasksLaunched++;
        cout << endl << "Starting task " << tid << endl;
        string name = "Task " + lexical_cast<string>(tid);
        map<string, string> taskParams;
        taskParams["cpus"] = "1";
        taskParams["mem"] = "33554432";
        TaskDescription desc(tid, offer.slaveId, name, taskParams, "");
        tasks.push_back(desc);
      }
    }
    string_map params;
    params["timeout"] = "-1";
    d->replyToOffer(id, tasks, params);
  }

  virtual void statusUpdate(SchedulerDriver* d, const TaskStatus& status) {
    cout << endl;
    cout << "Task " << status.taskId << " is in state " << status.state << endl;
    if (status.state == TASK_FINISHED)
      tasksFinished++;
    if (tasksFinished == totalTasks)
      d->stop();
  }
};


int main(int argc, char ** argv) {
  if (argc != 2) {
    cerr << "Usage: " << argv[0] << " <masterPid>" << endl;
    return -1;
  }
  char cwd[512];
  getcwd(cwd, sizeof(cwd));
  string executor = string(cwd) + "/cpp-test-executor";
  MyScheduler sched(executor);
  NexusSchedulerDriver driver(&sched, argv[1]);
  driver.run();
  return 0;
}
