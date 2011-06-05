#include <stdlib.h>
#include <unistd.h>

#include <iostream>

#include "nexus_exec.h"

using std::cout;
using std::cerr;
using std::endl;


void init(struct nexus_exec* exec,
          slave_id sid,
          framework_id fid,
          const char* name,
          const void* init_arg,
          int init_arg_len)
{
  cout << "init with slave ID " << sid << " framework ID " << fid
       << " and name " << name << endl;
}


void run(struct nexus_exec* exec, struct nexus_task_desc* task)
{
  struct nexus_task_status done = { task->tid, TASK_FINISHED, 0, 0 };
  cout << "run()" << endl;
  sleep(1);
  cout << "run() done" << endl;
  nexus_exec_status_update(exec, &done);
}


void kill(struct nexus_exec* exec, task_id tid)
{
  cout << "asked to kill task, but that isn't implemented" << endl;
}


void message(struct nexus_exec* exec, struct nexus_framework_message* msg)
{
  cout << "received framework message" << endl;
}


void shutdown(struct nexus_exec* exec)
{
  cerr << "shutdown requested" << endl;
  exit(1);
}


void error(struct nexus_exec* exec, int code, const char* msg)
{
  cerr << "error encountered: " << msg << endl;
  exit(1);
}


int main() {
  struct nexus_exec exec = { init, run, kill, message, shutdown, error, NULL };
  return nexus_exec_run(&exec);
}
