/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __SLAVE_HPP__
#define __SLAVE_HPP__

#include <process/http.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/http.hpp"
#include "slave/isolation_module.hpp"

#include "common/attributes.hpp"
#include "common/resources.hpp"
#include "common/type_utils.hpp"

#include "files/files.hpp"

#include "messages/messages.hpp"


namespace mesos {
namespace internal {
namespace slave {

using namespace process;

// Some forward declarations.
struct Executor;
struct Framework;


class Slave : public ProtobufProcess<Slave>
{
public:
  Slave(const Resources& resources,
        bool local,
        IsolationModule* isolationModule,
        Files* files);

  Slave(const flags::Flags<logging::Flags, slave::Flags>& flags,
        bool local,
        IsolationModule *isolationModule,
        Files* files);

  virtual ~Slave();

  void shutdown();

  void newMasterDetected(const UPID& pid);
  void noMasterDetected();
  void masterDetectionFailure();
  void registered(const SlaveID& slaveId);
  void reregistered(const SlaveID& slaveId);
  void doReliableRegistration();
  void runTask(const FrameworkInfo& frameworkInfo,
               const FrameworkID& frameworkId,
               const std::string& pid,
               const TaskInfo& task);
  void killTask(const FrameworkID& frameworkId,
                const TaskID& taskId);
  void shutdownFramework(const FrameworkID& frameworkId);
  void schedulerMessage(const SlaveID& slaveId,
			const FrameworkID& frameworkId,
			const ExecutorID& executorId,
			const std::string& data);
  void updateFramework(const FrameworkID& frameworkId,
                       const std::string& pid);
  void statusUpdateAcknowledgement(const SlaveID& slaveId,
                                   const FrameworkID& frameworkId,
                                   const TaskID& taskId,
                                   const std::string& uuid);
  void registerExecutor(const FrameworkID& frameworkId,
                        const ExecutorID& executorId);
  void statusUpdate(const StatusUpdate& update);
  void executorMessage(const SlaveID& slaveId,
                       const FrameworkID& frameworkId,
                       const ExecutorID& executorId,
                       const std::string& data);
  void ping(const UPID& from, const std::string& body);

  void statusUpdateTimeout(const FrameworkID& frameworkId, const UUID& uuid);

  StatusUpdate createStatusUpdate(const TaskID& taskId,
                                  const ExecutorID& executorId,
                                  const FrameworkID& frameworkId,
                                  TaskState taskState,
                                  const std::string& message);

  void executorStarted(const FrameworkID& frameworkId,
                       const ExecutorID& executorId,
                       pid_t pid);

  void executorExited(const FrameworkID& frameworkId,
                      const ExecutorID& executorId,
                      int status);

  void transitionLiveTask(const TaskID& taskId,
                          const ExecutorID& executorId,
                          const FrameworkID& frameworkId,
                          bool command_executor,
                          int status);
protected:
  virtual void initialize();
  virtual void finalize();
  virtual void exited(const UPID& pid);

  void fileAttached(const Future<Nothing>& result);

  // Helper routine to lookup a framework.
  Framework* getFramework(const FrameworkID& frameworkId);

  // Shut down an executor. This is a two phase process. First, an
  // executor receives a shut down message (shut down phase), then
  // after a configurable timeout the slave actually forces a kill
  // (kill phase, via the isolation module) if the executor has not
  // exited.
  void shutdownExecutor(Framework* framework, Executor* executor);

  // Handle the second phase of shutting down an executor for those
  // executors that have not properly shutdown within a timeout.
  void shutdownExecutorTimeout(const FrameworkID& frameworkId,
                               const ExecutorID& executorId,
                               const UUID& uuid);

//   // Create a new status update stream.
//   StatusUpdates* createStatusUpdateStream(const StatusUpdateStreamID& streamId,
//                                           const string& directory);

//   StatusUpdates* getStatusUpdateStream(const StatusUpdateStreamID& streamId);

  // Helper function for generating a unique work directory for this
  // framework/executor pair (non-trivial since a framework/executor
  // pair may be launched more than once on the same slave).
  std::string createUniqueWorkDirectory(const FrameworkID& frameworkId,
                                        const ExecutorID& executorId);

private:
  // HTTP handlers, friends of the slave in order to access state,
  // they get invoked from within the slave so there is no need to
  // use synchronization mechanisms to protect state.
  friend Future<process::http::Response> http::vars(
      const Slave& slave,
      const process::http::Request& request);

  friend Future<process::http::Response> http::json::stats(
      const Slave& slave,
      const process::http::Request& request);

  friend Future<process::http::Response> http::json::state(
      const Slave& slave,
      const process::http::Request& request);

  const flags::Flags<logging::Flags, slave::Flags> flags;

  bool local;

  SlaveID id;
  SlaveInfo info;

  UPID master;

  Resources resources;
  Attributes attributes;

  hashmap<FrameworkID, Framework*> frameworks;

  IsolationModule* isolationModule;
  Files* files;

  // Statistics (initialized in Slave::initialize).
  struct {
    uint64_t tasks[TaskState_ARRAYSIZE];
    uint64_t validStatusUpdates;
    uint64_t invalidStatusUpdates;
    uint64_t validFrameworkMessages;
    uint64_t invalidFrameworkMessages;
  } stats;

  double startTime;

