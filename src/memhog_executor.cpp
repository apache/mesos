#include <time.h>

#include <nexus_exec.hpp>

#include <boost/lexical_cast.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>


using namespace std;
using namespace nexus;

using boost::lexical_cast;


class MemHogExecutor;


struct ThreadArg
{
  MemHogExecutor *executor;
  TaskID tid;
  bool primary;

  ThreadArg(MemHogExecutor *e, TaskID t, bool p)
    : executor(e), tid(t), primary(p) {}
};


void *runTask(void *arg);


class MemHogExecutor : public Executor
{
public:
  double taskLen;
  int64_t memToHog;
  int threadsPerTask;
  virtual ~MemHogExecutor() {}

  virtual void init(const ExecutorArgs &args) {
    istringstream in(args.data);
    in >> memToHog >> taskLen >> threadsPerTask;
    cout << "Initialized: memToHog = " << memToHog
         << ", taskLen = " << taskLen
         << ", threadsPerTask = " << threadsPerTask << endl;
  }

  virtual void startTask(const TaskDescription& task) {
    cout << "Executor starting task " << task.taskId << endl;
    for (int i = 0; i < threadsPerTask; i++) {
      ThreadArg *arg = new ThreadArg(this, task.taskId, i == 0);
      pthread_t thread;
      pthread_create(&thread, 0, runTask, arg);
      pthread_detach(thread);
    }
  }
};


void *runTask(void *arg)
{
  ThreadArg *threadArg = (ThreadArg *) arg;
  MemHogExecutor *executor = threadArg->executor;
  int64_t memToHog = executor->memToHog;
  double taskLen = executor->taskLen;
  cout << "Running a task..." << endl;
  char *data = new char[memToHog];
  int32_t count = 0;
  time_t start = time(0);
  while (true) {
    for (int64_t i = 0; i < memToHog; i++) {
      data[i] = i;
      count++;
      if (count == 10000) {
        count = 0;
        time_t now = time(0);
        if (difftime(now, start) > taskLen) {
          delete[] data;
          if (threadArg->primary) {
            usleep(100000); // sleep 0.1 seconds
            TaskStatus status(threadArg->tid, TASK_FINISHED, "");
            executor->sendStatusUpdate(status);
          }
          return 0;
        }
      }
    }
  }
}


int main(int argc, char ** argv) {
  MemHogExecutor exec;
  exec.run();
  return 0;
}
