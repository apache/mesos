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

#include <signal.h>

#include <sys/types.h>

#include <iostream>
#include <string>
#include <sstream>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/fatal.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include "common/lock.hpp"
#include "common/protobuf_utils.hpp"
#include "common/type_utils.hpp"

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"
#include "slave/state.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using namespace process;

using std::string;

using process::wait; // Necessary on some OS's to disambiguate.


namespace mesos {
namespace internal {

class ShutdownProcess : public Process<ShutdownProcess>
{
protected:
  virtual void initialize()
  {
    VLOG(1) << "Scheduling shutdown of the executor";
    // TODO(benh): Pass the shutdown timeout with ExecutorRegistered
    // since it might have gotten configured on the command line.
    delay(slave::EXECUTOR_SHUTDOWN_GRACE_PERIOD, self(), &Self::kill);
  }

  void kill()
  {
    VLOG(1) << "Committing suicide by killing the process group";

    // TODO(vinod): Invoke killtree without killing ourselves.
    // Kill the process group (including ourself).
    killpg(0, SIGKILL);

    // The signal might not get delivered immediately, so sleep for a
    // few seconds. Worst case scenario, exit abnormally.
    os::sleep(Seconds(5));
    exit(-1);
  }
};


class ExecutorProcess : public ProtobufProcess<ExecutorProcess>
{
public:
  ExecutorProcess(const UPID& _slave,
                  MesosExecutorDriver* _driver,
                  Executor* _executor,
                  const SlaveID& _slaveId,
                  const FrameworkID& _frameworkId,
                  const ExecutorID& _executorId,
                  bool _local,
                  const string& _directory,
                  bool _checkpoint,
                  Duration _recoveryTimeout,
                  pthread_mutex_t* _mutex,
                  pthread_cond_t* _cond)
    : ProcessBase(ID::generate("executor")),
      slave(_slave),
      driver(_driver),
      executor(_executor),
      slaveId(_slaveId),
      frameworkId(_frameworkId),
      executorId(_executorId),
      connected(false),
      connection(UUID::random()),
      local(_local),
      aborted(false),
      mutex(_mutex),
      cond(_cond),
      directory(_directory),
      checkpoint(_checkpoint),
      recoveryTimeout(_recoveryTimeout)
  {
    LOG(INFO) << "Version: " << MESOS_VERSION;

    install<ExecutorRegisteredMessage>(
        &ExecutorProcess::registered,
        &ExecutorRegisteredMessage::executor_info,
        &ExecutorRegisteredMessage::framework_id,
        &ExecutorRegisteredMessage::framework_info,
        &ExecutorRegisteredMessage::slave_id,
        &ExecutorRegisteredMessage::slave_info);

    install<ExecutorReregisteredMessage>(
        &ExecutorProcess::reregistered,
        &ExecutorReregisteredMessage::slave_id,
        &ExecutorReregisteredMessage::slave_info);

    install<ReconnectExecutorMessage>(
        &ExecutorProcess::reconnect,
        &ReconnectExecutorMessage::slave_id);

    install<RunTaskMessage>(
        &ExecutorProcess::runTask,
        &RunTaskMessage::task);

    install<KillTaskMessage>(
        &ExecutorProcess::killTask,
        &KillTaskMessage::task_id);

    install<StatusUpdateAcknowledgementMessage>(
        &ExecutorProcess::statusUpdateAcknowledgement,
        &StatusUpdateAcknowledgementMessage::slave_id,
        &StatusUpdateAcknowledgementMessage::framework_id,
        &StatusUpdateAcknowledgementMessage::task_id,
        &StatusUpdateAcknowledgementMessage::uuid);

    install<FrameworkToExecutorMessage>(
        &ExecutorProcess::frameworkMessage,
        &FrameworkToExecutorMessage::slave_id,
        &FrameworkToExecutorMessage::framework_id,
        &FrameworkToExecutorMessage::executor_id,
        &FrameworkToExecutorMessage::data);

    install<ShutdownExecutorMessage>(
        &ExecutorProcess::shutdown);
  }

  virtual ~ExecutorProcess() {}

protected:
  virtual void initialize()
  {
    VLOG(1) << "Executor started at: " << self()
            << " with pid " << getpid();

    link(slave);

    // Register with slave.
    RegisterExecutorMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    send(slave, message);
  }