  bool connected; // Flag to indicate if slave is registered.

  GarbageCollector gc;
};


// Information describing an executor (goes away if executor crashes).
struct Executor
{
  Executor(const FrameworkID& _frameworkId,
           const ExecutorInfo& _info,
           const std::string& _directory)
    : id(_info.executor_id()),
      info(_info),
      frameworkId(_frameworkId),
      directory(_directory),
      uuid(UUID::random()),
      pid(UPID()),
      shutdown(false),
      resources(_info.resources()) {}

  ~Executor()
  {
    // Delete the tasks.
    foreachvalue (Task* task, launchedTasks) {
      delete task;
    }
  }

  Task* addTask(const TaskInfo& task)
  {
    // The master should enforce unique task IDs, but just in case
    // maybe we shouldn't make this a fatal error.
    CHECK(!launchedTasks.contains(task.task_id()));

    Task *t = new Task();
    t->mutable_framework_id()->MergeFrom(frameworkId);
    t->set_state(TASK_STAGING);
    t->set_name(task.name());
    t->mutable_task_id()->MergeFrom(task.task_id());
    t->mutable_slave_id()->MergeFrom(task.slave_id());
    t->mutable_resources()->MergeFrom(task.resources());

    if (!task.has_command()) {
      t->mutable_executor_id()->MergeFrom(id);
    }

    launchedTasks[task.task_id()] = t;
    resources += task.resources();
    return t;
  }

  void removeTask(const TaskID& taskId)
  {
    // Remove the task if it's queued.
    queuedTasks.erase(taskId);

    // Update the resources if it's been launched.
    if (launchedTasks.contains(taskId)) {
      Task* task = launchedTasks[taskId];
      foreach (const Resource& resource, task->resources()) {
        resources -= resource;
      }
      launchedTasks.erase(taskId);
      delete task;
    }
  }

  void updateTaskState(const TaskID& taskId, TaskState state)
  {
    if (launchedTasks.contains(taskId)) {
      launchedTasks[taskId]->set_state(state);
    }
  }

  const ExecutorID id;
  const ExecutorInfo info;

  const FrameworkID frameworkId;

  const std::string directory;

  const UUID uuid; // Distinguishes executor instances with same ExecutorID.

  UPID pid;

  bool shutdown; // Indicates if executor is being shut down.

  Resources resources; // Currently consumed resources.

  hashmap<TaskID, TaskInfo> queuedTasks;
  hashmap<TaskID, Task*> launchedTasks;
};


// Information about a framework.
struct Framework
{
  Framework(const FrameworkID& _id,
            const FrameworkInfo& _info,
            const UPID& _pid,
            const Flags& _flags)
    : id(_id),
      info(_info),
      pid(_pid),
      flags(_flags) {}

  ~Framework() {}

  // Returns an ExecutorInfo for a TaskInfo (possibly
  // constructing one if the task has a CommandInfo).
  ExecutorInfo getExecutorInfo(const TaskInfo& task)
  {
    CHECK(task.has_executor() != task.has_command());

    if (task.has_command()) {
      ExecutorInfo executor;

      // Prepare an executor id which includes information on the
      // command being launched.
      std::string id = "Task " + task.task_id().value() + " (";
      if (task.command().value().length() > 15) {
        id += task.command().value().substr(0, 12) + "...)";
      } else {
        id += task.command().value() + ")";
      }

      executor.mutable_executor_id()->set_value(id);

      // Now determine the path to the executor.
      Try<std::string> path = os::realpath(
          path::join(flags.launcher_dir, "mesos-executor"));

      if (path.isSome()) {
        executor.mutable_command()->set_value(path.get());
      } else {
        executor.mutable_command()->set_value(
            "echo '" + path.error() + "'; exit 1");
      }

      // TODO(benh): Set some resources for the executor so that a task
      // doesn't end up getting killed because the amount of resources of
      // the executor went over those allocated. Note that this might mean
      // that the number of resources on the machine will actually be
      // slightly oversubscribed, so we'll need to reevaluate with respect
      // to resources that can't be oversubscribed.
      return executor;
    }

    return task.executor();
  }

  Executor* createExecutor(const ExecutorInfo& executorInfo,
                           const std::string& directory)
  {
    Executor* executor = new Executor(id, executorInfo, directory);
    CHECK(!executors.contains(executorInfo.executor_id()));
    executors[executorInfo.executor_id()] = executor;
    return executor;
  }

  void destroyExecutor(const ExecutorID& executorId)
  {
    if (executors.contains(executorId)) {
      Executor* executor = executors[executorId];
      executors.erase(executorId);
      delete executor;
    }
  }

  Executor* getExecutor(const ExecutorID& executorId)
  {
    if (executors.contains(executorId)) {
      return executors[executorId];
    }

    return NULL;
  }

  Executor* getExecutor(const TaskID& taskId)
  {
    foreachvalue (Executor* executor, executors) {
      if (executor->queuedTasks.contains(taskId) ||
          executor->launchedTasks.contains(taskId)) {
        return executor;
      }
    }

    return NULL;
  }

  const FrameworkID id;
  const FrameworkInfo info;

  UPID pid;

  const Flags flags;

  // Current running executors.
  hashmap<ExecutorID, Executor*> executors;

  // Status updates keyed by uuid.
  hashmap<UUID, StatusUpdate> updates;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_HPP__
