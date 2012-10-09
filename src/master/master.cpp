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

#include <fstream>
#include <iomanip>
#include <list>
#include <sstream>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/run.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "common/build.hpp"
#include "common/date_utils.hpp"

#include "flags/flags.hpp"

#include "logging/flags.hpp"

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/slaves_manager.hpp"

namespace params = std::tr1::placeholders;

using std::list;
using std::string;
using std::vector;

using process::wait; // Necessary on some OS's to disambiguate.

using std::tr1::cref;
using std::tr1::bind;


namespace mesos {
namespace internal {
namespace master {

class WhitelistWatcher : public Process<WhitelistWatcher> {
public:
  WhitelistWatcher(const string& _path, AllocatorProcess* _allocator)
  : path(_path), allocator(_allocator) {}

protected:
  virtual void initialize()
  {
    watch();
  }

  void watch()
  {
    // Get the list of white listed slaves.
    Option<hashset<string> > whitelist;
    if (path == "*") { // Accept all slaves.
      LOG(WARNING) << "No whitelist given. Advertising offers for all slaves";
    } else {
      // Read from local file.
      // TODO(vinod): Add support for reading from ZooKeeper.
      CHECK(path.find("file://") == 0)
          << "File path " << path << " should start with file://";

      // TODO(vinod): Ensure this read is atomic w.r.t external
      // writes/updates to this file.
      Result<string> result = os::read(path.substr(strlen("file://")));
      if (result.isError()) {
        LOG(ERROR) << "Error reading whitelist file "
                   << result.error() << ". Retrying";
        whitelist = lastWhitelist;
      } else if (result.isNone()) {
        LOG(WARNING) << "Empty whitelist file "
                     << path << ". No offers will be made!";
        whitelist = Option<hashset<string> >::some(hashset<string>());
      } else {
        hashset<string> hostnames;
        vector<string> lines = strings::tokenize(result.get(), "\n");
        foreach (const string& hostname, lines) {
          hostnames.insert(hostname);
        }
        whitelist = Option<hashset<string> >::some(hostnames);
      }
    }

    // Send the whitelist to allocator, if necessary.
    if (whitelist != lastWhitelist) {
      dispatch(allocator, &AllocatorProcess::updateWhitelist, whitelist);
    }

    // Check again.
    lastWhitelist = whitelist;
    delay(WHITELIST_WATCH_INTERVAL, self(), &WhitelistWatcher::watch);
  }

private:
  const string path;
  AllocatorProcess* allocator;
  Option<hashset<string> > lastWhitelist;
};


class SlaveObserver : public Process<SlaveObserver>
{
public:
  SlaveObserver(const UPID& _slave,
                const SlaveInfo& _slaveInfo,
                const SlaveID& _slaveId,
                const PID<Master>& _master)
    : ProcessBase(ID::generate("slave-observer")),
      slave(_slave),
      slaveInfo(_slaveInfo),
      slaveId(_slaveId),
      master(_master),
      timeouts(0),
      pinged(false)
  {
    install("PONG", &SlaveObserver::pong);
  }

protected:
  virtual void initialize()
  {
    send(slave, "PING");
    pinged = true;
    delay(SLAVE_PING_TIMEOUT, self(), &SlaveObserver::timeout);
  }

  void pong(const UPID& from, const string& body)
  {
    timeouts = 0;
    pinged = false;
  }

  void timeout()
  {
    if (pinged) { // So we haven't got back a pong yet ...
      if (++timeouts >= MAX_SLAVE_PING_TIMEOUTS) {
        deactivate();
        return;
      }
    }

    send(slave, "PING");
    pinged = true;
    delay(SLAVE_PING_TIMEOUT, self(), &SlaveObserver::timeout);
  }

  void deactivate()
  {
    dispatch(master, &Master::deactivatedSlaveHostnamePort,
             slaveInfo.hostname(), slave.port);
  }

private:
  const UPID slave;
  const SlaveInfo slaveInfo;
  const SlaveID slaveId;
  const PID<Master> master;
  uint32_t timeouts;
  bool pinged;
};


// Performs slave registration asynchronously. There are two means of
// doing this, one first tries to add this slave to the slaves
// manager, while the other one simply tells the master to add the
// slave.
struct SlaveRegistrar
{
  static bool run(Slave* slave, const PID<Master>& master)
  {
    // TODO(benh): Do a reverse lookup to ensure IP maps to
    // hostname, or check credentials of this slave.
    dispatch(master, &Master::addSlave, slave, false);
    return true;
  }

  static bool run(Slave* slave,
                  const PID<Master>& master,
                  const PID<SlavesManager>& slavesManager)
  {
    Future<bool> added = dispatch(slavesManager, &SlavesManager::add,
                                  slave->info.hostname(), slave->pid.port);
    added.await();
    if (!added.isReady() || !added.get()) {
      LOG(WARNING) << "Could not register slave on "<< slave->info.hostname()
                   << " because failed to add it to the slaves maanger";
      // TODO(benh): This could be because our acknowledgement to the
      // slave was dropped, so they retried, and now we should
      // probably send another acknowledgement.
      delete slave;
      return false;
    }

    return run(slave, master);
  }
};


// Performs slave re-registration asynchronously as above.
struct SlaveReregistrar
{
  static bool run(Slave* slave,
                  const vector<ExecutorInfo>& executorInfos,
                  const vector<Task>& tasks,
                  const PID<Master>& master)
  {
    // TODO(benh): Do a reverse lookup to ensure IP maps to
    // hostname, or check credentials of this slave.
    dispatch(master, &Master::readdSlave, slave, executorInfos, tasks);
    return true;
  }