  void registered(const ExecutorInfo& executorInfo,
                  const FrameworkID& frameworkId,
                  const FrameworkInfo& frameworkInfo,
                  const SlaveID& slaveId,
                  const SlaveInfo& slaveInfo)
  {
    if (aborted) {
      VLOG(1) << "Ignoring registered message from slave " << slaveId
              << " because the driver is aborted!";
      return;
    }

    LOG(INFO) << "Executor registered on slave " << slaveId;

    connected = true;
    connection = UUID::random();

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->registered(driver, executorInfo, frameworkInfo, slaveInfo);

    VLOG(1) << "Executor::registered took " << stopwatch.elapsed();
  }

  void reregistered(const SlaveID& slaveId, const SlaveInfo& slaveInfo)
  {
    if (aborted) {
      VLOG(1) << "Ignoring re-registered message from slave " << slaveId
              << " because the driver is aborted!";
      return;
    }

    LOG(INFO) << "Executor re-registered on slave " << slaveId;

    connected = true;
    connection = UUID::random();

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->reregistered(driver, slaveInfo);

    VLOG(1) << "Executor::reregistered took " << stopwatch.elapsed();
  }

  void reconnect(const UPID& from, const SlaveID& slaveId)
  {
    if (aborted) {
      VLOG(1) << "Ignoring reconnect message from slave " << slaveId
              << " because the driver is aborted!";
      return;
    }

    LOG(INFO) << "Received reconnect request from slave " << slaveId;

    // Update the slave link.
    slave = from;
    link(slave);

    // Re-register with slave.
    ReregisterExecutorMessage message;
    message.mutable_executor_id()->MergeFrom(executorId);
    message.mutable_framework_id()->MergeFrom(frameworkId);

    // Send all unacknowledged updates.
    // TODO(vinod): Use foreachvalue instead once LinkedHashmap
    // supports it.
    foreach (const StatusUpdate& update, updates.values()) {
      message.add_updates()->MergeFrom(update);
    }

    // Send all unacknowledged tasks.
    // TODO(vinod): Use foreachvalue instead once LinkedHashmap
    // supports it.
    foreach (const TaskInfo& task, tasks.values()) {
      message.add_tasks()->MergeFrom(task);
    }

    send(slave, message);
  }

  void runTask(const TaskInfo& task)
  {
    if (aborted) {
      VLOG(1) << "Ignoring run task message for task " << task.task_id()
              << " because the driver is aborted!";
      return;
    }

    CHECK(!tasks.contains(task.task_id()))
      << "Unexpected duplicate task " << task.task_id();

    tasks[task.task_id()] = task;

    VLOG(1) << "Executor asked to run task '" << task.task_id() << "'";

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->launchTask(driver, task);

    VLOG(1) << "Executor::launchTask took " << stopwatch.elapsed();
  }

