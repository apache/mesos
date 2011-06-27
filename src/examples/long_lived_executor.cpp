#include <pthread.h>

#include <cstdlib>
#include <iostream>

#include <tr1/functional>

#include <mesos/executor.hpp>

using namespace mesos;
using namespace std;
using namespace std::tr1;


void run(ExecutorDriver* driver, const TaskDescription& task)
{
  sleep(100);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(task.task_id());
  status.set_state(TASK_FINISHED);

  driver->sendStatusUpdate(status);
}


void* start(void* arg)
{
  function<void(void)>* thunk = (function<void(void)>*) arg;
  (*thunk)();
  delete thunk;
  return NULL;
}


class MyExecutor : public Executor
{
public:
  virtual ~MyExecutor() {}

  virtual void init(ExecutorDriver*, const ExecutorArgs& args)
  {
    cout << "Initalized executor on " << args.hostname() << endl;
  }

  virtual void launchTask(ExecutorDriver* driver, const TaskDescription& task)
  {
    cout << "Starting task " << task.task_id().value() << endl;

    function<void(void)>* thunk =
      new function<void(void)>(bind(&run, driver, task));

    pthread_t pthread;
    if (pthread_create(&pthread, NULL, &start, thunk) != 0) {
      TaskStatus status;
      status.mutable_task_id()->MergeFrom(task.task_id());
      status.set_state(TASK_FAILED);

      driver->sendStatusUpdate(status);
    } else {
      pthread_detach(pthread);

      TaskStatus status;
      status.mutable_task_id()->MergeFrom(task.task_id());
      status.set_state(TASK_RUNNING);

      driver->sendStatusUpdate(status);
    }
  }

  virtual void killTask(ExecutorDriver* driver, const TaskID& taskId) {}

  virtual void frameworkMessage(ExecutorDriver* driver,
                                const string& data) {}

  virtual void shutdown(ExecutorDriver* driver) {}

  virtual void error(ExecutorDriver* driver, int code,
                     const std::string& message) {}
};


int main(int argc, char** argv)
{
  MyExecutor exec;
  MesosExecutorDriver driver(&exec);
  driver.run();
  return 0;
}