  static bool run(Slave* slave,
                  const vector<ExecutorInfo>& executorInfos,
                  const vector<Task>& tasks,
                  const PID<Master>& master,
                  const PID<SlavesManager>& slavesManager)
  {
    Future<bool> added = dispatch(slavesManager, &SlavesManager::add,
                                  slave->info.hostname(), slave->pid.port);
    added.await();
    if (!added.isReady() || !added.get()) {
      LOG(WARNING) << "Could not register slave on " << slave->info.hostname()
                   << " because failed to add it to the slaves manager";
      // TODO(benh): This could be because our acknowledgement to the
      // slave was dropped, so they retried, and now we should
      // probably send another acknowledgement.
      delete slave;
      return false;
    }

    return run(slave, executorInfos, tasks, master);
  }
};


Master::Master(AllocatorProcess* _allocator, Files* _files)
  : ProcessBase("master"),
    flags(),
    allocator(_allocator),
    files(_files) {}


Master::Master(AllocatorProcess* _allocator,
               Files* _files,
               const flags::Flags<logging::Flags, master::Flags>& _flags)
  : ProcessBase("master"),
    flags(_flags),
    allocator(_allocator),
    files(_files) {}


Master::~Master()
{
  LOG(INFO) << "Shutting down master";

  foreachvalue (Framework* framework, utils::copy(frameworks)) {
    removeFramework(framework);
  }

  foreachvalue (Slave* slave, utils::copy(slaves)) {
    removeSlave(slave);
  }

  CHECK(offers.size() == 0);

  terminate(slavesManager);
  wait(slavesManager);

  delete slavesManager;

  terminate(whitelistWatcher);
  wait(whitelistWatcher);

  delete whitelistWatcher;
}


void Master::initialize()
{
  LOG(INFO) << "Master started on " << string(self()).substr(7);

  // The master ID is currently comprised of the current date, the IP
  // address and port from self() and the OS PID.

  Try<string> id =
    strings::format("%s-%u-%u-%d", DateUtils::currentDate(),
                    self().ip, self().port, getpid());

  CHECK(!id.isError()) << id.error();

  info.set_id(id.get());
  info.set_ip(self().ip);
  info.set_port(self().port);

  LOG(INFO) << "Master ID: " << info.id();

  // Setup slave manager.
  slavesManager = new SlavesManager(flags, self());
  spawn(slavesManager);

  // Spawn the allocator.
  spawn(allocator);
  dispatch(allocator, &AllocatorProcess::initialize, flags, self());

  // Parse the white list
  whitelistWatcher = new WhitelistWatcher(flags.whitelist, allocator);
  spawn(whitelistWatcher);

  elected = false;

  nextFrameworkId = 0;
  nextSlaveId = 0;
  nextOfferId = 0;

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

  // Install handler functions for certain messages.
  install<SubmitSchedulerRequest>(
      &Master::submitScheduler,
      &SubmitSchedulerRequest::name);

  install<NewMasterDetectedMessage>(
      &Master::newMasterDetected,
      &NewMasterDetectedMessage::pid);

  install<NoMasterDetectedMessage>(
      &Master::noMasterDetected);

  install<RegisterFrameworkMessage>(
      &Master::registerFramework,
      &RegisterFrameworkMessage::framework);

  install<ReregisterFrameworkMessage>(
      &Master::reregisterFramework,
      &ReregisterFrameworkMessage::framework,
      &ReregisterFrameworkMessage::failover);

  install<UnregisterFrameworkMessage>(
      &Master::unregisterFramework,
      &UnregisterFrameworkMessage::framework_id);

  install<DeactivateFrameworkMessage>(
        &Master::deactivateFramework,
        &DeactivateFrameworkMessage::framework_id);

  install<ResourceRequestMessage>(
      &Master::resourceRequest,
      &ResourceRequestMessage::framework_id,
      &ResourceRequestMessage::requests);

  install<LaunchTasksMessage>(
      &Master::launchTasks,
      &LaunchTasksMessage::framework_id,
      &LaunchTasksMessage::offer_id,
      &LaunchTasksMessage::tasks,
      &LaunchTasksMessage::filters);

  install<ReviveOffersMessage>(
      &Master::reviveOffers,
      &ReviveOffersMessage::framework_id);

  install<KillTaskMessage>(
      &Master::killTask,
      &KillTaskMessage::framework_id,
      &KillTaskMessage::task_id);

  install<FrameworkToExecutorMessage>(
      &Master::schedulerMessage,
      &FrameworkToExecutorMessage::slave_id,
      &FrameworkToExecutorMessage::framework_id,
      &FrameworkToExecutorMessage::executor_id,
      &FrameworkToExecutorMessage::data);

  install<RegisterSlaveMessage>(
      &Master::registerSlave,
      &RegisterSlaveMessage::slave);

  install<ReregisterSlaveMessage>(
      &Master::reregisterSlave,
      &ReregisterSlaveMessage::slave_id,
      &ReregisterSlaveMessage::slave,
      &ReregisterSlaveMessage::executor_infos,
      &ReregisterSlaveMessage::tasks);

  install<UnregisterSlaveMessage>(
      &Master::unregisterSlave,
      &UnregisterSlaveMessage::slave_id);

  install<StatusUpdateMessage>(
      &Master::statusUpdate,
      &StatusUpdateMessage::update,
      &StatusUpdateMessage::pid);

  install<ExecutorToFrameworkMessage>(
      &Master::executorMessage,
      &ExecutorToFrameworkMessage::slave_id,
      &ExecutorToFrameworkMessage::framework_id,
      &ExecutorToFrameworkMessage::executor_id,
      &ExecutorToFrameworkMessage::data);

  install<ExitedExecutorMessage>(
      &Master::exitedExecutor,
      &ExitedExecutorMessage::slave_id,
      &ExitedExecutorMessage::framework_id,
      &ExitedExecutorMessage::executor_id,
      &ExitedExecutorMessage::status);

  // Setup HTTP request handlers.
  route("/redirect", bind(&http::redirect, cref(*this), params::_1));
  route("/vars", bind(&http::vars, cref(*this), params::_1));
  route("/stats.json", bind(&http::json::stats, cref(*this), params::_1));
  route("/state.json", bind(&http::json::state, cref(*this), params::_1));

  // Provide HTTP assets from a "webui" directory. This is either
  // specified via flags (which is necessary for running out of the
  // build directory before 'make install') or determined at build
  // time via the preprocessor macro '-DMESOS_WEBUI_DIR' set in the
  // Makefile.
  provide("", path::join(flags.webui_dir, "master/static/index.html"));
  provide("static", path::join(flags.webui_dir, "master/static"));

  // TODO(benh): Ask glog for file name (i.e., mesos-master.INFO).
  // Blocked on http://code.google.com/p/google-glog/issues/detail?id=116
  // Alternatively, initialize() could take the executable name.
  if (flags.log_dir.isSome()) {
    string logPath = path::join(flags.log_dir.get(), "mesos-master.INFO");
    Future<Nothing> result = files->attach(logPath, "/log");
    result.onAny(defer(self(), &Self::fileAttached, result, logPath));
  }
}


void Master::finalize()
{
  LOG(INFO) << "Master terminating";
  foreachvalue (Slave* slave, slaves) {
    send(slave->pid, ShutdownMessage());
  }

  terminate(allocator);
  wait(allocator);
}


void Master::exited(const UPID& pid)
{
  foreachvalue (Framework* framework, frameworks) {
    if (framework->pid == pid) {
      LOG(INFO) << "Framework " << framework->id << " disconnected";

//       removeFramework(framework);

      // Stop sending offers here for now.
      framework->active = false;

      // Tell the allocator to stop allocating resources to this framework.
      dispatch(allocator, &AllocatorProcess::frameworkDeactivated, framework->id);

      Seconds failoverTimeout(framework->info.failover_timeout());

      LOG(INFO) << "Giving framework " << framework->id << " "
                << failoverTimeout << " to failover";

      // Delay dispatching a message to ourselves for the timeout.
      delay(failoverTimeout, self(),
            &Master::frameworkFailoverTimeout,
            framework->id, framework->reregisteredTime);

      // Remove the framework's offers.
      foreach (Offer* offer, utils::copy(framework->offers)) {
        dispatch(allocator, &AllocatorProcess::resourcesRecovered,
                 offer->framework_id(),
                 offer->slave_id(),
                 Resources(offer->resources()));
        removeOffer(offer);
      }
      return;
    }
  }

  foreachvalue (Slave* slave, slaves) {
    if (slave->pid == pid) {
      LOG(INFO) << "Slave " << slave->id << "(" << slave->info.hostname()
                << ") disconnected";
      removeSlave(slave);
      return;
    }
  }
}


void Master::fileAttached(const Future<Nothing>& result, const string& path)
{
  CHECK(!result.isDiscarded());
  if (result.isReady()) {
    VLOG(1) << "Successfully attached file '" << path << "'";
  } else {
    LOG(ERROR) << "Failed to attach file '" << path << "': "
               << result.failure();
  }
}


void Master::submitScheduler(const string& name)
{
  LOG(INFO) << "Scheduler submit request for " << name;
  SubmitSchedulerResponse response;
  response.set_okay(false);
  reply(response);
}


void Master::newMasterDetected(const UPID& pid)
{
  // Check and see if we are (1) still waiting to be the elected
  // master, (2) newly elected master, (3) no longer elected master,
  // or (4) still elected master.

  leader = pid;

  if (leader != self() && !elected) {
    LOG(INFO) << "Waiting to be master!";
  } else if (leader == self() && !elected) {
    LOG(INFO) << "Elected as master!";
    elected = true;
  } else if (leader != self() && elected) {
    LOG(FATAL) << "No longer elected master ... committing suicide!";
  } else if (leader == self() && elected) {
    LOG(INFO) << "Still acting as master!";
  }
}


void Master::noMasterDetected()
{
  if (elected) {
    LOG(FATAL) << "No longer elected master ... committing suicide!";
  } else {
    LOG(FATAL) << "No master detected (?) ... committing suicide!";
  }
}


void Master::registerFramework(const FrameworkInfo& frameworkInfo)
{
  if (!elected) {
    LOG(WARNING) << "Ignoring register framework message since not elected yet";
    return;
  }

  // Check if this framework is already registered (because it retries).
  foreachvalue (Framework* framework, frameworks) {
    if (framework->pid == from) {
      LOG(INFO) << "Framework " << framework->id << " (" << framework->pid
                << ") already registered, resending acknowledgement";
      FrameworkRegisteredMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.mutable_master_info()->MergeFrom(info);
      reply(message);
      return;
    }
  }

  Framework* framework =
    new Framework(frameworkInfo, newFrameworkId(), from, Clock::now());

  LOG(INFO) << "Registering framework " << framework->id << " at " << from;

  bool rootSubmissions = flags.root_submissions;

  if (framework->info.user() == "root" && rootSubmissions == false) {
    LOG(INFO) << framework << " registering as root, but "
              << "root submissions are disabled on this cluster";
    FrameworkErrorMessage message;
    message.set_message("User 'root' is not allowed to run frameworks");
    reply(message);
    delete framework;
    return;
  }

  addFramework(framework);
}


void Master::reregisterFramework(const FrameworkInfo& frameworkInfo,
                                 bool failover)
{
  if (!elected) {
    LOG(WARNING) << "Ignoring re-register framework message since "
                 << "not elected yet";
    return;
  }

  if (!frameworkInfo.has_id() || frameworkInfo.id() == "") {
    LOG(ERROR) << "Framework re-registering without an id!";
    FrameworkErrorMessage message;
    message.set_message("Framework reregistered without a framework id");
    reply(message);
    return;
  }

  LOG(INFO) << "Re-registering framework " << frameworkInfo.id()
            << " at " << from;

  if (frameworks.count(frameworkInfo.id()) > 0) {
    // Using the "failover" of the scheduler allows us to keep a
    // scheduler that got partitioned but didn't die (in ZooKeeper
    // speak this means didn't lose their session) and then
    // eventually tried to connect to this master even though
    // another instance of their scheduler has reconnected. This
    // might not be an issue in the future when the
    // master/allocator launches the scheduler can get restarted
    // (if necessary) by the master and the master will always
    // know which scheduler is the correct one.

    Framework* framework = frameworks[frameworkInfo.id()];

    if (failover) {
      // TODO: Should we check whether the new scheduler has given
      // us a different framework name, user name or executor info?
      LOG(INFO) << "Framework " << frameworkInfo.id() << " failed over";
      failoverFramework(framework, from);
    } else {
      LOG(INFO) << "Allowing the Framework " << frameworkInfo.id()
                << " to re-register with an already used id";

      // Remove any offers sent to this framework.
      // NOTE: We need to do this because the scheduler might have
      // replied to the offers but the driver might have dropped
      // those messages since it wasn't connected to the master.
      foreach (Offer* offer, utils::copy(framework->offers)) {
        dispatch(allocator, &AllocatorProcess::resourcesRecovered,
		 offer->framework_id(), offer->slave_id(), offer->resources());
        removeOffer(offer);
      }

      FrameworkReregisteredMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkInfo.id());
      message.mutable_master_info()->MergeFrom(info);
      reply(message);
      return;
    }
  } else {
    // We don't have a framework with this ID, so we must be a newly
    // elected Mesos master to which either an existing scheduler or a
    // failed-over one is connecting. Create a Framework object and add
    // any tasks it has that have been reported by reconnecting slaves.
    Framework* framework =
      new Framework(frameworkInfo, frameworkInfo.id(), from, Clock::now());

    // TODO(benh): Check for root submissions like above!

    // Add any running tasks reported by slaves for this framework.
    foreachvalue (Slave* slave, slaves) {
      foreachvalue (Task* task, slave->tasks) {
        if (framework->id == task->framework_id()) {
          framework->addTask(task);
          // Also add the task's executor for resource accounting.
          if (task->has_executor_id()) {
            if (!framework->hasExecutor(slave->id, task->executor_id())) {
              CHECK(slave->hasExecutor(framework->id, task->executor_id()));
              const ExecutorInfo& executorInfo =
                slave->executors[framework->id][task->executor_id()];
              framework->addExecutor(slave->id, executorInfo);
            }
          }
        }
      }
    }

    // N.B. Need to add the framwwork _after_ we add it's tasks
    // (above) so that we can properly determine the resources it's
    // currently using!
    addFramework(framework);
  }

