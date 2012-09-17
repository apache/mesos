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

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/numify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/utils.hpp>

#include "common/build.hpp"
#include "common/type_utils.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

namespace params = std::tr1::placeholders;

using std::string;

using process::wait; // Necessary on some OS's to disambiguate.

using std::tr1::cref;
using std::tr1::bind;


namespace mesos {
namespace internal {
namespace slave {

// // Represents a pending status update that has been sent and we are
// // waiting for an acknowledgement. In pa

// // stream of status updates for a framework/task. Note
// // that these are stored in the slave rather than per Framework
// // because a framework might go away before all of the status
// // updates have been sent and acknowledged.
// struct Slave::StatusUpdateStream
// {
//   StatusUpdateStreamID streamId;
//   string directory;
//   FILE* updates;
//   FILE* acknowledged;
//   queue<StatusUpdate> pending;
//   double timeout;
// };


//   StatusUpdateStreamID id;



//   queue<StatusUpdate> pending;
//   double timeout;
// };

// Helper function that returns true if the task state is terminal
bool isTerminalTaskState(TaskState state)
{
  return state == TASK_FINISHED ||
    state == TASK_FAILED ||
    state == TASK_KILLED ||
    state == TASK_LOST;
}


Slave::Slave(const Resources& _resources,
             bool _local,
             IsolationModule* _isolationModule,
             Files* _files)
  : ProcessBase(ID::generate("slave")),
    flags(),
    local(_local),
    resources(_resources),
    isolationModule(_isolationModule),
    files(_files) {}


Slave::Slave(const flags::Flags<logging::Flags, slave::Flags>& _flags,
             bool _local,
             IsolationModule* _isolationModule,
             Files* _files)
  : ProcessBase(ID::generate("slave")),
    flags(_flags),
    local(_local),
    isolationModule(_isolationModule),
    files(_files)
{
  if (flags.resources.isNone()) {
    // TODO(benh): Move this compuation into Flags as the "default".
    Try<long> cpus = os::cpus();
    Try<long> mem = os::memory();

    if (!cpus.isSome()) {
      LOG(WARNING) << "Failed to auto-detect the number of cpus to use,"
                   << " defaulting to 1";
      cpus = Try<long>::some(1);
    }

    if (!mem.isSome()) {
      LOG(WARNING) << "Failed to auto-detect the size of main memory,"
                   << " defaulting to 1024 MB";
      mem = Try<long>::some(1024);
    } else {
      // Convert to MB.
      mem = mem.get() / 1048576;

      // Leave 1 GB free if we have more than 1 GB, otherwise, use all!
      // TODO(benh): Have better default scheme (e.g., % of mem not
      // greater than 1 GB?)
      if (mem.get() > 1024) {
        mem = Try<long>::some(mem.get() - 1024);
      }
    }

    Try<string> defaults =
      strings::format("cpus:%d;mem:%d", cpus.get(), mem.get());

    CHECK(defaults.isSome());

    resources = Resources::parse(defaults.get());
  } else {
    resources = Resources::parse(flags.resources.get());
  }

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
    foreachvalue (Executor* executor, framework->executors) {
      delete executor;
    }
    delete framework;
  }
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
  info.set_webui_hostname(webui_hostname);
  info.set_webui_port(flags.webui_port);
  info.mutable_resources()->MergeFrom(resources);
  info.mutable_attributes()->MergeFrom(attributes);

  // Spawn and initialize the isolation module.
  // TODO(benh): Seems like the isolation module should really be
  // spawned before being passed to the slave.
  spawn(isolationModule);
  dispatch(isolationModule,
           &IsolationModule::initialize,
           flags, local, self());

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

  // TODO(benh): Ask glog for file name (i.e., mesos-slave.INFO).
  // Blocked on http://code.google.com/p/google-glog/issues/detail?id=116
  // Alternatively, initialize() could take the executable name.
  if (flags.log_dir.isSome()) {
    Future<Nothing> result = files->attach(
        path::join(flags.log_dir.get(), "mesos-slave.INFO"),
        "/log");
    result.onAny(defer(self(), &Self::fileAttached, result));
  }
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
    shutdownFramework(frameworkId);
  }

