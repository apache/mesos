#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <pthread.h>
#include <nexus_sched.h>
#include <boost/unordered_map.hpp>
#include "lr.hpp"

using namespace std;
using namespace boost;


string nexusHost;
unsigned short nexusPort;
int numIters;
int numTasks;
double w[D];
double **taskOutputs;
volatile bool tasksAvailable = false;
volatile bool tasksDone = false;
int numTasksStarted;
int numTasksFinished;

nexus_handle nexusHandle;
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
  
  nexusHost = argv[1];
  nexusPort = atoi(argv[2]);
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
  
  nexus_close_scheduler(nexusHandle);
  exit(0);
}


// Nexus callbacks
void registerStarted(nexus_scheduler *sched);
void registered(nexus_scheduler *sched, framework_id fid);
void slot_offer(nexus_scheduler *sched, slot_offer_id oid, nexus_slot *slot);
void slot_offer_rescinded(nexus_scheduler *sched, slot_offer_id oid);
void status_update(nexus_scheduler *sched, nexus_task_status *status);
void framework_message(nexus_scheduler *sched, nexus_framework_message *msg);
void slave_lost(nexus_scheduler *sched, slave_id sid);
void error(nexus_scheduler *sched, int code, const char *message);

// Nexus scheduler
nexus_scheduler scheduler = {
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
  nexus_run_scheduler(nexusHost.c_str(), nexusPort, &scheduler, &nexusHandle);
}

void registerStarted(nexus_scheduler *sched)
{
  cout << "Started registering" << endl;
}

void registered(nexus_scheduler *sched, framework_id fid)
{
  cout << "Registered with Nexus, framework ID = " << fid << endl;
}

void slot_offer(nexus_scheduler *sched, slot_offer_id oid, nexus_slot *slot)
{
  cout << "Got slot offer for slave " << slot->sid << endl;
  if (tasksAvailable && numTasksStarted < numTasks) {
    nexus_task_desc task;
    task.tid = nextTaskId++;
    indexOfTask[task.tid] = numTasksStarted++;
    task.name = 0;
    task.arg = (void*) w;
    task.arg_len = sizeof(w);
    nexus_accept_slot_offer(nexusHandle, oid, &task);
    cout << "Scheduled task " << indexOfTask[task.tid] 
         << " as TID " << task.tid << endl;
  } else {
    nexus_refuse_slot_offer(nexusHandle, oid);
  }
}

void status_update(nexus_scheduler *sched, nexus_task_status *status)
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

void error(nexus_scheduler *sched, int code, const char *message)
{
  cout << "Error from Nexus: " << message << endl;
  exit(code);
}

void slot_offer_rescinded(nexus_scheduler *sched, slot_offer_id oid) {}

void framework_message(nexus_scheduler *sched, nexus_framework_message *msg) {}

void slave_lost(nexus_scheduler *sched, slave_id sid) {}
