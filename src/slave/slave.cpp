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
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <process/async.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/time.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
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

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/status_update_manager.hpp"

using std::list;
using std::map;
using std::string;
using std::vector;

using process::wait; // Necessary on some OS's to disambiguate.

namespace mesos {
namespace internal {
namespace slave {

using namespace state;

Slave::Slave(const slave::Flags& _flags,
             MasterDetector* _detector,
             Containerizer* _containerizer,
             Files* _files)
  : ProcessBase(ID::generate("slave")),
    state(RECOVERING),
    http(*this),
    flags(_flags),
    completedFrameworks(MAX_COMPLETED_FRAMEWORKS),
    detector(_detector),
    containerizer(_containerizer),
    files(_files),
    monitor(containerizer),
    statusUpdateManager(new StatusUpdateManager()),
    metaDir(paths::getMetaRootDir(flags.work_dir)),
    recoveryErrors(0) {}


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

  // Ensure slave work directory exists.
  CHECK_SOME(os::mkdir(flags.work_dir))
    << "Failed to create slave work directory '" << flags.work_dir << "'";

  Try<Resources> _resources = Containerizer::resources(flags);
  if (_resources.isError()) {
    EXIT(1) << "Failed to determine slave resources: " << _resources.error();
  }
  LOG(INFO) << "Slave resources: " << _resources.get();

  if (flags.attributes.isSome()) {
    attributes = Attributes::parse(flags.attributes.get());
  }

  // Determine our hostname or use the hostname provided.
  string hostname;

  if (flags.hostname.isNone()) {
    Try<string> result = net::getHostname(self().ip);

    if (result.isError()) {
      LOG(FATAL) << "Failed to get hostname: " << result.error();
    }

    hostname = result.get();
  } else {
    hostname = flags.hostname.get();
  }

  // Initialize slave info.
  info.set_hostname(hostname);
  info.set_port(self().port);
  info.mutable_resources()->CopyFrom(_resources.get());
  info.mutable_attributes()->CopyFrom(attributes);
  info.set_checkpoint(flags.checkpoint);

  LOG(INFO) << "Slave hostname: " << info.hostname();
  LOG(INFO) << "Slave checkpoint: " << stringify(flags.checkpoint);

  // The required 'webui_hostname' field has been deprecated and
  // changed to optional for now, but we still need to set it for
  // interoperating with code that expects it to be required (e.g., an
  // executor on an older release).
  // TODO(benh): Remove this after the deprecation cycle.
  info.set_webui_hostname(hostname);
  statusUpdateManager->initialize(flags, self());

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

  // Install protobuf handlers.
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
      &StatusUpdateMessage::update,
      &StatusUpdateMessage::pid);

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

  // Setup HTTP routes.
  route("/health", Http::HEALTH_HELP, lambda::bind(&Http::health, http, lambda::_1));
  route("/stats.json", None(), lambda::bind(&Http::stats, http, lambda::_1));
  route("/state.json", None(), lambda::bind(&Http::state, http, lambda::_1));

  if (flags.log_dir.isSome()) {
    Try<string> log = logging::getLogFile(google::INFO);
    if (log.isError()) {
      LOG(ERROR) << "Slave log file cannot be found: " << log.error();
    } else {
      files->attach(log.get(), "/slave/log")
        .onAny(defer(self(), &Self::fileAttached, lambda::_1, log.get()));
    }
  }

  // Check that the recover flag is valid.
  if (flags.recover != "reconnect" && flags.recover != "cleanup") {
    EXIT(1) << "Unknown option for 'recover' flag " << flags.recover
            << ". Please run the slave with '--help' to see the valid options";
  }

  // Do recovery.
  async(&state::recover, metaDir, flags.strict)
    .then(defer(self(), &Slave::recover, lambda::_1))
    .then(defer(self(), &Slave::_recover))
    .onAny(defer(self(), &Slave::__recover, lambda::_1));
}


void Slave::finalize()
{
  LOG(INFO) << "Slave terminating";

  // NOTE: We use 'frameworks.keys()' here because 'shutdownFramework'
  // can potentially remove a framework from 'frameworks'.
  foreach (const FrameworkID& frameworkId, frameworks.keys()) {
    // TODO(benh): Because a shut down isn't instantaneous (but has
    // a shut down/kill phases) we might not actually propogate all
    // the status updates appropriately here. Consider providing
    // an alternative function which skips the shut down phase and
    // simply does a kill (sending all status updates
    // immediately). Of course, this still isn't sufficient
    // because those status updates might get lost and we won't
    // resend them unless we build that into the system.
    // NOTE: We shut down the framework only if it has disabled
    // checkpointing. This is because slave recovery tests terminate
    // the slave to simulate slave restart.
    if (!frameworks[frameworkId]->info.checkpoint()) {
      shutdownFramework(UPID(), frameworkId);
    }
  }

  if (state == TERMINATING || flags.recover == "cleanup") {
    // We remove the "latest" symlink in meta directory, so that the
    // slave doesn't recover the state when it restarts and registers
    // as a new slave with the master.
    if (os::exists(paths::getLatestSlavePath(metaDir))) {
      CHECK_SOME(os::rm(paths::getLatestSlavePath(metaDir)));
    }
  }
}


void Slave::shutdown(const UPID& from)
{
  // Allow shutdown message only if
  // 1) Its a message received from the registered master or
  // 2) If its called locally (e.g tests)
  if (from && master != from) {
    LOG(WARNING) << "Ignoring shutdown message from " << from
                 << " because it is not from the registered master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  LOG(INFO) << "Slave asked to shut down by " << from;

  state = TERMINATING;

  if (frameworks.empty()) { // Terminate slave if there are no frameworks.
    terminate(self());
  } else {
    // NOTE: The slave will terminate after all
    // executors have terminated.
    // TODO(vinod): Wait until all updates have been acknowledged.
    // This is tricky without persistent state at master because the
    // slave might wait forever for status update acknowledgements,
    // since it cannot reliably know when a framework has shut down.
    // A short-term fix could be to wait for a certain time for ACKs
    // and then shutdown.
    // NOTE: We use 'frameworks.keys()' here because 'shutdownFramework'
    // can potentially remove a framework from 'frameworks'.
    foreach (const FrameworkID& frameworkId, frameworks.keys()) {
      shutdownFramework(from, frameworkId);
    }
  }
}


void Slave::fileAttached(const Future<Nothing>& result, const string& path)
{
  if (result.isReady()) {
    VLOG(1) << "Successfully attached file '" << path << "'";
  } else {
    LOG(ERROR) << "Failed to attach file '" << path << "': "
               << (result.isFailed() ? result.failure() : "discarded");
  }
}


// TODO(vinod/bmahler): Get rid of this helper.
Nothing Slave::detachFile(const string& path)
{
  files->detach(path);
  return Nothing();
}


void Slave::detected(const Future<Option<MasterInfo> >& _master)
{
  CHECK(state == DISCONNECTED ||
        state == RUNNING ||
        state == TERMINATING) << state;

  if (state != TERMINATING) {
    state = DISCONNECTED;
  }

  CHECK(!_master.isDiscarded());

  if (_master.isFailed()) {
    EXIT(1) << "Failed to detect a master: " << _master.failure();
  }

  if (_master.get().isSome()) {
    master = UPID(_master.get().get().pid());
  } else {
    master = None();
  }

  if (master.isSome()) {
    LOG(INFO) << "New master detected at " << master.get();
    link(master.get());

    // Inform the status updates manager about the new master.
    statusUpdateManager->newMasterDetected(master.get());

    if (state == TERMINATING) {
      LOG(INFO) << "Skipping registration because slave is terminating";
      return;
    }

    // The slave does not (re-)register if it is in the cleanup mode
    // because we do not want to accept new tasks.
    if (flags.recover == "cleanup") {
      LOG(INFO)
        << "Skipping registration because slave was started in cleanup mode";
      return;
    }

    doReliableRegistration();
  } else {
    LOG(INFO) << "Lost leading master";
  }

  // Keep detecting masters.
  LOG(INFO) << "Detecting new master";
  detector->detect(_master.get())
    .onAny(defer(self(), &Slave::detected, lambda::_1));
}


void Slave::registered(const UPID& from, const SlaveID& slaveId)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring registration message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  switch(state) {
    case DISCONNECTED: {
      CHECK_SOME(master);
      LOG(INFO) << "Registered with master " << master.get()
                << "; given slave ID " << slaveId;

      state = RUNNING;
      info.mutable_id()->CopyFrom(slaveId); // Store the slave id.

      if (flags.checkpoint) {
        // Create the slave meta directory.
        paths::createSlaveDirectory(metaDir, slaveId);

        // Checkpoint slave info.
        const string& path = paths::getSlaveInfoPath(metaDir, slaveId);

        LOG(INFO) << "Checkpointing SlaveInfo to '" << path << "'";
        CHECK_SOME(state::checkpoint(path, info));
      }
      break;
    }
    case RUNNING:
      // Already registered!
      if (!(info.id() == slaveId)) {
       EXIT(1) << "Registered but got wrong id: " << slaveId
               << "(expected: " << info.id() << "). Committing suicide";
      }
      CHECK_SOME(master);
      LOG(WARNING) << "Already registered with master " << master.get();
      break;
    case TERMINATING:
      LOG(WARNING) << "Ignoring registration because slave is terminating";
      break;
    case RECOVERING:
    default:
      LOG(FATAL) << "Unexpected slave state " << state;
      break;
  }
}


void Slave::reregistered(const UPID& from, const SlaveID& slaveId)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring re-registration message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  switch(state) {
    case DISCONNECTED:
      CHECK_SOME(master);
      LOG(INFO) << "Re-registered with master " << master.get();

      state = RUNNING;
      if (!(info.id() == slaveId)) {
        EXIT(1) << "Re-registered but got wrong id: " << slaveId
                << "(expected: " << info.id() << "). Committing suicide";
      }
      break;
    case RUNNING:
      // Already re-registered!
      if (!(info.id() == slaveId)) {
        EXIT(1) << "Re-registered but got wrong id: " << slaveId
                << "(expected: " << info.id() << "). Committing suicide";
      }
      CHECK_SOME(master);
      LOG(WARNING) << "Already re-registered with master " << master.get();
      break;
    case TERMINATING:
      LOG(WARNING) << "Ignoring re-registration because slave is terminating";
      break;
    case RECOVERING:
      // It's possible to receive a message intended for the previous
      // run of the slave here. Short term we can leave this as is and
      // crash in this case. Ideally responses can be tied to a
      // particular run of the slave, see:
      // https://issues.apache.org/jira/browse/MESOS-676
      // https://issues.apache.org/jira/browse/MESOS-677
    default:
      LOG(FATAL) << "Unexpected slave state " << state;
      break;
  }
}


