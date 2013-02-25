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

#include <list>
#include <string>

#include <tr1/functional>

#include <boost/circular_buffer.hpp>

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
#include "slave/paths.hpp"
#include "slave/state.hpp"

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

  void runTask(
      const FrameworkInfo& frameworkInfo,
      const FrameworkID& frameworkId,
      const std::string& pid,
      const TaskInfo& task);

  void killTask(const FrameworkID& frameworkId, const TaskID& taskId);

  void shutdownFramework(const FrameworkID& frameworkId);

  void schedulerMessage(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void updateFramework(const FrameworkID& frameworkId, const std::string& pid);

  void statusUpdateAcknowledgement(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const std::string& uuid);

  void registerExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  void statusUpdate(const StatusUpdate& update);

  void executorMessage(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void ping(const UPID& from, const std::string& body);

  void statusUpdateTimeout(const FrameworkID& frameworkId, const UUID& uuid);

  void sendStatusUpdate(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const TaskID& taskId,
      TaskState taskState,
      const std::string& message);

  void executorStarted(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      pid_t pid);

  void executorTerminated(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      int status,
      bool destroyed,
      const std::string& message);

  // NOTE: Pulled this to public to make it visible for testing.
  // Garbage collects the directories based on the current disk usage.
  // TODO(vinod): Instead of making this function public, we need to
  // mock both GarbageCollector (and pass it through slave's constructor)
  // and os calls.
  void _checkDiskUsage(const Future<Try<double> >& capacity);

protected:
  virtual void initialize();
  virtual void finalize();
  virtual void exited(const UPID& pid);

  void fileAttached(const Future<Nothing>& result, const std::string& path);

  void detachFile(const Future<Nothing>& result, const std::string& path);

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

  // This function returns the max age of executor/slave directories allowed,
  // given a disk usage. This value could be used to tune gc.
  Duration age(double usage);

  // Checks the current disk usage and schedules for gc as necessary.
  void checkDiskUsage();

private:
  Slave(const Slave&);              // No copying.
  Slave& operator = (const Slave&); // No assigning.

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
  boost::circular_buffer<std::tr1::shared_ptr<Framework> > completedFrameworks;

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

  state::SlaveState state;
};


// Information describing an executor.
struct Executor
{
  Executor(const FrameworkID& _frameworkId,
           const ExecutorInfo& _info,
           const UUID& _uuid,
           const std::string& _directory)
    : id(_info.executor_id()),
      info(_info),
      frameworkId(_frameworkId),
      directory(_directory),
      uuid(_uuid),
      pid(UPID()),
      shutdown(false),
      resources(_info.resources()),
      completedTasks(MAX_COMPLETED_TASKS_PER_EXECUTOR) {}

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

      completedTasks.push_back(*task);

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

  boost::circular_buffer<Task> completedTasks;

private:
  Executor(const Executor&);              // No copying.
  Executor& operator = (const Executor&); // No assigning.
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
      flags(_flags),
      completedExecutors(MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK) {}

  ~Framework()
  {
    // We own the non-completed executor pointers, so they need to be deleted.
    foreachvalue (Executor* executor, executors) {
      delete executor;
    }
  }

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
      executor.set_name("Command Executor");
      executor.set_source(task.task_id().value());

      // Copy the CommandInfo to get the URIs and environment, but
      // update it to invoke 'mesos-executor' (unless we couldn't
      // resolve 'mesos-executor' via 'realpath', in which case just
      // echo the error and exit).
      executor.mutable_command()->MergeFrom(task.command());

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

  Executor* createExecutor(const SlaveID& slaveId,
                           const ExecutorInfo& executorInfo)
  {
    // We create a UUID for the new executor. The UUID uniquely identifies this
    // new instance of the executor across executors sharing the same executorID
    // that may have previously run. It also provides a means for the executor
    // to have a unique directory.
    UUID executorUUID = UUID::random();

    // Create a directory for the executor.
    const std::string& directory = paths::createExecutorDirectory(
        flags.work_dir, slaveId, id, executorInfo.executor_id(), executorUUID);

    Executor* executor =
      new Executor(id, executorInfo, executorUUID, directory);
    CHECK(!executors.contains(executorInfo.executor_id()));
    executors[executorInfo.executor_id()] = executor;
    return executor;
  }

  void destroyExecutor(const ExecutorID& executorId)
  {
    if (executors.contains(executorId)) {
      Executor* executor = executors[executorId];
      executors.erase(executorId);

      // Pass ownership of the executor pointer.
      completedExecutors.push_back(std::tr1::shared_ptr<Executor>(executor));
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

  // Up to MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK completed executors.
  boost::circular_buffer<std::tr1::shared_ptr<Executor> > completedExecutors;

  // Status updates keyed by uuid.
  hashmap<UUID, StatusUpdate> updates;

private:
  Framework(const Framework&);              // No copying.
  Framework& operator = (const Framework&); // No assigning.
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_HPP__
