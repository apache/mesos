// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <signal.h>

#include <sys/types.h>

#include <atomic>
#include <iostream>
#include <string>
#include <sstream>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/latch.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/stringify.hpp>
#include <stout/synchronized.hpp>
#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "docker/executor.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"
#include "slave/state.hpp"

#include "version/version.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using namespace process;

using std::string;

using process::Latch;
using process::wait; // Necessary on some OS's to disambiguate.

using mesos::Executor; // Necessary on some OS's to disambiguate.

namespace mesos {
namespace internal {

// The ShutdownProcess is a relic of the pre-cgroup process isolation
// days. It ensures that the executor process tree is killed after a
// shutdown has been sent.
//
// TODO(bmahler): Update 'delay' to handle deferred callbacks without
// needing a Process. This would eliminate the need for an explicit
// Process here, see: MESOS-4729.
class ShutdownProcess : public Process<ShutdownProcess>
{
public:
  explicit ShutdownProcess(const Duration& _gracePeriod)
    : ProcessBase(ID::generate("exec-shutdown")),
      gracePeriod(_gracePeriod) {}

protected:
  void initialize() override
  {
    VLOG(1) << "Scheduling shutdown of the executor in " << gracePeriod;

    delay(gracePeriod, self(), &Self::kill);
  }

  void kill()
  {
    VLOG(1) << "Committing suicide by killing the process group";

    // TODO(vinod): Invoke killtree without killing ourselves.
    // Kill the process group (including ourself).
#ifndef __WINDOWS__
    killpg(0, SIGKILL);
#else
    LOG(WARNING) << "Shutting down process group. Windows does not support "
                    "`killpg`, so we simply call `exit` on the assumption "
                    "that the process was generated with the "
                    "`WindowsContainerizer`, which uses the 'close on exit' "
                    "feature of job objects to make sure all child processes "
                    "are killed when a parent process exits";
    exit(0);
#endif // __WINDOWS__

    // The signal might not get delivered immediately, so sleep for a
    // few seconds. Worst case scenario, exit abnormally.
    os::sleep(Seconds(5));
    exit(EXIT_FAILURE);
  }

private:
  const Duration gracePeriod;
};


class ExecutorProcess : public ProtobufProcess<ExecutorProcess>
{
public:
  ExecutorProcess(
      const UPID& _slave,
      MesosExecutorDriver* _driver,
      Executor* _executor,
      const SlaveID& _slaveId,
      const FrameworkID& _frameworkId,
      const ExecutorID& _executorId,
      bool _local,
      const string& _directory,
      bool _checkpoint,
      const Duration& _recoveryTimeout,
      const Duration& _shutdownGracePeriod,
      std::recursive_mutex* _mutex,
      Latch* _latch)
    : ProcessBase(ID::generate("executor")),
      slave(_slave),
      driver(_driver),
      executor(_executor),
      slaveId(_slaveId),
      frameworkId(_frameworkId),
      executorId(_executorId),
      connected(false),
      connection(id::UUID::random()),
      local(_local),
      aborted(false),
      mutex(_mutex),
      latch(_latch),
      directory(_directory),
      checkpoint(_checkpoint),
      recoveryTimeout(_recoveryTimeout),
      shutdownGracePeriod(_shutdownGracePeriod)
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
        &ExecutorProcess::killTask);

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

  ~ExecutorProcess() override {}

protected:
  void initialize() override
  {
    VLOG(1) << "Executor started at: " << self() << " with pid " << getpid();

    link(slave);

    // Register with slave.
    RegisterExecutorMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    send(slave, message);
  }

  void registered(
      const ExecutorInfo& executorInfo,
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const SlaveID& slaveId,
      const SlaveInfo& slaveInfo)
  {
    if (aborted.load()) {
      VLOG(1) << "Ignoring registered message from agent " << slaveId
              << " because the driver is aborted!";
      return;
    }

    LOG(INFO) << "Executor registered on agent " << slaveId;

    connected = true;
    connection = id::UUID::random();

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->registered(driver, executorInfo, frameworkInfo, slaveInfo);

    VLOG(1) << "Executor::registered took " << stopwatch.elapsed();
  }