void Slave::doReliableRegistration()
{
  if (master.isNone()) {
    LOG(INFO) << "Skipping registration because no master present";
    return;
  }

  if (state == RUNNING) { // Slave (re-)registered with the master.
    return;
  }

  CHECK(state == DISCONNECTED || state == TERMINATING) << state;

  if (info.id() == "") {
    // Slave started before master.
    // (Vinod): Is the above comment true?
    RegisterSlaveMessage message;
    message.mutable_slave()->CopyFrom(info);
    send(master.get(), message);
  } else {
    // Re-registering, so send tasks running.
    ReregisterSlaveMessage message;
    message.mutable_slave_id()->CopyFrom(info.id());
    message.mutable_slave()->CopyFrom(info);
    message.mutable_slave()->mutable_id()->CopyFrom(info.id());

    foreachvalue (Framework* framework, frameworks){
      foreachvalue (Executor* executor, framework->executors) {
        // Ignore terminated executors because they do not consume
        // any resources.
        if (executor->state == Executor::TERMINATED) {
          continue;
        }

        // Add launched, terminated, and queued tasks.
        foreach (Task* task, executor->launchedTasks.values()) {
          message.add_tasks()->CopyFrom(*task);
        }
        foreach (Task* task, executor->terminatedTasks.values()) {
          message.add_tasks()->CopyFrom(*task);
        }
        foreach (const TaskInfo& task, executor->queuedTasks.values()) {
          message.add_tasks()->CopyFrom(protobuf::createTask(
              task, TASK_STAGING, executor->id, framework->id));
        }

        // Do not re-register with Command Executors because the
        // master doesn't store them; they are generated by the slave.
        if (executor->commandExecutor) {
          // NOTE: We have to unset the executor id here for the task
          // because the master uses the absence of task.executor_id()
          // to detect command executors.
          for (int i = 0; i < message.tasks_size(); ++i) {
            message.mutable_tasks(i)->clear_executor_id();
          }
        } else {
          ExecutorInfo* executorInfo = message.add_executor_infos();
          executorInfo->MergeFrom(executor->info);

          // TODO(bmahler): Kill this in 0.15.0, as in 0.14.0 we've
          // added code into the Scheduler Driver to ensure the
          // framework id is set in ExecutorInfo, effectively making
          // it a required field.
          executorInfo->mutable_framework_id()->MergeFrom(framework->id);
        }
      }
    }

    CHECK_SOME(master);
    send(master.get(), message);
  }

  // Retry registration if necessary.
  delay(Seconds(1), self(), &Slave::doReliableRegistration);
}


// Helper to unschedule the path.
// TODO(vinod): Can we avoid this helper?
Future<bool> Slave::unschedule(const string& path)
{
  return gc.unschedule(path);
}


// TODO(vinod): Instead of crashing the slave on checkpoint errors,
// send TASK_LOST to the framework.
void Slave::runTask(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const FrameworkID& frameworkId,
    const string& pid,
    const TaskInfo& task)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring run task message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  LOG(INFO) << "Got assigned task " << task.task_id()
            << " for framework " << frameworkId;

  if (!(task.slave_id() == info.id())) {
    LOG(WARNING) << "Slave " << info.id() << " ignoring task " << task.task_id()
                 << " because it was intended for old slave " << task.slave_id();
    return;
  }

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  // TODO(bmahler): Also ignore if we're DISCONNECTED.
  if (state == RECOVERING || state == TERMINATING) {
    LOG(WARNING) << "Ignoring task " << task.task_id()
                 << " because the slave is " << state;
    // TODO(vinod): Consider sending a TASK_LOST here.
    // Currently it is tricky because 'statsuUpdate()'
    // ignores updates for unknown frameworks.
    return;
  }

  Future<bool> unschedule = true;

  // If we are about to create a new framework, unschedule the work
  // and meta directories from getting gc'ed.
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    // Unschedule framework work directory.
    string path = paths::getFrameworkPath(
        flags.work_dir, info.id(), frameworkId);

    if (os::exists(path)) {
      unschedule = unschedule.then(defer(self(), &Self::unschedule, path));
    }

    // Unschedule framework meta directory.
    path = paths::getFrameworkPath(metaDir, info.id(), frameworkId);
    if (os::exists(path)) {
      unschedule = unschedule.then(defer(self(), &Self::unschedule, path));
    }

    framework = new Framework(this, frameworkId, frameworkInfo, pid);
    frameworks[frameworkId] = framework;

    // Is this same framework in completedFrameworks? If so, move the completed
    // executors to this framework and remove it from that list.
    // TODO(brenden): Consider using stout/cache.hpp instead of boost
    // circular_buffer.
    for (boost::circular_buffer<Owned<Framework> >::iterator i =
        completedFrameworks.begin(); i != completedFrameworks.end(); ++i) {
      if ((*i)->id == frameworkId) {
        framework->completedExecutors = (*i)->completedExecutors;
        completedFrameworks.erase(i);
        break;
      }
    }
  }

  const ExecutorInfo& executorInfo = getExecutorInfo(frameworkId, task);
  const ExecutorID& executorId = executorInfo.executor_id();

  // We add the task to 'pending' to ensure the framework is not
  // removed and the framework and top level executor directories
  // are not scheduled for deletion before '_runTask()' is called.
  CHECK_NOTNULL(framework);
  framework->pending.put(executorId, task.task_id());

  // If we are about to create a new executor, unschedule the top
  // level work and meta directories from getting gc'ed.
  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    // Unschedule executor work directory.
    string path = paths::getExecutorPath(
        flags.work_dir, info.id(), frameworkId, executorId);

    if (os::exists(path)) {
      unschedule = unschedule.then(defer(self(), &Self::unschedule, path));
    }

    // Unschedule executor meta directory.
    path = paths::getExecutorPath(
        metaDir, info.id(), frameworkId, executorId);

    if (os::exists(path)) {
      unschedule = unschedule.then(defer(self(), &Self::unschedule, path));
    }
  }

  // Run the task after the unschedules are done.
  unschedule.onAny(
      defer(self(),
            &Self::_runTask,
            lambda::_1,
            frameworkInfo,
            frameworkId,
            pid,
            task));
}


void Slave::_runTask(
    const Future<bool>& future,
    const FrameworkInfo& frameworkInfo,
    const FrameworkID& frameworkId,
    const string& pid,
    const TaskInfo& task)
{
  LOG(INFO) << "Launching task " << task.task_id()
            << " for framework " << frameworkId;

  const ExecutorInfo& executorInfo = getExecutorInfo(frameworkId, task);
  const ExecutorID& executorId = executorInfo.executor_id();

  // Remove the pending task from framework.
  Framework* framework = getFramework(frameworkId);
  CHECK_NOTNULL(framework);

  framework->pending.remove(executorId, task.task_id());

  // We don't send a status update here because a terminating
  // framework cannot send acknowledgements.
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring run task " << task.task_id()
                 << " of framework " << frameworkId
                 << " because the framework is terminating";

    if (framework->executors.empty() && framework->pending.empty()) {
      removeFramework(framework);
    }
    return;
  }

  if (!future.isReady()) {
    LOG(ERROR) << "Failed to unschedule directories scheduled for gc: "
               << (future.isFailed() ? future.failure() : "future discarded");

    const StatusUpdate& update = protobuf::createStatusUpdate(
        frameworkId,
        info.id(),
        task.task_id(),
        TASK_LOST,
        "Could not launch the task because we failed to unschedule directories"
        " scheduled for gc");

    // TODO(vinod): Ensure that the status update manager reliably
    // delivers this update. Currently, we don't guarantee this
    // because removal of the framework causes the status update
    // manager to stop retrying for its un-acked updates.
    statusUpdate(update, UPID());

    if (framework->executors.empty() && framework->pending.empty()) {
      removeFramework(framework);
    }

    return;
  }

  // NOTE: The slave cannot be in 'RECOVERING' because the task would
  // have been rejected in 'runTask()' in that case.
  CHECK(state == DISCONNECTED || state == RUNNING || state == TERMINATING)
    << state;

  if (state == TERMINATING) {
    LOG(WARNING) << "Ignoring run task " << task.task_id()
                 << " of framework " << frameworkId
                 << " because the slave is terminating";

    // We don't send a TASK_LOST here because the slave is
    // terminating.
    return;
  }

  if (framework == NULL) {
    framework = new Framework(this, frameworkId, frameworkInfo, pid);
    frameworks[frameworkId] = framework;
  }

  CHECK_NOTNULL(framework);

  CHECK(framework->state == Framework::RUNNING) << framework->state;

  // Either send the task to an executor or start a new executor
  // and queue the task until the executor has started.
  Executor* executor = framework->getExecutor(executorId);

  if (executor == NULL) {
    executor = framework->launchExecutor(executorInfo, task);
  }

  CHECK_NOTNULL(executor);

  switch (executor->state) {
    case Executor::TERMINATING:
    case Executor::TERMINATED: {
      LOG(WARNING) << "Asked to run task '" << task.task_id()
                   << "' for framework " << frameworkId
                   << " with executor '" << executorId
                   << "' which is terminating/terminated";

      const StatusUpdate& update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          task.task_id(),
          TASK_LOST,
          "Executor terminating/terminated");

      statusUpdate(update, UPID());
      break;
    }
    case Executor::REGISTERING:
      // Checkpoint the task before we do anything else (this is a no-op
      // if the framework doesn't have checkpointing enabled).
      executor->checkpointTask(task);

      stats.tasks[TASK_STAGING]++;

      // Queue task if the executor has not yet registered.
      LOG(INFO) << "Queuing task '" << task.task_id()
                  << "' for executor " << executorId
                  << " of framework '" << frameworkId;

      executor->queuedTasks[task.task_id()] = task;
      break;
    case Executor::RUNNING: {
      // Checkpoint the task before we do anything else (this is a no-op
      // if the framework doesn't have checkpointing enabled).
      executor->checkpointTask(task);

      stats.tasks[TASK_STAGING]++;

      // Add the task and send it to the executor.
      executor->addTask(task);

      // Update the resources.
      // TODO(Charles Reiss): The isolator is not guaranteed to update
      // the resources before the executor acts on its RunTaskMessage.
      // TODO(idownes): Wait until this completes.
      containerizer->update(executor->containerId, executor->resources);

      LOG(INFO) << "Sending task '" << task.task_id()
                << "' to executor '" << executorId
                << "' of framework " << frameworkId;

      RunTaskMessage message;
      message.mutable_framework()->MergeFrom(framework->info);
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.set_pid(framework->pid);
      message.mutable_task()->MergeFrom(task);
      send(executor->pid, message);
      break;
    }
    default:
      LOG(FATAL) << " Executor '" << executor->id
                 << "' of framework " << framework->id
                 << " is in unexpected state " << executor->state;
      break;
  }
}


