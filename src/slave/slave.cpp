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

#include <errno.h>
#include <signal.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/fs.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/numify.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/utils.hpp>

#include "common/build.hpp"
#include "common/protobuf_utils.hpp"
#include "common/type_utils.hpp"

#include "logging/logging.hpp"

#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/status_update_manager.hpp"

namespace params = std::tr1::placeholders;

using std::string;
using std::vector;

using process::wait; // Necessary on some OS's to disambiguate.

using std::tr1::cref;
using std::tr1::bind;


namespace mesos {
namespace internal {
namespace slave {

using namespace state;

Slave::Slave(const Resources& _resources,
             bool _local,
             IsolationModule* _isolationModule,
             Files* _files)
  : ProcessBase(ID::generate("slave")),
    flags(),
    local(_local),
    resources(_resources),
    completedFrameworks(MAX_COMPLETED_FRAMEWORKS),
    isolationModule(_isolationModule),
    files(_files),
    monitor(isolationModule),
    statusUpdateManager(new StatusUpdateManager()) {}


Slave::Slave(const flags::Flags<logging::Flags, slave::Flags>& _flags,
             bool _local,
             IsolationModule* _isolationModule,
             Files* _files)
  : ProcessBase(ID::generate("slave")),
    flags(_flags),
    local(_local),
    completedFrameworks(MAX_COMPLETED_FRAMEWORKS),
    isolationModule(_isolationModule),
    files(_files),
    monitor(isolationModule),
    statusUpdateManager(new StatusUpdateManager())
{
  // TODO(benh): Move this computation into Flags as the "default".

  resources = Resources::parse(
      flags.resources.isSome() ? flags.resources.get() : "");

  double cpus;
  if (resources.cpus().isSome()) {
    cpus = resources.cpus().get();
  } else {
    Try<long> cpus_ = os::cpus();
    if (!cpus_.isSome()) {
      LOG(WARNING) << "Failed to auto-detect the number of cpus to use,"
                   << " defaulting to " << DEFAULT_CPUS;
      cpus = DEFAULT_CPUS;
    } else {
      cpus = cpus_.get();
    }
  }

  double mem; // in MB.
  if (resources.mem().isSome()) {
    mem = resources.mem().get();
  } else {
    Try<uint64_t> mem_ = os::memory(); // in bytes.
    if (!mem_.isSome()) {
      LOG(WARNING) << "Failed to auto-detect the size of main memory,"
                   << " defaulting to " << DEFAULT_MEM << " MB";
      mem = DEFAULT_MEM;
    } else {
      // Convert to MB.
      mem = mem_.get() / 1048576;

      // Leave 1 GB free if we have more than 1 GB, otherwise, use all!
      // TODO(benh): Have better default scheme (e.g., % of mem not
      // greater than 1 GB?)
      if (mem > 1024) {
        mem = mem - 1024;
      }
    }
  }

  double disk; // in MB.
  if (resources.disk().isSome()) {
    disk = resources.disk().get();
  } else {
    Try<uint64_t> disk_ = fs::available(); // in bytes.
    if (!disk_.isSome()) {
      LOG(WARNING) << "Failed to auto-detect the free disk space,"
                   << " defaulting to " << DEFAULT_DISK  << " MB";
      disk = DEFAULT_DISK;
    } else {
      // Convert to MB.
      disk = disk_.get() / 1048576;

      // Leave 5 GB free if we have more than 10 GB, otherwise, use all!
      // TODO(benh): Have better default scheme (e.g., % of disk not
      // greater than 10 GB?)
      if (disk > 1024 * 10) {
        disk = disk - (1024 * 5);
      }
    }
  }

  std::string ports;
  if (resources.ports().isSome()) {
    // TODO(vinod): Validate the ports range.
    ports = stringify(resources.ports().get());
  } else {
    ports = DEFAULT_PORTS;
  }

  Try<string> defaults = strings::format(
      "cpus:%f;mem:%f;ports:%s;disk:%f", cpus, mem, ports, disk);

  CHECK_SOME(defaults);

  resources = Resources::parse(defaults.get());

  if (flags.attributes.isSome()) {
    attributes = Attributes::parse(flags.attributes.get());
  }
}


Slave::~Slave()
{
  // TODO(benh): Shut down frameworks?

  // TODO(benh): Shut down executors? The executor should get an "exited"
  // event and initiate a shut down itself.

  foreachvalue (Framework* framework, frameworks) {
    delete framework;
  }

  delete statusUpdateManager;
}


void Slave::initialize()
{
  LOG(INFO) << "Slave started on " << string(self()).substr(6);
  LOG(INFO) << "Slave resources: " << resources;

  // Determine our hostname.
  Try<string> result = os::hostname();

  if (result.isError()) {
    LOG(FATAL) << "Failed to get hostname: " << result.error();
  }

  string hostname = result.get();

  // Check and see if we have a different public DNS name. Normally
  // this is our hostname, but on EC2 we look for the MESOS_PUBLIC_DNS
  // environment variable. This allows the master to display our
  // public name in its webui.
  string webui_hostname = hostname;
  if (getenv("MESOS_PUBLIC_DNS") != NULL) {
    webui_hostname = getenv("MESOS_PUBLIC_DNS");
  }

  // Initialize slave info.
  info.set_hostname(hostname);
  info.set_webui_hostname(webui_hostname); // Deprecated!
  info.mutable_resources()->MergeFrom(resources);
  info.mutable_attributes()->MergeFrom(attributes);

  // Spawn and initialize the isolation module.
  // TODO(benh): Seems like the isolation module should really be
  // spawned before being passed to the slave.
  spawn(isolationModule);
  dispatch(isolationModule,
           &IsolationModule::initialize,
           flags,
           resources,
           local,
           self());

  statusUpdateManager->initialize(self());

  // Start disk monitoring.
  // NOTE: We send a delayed message here instead of directly calling
  // checkDiskUsage, to make disabling this feature easy (e.g by specifying
  // a very large disk_watch_interval).
  delay(flags.disk_watch_interval, self(), &Slave::checkDiskUsage);

  // Start all the statistics at 0.
  stats.tasks[TASK_STAGING] = 0;
  stats.tasks[TASK_STARTING] = 0;
  stats.tasks[TASK_RUNNING] = 0;
  stats.tasks[TASK_FINISHED] = 0;
  stats.tasks[TASK_FAILED] = 0;
  stats.tasks[TASK_KILLED] = 0;
  stats.tasks[TASK_LOST] = 0;
  stats.validStatusUpdates = 0;
  stats.invalidStatusUpdates = 0;
  stats.validFrameworkMessages = 0;
  stats.invalidFrameworkMessages = 0;

  startTime = Clock::now();

  connected = false;

  halting = false;

  // Install protobuf handlers.
  install<NewMasterDetectedMessage>(
      &Slave::newMasterDetected,
      &NewMasterDetectedMessage::pid);

  install<NoMasterDetectedMessage>(
      &Slave::noMasterDetected);

  install<SlaveRegisteredMessage>(
      &Slave::registered,
      &SlaveRegisteredMessage::slave_id);

  install<SlaveReregisteredMessage>(
      &Slave::reregistered,
      &SlaveReregisteredMessage::slave_id);

  install<RunTaskMessage>(
      &Slave::runTask,
      &RunTaskMessage::framework,
      &RunTaskMessage::framework_id,
      &RunTaskMessage::pid,
      &RunTaskMessage::task);

  install<KillTaskMessage>(
      &Slave::killTask,
      &KillTaskMessage::framework_id,
      &KillTaskMessage::task_id);

  install<ShutdownFrameworkMessage>(
      &Slave::shutdownFramework,
      &ShutdownFrameworkMessage::framework_id);

  install<FrameworkToExecutorMessage>(
      &Slave::schedulerMessage,
      &FrameworkToExecutorMessage::slave_id,
      &FrameworkToExecutorMessage::framework_id,
      &FrameworkToExecutorMessage::executor_id,
      &FrameworkToExecutorMessage::data);

  install<UpdateFrameworkMessage>(
      &Slave::updateFramework,
      &UpdateFrameworkMessage::framework_id,
      &UpdateFrameworkMessage::pid);

  install<StatusUpdateAcknowledgementMessage>(
      &Slave::statusUpdateAcknowledgement,
      &StatusUpdateAcknowledgementMessage::slave_id,
      &StatusUpdateAcknowledgementMessage::framework_id,
      &StatusUpdateAcknowledgementMessage::task_id,
      &StatusUpdateAcknowledgementMessage::uuid);

  install<RegisterExecutorMessage>(
      &Slave::registerExecutor,
      &RegisterExecutorMessage::framework_id,
      &RegisterExecutorMessage::executor_id);

  install<ReregisterExecutorMessage>(
      &Slave::reregisterExecutor,
      &ReregisterExecutorMessage::framework_id,
      &ReregisterExecutorMessage::executor_id,
      &ReregisterExecutorMessage::tasks,
      &ReregisterExecutorMessage::updates);

  install<StatusUpdateMessage>(
      &Slave::statusUpdate,
      &StatusUpdateMessage::update);

  install<ExecutorToFrameworkMessage>(
      &Slave::executorMessage,
      &ExecutorToFrameworkMessage::slave_id,
      &ExecutorToFrameworkMessage::framework_id,
      &ExecutorToFrameworkMessage::executor_id,
      &ExecutorToFrameworkMessage::data);

  install<ShutdownMessage>(
      &Slave::shutdown);

  // Install the ping message handler.
  install("PING", &Slave::ping);

  // Setup some HTTP routes.
  route("/vars", bind(&http::vars, cref(*this), params::_1));
  route("/stats.json", bind(&http::json::stats, cref(*this), params::_1));
  route("/state.json", bind(&http::json::state, cref(*this), params::_1));

  if (flags.log_dir.isSome()) {
    Try<string> log = logging::getLogFile(google::INFO);
    if (log.isError()) {
      LOG(ERROR) << "Slave log file cannot be found: " << log.error();
    } else {
      files->attach(log.get(), "/slave/log")
        .onAny(defer(self(), &Self::fileAttached, params::_1, log.get()));
    }
  }

  // Check that the recover flag is valid.
  if (flags.recover != "reconnect" && flags.recover != "cleanup") {
    EXIT(1) << "Unknown option for 'recover' flag " << flags.recover
            << ". Please run the slave with '--help' to see the valid options";
  }

  // Start recovery.
  recover(flags.recover == "reconnect", flags.safe)
   .onAny(defer(self(), &Slave::_recover, params::_1));
}


void Slave::_recover(const Future<Nothing>& future)
{
  if (!future.isReady()) {
    LOG(FATAL) << "Recovery failure: " << future.failure();
  }

  LOG(INFO) << "Finished recovery";

  // Signal recovery.
  recovered.set(Nothing());
}


void Slave::finalize()
{
  LOG(INFO) << "Slave terminating";

  foreachkey (const FrameworkID& frameworkId, frameworks) {
    // TODO(benh): Because a shut down isn't instantaneous (but has
    // a shut down/kill phases) we might not actually propogate all
    // the status updates appropriately here. Consider providing
    // an alternative function which skips the shut down phase and
    // simply does a kill (sending all status updates
    // immediately). Of course, this still isn't sufficient
    // because those status updates might get lost and we won't
    // resend them unless we build that into the system.
    // NOTE: We shut down the framework if either
    // 1: The slave is asked to shutdown (halting = true) or
    // 2: The framework has disabled checkpointing.
    if (halting || !frameworks[frameworkId]->info.checkpoint()) {
      shutdownFramework(frameworkId);
    }
  }

  // Stop the isolation module.
  // TODO(vinod): Wait until all the executors have terminated.
  terminate(isolationModule);
  wait(isolationModule);

  // We send an unregister message to the master here, so that it can
  // remove the slave. This is important because lot of our tests terminate()
  // the slave and expect the master to remove the slave as a consequence.
  // But since the master no longer removes the slave when a slave exits, we
  // send an UnregisterSlaveMessage to master so that it removes the slave.
  // This is OK because, finalize() is only ever going to be called in tests!
  UnregisterSlaveMessage message;
  message.mutable_slave_id()->CopyFrom(info.id());
  send(master, message);
}


void Slave::shutdown()
{
  // Allow shutdown message only if
  // 1) Its a message received from the registered master or
  // 2) If its called locally (e.g tests)
  if (from && from != master) {
    LOG(WARNING) << "Ignoring shutdown message from " << from
                 << " because it is not from the registered master ("
                 << master << ")";
    return;
  }

  LOG(INFO) << "Slave asked to shut down by " << from;

  halting = true;

  terminate(self());
}


void Slave::fileAttached(const Future<Nothing>& result, const string& path)
{
  CHECK(!result.isDiscarded());
  if (result.isReady()) {
    LOG(INFO) << "Successfully attached file '" << path << "'";
  } else {
    LOG(ERROR) << "Failed to attach file '" << path << "': "
               << result.failure();
  }
}


void Slave::detachFile(const Future<Nothing>& result, const std::string& path)
{
  CHECK(!result.isDiscarded());
  files->detach(path);
}


void Slave::newMasterDetected(const UPID& pid)
{
  LOG(INFO) << "New master detected at " << pid;

  master = pid;
  link(master);

  connected = false;

  // Do registration after recovery is complete.
  // NOTE: Slave only registers with master when it is in "reconnect" mode.
  // This ensures that master doesn't offer resources of a slave in "cleanup"
  // mode.
  if (flags.recover == "reconnect") {
    recovered.future()
      .onReady(defer(self(), &Self::doReliableRegistration, params::_1));
  } else {
    LOG(INFO)
      << "Skipping registration because slave is started in 'cleanup' mode";
  }

  // Inform the status updates manager about the new master.
  statusUpdateManager->newMasterDetected(master);
}


void Slave::noMasterDetected()
{
  LOG(INFO) << "Lost master(s) ... waiting";
  connected = false;
  master = UPID();
}


void Slave::registered(const SlaveID& slaveId)
{
  LOG(INFO) << "Registered with master; given slave ID " << slaveId;
  info.mutable_id()->CopyFrom(slaveId); // Store the slave id.
  connected = true;

  if (flags.checkpoint) {
    // Create the slave meta directory.
    paths::createSlaveDirectory(paths::getMetaRootDir(flags.work_dir), slaveId);

    // Checkpoint slave info.
    const string& path = paths::getSlaveInfoPath(
        paths::getMetaRootDir(flags.work_dir), slaveId);

    CHECK_SOME(state::checkpoint(path, info));
  }

  // Schedule all old slave directories to get garbage collected.
  // TODO(benh): It's unclear if we really need/want to
  // wait until the slave is registered to do this.
  const string& directory = path::join(flags.work_dir, "slaves");

  foreach (const string& file, os::ls(directory)) {
    const string& path = path::join(directory, file);

    // Check that this path is a directory but not our directory!
    if (os::isdir(path) && file != info.id().value()) {

      Try<long> time = os::mtime(path);

      if (time.isSome()) {
        // Schedule the directory to be removed after some remaining
        // delta of the delay and last modification time.
        Seconds delta(flags.gc_delay.secs() - (Clock::now() - time.get()));
        gc.schedule(delta, path);
      } else {
        LOG(WARNING) << "Failed to get the modification time of "
                     << path << ": " << time.error();
        gc.schedule(flags.gc_delay, path);
      }
    }
  }
}


void Slave::reregistered(const SlaveID& slaveId)
{
  LOG(INFO) << "Re-registered with master";

  if (!(info.id() == slaveId)) {
    LOG(FATAL) << "Slave re-registered but got wrong ID";
  }
  connected = true;
}


void Slave::doReliableRegistration(const Future<Nothing>& future)
{
  CHECK(future.isReady());

  if (connected || !master) {
    return;
  }

  if (info.id() == "") {
    // Slave started before master.
    // (Vinod): Is the above comment true?
    RegisterSlaveMessage message;
    message.mutable_slave()->MergeFrom(info);
    send(master, message);
  } else {
    // Re-registering, so send tasks running.
    ReregisterSlaveMessage message;
    message.mutable_slave_id()->MergeFrom(info.id());
    message.mutable_slave()->MergeFrom(info);

    foreachvalue (Framework* framework, frameworks){
      foreachvalue (Executor* executor, framework->executors) {
        // TODO(benh): Kill this once framework_id is required
        // on ExecutorInfo.
        ExecutorInfo* executorInfo = message.add_executor_infos();
        executorInfo->MergeFrom(executor->info);
        executorInfo->mutable_framework_id()->MergeFrom(framework->id);

        // Add launched tasks.
        foreachvalue (Task* task, executor->launchedTasks) {
          message.add_tasks()->CopyFrom(*task);
        }

        // Add queued tasks.
        foreachvalue (const TaskInfo& task, executor->queuedTasks) {
          const Task& t = protobuf::createTask(
              task, TASK_STAGING, executor->id, framework->id);

          message.add_tasks()->CopyFrom(t);
        }
      }
    }
    send(master, message);
  }

  // Retry registration if necessary.
  delay(Seconds(1.0), self(), &Slave::doReliableRegistration, future);
}


// TODO(vinod): Instead of crashing the slave on checkpoint errors,
// send TASK_LOST to the framework.
void Slave::runTask(
    const FrameworkInfo& frameworkInfo,
    const FrameworkID& frameworkId,
    const string& pid,
    const TaskInfo& task)
{
  LOG(INFO) << "Got assigned task " << task.task_id()
            << " for framework " << frameworkId;

  if (frameworkInfo.checkpoint() && !flags.checkpoint) {
     LOG(WARNING) << "Asked to checkpoint framework " << frameworkId
                  << " but the checkpointing is disabled on the slave!"
                  << " Please start the slave with '--checkpoint' flag";

     const StatusUpdate& update = protobuf::createStatusUpdate(
         frameworkId,
         info.id(),
         task.task_id(),
         TASK_LOST,
         "Could not launch the task because the framework expects checkpointing"
         ", but checkpointing is disabled on the slave");

     statusUpdate(update);
     return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    framework = new Framework(frameworkId, frameworkInfo, pid, flags);
    frameworks[frameworkId] = framework;

    if (frameworkInfo.checkpoint()) {
      // Checkpoint the framework info.
      string path = paths::getFrameworkInfoPath(
          paths::getMetaRootDir(flags.work_dir),
          info.id(),
          frameworkId);

      CHECK_SOME(state::checkpoint(path, frameworkInfo));

      // Checkpoint the framework pid.
      path = paths::getFrameworkPidPath(
          paths::getMetaRootDir(flags.work_dir),
          info.id(),
          frameworkId);

      CHECK_SOME(state::checkpoint(path, framework->pid));
    }
  }

  const ExecutorInfo& executorInfo = framework->getExecutorInfo(task);
  const ExecutorID& executorId = executorInfo.executor_id();

  // Either send the task to an executor or start a new executor
  // and queue the task until the executor has started.
  Executor* executor = framework->getExecutor(executorId);

  if (executor != NULL) {
    if (executor->shutdown) {
      LOG(WARNING) << "WARNING! Asked to run task '" << task.task_id()
                   << "' for framework " << frameworkId
                   << " with executor '" << executorId
                   << "' which is being shut down";

      const StatusUpdate& update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          task.task_id(),
          TASK_LOST,
          "Executor shutting down");

      statusUpdate(update);
    } else if (!executor->pid) {
      // Queue task until the executor starts up.
      LOG(INFO) << "Queuing task '" << task.task_id()
                << "' for executor " << executorId
                << " of framework '" << frameworkId;

      if (frameworkInfo.checkpoint()) {
        // Checkpoint the task.
        // TODO(vinod): Consider moving these 3 statements into
        // a helper function, since its used 3 times in this function!
        const string& path = paths::getTaskInfoPath(
            paths::getMetaRootDir(flags.work_dir),
            info.id(),
            executor->frameworkId,
            executor->id,
            executor->uuid,
            task.task_id());

        const Task& t = protobuf::createTask(
            task, TASK_STAGING, executor->id, executor->frameworkId);

        CHECK_SOME(state::checkpoint(path, t));
      }

      executor->queuedTasks[task.task_id()] = task;
    } else {
      if (frameworkInfo.checkpoint()) {
        // Checkpoint the task.
        const string& path = paths::getTaskInfoPath(
            paths::getMetaRootDir(flags.work_dir),
            info.id(),
            executor->frameworkId,
            executor->id,
            executor->uuid,
            task.task_id());

        const Task& t = protobuf::createTask(
            task, TASK_STAGING, executor->id, executor->frameworkId);

        CHECK_SOME(state::checkpoint(path, t));
      }

      // Add the task and send it to the executor.
      executor->addTask(task);

      stats.tasks[TASK_STAGING]++;

      // Update the resources.
      // TODO(Charles Reiss): The isolation module is not guaranteed to update
      // the resources before the executor acts on its RunTaskMessage.
      dispatch(isolationModule,
               &IsolationModule::resourcesChanged,
               framework->id,
               executor->id,
               executor->resources);

      LOG(INFO) << "Sending task '" << task.task_id()
                << "' to executor '" << executorId
                << "' of framework " << framework->id;

      RunTaskMessage message;
      message.mutable_framework()->MergeFrom(framework->info);
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.set_pid(framework->pid);
      message.mutable_task()->MergeFrom(task);
      send(executor->pid, message);
    }
  } else {
    // Launch an executor for this task.
    executor = framework->createExecutor(info.id(), executorInfo);

    files->attach(executor->directory, executor->directory)
      .onAny(defer(self(),
                   &Self::fileAttached,
                   params::_1,
                   executor->directory));

    // Check to see if we need to checkpoint this executor and the task.
    Option<string> pidPath;
    if (frameworkInfo.checkpoint()) {
      // Checkpoint the executor info.
      string path = paths::getExecutorInfoPath(
          paths::getMetaRootDir(flags.work_dir),
          info.id(),
          executor->frameworkId,
          executor->id);

      CHECK_SOME(state::checkpoint(path, executor->info));

      // Create the meta executor directory.
      // NOTE: This creates the 'latest' symlink in the meta directory.
      paths::createExecutorDirectory(
          paths::getMetaRootDir(flags.work_dir),
          info.id(),
          framework->id,
          executor->id,
          executor->uuid);

      // Checkpoint the task.
      path = paths::getTaskInfoPath(
          paths::getMetaRootDir(flags.work_dir),
          info.id(),
          executor->frameworkId,
          executor->id,
          executor->uuid,
          task.task_id());

      const Task& t = protobuf::createTask(
          task, TASK_STAGING, executor->id, executor->frameworkId);

      CHECK_SOME(state::checkpoint(path, t));

      // Get the path for isolation module to checkpoint the forked pid.
      pidPath = paths::getForkedPidPath(
          paths::getMetaRootDir(flags.work_dir),
          info.id(),
          framework->id,
          executor->id,
          executor->uuid);
    }

    // Queue task until the executor starts up.
    executor->queuedTasks[task.task_id()] = task;

    // Tell the isolation module to launch the executor.
    dispatch(isolationModule,
             &IsolationModule::launchExecutor,
             info.id(),
             framework->id,
             framework->info,
             executor->info,
             executor->uuid,
             executor->directory,
             executor->resources,
             pidPath);
  }
}


void Slave::killTask(const FrameworkID& frameworkId, const TaskID& taskId)
{
  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "WARNING! Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such framework is running";

    const StatusUpdate& update = protobuf::createStatusUpdate(
        frameworkId, info.id(), taskId, TASK_LOST, "Cannot find framework");

    statusUpdate(update);
    return;
  }