  void reregistered(const SlaveID& slaveId, const SlaveInfo& slaveInfo)
  {
    if (aborted.load()) {
      VLOG(1) << "Ignoring reregistered message from agent " << slaveId
              << " because the driver is aborted!";
      return;
    }

    LOG(INFO) << "Executor reregistered on agent " << slaveId;

    connected = true;
    connection = id::UUID::random();

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->reregistered(driver, slaveInfo);

    VLOG(1) << "Executor::reregistered took " << stopwatch.elapsed();
  }

  void reconnect(const UPID& from, const SlaveID& slaveId)
  {
    if (aborted.load()) {
      VLOG(1) << "Ignoring reconnect message from agent " << slaveId
              << " because the driver is aborted!";
      return;
    }

    LOG(INFO) << "Received reconnect request from agent " << slaveId;

    // Update the slave link.
    slave = from;

    // We force a reconnect here to avoid sending on a stale "half-open"
    // socket. We do not detect a disconnection in some cases when the
    // connection is terminated by a netfilter module e.g., iptables
    // running on the agent (see MESOS-5332).
    link(slave, RemoteConnection::RECONNECT);

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
    if (aborted.load()) {
      VLOG(1) << "Ignoring run task message for task " << task.task_id()
              << " because the driver is aborted!";
      return;
    }

    if (!connected) {
      LOG(WARNING) << "Ignoring run task message for task " << task.task_id()
                   << " because the driver is disconnected!";
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

  void killTask(KillTaskMessage&& killTaskMessage)
  {
    const TaskID taskId = killTaskMessage.task_id();

    if (aborted.load()) {
      VLOG(1) << "Ignoring kill task message for task " << taskId
              << " because the driver is aborted!";
      return;
    }

    // A kill task request is received when the driver is not connected. This
    // can happen, for example, if `ExecutorRegisteredMessage` has not been
    // delivered. We do not shutdown the driver because there might be other
    // still running tasks and the executor might eventually reconnect, e.g.,
    // after the agent failover. We do not drop ignore the message because the
    // actual executor may still want to react, e.g., commit suicide.
    if (!connected) {
      LOG(WARNING) << "Executor received kill task message for task " << taskId
                   << " while disconnected from the agent!";
    }

    VLOG(1) << "Executor asked to kill task '" << taskId << "'";

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    // If this is a Docker executor, call the `killTask()` overload which
    // allows the kill policy to be overridden.
    auto* dockerExecutor = dynamic_cast<docker::DockerExecutor*>(executor);
    if (dockerExecutor) {
      Option<KillPolicy> killPolicy = killTaskMessage.has_kill_policy()
        ? killTaskMessage.kill_policy()
        : Option<KillPolicy>::none();

      dockerExecutor->killTask(driver, taskId, killPolicy);
    } else {
      executor->killTask(driver, taskId);
    }

    VLOG(1) << "Executor::killTask took " << stopwatch.elapsed();
  }

  void statusUpdateAcknowledgement(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const string& uuid)
  {
    Try<id::UUID> uuid_ = id::UUID::fromBytes(uuid);
    CHECK_SOME(uuid_);

    if (aborted.load()) {
      VLOG(1) << "Ignoring status update acknowledgement "
              << uuid_.get() << " for task " << taskId
              << " of framework " << frameworkId
              << " because the driver is aborted!";
      return;
    }

    if (!connected) {
      LOG(WARNING) << "Ignoring status update acknowledgement "
                   << uuid_.get() << " for task " << taskId
                   << " of framework " << frameworkId
                   << " because the driver is disconnected!";
      return;
    }

    if (!updates.contains(uuid_.get())) {
      LOG(WARNING) << "Ignoring unknown status update acknowledgement "
                   << uuid_.get() << " for task " << taskId
                   << " of framework " << frameworkId;
      return;
    }

    VLOG(1) << "Executor received status update acknowledgement "
            << uuid_.get() << " for task " << taskId
            << " of framework " << frameworkId;

    // If this is a terminal status update acknowledgment for the Docker
    // executor, stop the driver to terminate the executor.
    //
    // TODO(abudnik): This is a workaround for MESOS-9847. A better solution
    // is to update supported API for the Docker executor from V0 to V1. It
    // will allow the executor to handle status update acknowledgments itself.
    if (mesos::internal::protobuf::isTerminalState(
            updates[uuid_.get()].status().state()) &&
        dynamic_cast<docker::DockerExecutor*>(executor)) {
      driver->stop();
    }

    // Remove the corresponding update.
    updates.erase(uuid_.get());

    // Remove the corresponding task.
    tasks.erase(taskId);
  }

  void frameworkMessage(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const string& data)
  {
    if (aborted.load()) {
      VLOG(1) << "Ignoring framework message because the driver is aborted!";
      return;
    }

    if (!connected) {
      LOG(WARNING) << "Ignoring framework message because"
                   << " the driver is disconnected!";
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
    if (aborted.load()) {
      VLOG(1) << "Ignoring shutdown message because the driver is aborted!";
      return;
    }

    LOG(INFO) << "Executor asked to shutdown";

    if (!local) {
      // Start the Shutdown Process.
      spawn(new ShutdownProcess(shutdownGracePeriod), true);
    }

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    // TODO(benh): Any need to invoke driver.stop?
    executor->shutdown(driver);

    VLOG(1) << "Executor::shutdown took " << stopwatch.elapsed();

    aborted.store(true); // To make sure not to accept any new messages.

    if (local) {
      terminate(this);
    }
  }

  void stop()
  {
    terminate(self());

    synchronized (mutex) {
      CHECK_NOTNULL(latch)->trigger();
    }
  }

  void abort()
  {
    LOG(INFO) << "Deactivating the executor libprocess";
    CHECK(aborted.load());

    synchronized (mutex) {
      CHECK_NOTNULL(latch)->trigger();
    }
  }

  void _recoveryTimeout(id::UUID _connection)
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

  void exited(const UPID& pid) override
  {
    if (aborted.load()) {
      VLOG(1) << "Ignoring exited event because the driver is aborted!";
      return;
    }

    // If the framework has checkpointing enabled and the executor has
    // successfully registered with the slave, the slave can reconnect with
    // this executor when it comes back up and performs recovery!
    if (checkpoint && connected) {
      connected = false;

      LOG(INFO) << "Agent exited, but framework has checkpointing enabled. "
                << "Waiting " << recoveryTimeout << " to reconnect with agent "
                << slaveId;

      delay(recoveryTimeout, self(), &Self::_recoveryTimeout, connection);

      return;
    }

    LOG(INFO) << "Agent exited ... shutting down";

    connected = false;

    if (!local) {
      // Start the Shutdown Process.
      spawn(new ShutdownProcess(shutdownGracePeriod), true);
    }

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    // TODO(benh): Pass an argument to shutdown to tell it this is abnormal?
    executor->shutdown(driver);

    VLOG(1) << "Executor::shutdown took " << stopwatch.elapsed();

    aborted.store(true); // To make sure not to accept any new messages.

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
    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(frameworkId);
    update->mutable_executor_id()->MergeFrom(executorId);
    update->mutable_slave_id()->MergeFrom(slaveId);
    update->mutable_status()->MergeFrom(status);
    update->set_timestamp(Clock::now().secs());
    update->mutable_status()->set_timestamp(update->timestamp());
    message.set_pid(self());

    // We overwrite the UUID for this status update, however with
    // the HTTP API, the executor will have to generate a UUID
    // (which needs to be validated to be RFC-4122 compliant).
    id::UUID uuid = id::UUID::random();
    update->set_uuid(uuid.toBytes());
    update->mutable_status()->set_uuid(uuid.toBytes());

    // We overwrite the SlaveID for this status update, however with
    // the HTTP API, this can be overwritten by the slave instead.
    update->mutable_status()->mutable_slave_id()->CopyFrom(slaveId);

    VLOG(1) << "Executor sending status update " << *update;

    // Capture the status update.
    updates[uuid] = *update;

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
  id::UUID connection; // UUID to identify the connection instance.
  bool local;
  std::atomic_bool aborted;
  std::recursive_mutex* mutex;
  Latch* latch;
  const string directory;
  bool checkpoint;
  Duration recoveryTimeout;
  Duration shutdownGracePeriod;

  LinkedHashMap<id::UUID, StatusUpdate> updates; // Unacknowledged updates.

  // We store tasks that have not been acknowledged
  // (via status updates) by the slave. This ensures that, during
  // recovery, the slave relaunches only those tasks that have
  // never reached this executor.
  LinkedHashMap<TaskID, TaskInfo> tasks; // Unacknowledged tasks.
};

} // namespace internal {
} // namespace mesos {


// Implementation of C++ API.


MesosExecutorDriver::MesosExecutorDriver(mesos::Executor* _executor)
  : MesosExecutorDriver(_executor, os::environment())
{}


MesosExecutorDriver::MesosExecutorDriver(
    mesos::Executor* _executor,
    const std::map<std::string, std::string>& _environment)
  : executor(_executor),
    process(nullptr),
    latch(nullptr),
    status(DRIVER_NOT_STARTED),
    environment(_environment)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Load any logging flags from the environment.
  logging::Flags flags;

  // Filter out environment variables whose keys don't start with "MESOS_".
  //
  // TODO(alexr): This should be supported by `FlagsBase`, see MESOS-9001.
  std::map<std::string, std::string> env;

  foreachpair (const string& key, const string& value, environment) {
    if (strings::startsWith(key, "MESOS_")) {
      env.emplace(key, value);
    }
  }

  Try<flags::Warnings> load = flags.load(env, true);

  if (load.isError()) {
    status = DRIVER_ABORTED;
    executor->error(this, load.error());
    return;
  }

  // Initialize libprocess.
  process::initialize();

  // Initialize Latch.
  latch = new Latch();

  // Initialize logging.
  if (flags.initialize_driver_logging) {
    logging::initialize("mesos", false, flags);
  } else {
    VLOG(1) << "Disabling initialization of GLOG logging";
  }

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  spawn(new VersionProcess(), true);
}


MesosExecutorDriver::~MesosExecutorDriver()
{
  // Just like with the MesosSchedulerDriver it's possible to get a
  // deadlock here. Otherwise we terminate the ExecutorProcess and
  // wait for it before deleting.
  terminate(process);
  wait(process);
  delete process;

  delete latch;
}


Status MesosExecutorDriver::start()
{
  synchronized (mutex) {
    if (status != DRIVER_NOT_STARTED) {
      return status;
    }

    // Set stream buffering mode to flush on newlines so that we
    // capture logs from user processes even when output is redirected
    // to a file. On POSIX, the buffer size is determined by the system
    // when the `buf` parameter is null. On Windows we have to specify
    // the size, so we use 1024 bytes, a number that is arbitrary, but
    // large enough to not affect performance.
    const size_t bufferSize =
#ifdef __WINDOWS__
      1024;
#else // __WINDOWS__
      0;
#endif // __WINDOWS__
    setvbuf(stdout, nullptr, _IOLBF, bufferSize);
    setvbuf(stderr, nullptr, _IOLBF, bufferSize);

    bool local;

    UPID slave;
    SlaveID slaveId;
    FrameworkID frameworkId;
    ExecutorID executorId;
    string workDirectory;
    bool checkpoint;

    Option<string> value;
    std::istringstream iss;
    hashmap<string, string> env(environment);

    // Check if this is local (for example, for testing).
    local = env.contains("MESOS_LOCAL");

    // Get slave PID from environment.
    value = env.get("MESOS_SLAVE_PID");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting 'MESOS_SLAVE_PID' to be set in the environment";
    }

    slave = UPID(value.get());
    CHECK(slave) << "Cannot parse MESOS_SLAVE_PID '" << value.get() << "'";

    // Get slave ID from environment.
    value = env.get("MESOS_SLAVE_ID");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting 'MESOS_SLAVE_ID' to be set in the environment";
    }
    slaveId.set_value(value.get());

