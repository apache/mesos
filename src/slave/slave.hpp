#ifndef __SLAVE_HPP__
#define __SLAVE_HPP__

#include <dirent.h>
#include <libgen.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <strings.h>

#include <iostream>
#include <list>
#include <sstream>
#include <vector>

#include <arpa/inet.h>

#include <boost/lexical_cast.hpp>
#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>

#include <glog/logging.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <reliable.hpp>

#include "isolation_module.hpp"
#include "state.hpp"

#include "common/fatal.hpp"
#include "common/foreach.hpp"
#include "common/params.hpp"
#include "common/resources.hpp"
#include "common/task.hpp"

#include "configurator/configurator.hpp"

#include "detector/detector.hpp"

#include "messaging/messages.hpp"


namespace mesos { namespace internal { namespace slave {

using namespace mesos;
using namespace mesos::internal;

using std::list;
using std::pair;
using std::make_pair;
using std::ostringstream;
using std::string;
using std::vector;

using boost::lexical_cast;
using boost::unordered_map;
using boost::unordered_set;


// A description of a task that is yet to be launched
struct TaskDescription
{
  TaskID tid;
  string name;
  string args; // Opaque data
  Params params;
  
  TaskDescription(TaskID _tid, string _name, const string& _args,
      const Params& _params)
      : tid(_tid), name(_name), args(_args), params(_params) {}
};


// Information about a framework
struct Framework
{
  FrameworkID id;
  string name;
  string user;
  ExecutorInfo executorInfo;
  list<TaskDescription *> queuedTasks; // Holds tasks until executor starts
  unordered_map<TaskID, Task *> tasks;
  Resources resources;
  PID pid;

  // Information about the status of the executor for this framework, set by
  // the isolation module. For example, this might include a PID, a VM ID, etc.
  string executorStatus;
  
  Framework(FrameworkID _id, const string& _name, const string& _user,
            const ExecutorInfo& _executorInfo, const PID& _pid)
    : id(_id), name(_name), user(_user), executorInfo(_executorInfo), pid(_pid) {}

  ~Framework()
  {
    foreach(TaskDescription *desc, queuedTasks)
      delete desc;
    foreachpair (_, Task *task, tasks)
      delete task;
  }

  Task * lookupTask(TaskID tid)
  {
    unordered_map<TaskID, Task *>::iterator it = tasks.find(tid);
    if (it != tasks.end())
      return it->second;
    else
      return NULL;
  }

  Task * addTask(TaskID tid, const std::string& name, Resources res)
  {
    if (tasks.find(tid) != tasks.end()) {
      // This should never happen - the master will make sure that it never
      // lets a framework launch two tasks with the same ID.
      LOG(FATAL) << "Task ID " << tid << "already exists in framework " << id;
    }
    Task *task = new Task(tid, res);
    task->frameworkId = id;
    task->state = TASK_STARTING;
    task->name = name;
    tasks[tid] = task;
    resources += res;
    return task;
  }

  void removeTask(TaskID tid)
  {
    // Remove task from the queue if it's queued
    for (list<TaskDescription *>::iterator it = queuedTasks.begin();
	 it != queuedTasks.end(); ++it) {
      if ((*it)->tid == tid) {
	delete *it;
	queuedTasks.erase(it);
	break;
      }
    }

    // Remove it from tasks as well
    unordered_map<TaskID, Task *>::iterator it = tasks.find(tid);
    if (it != tasks.end()) {
      resources -= it->second->resources;
      delete it->second;
      tasks.erase(it);
    }
  }
};


// A connection to an executor (goes away if executor crashes)
struct Executor
{
  FrameworkID frameworkId;
  PID pid;
  
  Executor(FrameworkID _fid, PID _pid) : frameworkId(_fid), pid(_pid) {}
};


class Slave : public MesosProcess
{
public:
  Params conf;

  typedef unordered_map<FrameworkID, Framework*> FrameworkMap;
  typedef unordered_map<FrameworkID, Executor*> ExecutorMap;
  
  PID master;
  SlaveID id;
  Resources resources;
  bool local;
  FrameworkMap frameworks;
  ExecutorMap executors;  // Invariant: framework will exist if executor exists
  IsolationModule *isolationModule;

  // Sequence numbers of reliable messages sent on behalf of framework.
  unordered_map<FrameworkID, unordered_set<int> > seqs;

public:
  Slave(Resources resources, bool local, IsolationModule* isolationModule);

  Slave(const Params& conf, bool local, IsolationModule *isolationModule);

  virtual ~Slave();

  static void registerOptions(Configurator* conf);

  state::SlaveState *getState();

  // Callback used by isolation module to tell us when an executor exits.
  void executorExited(FrameworkID frameworkId, int status);

  // Kill a framework (possibly killing its executor).
  void killFramework(Framework *framework, bool killExecutor = true);

  string getUniqueWorkDirectory(FrameworkID fid);

  const Params& getConf();

protected:
  void operator () ();

  Framework * getFramework(FrameworkID frameworkId);

  Executor * getExecutor(FrameworkID frameworkId);

  // Send any tasks queued up for the given framework to its executor
  // (needed if we received tasks while the executor was starting up).
  void sendQueuedTasks(Framework *framework);
};

}}}

#endif /* __SLAVE_HPP__ */