  // Tell the executor to kill the task if it is up and
  // running, otherwise, consider the task lost.
  Executor* executor = framework->getExecutor(taskId);
  if (executor == NULL) {
    LOG(WARNING) << "WARNING! Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no corresponding executor is running";

    const StatusUpdate& update = protobuf::createStatusUpdate(
        frameworkId, info.id(), taskId, TASK_LOST, "Cannot find executor");

    statusUpdate(update);
  } else if (!executor->pid) {
    // We are here, if the executor hasn't registered with the slave yet.

    const StatusUpdate& update = protobuf::createStatusUpdate(
        frameworkId,
        info.id(),
        taskId,
        TASK_KILLED,
        "Unregistered executor",
        executor->id);

    statusUpdate(update);
  } else {
    // Otherwise, send a message to the executor and wait for
    // it to send us a status update.
    KillTaskMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_task_id()->MergeFrom(taskId);
    send(executor->pid, message);
  }
}


// TODO(benh): Consider sending a boolean that specifies if the
// shut down should be graceful or immediate. Likewise, consider
// sending back a shut down acknowledgement, because otherwise you
// could get into a state where a shut down was sent, dropped, and
// therefore never processed.
void Slave::shutdownFramework(const FrameworkID& frameworkId)
{
  // Allow shutdownFramework() only if
  // its called directly (e.g. Slave::finalize()) or
  // its a message from the currently registered master.
  if (from && from != master) {
    LOG(WARNING) << "Ignoring shutdown framework message from " << from
                 << " because it is not from the registered master ("
                 << master << ")";
    return;
  }

  LOG(INFO) << "Asked to shut down framework " << frameworkId
            << " by " << from;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Shutting down framework " << framework->id;

    // Shut down all executors of this framework.
    // Note that the framework and its corresponding executors are removed from
    // the frameworks map by shutdownExecutorTimeout() or executorTerminated().
    foreachvalue (Executor* executor, framework->executors) {
      shutdownExecutor(framework, executor);
    }
  }