  // Stop the isolation module.
  terminate(isolationModule);
  wait(isolationModule);
}


void Slave::shutdown()
{
  LOG(INFO) << "Slave asked to shut down";
  terminate(self());
}


void Slave::fileAttached(const Future<Nothing>& result)
{
  CHECK(!result.isDiscarded());
  if (result.isReady()) {
    LOG(INFO) << "Master attached log file successfully";
  } else {
    LOG(ERROR) << "Failed to attach log file: " << result.failure();
  }
}


void Slave::newMasterDetected(const UPID& pid)
{
  LOG(INFO) << "New master detected at " << pid;

  master = pid;
  link(master);

  connected = false;
  doReliableRegistration();
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
  id = slaveId;

  connected = true;

  // Schedule all old slave directories to get garbage
  // collected. TODO(benh): It's unclear if we really need/want to
  // wait until the slave is registered to do this.
  const string& directory = path::join(flags.work_dir, "slaves");

  foreach (const string& file, os::ls(directory)) {
    const string& path = path::join(directory, file);

    // Check that this path is a directory but not our directory!
    if (os::isdir(path) && file != id.value()) {

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

  if (!(id == slaveId)) {
    LOG(FATAL) << "Slave re-registered but got wrong ID";
  }
  connected = true;
}


void Slave::doReliableRegistration()
{
  if (connected || !master) {
    return;
  }

  if (id == "") {
    // Slave started before master.
    // (Vinod): Is the above comment true?
    RegisterSlaveMessage message;
    message.mutable_slave()->MergeFrom(info);
    send(master, message);
  } else {
    // Re-registering, so send tasks running.
    ReregisterSlaveMessage message;
    message.mutable_slave_id()->MergeFrom(id);
    message.mutable_slave()->MergeFrom(info);

    foreachvalue (Framework* framework, frameworks) {
      foreachvalue (Executor* executor, framework->executors) {
        // TODO(benh): Kill this once framework_id is required on ExecutorInfo.
        ExecutorInfo* executorInfo = message.add_executor_infos();
        executorInfo->MergeFrom(executor->info);
        executorInfo->mutable_framework_id()->MergeFrom(framework->id);
        foreachvalue (Task* task, executor->launchedTasks) {
          // TODO(benh): Also need to send queued tasks here ...
          message.add_tasks()->MergeFrom(*task);
        }
      }
    }

    send(master, message);
  }

  // Re-try registration if necessary.
  delay(Seconds(1.0), self(), &Slave::doReliableRegistration);
}


void Slave::runTask(const FrameworkInfo& frameworkInfo,
                    const FrameworkID& frameworkId,
                    const string& pid,
                    const TaskInfo& task)
{
  LOG(INFO) << "Got assigned task " << task.task_id()
            << " for framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    framework = new Framework(frameworkId, frameworkInfo, pid, flags);
    frameworks[frameworkId] = framework;
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

      StatusUpdateMessage message;
      StatusUpdate* update = message.mutable_update();
      update->mutable_framework_id()->MergeFrom(frameworkId);
      update->mutable_slave_id()->MergeFrom(id);
      TaskStatus* status = update->mutable_status();
      status->mutable_task_id()->MergeFrom(task.task_id());
      status->set_state(TASK_LOST);
      update->set_timestamp(Clock::now());
      update->set_uuid(UUID::random().toBytes());
      send(master, message);
    } else if (!executor->pid) {
      // Queue task until the executor starts up.
      LOG(INFO) << "Queuing task '" << task.task_id()
                << "' for executor " << executorId
                << " of framework '" << frameworkId;
      executor->queuedTasks[task.task_id()] = task;
    } else {
      // Add the task and send it to the executor.
      executor->addTask(task);

      stats.tasks[TASK_STAGING]++;

      // Update the resources.
      // TODO(Charles Reiss): The isolation module is not guaranteed to update
      // the resources before the executor acts on its RunTaskMessage.
      dispatch(isolationModule,
               &IsolationModule::resourcesChanged,
               framework->id, executor->id, executor->resources);

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
    const string& directory =
      createUniqueWorkDirectory(framework->id, executorId);

    LOG(INFO) << "Using '" << directory
              << "' as work directory for executor '" << executorId
              << "' of framework " << framework->id;

    executor = framework->createExecutor(executorInfo, directory);

    // Queue task until the executor starts up.
    executor->queuedTasks[task.task_id()] = task;

    // Tell the isolation module to launch the executor. (TODO(benh):
    // Make the isolation module a process so that it can block while
    // trying to launch the executor.)
    dispatch(isolationModule,
             &IsolationModule::launchExecutor,
             framework->id, framework->info, executor->info,
             directory, executor->resources);
  }
}


void Slave::killTask(const FrameworkID& frameworkId,
                     const TaskID& taskId)
{
  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "WARNING! Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such framework is running";

    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(frameworkId);
    update->mutable_slave_id()->MergeFrom(id);
    TaskStatus* status = update->mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->set_state(TASK_LOST);
    update->set_timestamp(Clock::now());
    update->set_uuid(UUID::random().toBytes());
    send(master, message);

    return;
  }


  // Tell the executor to kill the task if it is up and
  // running, otherwise, consider the task lost.
  Executor* executor = framework->getExecutor(taskId);
  if (executor == NULL) {
    LOG(WARNING) << "WARNING! Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such task is running";

    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(framework->id);
    update->mutable_slave_id()->MergeFrom(id);
    TaskStatus* status = update->mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->set_state(TASK_LOST);
    update->set_timestamp(Clock::now());
    update->set_uuid(UUID::random().toBytes());
    send(master, message);
  } else if (!executor->pid) {
    // Remove the task.
    executor->removeTask(taskId);

    // Tell the isolation module to update the resources.
    dispatch(isolationModule,
             &IsolationModule::resourcesChanged,
             framework->id, executor->id, executor->resources);

    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(framework->id);
    update->mutable_executor_id()->MergeFrom(executor->id);
    update->mutable_slave_id()->MergeFrom(id);
    TaskStatus* status = update->mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->set_state(TASK_KILLED);
    update->set_timestamp(Clock::now());
    update->set_uuid(UUID::random().toBytes());
    send(master, message);
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
// couuld get into a state where a shut down was sent, dropped, and
// therefore never processed.
void Slave::shutdownFramework(const FrameworkID& frameworkId)
{
  LOG(INFO) << "Asked to shut down framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Shutting down framework " << framework->id;

    // Shut down all executors of this framework.
    foreachvalue (Executor* executor, framework->executors) {
      shutdownExecutor(framework, executor);
    }
  }
}


void Slave::schedulerMessage(const SlaveID& slaveId,
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


void Slave::updateFramework(const FrameworkID& frameworkId,
                            const string& pid)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Updating framework " << frameworkId
              << " pid to " <<pid;
    framework->pid = pid;
  }
}


void Slave::statusUpdateAcknowledgement(const SlaveID& slaveId,
                                        const FrameworkID& frameworkId,
                                        const TaskID& taskId,
                                        const string& uuid)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    if (framework->updates.contains(UUID::fromBytes(uuid))) {
      LOG(INFO) << "Got acknowledgement of status update"
                << " for task " << taskId
                << " of framework " << frameworkId;

      framework->updates.erase(UUID::fromBytes(uuid));

      // Cleanup if this framework has no executors running and no pending updates.
      if (framework->executors.size() == 0 && framework->updates.empty()) {
        frameworks.erase(framework->id);
        delete framework;
      }
    }
  }
}