void Slave::killTask(
    const UPID& from,
    const FrameworkID& frameworkId,
    const TaskID& taskId)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring kill task message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  // TODO(bmahler): Also ignore if we're DISCONNECTED.
  if (state == RECOVERING || state == TERMINATING) {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because the slave is " << state;
    // TODO(vinod): Consider sending a TASK_LOST here.
    // Currently it is tricky because 'statusUpdate()'
    // ignores updates for unknown frameworks.
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Ignoring kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such framework is running";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // We don't send a status update here because a terminating
  // framework cannot send acknowledgements.
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring kill task " << taskId
                 << " of framework " << frameworkId
                 << " because the framework is terminating";
    return;
  }

  Executor* executor = framework->getExecutor(taskId);
  if (executor == NULL) {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no corresponding executor is running";

    // We send a TASK_LOST update because this task might have never
    // been launched on this slave!
    const StatusUpdate& update = protobuf::createStatusUpdate(
        frameworkId, info.id(), taskId, TASK_LOST, "Cannot find executor");

    statusUpdate(update, UPID());
    return;
  }

  switch (executor->state) {
    case Executor::REGISTERING: {
      // The executor hasn't registered yet.
      // NOTE: Sending a TASK_KILLED update removes the task from
      // Executor::queuedTasks, so that if the executor registers at
      // a later point in time, it won't get this task.
      const StatusUpdate& update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          taskId,
          TASK_KILLED,
          "Unregistered executor",
          executor->id);

      statusUpdate(update, UPID());

      // This executor no longer has any running tasks, so kill it.
      if (executor->queuedTasks.empty()) {
        CHECK(executor->launchedTasks.empty())
            << " Unregistered executor " << executor->id
            << " has launched tasks";

        LOG(WARNING) << "Killing the unregistered executor '" << executor->id
                     << "' of framework " << framework->id
                     << " because it has no tasks";
        containerizer->destroy(executor->containerId);
      }
      break;
    }
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      LOG(WARNING) << "Ignoring kill task " << taskId
                   << " of framework " << frameworkId
                   << " because the executor '" << executor->id
                   << "' is terminating/terminated";
      break;
    case Executor::RUNNING: {
      // Send a message to the executor and wait for
      // it to send us a status update.
      KillTaskMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_task_id()->MergeFrom(taskId);
      send(executor->pid, message);
      break;
    }
    default:
      LOG(FATAL) << " Executor '" << executor->id
                 << "' of framework " << framework->id
                 << " is in unexpected state " << executor->state;
      break;
  }
}


// TODO(benh): Consider sending a boolean that specifies if the
// shut down should be graceful or immediate. Likewise, consider
// sending back a shut down acknowledgement, because otherwise you
// could get into a state where a shut down was sent, dropped, and
// therefore never processed.
void Slave::shutdownFramework(
    const UPID& from,
    const FrameworkID& frameworkId)
{
  // Allow shutdownFramework() only if
  // its called directly (e.g. Slave::finalize()) or
  // its a message from the currently registered master.
  if (from && master != from) {
    LOG(WARNING) << "Ignoring shutdown framework message for " << frameworkId
                 << " from " << from
                 << " because it is not from the registered master ("
                 << (master.isSome() ? stringify(master.get()) : "None") << ")";
    return;
  }

  LOG(INFO) << "Asked to shut down framework " << frameworkId
            << " by " << from;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state == RECOVERING || state == DISCONNECTED) {
    LOG(WARNING) << "Ignoring shutdown framework message for " << frameworkId
                 << " because the slave has not yet registered with the master";
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Cannot shut down unknown framework " << frameworkId;
    return;
  }

  switch (framework->state) {
    case Framework::TERMINATING:
      LOG(WARNING) << "Ignoring shutdown framework " << framework->id
                   << " because it is terminating";
      break;
    case Framework::RUNNING:
      LOG(INFO) << "Shutting down framework " << framework->id;

      framework->state = Framework::TERMINATING;

      // Shut down all executors of this framework.
      // NOTE: We use 'executors.keys()' here because 'shutdownExecutor'
      // and 'removeExecutor' can remove an executor from 'executors'.
      foreach (const ExecutorID& executorId, framework->executors.keys()) {
        Executor* executor = framework->executors[executorId];
        CHECK(executor->state == Executor::REGISTERING ||
              executor->state == Executor::RUNNING ||
              executor->state == Executor::TERMINATING ||
              executor->state == Executor::TERMINATED)
          << executor->state;

        if (executor->state == Executor::REGISTERING ||
            executor->state == Executor::RUNNING) {
          shutdownExecutor(framework, executor);
        } else if (executor->state == Executor::TERMINATED) {
          // NOTE: We call remove here to ensure we can remove an
          // executor (of a terminating framework) that is terminated
          // but waiting for acknowledgements.
          removeExecutor(framework, executor);
        } else {
          // Executor is terminating. Ignore.
        }
      }

      // Remove this framework if it has no pending executors and tasks.
      if (framework->executors.empty() && framework->pending.empty()) {
        removeFramework(framework);
      }
      break;
    default:
      LOG(FATAL) << "Framework " << frameworkId
                 << " is in unexpected state " << framework->state;
      break;
  }
}


void Slave::schedulerMessage(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state != RUNNING) {
    LOG(WARNING) << "Dropping message from framework "<< frameworkId
                 << " because the slave is in " << state << " state";
    stats.invalidFrameworkMessages++;
    return;
  }


  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Dropping message from framework "<< frameworkId
                 << " because framework does not exist";
    stats.invalidFrameworkMessages++;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Dropping message from framework "<< frameworkId
                 << " because framework is terminating";
    stats.invalidFrameworkMessages++;
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Dropping message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because executor does not exist";
    stats.invalidFrameworkMessages++;
    return;
  }

  switch (executor->state) {
    case Executor::REGISTERING:
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      // TODO(*): If executor is not yet registered, queue framework
      // message? It's probably okay to just drop it since frameworks
      // can have the executor send a message to the master to say when
      // it's ready.
      LOG(WARNING) << "Dropping message for executor '"
                   << executorId << "' of framework " << frameworkId
                   << " because executor is not running";
      stats.invalidFrameworkMessages++;
      break;
    case Executor::RUNNING: {
      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(executor->pid, message);
      stats.validFrameworkMessages++;
      break;
    }
    default:
      LOG(FATAL) << " Executor '" << executor->id
                 << "' of framework " << framework->id
                 << " is in unexpected state " << executor->state;
      break;
  }
}


void Slave::updateFramework(const FrameworkID& frameworkId, const string& pid)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state != RUNNING) {
    LOG(WARNING) << "Dropping updateFramework message for "<< frameworkId
                 << " because the slave is in " << state << " state";
    stats.invalidFrameworkMessages++;
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Ignoring updating pid for framework " << frameworkId
                 << " because it does not exist";
    return;
  }

  switch (framework->state) {
    case Framework::TERMINATING:
      LOG(WARNING) << "Ignoring updating pid for framework " << frameworkId
                   << " because it is terminating";
      break;
    case Framework::RUNNING: {
      LOG(INFO) << "Updating framework " << frameworkId << " pid to " << pid;

      framework->pid = pid;
      if (framework->info.checkpoint()) {
        // Checkpoint the framework pid.
        const string& path = paths::getFrameworkPidPath(
            metaDir, info.id(), frameworkId);

        LOG(INFO) << "Checkpointing framework pid '"
                  << framework->pid << "' to '" << path << "'";
        CHECK_SOME(state::checkpoint(path, framework->pid));
      }

      // Inform status update manager to immediately resend any pending
      // updates.
      statusUpdateManager->flush();

      break;
    }
    default:
      LOG(FATAL) << "Framework " << framework->id
                << " is in unexpected state " << framework->state;
      break;
  }
}


void Slave::statusUpdateAcknowledgement(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const TaskID& taskId,
    const string& uuid)
{
  statusUpdateManager->acknowledgement(
      taskId, frameworkId, UUID::fromBytes(uuid))
    .onAny(defer(self(),
                 &Slave::_statusUpdateAcknowledgement,
                 lambda::_1,
                 taskId,
                 frameworkId,
                 UUID::fromBytes(uuid)));
}


void Slave::_statusUpdateAcknowledgement(
    const Future<bool>& future,
    const TaskID& taskId,
    const FrameworkID& frameworkId,
    const UUID& uuid)
{
  // The future could fail if this is a duplicate status update acknowledgement.
  if (!future.isReady()) {
    LOG(ERROR) << "Failed to handle status update acknowledgement (UUID: "
               << uuid << ") for task " << taskId
               << " of framework " << frameworkId << ": "
               << (future.isFailed() ? future.failure() : "future discarded");
    return;
  }

  VLOG(1) << "Status update manager successfully handled status update"
          << " acknowledgement (UUID: " << uuid
          << ") for task " << taskId
          << " of framework " << frameworkId;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(ERROR) << "Status update acknowledgement (UUID: " << uuid
               << ") for task " << taskId
               << " of unknown framework " << frameworkId;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // Find the executor that has this update.
  Executor* executor = framework->getExecutor(taskId);
  if (executor == NULL) {
    LOG(ERROR) << "Status update acknowledgement (UUID: " << uuid
              << ") for task " << taskId
               << " of unknown executor";
    return;
  }

  CHECK(executor->state == Executor::REGISTERING ||
        executor->state == Executor::RUNNING ||
        executor->state == Executor::TERMINATING ||
        executor->state == Executor::TERMINATED)
    << executor->state;

  // If the task has reached terminal state and all its updates have
  // been acknowledged, mark it completed.
  if (executor->terminatedTasks.contains(taskId) && !future.get()) {
    executor->completeTask(taskId);
  }

  // Remove the executor if it has terminated and there are no more
  // incomplete tasks.
  if (executor->state == Executor::TERMINATED && !executor->incompleteTasks()) {
    removeExecutor(framework, executor);
  }

  // Remove this framework if it has no pending executors and tasks.
  if (framework->executors.empty() && framework->pending.empty()) {
    removeFramework(framework);
  }
}


