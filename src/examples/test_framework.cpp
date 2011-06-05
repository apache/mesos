#include <iostream>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mesos_sched.h>

using namespace std;

const int TOTAL_TASKS = 5;
int tasksStarted = 0;
int tasksFinished = 0;


void registered(mesos_sched *sched, framework_id fid)
{
  cout << "Registered with Mesos, framework ID = " << fid << endl;
}


void slot_offer(mesos_sched *sched, offer_id oid,
                mesos_slot *slots, int num_slots)
{
  // TODO: Make this loop over offers rather than looking only at first offer!
  cout << "Got slot offer " << oid << endl;
  if (tasksStarted >= TOTAL_TASKS) {
    cout << "Refusing it" << endl;
    mesos_sched_reply_to_offer(sched, oid, 0, 0, "timeout=-1");
  } else {
    task_id tid = tasksStarted++;
    cout << "Accepting it to start task " << tid << endl;
    mesos_task_desc desc = { tid, slots[0].sid, "task",
      "cpus=1\nmem=32", 0, 0 };
    mesos_sched_reply_to_offer(sched, oid, &desc, 1, "");
    if (tasksStarted > 4)
      mesos_sched_unreg(sched);
  }
}


void slot_offer_rescinded(mesos_sched *sched, offer_id oid)
{
  cout << "Slot offer rescinded: " << oid << endl;
}


void status_update(mesos_sched *sched, mesos_task_status *status)
{
  cout << "Task " << status->tid << " entered state " << status->state << endl;
  if (status->state == TASK_FINISHED) {
    tasksFinished++;
    if (tasksFinished == TOTAL_TASKS)
      exit(0);
  }
}


void framework_message(mesos_sched *sched, mesos_framework_message *msg)
{
  cout << "Got a framework message from slave " << msg->sid
       << ": " << (char *) msg->data << endl;
}


void slave_lost(mesos_sched *sched, slave_id sid)
{
  cout << "Lost slave " << sid << endl;
}  


void error(mesos_sched *sched, int code, const char *message)
{
  cout << "Error from Mesos: " << message << endl;
  exit(code);
}


mesos_sched sched = {
  "test framework",
  "", // Executor (will be set in main to get absolute path)
  registered,
  slot_offer,
  slot_offer_rescinded,
  status_update,
  framework_message,
  slave_lost,
  error,
  (void *) "test",
  4,
  0
};


int main(int argc, char **argv)
{
  // Find this executable's directory to locate executor
  char buf[4096];
  realpath(dirname(argv[0]), buf);
  string executor = string(buf) + "/test-executor";
  sched.executor_name = executor.c_str();

  if (argc == 2 && string("--help") == argv[0]) {
    cerr << "Usage: " << argv[0] << " MASTER_URL" << endl
         << "  OR   " << argv[0] << " --url=URL [OPTIONS]" << endl;
    exit(1);
  }

  if (mesos_sched_init(&sched) < 0) {
    perror("mesos_sched_init");
    exit(1);
  }

  if (argc == 1 && strlen(argv[1]) > 0 && argv[1][0] != '-') {
    // Initialize with master URL alone
    if (mesos_sched_reg(&sched, argv[1]) < 0) {
      perror("mesos_sched_reg");
      exit(1);
    }
  } else {
    // Initialize by parsing command line
    if (mesos_sched_reg_with_cmdline(&sched, argc, argv) < 0) {
      perror("mesos_sched_reg");
      exit(1);
    }
  }

  mesos_sched_join(&sched);

  if (mesos_sched_destroy(&sched) < 0) {
    perror("mesos_sched_destroy");
    exit(1);
  }

  return 0;
}
