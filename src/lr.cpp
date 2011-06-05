#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <pthread.h>
#include <mesos_sched.h>
#include <boost/unordered_map.hpp>
#include "lr.hpp"

using namespace std;
using namespace boost;


string mesosHost;
unsigned short mesosPort;
int numIters;
int numTasks;
double w[D];
double **taskOutputs;
volatile bool tasksAvailable = false;
volatile bool tasksDone = false;
int numTasksStarted;
int numTasksFinished;

mesos_handle mesosHandle;
pthread_t schedulerThread;
pthread_mutex_t mutex;
pthread_cond_t tasksDoneCond;
void * runScheduler(void *);


void evalObjGrad(double *objGrad) {
  // Set up scheduling state
  numTasksStarted = 0;
  numTasksFinished = 0;
  tasksDone = false;
  tasksAvailable = true;
  
  // Wait for tasks to run
  pthread_mutex_lock(&mutex);
  if (!tasksDone) {
    pthread_cond_wait(&tasksDoneCond, &mutex);
  }
  pthread_mutex_unlock(&mutex);
  
  // Add up the results
  memset(objGrad, 0, D*sizeof(double));
  for (int i=0; i<numTasks; i++) {
    for (int j=0; j<D; j++) {
      objGrad[j] += taskOutputs[i][j];
    }
  }
}


int main(int argc, char **argv)
{
  if (argc != 5) {
    cerr << "Usage: lr <host> <port> <numIters> <tasksPerIter>" << endl;
    return 1;
  }
  
  mesosHost = argv[1];
  mesosPort = atoi(argv[2]);
  numIters = atoi(argv[3]);
  numTasks = atoi(argv[4]);
  
  taskOutputs = new double*[numTasks];
  for (int i=0; i<numTasks; i++) {
    taskOutputs[i] = new double[D];
  }

  pthread_mutex_init(&mutex, 0);
  pthread_cond_init(&tasksDoneCond, 0);
  pthread_create(&schedulerThread, 0, runScheduler, 0);

  // Start w at a random value
  srand(time(0));
  for (int i=0; i<D; i++) {
    w[i] = 50.0 * rand() / double(RAND_MAX);
  }  
  cout << setprecision(4);
  cout << "Initial w =";
  for (int i=0; i<D; i++) {
    cout << " " << w[i];
  }
  cout << endl << endl;
  
  // Iterate NUM_ITERS times
  for (int iter = 1; iter <= numIters; iter++) {
    // Evaluate objective gradient
    double objGrad[D];
    evalObjGrad(objGrad);
    cout << "objGrad =";
    for (int i=0; i<D; i++) {
      cout << " " << objGrad[i];
    }
    cout << endl;
    // Add objective gradient to w
    for (int i=0; i<D; i++) {
      w[i] -= (0.008 / iter) * objGrad[i];
    }
    // Print current w
    cout << "After iteration " << iter << ": w =";
    for (int i=0; i<D; i++) {
      cout << " " << w[i];
    }
    cout << endl << endl;
  }
  
  mesos_close_scheduler(mesosHandle);
  exit(0);
}


// Mesos callbacks
void registerStarted(mesos_scheduler *sched);
void registered(mesos_scheduler *sched, framework_id fid);
void slot_offer(mesos_scheduler *sched, slot_offer_id oid, mesos_slot *slot);
void slot_offer_rescinded(mesos_scheduler *sched, slot_offer_id oid);
void status_update(mesos_scheduler *sched, mesos_task_status *status);
void framework_message(mesos_scheduler *sched, mesos_framework_message *msg);
void slave_lost(mesos_scheduler *sched, slave_id sid);
void error(mesos_scheduler *sched, int code, const char *message);

// Mesos scheduler
mesos_scheduler scheduler = {
  "LR framework",
  "lr_exec",
  registerStarted,
  registered,
  slot_offer,
  slot_offer_rescinded,
  status_update,
  framework_message,
  slave_lost,
  error,
  0,
  0,
  0
};

// Maps task_ids to indexes from 0 to numTasks
unordered_map<int, int> indexOfTask;

// Global counter of tasks to assign unique TaskIDs
task_id nextTaskId = 0;

void * runScheduler(void *)
{
  mesos_run_scheduler(mesosHost.c_str(), mesosPort, &scheduler, &mesosHandle);
}

void registerStarted(mesos_scheduler *sched)
{
  cout << "Started registering" << endl;
}

void registered(mesos_scheduler *sched, framework_id fid)
{
  cout << "Registered with Mesos, framework ID = " << fid << endl;
}

void slot_offer(mesos_scheduler *sched, slot_offer_id oid, mesos_slot *slot)
{
  cout << "Got slot offer for slave " << slot->sid << endl;
  if (tasksAvailable && numTasksStarted < numTasks) {
    mesos_task_desc task;
    task.tid = nextTaskId++;
    indexOfTask[task.tid] = numTasksStarted++;
    task.name = 0;
    task.arg = (void*) w;
    task.arg_len = sizeof(w);
    mesos_accept_slot_offer(mesosHandle, oid, &task);
    cout << "Scheduled task " << indexOfTask[task.tid] 
         << " as TID " << task.tid << endl;
  } else {
    mesos_refuse_slot_offer(mesosHandle, oid);
  }
}

void status_update(mesos_scheduler *sched, mesos_task_status *status)
{
  if (status->state == TASK_FINISHED) {
    cout << "Task ID " << status->tid << " finished" << endl;
    numTasksFinished++;
    
    // Save the output
    int index = indexOfTask[status->tid];
    double *result = (double*) status->data;
    memcpy(taskOutputs[index], result, D * sizeof(double));
    
    // If all tasks are done, set tasksAvailable to false and fire condition
    if (numTasksFinished == numTasks) {
      tasksAvailable = false;
      pthread_mutex_lock(&mutex);
      tasksDone = true;
      pthread_cond_signal(&tasksDoneCond);
      pthread_mutex_unlock(&mutex);
    }
  }
}

void error(mesos_scheduler *sched, int code, const char *message)
{
  cout << "Error from Mesos: " << message << endl;
  exit(code);
}

void slot_offer_rescinded(mesos_scheduler *sched, slot_offer_id oid) {}

void framework_message(mesos_scheduler *sched, mesos_framework_message *msg) {}

void slave_lost(mesos_scheduler *sched, slave_id sid) {}