  void killTask(const TaskID& taskId)
  {
    if (aborted) {
      VLOG(1) << "Ignoring kill task message for task " << taskId
              <<" because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor asked to kill task '" << taskId << "'";

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->killTask(driver, taskId);

    VLOG(1) << "Executor::killTask took " << stopwatch.elapsed();
  }

  void statusUpdateAcknowledgement(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const string& uuid)
  {
    if (aborted) {
      VLOG(1) << "Ignoring status update acknowledgement "
              << UUID::fromBytes(uuid) << " for task " << taskId
              << " of framework " << frameworkId
              << " because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor received status update acknowledgement "
            << UUID::fromBytes(uuid) << " for task " << taskId
            << " of framework " << frameworkId;

    // Remove the corresponding update.
    updates.erase(UUID::fromBytes(uuid));

    // Remove the corresponding task.
    tasks.erase(taskId);
  }

  void frameworkMessage(const SlaveID& slaveId,
                        const FrameworkID& frameworkId,
                        const ExecutorID& executorId,
                        const string& data)
  {
    if (aborted) {
      VLOG(1) << "Ignoring framework message because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor received framework message";

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->frameworkMessage(driver, data);

    VLOG(1) << "Executor::frameworkMessage took " << stopwatch.elapsed();
  }

  void shutdown()
  {
    if (aborted) {
      VLOG(1) << "Ignoring shutdown message because the driver is aborted!";
      return;
    }

    LOG(INFO) << "Executor asked to shutdown";

    if (!local) {
      // Start the Shutdown Process.
      spawn(new ShutdownProcess(), true);
    }

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    // TODO(benh): Any need to invoke driver.stop?
    executor->shutdown(driver);

    VLOG(1) << "Executor::shutdown took " << stopwatch.elapsed();

    aborted = true; // To make sure not to accept any new messages.

    if (local) {
      terminate(this);
    }
  }

  void stop()
  {
    terminate(self());

    Lock lock(mutex);
    pthread_cond_signal(cond);
  }

  void abort()
  {
    LOG(INFO) << "Deactivating the executor libprocess";
    CHECK(aborted);

    Lock lock(mutex);
    pthread_cond_signal(cond);
  }

  void _recoveryTimeout(UUID _connection)
  {
    // If we're connected, no need to shut down the driver!
    if (connected) {
      return;
    }

    // We need to compare the connections here to ensure there have
    // not been any subsequent re-registrations with the slave in the
    // interim.
    if (connection == _connection) {
      LOG(INFO) << "Recovery timeout of " << recoveryTimeout << " exceeded; "
                << "Shutting down";
      shutdown();
    }
  }

  virtual void exited(const UPID& pid)
  {
    if (aborted) {
      VLOG(1) << "Ignoring exited event because the driver is aborted!";
      return;
    }

    // If the framework has checkpointing enabled and the executor has
    // successfully registered with the slave, the slave can reconnect with
    // this executor when it comes back up and performs recovery!
    if (checkpoint && connected) {
      connected = false;

      LOG(INFO) << "Slave exited, but framework has checkpointing enabled. "
                << "Waiting " << recoveryTimeout << " to reconnect with slave "
                << slaveId;

      delay(recoveryTimeout, self(), &Self::_recoveryTimeout, connection);

      return;
    }

    LOG(INFO) << "Slave exited ... shutting down";

    connected = false;

    if (!local) {
      // Start the Shutdown Process.
      spawn(new ShutdownProcess(), true);
    }

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    // TODO: Pass an argument to shutdown to tell it this is abnormal?
    executor->shutdown(driver);

    VLOG(1) << "Executor::shutdown took " << stopwatch.elapsed();

    aborted = true; // To make sure not to accept any new messages.

    // This is a pretty bad state ... no slave is left. Rather
    // than exit lets kill our process group (which includes
    // ourself) hoping to clean up any processes this executor
    // launched itself.
    // TODO(benh): Maybe do a SIGTERM and then later do a SIGKILL?
    if (local) {
      terminate(this);
    }
  }

  void sendStatusUpdate(const TaskStatus& status)
  {
    if (status.state() == TASK_STAGING) {
      LOG(ERROR) << "Executor is not allowed to send "
                 << "TASK_STAGING status update. Aborting!";

      driver->abort();

      Stopwatch stopwatch;
      if (FLAGS_v >= 1) {
        stopwatch.start();
      }

      executor->error(driver, "Attempted to send TASK_STAGING status update");

      VLOG(1) << "Executor::error took " << stopwatch.elapsed();

      return;
    }

    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(frameworkId);
    update->mutable_executor_id()->MergeFrom(executorId);
    update->mutable_slave_id()->MergeFrom(slaveId);
    update->mutable_status()->MergeFrom(status);
    update->set_timestamp(Clock::now().secs());
    update->mutable_status()->set_timestamp(update->timestamp());
    update->set_uuid(UUID::random().toBytes());
    message.set_pid(self());

    // Incoming status update might come from an executor which has not set
    // slave id in TaskStatus. Set/overwrite slave id.
    update->mutable_status()->mutable_slave_id()->CopyFrom(slaveId);;

    VLOG(1) << "Executor sending status update " << *update;

    // Capture the status update.
    updates[UUID::fromBytes(update->uuid())] = *update;

    send(slave, message);
  }

  void sendFrameworkMessage(const string& data)
  {
    ExecutorToFrameworkMessage message;
    message.mutable_slave_id()->MergeFrom(slaveId);
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    message.set_data(data);
    send(slave, message);
  }

private:
  friend class mesos::MesosExecutorDriver;

  UPID slave;
  MesosExecutorDriver* driver;
  Executor* executor;
  SlaveID slaveId;
  FrameworkID frameworkId;
  ExecutorID executorId;
  bool connected; // Registered with the slave.
  UUID connection; // UUID to identify the connection instance.
  bool local;
  volatile bool aborted;
  pthread_mutex_t* mutex;
  pthread_cond_t* cond;
  const string directory;
  bool checkpoint;
  Duration recoveryTimeout;

  LinkedHashMap<UUID, StatusUpdate> updates; // Unacknowledged updates.

  // We store tasks that have not been acknowledged
  // (via status updates) by the slave. This ensures that, during
  // recovery, the slave relaunches only those tasks that have
  // never reached this executor.
  LinkedHashMap<TaskID, TaskInfo> tasks; // Unacknowledged tasks.
};

} // namespace internal {
} // namespace mesos {


// Implementation of C++ API.


MesosExecutorDriver::MesosExecutorDriver(Executor* _executor)
  : executor(_executor),
    process(NULL),
    status(DRIVER_NOT_STARTED)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Create mutex and condition variable
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&cond, 0);