// void Slave::statusUpdateAcknowledged(const SlaveID& slaveId,
//                                      const FrameworkID& frameworkId,
//                                      const TaskID& taskId,
//                                      uint32_t sequence)
// {
//   StatusUpdateStreamID id(frameworkId, taskId);
//   StatusUpdateStream* stream = getStatusUpdateStream(id);

//   if (stream == NULL) {
//     LOG(WARNING) << "WARNING! Received unexpected status update"
//                  << " acknowledgement for task " << taskId
//                  << " of framework " << frameworkId;
//     return;
//   }

//   CHECK(!stream->pending.empty());

//   const StatusUpdate& update = stream->pending.front();

//   if (update->sequence() != sequence) {
//     LOG(WARNING) << "WARNING! Received status update acknowledgement"
//                  << " with bad sequence number (received " << sequence
//                  << ", expecting " << update->sequence()
//                  << ") for task " << taskId
//                  << " of framework " << frameworkId;
//   } else {
//     LOG(INFO) << "Received status update acknowledgement for task "
//               << taskId << " of framework " << frameworkId;

//     // Write the update out to disk.
//     CHECK(stream->acknowledged != NULL);

//     Try<bool> result =
//       utils::protobuf::write(stream->acknowledged, update);

//     if (result.isError()) {
//       // Failing here is rather dramatic, but so is not being able to
//       // write to disk ... seems like failing early and often might do
//       // more benefit than harm.
//       LOG(FATAL) << "Failed to write status update to "
//                  << stream->directory << "/acknowledged: "
//                  << result.message();
//     }