  // Close all status update streams for this framework.
  statusUpdateManager->cleanup(frameworkId);
}


void Slave::schedulerMessage(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Dropping message for framework "<< frameworkId
                 << " because framework does not exist";
    stats.invalidFrameworkMessages++;
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Dropping message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because executor does not exist";
    stats.invalidFrameworkMessages++;
  } else if (!executor->pid) {
    // TODO(*): If executor is not started, queue framework message?
    // (It's probably okay to just drop it since frameworks can have
    // the executor send a message to the master to say when it's ready.)
    LOG(WARNING) << "Dropping message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because executor is not running";
    stats.invalidFrameworkMessages++;
  } else {
    FrameworkToExecutorMessage message;
    message.mutable_slave_id()->MergeFrom(slaveId);
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    message.set_data(data);
    send(executor->pid, message);

    stats.validFrameworkMessages++;
  }
}


void Slave::updateFramework(const FrameworkID& frameworkId, const string& pid)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Updating framework " << frameworkId
              << " pid to " <<pid;
    framework->pid = pid;
  }
}


void Slave::statusUpdateAcknowledgement(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const TaskID& taskId,
    const string& uuid)
{
  LOG(INFO) << "Got acknowledgement of status update"
            << " for task " << taskId
            << " of framework " << frameworkId;

  statusUpdateManager->acknowledgement(taskId, frameworkId, uuid)
    .onAny(defer(self(),
                 &Slave::_statusUpdateAcknowledgement,
                 params::_1,
                 taskId,
                 frameworkId,
                 uuid));
}


