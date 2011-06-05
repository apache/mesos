#ifndef __SLAVE_HPP__
#define __SLAVE_HPP__

#include <dirent.h>
#include <libgen.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <strings.h>

#include <arpa/inet.h>

#include <glog/logging.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <iostream>
#include <list>
#include <sstream>
#include <set>
#include <vector>

#include <boost/lexical_cast.hpp>
#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>

#include <process/process.hpp>

#include "isolation_module.hpp"
#include "state.hpp"

#include "common/build.hpp"
#include "common/fatal.hpp"
#include "common/foreach.hpp"
#include "common/resources.hpp"
#include "common/type_utils.hpp"

#include "configurator/configurator.hpp"

#include "messaging/messages.hpp"


namespace mesos { namespace internal { namespace slave {

using foreach::_;


const double STATUS_UPDATE_RETRY_TIMEOUT = 10;


// Information describing an executor (goes away if executor crashes).
struct Executor
{
  Executor(const FrameworkID& _frameworkId, const ExecutorInfo& _info)
    : frameworkId(_frameworkId), info(_info), pid(process::UPID()) {}

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

  void updateTaskState(const TaskID& taskId, TaskState state)
  {
    if (tasks.count(taskId) > 0) {
      tasks[taskId]->set_state(state);
    }
  }

  const FrameworkID frameworkId;
  const ExecutorInfo info;

  process::UPID pid;

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
            const process::UPID& _pid)
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

  process::UPID pid;

  boost::unordered_map<ExecutorID, Executor*> executors;
  boost::unordered_map<double, boost::unordered_map<TaskID, TaskStatus> > statuses;
};


class Slave : public MesosProcess<Slave>
{
public:
  Slave(const Resources& resources, bool local,
        IsolationModule* isolationModule);

  Slave(const Configuration& conf, bool local,
        IsolationModule *isolationModule);

  virtual ~Slave();

  static void registerOptions(Configurator* conf);

  process::Promise<state::SlaveState*> getState();

  // Callback used by isolation module to tell us when an executor exits.
  void executorExited(const FrameworkID& frameworkId, const ExecutorID& executorId, int result);

  // Kill a framework (possibly killing its executor).
  void killFramework(Framework *framework, bool killExecutors = true);

  std::string getUniqueWorkDirectory(const FrameworkID& frameworkId, const ExecutorID& executorId);

  const Configuration& getConfiguration();

  void newMasterDetected(const std::string& pid);
  void noMasterDetected();
  void masterDetectionFailure();
  void registerReply(const SlaveID& slaveId);
  void reregisterReply(const SlaveID& slaveId);
  void runTask(const FrameworkInfo& frameworkInfo,
               const FrameworkID& frameworkId,
               const std::string& pid,
               const TaskDescription& task);
  void killTask(const FrameworkID& frameworkId,
                const TaskID& taskId);
  void killFramework(const FrameworkID& frameworkId);
  void schedulerMessage(const SlaveID& slaveId,
			const FrameworkID& frameworkId,
			const ExecutorID& executorId,
			const std::string& data);
  void updateFramework(const FrameworkID& frameworkId,
                       const std::string& pid);
  void statusUpdateAck(const FrameworkID& frameworkId,
                       const SlaveID& slaveId,
                       const TaskID& taskId);
  void registerExecutor(const FrameworkID& frameworkId,
                        const ExecutorID& executorId);
  void statusUpdate(const FrameworkID& frameworkId,
                    const TaskStatus& status);
  void executorMessage(const SlaveID& slaveId,
		       const FrameworkID& frameworkId,
		       const ExecutorID& executorId,
		       const std::string& data);
  void ping();
  void timeout();
  void exited();

  // TODO(...): Don't make these instance variables public! Hack for
  // now because they are needed in the isolation modules.
  bool local;
  SlaveID slaveId;

protected:
  virtual void operator () ();

  void initialize();

  Framework* getFramework(const FrameworkID& frameworkId);

  // Send any tasks queued up for the given framework to its executor
  // (needed if we received tasks while the executor was starting up).
  void sendQueuedTasks(Framework* framework, Executor* executor);

private:
  Configuration conf;

  SlaveInfo slave;

  process::UPID master;
  Resources resources;

  // Invariant: framework will exist if executor exists.
  boost::unordered_map<FrameworkID, Framework*> frameworks;

  IsolationModule *isolationModule;
};

}}}

#endif /* __SLAVE_HPP__ */