  CHECK(frameworks.count(frameworkInfo.id()) > 0);

  // Broadcast the new framework pid to all the slaves. We have to
  // broadcast because an executor might be running on a slave but
  // it currently isn't running any tasks. This could be a
  // potential scalability issue ...
  foreachvalue (Slave* slave, slaves) {
    UpdateFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkInfo.id());
    message.set_pid(from);
    send(slave->pid, message);
  }
}


void Master::unregisterFramework(const FrameworkID& frameworkId)
{
  LOG(INFO) << "Asked to unregister framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    if (framework->pid == from) {
      removeFramework(framework);
    } else {
      LOG(WARNING) << from << " tried to unregister framework; "
                   << "expecting " << framework->pid;
    }
  }
}


void Master::deactivateFramework(const FrameworkID& frameworkId)
{
  Framework* framework = getFramework(frameworkId);

  if (framework != NULL) {
    if (framework->pid == from) {
      LOG(INFO) << "Deactivating framework " << frameworkId
                << " as requested by " << from;
      framework->active = false;
    } else {
      LOG(WARNING) << from << " tried to deactivate framework; "
                   << "expecting " << framework->pid;
    }
  }
}


void Master::resourceRequest(const FrameworkID& frameworkId,
                             const vector<Request>& requests)
{
  dispatch(allocator, &AllocatorProcess::resourcesRequested, frameworkId, requests);
}


void Master::launchTasks(const FrameworkID& frameworkId,
                         const OfferID& offerId,
                         const vector<TaskInfo>& tasks,
                         const Filters& filters)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    // TODO(benh): Support offer "hoarding" and allow multiple offers
    // *from the same slave* to be used to launch tasks. This can be
    // accomplished rather easily by collecting and merging all offers
    // into a mega-offer and passing that offer to
    // Master::processTasks.
    Offer* offer = getOffer(offerId);
    if (offer != NULL) {
      CHECK(offer->framework_id() == frameworkId);
      Slave* slave = getSlave(offer->slave_id());
      CHECK(slave != NULL) << "An offer should not outlive a slave!";
      processTasks(offer, framework, slave, tasks, filters);
    } else {
      // The offer is gone (possibly rescinded, lost slave, re-reply
      // to same offer, etc). Report all tasks in it as failed.
      // TODO: Consider adding a new task state TASK_INVALID for
      // situations like these.
      LOG(WARNING) << "Offer " << offerId << " is no longer valid";
      foreach (const TaskInfo& task, tasks) {
        StatusUpdateMessage message;
        StatusUpdate* update = message.mutable_update();
        update->mutable_framework_id()->MergeFrom(frameworkId);
        TaskStatus* status = update->mutable_status();
        status->mutable_task_id()->MergeFrom(task.task_id());
        status->set_state(TASK_LOST);
        status->set_message("Task launched with invalid offer");
        update->set_timestamp(Clock::now());
        update->set_uuid(UUID::random().toBytes());
        send(framework->pid, message);
      }
    }
  }
}