    // Get framework ID from environment.
    value = env.get("MESOS_FRAMEWORK_ID");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting 'MESOS_FRAMEWORK_ID' to be set in the environment";
    }
    frameworkId.set_value(value.get());

    // Get executor ID from environment.
    value = env.get("MESOS_EXECUTOR_ID");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting 'MESOS_EXECUTOR_ID' to be set in the environment";
    }
    executorId.set_value(value.get());

    // Get working directory from environment.
    value = env.get("MESOS_DIRECTORY");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting 'MESOS_DIRECTORY' to be set in the environment";
    }
    workDirectory = value.get();

    // Get executor shutdown grace period from the environment.
    //
    // NOTE: We do not require this variable to be set
    // (in contrast to the others above) for backwards
    // compatibility: agents < 0.28.0 do not set it.
    Duration shutdownGracePeriod = DEFAULT_EXECUTOR_SHUTDOWN_GRACE_PERIOD;
    value = env.get("MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD");
    if (value.isSome()) {
      Try<Duration> parse = Duration::parse(value.get());
      if (parse.isError()) {
        EXIT(EXIT_FAILURE)
          << "Failed to parse value '" << value.get() << "' of "
          << "'MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD': " << parse.error();
      }

      shutdownGracePeriod = parse.get();
    }

    // Get checkpointing status from environment.
    value = env.get("MESOS_CHECKPOINT");
    checkpoint = value.isSome() && value.get() == "1";

    Duration recoveryTimeout = RECOVERY_TIMEOUT;

    // Get the recovery timeout if checkpointing is enabled.
    if (checkpoint) {
      value = env.get("MESOS_RECOVERY_TIMEOUT");

      if (value.isSome()) {
        Try<Duration> parse = Duration::parse(value.get());

        if (parse.isError()) {
          EXIT(EXIT_FAILURE)
            << "Failed to parse value '" << value.get() << "'"
            << " of 'MESOS_RECOVERY_TIMEOUT': " << parse.error();
        }

        recoveryTimeout = parse.get();
      }
    }

    CHECK(process == nullptr);

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
        shutdownGracePeriod,
        &mutex,
        latch);

    spawn(process);

    return status = DRIVER_RUNNING;
  }
}


