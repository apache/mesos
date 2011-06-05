#include <nexus_exec.hpp>

#include <cstdlib>
#include <iostream>


using namespace std;
using namespace nexus;

class MyExecutor : public Executor
{
public:
  virtual ~MyExecutor() {}

  virtual void init(ExecutorDriver*, const ExecutorArgs& args) {
    cout << "Init" << endl;
  }

  virtual void startTask(ExecutorDriver* d, const TaskDescription& task) {
    cout << "Starting task " << task.taskId << endl;
    sleep(1);
    cout << "Finishing task " << task.taskId << endl;
    TaskStatus status(task.taskId, TASK_FINISHED, "");
    d->sendStatusUpdate(status);
  }
};


int main(int argc, char** argv) {
  MyExecutor exec;
  NexusExecutorDriver driver(&exec);
  driver.run();
  return 0;
}