void Master::reviveOffers(const FrameworkID& frameworkId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Reviving offers for framework " << framework->id;
    dispatch(allocator, &AllocatorProcess::offersRevived, framework->id);
  }
}


void Master::killTask(const FrameworkID& frameworkId,
                      const TaskID& taskId)
{
  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Task* task = framework->getTask(taskId);
    if (task != NULL) {
      Slave* slave = getSlave(task->slave_id());
      CHECK(slave != NULL);

      LOG(INFO) << "Telling slave " << slave->id << " ("
                << slave->info.hostname() << ")"
                << " to kill task " << taskId
                << " of framework " << frameworkId;

      KillTaskMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_task_id()->MergeFrom(taskId);
      send(slave->pid, message);
    } else {
      // TODO(benh): Once the scheduler has persistance and
      // high-availability of it's tasks, it will be the one that
      // determines that this invocation of 'killTask' is silly, and
      // can just return "locally" (i.e., after hitting only the other
      // replicas). Unfortunately, it still won't know the slave id.

      LOG(WARNING) << "Cannot kill task " << taskId
                   << " of framework " << frameworkId
                   << " because it cannot be found";
      StatusUpdateMessage message;
      StatusUpdate* update = message.mutable_update();
      update->mutable_framework_id()->MergeFrom(frameworkId);
      TaskStatus* status = update->mutable_status();
      status->mutable_task_id()->MergeFrom(taskId);
      status->set_state(TASK_LOST);
      status->set_message("Task not found");
      update->set_timestamp(Clock::now());
      update->set_uuid(UUID::random().toBytes());
      send(framework->pid, message);
    }
  }
}


void Master::schedulerMessage(const SlaveID& slaveId,
                              const FrameworkID& frameworkId,
                              const ExecutorID& executorId,
                              const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    Slave* slave = getSlave(slaveId);
    if (slave != NULL) {
      LOG(INFO) << "Sending framework message for framework "
                << frameworkId << " to slave " << slaveId
                << " (" << slave->info.hostname() << ")";

      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(slave->pid, message);

      stats.validFrameworkMessages++;
    } else {
      LOG(WARNING) << "Cannot send framework message for framework "
                   << frameworkId << " to slave " << slaveId
                   << " because slave does not exist";
      stats.invalidFrameworkMessages++;
    }
  } else {
    LOG(WARNING) << "Cannot send framework message for framework "
                 << frameworkId << " to slave " << slaveId
                 << " because framework does not exist";
    stats.invalidFrameworkMessages++;
  }
}


void Master::registerSlave(const SlaveInfo& slaveInfo)
{
  if (!elected) {
    LOG(WARNING) << "Ignoring register slave message from "
                 << slaveInfo.hostname() << " since not elected yet";
    return;
  }

  // Check if this slave is already registered (because it retries).
  foreachvalue (Slave* slave, slaves) {
    if (slave->pid == from) {
      LOG(INFO) << "Slave " << slave->id << " (" << slave->info.hostname()
                << ") already registered, resending acknowledgement";
      SlaveRegisteredMessage message;
      message.mutable_slave_id()->MergeFrom(slave->id);
      reply(message);
      return;
    }
  }

  Slave* slave = new Slave(slaveInfo, newSlaveId(), from, Clock::now());

  LOG(INFO) << "Attempting to register slave on " << slave->info.hostname()
            << " at " << slave->pid;

  // TODO(benh): We assume all slaves can register for now.
  CHECK(flags.slaves == "*");
  activatedSlaveHostnamePort(slave->info.hostname(), slave->pid.port);
  addSlave(slave);

//   // Checks if this slave, or if all slaves, can be accepted.
//   if (slaveHostnamePorts.contains(slaveInfo.hostname(), from.port)) {
//     run(&SlaveRegistrar::run, slave, self());
//   } else if (flags.slaves == "*") {
//     run(&SlaveRegistrar::run, slave, self(), slavesManager->self());
//   } else {
//     LOG(WARNING) << "Cannot register slave at "
//                  << slaveInfo.hostname() << ":" << from.port
//                  << " because not in allocated set of slaves!";
//     reply(ShutdownMessage());
//   }
}


void Master::reregisterSlave(const SlaveID& slaveId,
                             const SlaveInfo& slaveInfo,
                             const vector<ExecutorInfo>& executorInfos,
                             const vector<Task>& tasks)
{
  if (!elected) {
    LOG(WARNING) << "Ignoring re-register slave message from "
                 << slaveInfo.hostname() << " since not elected yet";
    return;
  }

  if (slaveId == "") {
    LOG(ERROR) << "Slave re-registered without an id!";
    reply(ShutdownMessage());
  } else {
    Slave* slave = getSlave(slaveId);
    if (slave != NULL) {
      // NOTE: This handles the case where a slave tries to
      // re-register with an existing master (e.g. because of a
      // spurious zookeeper session expiration).
      // For now, we assume this slave is not nefarious (eventually
      // this will be handled by orthogonal security measures like key
      // based authentication).

      LOG(WARNING) << "Slave at " << from << " (" << slave->info.hostname()
                   << ") is being allowed to re-register with an already"
                   << " in use id (" << slaveId << ")";

      SlaveReregisteredMessage message;
      message.mutable_slave_id()->MergeFrom(slave->id);
      reply(message);
    } else {
      Slave* slave = new Slave(slaveInfo, slaveId, from, Clock::now());

      LOG(INFO) << "Attempting to re-register slave " << slave->id << " at "
                << slave->pid << " (" << slave->info.hostname() << ")";

      // TODO(benh): We assume all slaves can register for now.
      CHECK(flags.slaves == "*");
      activatedSlaveHostnamePort(slave->info.hostname(), slave->pid.port);
      readdSlave(slave, executorInfos, tasks);

//       // Checks if this slave, or if all slaves, can be accepted.
//       if (slaveHostnamePorts.contains(slaveInfo.hostname(), from.port)) {
//         run(&SlaveReregistrar::run, slave, executorInfos, tasks, self());
//       } else if (flags.slaves == "*") {
//         run(&SlaveReregistrar::run,
//             slave, executorInfos, tasks, self(), slavesManager->self());
//       } else {
//         LOG(WARNING) << "Cannot re-register slave at "
//                      << slaveInfo.hostname() << ":" << from.port
//                      << " because not in allocated set of slaves!";
//         reply(ShutdownMessage());
//       }
    }
  }
}


void Master::unregisterSlave(const SlaveID& slaveId)
{
  LOG(INFO) << "Asked to unregister slave " << slaveId;

  // TODO(benh): Check that only the slave is asking to unregister?

  Slave* slave = getSlave(slaveId);
  if (slave != NULL) {
    removeSlave(slave);
  }
}