void Slave::_statusUpdateAcknowledgement(
    const Future<Try<Nothing> >& future,
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const string& uuid)
{
  if (!future.isReady()) {
    LOG(FATAL) << "Failed to handle status update acknowledgement " << uuid
               << " for task " << taskId
               << " of framework " << frameworkId
               << (future.isFailed() ? future.failure() : "future discarded");
  }

  if (future.get().isError()) {
    LOG(ERROR) << "Failed to handle the status update acknowledgement " << uuid
        << " for task " << taskId
        << " of framework " << frameworkId
        << future.get().error();
    return;
  }

  // If this slave is in 'recover=cleanup' mode, exit after all executors
  // have exited.
  if (flags.recover == "cleanup" && frameworks.empty()) {
    LOG(INFO) << "Slave is shutting down because it was started in cleanup "
              << " recovery mode and all updates have been acknowledged!";
    shutdown();
  }
}


void Slave::registerExecutor(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  LOG(INFO) << "Got registration for executor '" << executorId
            << "' of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    // Framework is gone; tell the executor to exit.
    LOG(WARNING) << "Framework " << frameworkId
                 << " does not exist (it may have been killed),"
                 << " telling executor to exit";
    reply(ShutdownExecutorMessage());
    return;
  }

  Executor* executor = framework->getExecutor(executorId);

  // Check the status of the executor.
  if (executor == NULL) {
    LOG(WARNING) << "WARNING! Unexpected executor '" << executorId
                 << "' registering for framework " << frameworkId;
    reply(ShutdownExecutorMessage());
  } else if (executor->pid) {
    LOG(WARNING) << "WARNING! executor '" << executorId
                 << "' of framework " << frameworkId
                 << " is already running";
    reply(ShutdownExecutorMessage());
  } else if (executor->shutdown) {
    LOG(WARNING) << "WARNING! executor '" << executorId
                 << "' of framework " << frameworkId
                 << " should be shutting down";
    reply(ShutdownExecutorMessage());
  } else {
    // Save the pid for the executor.
    executor->pid = from;

    if (framework->info.checkpoint()) {
      // TODO(vinod): This checkpointing should be done asynchronously as it is
      // in the fast path of the slave!

      // Checkpoint the libprocess pid.
      string path = paths::getLibprocessPidPath(
          paths::getMetaRootDir(flags.work_dir),
          info.id(),
          executor->frameworkId,
          executor->id,
          executor->uuid);

      CHECK_SOME(state::checkpoint(path, executor->pid));
    }

    // First account for the tasks we're about to start.
    foreachvalue (const TaskInfo& task, executor->queuedTasks) {
      // Add the task to the executor.
      executor->addTask(task);
    }

    // Now that the executor is up, set its resource limits including the
    // currently queued tasks.
    // TODO(Charles Reiss): We don't actually have a guarantee that this will
    // be delivered or (where necessary) acted on before the executor gets its
    // RunTaskMessages.
    dispatch(isolationModule,
             &IsolationModule::resourcesChanged,
             framework->id,
             executor->id,
             executor->resources);

    // Tell executor it's registered and give it any queued tasks.
    ExecutorRegisteredMessage message;
    message.mutable_executor_info()->MergeFrom(executor->info);
    message.mutable_framework_id()->MergeFrom(framework->id);
    message.mutable_framework_info()->MergeFrom(framework->info);
    message.mutable_slave_id()->MergeFrom(info.id());
    message.mutable_slave_info()->MergeFrom(info);
    send(executor->pid, message);

    LOG(INFO) << "Flushing queued tasks for framework " << framework->id;

    foreachvalue (const TaskInfo& task, executor->queuedTasks) {
      stats.tasks[TASK_STAGING]++;

      RunTaskMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.mutable_framework()->MergeFrom(framework->info);
      message.set_pid(framework->pid);
      message.mutable_task()->MergeFrom(task);
      send(executor->pid, message);
    }

    executor->queuedTasks.clear();
  }
}