Status MesosExecutorDriver::stop()
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING && status != DRIVER_ABORTED) {
      return status;
    }

    CHECK(process != nullptr);

    dispatch(process, &ExecutorProcess::stop);

    bool aborted = status == DRIVER_ABORTED;

    status = DRIVER_STOPPED;

    return aborted ? DRIVER_ABORTED : status;
  }
}


Status MesosExecutorDriver::abort()
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != nullptr);

    // We set the atomic aborted to true here to prevent any further
    // messages from being processed in the ExecutorProcess. However,
    // if abort() is called from another thread as the ExecutorProcess,
    // there may be at most one additional message processed.
    process->aborted.store(true);

    // Dispatching here ensures that we still process the outstanding
    // requests *from* the executor, since those do proceed when
    // aborted is true.
    dispatch(process, &ExecutorProcess::abort);

    return status = DRIVER_ABORTED;
  }
}


Status MesosExecutorDriver::join()
{
  // Exit early if the driver is not running.
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }
  }

  // If the driver was running, the latch will be triggered regardless
  // of the current `status`. Wait for this to happen to signify
  // termination.
  CHECK_NOTNULL(latch)->await();

  // Now return the current `status` of the driver.
  synchronized (mutex) {
    CHECK(status == DRIVER_ABORTED || status == DRIVER_STOPPED);

    return status;
  }
}


Status MesosExecutorDriver::run()
{
  Status status = start();
  return status != DRIVER_RUNNING ? status : join();
}


Status MesosExecutorDriver::sendStatusUpdate(const TaskStatus& taskStatus)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != nullptr);

    dispatch(process, &ExecutorProcess::sendStatusUpdate, taskStatus);

    return status;
  }
}


Status MesosExecutorDriver::sendFrameworkMessage(const string& data)
{
  synchronized (mutex) {
    if (status != DRIVER_RUNNING) {
      return status;
    }

    CHECK(process != nullptr);

    dispatch(process, &ExecutorProcess::sendFrameworkMessage, data);

    return status;
  }
}
