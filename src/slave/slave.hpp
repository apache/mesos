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
#include "common/resources.hpp"
#include "common/type_utils.hpp"

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

using foreach::_;


// Information about a framework
struct Framework
{
  FrameworkInfo info;
  FrameworkID frameworkId;
  PID pid;

  list<TaskDescription> queuedTasks; // Holds tasks until executor starts
  unordered_map<TaskID, Task *> tasks;

  Resources resources;

  // Information about the status of the executor for this framework, set by
  // the isolation module. For example, this might include a PID, a VM ID, etc.
  string executorStatus;
  
  Framework(const FrameworkInfo& _info, const FrameworkID& _frameworkId,
            const PID& _pid)
    : info(_info), frameworkId(_frameworkId), pid(_pid) {}

  ~Framework()
  {
    // Delete the tasks.
    foreachpair (_, Task *task, tasks) {
      delete task;
    }
  }

  Task * lookupTask(const TaskID& taskId)
  {
    unordered_map<TaskID, Task *>::iterator it = tasks.find(taskId);
    if (it != tasks.end())
      return it->second;
    else
      return NULL;
  }

  Task * addTask(const TaskDescription& task)
  {
    // The master should enforce unique task IDs, but just in case
    // maybe we shouldn't make this a fatal error.
    CHECK(tasks.count(task.task_id()) == 0);

    Task *t = new Task();
    t->set_name(task.name());
    t->mutable_task_id()->MergeFrom(task.task_id());
    t->mutable_framework_id()->MergeFrom(frameworkId);
    t->mutable_slave_id()->MergeFrom(task.slave_id());
    t->mutable_resources()->MergeFrom(task.resources());
    t->set_state(TASK_STARTING);

    tasks[task.task_id()] = t;
    resources += task.resources();

    return t;
  }

  void removeTask(const TaskID& taskId)
  {
    // Remove task from the queue if it's queued
    for (list<TaskDescription>::iterator it = queuedTasks.begin();
	 it != queuedTasks.end(); ++it) {
      if ((*it).task_id() == taskId) {
	queuedTasks.erase(it);
	break;
      }
    }

    // Remove it from tasks as well.
    if (tasks.count(taskId) > 0) {
      Task* task = tasks[taskId];
      foreach (const Resource& resource, task->resources()) {
        resources -= resource;
      }
      tasks.erase(taskId);
      delete task;
    }
  }
};


// A connection to an executor (goes away if executor crashes)
struct Executor
{
  FrameworkID frameworkId;
  PID pid;
  
  Executor(const FrameworkID& _frameworkId, const PID& _pid)
    : frameworkId(_frameworkId), pid(_pid) {}
};


// Periodically sends heartbeats to the master
class Heart : public MesosProcess
{
public:
  Heart(const PID &_master, const PID &_slave,
        const SlaveID& _slaveId, double _interval)
    : master(_master), slave(_slave), slaveId(_slaveId), interval(_interval) {}

protected:
  virtual void operator () ()
  {
    link(slave);
    link(master);
    do {
      switch (receive(interval)) {
        case PROCESS_TIMEOUT: {
          Message<SH2M_HEARTBEAT> msg;
          msg.mutable_slave_id()->MergeFrom(slaveId);
          send(master, msg);
          break;
        }
        case PROCESS_EXIT:
        default:
          return;
      }
    } while (true);
  }

private:
  const PID master;
  const PID slave;
  const SlaveID slaveId;
  const double interval;
};


class Slave : public MesosProcess
{
public:
  Slave(const Resources& resources, bool local,
        IsolationModule* isolationModule);

  Slave(const Configuration& conf, bool local,
        IsolationModule *isolationModule);

  virtual ~Slave();

  static void registerOptions(Configurator* conf);

  state::SlaveState *getState();

  // Callback used by isolation module to tell us when an executor exits.
  void executorExited(const FrameworkID& frameworkId, int status);

  // Kill a framework (possibly killing its executor).
  void killFramework(Framework *framework, bool killExecutor = true);

  string getUniqueWorkDirectory(const FrameworkID& frameworkId);

  const Configuration& getConfiguration();

  // TODO(...): Don't make these instance variables public! Hack for
  // now because they are needed in the isolation modules.
  bool local;
  SlaveID slaveId;

protected:
  virtual void operator () ();

  Framework * getFramework(const FrameworkID& frameworkId);

  Executor * getExecutor(const FrameworkID& frameworkId);

  // Send any tasks queued up for the given framework to its executor
  // (needed if we received tasks while the executor was starting up).
  void sendQueuedTasks(Framework* framework, Executor* executor);

private:
  Configuration conf;

  PID master;
  Resources resources;

  // Invariant: framework will exist if executor exists.
  unordered_map<FrameworkID, Framework*> frameworks;
  unordered_map<FrameworkID, Executor*> executors;

  IsolationModule *isolationModule;
  Heart* heart;

  // Sequence numbers of reliable messages sent on behalf of framework.
  unordered_map<FrameworkID, unordered_set<int> > seqs;
};

}}}

#endif /* __SLAVE_HPP__ */