//     stream->pending.pop();

//     bool empty = stream->pending.empty();

//     bool terminal =
//       update.status().state() == TASK_FINISHED &&
//       update.status().state() == TASK_FAILED &&
//       update.status().state() == TASK_KILLED &&
//       update.status().state() == TASK_LOST;

//     if (empty && terminal) {
//       cleanupStatusUpdateStream(stream);
//     } else if (!empty && terminal) {
//       LOG(WARNING) << "WARNING! Acknowledged a \"terminal\""
//                    << " task status but updates are still pending";
//     } else if (!empty) {
//       StatusUpdateMessage message;
//       message.mutable_update()->MergeFrom(stream->pending.front());
//       message.set_reliable(true);
//       send(master, message);

//       stream->timeout = Clock::now() + STATUS_UPDATE_RETRY_INTERVAL;
//     }
//   }
// }


void Slave::registerExecutor(const FrameworkID& frameworkId,
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
             framework->id, executor->id, executor->resources);

    // Tell executor it's registered and give it any queued tasks.
    ExecutorRegisteredMessage message;
    message.mutable_executor_info()->MergeFrom(executor->info);
    message.mutable_framework_id()->MergeFrom(framework->id);
    message.mutable_framework_info()->MergeFrom(framework->info);
    message.mutable_slave_id()->MergeFrom(id);
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


// void Slave::statusUpdate(const StatusUpdate& update)
// {
//   LOG(INFO) << "Received update that task " << update.status().task_id()
//             << " of framework " << update.framework_id()
//             << " is now in state " << update.status().state();

//   Framework* framework = getFramework(update.framework_id());
//   if (framework == NULL) {
//     LOG(WARNING) << "WARNING! Failed to lookup"
//                  << " framework " << update.framework_id()
//                  << " of received status update";
//     stats.invalidStatusUpdates++;
//     return;
//   }

//   Executor* executor = framework->getExecutor(update.status().task_id());
//   if (executor == NULL) {
//     LOG(WARNING) << "WARNING! Failed to lookup executor"
//                  << " for framework " << update.framework_id()
//                  << " of received status update";
//     stats.invalidStatusUpdates++;
//     return;
//   }

//   // Create/Get the status update stream for this framework/task.
//   StatusUpdateStreamID id(update.framework_id(), update.status().task_id());

//   if (!statusUpdateStreams.contains(id)) {
//     StatusUpdateStream* stream =
//       createStatusUpdateStream(id, executor->directory);

//     if (stream == NULL) {
//       LOG(WARNING) << "WARNING! Failed to create status update"
//                    << " stream for task " << update.status().task_id()
//                    << " of framework " << update.framework_id()
//                    << " ... removing executor!";
//       removeExecutor(framework, executor);
//       return;
//     }
//   }

//   StatusUpdateStream* stream = getStatusUpdateStream(id);

//   CHECK(stream != NULL);

//   // If we are already waiting on an acknowledgement, check that this
//   // update (coming from the executor), is the same one that we are
//   // waiting on being acknowledged.