void Master::statusUpdate(const StatusUpdate& update, const UPID& pid)
{
  const TaskStatus& status = update.status();

  LOG(INFO) << "Status update from " << from
            << ": task " << status.task_id()
            << " of framework " << update.framework_id()
            << " is now in state " << status.state();

  Slave* slave = getSlave(update.slave_id());
  if (slave != NULL) {
    Framework* framework = getFramework(update.framework_id());
    if (framework != NULL) {
      // Pass on the (transformed) status update to the framework.
      StatusUpdateMessage message;
      message.mutable_update()->MergeFrom(update);
      message.set_pid(pid);
      send(framework->pid, message);

      // Lookup the task and see if we need to update anything locally.
      Task* task = slave->getTask(update.framework_id(), status.task_id());
      if (task != NULL) {
        task->set_state(status.state());

        // Handle the task appropriately if it's terminated.
        if (status.state() == TASK_FINISHED ||
            status.state() == TASK_FAILED ||
            status.state() == TASK_KILLED ||
            status.state() == TASK_LOST) {
          removeTask(task);
        }

        stats.tasks[status.state()]++;

        stats.validStatusUpdates++;
      } else {
        LOG(WARNING) << "Status update from " << from << " ("
                     << slave->info.hostname() << "): error, couldn't lookup "
                     << "task " << status.task_id();
        stats.invalidStatusUpdates++;
      }
    } else {
      LOG(WARNING) << "Status update from " << from << " ("
                   << slave->info.hostname() << "): error, couldn't lookup "
                   << "framework " << update.framework_id();
      stats.invalidStatusUpdates++;
    }
  } else {
    LOG(WARNING) << "Status update from " << from
                 << ": error, couldn't lookup slave "
                 << update.slave_id();
    stats.invalidStatusUpdates++;
  }
}


void Master::executorMessage(const SlaveID& slaveId,
                             const FrameworkID& frameworkId,
                             const ExecutorID& executorId,
                             const string& data)
{
  Slave* slave = getSlave(slaveId);
  if (slave != NULL) {
    Framework* framework = getFramework(frameworkId);
    if (framework != NULL) {
      LOG(INFO) << "Sending framework message from slave " << slaveId << " ("
                << slave->info.hostname() << ")"
                << " to framework " << frameworkId;
      ExecutorToFrameworkMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(framework->pid, message);

      stats.validFrameworkMessages++;
    } else {
      LOG(WARNING) << "Cannot send framework message from slave "
                   << slaveId << " (" << slave->info.hostname()
                   << ") to framework " << frameworkId
                   << " because framework does not exist";
      stats.invalidFrameworkMessages++;
    }
  } else {
    LOG(WARNING) << "Cannot send framework message from slave "
                 << slaveId << " to framework " << frameworkId
                 << " because slave does not exist";
    stats.invalidFrameworkMessages++;
  }
}


void Master::exitedExecutor(const SlaveID& slaveId,
                            const FrameworkID& frameworkId,
                            const ExecutorID& executorId,
                            int32_t status)
{
  // Only update master's internal data structures here for properly accounting.
  // The TASK_LOST updates are handled by the slave.
  Slave* slave = getSlave(slaveId);
  if (slave != NULL) {
    // Tell the allocator about the recovered resources.
    if (slave->hasExecutor(frameworkId, executorId)) {
      ExecutorInfo executor = slave->executors[frameworkId][executorId];

      LOG(INFO) << "Executor " << executorId
                << " of framework " << frameworkId
                << " on slave " << slaveId
                << " (" << slave->info.hostname() << ")"
                << " exited with status " << status;

      dispatch(allocator, &AllocatorProcess::resourcesRecovered,
               frameworkId,
               slaveId,
               Resources(executor.resources()));

      // Remove executor from slave and framework.
      slave->removeExecutor(frameworkId, executorId);
    } else {
      LOG(WARNING) << "Ignoring unknown exited executor "
                   << executorId << " on slave " << slaveId
                   << " (" << slave->info.hostname() << ")";
    }

    Framework* framework = getFramework(frameworkId);
    if (framework != NULL) {
      framework->removeExecutor(slave->id, executorId);

      // TODO(benh): Send the framework it's executor's exit status?
      // Or maybe at least have something like
      // Scheduler::executorLost?
    }
  } else {
    LOG(INFO) << "Ignoring unknown exited executor " << executorId
              << " of framework " << frameworkId
              << " on unknown slave " << slaveId;
  }
}


void Master::activatedSlaveHostnamePort(const string& hostname, uint16_t port)
{
  LOG(INFO) << "Master now considering a slave at "
            << hostname << ":" << port << " as active";
  slaveHostnamePorts.put(hostname, port);
}


void Master::deactivatedSlaveHostnamePort(const string& hostname,
                                          uint16_t port)
{
  if (slaveHostnamePorts.contains(hostname, port)) {
    // Look for a connected slave and remove it.
    foreachvalue (Slave* slave, slaves) {
      if (slave->info.hostname() == hostname && slave->pid.port == port) {
        LOG(WARNING) << "Removing slave " << slave->id << " at "
                     << hostname << ":" << port
                     << " because it has been deactivated";
        send(slave->pid, ShutdownMessage());
        removeSlave(slave);
        break;
      }
    }

    LOG(INFO) << "Master now considering a slave at "
	            << hostname << ":" << port << " as inactive";
    slaveHostnamePorts.remove(hostname, port);
  }
}


void Master::frameworkFailoverTimeout(const FrameworkID& frameworkId,
                                      double reregisteredTime)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL && !framework->active &&
      framework->reregisteredTime == reregisteredTime) {
    LOG(INFO) << "Framework failover timeout, removing framework "
              << framework->id;
    removeFramework(framework);
  }
}


void Master::offer(const FrameworkID& frameworkId,
                   const hashmap<SlaveID, Resources>& resources)
{
  if (!frameworks.contains(frameworkId) || !frameworks[frameworkId]->active) {
    LOG(WARNING) << "Master returning resources offered to framework "
                 << frameworkId << " because the framework"
                 << " has terminated or is inactive";

    foreachpair (const SlaveID& slaveId, const Resources& offered, resources) {
      dispatch(allocator, &AllocatorProcess::resourcesRecovered,
               frameworkId, slaveId, offered);
    }
    return;
  }

  // Create an offer for each slave and add it to the message.
  ResourceOffersMessage message;

  Framework* framework = frameworks[frameworkId];
  foreachpair (const SlaveID& slaveId, const Resources& offered, resources) {
    if (!slaves.contains(slaveId)) {
      LOG(WARNING) << "Master returning resources offered to framework "
                   << frameworkId << " because slave " << slaveId
                   << " is not valid";

      dispatch(allocator, &AllocatorProcess::resourcesRecovered,
               frameworkId, slaveId, offered);
      continue;
    }

    Slave* slave = slaves[slaveId];

    Offer* offer = new Offer();
    offer->mutable_id()->MergeFrom(newOfferId());
    offer->mutable_framework_id()->MergeFrom(framework->id);
    offer->mutable_slave_id()->MergeFrom(slave->id);
    offer->set_hostname(slave->info.hostname());
    offer->mutable_resources()->MergeFrom(offered);
    offer->mutable_attributes()->MergeFrom(slave->info.attributes());

    // Add all framework's executors running on this slave.
    if (slave->executors.contains(framework->id)) {
      const hashmap<ExecutorID, ExecutorInfo>& executors =
        slave->executors[framework->id];
      foreachkey (const ExecutorID& executorId, executors) {
        offer->add_executor_ids()->MergeFrom(executorId);
      }
    }

    offers[offer->id()] = offer;

    framework->addOffer(offer);
    slave->addOffer(offer);

    // Add the offer *AND* the corresponding slave's PID.
    message.add_offers()->MergeFrom(*offer);
    message.add_pids(slave->pid);
  }

  if (message.offers().size() == 0) {
    return;
  }

  LOG(INFO) << "Sending " << message.offers().size()
            << " offers to framework " << framework->id;

  send(framework->pid, message);
}