void Slave::reregisterExecutor(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const vector<TaskInfo>& tasks,
    const vector<StatusUpdate>& updates)
{
  LOG(INFO) << "Re-registering executor " << executorId
            << " of framework " << frameworkId;

  CHECK(frameworks.contains(frameworkId));
  Framework* framework = frameworks[frameworkId];

  CHECK(framework->executors.contains(executorId));
  Executor* executor = framework->executors[executorId];

  executor->pid = from; // Update the pid, to signal re-registration.

  // Send re-registration message to the executor.
  ExecutorReregisteredMessage message;
  message.mutable_slave_id()->MergeFrom(info.id());
  message.mutable_slave_info()->MergeFrom(info);
  send(executor->pid, message);

  // Handle all the pending updates.
  // NOTE: The status update manager might have already checkpointed some
  // of these pending updates (for e.g: if the slave died right after it
  // checkpointed the update but before it could send the ACK to the executor).
  // This is ok because, the status update manager simply re-ACKs the executor.
  foreach (const StatusUpdate& update, updates) {
    statusUpdate(update); // This also updates the isolation module's resources!
  }

  // Now, if there is any task still in STAGING state and not in 'tasks' known
  // to the executor, the slave must have died before the executor received
  // the task! Relaunch it!
  hashmap<TaskID, TaskInfo> launched;
  foreach (const TaskInfo& task, tasks) {
    launched[task.task_id()] = task;
  }

  foreachvalue (Task* task, executor->launchedTasks) {
    if (task->state() == TASK_STAGING && !launched.contains(task->task_id())) {
      LOG (INFO) << "Relaunching STAGED task " << task->task_id()
                 << " of executor " << task->executor_id();

      RunTaskMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.mutable_framework()->MergeFrom(framework->info);
      message.set_pid(framework->pid);
      message.mutable_task()->MergeFrom(launched[task->task_id()]);
      send(executor->pid, message);
    }
  }
}



