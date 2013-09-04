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
#include <vector>

#include <tr1/functional>

#include <boost/circular_buffer.hpp>

#include <mesos/resources.hpp>

#include <process/http.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/bytes.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/multihashmap.hpp>
#include <stout/os.hpp>
#include <stout/owned.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/isolator.hpp"
#include "slave/monitor.hpp"
#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "common/attributes.hpp"
#include "common/protobuf_utils.hpp"
#include "common/type_utils.hpp"

#include "files/files.hpp"

#include "messages/messages.hpp"


namespace mesos {
namespace internal {
namespace slave {

using namespace process;

// Some forward declarations.
class StatusUpdateManager;
struct Executor;
struct Framework;


class Slave : public ProtobufProcess<Slave>
{
public:
  Slave(const Flags& flags,
        bool local,
        Isolator *isolator,
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

  void _runTask(
      const Future<bool>& future,
      const FrameworkInfo& frameworkInfo,
      const FrameworkID& frameworkId,
      const std::string& pid,
      const TaskInfo& task);

  Future<bool> unschedule(const std::string& path);

  void killTask(const FrameworkID& frameworkId, const TaskID& taskId);

  void shutdownFramework(const FrameworkID& frameworkId);

  void schedulerMessage(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void updateFramework(const FrameworkID& frameworkId, const std::string& pid);

  void registerExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Called when an executor re-registers with a recovering slave.
  // 'tasks' : Unacknowledged tasks (i.e., tasks that the executor
  //           driver never received an ACK for.)
  // 'updates' : Unacknowledged updates.
  void reregisterExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::vector<TaskInfo>& tasks,
      const std::vector<StatusUpdate>& updates);

  void executorMessage(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void ping(const UPID& from, const std::string& body);

  // Handles the status update.
  // NOTE: If 'pid' is a valid UPID an ACK is sent to this pid
  // after the update is successfully handled. If pid == UPID()
  // no ACK is sent. The latter is used by the slave to send
  // status updates it generated (e.g., TASK_LOST).
  void statusUpdate(const StatusUpdate& update, const UPID& pid);

  // This is called when the status update manager finishes
  // handling the update. If the handling is successful, an
  // acknowledgment is sent to the executor.
  void _statusUpdate(
      const Future<Nothing>& future,
      const StatusUpdate& update,
      const UPID& pid);

  void statusUpdateAcknowledgement(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const std::string& uuid);

  void _statusUpdateAcknowledgement(
      const Future<bool>& future,
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const UUID& uuid);

  void executorStarted(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      pid_t pid);

  void executorTerminated(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const Option<int>& status,
      bool destroyed,
      const std::string& message);

  // NOTE: Pulled these to public to make it visible for testing.
  // TODO(vinod): Make tests friends to this class instead.

  // Garbage collects the directories based on the current disk usage.
  // TODO(vinod): Instead of making this function public, we need to
  // mock both GarbageCollector (and pass it through slave's constructor)
  // and os calls.
  void _checkDiskUsage(const Future<Try<double> >& usage);

  // Shut down an executor. This is a two phase process. First, an
  // executor receives a shut down message (shut down phase), then
  // after a configurable timeout the slave actually forces a kill
  // (kill phase, via the isolator) if the executor has not
  // exited.
  void shutdownExecutor(Framework* framework, Executor* executor);

  enum State {
    RECOVERING,   // Slave is doing recovery.
    DISCONNECTED, // Slave is not connected to the master.
    RUNNING,      // Slave has (re-)registered.
    TERMINATING,  // Slave is shutting down.
  } state;

protected:
  virtual void initialize();
  virtual void finalize();
  virtual void exited(const UPID& pid);

  // This is called when recovery finishes.
  void _initialize(const Future<Nothing>& future);

  void fileAttached(const Future<Nothing>& result, const std::string& path);

  Nothing detachFile(const std::string& path);

  // Helper routine to lookup a framework.
  Framework* getFramework(const FrameworkID& frameworkId);

  // Returns an ExecutorInfo for a TaskInfo (possibly
  // constructing one if the task has a CommandInfo).
  ExecutorInfo getExecutorInfo(
      const FrameworkID& frameworkId,
      const TaskInfo& task);

  // Handle the second phase of shutting down an executor for those
  // executors that have not properly shutdown within a timeout.
  void shutdownExecutorTimeout(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const UUID& uuid);

  // Shuts down the executor if it did not register yet.
  void registerExecutorTimeout(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const UUID& uuid);

  // Cleans up all un-reregistered executors during recovery.
  void reregisterExecutorTimeout();

  // This function returns the max age of executor/slave directories allowed,
  // given a disk usage. This value could be used to tune gc.
  Duration age(double usage);

  // Checks the current disk usage and schedules for gc as necessary.
  void checkDiskUsage();

  // Reads the checkpointed data from a previous run and recovers state.
  // If 'reconnect' is true, the slave attempts to reconnect to any old
  // live executors. Otherwise, the slave attempts to shutdown/kill them.
  // If 'strict' is true, any recovery errors are considered fatal.
  Future<Nothing> recover(bool reconnect, bool strict);
  Future<Nothing> _recover(const state::SlaveState& state, bool reconnect);

  // Helper to recover a framework from the specified state.
  void recoverFramework(const state::FrameworkState& state);

  // Removes and garbage collects the executor.
  void removeExecutor(Framework* framework, Executor* executor);

  // Removes and garbage collects the framework.
  void removeFramework(Framework* framework);

private:
  // Inner class used to namespace HTTP route handlers (see
  // slave/http.cpp for implementations).
  class Http
  {
  public:
    Http(const Slave& _slave) : slave(_slave) {}

    // /slave/health
    process::Future<process::http::Response> health(
        const process::http::Request& request);

    // /slave/stats.json
    process::Future<process::http::Response> stats(
        const process::http::Request& request);

    // /slave/state.json
    process::Future<process::http::Response> state(
        const process::http::Request& request);

    static const std::string HEALTH_HELP;

  private:
    const Slave& slave;
  } http;

  friend class Framework;
  friend class Executor;

  Slave(const Slave&);              // No copying.
  Slave& operator = (const Slave&); // No assigning.

  const Flags flags;

  bool local;

  SlaveInfo info;

  UPID master;

  Resources resources;
  Attributes attributes;

  hashmap<FrameworkID, Framework*> frameworks;

  boost::circular_buffer<Owned<Framework> > completedFrameworks;

  Isolator* isolator;
  Files* files;

  // Statistics (initialized in Slave::initialize).
  struct {
    uint64_t tasks[TaskState_ARRAYSIZE];
    uint64_t validStatusUpdates;
    uint64_t invalidStatusUpdates;
    uint64_t validFrameworkMessages;
    uint64_t invalidFrameworkMessages;
  } stats;

  Time startTime;

  GarbageCollector gc;
  ResourceMonitor monitor;

  StatusUpdateManager* statusUpdateManager;

  // Flag to indicate if recovery, including reconciling (i.e., reconnect/kill)
  // with executors is finished.
  Promise<Nothing> recovered;

  // Root meta directory containing checkpointed data.
  const std::string metaDir;

  // Indicates the number of errors ignored in "--no-strict" recovery mode.
  unsigned int recoveryErrors;
};


// Information describing an executor.
struct Executor
{
  Executor(
      Slave* slave,
      const FrameworkID& frameworkId,
      const ExecutorInfo& info,
      const UUID& uuid,
      const std::string& directory,
      bool checkpoint);

  ~Executor();

  Task* addTask(const TaskInfo& task);
  void terminateTask(const TaskID& taskId, const mesos::TaskState& state);
  void completeTask(const TaskID& taskId);
  void checkpointTask(const TaskInfo& task);
  void recoverTask(const state::TaskState& state);
  void updateTaskState(const TaskID& taskId, TaskState state);

  // Returns true if there are any queued/launched/terminated tasks.
  bool incompleteTasks();

  enum State {
    REGISTERING,  // Executor is launched but not (re-)registered yet.
    RUNNING,      // Executor has (re-)registered.
    TERMINATING,  // Executor is being shutdown/killed.
    TERMINATED,   // Executor has terminated but there might be pending updates.
  } state;

  // We store the pointer to 'Slave' to get access to its methods
  // variables. One could imagine 'Executor' as being an inner class
  // of the 'Slave' class.
  Slave* slave;

  const ExecutorID id;
  const ExecutorInfo info;

  const FrameworkID frameworkId;

  const UUID uuid; // Distinguishes executor instances with same ExecutorID.

  const std::string directory;

  const bool checkpoint;

  const bool commandExecutor;

  UPID pid;

  Resources resources; // Currently consumed resources.

  LinkedHashMap<TaskID, TaskInfo> queuedTasks; // Not yet launched.
  LinkedHashMap<TaskID, Task*> launchedTasks;  // Running.
  LinkedHashMap<TaskID, Task*> terminatedTasks; // Terminated but pending updates.
  boost::circular_buffer<Task> completedTasks; // Terminated and updates acked.

private:
  Executor(const Executor&);              // No copying.
  Executor& operator = (const Executor&); // No assigning.
};


// Information about a framework.
struct Framework
{
  Framework(
      Slave* slave,
      const FrameworkID& id,
      const FrameworkInfo& info,
      const UPID& pid);

  ~Framework();

  Executor* launchExecutor(const ExecutorInfo& executorInfo);
  void destroyExecutor(const ExecutorID& executorId);
  Executor* getExecutor(const ExecutorID& executorId);
  Executor* getExecutor(const TaskID& taskId);
  Executor* recoverExecutor(const state::ExecutorState& state);

  enum State {
    RUNNING,      // First state of a newly created framework.
    TERMINATING,  // Framework is shutting down in the cluster.
  } state;

  // We store the pointer to 'Slave' to get access to its methods
  // variables. One could imagine 'Framework' as being an inner class
  // of the 'Slave' class.
  Slave* slave;

  const FrameworkID id;
  const FrameworkInfo info;

  UPID pid;

  multihashmap<ExecutorID, TaskID> pending; // Executors with pending tasks.

  // Current running executors.
  hashmap<ExecutorID, Executor*> executors;

  // Up to MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK completed executors.
  boost::circular_buffer<Owned<Executor> > completedExecutors;
private:
  Framework(const Framework&);              // No copying.
  Framework& operator = (const Framework&); // No assigning.
};


std::ostream& operator << (std::ostream& stream, Slave::State state);
std::ostream& operator << (std::ostream& stream, Framework::State state);
std::ostream& operator << (std::ostream& stream, Executor::State state);

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_HPP__
