#include <mesos_exec.hpp>

#include <cstdlib>
#include <iostream>


using namespace std;
using namespace mesos;

class MyExecutor : public Executor
{
public:
  virtual ~MyExecutor() {}

  virtual void init(ExecutorDriver*, const ExecutorArgs& args) {
    cout << "Initalized executor on " << args.host << endl;
  }

  virtual void launchTask(ExecutorDriver* d, const TaskDescription& task) {
    cout << "Starting task " << task.taskId << endl;
    sleep(1);
    cout << "Finishing task " << task.taskId << endl;
    TaskStatus status(task.taskId, TASK_FINISHED, "");
    d->sendStatusUpdate(status);
  }
};


int main(int argc, char** argv) {
  MyExecutor exec;
  MesosExecutorDriver driver(&exec);
  driver.run();
  return 0;
}