// Return connected frameworks that are not in the process of being removed
vector<Framework*> Master::getActiveFrameworks() const
{
  vector <Framework*> result;
  foreachvalue (Framework* framework, frameworks) {
    if (framework->active) {
      result.push_back(framework);
    }
  }
  return result;
}


// We use the visitor pattern to abstract the process of performing
// any validations, aggregations, etc. of tasks that a framework
// attempts to run within the resources provided by an offer. A
// visitor can return an optional error (typedef'ed as an option of a
// string) which will cause the master to send a failed status update
// back to the framework for only that task description. An instance
// will be reused for each task description from same offer, but not
// for task descriptions from different offers.
typedef Option<string> TaskInfoError;

struct TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      Offer* offer,
      Framework* framework,
      Slave* slave) = 0;

  virtual ~TaskInfoVisitor() {}
};


// Checks that the slave ID used by a task is correct.
struct SlaveIDChecker : TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      Offer* offer,
      Framework* framework,
      Slave* slave)
  {
    if (!(task.slave_id() == slave->id)) {
      return TaskInfoError::some(
          "Task uses invalid slave: " + task.slave_id().value());
    }

    return TaskInfoError::none();
  }
};


// Checks that each task uses a unique ID. Regardless of whether a
// task actually gets launched (for example, another checker may
// return an error for a task), we always consider it an error when a
// task tries to re-use an ID.
struct UniqueTaskIDChecker : TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      Offer* offer,
      Framework* framework,
      Slave* slave)
  {
    const TaskID& taskId = task.task_id();

    if (ids.contains(taskId) || framework->tasks.contains(taskId)) {
      return TaskInfoError::some(
          "Task has duplicate ID: " + taskId.value());
    }

    ids.insert(taskId);

    return TaskInfoError::none();
  }

  hashset<TaskID> ids;
};


// Checks that the used resources by a task (and executor if
// necessary) on each slave does not exceed the total resources
// offered on that slave
struct ResourceUsageChecker : TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      Offer* offer,
      Framework* framework,
      Slave* slave)
  {
    if (task.resources().size() == 0) {
      return TaskInfoError::some("Task uses no resources");
    }

    foreach (const Resource& resource, task.resources()) {
      if (!Resources::isAllocatable(resource)) {
        // TODO(benh): Send back the invalid resources?
        return TaskInfoError::some("Task uses invalid resources");
      }
    }

    // Check if this task uses more resources than offered.
    Resources taskResources = task.resources();

    if (!((usedResources + taskResources) <= offer->resources())) {
      LOG(WARNING) << "Task " << task.task_id() << " attempted to use "
                   << taskResources << " combined with already used "
                   << usedResources << " is greater than offered "
                   << offer->resources();

      return TaskInfoError::some("Task uses more resources than offered");
    }

    // Check this task's executor's resources.
    if (task.has_executor()) {
      // TODO(benh): Check that the executor uses some resources.

      foreach (const Resource& resource, task.executor().resources()) {
        if (!Resources::isAllocatable(resource)) {
          // TODO(benh): Send back the invalid resources?
          LOG(WARNING) << "Executor for task " << task.task_id()
                       << " uses invalid resources " << resource;
          return TaskInfoError::some("Task's executor uses invalid resources");
        }
      }

      // Check if this task's executor is running, and if not check if
      // the task + the executor use more resources than offered.
      if (!executors.contains(task.executor().executor_id())) {
        if (!slave->hasExecutor(framework->id, task.executor().executor_id())) {
          taskResources += task.executor().resources();
          if (!((usedResources + taskResources) <= offer->resources())) {
            LOG(WARNING) << "Task " << task.task_id() << " + executor attempted"
                         << " to use " << taskResources << " combined with"
                         << " already used " << usedResources << " is greater"
                         << " than offered " << offer->resources();

            return TaskInfoError::some(
                "Task + executor uses more resources than offered");
          }
        }
        executors.insert(task.executor().executor_id());
      }
    }

    usedResources += taskResources;

    return TaskInfoError::none();
  }

  Resources usedResources;
  hashset<ExecutorID> executors;
};


// Checks that tasks that use the "same" executor (i.e., same
// ExecutorID) have an identical ExecutorInfo.
struct ExecutorInfoChecker : TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      Offer* offer,
      Framework* framework,
      Slave* slave)
  {
    if (task.has_executor()) {
      if (slave->hasExecutor(framework->id, task.executor().executor_id())) {
        const ExecutorInfo& executorInfo =
          slave->executors[framework->id][task.executor().executor_id()];
        if (!(task.executor() == executorInfo)) {
          return TaskInfoError::some(
              "Task has invalid ExecutorInfo (existing ExecutorInfo"
              " with same ExecutorID is not compatible)");
        }
      }
    }

    return TaskInfoError::none();
  }
};


// Process a resource offer reply (for a non-cancelled offer) by
// launching the desired tasks (if the offer contains a valid set of
// tasks) and reporting used resources to the allocator.
void Master::processTasks(Offer* offer,
                          Framework* framework,
                          Slave* slave,
                          const vector<TaskInfo>& tasks,
                          const Filters& filters)
{
  VLOG(1) << "Processing reply for offer " << offer->id()
          << " on slave " << slave->id
          << " (" << slave->info.hostname() << ")"
          << " for framework " << framework->id;

  Resources usedResources; // Accumulated resources used from this offer.

  // Create task visitors.
  list<TaskInfoVisitor*> visitors;
  visitors.push_back(new SlaveIDChecker());
  visitors.push_back(new UniqueTaskIDChecker());
  visitors.push_back(new ResourceUsageChecker());
  visitors.push_back(new ExecutorInfoChecker());

  // Loop through each task and check it's validity.
  foreach (const TaskInfo& task, tasks) {
    // Possible error found while checking task's validity.
    TaskInfoError error = TaskInfoError::none();

    // Invoke each visitor.
    foreach (TaskInfoVisitor* visitor, visitors) {
      error = (*visitor)(task, offer, framework, slave);
      if (error.isSome()) {
        break;
      }
    }

    if (error.isNone()) {
      // Task looks good, get it running!
      usedResources += launchTask(task, framework, slave);
    } else {
      // Error validating task, send a failed status update.
      LOG(WARNING) << "Error validating task " << task.task_id()
                   << " : " << error.get();
      StatusUpdateMessage message;
      StatusUpdate* update = message.mutable_update();
      update->mutable_framework_id()->MergeFrom(framework->id);
      TaskStatus* status = update->mutable_status();
      status->mutable_task_id()->MergeFrom(task.task_id());
      status->set_state(TASK_LOST);
      status->set_message(error.get());
      update->set_timestamp(Clock::now());
      update->set_uuid(UUID::random().toBytes());
      send(framework->pid, message);
    }
  }

  // Cleanup visitors.
  do {
    TaskInfoVisitor* visitor = visitors.front();
    visitors.pop_front();
    delete visitor;
  } while (!visitors.empty());

  // All used resources should be allocatable, enforced by our validators.
  CHECK(usedResources == usedResources.allocatable());

  // Calculate unused resources.
  Resources unusedResources = offer->resources() - usedResources;

  if (unusedResources.allocatable().size() > 0) {
    // Tell the allocator about the unused (e.g., refused) resources.
    dispatch(allocator, &AllocatorProcess::resourcesUnused,
	     offer->framework_id(),
	     offer->slave_id(),
	     unusedResources,
	     filters);
  }

  removeOffer(offer);
}