void Slave::registerExecutor(
    const UPID& from,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  LOG(INFO) << "Got registration for executor '" << executorId
            << "' of framework " << frameworkId;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state == RECOVERING) {
    LOG(WARNING) << "Shutting down executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the slave is still recovering";
    reply(ShutdownExecutorMessage());
    return;
  }

  if (state == TERMINATING) {
    LOG(WARNING) << "Shutting down executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the slave is terminating";
    reply(ShutdownExecutorMessage());
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << " Shutting down executor '" << executorId
                 << "' as the framework " << frameworkId
                 << " does not exist";

    reply(ShutdownExecutorMessage());
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << " Shutting down executor '" << executorId
                 << "' as the framework " << frameworkId
                 << " is terminating";

    reply(ShutdownExecutorMessage());
    return;
  }

  Executor* executor = framework->getExecutor(executorId);

  // Check the status of the executor.
  if (executor == NULL) {
    LOG(WARNING) << "Unexpected executor '" << executorId
                 << "' registering for framework " << frameworkId;
    reply(ShutdownExecutorMessage());
    return;
  }

  switch (executor->state) {
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      // TERMINATED is possible if the executor forks, the parent process
      // terminates and the child process (driver) tries to register!
    case Executor::RUNNING:
      LOG(WARNING) << "Shutting down executor '" << executorId
                   << "' of framework " << frameworkId
                   << " because it is in unexpected state " << executor->state;
      reply(ShutdownExecutorMessage());
      break;
    case Executor::REGISTERING: {
      executor->state = Executor::RUNNING;

      // Save the pid for the executor.
      executor->pid = from;

      if (framework->info.checkpoint()) {
        // TODO(vinod): This checkpointing should be done
        // asynchronously as it is in the fast path of the slave!

        // Checkpoint the libprocess pid.
        string path = paths::getLibprocessPidPath(
            metaDir,
            info.id(),
            executor->frameworkId,
            executor->id,
            executor->containerId);

        LOG(INFO) << "Checkpointing executor pid '"
                  << executor->pid << "' to '" << path << "'";
        CHECK_SOME(state::checkpoint(path, executor->pid));
      }

      // First account for the tasks we're about to start.
      // TODO(vinod): Use foreachvalue instead once LinkedHashmap
      // supports it.
      foreach (const TaskInfo& task, executor->queuedTasks.values()) {
        // Add the task to the executor.
        executor->addTask(task);
      }

      // Now that the executor is up, set its resource limits
      // including the currently queued tasks.
      // TODO(Charles Reiss): We don't actually have a guarantee
      // that this will be delivered or (where necessary) acted on
      // before the executor gets its RunTaskMessages.
      // TODO(idownes): Wait until this completes.
      containerizer->update(executor->containerId, executor->resources);

      // Tell executor it's registered and give it any queued tasks.
      ExecutorRegisteredMessage message;
      message.mutable_executor_info()->MergeFrom(executor->info);
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.mutable_framework_info()->MergeFrom(framework->info);
      message.mutable_slave_id()->MergeFrom(info.id());
      message.mutable_slave_info()->MergeFrom(info);
      send(executor->pid, message);

      // TODO(vinod): Use foreachvalue instead once LinkedHashmap
      // supports it.
      foreach (const TaskInfo& task, executor->queuedTasks.values()) {
        LOG(INFO) << "Flushing queued task " << task.task_id()
                  << " for executor '" << executor->id << "'"
                  << " of framework " << framework->id;

        stats.tasks[TASK_STAGING]++;

        RunTaskMessage message;
        message.mutable_framework_id()->MergeFrom(framework->id);
        message.mutable_framework()->MergeFrom(framework->info);
        message.set_pid(framework->pid);
        message.mutable_task()->MergeFrom(task);
        send(executor->pid, message);
      }

      executor->queuedTasks.clear();
      break;
    }
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id
                 << " is in unexpected state " << executor->state;
      break;
  }
}


