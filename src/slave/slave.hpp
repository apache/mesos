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
#include <set>
#include <vector>

#include <arpa/inet.h>

#include <boost/lexical_cast.hpp>
#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>

#include <glog/logging.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <process.hpp>

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

using foreach::_;


const double STATUS_UPDATE_RETRY_TIMEOUT = 10;


// Information describing an executor (goes away if executor crashes).
struct Executor
{
  Executor(const FrameworkID& _frameworkId, const ExecutorInfo& _info)
    : frameworkId(_frameworkId), info(_info), pid(PID()) {}

  ~Executor()
  {
    // Delete the tasks.
    foreachpair (_, Task *task, tasks) {
      delete task;
    }
  }

  Task* addTask(const TaskDescription& task)
  {
    // The master should enforce unique task IDs, but just in case
    // maybe we shouldn't make this a fatal error.
    CHECK(tasks.count(task.task_id()) == 0);

    Task *t = new Task();
    t->mutable_framework_id()->MergeFrom(frameworkId);
    t->mutable_executor_id()->MergeFrom(info.executor_id());
    t->set_state(TASK_STARTING);
    t->set_name(task.name());
    t->mutable_task_id()->MergeFrom(task.task_id());
    t->mutable_slave_id()->MergeFrom(task.slave_id());
    t->mutable_resources()->MergeFrom(task.resources());

    tasks[task.task_id()] = t;
    resources += task.resources();
  }

  void removeTask(const TaskID& taskId)
  {
    // Remove task from the queue if it's queued
    for (std::list<TaskDescription>::iterator it = queuedTasks.begin();
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

  const FrameworkID frameworkId;
  const ExecutorInfo info;

  PID pid;

  std::list<TaskDescription> queuedTasks;
  boost::unordered_map<TaskID, Task*> tasks;

  Resources resources;

  // Information about the status of the executor for this framework, set by
  // the isolation module. For example, this might include a PID, a VM ID, etc.
  std::string executorStatus;
};


// Information about a framework.
struct Framework
{
  Framework( const FrameworkID& _frameworkId, const FrameworkInfo& _info,
            const PID& _pid)
    : frameworkId(_frameworkId), info(_info), pid(_pid) {}

  ~Framework() {}

  Executor* createExecutor(const ExecutorInfo& info)
  {
    Executor* executor = new Executor(frameworkId, info);
    CHECK(executors.count(info.executor_id()) == 0);
    executors[info.executor_id()] = executor;
    return executor;
  }

  void destroyExecutor(const ExecutorID& executorId)
  {
    if (executors.count(executorId) > 0) {
      Executor* executor = executors[executorId];
      executors.erase(executorId);
      delete executor;
    }
  }

  Executor* getExecutor(const ExecutorID& executorId)
  {
    if (executors.count(executorId) > 0) {
      return executors[executorId];
    }

    return NULL;
  }

  Executor* getExecutor(const TaskID& taskId)
  {
    foreachpair (_, Executor* executor, executors) {
      if (executor->tasks.count(taskId) > 0) {
        return executor;
      }
    }

    return NULL;
  }

  const FrameworkID frameworkId;
  const FrameworkInfo info;

  PID pid;

  boost::unordered_map<ExecutorID, Executor*> executors;
  boost::unordered_map<double, boost::unordered_map<TaskID, TaskStatus> > statuses;
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
          MSG<SH2M_HEARTBEAT> msg;
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
  void executorExited(const FrameworkID& frameworkId, const ExecutorID& executorId, int status);

  // Kill a framework (possibly killing its executor).
  void killFramework(Framework *framework, bool killExecutors = true);

  std::string getUniqueWorkDirectory(const FrameworkID& frameworkId);

  const Configuration& getConfiguration();

  // TODO(...): Don't make these instance variables public! Hack for
  // now because they are needed in the isolation modules.
  bool local;
  SlaveID slaveId;

protected:
  virtual void operator () ();

  Framework* getFramework(const FrameworkID& frameworkId);

  // Send any tasks queued up for the given framework to its executor
  // (needed if we received tasks while the executor was starting up).
  void sendQueuedTasks(Framework* framework, Executor* executor);

private:
  Configuration conf;

  PID master;
  Resources resources;

  // Invariant: framework will exist if executor exists.
  boost::unordered_map<FrameworkID, Framework*> frameworks;

  IsolationModule *isolationModule;
  Heart* heart;
};

}}}

#endif /* __SLAVE_HPP__ */
