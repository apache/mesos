#include <mesos_sched.hpp>

#include <libgen.h>

#include <cstdlib>
#include <iostream>
#include <sstream>

#include <boost/lexical_cast.hpp>

#include "foreach.hpp"

using namespace std;
using namespace mesos;

using boost::lexical_cast;


class MyScheduler : public Scheduler
{
public:
  MyScheduler(const string& uri_, int totalTasks_, double taskLen_,
      int threadsPerTask_, int64_t memToRequest_, int64_t memToHog_)
    : uri(uri_), totalTasks(totalTasks_), taskLen(taskLen_),
      threadsPerTask(threadsPerTask_),
      memToRequest(memToRequest_), memToHog(memToHog_),
      tasksLaunched(0), tasksFinished(0) {}

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
  }

  virtual void resourceOffer(SchedulerDriver* driver,
                             const OfferID& offerId,
                             const vector<SlaveOffer>& offers)
  {
    vector<TaskDescription> tasks;
    foreach (const SlaveOffer& offer, offers) {
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

      // Launch task.
      if ((tasksLaunched < totalTasks) && (cpus >= 1 && mem >= memToRequest)) {
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
        param->set_value(lexical_cast<string>(memToRequest));

        ostringstream data;
        data << memToHog << " " << taskLen << " " << threadsPerTask;
        task.set_data(data.str());

        tasks.push_back(task);
      }
    }

    map<string, string> params;
    params["timeout"] = "-1";
    driver->replyToOffer(offerId, tasks, params);
  }

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) {}

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    int taskId = lexical_cast<int>(status.task_id().value());

    cout << "Task " << taskId << " is in state " << status.state() << endl;

    if (status.state() == TASK_LOST)
      cout << "Task " << taskId
           << " lost. Not doing anything about it." << endl;

    if (status.state() == TASK_FINISHED)
      tasksFinished++;

    if (tasksFinished == totalTasks)
      driver->stop();
  }

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const FrameworkMessage& message) {}

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& sid) {}

  virtual void error(SchedulerDriver* driver, int code,
                     const std::string& message) {}

private:
  string uri;
  double taskLen;
  int threadsPerTask;
  int memToRequest;
  int memToHog;
  int tasksLaunched;
  int tasksFinished;
  int totalTasks;
};


int main(int argc, char** argv)
{
  if (argc != 7) {
    cerr << "Usage: " << argv[0]
         << " <master> <tasks> <task_len> <threads_per_task>"
         << " <MB_to_request> <MB_per_task>" << endl;
    return -1;
  }
  // Find this executable's directory to locate executor
  char buf[4096];
  realpath(dirname(argv[0]), buf);
  string executor = string(buf) + "/memhog-executor";
  MyScheduler sched(executor,
                    lexical_cast<int>(argv[2]),
                    lexical_cast<double>(argv[3]),
                    lexical_cast<int>(argv[4]),
                    lexical_cast<int64_t>(argv[5]),
                    lexical_cast<int64_t>(argv[6]));
  MesosSchedulerDriver driver(&sched, argv[1]);
  driver.run();
  return 0;
}