//   // Check that this is status update has not already been
//   // acknowledged. this could happen because a slave writes the
//   // acknowledged message but then fails before it can pass the
//   // message on to the executor, so the executor tries again.

//   returnhere;

//   // TODO(benh): Check that this update hasn't already been received
//   // or acknowledged! This could happen if a slave receives a status
//   // update from an executor, then crashes after it writes it to disk
//   // but before it sends an ack back to

//   // Okay, record this update as received.
//   CHECK(stream->received != NULL);

//   Result<bool> result =
//     utils::protobuf::write(stream->received, &update);

//   if (result.isError()) {
//     // Failing here is rather dramatic, but so is not being able to
//     // write to disk ... seems like failing early and often might do
//     // more benefit than harm.
//     LOG(FATAL) << "Failed to write status update to "
//                << stream->directory << "/received: "
//                << result.message();
//   }

//   // Now acknowledge the executor.
//   StatusUpdateAcknowledgementMessage message;
//   message.mutable_framework_id()->MergeFrom(update.framework_id());
//   message.mutable_slave_id()->MergeFrom(update.slave_id());
//   message.mutable_task_id()->MergeFrom(update.status().task_id());
//   send(executor->pid, message);

//   executor->updateTaskState(
//       update.status().task_id(),
//       update.status().state());

//   // Remove the task if it's reached a terminal state.
//   bool terminal =
//     update.status().state() == TASK_FINISHED &&
//     update.status().state() == TASK_FAILED &&
//     update.status().state() == TASK_KILLED &&
//     update.status().state() == TASK_LOST;

//   if (terminal) {
//     executor->removeTask(update.status().task_id());
//     isolationModule->resourcesChanged(
//         framework->id, framework->info,
//         executor->info, executor->resources);
//   }

//   stream->pending.push(update);

//   // Send the status update if this is the first in the
//   // stream. Subsequent status updates will get sent in
//   // Slave::statusUpdateAcknowledged.
//   if (stream->pending.size() == 1) {
//     CHECK(stream->timeout == -1);
//     StatusUpdateMessage message;
//     message.mutable_update()->MergeFrom(update);
//     message.set_reliable(true);
//     send(master, message);

//     stream->timeout = Clock::now() + STATUS_UPDATE_RETRY_INTERVAL;
//   }

//   stats.tasks[status.state()]++;
//   stats.validStatusUpdates++;
// }

void Slave::statusUpdate(const StatusUpdate& update)
{
  const TaskStatus& status = update.status();

  LOG(INFO) << "Status update: task " << status.task_id()
            << " of framework " << update.framework_id()
            << " is now in state " << status.state();

  Framework* framework = getFramework(update.framework_id());
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(status.task_id());
    if (executor != NULL) {
      executor->updateTaskState(status.task_id(), status.state());

      // Handle the task appropriately if it's terminated.
      if (isTerminalTaskState(status.state())) {
        executor->removeTask(status.task_id());

        dispatch(isolationModule,
                 &IsolationModule::resourcesChanged,
                 framework->id, executor->id, executor->resources);
      }

      // Send message and record the status for possible resending.
      StatusUpdateMessage message;
      message.mutable_update()->MergeFrom(update);
      message.set_pid(self());
      send(master, message);

      UUID uuid = UUID::fromBytes(update.uuid());

      // Send us a message to try and resend after some delay.
      delay(STATUS_UPDATE_RETRY_INTERVAL,
            self(), &Slave::statusUpdateTimeout,
            framework->id, uuid);

      framework->updates[uuid] = update;

      stats.tasks[status.state()]++;

      stats.validStatusUpdates++;
    } else {
      LOG(WARNING) << "Status update error: couldn't lookup "
                   << "executor for framework " << update.framework_id();
      stats.invalidStatusUpdates++;
    }
  } else {
    LOG(WARNING) << "Status update error: couldn't lookup "
                 << "framework " << update.framework_id();
    stats.invalidStatusUpdates++;
  }
}