  // Initialize libprocess.
  process::initialize();

  // TODO(benh): Initialize glog.
}


MesosExecutorDriver::~MesosExecutorDriver()
{
  // Just as in SchedulerProcess, we might wait here indefinitely if
  // MesosExecutorDriver::stop has not been invoked.
  wait(process);
  delete process;

  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);
}


Status MesosExecutorDriver::start()
{
  Lock lock(&mutex);

  if (status != DRIVER_NOT_STARTED) {
    return status;
  }

  // Set stream buffering mode to flush on newlines so that we capture logs
  // from user processes even when output is redirected to a file.
  setvbuf(stdout, 0, _IOLBF, 0);
  setvbuf(stderr, 0, _IOLBF, 0);

  bool local;

  UPID slave;
  SlaveID slaveId;
  FrameworkID frameworkId;
  ExecutorID executorId;
  string workDirectory;
  bool checkpoint;

  string value;
  std::istringstream iss;

  // Check if this is local (for example, for testing).
  value = os::getenv("MESOS_LOCAL", false);

  if (!value.empty()) {
    local = true;
  } else {
    local = false;
  }

  // Get slave PID from environment.
  value = os::getenv("MESOS_SLAVE_PID");
  slave = UPID(value);
  if (!slave) {
    fatal("Cannot parse MESOS_SLAVE_PID '%s'", value.c_str());
  }

  // Get slave ID from environment.
  value = os::getenv("MESOS_SLAVE_ID");
  slaveId.set_value(value);

  // Get framework ID from environment.
  value = os::getenv("MESOS_FRAMEWORK_ID");
  frameworkId.set_value(value);

  // Get executor ID from environment.
  value = os::getenv("MESOS_EXECUTOR_ID");
  executorId.set_value(value);

  // Get working directory from environment.
  value = os::getenv("MESOS_DIRECTORY");
  workDirectory = value;

  // Get checkpointing status from environment.
  value = os::getenv("MESOS_CHECKPOINT", false);

  if (!value.empty()) {
    checkpoint = value == "1";
  } else {
    checkpoint = false;
  }

  Duration recoveryTimeout = slave::RECOVERY_TIMEOUT;

  // Get the recovery timeout if checkpointing is enabled.
  if (checkpoint) {
    value = os::getenv("MESOS_RECOVERY_TIMEOUT", false);

    if (!value.empty()) {
      Try<Duration> _recoveryTimeout = Duration::parse(value);

      if (_recoveryTimeout.isError()) {
        fatal("Cannot parse MESOS_RECOVERY_TIMEOUT '%s': %s",
              value.c_str(),
              _recoveryTimeout.error().c_str());
      }

      recoveryTimeout = _recoveryTimeout.get();
    }
  }

  CHECK(process == NULL);

  process = new ExecutorProcess(
      slave,
      this,
      executor,
      slaveId,
      frameworkId,
      executorId,
      local,
      workDirectory,
      checkpoint,
      recoveryTimeout,
      &mutex,
      &cond);

  spawn(process);

  return status = DRIVER_RUNNING;
}


Status MesosExecutorDriver::stop()
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING && status != DRIVER_ABORTED) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &ExecutorProcess::stop);

  bool aborted = status == DRIVER_ABORTED;

  status = DRIVER_STOPPED;

  return aborted ? DRIVER_ABORTED : status;
}


Status MesosExecutorDriver::abort()
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  // We set the volatile aborted to true here to prevent any further
  // messages from being processed in the ExecutorProcess. However,
  // if abort() is called from another thread as the ExecutorProcess,
  // there may be at most one additional message processed.
  // TODO(bmahler): Use an atomic boolean.
  process->aborted = true;

  // Dispatching here ensures that we still process the outstanding
  // requests *from* the executor, since those do proceed when
  // aborted is true.
  dispatch(process, &ExecutorProcess::abort);

  return status = DRIVER_ABORTED;
}


Status MesosExecutorDriver::join()
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  while (status == DRIVER_RUNNING) {
    pthread_cond_wait(&cond, &mutex);
  }

  CHECK(status == DRIVER_ABORTED || status == DRIVER_STOPPED);

  return status;
}


Status MesosExecutorDriver::run()
{
  Status status = start();
  return status != DRIVER_RUNNING ? status : join();
}


Status MesosExecutorDriver::sendStatusUpdate(const TaskStatus& taskStatus)
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &ExecutorProcess::sendStatusUpdate, taskStatus);

  return status;
}


Status MesosExecutorDriver::sendFrameworkMessage(const string& data)
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &ExecutorProcess::sendFrameworkMessage, data);

  return status;
}
