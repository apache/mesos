#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <mesos_exec.h>
#include <boost/unordered_map.hpp>
#include "lr.hpp"

using namespace std;
using namespace boost;


pthread_mutex_t mutex;
unordered_map<string, vector<double*> > dataCache;

struct Task
{
  task_id tid;
  double w[D];
};


vector<double*>& getData(const char *file)
{
  pthread_mutex_lock(&mutex);
  if (dataCache.find(file) == dataCache.end()) {
    // Data file is not in cache; read it
    // TODO: allow reading different data files in parallel?
    cout << "Loading file " << file << endl;
    ifstream in(file);
    double y; // First element of line is 1 or -1 (classification of data point)
    while (in >> y) {
      double *point = new double[1 + D];
      point[0] = y;
      for (int i=1; i<=D; i++) in >> point[i];
      dataCache[file].push_back(point);
    }
    in.close();
    cout << "Done loading, total points = " << dataCache[file].size() << endl;
  }
  vector<double*>& ret = dataCache[file];
  pthread_mutex_unlock(&mutex);
  return ret;
}


double dot(double *a, double *b) {
  double result = 0;
  for (int i=0; i<D; i++)
    result += a[i] * b[i];
  return result;
}


void * runTask(void *arg) {
  pthread_detach(pthread_self());
  
  Task *task = (Task *) arg;
  cout << "running task " << task->tid << endl;
  const char *file = "/tmp/lr_data.txt"; // TODO: get file from task description
  
  // Get the cached list of points for this file
  vector<double*>& data = getData(file);
  
  // Evaluate the objective gradient
  double result[D];
  memset(result, 0, D * sizeof(double));
  for (int p = 0; p < data.size(); p++) {
    double y = data[p][0];
    double *x = data[p] + 1;
    double scale = (1 / (1 + exp(-y * dot(task->w, x))) - 1) * y;
    for (int i=0; i<D; i++) {
      result[i] += scale * x[i];
    }
  }
  
  // Return the result in a task status
  mesos_task_status status;
  status.tid = task->tid;
  status.state = TASK_FINISHED;
  status.data = (void*) result;
  status.data_len = D * sizeof(double);
  mesos_send_status_update(&status);
  
  // Clean up the task (since we own it) and exit the worker thread
  delete task;
  pthread_exit(0);
}


extern "C" {

bool lr_exec_initialize(mesos_exec_args *args)
{
  cout << "lr_exec_initialize" << endl;
  pthread_mutex_init(&mutex, 0);
  return true;
}


bool lr_exec_start_task(struct mesos_task_desc *task_desc)
{
  cout << "lr_exec_start_task" << endl;
  
  // Read w from task description
  Task *task = new Task;
  task->tid = task_desc->tid;
  memcpy(task->w, task_desc->arg, D * sizeof(double));
  
  // Launch a thread to run the task
  pthread_t thread;
  pthread_create(&thread, 0, runTask, (void *) task);

  return true;
}


bool lr_exec_kill_task(task_id tid)
{
  printf("asked to kill task %d\n", tid);
  // TODO: kill the task
  return true;
}


bool lr_exec_framework_message(struct mesos_framework_message *message)
{
  return true;
}

}