void Slave::reregisterExecutor(
    const UPID& from,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const vector<TaskInfo>& tasks,
    const vector<StatusUpdate>& updates)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state != RECOVERING) {
    LOG(WARNING) << "Shutting down executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the slave is not in recovery mode";
    reply(ShutdownExecutorMessage());
    return;
  }

  LOG(INFO) << "Re-registering executor " << executorId
            << " of framework " << frameworkId;

  CHECK(frameworks.contains(frameworkId))
    << "Unknown framework " << frameworkId;

  Framework* framework = frameworks[frameworkId];

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << " Shutting down executor '" << executorId
                 << "' as the framework " << frameworkId
                 << " is terminating";

    reply(ShutdownExecutorMessage());
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  CHECK_NOTNULL(executor);

  switch (executor->state) {
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      // TERMINATED is possible if the executor forks, the parent process
      // terminates and the child process (driver) tries to register!
    case Executor::RUNNING:
      LOG(WARNING) << "Shutting down executor '" << executorId
                   << "' of framework " << frameworkId
                   << " because it is in unexpected state " << executor->state;
      reply(ShutdownExecutorMessage());
      break;
    case Executor::REGISTERING: {
      executor->state = Executor::RUNNING;

      executor->pid = from; // Update the pid.

      // Send re-registration message to the executor.
      ExecutorReregisteredMessage message;
      message.mutable_slave_id()->MergeFrom(info.id());
      message.mutable_slave_info()->MergeFrom(info);
      send(executor->pid, message);

      // Handle all the pending updates.
      // The status update manager might have already checkpointed some
      // of these pending updates (for example, if the slave died right
      // after it checkpointed the update but before it could send the
      // ACK to the executor). This is ok because the status update
      // manager correctly handles duplicate updates.
      foreach (const StatusUpdate& update, updates) {
        // NOTE: This also updates the executor's resources!
        statusUpdate(update, executor->pid);
      }

      // Tell the containerizer to update the resources.
      // TODO(idownes): Wait until this completes.
      containerizer->update(executor->containerId, executor->resources);

      hashmap<TaskID, TaskInfo> unackedTasks;
      foreach (const TaskInfo& task, tasks) {
        unackedTasks[task.task_id()] = task;
      }

      // Now, if there is any task still in STAGING state and not in
      // unacknowledged 'tasks' known to the executor, the slave must
      // have died before the executor received the task! We should
      // transition it to TASK_LOST. We only consider/store
      // unacknowledged 'tasks' at the executor driver because if a
      // task has been acknowledged, the slave must have received
      // an update for that task and transitioned it out of STAGING!
      // TODO(vinod): Consider checkpointing 'TaskInfo' instead of
      // 'Task' so that we can relaunch such tasks! Currently we
      // don't do it because 'TaskInfo.data' could be huge.
      // TODO(vinod): Use foreachvalue instead once LinkedHashmap
      // supports it.
      foreach (Task* task, executor->launchedTasks.values()) {
        if (task->state() == TASK_STAGING &&
            !unackedTasks.contains(task->task_id())) {

          LOG(INFO)
            << "Transitioning STAGED task " << task->task_id() << " to LOST"
            << " because it is unknown to the executor " << executorId;

          const StatusUpdate& update = protobuf::createStatusUpdate(
              frameworkId,
              info.id(),
              task->task_id(),
              TASK_LOST,
              "Task launched during slave restart",
              executorId);

          statusUpdate(update, UPID());
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id
                 << " is in unexpected state " << executor->state;
      break;
  }
}



void Slave::reregisterExecutorTimeout()
{
  CHECK(state == RECOVERING || state == TERMINATING) << state;

  LOG(INFO) << "Cleaning up un-reregistered executors";

  foreachvalue (Framework* framework, frameworks) {
    CHECK(framework->state == Framework::RUNNING ||
          framework->state == Framework::TERMINATING)
      << framework->state;

    foreachvalue (Executor* executor, framework->executors) {
      switch (executor->state) {
        case Executor::RUNNING:     // Executor re-registered.
        case Executor::TERMINATING:
        case Executor::TERMINATED:
          break;
        case Executor::REGISTERING:
          // If we are here, the executor must have been hung and not
          // exited! This is because if the executor properly exited,
          // it should have already been identified by the isolator
          // (via the reaper) and cleaned up!
          LOG(INFO) << "Killing un-reregistered executor '" << executor->id
                    << "' of framework " << framework->id;

          executor->state = Executor::TERMINATING;

          containerizer->destroy(executor->containerId);
          break;
        default:
          LOG(FATAL) << "Executor '" << executor->id
                     << "' of framework " << framework->id
                     << " is in unexpected state " << executor->state;
          break;
      }
    }
  }

  // Signal the end of recovery.
  recovered.set(Nothing());
}


// This can be called in two ways:
// 1) When a status update from the executor is received.
// 2) When slave generates task updates (e.g LOST/KILLED/FAILED).
// NOTE: We set the pid in 'Slave::_statusUpdate()' to 'pid' so that
// whoever sent this update will get an ACK. This is important because
// we allow executors to send updates for tasks that belong to other
// executors. Currently we allow this because we cannot guarantee
// reliable delivery of status updates. Since executor driver caches
// unacked updates it is important that whoever sent the update gets
// acknowledgement for it.
void Slave::statusUpdate(const StatusUpdate& update, const UPID& pid)
{
  LOG(INFO) << "Handling status update " << update << " from " << pid;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  const TaskStatus& status = update.status();

  Framework* framework = getFramework(update.framework_id());
  if (framework == NULL) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " for unknown framework " << update.framework_id();
    stats.invalidStatusUpdates++;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // We don't send update when a framework is terminating because
  // it cannot send acknowledgements.
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " for terminating framework " << framework->id;
    stats.invalidStatusUpdates++;
    return;
  }

  Executor* executor = framework->getExecutor(status.task_id());
  if (executor == NULL) {
    LOG(WARNING)  << "Could not find the executor for "
                  << "status update " << update;
    stats.validStatusUpdates++;

    // NOTE: We forward the update here because this update could be
    // generated by the slave when the executor is unknown to it
    // (e.g., killTask(), _runTask()) or sent by an executor for a
    // task that belongs to another executor.
    // We also end up here if 1) the previous slave died after
    // checkpointing a _terminal_ update but before it could send an
    // ACK to the executor AND 2) after recovery the status update
    // manager successfully retried the update, got the ACK from the
    // scheduler and cleaned up the stream before the executor
    // re-registered. In this case, the slave cannot find the executor
    // corresponding to this task because the task has been moved to
    // 'Executor::completedTasks'.
    statusUpdateManager->update(update, info.id())
      .onAny(defer(self(), &Slave::_statusUpdate, lambda::_1, update, pid));

    return;
  }

  CHECK(executor->state == Executor::REGISTERING ||
        executor->state == Executor::RUNNING ||
        executor->state == Executor::TERMINATING ||
        executor->state == Executor::TERMINATED)
    << executor->state;

  // TODO(vinod): Revisit these semantics when we disallow executors
  // from sending updates for tasks that belong to other executors.
  if (pid != UPID() && executor->pid != pid) {
    LOG(WARNING) << "Received status update " << update << " from " << pid
                 << " on behalf of a different executor " << executor->id
                 << " (" << executor->pid << ")";
  }

  stats.tasks[update.status().state()]++;
  stats.validStatusUpdates++;

  executor->updateTaskState(status);

  // Handle the task appropriately if it is terminated.
  // TODO(vinod): Revisit these semantics when we disallow duplicate
  // terminal updates (e.g., when slave recovery is always enabled).
  if (protobuf::isTerminalState(status.state()) &&
      (executor->queuedTasks.contains(status.task_id()) ||
       executor->launchedTasks.contains(status.task_id()))) {
    executor->terminateTask(status.task_id(), status.state());

    // Tell the isolator to update the resources.
    // TODO(idownes): Wait until this completes.
    containerizer->update(executor->containerId, executor->resources);
  }

  if (executor->checkpoint) {
    // Ask the status update manager to checkpoint and reliably send the update.
    statusUpdateManager->update(
        update,
        info.id(),
        executor->id,
        executor->containerId)
      .onAny(defer(self(),
                   &Slave::_statusUpdate,
                   lambda::_1,
                   update,
                   pid));
  } else {
    // Ask the status update manager to just retry the update.
    statusUpdateManager->update(update, info.id())
      .onAny(defer(self(),
                   &Slave::_statusUpdate,
                   lambda::_1,
                   update,
                   pid));
  }
}


void Slave::_statusUpdate(
    const Future<Nothing>& future,
    const StatusUpdate& update,
    const UPID& pid)
{
  if (!future.isReady()) {
    LOG(FATAL) << "Failed to handle status update " << update << ": "
               << (future.isFailed() ? future.failure() : "future discarded");
    return;
  }

  VLOG(1) << "Status update manager successfully handled status update "
          << update;

  // Status update manager successfully handled the status update.
  // Acknowledge the executor, if we have a valid pid.
  if (pid != UPID()) {
    LOG(INFO) << "Sending acknowledgement for status update " << update
              << " to " << pid;
    StatusUpdateAcknowledgementMessage message;
    message.mutable_framework_id()->MergeFrom(update.framework_id());
    message.mutable_slave_id()->MergeFrom(update.slave_id());
    message.mutable_task_id()->MergeFrom(update.status().task_id());
    message.set_uuid(update.uuid());

    send(pid, message);
  }
}


void Slave::executorMessage(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state != RUNNING) {
    LOG(WARNING) << "Dropping framework message from executor "
                 << executorId << " to framework " << frameworkId
                 << " because the slave is in " << state << " state";
    stats.invalidFrameworkMessages++;
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Cannot send framework message from executor "
                 << executorId << " to framework " << frameworkId
                 << " because framework does not exist";
    stats.invalidFrameworkMessages++;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring framework message from executor "
                 << executorId << " to framework " << frameworkId
                 << " because framework is terminating";
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
  LOG(INFO) << pid << " exited";

  if (master.isNone() || master.get() == pid) {
    LOG(WARNING) << "Master disconnected!"
                 << " Waiting for a new master to be elected";
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


ExecutorInfo Slave::getExecutorInfo(
    const FrameworkID& frameworkId,
    const TaskInfo& task)
{
  CHECK_NE(task.has_executor(), task.has_command())
    << "Task " << task.task_id()
    << " should have either CommandInfo or ExecutorInfo set but not both";

  if (task.has_command()) {
    ExecutorInfo executor;

    // Command executors share the same id as the task.
    executor.mutable_executor_id()->set_value(task.task_id().value());

    executor.mutable_framework_id()->CopyFrom(frameworkId);

    // Prepare an executor name which includes information on the
    // command being launched.
    string name =
      "(Task: " + task.task_id().value() + ") " + "(Command: sh -c '";

    if (task.command().value().length() > 15) {
      name += task.command().value().substr(0, 12) + "...')";
    } else {
      name += task.command().value() + "')";
    }

    executor.set_name("Command Executor " + name);
    executor.set_source(task.task_id().value());

    // Copy the CommandInfo to get the URIs and environment, but
    // update it to invoke 'mesos-executor' (unless we couldn't
    // resolve 'mesos-executor' via 'realpath', in which case just
    // echo the error and exit).
    executor.mutable_command()->MergeFrom(task.command());

    Result<string> path = os::realpath(
        path::join(flags.launcher_dir, "mesos-executor"));

    if (path.isSome()) {
      executor.mutable_command()->set_value(path.get());
    } else {
      executor.mutable_command()->set_value(
          "echo '" +
          (path.isError()
           ? path.error()
           : "No such file or directory") +
          "'; exit 1");
    }

    return executor;
  }

  return task.executor();
}


void _monitor(
    const Future<Nothing>& monitor,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  if (!monitor.isReady()) {
    LOG(ERROR) << "Failed to monitor container '" << containerId
               << "' for executor '" << executorId
               << "' of framework '" << frameworkId
               << ":" << (monitor.isFailed() ? monitor.failure() : "discarded");
  }
}

void Slave::executorStarted(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const Future<Nothing>& future)
{
  if (!future.isReady()) {
    // The containerizer will clean up if the launch fails we'll just log this
    // and leave the executor registration to timeout.
    LOG(ERROR) << "Container '" << containerId
               << "' for executor '" << executorId
               << "' of framework '" << frameworkId
               << "' failed to start: "
               << (future.isFailed() ? future.failure() : " future discarded");
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Framework '" << frameworkId
                 << "' for executor '" << executorId
                 << "' is no longer valid";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Killing executor '" << executorId
                 << "' of framework '" << frameworkId
                 << "' because the framework is terminating";
    containerizer->destroy(containerId);
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Killing unknown executor '" << executorId
                 << "' of framework '" << frameworkId << "'";
    containerizer->destroy(containerId);
    return;
  }

  switch (executor->state) {
    case Executor::TERMINATING:
      LOG(WARNING) << "Killing executor '" << executorId
                   << "' of framework '" << frameworkId
                   << "' because the executor is terminating";
      containerizer->destroy(containerId);
      break;
    case Executor::REGISTERING:
    case Executor::RUNNING:
      LOG(INFO) << "Monitoring executor '" << executorId
                << "' of framework '" << frameworkId
                << "' in container '" << containerId << "'";
      // Start monitoring the container's resources.
      monitor.start(
          containerId,
          executor->info,
          flags.resource_monitoring_interval)
        .onAny(lambda::bind(_monitor,
                            lambda::_1,
                            frameworkId,
                            executorId,
                            containerId));
      break;
    case Executor::TERMINATED:
    default:
      LOG(FATAL) << " Executor '" << executorId
                 << "' of framework '" << frameworkId
                 << "' is in an unexpected state " << executor->state;
      break;
  }
}


void _unmonitor(
    const Future<Nothing>& watch,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId);


// Called by the isolator when an executor process terminates.
void Slave::executorTerminated(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Future<Containerizer::Termination>& termination)
{
  int status;
  // A termination failure indicates the containerizer could not destroy a
  // container.
  // TODO(idownes): This is a serious error so consider aborting the slave if
  // this occurs.
  if (!termination.isReady()) {
    LOG(ERROR) << "Termination of executor '" << executorId
               << "' of framework '" << frameworkId
               << "' failed: "
               << (termination.isFailed()
                   ? termination.failure()
                   : "discarded");
    // Set a special status for failure.
    status = -1;
  } else if (termination.get().status.isNone()) {
    LOG(INFO) << "Executor '" << executorId
              << "' of framework " << frameworkId
              << " has terminated with unknown status";
    // Set a special status for None.
    status = -1;
  } else {
    status = termination.get().status.get();
    LOG(INFO) << "Executor '" << executorId
              << "' of framework " << frameworkId
              << (WIFEXITED(status)
                  ? " has exited with status "
                  : " has terminated with signal ")
              << (WIFEXITED(status)
                  ? stringify(WEXITSTATUS(status))
                  : strsignal(WTERMSIG(status)));
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Framework " << frameworkId
                 << " for executor '" << executorId
                 << "' does not exist";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Executor '" << executorId
                 << "' of framework " << frameworkId
                 << " does not exist";
    return;
  }

  switch (executor->state) {
    case Executor::REGISTERING:
    case Executor::RUNNING:
    case Executor::TERMINATING: {
      executor->state = Executor::TERMINATED;

      // Stop monitoring the executor's container.
      monitor.stop(executor->containerId)
        .onAny(lambda::bind(_unmonitor, lambda::_1, frameworkId, executorId));

      // Transition all live tasks to TASK_LOST/TASK_FAILED.
      // If the containerizer killed  the executor (e.g., due to OOM event)
      // or if this is a command executor, we send TASK_FAILED status updates
      // instead of TASK_LOST.
      // NOTE: We don't send updates if the framework is terminating
      // because we don't want the status update manager to keep retrying
      // these updates since it won't receive ACKs from the scheduler.  Also,
      // the status update manager should have already cleaned up all the
      // status update streams for a framework that is terminating.
      if (framework->state != Framework::TERMINATING) {
        StatusUpdate update;

        // Transition all live launched tasks.
        // TODO(vinod): Use foreachvalue instead once LinkedHashmap
        // supports it.
        foreach (Task* task, executor->launchedTasks.values()) {
          if (!protobuf::isTerminalState(task->state())) {
            mesos::TaskState taskState;
            if ((termination.isReady() && termination.get().killed) ||
                 executor->commandExecutor) {
              taskState = TASK_FAILED;
            } else {
              taskState = TASK_LOST;
            }
            statusUpdate(protobuf::createStatusUpdate(
                frameworkId,
                info.id(),
                task->task_id(),
                taskState,
                termination.isReady() ? termination.get().message :
                                        "Abnormal executor termination",
                executorId),
                UPID());
          }
        }

        // Transition all queued tasks.
        // TODO(vinod): Use foreachvalue instead once LinkedHashmap
        // supports it.
        foreach (const TaskInfo& task, executor->queuedTasks.values()) {
          mesos::TaskState taskState;
          if ((termination.isReady() && termination.get().killed) ||
               executor->commandExecutor) {
            taskState = TASK_FAILED;
          } else {
            taskState = TASK_LOST;
          }
          statusUpdate(protobuf::createStatusUpdate(
              frameworkId,
              info.id(),
              task.task_id(),
              taskState,
              termination.isReady() ? termination.get().message :
                                      "Abnormal executor termination",
              executorId),
              UPID());
        }
      }

      // Only send ExitedExecutorMessage if it is not a Command
      // Executor because the master doesn't store them; they are
      // generated by the slave.
      if (!executor->commandExecutor) {
        ExitedExecutorMessage message;
        message.mutable_slave_id()->MergeFrom(info.id());
        message.mutable_framework_id()->MergeFrom(frameworkId);
        message.mutable_executor_id()->MergeFrom(executorId);
        message.set_status(status);

        if (master.isSome()) { send(master.get(), message); }
      }

      // Remove the executor if either the framework is terminating or
      // there are no incomplete tasks.
      if (framework->state == Framework::TERMINATING ||
          !executor->incompleteTasks()) {
        removeExecutor(framework, executor);
      }

      // Remove this framework if it has no pending executors and tasks.
      if (framework->executors.empty() && framework->pending.empty()) {
        removeFramework(framework);
      }
      break;
    }
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id
                 << " in unexpected state " << executor->state;
      break;
  }
}


void Slave::removeExecutor(Framework* framework, Executor* executor)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(executor);

  LOG(INFO) << "Cleaning up executor '" << executor->id
            << "' of framework " << framework->id;

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // Check that this executor has terminated and either has no
  // pending updates or the framework is terminating. We don't
  // care for pending updates when a framework is terminating
  // because the framework cannot ACK them.
  CHECK(executor->state == Executor::TERMINATED) << executor->state;
  CHECK(framework->state == Framework::TERMINATING ||
        !executor->incompleteTasks());

  // Write a sentinel file to indicate that this executor
  // is completed.
  if (executor->checkpoint) {
    const string& path = paths::getExecutorSentinelPath(
        metaDir, info.id(), framework->id, executor->id, executor->containerId);
    CHECK_SOME(os::touch(path));
  }

  // TODO(vinod): Move the responsibility of gc'ing to the
  // Executor struct.

  // Schedule the executor run work directory to get garbage collected.
  const string& path = paths::getExecutorRunPath(
      flags.work_dir,
      info.id(),
      framework->id,
      executor->id,
      executor->containerId);

  os::utime(path); // Update the modification time.
  garbageCollect(path)
    .then(defer(self(), &Self::detachFile, path));

  // Schedule the top level executor work directory, only if the
  // framework doesn't have any 'pending' tasks for this executor.
  if (!framework->pending.contains(executor->id)) {
    const string& path = paths::getExecutorPath(
        flags.work_dir, info.id(), framework->id, executor->id);

    os::utime(path); // Update the modification time.
    garbageCollect(path);
  }

  if (executor->checkpoint) {
    // Schedule the executor run meta directory to get garbage collected.
    const string& path = paths::getExecutorRunPath(
        metaDir, info.id(), framework->id, executor->id, executor->containerId);

    os::utime(path); // Update the modification time.
    garbageCollect(path);

    // Schedule the top level executor meta directory, only if the
    // framework doesn't have any 'pending' tasks for this executor.
    if (!framework->pending.contains(executor->id)) {
      const string& path = paths::getExecutorPath(
          metaDir, info.id(), framework->id, executor->id);

      os::utime(path); // Update the modification time.
      garbageCollect(path);
    }
  }

  framework->destroyExecutor(executor->id);
}


void Slave::removeFramework(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO)<< "Cleaning up framework " << framework->id;

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING);

  // The invariant here is that a framework should not be removed
  // if it has either pending executors or pending tasks.
  CHECK(framework->executors.empty());
  CHECK(framework->pending.empty());

  // Close all status update streams for this framework.
  statusUpdateManager->cleanup(framework->id);

  // Schedule the framework work and meta directories for garbage
  // collection.
  // TODO(vinod): Move the responsibility of gc'ing to the
  // Framework struct.

  const string& path = paths::getFrameworkPath(
      flags.work_dir, info.id(), framework->id);

  os::utime(path); // Update the modification time.
  garbageCollect(path);

  if (framework->info.checkpoint()) {
    // Schedule the framework meta directory to get garbage collected.
    const string& path = paths::getFrameworkPath(
        metaDir, info.id(), framework->id);

    os::utime(path); // Update the modification time.
    garbageCollect(path);
  }

  frameworks.erase(framework->id);

  // Pass ownership of the framework pointer.
  completedFrameworks.push_back(Owned<Framework>(framework));

  if (frameworks.empty()) {
    // Terminate the slave if
    // 1) it's being shut down or
    // 2) it's started in cleanup mode and recovery finished.
    // TODO(vinod): Instead of doing it this way, shutdownFramework()
    // and shutdownExecutor() could return Futures and a slave could
    // shutdown when all the Futures are satisfied (e.g., collect()).
    if (state == TERMINATING ||
        (flags.recover == "cleanup" && !recovered.future().isPending())) {
      terminate(self());
    }
  }
}


