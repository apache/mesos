#include <time.h>

#include <cstdlib>
#include <iostream>
#include <sstream>

#include <mesos/executor.hpp>

using namespace mesos;
using namespace std;


class MemHogExecutor;


struct ThreadArg
{
  MemHogExecutor* executor;
  TaskDescription task;
  int threadId;
  int64_t memToHog; // in bytes
  double duration;

  ThreadArg(MemHogExecutor* executor_, TaskDescription task_, int threadId_,
            int64_t memToHog_, double duration_)
    : executor(executor_), task(task_), threadId(threadId_),
      memToHog(memToHog_), duration(duration_) {}
};


void* runTask(void* threadArg);


class MemHogExecutor : public Executor
{
public:
  ExecutorDriver* driver;

  virtual ~MemHogExecutor() {}

  virtual void init(ExecutorDriver* driver, const ExecutorArgs &args) {
    this->driver = driver;
  }

  virtual void launchTask(ExecutorDriver*, const TaskDescription& task) {
    cout << "Executor starting task " << task.task_id().value() << endl;
    int64_t memToHog;
    double duration;
    int numThreads;
    istringstream in(task.data());
    in >> memToHog >> duration >> numThreads;
    memToHog *= 1024LL * 1024LL; // Convert from MB to bytes
    for (int i = 0; i < numThreads; i++) {
      ThreadArg* arg = new ThreadArg(this, task, i, memToHog, duration);
      pthread_t thread;
      pthread_create(&thread, 0, runTask, arg);
      pthread_detach(thread);

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


// A simple linear congruential generator, used to access memory in a random
// pattern without relying on a possibly synchronized stdlib rand().
// Constants from http://en.wikipedia.org/wiki/Linear_congruential_generator.
uint32_t nextRand(uint32_t x) {
  const int64_t A = 1664525;
  const int64_t B = 1013904223;
  int64_t longX = x;
  return (uint32_t) ((A * longX + B) & 0xFFFFFFFF);
}


// Function executed by each worker thread.
void* runTask(void* threadArg)
{
  ThreadArg* arg = (ThreadArg*) threadArg;
  cout << "Running a worker thread..." << endl;
  char* data = new char[arg->memToHog];
  int32_t count = 0;
  time_t start = time(0);
  uint32_t pos = arg->threadId;
  while (true) {
    pos = nextRand(pos);
    data[pos % arg->memToHog] = pos;
    count++;
    if (count == 2000) {
      // Check whether enough time has elapsed to end the task
      count = 0;
      time_t now = time(0);
      if (difftime(now, start) > arg->duration) {
        delete[] data;
        if (arg->threadId == 0) {
          usleep(100000); // sleep 0.1 seconds for other threads to finish
          TaskStatus status;
          status.mutable_task_id()->MergeFrom(arg->task.task_id());
          status.set_state(TASK_FINISHED);
          arg->executor->driver->sendStatusUpdate(status);
        }
        return 0;
      }
    }
  }
}


int main(int argc, char** argv)
{
  MemHogExecutor exec;
  MesosExecutorDriver driver(&exec);
  driver.run();
  return 0;
}