void Slave::executorMessage(const SlaveID& slaveId,
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


void Slave::statusUpdateTimeout(
    const FrameworkID& frameworkId,
    const UUID& uuid)
{
  // Check and see if we still need to send this update.
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    if (framework->updates.contains(uuid)) {
      const StatusUpdate& update = framework->updates[uuid];

      LOG(INFO) << "Resending status update"
                << " for task " << update.status().task_id()
                << " of framework " << update.framework_id();

      StatusUpdateMessage message;
      message.mutable_update()->MergeFrom(update);
      message.set_pid(self());
      send(master, message);

      // Send us a message to try and resend after some delay.
      delay(STATUS_UPDATE_RETRY_INTERVAL,
            self(), &Slave::statusUpdateTimeout,
            framework->id, uuid);
    }
  }
}


// void Slave::timeout()
// {
//   // Check and see if we should re-send any status updates.
//   double now = Clock::now();

//   foreachvalue (StatusUpdateStream* stream, statusUpdateStreams) {
//     CHECK(stream->timeout > 0);
//     if (stream->timeout < now) {
//       CHECK(!stream->pending.empty());
//       const StatusUpdate& update = stream->pending.front();

//       LOG(WARNING) << "WARNING! Resending status update"
//                 << " for task " << update.status().task_id()
//                 << " of framework " << update.framework_id();

//       StatusUpdateMessage message;
//       message.mutable_update()->MergeFrom(update);
//       message.set_reliable(true);
//       send(master, message);

//       stream->timeout = now + STATUS_UPDATE_RETRY_INTERVAL;
//     }
//   }
// }


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


// StatusUpdates* Slave::getStatusUpdateStream(const StatusUpdateStreamID& id)
// {
//   if (statusUpdateStreams.contains(id)) {
//     return statusUpdateStreams[id];
//   }

//   return NULL;
// }


// StatusUpdateStream* Slave::createStatusUpdateStream(
//     const FrameworkID& frameworkId,
//     const TaskID& taskId,
//     const string& directory)
// {
//   StatusUpdateStream* stream = new StatusUpdates();
//   stream->id = id;
//   stream->directory = directory;
//   stream->received = NULL;
//   stream->acknowledged = NULL;
//   stream->timeout = -1;

//   streams[id] = stream;

//   // Open file descriptors for "updates" and "acknowledged".
//   string path;
//   Result<int> result;

//   path = stream->directory + "/received";
//   result = utils::os::open(path, O_CREAT | O_RDWR | O_SYNC);
//   if (result.isError() || result.isNone()) {
//     LOG(WARNING) << "Failed to open " << path
//                  << " for storing received status updates";
//     cleanupStatusUpdateStream(stream);
//     return NULL;
//   }

//   stream->received = result.get();

//   path = updates->directory + "/acknowledged";
//   result = utils::os::open(path, O_CREAT | O_RDWR | O_SYNC);
//   if (result.isError() || result.isNone()) {
//     LOG(WARNING) << "Failed to open " << path <<
//                  << " for storing acknowledged status updates";
//     cleanupStatusUpdateStream(stream);
//     return NULL;
//   }

//   stream->acknowledged = result.get();

//   // Replay the status updates. This is necessary because the slave
//   // might have crashed but was restarted before the executors
//   // died. Or another task with the same id as before got run again on
//   // the same executor.
//   bool replayed = replayStatusUpdateStream(stream);

//   if (!replayed) {
//     LOG(WARNING) << "Failed to correctly replay status updates"
//                  << " for task " << taskId
//                  << " of framework " << frameworkId
//                  << " found at " << path;
//     cleanupStatusUpdateStream(stream);
//     return NULL;
//   }

//   // Start sending any pending status updates. In this case, the slave
//   // probably died after it sent the status update and never received
//   // the acknowledgement.
//   if (!stream->pending.empty()) {
//     StatusUpdate* update = stream->pending.front();
//     StatusUpdateMessage message;
//     message.mutable_update()->MergeFrom(*update);
//     message.set_reliable(true);
//     send(master, message);

//     stream->timeout = Clock::now() + STATUS_UPDATE_RETRY_INTERVAL;
//   }

//   return stream;
// }


// bool Slave::replayStatusUpdateStream(StatusUpdateStream* stream)
// {
//   CHECK(stream->received != NULL);
//   CHECK(stream->acknowledged != NULL);