Resources Master::launchTask(const TaskInfo& task,
                             Framework* framework,
                             Slave* slave)
{
  CHECK(framework != NULL);
  CHECK(slave != NULL);

  Resources resources; // Total resources used on slave by launching this task.

  // Determine if this task launches an executor, and if so make sure
  // the slave and framework state has been updated accordingly.
  Option<ExecutorID> executorId;

  if (task.has_executor()) {
    // TODO(benh): Refactor this code into Slave::addTask.
    if (!slave->hasExecutor(framework->id, task.executor().executor_id())) {
      CHECK(!framework->hasExecutor(slave->id, task.executor().executor_id()));
      slave->addExecutor(framework->id, task.executor());
      framework->addExecutor(slave->id, task.executor());
      resources += task.executor().resources();
    }

    executorId = Option<ExecutorID>::some(task.executor().executor_id());
  }

  // Add the task to the framework and slave.
  Task* t = new Task();
  t->mutable_framework_id()->MergeFrom(framework->id);
  t->set_state(TASK_STAGING);
  t->set_name(task.name());
  t->mutable_task_id()->MergeFrom(task.task_id());
  t->mutable_slave_id()->MergeFrom(task.slave_id());
  t->mutable_resources()->MergeFrom(task.resources());

  if (executorId.isSome()) {
    t->mutable_executor_id()->MergeFrom(executorId.get());
  }

  framework->addTask(t);

  slave->addTask(t);

  resources += task.resources();

  // Tell the slave to launch the task!
  LOG(INFO) << "Launching task " << task.task_id()
            << " with resources " << task.resources() << " on slave "
            << slave->id << " (" << slave->info.hostname() << ")";

  RunTaskMessage message;
  message.mutable_framework()->MergeFrom(framework->info);
  message.mutable_framework_id()->MergeFrom(framework->id);
  message.set_pid(framework->pid);
  message.mutable_task()->MergeFrom(task);
  send(slave->pid, message);

  stats.tasks[TASK_STAGING]++;

  return resources;
}


void Master::addFramework(Framework* framework)
{
  CHECK(frameworks.count(framework->id) == 0);

  frameworks[framework->id] = framework;

  link(framework->pid);

  FrameworkRegisteredMessage message;
  message.mutable_framework_id()->MergeFrom(framework->id);
  message.mutable_master_info()->MergeFrom(info);
  send(framework->pid, message);

  dispatch(allocator, &AllocatorProcess::frameworkAdded,
           framework->id, framework->info, framework->resources);
}


// Replace the scheduler for a framework with a new process ID, in the
// event of a scheduler failover.
void Master::failoverFramework(Framework* framework, const UPID& newPid)
{
  const UPID& oldPid = framework->pid;

  {
    FrameworkErrorMessage message;
    message.set_message("Framework failed over");
    send(oldPid, message);
  }

  // TODO(benh): unlink(oldPid);

  framework->pid = newPid;
  link(newPid);

  // Make sure we can get offers again.
  if (!framework->active) {
    framework->active = true;
    dispatch(allocator, &AllocatorProcess::frameworkActivated,
             framework->id, framework->info);
  }

  framework->reregisteredTime = Clock::now();

  {
    FrameworkRegisteredMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id);
    message.mutable_master_info()->MergeFrom(info);
    send(newPid, message);
  }

  // Remove the framework's offers (if they weren't removed before).
  // We do this after we have updated the pid and sent the framework
  // registered message so that the allocator can immediately re-offer
  // these resources to this framework if it wants.
  // TODO(benh): Consider just reoffering these to
  foreach (Offer* offer, utils::copy(framework->offers)) {
    dispatch(allocator, &AllocatorProcess::resourcesRecovered,
             offer->framework_id(),
             offer->slave_id(),
             Resources(offer->resources()));
    removeOffer(offer);
  }
}


void Master::removeFramework(Framework* framework)
{
  if (framework->active) {
    // Tell the allocator to stop allocating resources to this framework.
    dispatch(allocator, &AllocatorProcess::frameworkDeactivated, framework->id);
  }

  // Tell slaves to shutdown the framework.
  foreachvalue (Slave* slave, slaves) {
    ShutdownFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id);
    send(slave->pid, message);
  }

  // Remove pointers to the framework's tasks in slaves.
  foreachvalue (Task* task, utils::copy(framework->tasks)) {
    Slave* slave = getSlave(task->slave_id());
    // Since we only find out about tasks when the slave reregisters,
    // it must be the case that the slave exists!
    CHECK(slave != NULL);
    removeTask(task);
  }

  // Remove the framework's offers (if they weren't removed before).
  foreach (Offer* offer, utils::copy(framework->offers)) {
    dispatch(allocator, &AllocatorProcess::resourcesRecovered,
             offer->framework_id(),
             offer->slave_id(),
             Resources(offer->resources()));
    removeOffer(offer);
  }

  // Remove the framework's executors for correct resource accounting.
  foreachkey (const SlaveID& slaveId, framework->executors) {
    Slave* slave = getSlave(slaveId);
    if (slave != NULL) {
      foreachpair (const ExecutorID& executorId,
                   const ExecutorInfo& executorInfo,
                   framework->executors[slaveId]) {
        dispatch(allocator, &AllocatorProcess::resourcesRecovered,
                 framework->id,
                 slave->id,
                 executorInfo.resources());
        slave->removeExecutor(framework->id, executorId);
      }
    }
  }

  // TODO(benh): Similar code between removeFramework and
  // failoverFramework needs to be shared!

  // TODO(benh): unlink(framework->pid);

  framework->unregisteredTime = Clock::now();

  completedFrameworks.push_back(*framework);

  if (completedFrameworks.size() > MAX_COMPLETED_FRAMEWORKS) {
    completedFrameworks.pop_front();
  }

  // Delete it.
  frameworks.erase(framework->id);
  dispatch(allocator, &AllocatorProcess::frameworkRemoved, framework->id);

  delete framework;
}


void Master::addSlave(Slave* slave, bool reregister)
{
  CHECK(slave != NULL);

  LOG(INFO) << "Adding slave " << slave->id
            << " at " << slave->info.hostname()
            << " with " << slave->info.resources();

  slaves[slave->id] = slave;

  link(slave->pid);

  if (!reregister) {
    SlaveRegisteredMessage message;
    message.mutable_slave_id()->MergeFrom(slave->id);
    send(slave->pid, message);
  } else {
    SlaveReregisteredMessage message;
    message.mutable_slave_id()->MergeFrom(slave->id);
    send(slave->pid, message);
  }

  // TODO(benh):
  //     // Ask the slaves manager to monitor this slave for us.
  //     dispatch(slavesManager->self(), &SlavesManager::monitor,
  //              slave->pid, slave->info, slave->id);

  // Set up an observer for the slave.
  slave->observer = new SlaveObserver(slave->pid, slave->info,
                                      slave->id, self());
  spawn(slave->observer);

  if (!reregister) {
    dispatch(allocator, &AllocatorProcess::slaveAdded,
             slave->id, slave->info, hashmap<FrameworkID, Resources>());
  }
}


