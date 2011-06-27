#include <cstdlib>
#include <iostream>

#include <mesos/executor.hpp>

using namespace mesos;
using namespace std;


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

    TaskStatus status;
    status.mutable_task_id()->MergeFrom(task.task_id());
    status.set_state(TASK_RUNNING);

    driver->sendStatusUpdate(status);

    sleep(1);

    cout << "Finishing task " << task.task_id().value() << endl;

    status.mutable_task_id()->MergeFrom(task.task_id());
    status.set_state(TASK_FINISHED);

    driver->sendStatusUpdate(status);
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