void Slave::reregisterExecutorTimeout()
{
  LOG(INFO) << "Cleaning up un-reregistered executors";

  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      // If we are here, the executor must have been hung and
      // not exited! This is because, if the executor properly
      // exited, it should have already been identified by the
      // isolation module (via reaper) and cleaned up!
      if (!executor->pid) {
        LOG(INFO) << "Shutting down un-reregistered executor " << executor->id
                  << " of framework " << framework->id;

        // TODO(vinod): Call shutdownExecutor() when it supports
        // immediate shutdown of the executor.
        shutdownExecutorTimeout(framework->id, executor->id, executor->uuid);
      }
    }
  }

  // Signal the end of recovery.
  recovered.set(Nothing());
}


// This can be called in two ways:
// 1) When a status update from the executor is received.
// 2) When slave generates task updates (e.g LOST/KILLED/FAILED).
void Slave::statusUpdate(const StatusUpdate& update)
{
  const TaskStatus& status = update.status();

  LOG(INFO) << "Handling status update " << update;

  Framework* framework = getFramework(update.framework_id());
  Executor* executor = NULL;

  if (framework != NULL) {
    executor = framework->getExecutor(status.task_id());

    if (executor != NULL) {
      executor->updateTaskState(status.task_id(), status.state());

      // Handle the task appropriately if it's terminated.
      if (protobuf::isTerminalState(status.state())) {
        executor->removeTask(status.task_id());

        dispatch(
            isolationModule,
            &IsolationModule::resourcesChanged,
            framework->id,
            executor->id,
            executor->resources);
      }
    } else {
      LOG(WARNING) << "Could not find executor for task " << status.task_id()
                   << " of framework " << update.framework_id();

      stats.invalidStatusUpdates++;
    }
  } else {
    LOG(WARNING) << "Could not find framework " << update.framework_id()
                 << " for task " << status.task_id();

    stats.invalidStatusUpdates++;
  }

  // Forward the update to the status update manager.
  // NOTE: We forward the update even if the framework/executor is unknown
  // because currently there is no persistent state in the master.
  // The lack of persistence might lead frameworks to use out-of-band means
  // to figure out the task state mismatch and use status updates to reconcile.
  // We need to revisit this issue once master has persistent state.
  forwardUpdate(update, executor);
}


void Slave::forwardUpdate(const StatusUpdate& update, Executor* executor)
{
  LOG(INFO) << "Forwarding status update " << update
            << " to the status update manager";

  const FrameworkID& frameworkId = update.framework_id();
  const TaskID& taskId = update.status().task_id();

  Option<UPID> pid;
  Option<string> path;
  bool checkpoint = false;

  if (executor != NULL) {
    // Get the executor pid.
    if (executor->pid) {
      pid = executor->pid;
    }

    // Check whether we need to do checkpointing.
    Framework* framework = getFramework(frameworkId);
    CHECK_NOTNULL(framework);
    checkpoint = framework->info.checkpoint();

    if (checkpoint) {
      // Get the path to store the updates.
      path = paths::getTaskUpdatesPath(
          paths::getMetaRootDir(flags.work_dir),
          info.id(),
          frameworkId,
          executor->id,
          executor->uuid,
          taskId);
    }
  }

  stats.tasks[update.status().state()]++;
  stats.validStatusUpdates++;

  statusUpdateManager->update(update, checkpoint, path)
    .onAny(defer(self(), &Slave::_forwardUpdate, params::_1, update, pid));;
}


void Slave::_forwardUpdate(
    const Future<Try<Nothing> >& future,
    const StatusUpdate& update,
    const Option<UPID>& pid)
{
  if (!future.isReady()) {
    LOG(FATAL) << "Failed to handle status update " << update
               << (future.isFailed() ? future.failure() : "future discarded");
    return;
  }

  if (future.get().isError()) {
    LOG(ERROR)
      << "Failed to handle the status update " << update
      << ": " << future.get().error();
    return;
  }

  // Status update manager successfully handled the status update.
  // Acknowledge the executor, if necessary.
  if (pid.isSome()) {
    LOG(INFO) << "Sending ACK for status update " << update
              << " to executor " << pid.get();
    StatusUpdateAcknowledgementMessage message;
    message.mutable_framework_id()->MergeFrom(update.framework_id());
    message.mutable_slave_id()->MergeFrom(update.slave_id());
    message.mutable_task_id()->MergeFrom(update.status().task_id());
    message.set_uuid(update.uuid());

    send(pid.get(), message);
  }
}


void Slave::executorMessage(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Cannot send framework message from slave "
                 << slaveId << " to framework " << frameworkId
                 << " because framework does not exist";
    stats.invalidFrameworkMessages++;
    return;
  }

  LOG(INFO) << "Sending message for framework " << frameworkId
            << " to " << framework->pid;

  ExecutorToFrameworkMessage message;
  message.mutable_slave_id()->MergeFrom(slaveId);
  message.mutable_framework_id()->MergeFrom(frameworkId);
  message.mutable_executor_id()->MergeFrom(executorId);
  message.set_data(data);
  send(framework->pid, message);

  stats.validFrameworkMessages++;
}


void Slave::ping(const UPID& from, const string& body)
{
  send(from, "PONG");
}