//   Result<StatusUpdate*> result;

//   // Okay, now read all the recevied status updates.
//   hashmap<uint32_t, StatusUpdate> pending;

//   result = utils::protobuf::read(stream->received);
//   while (result.isSome()) {
//     StatusUpdate* update = result.get();
//     CHECK(!pending.contains(update->sequence()));
//     pending[update->sequence()] = *update;
//     delete update;
//     result = utils::protobuf::read(stream->received);
//   }

//   if (result.isError()) {
//     return false;
//   }

//   CHECK(result.isNone());

//   LOG(INFO) << "Recovered " << pending.size()
//             << " TOTAL status updates for task "
//             << stream->id.second << " of framework "
//             << stream->id.first;

//   // Okay, now get all the acknowledged status updates.
//   result = utils::protobuf::read(stream->acknowledged);
//   while (result.isSome()) {
//     StatusUpdate* update = result.get();
//     stream->sequence = std::max(stream->sequence, update->sequence());
//     CHECK(pending.contains(update->sequence()));
//     pending.erase(update->sequence());
//     delete update;
//     result = utils::protobuf::read(stream->acknowledged);
//   }

//   if (result.isError()) {
//     return false;
//   }

//   CHECK(result.isNone());

//   LOG(INFO) << "Recovered " << pending.size()
//             << " PENDING status updates for task "
//             << stream->id.second << " of framework "
//             << stream->id.first;

//   // Add the pending status updates in sorted order.
//   uint32_t sequence = 0;

//   while (!pending.empty()) {
//     // Find the smallest sequence number.
//     foreachvalue (const StatusUpdate& update, pending) {
//       sequence = std::min(sequence, update.sequence());
//     }

//     // Push that update and remove it from pending.
//     stream->pending.push(pending[sequence]);
//     pending.erase(sequence);
//   }

//   return true;
// }


// void Slave::cleanupStatusUpdateStream(StatusUpdateStream* stream)
// {
//   if (stream->received != NULL) {
//     fclose(stream->received);
//   }

//   if (stream->acknowledged != NULL) {
//     fclose(stream->acknowledged);
//   }

//   streams.erase(stream->id);

//   delete stream;
// }


// N.B. When the slave is running in "local" mode then the pid is
// uninteresting (and possibly could cause bugs).
void Slave::executorStarted(const FrameworkID& frameworkId,
                            const ExecutorID& executorId,
                            pid_t pid)
{

}


StatusUpdate Slave::createStatusUpdate(const TaskID& taskId,
                                       const ExecutorID& executorId,
                                       const FrameworkID& frameworkId,
                                       TaskState taskState,
                                       const string& reason)
{
  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskId);
  status.set_state(taskState);
  status.set_message(reason);

  StatusUpdate update;
  update.mutable_framework_id()->MergeFrom(frameworkId);
  update.mutable_slave_id()->MergeFrom(id);
  update.mutable_executor_id()->MergeFrom(executorId);
  update.mutable_status()->MergeFrom(status);
  update.set_timestamp(Clock::now());
  update.set_uuid(UUID::random().toBytes());

  return update;
}


// Called when an executor is exited.
// Transitions a live task to TASK_LOST/TASK_FAILED and sends status update.
void Slave::transitionLiveTask(const TaskID& taskId,
                               const ExecutorID& executorId,
                               const FrameworkID& frameworkId,
                               bool isCommandExecutor,
                               int status)
{
  StatusUpdate update;

  if (isCommandExecutor) {
    update = createStatusUpdate(taskId,
                                executorId,
                                frameworkId,
                                TASK_FAILED,
                                "Executor running the task's command failed");
  } else {
    update = createStatusUpdate(taskId,
                                executorId,
                                frameworkId,
                                TASK_LOST,
                                "Executor exited");
  }

  statusUpdate(update);
}


