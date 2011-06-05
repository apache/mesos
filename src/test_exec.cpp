#include <stdlib.h>
#include <unistd.h>

#include <iostream>

#include "mesos_exec.h"

using std::cout;
using std::cerr;
using std::endl;


void init(struct mesos_exec* exec,
          slave_id sid,
          const char* host,
          framework_id fid,
          const char* name,
          const void* init_arg,
          int init_arg_len)
{
  cout << "init executor on " << host << " (slave ID " << sid << ") "
       << "for framework " << name << " (framework ID " << fid << ")" << endl;
}


void launch_task(struct mesos_exec* exec, struct mesos_task_desc* task)
{
  struct mesos_task_status done = { task->tid, TASK_FINISHED, 0, 0 };
  cout << "run()" << endl;
  sleep(1);
  cout << "run() done" << endl;
  mesos_exec_status_update(exec, &done);
}


void kill_task(struct mesos_exec* exec, task_id tid)
{
  cout << "asked to kill task, but that isn't implemented" << endl;
}


void framework_message(struct mesos_exec* exec,
                       struct mesos_framework_message* msg)
{
  cout << "received framework message" << endl;
}


void shutdown(struct mesos_exec* exec)
{
  cerr << "shutdown requested" << endl;
  exit(1);
}


void error(struct mesos_exec* exec, int code, const char* msg)
{
  cerr << "error encountered: " << msg << endl;
  exit(1);
}


struct mesos_exec exec = { 
  init,
  launch_task,
  kill_task,
  framework_message,
  shutdown,
  error,
  NULL
};


int main() {
  return mesos_exec_run(&exec);
}