void Slave::exited(const UPID& pid)
{
  LOG(INFO) << "Process exited: " << from;

  if (master == pid) {
    LOG(WARNING) << "WARNING! Master disconnected!"
                 << " Waiting for a new master to be elected.";
    // TODO(benh): After so long waiting for a master, commit suicide.
  }
}


Framework* Slave::getFramework(const FrameworkID& frameworkId)
{
  if (frameworks.count(frameworkId) > 0) {
    return frameworks[frameworkId];
  }

  return NULL;
}


void _watch(
    const Future<Nothing>& watch,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId);


// N.B. When the slave is running in "local" mode then the pid is
// uninteresting (and possibly could cause bugs).
void Slave::executorStarted(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    pid_t pid)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Framework " << frameworkId
                 << " for executor '" << executorId
                 << "' is no longer valid";
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Invalid executor '" << executorId
                 << "' of framework " << frameworkId
                 << " has started";
    return;
  }

  monitor.watch(
      frameworkId,
      executorId,
      executor->info,
      flags.resource_monitoring_interval)
    .onAny(lambda::bind(_watch, lambda::_1, frameworkId, executorId));
}


void _watch(
    const Future<Nothing>& watch,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CHECK(!watch.isDiscarded());

  if (!watch.isReady()) {
    LOG(ERROR) << "Failed to watch executor " << executorId
               << " of framework " << frameworkId
               << ": " << watch.failure();
  }
}


void _unwatch(
    const Future<Nothing>& watch,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId);


// Called by the isolation module when an executor process terminates.
void Slave::executorTerminated(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    int status,
    bool destroyed,
    const string& message)
{
  LOG(INFO) << "Executor '" << executorId
            << "' of framework " << frameworkId
            << (WIFEXITED(status)
                ? " has exited with status "
                : " has terminated with signal ")
            << (WIFEXITED(status)
                ? stringify(WEXITSTATUS(status))
                : strsignal(WTERMSIG(status)));

  // Stop monitoring this executor.
  monitor.unwatch(frameworkId, executorId)
      .onAny(lambda::bind(_unwatch, lambda::_1, frameworkId, executorId));

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Framework " << frameworkId
                 << " for executor '" << executorId
                 << "' is no longer valid";
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Invalid executor '" << executorId
                 << "' of framework " << frameworkId
                 << " has exited/terminated";
    return;
  }

  bool isCommandExecutor = false;

  // Transition all live tasks to TASK_LOST/TASK_FAILED.
  // If the isolation module destroyed the executor (e.g., due to OOM event)
  // or if this is a command executor, we send TASK_FAILED status updates
  // instead of TASK_LOST.

  StatusUpdate update;

  // Transition all live launched tasks.
  foreachvalue (Task* task, utils::copy(executor->launchedTasks)) {
    if (!protobuf::isTerminalState(task->state())) {
      isCommandExecutor = !task->has_executor_id();
      if (destroyed || isCommandExecutor) {
        update = protobuf::createStatusUpdate(
            frameworkId,
            info.id(),
            task->task_id(),
            TASK_FAILED,
            message,
            executorId);
      } else {
        update = protobuf::createStatusUpdate(
            frameworkId,
            info.id(),
            task->task_id(),
            TASK_LOST,
            message,
            executorId);
      }
      statusUpdate(update); // Handle the status update.
    }
  }

  // Transition all queued tasks.
  foreachvalue (const TaskInfo& task, utils::copy(executor->queuedTasks)) {
    isCommandExecutor = task.has_command();

    if (destroyed || isCommandExecutor) {
      update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          task.task_id(),
          TASK_FAILED,
          message,
          executorId);
    } else {
      update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          task.task_id(),
          TASK_LOST,
          message,
          executorId);
    }
    statusUpdate(update); // Handle the status update.
  }

  if (!isCommandExecutor) {
    ExitedExecutorMessage message;
    message.mutable_slave_id()->MergeFrom(info.id());
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    message.set_status(status);

    send(master, message);
  }

  // Schedule the executor directory to get garbage collected.
  gc.schedule(flags.gc_delay, executor->directory)
    .onAny(defer(self(), &Self::detachFile, params::_1, executor->directory));

  framework->destroyExecutor(executor->id);

  // Cleanup if this framework has no executors running.
  if (framework->executors.empty()) {
    frameworks.erase(framework->id);

    // Pass ownership of the framework pointer.
    completedFrameworks.push_back(std::tr1::shared_ptr<Framework>(framework));
  }
}


void _unwatch(
    const Future<Nothing>& unwatch,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CHECK(!unwatch.isDiscarded());

  if (!unwatch.isReady()) {
    LOG(ERROR) << "Failed to unwatch executor " << executorId
               << " of framework " << frameworkId
               << ": " << unwatch.failure();
  }
}


void Slave::shutdownExecutor(Framework* framework, Executor* executor)
{
  LOG(INFO) << "Shutting down executor '" << executor->id
            << "' of framework " << framework->id;

  // If the executor hasn't yet registered, this message
  // will be dropped to the floor!
  send(executor->pid, ShutdownExecutorMessage());

  executor->shutdown = true;

  // Prepare for sending a kill if the executor doesn't comply.
  delay(flags.executor_shutdown_grace_period,
        self(),
        &Slave::shutdownExecutorTimeout,
        framework->id,
        executor->id,
        executor->uuid);
}


void Slave::shutdownExecutorTimeout(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const UUID& uuid)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  // Make sure this timeout is valid.
  if (executor != NULL && executor->uuid == uuid) {
    LOG(INFO) << "Killing executor '" << executor->id
              << "' of framework " << framework->id;

    dispatch(isolationModule,
             &IsolationModule::killExecutor,
             framework->id,
             executor->id);
  }
}


// TODO(vinod): Figure out a way to express this function via cmd line.
Duration Slave::age(double usage)
{
 return Weeks(flags.gc_delay.weeks() * (1.0 - usage));
}


void Slave::checkDiskUsage()
{
  // TODO(vinod): We are making usage a Future, so that we can plug in
  // fs::usage() into async.
  Future<Try<double> >(fs::usage())
    .onAny(defer(self(), &Slave::_checkDiskUsage, params::_1));
}