// Called by the isolation module when an executor process exits.
void Slave::executorExited(const FrameworkID& frameworkId,
                           const ExecutorID& executorId,
                           int status)
{
  LOG(INFO) << "Executor '" << executorId
            << "' of framework " << frameworkId
            << (WIFEXITED(status)
                ? " has exited with status "
                : " has terminated with signal ")
            << (WIFEXITED(status)
                ? stringify(WEXITSTATUS(status))
                : strsignal(WTERMSIG(status)));

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
  foreachvalue (Task* task, utils::copy(executor->launchedTasks)) {
    if (!isTerminalTaskState(task->state())) {
      isCommandExecutor = !task->has_executor_id();

      transitionLiveTask(task->task_id(),
                         executor->id,
                         framework->id,
                         isCommandExecutor,
                         status);
    }
  }

  // Transition all queued tasks to TASK_LOST/TASK_FAILED.
  foreachvalue (const TaskInfo& task, utils::copy(executor->queuedTasks)) {
    isCommandExecutor = task.has_command();

    transitionLiveTask(task.task_id(),
                       executor->id,
                       framework->id,
                       isCommandExecutor,
                       status);
  }


  if (!isCommandExecutor) {
    ExitedExecutorMessage message;
    message.mutable_slave_id()->MergeFrom(id);
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    message.set_status(status);

    send(master, message);
  }

  // Schedule the executor directory to get garbage collected.
  gc.schedule(flags.gc_delay, executor->directory);

  framework->destroyExecutor(executor->id);
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
        framework->id, executor->id, executor->uuid);
}


void Slave::shutdownExecutorTimeout(const FrameworkID& frameworkId,
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
             framework->id, executor->id);

    // Schedule the executor directory to get garbage collected.
    gc.schedule(flags.gc_delay, executor->directory);

    framework->destroyExecutor(executor->id);
  }

  // Cleanup if this framework has no executors running.
  if (framework->executors.size() == 0) {
    frameworks.erase(framework->id);
    delete framework;
  }
}


// void Slave::recover()
// {
//   // if we find an executor that is no longer running and it's last
//   // acknowledged task statuses are not terminal, create a
//   // statusupdatestream for each task and try and reliably send
//   // TASK_LOST updates.

//   // otherwise once we reconnect the executor will just start sending
//   // us status updates that we need to send, wait for ack, write to
//   // disk, and then respond.
// }


string Slave::createUniqueWorkDirectory(const FrameworkID& frameworkId,
                                        const ExecutorID& executorId)
{
  LOG(INFO) << "Generating a unique work directory for executor '"
            << executorId << "' of framework " << frameworkId;

  std::ostringstream out(std::ios_base::app | std::ios_base::out);
  out << flags.work_dir
      << "/slaves/" << id
      << "/frameworks/" << frameworkId
      << "/executors/" << executorId;

  // Find a unique directory based on the path given by the slave
  // (this is because we might launch multiple executors from the same
  // framework on this slave).
  // NOTE: The run number of the new directory will be the highest of
  // all the existing run directories for this executor.
  out << "/runs/";

  int maxrun = 0;
  foreach (const string& runStr, os::ls(out.str())) {
    Try<int> run = numify<int>(runStr);
    if (run.isError()) {
      LOG(ERROR) << "Ignoring invalid run directory " << runStr;
      continue;
    }

    maxrun = std::max(maxrun, run.get());
  }
  out << maxrun;

  bool created = os::mkdir(out.str());
  CHECK(created) << "Error creating work directory " << out.str();

  return out.str();
}


// TODO(vinod): Figure out a way to express this function via cmd line.
Duration Slave::age(double usage)
{
 return Weeks(flags.gc_delay.weeks() * (1.0 - usage));
}


void Slave::checkDiskUsage()
{
  VLOG(1) << "Checking disk usage";

  // TODO(vinod): We are making usage a Future, so that we can plug-in
  // os::usage() into async.
  Future<Try<double> > usage = os::usage();

  usage.onAny(
      defer(self(),
            &Slave::_checkDiskUsage,
            usage));
}


void Slave::_checkDiskUsage(const Future<Try<double> >& usage)
{
  if (!usage.isReady()) {
    LOG(WARNING) << "Error getting disk usage";
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

} // namespace slave {
} // namespace internal {
} // namespace mesos {