void Master::readdSlave(Slave* slave,
			const vector<ExecutorInfo>& executorInfos,
			const vector<Task>& tasks)
{
  CHECK(slave != NULL);

  addSlave(slave, true);

  // Add the executors and tasks to the slave and framework state and
  // determine the resources that have been allocated to frameworks.
  hashmap<FrameworkID, Resources> resources;

  foreach (const ExecutorInfo& executorInfo, executorInfos) {
    // TODO(benh): Remove this check if framework_id becomes required
    // on ExecutorInfo (which will also mean we can remove setting it
    // in the slave).
    CHECK(executorInfo.has_framework_id());
    if (!slave->hasExecutor(executorInfo.framework_id(),
                            executorInfo.executor_id())) {
      slave->addExecutor(executorInfo.framework_id(), executorInfo);
    }

    Framework* framework = getFramework(executorInfo.framework_id());
    if (framework != NULL) {
      if (!framework->hasExecutor(slave->id, executorInfo.executor_id())) {
        framework->addExecutor(slave->id, executorInfo);
      }
    }

    resources[executorInfo.framework_id()] += executorInfo.resources();
  }

  foreach (const Task& task, tasks) {
    Task* t = new Task(task);

    // Add the task to the slave.
    slave->addTask(t);

    // Try and add the task to the framework too, but since the
    // framework might not yet be connected we won't be able to
    // add them. However, when the framework connects later we
    // will add them then. We also tell this slave the current
    // framework pid for this task. Again, we do the same thing
    // if a framework currently isn't registered.
    Framework* framework = getFramework(task.framework_id());
    if (framework != NULL) {
      framework->addTask(t);
      UpdateFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.set_pid(framework->pid);
      send(slave->pid, message);
    } else {
      // TODO(benh): We should really put a timeout on how long we
      // keep tasks running on a slave that never have frameworks
      // reregister and claim them.
      LOG(WARNING) << "Possibly orphaned task " << task.task_id()
                   << " of framework " << task.framework_id()
                   << " running on slave " << slave->id << " ("
                   << slave->info.hostname() << ")";
    }

    resources[task.framework_id()] += task.resources();
  }

  dispatch(allocator, &AllocatorProcess::slaveAdded,
           slave->id, slave->info, resources);
}


// Lose all of a slave's tasks and delete the slave object
void Master::removeSlave(Slave* slave)
{
  // Remove pointers to slave's tasks in frameworks, and send status updates
  foreachvalue (Task* task, utils::copy(slave->tasks)) {
    Framework* framework = getFramework(task->framework_id());
    // A framework might not actually exist because the master failed
    // over and the framework hasn't reconnected. This can be a tricky
    // situation for frameworks that want to have high-availability,
    // because if they eventually do connect they won't ever get a
    // status update about this task.  Perhaps in the future what we
    // want to do is create a local Framework object to represent that
    // framework until it fails over. See the TODO above in
    // Master::reregisterSlave.
    if (framework != NULL) {
      StatusUpdateMessage message;
      StatusUpdate* update = message.mutable_update();
      update->mutable_framework_id()->MergeFrom(task->framework_id());
      update->mutable_executor_id()->MergeFrom(task->executor_id());
      update->mutable_slave_id()->MergeFrom(task->slave_id());
      TaskStatus* status = update->mutable_status();
      status->mutable_task_id()->MergeFrom(task->task_id());
      status->set_state(TASK_LOST);
      status->set_message("Slave " + slave->info.hostname() + " removed");
      update->set_timestamp(Clock::now());
      update->set_uuid(UUID::random().toBytes());
      send(framework->pid, message);
    }
    removeTask(task);
  }

  // Remove and rescind offers (but don't "recover" any resources
  // since the slave is gone).
  foreach (Offer* offer, utils::copy(slave->offers)) {
    removeOffer(offer, true); // Rescind!
  }

  // Remove executors from the slave for proper resource accounting.
  foreachkey (const FrameworkID& frameworkId, slave->executors) {
    Framework* framework = getFramework(frameworkId);
    if (framework != NULL) {
      foreachkey (const ExecutorID& executorId, slave->executors[frameworkId]) {
        framework->removeExecutor(slave->id, executorId);
      }
    }
  }

  // Send lost-slave message to all frameworks (this helps them re-run
  // previously finished tasks whose output was on the lost slave).
  foreachvalue (Framework* framework, frameworks) {
    LostSlaveMessage message;
    message.mutable_slave_id()->MergeFrom(slave->id);
    send(framework->pid, message);
  }

  // TODO(benh):
  //     // Tell the slaves manager to stop monitoring this slave for us.
  //     dispatch(slavesManager->self(), &SlavesManager::forget,
  //              slave->pid, slave->info, slave->id);

  // Kill the slave observer.
  terminate(slave->observer);
  wait(slave->observer);

  delete slave->observer;

  // TODO(benh): unlink(slave->pid);

  // Delete it.
  slaves.erase(slave->id);
  dispatch(allocator, &AllocatorProcess::slaveRemoved, slave->id);
  delete slave;
}


void Master::removeTask(Task* task)
{
  // Remove from framework.
  Framework* framework = getFramework(task->framework_id());
  if (framework != NULL) { // A framework might not be re-connected yet.
    framework->removeTask(task);
  }

  // Remove from slave.
  Slave* slave = getSlave(task->slave_id());
  CHECK(slave != NULL);
  slave->removeTask(task);

  // Tell the allocator about the recovered resources.
  dispatch(allocator, &AllocatorProcess::resourcesRecovered,
           task->framework_id(),
           task->slave_id(),
           Resources(task->resources()));

  delete task;
}


void Master::removeOffer(Offer* offer, bool rescind)
{
  // Remove from framework.
  Framework* framework = getFramework(offer->framework_id());
  CHECK(framework != NULL);
  framework->removeOffer(offer);

  // Remove from slave.
  Slave* slave = getSlave(offer->slave_id());
  CHECK(slave != NULL);
  slave->removeOffer(offer);

  if (rescind) {
    RescindResourceOfferMessage message;
    message.mutable_offer_id()->MergeFrom(offer->id());
    send(framework->pid, message);
  }

  // Delete it.
  offers.erase(offer->id());
  delete offer;
}


Framework* Master::getFramework(const FrameworkID& frameworkId)
{
  if (frameworks.count(frameworkId) > 0) {
    return frameworks[frameworkId];
  } else {
    return NULL;
  }
}


Slave* Master::getSlave(const SlaveID& slaveId)
{
  if (slaves.count(slaveId) > 0) {
    return slaves[slaveId];
  } else {
    return NULL;
  }
}


Offer* Master::getOffer(const OfferID& offerId)
{
  if (offers.count(offerId) > 0) {
    return offers[offerId];
  } else {
    return NULL;
  }
}


// Create a new framework ID. We format the ID as MASTERID-FWID, where
// MASTERID is the ID of the master (launch date plus fault tolerant ID)
// and FWID is an increasing integer.
FrameworkID Master::newFrameworkId()
{
  std::ostringstream out;

  out << info.id() << "-" << std::setw(4)
      << std::setfill('0') << nextFrameworkId++;

  FrameworkID frameworkId;
  frameworkId.set_value(out.str());

  return frameworkId;
}


OfferID Master::newOfferId()
{
  OfferID offerId;
  offerId.set_value(info.id() + "-" + stringify(nextOfferId++));
  return offerId;
}


SlaveID Master::newSlaveId()
{
  SlaveID slaveId;
  slaveId.set_value(info.id() + "-" + stringify(nextSlaveId++));
  return slaveId;
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