void _unmonitor(
    const Future<Nothing>& unmonitor,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (!unmonitor.isReady()) {
    LOG(ERROR) << "Failed to unmonitor container for executor " << executorId
               << " of framework " << frameworkId << ": "
               << (unmonitor.isFailed() ? unmonitor.failure() : "discarded");
  }
}


void Slave::shutdownExecutor(Framework* framework, Executor* executor)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(executor);

  LOG(INFO) << "Shutting down executor '" << executor->id
            << "' of framework " << framework->id;

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;


  CHECK(executor->state == Executor::REGISTERING ||
        executor->state == Executor::RUNNING)
    << executor->state;

  executor->state = Executor::TERMINATING;

  // If the executor hasn't yet registered, this message
  // will be dropped to the floor!
  send(executor->pid, ShutdownExecutorMessage());

  // Prepare for sending a kill if the executor doesn't comply.
  delay(flags.executor_shutdown_grace_period,
        self(),
        &Slave::shutdownExecutorTimeout,
        framework->id,
        executor->id,
        executor->containerId);
}


void Slave::shutdownExecutorTimeout(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(INFO) << "Framework " << frameworkId
              << " seems to have exited. Ignoring shutdown timeout"
              << " for executor '" << executorId << "'";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    VLOG(1) << "Executor '" << executorId
            << "' of framework " << frameworkId
            << " seems to have exited. Ignoring its shutdown timeout";
    return;
  }

  if (executor->containerId != containerId) { // Make sure this timeout is valid.
    LOG(INFO) << "A new executor '" << executorId
              << "' of framework " << frameworkId
              << " with run " << executor->containerId
              << " seems to be active. Ignoring the shutdown timeout"
              << " for the old executor run " << containerId;
    return;
  }

  switch (executor->state) {
    case Executor::TERMINATED:
      LOG(INFO) << "Executor '" << executorId
                << "' of framework " << frameworkId
                << " has already terminated";
      break;
    case Executor::TERMINATING:
      LOG(INFO) << "Killing executor '" << executor->id
                << "' of framework " << framework->id;

      containerizer->destroy(executor->containerId);
      break;
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id
                 << " is in unexpected state " << executor->state;
      break;
  }
}


void Slave::registerExecutorTimeout(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(INFO) << "Framework " << frameworkId
              << " seems to have exited. Ignoring registration timeout"
              << " for executor '" << executorId << "'";
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(INFO) << "Ignoring registration timeout for executor '" << executorId
              << "' because the  framework " << frameworkId
              << " is terminating";
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    VLOG(1) << "Executor '" << executorId
            << "' of framework " << frameworkId
            << " seems to have exited. Ignoring its registration timeout";
    return;
  }

  if (executor->containerId != containerId ) {
    LOG(INFO) << "A new executor '" << executorId
              << "' of framework " << frameworkId
              << " with run " << executor->containerId
              << " seems to be active. Ignoring the registration timeout"
              << " for the old executor run " << containerId;
    return;
  }

  switch (executor->state) {
    case Executor::RUNNING:
    case Executor::TERMINATING:
    case Executor::TERMINATED:
      // Ignore the registration timeout.
      break;
    case Executor::REGISTERING:
      LOG(INFO) << "Terminating executor " << executor->id
                << " of framework " << framework->id
                << " because it did not register within "
                << flags.executor_registration_timeout;

      executor->state = Executor::TERMINATING;

      // Immediately kill the executor.
      containerizer->destroy(executor->containerId);
      break;
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id
                 << " is in unexpected state " << executor->state;
      break;
  }
}


// TODO(vinod): Figure out a way to express this function via cmd line.
Duration Slave::age(double usage)
{
  return flags.gc_delay * std::max(0.0, (1.0 - GC_DISK_HEADROOM - usage));
}


void Slave::checkDiskUsage()
{
  // TODO(vinod): We are making usage a Future, so that we can plug in
  // fs::usage() into async.
  // NOTE: We calculate disk usage of the file system on which the
  // slave work directory is mounted.
  Future<Try<double> >(fs::usage(flags.work_dir))
    .onAny(defer(self(), &Slave::_checkDiskUsage, lambda::_1));
}


void Slave::_checkDiskUsage(const Future<Try<double> >& usage)
{
  if (!usage.isReady()) {
    LOG(ERROR) << "Failed to get disk usage: "
               << (usage.isFailed() ? usage.failure() : "future discarded");
  } else {
    const Try<double>& result = usage.get();

    if (result.isSome()) {
      double use = result.get();

      LOG(INFO) << "Current usage " << std::setiosflags(std::ios::fixed)
                << std::setprecision(2) << 100 * use << "%."
                << " Max allowed age: " << age(use);

      // We prune all directories whose deletion time is within
      // the next 'gc_delay - age'. Since a directory is always
      // scheduled for deletion 'gc_delay' into the future, only directories
      // that are at least 'age' old are deleted.
      gc.prune(flags.gc_delay - age(use));
    } else {
      LOG(WARNING) << "Unable to get disk usage: " << result.error();
    }
  }
  delay(flags.disk_watch_interval, self(), &Slave::checkDiskUsage);
}



Future<Nothing> Slave::recover(const Result<SlaveState>& _state)
{
  if (_state.isError()) {
    return Failure(_state.error());
  }

  // Convert Result<SlaveState> to Option<SlaveState> for convenience.
  Option<SlaveState> state;
  if (_state.isSome()) {
    state = _state.get();
  }

  if (state.isSome() && state.get().info.isSome()) {
    // Check for SlaveInfo compatibility.
    // TODO(vinod): Also check for version compatibility.
    // NOTE: We set the 'id' field in 'info' from the recovered state,
    // as a hack to compare the info created from options/flags with
    // the recovered info.
    info.mutable_id()->CopyFrom(state.get().id);
    if (flags.recover == "reconnect" && !(info == state.get().info.get())) {
      return Failure(strings::join(
          "\n",
          "Incompatible slave info detected.",
          "------------------------------------------------------------",
          "Old slave info:\n" + stringify(state.get().info.get()),
          "------------------------------------------------------------",
          "New slave info:\n" + stringify(info),
          "------------------------------------------------------------"));
    }

    info = state.get().info.get(); // Recover the slave info.

    recoveryErrors = state.get().errors;
    if (recoveryErrors > 0) {
      LOG(WARNING) << "Errors encountered during recovery: " << recoveryErrors;
    }

    // Recover the frameworks.
    foreachvalue (const FrameworkState& frameworkState,
                  state.get().frameworks) {
      recoverFramework(frameworkState);
    }
  }

  return statusUpdateManager->recover(metaDir, state)
    .then(defer(self(), &Slave::_recoverContainerizer, state));
}


