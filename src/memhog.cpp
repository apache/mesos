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
  double taskLen;
  int threadsPerTask;
  int64_t memToRequest;
  int64_t memToHog;
  int tasksLaunched;
  int tasksFinished;
  int totalTasks;

public:
  MyScheduler(const string& executor_, int totalTasks_, double taskLen_,
      int threadsPerTask_, int64_t memToRequest_, int64_t memToHog_)
    : executor(executor_), totalTasks(totalTasks_), taskLen(taskLen_),
      threadsPerTask(threadsPerTask_),
      memToRequest(memToRequest_), memToHog(memToHog_),
      tasksLaunched(0), tasksFinished(0) {}

  virtual ~MyScheduler() {}

  virtual string getFrameworkName(SchedulerDriver*) {
    return "Memory hog";
  }

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*) {
    ostringstream arg;
    arg << memToHog << " " << taskLen << " " << threadsPerTask;
    cout << "Executor arg: " << arg.str() << endl;
    return ExecutorInfo(executor, arg.str());
  }

  virtual void registered(SchedulerDriver*, FrameworkID fid) {
    cout << "Registered!" << endl;
  }

  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID id,
                             const vector<SlaveOffer>& offers) {
    cout << "." << flush;
    vector<TaskDescription> tasks;
    foreach (const SlaveOffer &offer, offers) {
      // This is kind of ugly because operator[] isn't a const function
      int32_t cpus = lexical_cast<int32_t>(offer.params.find("cpus")->second);
      int64_t mem = lexical_cast<int64_t>(offer.params.find("mem")->second);
      if ((tasksLaunched < totalTasks) && (cpus >= 1 && mem >= memToRequest)) {
	stringstream ss;
	ss<<tasksLaunched++;
        TaskID tid = ss.str();

        cout << endl << "accepting it to start task " << tid << endl;
        map<string, string> taskParams;
        taskParams["cpus"] = "1";
        taskParams["mem"] = lexical_cast<string>(memToRequest);
        TaskDescription desc(tid, offer.slaveId, "task", taskParams, "");
        tasks.push_back(desc);
      }
    }
    map<string, string> params;
    params["timeout"] = "-1";
    d->replyToOffer(id, tasks, params);
  }

  virtual void statusUpdate(SchedulerDriver* d, const TaskStatus& status) {
    cout << endl << "Task " << status.taskId << " is in state " << status.state << endl;
    if (status.state == TASK_FINISHED)
      tasksFinished++;
    if (tasksFinished == totalTasks)
      d->stop();
  }
};


int main(int argc, char ** argv) {
  if (argc != 7) {
    cerr << "Usage: " << argv[0]
         << " <master> <tasks> <task_len> <threads_per_task>"
         << " <MB_to_request> <MB_per_task>" << endl;
    return -1;
  }
  char cwd[512];
  getcwd(cwd, sizeof(cwd));
  string executor = string(cwd) + "/memhog-executor";
  MyScheduler sched(executor,
                    lexical_cast<int>(argv[2]),
                    lexical_cast<double>(argv[3]),
                    lexical_cast<int>(argv[4]),
                    lexical_cast<int64_t>(argv[5]) * 1024 * 1024,
                    lexical_cast<int64_t>(argv[6]) * 1024 * 1024);
  NexusSchedulerDriver driver(&sched, argv[1]);
  driver.run();
  return 0;
}