void Slave::_checkDiskUsage(const Future<Try<double> >& usage)
{
  if (!usage.isReady()) {
    LOG(ERROR) << "Failed to get disk usage: "
               << (usage.isFailed() ? usage.failure() : "future discarded");
  } else {
    Try<double> result = usage.get();

    if (result.isSome()) {
      double use = result.get();

      LOG(INFO) << "Current disk usage " << std::setiosflags(std::ios::fixed)
                << std::setprecision(2) << 100 * use << "%."
                << " Max allowed age: " << age(use);

      // We prune all directories whose deletion time is within
      // the next 'gc_delay - age'. Since a directory is always
      // scheduled for deletion 'gc_delay' into the future, only directories
      // that are at least 'age' old are deleted.
      gc.prune(Weeks(flags.gc_delay.weeks() - age(use).weeks()));
    } else {
      LOG(WARNING) << "Unable to get disk usage: " << result.error();
    }
  }
  delay(flags.disk_watch_interval, self(), &Slave::checkDiskUsage);
}


Future<Nothing> Slave::recover(bool reconnect, bool safe)
{
  const string& metaDir = paths::getMetaRootDir(flags.work_dir);

  // We consider the absence of 'metaDir' to mean that this is the
  // very first time this slave was started with checkpointing
  // enabled.
  if (!os::exists(metaDir)) {
    // NOTE: We recover the isolation module here to cleanup any old
    // executors (e.g: orphaned cgroups).
    return dispatch(isolationModule, &IsolationModule::recover, None());
  }

  // TODO(vinod): Check for version and slaveinfo compatibility.

  // First, recover the slave state.
  Result<SlaveState> state = state::recover(metaDir, safe);
  if (state.isError()) {
    EXIT(1) << "Failed to recover slave state: " << state.error();
  }

  if (state.isNone() || state.get().info.isNone()) {
    // We are here if the slave died before checkpointing its info.
    // NOTE: We recover the isolation module here to cleanup any old
    // executors (e.g: orphaned cgroups).
    return dispatch(isolationModule, &IsolationModule::recover, None());
  }

  info = state.get().info.get(); // Recover the slave info.

  // Recover the status update manager, then the isolation module and
  // then the executors.
  return statusUpdateManager->recover(metaDir, state.get())
           .then(defer(isolationModule,
                       &IsolationModule::recover,
                       state.get()))
           .then(defer(self(),
                       &Self::recoverExecutors,
                       state.get(),
                       reconnect));
}


Future<Nothing> Slave::recoverExecutors(
    const SlaveState& state,
    bool reconnect)
{
  LOG(INFO) << "Recovering executors";

  foreachvalue (const FrameworkState& framework_, state.frameworks) {
    foreachvalue (const ExecutorState& executor_, framework_.executors) {
      LOG(INFO) << "Recovering executor '" << executor_.id
                << "' of framework " << framework_.id;

      if (executor_.info.isNone()) {
        LOG(WARNING) << "Skipping recovery of executor '" << executor_.id
                     << "' of framework " << framework_.id
                     << " because its info cannot be recovered";
        continue;
      }

      if (executor_.latest.isNone()) {
        LOG(WARNING) << "Skipping recovery of executor '" << executor_.id
                     << "' of framework " << framework_.id
                     << " because its latest run cannot be recovered";
        continue;
      }

      // We are only interested in the latest run of the executor!
      const UUID& uuid = executor_.latest.get();
      CHECK(executor_.runs.contains(uuid));
      const RunState& run  = executor_.runs.get(uuid).get();

      // Create framework, if necessary.
      Framework* framework = getFramework(framework_.id);
      if (framework == NULL) {
        CHECK_SOME(framework_.info);
        CHECK_SOME(framework_.pid);

        framework = new Framework(
            framework_.id, framework_.info.get(), framework_.pid.get(), flags);

        frameworks[framework_.id] = framework;
      }

      // Create executor.
      const string& directory = paths::getExecutorRunPath(
          flags.work_dir, info.id(), framework_.id, executor_.id, uuid);

      Executor* executor =
          new Executor(framework_.id, executor_.info.get(), uuid, directory);

      // Recover the tasks.
      foreachvalue (const TaskState& task, run.tasks) {
        if (task.info.isNone()) {
          LOG(WARNING) << "Skipping recovery of task " << task.id
                       << " because its info cannot be recovered";
          continue;
        }

        executor->launchedTasks[task.id] = new Task(task.info.get());

        // NOTE: Since some tasks might have been terminated when the slave
        // was down, the executor resources we capture here is an upper-bound.
        // The actual resources needed (for live tasks) by the isolation module
        // will be calculated when the executor re-registers.
        executor->resources += task.info.get().resources();

        // Read updates to get the latest state of the task.
        foreach (const StatusUpdate& update, task.updates) {
          executor->updateTaskState(task.id, update.status().state());

          // Remove the task if it terminated.
          if (protobuf::isTerminalState(update.status().state())) {
            executor->removeTask(task.id);
            break;
          }
        }
      }

      // Add the executor to the framework.
      framework->executors[executor_.id] = executor;

      files->attach(executor->directory, executor->directory)
        .onAny(defer(self(),
                     &Self::fileAttached,
                     params::_1,
                     executor->directory));

      // Reconnect with executor, if possible.
      if (reconnect && run.libprocessPid.isSome()) {
        CHECK_SOME(run.forkedPid);

        LOG(INFO) << "Sending reconnect request to executor " << executor_.id
                  << " of framework " << framework_.id
                  << " at " << run.libprocessPid.get();

        ReconnectExecutorMessage message;
        message.mutable_slave_id()->MergeFrom(info.id());
        send(run.libprocessPid.get(), message);
      } else if (run.libprocessPid.isSome()) {
        // Cleanup executors.
        LOG(INFO) << "Sending shutdown to executor " << executor_.id
                  << " of framework " << framework_.id
                  << " at " << run.libprocessPid.get();

        executor->pid = run.libprocessPid.get();
        shutdownExecutor(framework, executor);
      }

      // Beging monitoring the executor.
      monitor.watch(
          framework_.id,
          executor_.id,
          executor_.info.get(),
          flags.resource_monitoring_interval)
        .onAny(lambda::bind(_watch, lambda::_1, framework_.id, executor_.id));
    }
  }

  if (reconnect) {
    // Cleanup unregistered executors after a delay.
    delay(EXECUTOR_REREGISTER_TIMEOUT,
          self(),
          &Slave::reregisterExecutorTimeout);

    // We set 'recovered' flag inside reregisterExecutorTimeout(),
    // so that when the slave re-registers with master it can
    // correctly inform the master about the launched tasks.
    return recovered.future();
  }

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
