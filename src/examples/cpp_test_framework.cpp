#include <libgen.h>

#include <cstdlib>
#include <iostream>
#include <sstream>

#include <boost/lexical_cast.hpp>

#include <mesos/scheduler.hpp>

using namespace mesos;
using namespace std;

using boost::lexical_cast;


const int32_t CPUS_PER_TASK = 1;
const int32_t MEM_PER_TASK = 32;


class MyScheduler : public Scheduler
{
public:
  MyScheduler(const string& _uri)
    : uri(_uri), tasksLaunched(0), tasksFinished(0), totalTasks(5) {}

  virtual ~MyScheduler() {}

  virtual string getFrameworkName(SchedulerDriver*)
  {
    return "C++ Test Framework";
  }

  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*)
  {
    ExecutorInfo executor;
    executor.mutable_executor_id()->set_value("default");
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
    cout << "." << flush;
    vector<TaskDescription> tasks;
    vector<SlaveOffer>::const_iterator iterator = offers.begin();
    for (; iterator != offers.end(); ++iterator) {
      const SlaveOffer& offer = *iterator;
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

        Resource* resource;

        resource = task.add_resources();
        resource->set_name("cpus");
        resource->set_type(Resource::SCALAR);
        resource->mutable_scalar()->set_value(CPUS_PER_TASK);

        resource = task.add_resources();
        resource->set_name("mem");
        resource->set_type(Resource::SCALAR);
        resource->mutable_scalar()->set_value(MEM_PER_TASK);

        tasks.push_back(task);

        cpus -= CPUS_PER_TASK;
        mem -= MEM_PER_TASK;
      }
    }

    driver->replyToOffer(offerId, tasks);
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

  virtual void error(SchedulerDriver* driver, int code,
                     const string& message) {}

private:
  string uri;
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
  // Find this executable's directory to locate executor
  char buf[4096];
  realpath(dirname(argv[0]), buf);
  string executor = string(buf) + "/cpp-test-executor";
  // Run a Mesos scheduler
  MyScheduler sched(executor);
  MesosSchedulerDriver driver(&sched, argv[1]);
  driver.run();
  return 0;
}