Future<Nothing> Slave::_recoverContainerizer(
    const Option<state::SlaveState>& state)
{
  return containerizer->recover(state);
}


Future<Nothing> Slave::_recover()
{
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      // Monitor the executor.
      monitor.start(
          executor->containerId,
          executor->info,
          flags.resource_monitoring_interval)
        .onAny(lambda::bind(_monitor,
                            lambda::_1,
                            framework->id,
                            executor->id,
                            executor->containerId));

      // Set up callback for executor termination.
      containerizer->wait(executor->containerId)
        .onAny(defer(self(),
                     &Self::executorTerminated,
                     framework->id,
                     executor->id,
                     lambda::_1));


      if (flags.recover == "reconnect") {
        if (executor->pid) {
          LOG(INFO) << "Sending reconnect request to executor " << executor->id
                    << " of framework " << framework->id
                    << " at " << executor->pid;

          ReconnectExecutorMessage message;
          message.mutable_slave_id()->MergeFrom(info.id());
          send(executor->pid, message);
        } else {
          LOG(INFO) << "Unable to reconnect to executor '" << executor->id
                    << "' of framework " << framework->id
                    << " because no libprocess PID was found";
        }
      } else {
        if (executor->pid) {
          // Cleanup executors.
          LOG(INFO) << "Sending shutdown to executor '" << executor->id
                    << "' of framework " << framework->id
                    << " to " << executor->pid;

          shutdownExecutor(framework, executor);
        } else {
          LOG(INFO) << "Killing executor '" << executor->id
                    << "' of framework " << framework->id
                    << " because no libprocess PID was found";

          containerizer->destroy(executor->containerId);
        }
      }
    }
  }

  if (!frameworks.empty() && flags.recover == "reconnect") {
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


void Slave::__recover(const Future<Nothing>& future)
{
  if (!future.isReady()) {
    EXIT(1)
      << "Failed to perform recovery: "
      << (future.isFailed() ? future.failure() : "future discarded") << "\n"
      << "To remedy this do as follows:\n"
      << "Step 1: rm -f " << paths::getLatestSlavePath(metaDir) << "\n"
      << "        This ensures slave doesn't recover old live executors.\n"
      << "Step 2: Restart the slave.";
  }

  LOG(INFO) << "Finished recovery";

  CHECK_EQ(RECOVERING, state);
  state = DISCONNECTED;

  // Checkpoint boot ID.
  Try<string> bootId = os::bootId();
  if (bootId.isError()) {
    LOG(ERROR) << "Could not retrieve boot id: " << bootId.error();
  } else {
    const string& path = paths::getBootIdPath(metaDir);
    CHECK_SOME(state::checkpoint(path, bootId.get()));
  }

  // Schedule all old slave directories for garbage collection.
  // TODO(vinod): Do this as part of recovery. This needs a fix
  // in the recovery code, to recover all slaves instead of only
  // the latest slave.
  const string& directory = path::join(flags.work_dir, "slaves");
  foreach (const string& entry, os::ls(directory)) {
    string path = path::join(directory, entry);
    // Ignore non-directory entries.
    if (!os::isdir(path)) {
      continue;
    }

    // We garbage collect a directory if either the slave has not
    // recovered its id (hence going to get a new id when it
    // registers with the master) or if it is an old work directory.
    SlaveID slaveId;
    slaveId.set_value(entry);
    if (!info.has_id() || !(slaveId == info.id())) {
      LOG(INFO) << "Garbage collecting old slave " << slaveId;

      // NOTE: We update the modification time of the slave work/meta
      // directories even though these are old because these
      // directories might not have been scheduled for gc before.

      // GC the slave work directory.
      os::utime(path); // Update the modification time.
      garbageCollect(path);

      // GC the slave meta directory.
      path = paths::getSlavePath(metaDir, slaveId);
      if (os::exists(path)) {
        os::utime(path); // Update the modification time.
        garbageCollect(path);
      }
    }
  }

  recovered.set(Nothing()); // Signal recovery.

  // Terminate slave, if it has no active frameworks and is started
  // in 'cleanup' mode.
  if (frameworks.empty() && flags.recover == "cleanup") {
    terminate(self());
    return;
  }

  // Start detecting masters.
  detector->detect()
    .onAny(defer(self(), &Slave::detected, lambda::_1));
}


void Slave::recoverFramework(const FrameworkState& state)
{
  LOG(INFO) << "Recovering framework " << state.id;

  if (state.executors.empty()) {
    // GC the framework work directory.
    garbageCollect(
        paths::getFrameworkPath(flags.work_dir, info.id(), state.id));

    // GC the framework meta directory.
    garbageCollect(
        paths::getFrameworkPath(metaDir, info.id(), state.id));

    return;
  }

  CHECK(!frameworks.contains(state.id));
  Framework* framework = new Framework(
      this, state.id, state.info.get(), state.pid.get());

  frameworks[framework->id] = framework;

  // Now recover the executors for this framework.
  foreachvalue (const ExecutorState& executorState, state.executors) {
    framework->recoverExecutor(executorState);
  }

  // Remove the framework in case we didn't recover any executors.
  if (framework->executors.empty()) {
    removeFramework(framework);
  }
}


Future<Nothing> Slave::garbageCollect(const string& path)
{
  Try<long> mtime = os::mtime(path);
  if (mtime.isError()) {
    LOG(ERROR) << "Failed to find the mtime of '" << path
               << "': " << mtime.error();
    return Failure(mtime.error());
  }

  // It is unsafe for testing to use unix time directly, we must use
  // Time::create to convert into a Time object that reflects the
  // possibly advanced state state of the libprocess Clock.
  Try<Time> time = Time::create(mtime.get());
  CHECK_SOME(time);

  // GC based on the modification time.
  Duration delay = flags.gc_delay - (Clock::now() - time.get());

  return gc.schedule(delay, path);
}


Framework::Framework(
    Slave* _slave,
    const FrameworkID& _id,
    const FrameworkInfo& _info,
    const UPID& _pid)
  : state(RUNNING),
    slave(_slave),
    id(_id),
    info(_info),
    pid(_pid),
    completedExecutors(MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK)
{
  if (info.checkpoint() && slave->state != slave->RECOVERING) {
    // Checkpoint the framework info.
    string path = paths::getFrameworkInfoPath(
        slave->metaDir, slave->info.id(), id);

    LOG(INFO) << "Checkpointing FrameworkInfo to '" << path << "'";
    CHECK_SOME(state::checkpoint(path, info));

    // Checkpoint the framework pid.
    path = paths::getFrameworkPidPath(
        slave->metaDir, slave->info.id(), id);

    LOG(INFO) << "Checkpointing framework pid '"
              << pid << "' to '" << path << "'";
    CHECK_SOME(state::checkpoint(path, pid));
  }
}


Framework::~Framework()
{
  // We own the non-completed executor pointers, so they need to be deleted.
  foreachvalue (Executor* executor, executors) {
    delete executor;
  }
}


// Create and launch an executor.
Executor* Framework::launchExecutor(
    const ExecutorInfo& executorInfo,
    const TaskInfo& taskInfo)
{
  // Generate an ID for the executor's container.
  // TODO(idownes) This should be done by the containerizer but we need the
  // ContainerID to create the executor's directory and to set up monitoring.
  // Fix this when 'launchExecutor()' is handled asynchronously.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Create a directory for the executor.
  const string& directory = paths::createExecutorDirectory(
      slave->flags.work_dir,
      slave->info.id(),
      id,
      executorInfo.executor_id(),
      containerId);

  Executor* executor = new Executor(
      slave, id, executorInfo, containerId, directory, info.checkpoint());

  CHECK(!executors.contains(executorInfo.executor_id()))
    << "Unknown executor " << executorInfo.executor_id();

  executors[executorInfo.executor_id()] = executor;

  slave->files->attach(executor->directory, executor->directory)
    .onAny(defer(slave,
                 &Slave::fileAttached,
                 lambda::_1,
                 executor->directory));

  // Tell the containerizer to launch the executor.
  // NOTE: We modify the ExecutorInfo to include the task's
  // resources when launching the executor so that the containerizer
  // has non-zero resources to work with when the executor has
  // no resources. This should be revisited after MESOS-600.
  ExecutorInfo executorInfo_ = executor->info;
  executorInfo_.mutable_resources()->MergeFrom(taskInfo.resources());

  // Launch the container.
  slave->containerizer->launch(
      containerId,
      executorInfo_, // modified to include the task's resources
      executor->directory,
      slave->flags.switch_user ? Option<string>(info.user()) : None(),
      slave->info.id(),
      slave->self(),
      info.checkpoint())
    .onAny(defer(slave,
                 &Slave::executorStarted,
                 id,
                 executor->id,
                 containerId,
                 lambda::_1));

  // Set up callback for executor termination.
  slave->containerizer->wait(containerId)
    .onAny(defer(slave,
                 &Slave::executorTerminated,
                 id,
                 executor->id,
                 lambda::_1));

  // Make sure the executor registers within the given timeout.
  delay(slave->flags.executor_registration_timeout,
        slave,
        &Slave::registerExecutorTimeout,
        id,
        executor->id,
        containerId);

  return executor;
}


void Framework::destroyExecutor(const ExecutorID& executorId)
{
  if (executors.contains(executorId)) {
    Executor* executor = executors[executorId];
    executors.erase(executorId);

    // Pass ownership of the executor pointer.
    completedExecutors.push_back(Owned<Executor>(executor));
  }
}


Executor* Framework::getExecutor(const ExecutorID& executorId)
{
  if (executors.contains(executorId)) {
    return executors[executorId];
  }

  return NULL;
}


Executor* Framework::getExecutor(const TaskID& taskId)
{
  foreachvalue (Executor* executor, executors) {
    if (executor->queuedTasks.contains(taskId) ||
        executor->launchedTasks.contains(taskId) ||
        executor->terminatedTasks.contains(taskId)) {
      return executor;
    }
  }
  return NULL;
}


void Framework::recoverExecutor(const ExecutorState& state)
{
  LOG(INFO) << "Recovering executor '" << state.id
            << "' of framework " << id;

  CHECK_NOTNULL(slave);

  if (state.runs.empty() || state.latest.isNone()) {
    LOG(WARNING) << "Skipping recovery of executor '" << state.id
                 << "' of framework " << id
                 << " because its latest run cannot be recovered";

    // GC the top level executor work directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->flags.work_dir, slave->info.id(), id, state.id));

    // GC the top level executor meta directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->metaDir, slave->info.id(), id, state.id));

    return;
  }

  // We are only interested in the latest run of the executor!
  // So, we GC all the old runs.
  // NOTE: We don't schedule the top level executor work and meta
  // directories for GC here, because they will be scheduled when
  // the latest executor run terminates.
  const ContainerID& latest = state.latest.get();
  foreachvalue (const RunState& run, state.runs) {
    CHECK_SOME(run.id);
    const ContainerID& runId = run.id.get();
    if (latest != runId) {
      // GC the executor run's work directory.
      // TODO(vinod): Expose this directory to webui by recovering the
      // tasks and doing a 'files->attach()'.
      slave->garbageCollect(paths::getExecutorRunPath(
          slave->flags.work_dir, slave->info.id(), id, state.id, runId));

      // GC the executor run's meta directory.
      slave->garbageCollect(paths::getExecutorRunPath(
          slave->metaDir, slave->info.id(), id, state.id, runId));
    }
  }

  CHECK(state.runs.contains(latest))
    << "Cannot find latest run " << latest << " for executor " << state.id
    << " of framework " << id;

  const RunState& run = state.runs.get(latest).get();

  // Create executor.
  const string& directory = paths::getExecutorRunPath(
      slave->flags.work_dir, slave->info.id(), id, state.id, latest);

  Executor* executor = new Executor(
      slave, id, state.info.get(), latest, directory, info.checkpoint());

  // Recover the libprocess PID if possible.
  if (run.libprocessPid.isSome()) {
    // When recovering in non-strict mode, the assumption is that the
    // slave can die after checkpointing the forked pid but before the
    // libprocess pid. So, it is not possible for the libprocess pid
    // to exist but not the forked pid. If so, it is a really bad
    // situation (e.g., disk corruption).
    CHECK_SOME(run.forkedPid)
      << "Failed to get forked pid for executor " << state.id
      << " of framework " << id;

    executor->pid = run.libprocessPid.get();
  }

  // And finally recover all the executor's tasks.
  foreachvalue (const TaskState& taskState, run.tasks) {
    executor->recoverTask(taskState);
  }

  // Expose the executor's files.
  slave->files->attach(executor->directory, executor->directory)
    .onAny(defer(slave,
                 &Slave::fileAttached,
                 lambda::_1,
                 executor->directory));

  // Add the executor to the framework.
  executors[executor->id] = executor;

  // If the latest run of the executor was completed (i.e., terminated
  // and all updates are acknowledged) in the previous run, we
  // transition its state to 'TERMINATED' and gc the directories.
  if (run.completed) {
    executor->state = Executor::TERMINATED;

    CHECK_SOME(run.id);
    const ContainerID& runId = run.id.get();

    // GC the executor run's work directory.
    const string& path = paths::getExecutorRunPath(
        slave->flags.work_dir, slave->info.id(), id, state.id, runId);

    slave->garbageCollect(path)
       .then(defer(slave, &Slave::detachFile, path));

    // GC the executor run's meta directory.
    slave->garbageCollect(paths::getExecutorRunPath(
        slave->metaDir, slave->info.id(), id, state.id, runId));

    // GC the top level executor work directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->flags.work_dir, slave->info.id(), id, state.id));

    // GC the top level executor meta directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->metaDir, slave->info.id(), id, state.id));

    // Move the executor to 'completedExecutors'.
    destroyExecutor(executor->id);
  }

  return;
}


Executor::Executor(
    Slave* _slave,
    const FrameworkID& _frameworkId,
    const ExecutorInfo& _info,
    const ContainerID& _containerId,
    const string& _directory,
    bool _checkpoint)
  : state(REGISTERING),
    slave(_slave),
    id(_info.executor_id()),
    info(_info),
    frameworkId(_frameworkId),
    containerId(_containerId),
    directory(_directory),
    checkpoint(_checkpoint),
    commandExecutor(strings::contains(
        info.command().value(),
        path::join(slave->flags.launcher_dir, "mesos-executor"))),
    pid(UPID()),
    resources(_info.resources()),
    completedTasks(MAX_COMPLETED_TASKS_PER_EXECUTOR)
{
  CHECK_NOTNULL(slave);

  if (checkpoint && slave->state != slave->RECOVERING) {
    // Checkpoint the executor info.
    const string& path = paths::getExecutorInfoPath(
        slave->metaDir, slave->info.id(), frameworkId, id);

    LOG(INFO) << "Checkpointing ExecutorInfo to '" << path << "'";
    CHECK_SOME(state::checkpoint(path, info));

    // Create the meta executor directory.
    // NOTE: This creates the 'latest' symlink in the meta directory.
    paths::createExecutorDirectory(
        slave->metaDir, slave->info.id(), frameworkId, id, containerId);
  }
}


Executor::~Executor()
{
  // Delete the tasks.
  // TODO(vinod): Use foreachvalue instead once LinkedHashmap
  // supports it.
  foreach (Task* task, launchedTasks.values()) {
    delete task;
  }
  foreach (Task* task, terminatedTasks.values()) {
    delete task;
  }
}


Task* Executor::addTask(const TaskInfo& task)
{
  // The master should enforce unique task IDs, but just in case
  // maybe we shouldn't make this a fatal error.
  CHECK(!launchedTasks.contains(task.task_id()))
    << "Duplicate task " << task.task_id();

  Task* t = new Task(
      protobuf::createTask(task, TASK_STAGING, id, frameworkId));

  launchedTasks[task.task_id()] = t;
  resources += task.resources();
  return t;
}


void Executor::terminateTask(
    const TaskID& taskId,
    const mesos::TaskState& state)
{
  VLOG(1) << "Terminating task " << taskId;

  Task* task = NULL;
  // Remove the task if it's queued.
  if (queuedTasks.contains(taskId)) {
    task = new Task(
        protobuf::createTask(queuedTasks[taskId], state, id, frameworkId));
    queuedTasks.erase(taskId);
  } else if (launchedTasks.contains(taskId)) {
    // Update the resources if it's been launched.
    task = launchedTasks[taskId];
    foreach (const Resource& resource, task->resources()) {
      resources -= resource;
    }
    launchedTasks.erase(taskId);
  }

  terminatedTasks[taskId] = CHECK_NOTNULL(task);
}


void Executor::completeTask(const TaskID& taskId)
{
  VLOG(1) << "Completing task " << taskId;

  CHECK(terminatedTasks.contains(taskId))
    << "Failed to find terminated task " << taskId;

  Task* task = terminatedTasks[taskId];
  completedTasks.push_back(memory::shared_ptr<Task>(task));
  terminatedTasks.erase(taskId);
}


void Executor::checkpointTask(const TaskInfo& task)
{
  if (checkpoint) {
    CHECK_NOTNULL(slave);

    const Task& t = protobuf::createTask(task, TASK_STAGING, id, frameworkId);
    const string& path = paths::getTaskInfoPath(
        slave->metaDir,
        slave->info.id(),
        frameworkId,
        id,
        containerId,
        t.task_id());

    LOG(INFO) << "Checkpointing TaskInfo to '" << path << "'";
    CHECK_SOME(state::checkpoint(path, t));
  }
}


void Executor::recoverTask(const TaskState& state)
{
  if (state.info.isNone()) {
    LOG(WARNING) << "Skipping recovery of task " << state.id
                 << " because its info cannot be recovered";
    return;
  }

  launchedTasks[state.id] = new Task(state.info.get());

  // NOTE: Since some tasks might have been terminated when the
  // slave was down, the executor resources we capture here is an
  // upper-bound. The actual resources needed (for live tasks) by
  // the isolator will be calculated when the executor re-registers.
  resources += state.info.get().resources();

  // Read updates to get the latest state of the task.
  foreach (const StatusUpdate& update, state.updates) {
    updateTaskState(update.status());

    // Terminate the task if it received a terminal update.
    // We ignore duplicate terminal updates by checking if
    // the task is present in launchedTasks.
    // TODO(vinod): Revisit these semantics when we disallow duplicate
    // terminal updates (e.g., when slave recovery is always enabled).
    if (protobuf::isTerminalState(update.status().state()) &&
        launchedTasks.contains(state.id)) {
      terminateTask(state.id, update.status().state());

      // If the terminal update has been acknowledged, remove it.
      if (state.acks.contains(UUID::fromBytes(update.uuid()))) {
        completeTask(state.id);
      }
      break;
    }
  }
}


void Executor::updateTaskState(const TaskStatus& status)
{
  if (launchedTasks.contains(status.task_id())) {
    Task* task = launchedTasks[status.task_id()];
    // TODO(brenden): Consider wiping the `data` and `message` fields?
    if (task->statuses_size() > 0 &&
        task->statuses(task->statuses_size() - 1).state() == status.state()) {
      task->mutable_statuses()->RemoveLast();
    }
    task->add_statuses()->CopyFrom(status);
    task->set_state(status.state());
  }
}


bool Executor::incompleteTasks()
{
  return !queuedTasks.empty() ||
         !launchedTasks.empty() ||
         !terminatedTasks.empty();
}


std::ostream& operator << (std::ostream& stream, Slave::State state)
{
  switch (state) {
    case Slave::RECOVERING:   return stream << "RECOVERING";
    case Slave::DISCONNECTED: return stream << "DISCONNECTED";
    case Slave::RUNNING:      return stream << "RUNNING";
    case Slave::TERMINATING:  return stream << "TERMINATING";
    default:                  return stream << "UNKNOWN";
  }
}


std::ostream& operator << (std::ostream& stream, Framework::State state)
{
  switch (state) {
    case Framework::RUNNING:     return stream << "RUNNING";
    case Framework::TERMINATING: return stream << "TERMINATING";
    default:                     return stream << "UNKNOWN";
  }
}


std::ostream& operator << (std::ostream& stream, Executor::State state)
{
  switch (state) {
    case Executor::REGISTERING: return stream << "REGISTERING";
    case Executor::RUNNING:     return stream << "RUNNING";
    case Executor::TERMINATING: return stream << "TERMINATING";
    case Executor::TERMINATED:  return stream << "TERMINATED";
    default:                    return stream << "UNKNOWN";
  }
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
