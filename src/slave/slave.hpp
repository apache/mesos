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

#include <stdint.h>

#include <list>
#include <string>
#include <vector>

#include <boost/circular_buffer.hpp>

#include <mesos/resources.hpp>

#include <process/http.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/gauge.hpp>

#include <stout/bytes.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/multihashmap.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include "master/detector.hpp"

#include "slave/constants.hpp"
#include "slave/containerizer/containerizer.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
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

class MasterDetector; // Forward declaration.

namespace sasl {
class Authenticatee;
} // namespace sasl {

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
        MasterDetector* detector,
        Containerizer* containerizer,
        Files* files);

  virtual ~Slave();

  void shutdown(const process::UPID& from, const std::string& message);

  void registered(const process::UPID& from, const SlaveID& slaveId);
  void reregistered(const process::UPID& from, const SlaveID& slaveId);
  void doReliableRegistration(const Duration& duration);

  void runTask(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const FrameworkID& frameworkId,
      const std::string& pid,
      const TaskInfo& task);

  void _runTask(
      const process::Future<bool>& future,
      const FrameworkInfo& frameworkInfo,
      const FrameworkID& frameworkId,
      const std::string& pid,
      const TaskInfo& task);

  process::Future<bool> unschedule(const std::string& path);

  void killTask(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const TaskID& taskId);

  void shutdownFramework(
      const process::UPID& from,
      const FrameworkID& frameworkId);

  void schedulerMessage(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void updateFramework(const FrameworkID& frameworkId, const std::string& pid);

  void registerExecutor(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Called when an executor re-registers with a recovering slave.
  // 'tasks' : Unacknowledged tasks (i.e., tasks that the executor
  //           driver never received an ACK for.)
  // 'updates' : Unacknowledged updates.
  void reregisterExecutor(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::vector<TaskInfo>& tasks,
      const std::vector<StatusUpdate>& updates);

  void executorMessage(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void ping(const process::UPID& from, const std::string& body);

  // Handles the status update.
  // NOTE: If 'pid' is a valid UPID an ACK is sent to this pid
  // after the update is successfully handled. If pid == UPID()
  // no ACK is sent. The latter is used by the slave to send
  // status updates it generated (e.g., TASK_LOST).
  void statusUpdate(const StatusUpdate& update, const process::UPID& pid);

  // Continue handling the status update after optionally updating the
  // container's resources.
  void _statusUpdate(
      const Option<Future<Nothing> >& future,
      const StatusUpdate& update,
      const UPID& pid,
      const ExecutorID& executorId,
      const ContainerID& containerId,
      bool checkpoint);

  // This is called when the status update manager finishes
  // handling the update. If the handling is successful, an
  // acknowledgment is sent to the executor.
  void __statusUpdate(
      const process::Future<Nothing>& future,
      const StatusUpdate& update,
      const process::UPID& pid);

  void statusUpdateAcknowledgement(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const std::string& uuid);

  void _statusUpdateAcknowledgement(
      const process::Future<bool>& future,
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const UUID& uuid);

  void executorLaunched(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId,
      const process::Future<bool>& future);

  void executorTerminated(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const process::Future<containerizer::Termination>& termination);

  // NOTE: Pulled these to public to make it visible for testing.
  // TODO(vinod): Make tests friends to this class instead.

  // Garbage collects the directories based on the current disk usage.
  // TODO(vinod): Instead of making this function public, we need to
  // mock both GarbageCollector (and pass it through slave's constructor)
  // and os calls.
  void _checkDiskUsage(const process::Future<double>& usage);

  // Shut down an executor. This is a two phase process. First, an
  // executor receives a shut down message (shut down phase), then
  // after a configurable timeout the slave actually forces a kill
  // (kill phase, via the isolator) if the executor has not
  // exited.
  void shutdownExecutor(Framework* framework, Executor* executor);

  // Invoked whenever the detector detects a change in masters.
  // Made public for testing purposes.
  void detected(const process::Future<Option<MasterInfo> >& pid);

  enum State {
    RECOVERING,   // Slave is doing recovery.
    DISCONNECTED, // Slave is not connected to the master.
    RUNNING,      // Slave has (re-)registered.
    TERMINATING,  // Slave is shutting down.
  } state;

  // TODO(benh): Clang requires members to be public in order to take
  // their address which we do in tests (for things like
  // FUTURE_DISPATCH).
// protected:
  virtual void initialize();
  virtual void finalize();
  virtual void exited(const process::UPID& pid);

  void fileAttached(const process::Future<Nothing>& result,
                    const std::string& path);

  Nothing detachFile(const std::string& path);

  // Triggers a re-detection of the master when the slave does
  // not receive a ping.
  void pingTimeout(process::Future<Option<MasterInfo> > future);

  void authenticate();

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
      const ContainerID& containerId);

  // Shuts down the executor if it did not register yet.
  void registerExecutorTimeout(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId);

  // Cleans up all un-reregistered executors during recovery.
  void reregisterExecutorTimeout();

  // This function returns the max age of executor/slave directories allowed,
  // given a disk usage. This value could be used to tune gc.
  Duration age(double usage);

  // Checks the current disk usage and schedules for gc as necessary.
  void checkDiskUsage();

  // Recovers the slave, status update manager and isolator.
  process::Future<Nothing> recover(const Result<state::SlaveState>& state);

  // This is called after 'recover()'. If 'flags.reconnect' is
  // 'reconnect', the slave attempts to reconnect to any old live
  // executors. Otherwise, the slave attempts to shutdown/kill them.
  process::Future<Nothing> _recover();

  // This is a helper to call recover() on the containerizer at the end of
  // recover() and before __recover().
  // TODO(idownes): Remove this when we support defers to objects.
  process::Future<Nothing> _recoverContainerizer(
      const Option<state::SlaveState>& state);

  // This is called when recovery finishes.
  void __recover(const process::Future<Nothing>& future);

  // Helper to recover a framework from the specified state.
  void recoverFramework(const state::FrameworkState& state);

  // Removes and garbage collects the executor.
  void removeExecutor(Framework* framework, Executor* executor);

  // Removes and garbage collects the framework.
  void removeFramework(Framework* framework);

  // Schedules a 'path' for gc based on its modification time.
  Future<Nothing> garbageCollect(const std::string& path);

  // Called when the slave was signaled from the specified user.
  void signaled(int signal, int uid);

private:
  void _authenticate();
  void authenticationTimeout(process::Future<bool> future);

  // Inner class used to namespace HTTP route handlers (see
  // slave/http.cpp for implementations).
  class Http
  {
  public:
    explicit Http(Slave* _slave) : slave(_slave) {}

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
    Slave* slave;
  } http;

  friend struct Framework;
  friend struct Executor;

  Slave(const Slave&);              // No copying.
  Slave& operator = (const Slave&); // No assigning.

  // Gauge methods.
  double _frameworks_active()
  {
    return frameworks.size();
  }

  double _uptime_secs()
  {
    return (Clock::now() - startTime).secs();
  }

  double _registered()
  {
    return master.isSome() ? 1 : 0;
  }

  double _tasks_staging();
  double _tasks_starting();
  double _tasks_running();

  double _executors_registering();
  double _executors_running();
  double _executors_terminating();

  const Flags flags;

  SlaveInfo info;

  Option<process::UPID> master;

  Resources resources;
  Attributes attributes;

  hashmap<FrameworkID, Framework*> frameworks;

  boost::circular_buffer<process::Owned<Framework> > completedFrameworks;

  MasterDetector* detector;

  Containerizer* containerizer;

  Files* files;

  // Statistics (initialized in Slave::initialize).
  struct
  {
    uint64_t tasks[TaskState_ARRAYSIZE];
    uint64_t validStatusUpdates;
    uint64_t invalidStatusUpdates;
    uint64_t validFrameworkMessages;
    uint64_t invalidFrameworkMessages;
  } stats;

  struct Metrics
  {
    Metrics(const Slave& slave);

    ~Metrics();

    process::metrics::Gauge uptime_secs;
    process::metrics::Gauge registered;

    process::metrics::Counter recovery_errors;

    process::metrics::Gauge frameworks_active;

    process::metrics::Gauge tasks_staging;
    process::metrics::Gauge tasks_starting;
    process::metrics::Gauge tasks_running;
    process::metrics::Counter tasks_finished;
    process::metrics::Counter tasks_failed;
    process::metrics::Counter tasks_killed;
    process::metrics::Counter tasks_lost;

    process::metrics::Gauge executors_registering;
    process::metrics::Gauge executors_running;
    process::metrics::Gauge executors_terminating;
    process::metrics::Counter executors_terminated;

    process::metrics::Counter valid_status_updates;
    process::metrics::Counter invalid_status_updates;

    process::metrics::Counter valid_framework_messages;
    process::metrics::Counter invalid_framework_messages;
  } metrics;

  process::Time startTime;

  GarbageCollector gc;
  ResourceMonitor monitor;

  StatusUpdateManager* statusUpdateManager;

  // Master detection future.
  process::Future<Option<MasterInfo> > detection;

  // Timer for triggering re-detection when no ping is received from
  // the master.
  process::Timer pingTimer;

  // Flag to indicate if recovery, including reconciling (i.e., reconnect/kill)
  // with executors is finished.
  process::Promise<Nothing> recovered;

  // Root meta directory containing checkpointed data.
  const std::string metaDir;

  // Indicates the number of errors ignored in "--no-strict" recovery mode.
  unsigned int recoveryErrors;

  Option<Credential> credential;

  sasl::Authenticatee* authenticatee;

  // Indicates if an authentication attempt is in progress.
  Option<Future<bool> > authenticating;

  // Indicates if the authentication is successful.
  bool authenticated;

  // Indicates if a new authentication attempt should be enforced.
  bool reauthenticate;
};


// Information describing an executor.
struct Executor
{
  Executor(
      Slave* slave,
      const FrameworkID& frameworkId,
      const ExecutorInfo& info,
      const ContainerID& containerId,
      const std::string& directory,
      bool checkpoint);

  ~Executor();

  Task* addTask(const TaskInfo& task);
  void terminateTask(const TaskID& taskId, const mesos::TaskState& state);
  void completeTask(const TaskID& taskId);
  void checkpointTask(const TaskInfo& task);
  void recoverTask(const state::TaskState& state);
  void updateTaskState(const TaskStatus& status);

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

  const ContainerID containerId;

  const std::string directory;

  const bool checkpoint;

  const bool commandExecutor;

  process::UPID pid;

  // Currently consumed resources. It is an option type as the
  // executor info will not be known up-front and the executor
  // resources therefore cannot be known until after the containerizer
  // has launched the container.
  Option<Resources> resources;

  // Tasks can be found in one of the following four data structures:

  // Not yet launched.
  LinkedHashMap<TaskID, TaskInfo> queuedTasks;

  // Running.
  LinkedHashMap<TaskID, Task*> launchedTasks;

  // Terminated but pending updates.
  LinkedHashMap<TaskID, Task*> terminatedTasks;

  // Terminated and updates acked.
  // NOTE: We use a shared pointer for Task because clang doesn't like
  // Boost's implementation of circular_buffer with Task (Boost
  // attempts to do some memset's which are unsafe).
  boost::circular_buffer<memory::shared_ptr<Task> > completedTasks;

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
      const process::UPID& pid);

  ~Framework();

  Executor* launchExecutor(
      const ExecutorInfo& executorInfo,
      const TaskInfo& taskInfo);
  void destroyExecutor(const ExecutorID& executorId);
  Executor* getExecutor(const ExecutorID& executorId);
  Executor* getExecutor(const TaskID& taskId);
  void recoverExecutor(const state::ExecutorState& state);

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
  boost::circular_buffer<process::Owned<Executor> > completedExecutors;
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
