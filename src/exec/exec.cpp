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

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/fatal.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
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
                  bool _checkpoint)
    : ProcessBase(ID::generate("executor")),
      slave(_slave),
      driver(_driver),
      executor(_executor),
      slaveId(_slaveId),
      frameworkId(_frameworkId),
      executorId(_executorId),
      connected(false),
      local(_local),
      aborted(false),
      directory(_directory),
      checkpoint(_checkpoint)
  {
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

    VLOG(1) << "Executor registered on slave " << slaveId;

    connected = true;
    executor->registered(driver, executorInfo, frameworkInfo, slaveInfo);
  }

  void reregistered(const SlaveID& slaveId, const SlaveInfo& slaveInfo)
  {
    if (aborted) {
      VLOG(1) << "Ignoring re-registered message from slave " << slaveId
              << " because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor re-registered on slave " << slaveId;

    executor->reregistered(driver, slaveInfo);
  }

  void reconnect(const SlaveID& slaveId)
  {
    if (aborted) {
      VLOG(1) << "Ignoring reconnect message from slave " << slaveId
              << " because the driver is aborted!";
      return;
    }

    VLOG(1) << "Received reconnect request from slave " << slaveId;

    // Update the slave link.
    slave = from;
    link(slave);

    // Re-register with slave.
    ReregisterExecutorMessage message;
    message.mutable_executor_id()->MergeFrom(executorId);
    message.mutable_framework_id()->MergeFrom(frameworkId);

    // Send all unacknowledged updates.
    foreachvalue (const StatusUpdate& update, updates) {
      message.add_updates()->MergeFrom(update);
    }

    // Send all unacknowledged tasks.
    foreachvalue (const TaskInfo& task, tasks) {
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

    executor->launchTask(driver, task);
  }

  void killTask(const TaskID& taskId)
  {
    if (aborted) {
      VLOG(1) << "Ignoring kill task message for task " << taskId
              <<" because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor asked to kill task '" << taskId << "'";

    executor->killTask(driver, taskId);
  }

  void statusUpdateAcknowledgement(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const string& uuid)
  {
    if (aborted) {
      VLOG(1) << "Ignoring status update acknowledgement " << uuid
              << " for task " << taskId
              << " of framework " << frameworkId
              << " because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor received status update acknowledgement " << uuid
            << " for task " << taskId
            << " of framework " << frameworkId;

    // Remove the corresponding update.
    updates.erase(uuid);

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

    executor->frameworkMessage(driver, data);
  }

  void shutdown()
  {
    if (aborted) {
      VLOG(1) << "Ignoring shutdown message because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor asked to shutdown";

    if (!local) {
      // Start the Shutdown Process.
      spawn(new ShutdownProcess(), true);
    }

    // TODO(benh): Any need to invoke driver.stop?
    executor->shutdown(driver);
    aborted = true; // To make sure not to accept any new messages.

    if (local) {
      terminate(this);
    }
  }

  void abort()
  {
    VLOG(1) << "De-activating the executor libprocess";
    aborted = true;
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
      VLOG(1) << "Slave exited, but framework has checkpointing enabled. "
              << "Waiting to reconnect with slave " << slaveId;
      return;
    }

    VLOG(1) << "Slave exited ... shutting down";

    if (!local) {
      // Start the Shutdown Process.
      spawn(new ShutdownProcess(), true);
    }

    // TODO: Pass an argument to shutdown to tell it this is abnormal?
    executor->shutdown(driver);
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
      VLOG(1) << "Executor is not allowed to send "
              << "TASK_STAGING status update. Aborting!";

      driver->abort();

      executor->error(driver, "Attempted to send TASK_STAGING status update");

      return;
    }

    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(frameworkId);
    update->mutable_executor_id()->MergeFrom(executorId);
    update->mutable_slave_id()->MergeFrom(slaveId);
    update->mutable_status()->MergeFrom(status);
    update->set_timestamp(Clock::now());
    update->set_uuid(UUID::random().toBytes());

    VLOG(1) << "Executor sending status update " << *update;

    // Capture the status update.
    updates[update->uuid()] = *update;

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
  bool local;
  bool aborted;
  const string directory;
  bool checkpoint;

  hashmap<string, StatusUpdate> updates; // Unacknowledged updates.

  // We store tasks that have not been acknowledged
  // (via status updates) by the slave. This ensures that, during
  // recovery, the slave relaunches only those tasks that have
  // never reached this executor.
  hashmap<TaskID, TaskInfo> tasks; // Unacknowledged tasks.
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
    fatal("cannot parse MESOS_SLAVE_PID");
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
      checkpoint);

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

  terminate(process);

  // TODO(benh): Set the condition variable in ExecutorProcess just as
  // we do with the MesosSchedulerDriver and SchedulerProcess:
  // dispatch(process, &ExecutorProcess::stop);

  pthread_cond_signal(&cond);

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

  // TODO(benh): Set the condition variable in ExecutorProcess just as
  // we do with the MesosSchedulerDriver and SchedulerProcess.

  dispatch(process, &ExecutorProcess::abort);

  pthread_cond_signal(&cond);

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
