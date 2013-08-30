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

#include <stout/check.hpp>
#include <stout/multihashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "common/build.hpp"
#include "common/date_utils.hpp"
#include "common/protobuf_utils.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

namespace params = std::tr1::placeholders;

using std::list;
using std::string;
using std::vector;

using process::wait; // Necessary on some OS's to disambiguate.

using std::tr1::cref;
using std::tr1::bind;
using std::tr1::shared_ptr;

namespace mesos {
namespace internal {
namespace master {

using allocator::Allocator;

class WhitelistWatcher : public Process<WhitelistWatcher> {
public:
  WhitelistWatcher(const string& _path, Allocator* _allocator)
  : ProcessBase(ID::generate("whitelist")),
    path(_path),
    allocator(_allocator) {}

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
      Try<string> read = os::read(path.substr(strlen("file://")));
      if (read.isError()) {
        LOG(ERROR) << "Error reading whitelist file: " << read.error() << ". "
                   << "Retrying";
        whitelist = lastWhitelist;
      } else if (read.get().empty()) {
        LOG(WARNING) << "Empty whitelist file " << path << ". "
                     << "No offers will be made!";
        whitelist = Option<hashset<string> >::some(hashset<string>());
      } else {
        hashset<string> hostnames;
        vector<string> lines = strings::tokenize(read.get(), "\n");
        foreach (const string& hostname, lines) {
          hostnames.insert(hostname);
        }
        whitelist = Option<hashset<string> >::some(hostnames);
      }
    }

    // Send the whitelist to allocator, if necessary.
    if (whitelist != lastWhitelist) {
      allocator->updateWhitelist(whitelist);
    }

    // Check again.
    lastWhitelist = whitelist;
    delay(WHITELIST_WATCH_INTERVAL, self(), &WhitelistWatcher::watch);
  }

private:
  const string path;
  Allocator* allocator;
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
    dispatch(master, &Master::deactivateSlave, slaveId);
  }

private:
  const UPID slave;
  const SlaveInfo slaveInfo;
  const SlaveID slaveId;
  const PID<Master> master;
  uint32_t timeouts;
  bool pinged;
};


Master::Master(Allocator* _allocator, Files* _files)
  : ProcessBase("master"),
    http(*this),
    flags(),
    allocator(_allocator),
    files(_files),
    completedFrameworks(MAX_COMPLETED_FRAMEWORKS) {}


Master::Master(Allocator* _allocator, Files* _files, const Flags& _flags)
  : ProcessBase("master"),
    http(*this),
    flags(_flags),
    allocator(_allocator),
    files(_files),
    completedFrameworks(MAX_COMPLETED_FRAMEWORKS) {}


Master::~Master()
{
  LOG(INFO) << "Shutting down master";

  // Remove the frameworks.
  // Note we are not deleting the pointers to the frameworks from the
  // allocator or the roles because it is unnecessary bookkeeping at
  // this point since we are shutting down.
  foreachvalue (Framework* framework, frameworks) {
    // Remove pointers to the framework's tasks in slaves.
    foreachvalue (Task* task, utils::copy(framework->tasks)) {
      Slave* slave = getSlave(task->slave_id());
      // Since we only find out about tasks when the slave re-registers,
      // it must be the case that the slave exists!
      CHECK(slave != NULL);
      removeTask(task);
    }

    // Remove the framework's offers (if they weren't removed before).
    foreach (Offer* offer, utils::copy(framework->offers)) {
      removeOffer(offer);
    }

    delete framework;
  }
  frameworks.clear();

  CHECK(offers.size() == 0);

  foreachvalue (Slave* slave, slaves) {
    LOG(INFO) << "Removing slave " << slave->id
              << " (" << slave->info.hostname() << ")";

    // Remove tasks that are in the slave but not in any framework.
    // This could happen when the framework has yet to reregister
    // after master failover.
    foreachvalue (Task* task, utils::copy(slave->tasks)) {
      removeTask(task);
    }

    // Kill the slave observer.
    terminate(slave->observer);
    wait(slave->observer);

    delete slave->observer;
    delete slave;
  }
  slaves.clear();

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

  hashmap<string, RoleInfo> roleInfos;

  // Add the default role.
  RoleInfo roleInfo;
  roleInfo.set_name("*");
  roleInfos["*"] = roleInfo;

  // Add other roles.
  if (flags.roles.isSome()) {
    vector<string> tokens = strings::tokenize(flags.roles.get(), ",");

    foreach (const std::string& role, tokens) {
      RoleInfo roleInfo;
      roleInfo.set_name(role);
      roleInfos[role] = roleInfo;
    }
  }

  // Add role weights.
  if (flags.weights.isSome()) {
    vector<string> tokens = strings::tokenize(flags.weights.get(), ",");

    foreach (const std::string& token, tokens) {
      vector<string> pair = strings::tokenize(token, "=");
      if (pair.size() != 2) {
        EXIT(1) << "Invalid weight: '" << token << "'. --weights should"
          "be of the form 'role=weight,role=weight'\n";
      } else if (!roles.contains(pair[0])) {
        EXIT(1) << "Invalid weight: '" << token << "'. " << pair[0]
                << " is not a valid role.";
      }

      double weight = atof(pair[1].c_str());
      if (weight <= 0) {
        EXIT(1) << "Invalid weight: '" << token
                << "'. Weights must be positive.";
      }

      roleInfos[pair[0]].set_weight(weight);
    }
  }

  foreachpair (const std::string& role,
               const RoleInfo& roleInfo,
               roleInfos) {
    roles[role] = new Role(roleInfo);
  }

  // Initialize the allocator.
  allocator->initialize(flags, self(), roleInfos);

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

  install<ExitedExecutorMessage>(
      &Master::exitedExecutor,
      &ExitedExecutorMessage::slave_id,
      &ExitedExecutorMessage::framework_id,
      &ExitedExecutorMessage::executor_id,
      &ExitedExecutorMessage::status);

  // Setup HTTP routes.
  route("/health",
        Http::HEALTH_HELP,
        bind(&Http::health, http, params::_1));
  route("/redirect",
        Http::REDIRECT_HELP,
        bind(&Http::redirect, http, params::_1));
  route("/stats.json",
        None(),
        bind(&Http::stats, http, params::_1));
  route("/state.json",
        None(),
        bind(&Http::state, http, params::_1));
  route("/roles.json",
        None(),
        bind(&Http::roles, http, params::_1));

  // Provide HTTP assets from a "webui" directory. This is either
  // specified via flags (which is necessary for running out of the
  // build directory before 'make install') or determined at build
  // time via the preprocessor macro '-DMESOS_WEBUI_DIR' set in the
  // Makefile.
  provide("", path::join(flags.webui_dir, "master/static/index.html"));
  provide("static", path::join(flags.webui_dir, "master/static"));

  if (flags.log_dir.isSome()) {
    Try<string> log = logging::getLogFile(google::INFO);
    if (log.isError()) {
      LOG(ERROR) << "Master log file cannot be found: " << log.error();
    } else {
      files->attach(log.get(), "/master/log")
        .onAny(defer(self(), &Self::fileAttached, params::_1, log.get()));
    }
  }
}


void Master::finalize()
{
  LOG(INFO) << "Master terminating";
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
      allocator->frameworkDeactivated(framework->id);

      // Set 'failoverTimeout' to the default and update only if the
      // input is valid.
      Try<Duration> failoverTimeout_ =
        Duration::create(FrameworkInfo().failover_timeout());
      CHECK_SOME(failoverTimeout_);
      Duration failoverTimeout = failoverTimeout_.get();

      failoverTimeout_ =
        Duration::create(framework->info.failover_timeout());
      if (failoverTimeout_.isSome()) {
        failoverTimeout = failoverTimeout_.get();
      } else {
        LOG(WARNING) << "Using the default value for 'failover_timeout' because"
                     << "the input value is invalid: "
                     << failoverTimeout_.error();
      }

      LOG(INFO) << "Giving framework " << framework->id << " "
                << failoverTimeout << " to failover";

      // Delay dispatching a message to ourselves for the timeout.
      delay(failoverTimeout,
          self(),
          &Master::frameworkFailoverTimeout,
          framework->id,
          framework->reregisteredTime);

      // Remove the framework's offers.
      foreach (Offer* offer, utils::copy(framework->offers)) {
        allocator->resourcesRecovered(
            offer->framework_id(),
            offer->slave_id(),
            Resources(offer->resources()));

        removeOffer(offer);
      }
      return;
    }
  }

  // The semantics when a slave gets disconnected are as follows:
  // 1) If the slave is not checkpointing, the slave is immediately
  //    removed and all tasks running on it are transitioned to LOST.
  //    No resources are recovered, because the slave is removed.
  // 2) If the slave is checkpointing, the frameworks running on it
  //    fall into one of the 2 cases:
  //    2.1) Framework is checkpointing: No immediate action is taken.
  //         The slave is given a chance to reconnect until the slave
  //         observer times out (75s) and removes the slave (Case 1).
  //    2.2) Framework is not-checkpointing: The slave is not removed
  //         but the framework is removed from the slave's structs,
  //         its tasks transitioned to LOST and resources recovered.
  foreachvalue (Slave* slave, slaves) {
    if (slave->pid == pid) {
      LOG(INFO) << "Slave " << slave->id << " (" << slave->info.hostname()
                << ") disconnected";

      // Remove the slave, if it is not checkpointing.
      if (!slave->info.checkpoint()) {
        LOG(INFO) << "Removing disconnected slave " << slave->id
                  << "(" << slave->info.hostname() << ") "
                  << "because it is not checkpointing!";
        removeSlave(slave);
        return;
      } else if (!slave->disconnected) {
        // Mark the slave as disconnected and remove it from the allocator.
        slave->disconnected = true;

        // TODO(vinod/Thomas): Instead of removing the slave, we should
        // have 'Allocator::slave{Reconnected, Disconnected}'.
        allocator->slaveRemoved(slave->id);

        // If a slave is checkpointing, remove all non-checkpointing
        // frameworks from the slave.
        // First, collect all the frameworks running on this slave.
        hashset<FrameworkID> frameworkIds;
        foreachvalue (Task* task, slave->tasks) {
          frameworkIds.insert(task->framework_id());
        }
        foreachkey (const FrameworkID& frameworkId, slave->executors) {
          frameworkIds.insert(frameworkId);
        }

        // Now, remove all the non-checkpointing frameworks.
        foreach (const FrameworkID& frameworkId, frameworkIds) {
          Framework* framework = getFramework(frameworkId);
          if (framework != NULL && !framework->info.checkpoint()) {
            LOG(INFO) << "Removing non-checkpointing framework " << frameworkId
                      << " from disconnected slave " << slave->id
                      << "(" << slave->info.hostname() << ")";

            removeFramework(slave, framework);
          }
        }

        foreach (Offer* offer, utils::copy(slave->offers)) {
          // TODO(vinod): We don't need to call 'Allocator::resourcesRecovered'
          // once MESOS-621 is fixed.
          allocator->resourcesRecovered(
              offer->framework_id(), slave->id, offer->resources());

          // Remove and rescind offers.
          removeOffer(offer, true); // Rescind!
        }
      } else {
        LOG(WARNING) << "Ignoring duplicate exited() notification for "
                     << "checkpointing slave " << slave->id
                     << " (" << slave->info.hostname() << ")";
      }
    }
  }
}


void Master::fileAttached(const Future<Nothing>& result, const string& path)
{
  CHECK(!result.isDiscarded());
  if (result.isReady()) {
    LOG(INFO) << "Successfully attached file '" << path << "'";
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

  if (!roles.contains(frameworkInfo.role())) {
    FrameworkErrorMessage message;
    message.set_message("Role '" + frameworkInfo.role() + "' is not valid.");
    reply(message);
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

  if (!roles.contains(frameworkInfo.role())) {
    FrameworkErrorMessage message;
    message.set_message("Role '" + frameworkInfo.role() + "' is not valid.");
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
        allocator->resourcesRecovered(offer->framework_id(),
                                      offer->slave_id(),
                                      offer->resources());
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
  allocator->resourcesRequested(frameworkId, requests);
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
      CHECK(slave != NULL)
        << "Offer " << offerId << " outlived  slave "
        << slave->id << " (" << slave->info.hostname() << ")";

      // If a slave is disconnected we should've removed its offers.
      CHECK(!slave->disconnected)
        << "Offer " << offerId << " outlived disconnected slave "
        << slave->id << " (" << slave->info.hostname() << ")";

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
        update->set_timestamp(Clock::now().secs());
        update->set_uuid(UUID::random().toBytes());

        LOG(INFO) << "Sending status update " << *update
                  << " for launch task attempt on invalid offer " << offerId;
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
    allocator->offersRevived(framework->id);
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

      // We add the task to 'killedTasks' here because the slave
      // might be partitioned or disconnected but the master
      // doesn't know it yet.
      slave->killedTasks.put(frameworkId, taskId);

      // NOTE: This task will be properly reconciled when the
      // disconnected slave re-registers with the master.
      if (!slave->disconnected) {
        LOG(INFO) << "Telling slave " << slave->id << " ("
                  << slave->info.hostname() << ")"
                  << " to kill task " << taskId
                  << " of framework " << frameworkId;

        KillTaskMessage message;
        message.mutable_framework_id()->MergeFrom(frameworkId);
        message.mutable_task_id()->MergeFrom(taskId);
        send(slave->pid, message);
      }
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
      update->set_timestamp(Clock::now().secs());
      update->set_uuid(UUID::random().toBytes());
      send(framework->pid, message);
    }
  } else {
    LOG(WARNING) << "Failed to kill task " << taskId
                 << " of framework " << frameworkId
                 << " because the framework cannot be found";
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
      if (!slave->disconnected) {
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
                     << " (" << slave->info.hostname() << ")"
                     << " because slave is disconnected";
        stats.invalidFrameworkMessages++;
      }
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
      if (slave->disconnected) {
        // The slave was previously disconnected but it is now trying
        // to register as a new slave. This could happen if the slave
        // failed recovery and hence registering as a new slave before
        // the master removed the old slave from its map.
        LOG(INFO) << "Removing old disconnected slave " << slave->id << " ("
                  << slave->info.hostname() << ") because a registration"
                  << " attempt is being made from " << from;
        removeSlave(slave);
        break;
      } else {
        LOG(INFO) << "Slave " << slave->id << " (" << slave->info.hostname()
                  << ") already registered, resending acknowledgement";
        SlaveRegisteredMessage message;
        message.mutable_slave_id()->MergeFrom(slave->id);
        reply(message);
        return;
      }
    }
  }

  Slave* slave = new Slave(slaveInfo, newSlaveId(), from, Clock::now());

  LOG(INFO) << "Attempting to register slave on " << slave->info.hostname()
            << " at " << slave->pid;

  // TODO(benh): We assume all slaves can register for now.
  CHECK(flags.slaves == "*");
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
    LOG(ERROR) << "Slave " << from << " re-registered without an id!";
    reply(ShutdownMessage());
  } else if (deactivatedSlaves.contains(from)) {
    // We disallow deactivated slaves from re-registering. This is
    // to ensure that when a master deactivates a slave that was
    // partitioned, we don't allow the slave to re-register, as we've
    // already informed frameworks that the tasks were lost.
    LOG(ERROR) << "Slave " << slaveId << " at " << from
               << " attempted to re-register after deactivation";
    reply(ShutdownMessage());
  } else {
    Slave* slave = getSlave(slaveId);
    if (slave != NULL) {
      slave->reregisteredTime = Clock::now();

      // NOTE: This handles the case where a slave tries to
      // re-register with an existing master (e.g. because of a
      // spurious Zookeeper session expiration or after the slave
      // recovers after a restart).
      // For now, we assume this slave is not nefarious (eventually
      // this will be handled by orthogonal security measures like key
      // based authentication).
      LOG(WARNING) << "Slave at " << from << " (" << slave->info.hostname()
                   << ") is being allowed to re-register with an already"
                   << " in use id (" << slaveId << ")";

      // If this is a disconnected slave, add it back to the allocator.
      if (slave->disconnected) {
        slave->disconnected = false; // Reset the flag.

        hashmap<FrameworkID, Resources> resources;
        foreach (const ExecutorInfo& executorInfo, executorInfos) {
          resources[executorInfo.framework_id()] += executorInfo.resources();
        }
        foreach (const Task& task, tasks) {
          // Ignore tasks that have reached terminal state.
          if (!protobuf::isTerminalState(task.state())) {
            resources[task.framework_id()] += task.resources();
          }
        }
        allocator->slaveAdded(slaveId, slaveInfo, resources);
      }

      SlaveReregisteredMessage message;
      message.mutable_slave_id()->MergeFrom(slave->id);
      reply(message);

      // Update the slave pid and relink to it.
      // NOTE: Re-linking the slave here always rather than only when
      // the slave is disconnected can lead to multiple exited events
      // in succession for a disconnected slave. As a result, we
      // ignore duplicate exited events for disconnected checkpointing
      // slaves.
      // See: https://issues.apache.org/jira/browse/MESOS-675
      slave->pid = from;
      link(slave->pid);

      // Reconcile tasks between master and the slave.
      // NOTE: This needs to be done after the registration message is
      // sent to the slave and the new pid is linked.
      reconcile(slave, executorInfos, tasks);
    } else {
      // NOTE: This handles the case when the slave tries to
      // re-register with a failed over master.
      slave = new Slave(slaveInfo, slaveId, from, Clock::now());
      slave->reregisteredTime = Clock::now();

      LOG(INFO) << "Attempting to re-register slave " << slave->id << " at "
                << slave->pid << " (" << slave->info.hostname() << ")";

      // TODO(benh): We assume all slaves can register for now.
      CHECK(flags.slaves == "*");
      readdSlave(slave, executorInfos, tasks);
    }

    // Send the latest framework pids to the slave.
    CHECK_NOTNULL(slave);
    hashset<UPID> pids;
    foreach (const Task& task, tasks) {
      Framework* framework = getFramework(task.framework_id());
      if (framework != NULL && !pids.contains(framework->pid)) {
        UpdateFrameworkMessage message;
        message.mutable_framework_id()->MergeFrom(framework->id);
        message.set_pid(framework->pid);
        send(slave->pid, message);

        pids.insert(framework->pid);
      }
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

  // NOTE: We cannot use 'from' here to identify the slave as this is
  // now sent by the StatusUpdateManagerProcess. Only 'pid' can
  // be used to identify the slave.
  LOG(INFO) << "Status update " << update << " from " << pid;

  Slave* slave = getSlave(update.slave_id());
  if (slave == NULL) {
    if (deactivatedSlaves.contains(pid)) {
      // If the slave is deactivated, we have already informed
      // frameworks that its tasks were LOST, so the slave should
      // shut down.
      LOG(WARNING) << "Ignoring status update " << update
                   << " from deactivated slave " << pid
                   << " with id " << update.slave_id() << " ; asking slave "
                   << " to shutdown";
      send(pid, ShutdownMessage());
    } else {
      LOG(WARNING) << "Ignoring status update " << update
                   << " from unknown slave " << pid
                   << " with id " << update.slave_id();
    }
    stats.invalidStatusUpdates++;
    return;
  }

  CHECK(!deactivatedSlaves.contains(pid));

  Framework* framework = getFramework(update.framework_id());
  if (framework == NULL) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " from " << pid << " ("
                 << slave->info.hostname() << "): error, couldn't lookup "
                 << "framework " << update.framework_id();
    stats.invalidStatusUpdates++;
    return;
  }

  // Pass on the (transformed) status update to the framework.
  StatusUpdateMessage message;
  message.mutable_update()->MergeFrom(update);
  message.set_pid(pid);
  send(framework->pid, message);

  // Lookup the task and see if we need to update anything locally.
  Task* task = slave->getTask(update.framework_id(), status.task_id());
  if (task == NULL) {
    LOG(WARNING) << "Status update " << update
                 << " from " << pid << " ("
                 << slave->info.hostname() << "): error, couldn't lookup "
                 << "task " << status.task_id();
    stats.invalidStatusUpdates++;
    return;
  }

  task->set_state(status.state());

  // Handle the task appropriately if it's terminated.
  if (protobuf::isTerminalState(status.state())) {
    removeTask(task);
  }

  stats.tasks[status.state()]++;
  stats.validStatusUpdates++;
}


void Master::exitedExecutor(
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    int32_t status)
{
  // Only update master's internal data structures here for properly accounting.
  // The TASK_LOST updates are handled by the slave.
  Slave* slave = getSlave(slaveId);
  if (slave == NULL) {
    if (deactivatedSlaves.contains(from)) {
      // If the slave is deactivated, we have already informed
      // frameworks that its tasks were LOST, so the slave should
      // shut down.
      LOG(WARNING) << "Ignoring exited executor '" << executorId
                   << "' of framework " << frameworkId
                   << " on deactivated slave " << slaveId
                   << " ; asking slave to shutdown";
      reply(ShutdownMessage());
    } else {
      LOG(WARNING) << "Ignoring exited executor '" << executorId
                   << "' of framework " << frameworkId
                   << " on unknown slave " << slaveId;
    }
    return;
  }

  CHECK(!deactivatedSlaves.contains(from));

  // Tell the allocator about the recovered resources.
  if (slave->hasExecutor(frameworkId, executorId)) {
    ExecutorInfo executor = slave->executors[frameworkId][executorId];

    LOG(INFO) << "Executor " << executorId
              << " of framework " << frameworkId
              << " on slave " << slaveId
              << " (" << slave->info.hostname() << ")"
              << " exited with status " << status;

    allocator->resourcesRecovered(frameworkId,
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

    // TODO(benh): Send the framework its executor's exit status?
    // Or maybe at least have something like
    // Scheduler::executorLost?
  }
}


void Master::deactivateSlave(const SlaveID& slaveId)
{
  if (!slaves.contains(slaveId)) {
    // Possible when the SlaveObserver dispatched to deactivate a slave,
    // but exited() was already called for this slave.
    LOG(WARNING) << "Unable to deactivate unknown slave " << slaveId;
    return;
  }

  Slave* slave = slaves[slaveId];
  CHECK_NOTNULL(slave);

  LOG(WARNING) << "Removing slave " << slave->id << " at " << slave->pid
               << " because it has been deactivated";

  send(slave->pid, ShutdownMessage());
  removeSlave(slave);
}


void Master::frameworkFailoverTimeout(const FrameworkID& frameworkId,
                                      const Time& reregisteredTime)
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
      allocator->resourcesRecovered(frameworkId, slaveId, offered);
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

      allocator->resourcesRecovered(frameworkId, slaveId, offered);
      continue;
    }

    Slave* slave = slaves[slaveId];

    CHECK(slave->info.checkpoint() || !framework->info.checkpoint())
        << "Resources of non checkpointing slave " << slaveId
        << " (" << slave->info.hostname() << ") are being offered to"
        << " checkpointing framework " << frameworkId;

    // This could happen if the allocator dispatched 'Master::offer' before
    // it received 'Allocator::slaveRemoved' from the master.
    if (slave->disconnected) {
      LOG(WARNING) << "Master returning resources offered because slave "
                   << slaveId << " is disconnected";

      allocator->resourcesRecovered(frameworkId, slaveId, offered);
      continue;
    }

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
      return TaskInfoError::some(
          "Task " + stringify(task.task_id()) + " attempted to use " +
          stringify(taskResources) + " combined with already used " +
          stringify(usedResources) + " is greater than offered " +
          stringify(offer->resources()));
    }

    // Check this task's executor's resources.
    if (task.has_executor()) {
      // TODO(benh): Check that the executor uses some resources.

      foreach (const Resource& resource, task.executor().resources()) {
        if (!Resources::isAllocatable(resource)) {
          // TODO(benh): Send back the invalid resources?
          return TaskInfoError::some(
              "Executor for task " + stringify(task.task_id()) +
              " uses invalid resources " + stringify(resource));
        }
      }

      // Check if this task's executor is running, and if not check if
      // the task + the executor use more resources than offered.
      if (!executors.contains(task.executor().executor_id())) {
        if (!slave->hasExecutor(framework->id, task.executor().executor_id())) {
          taskResources += task.executor().resources();
          if (!((usedResources + taskResources) <= offer->resources())) {
            return TaskInfoError::some(
                "Task " + stringify(task.task_id()) + " + executor attempted" +
                " to use " + stringify(taskResources) + " combined with" +
                " already used " + stringify(usedResources) + " is greater" +
                " than offered " + stringify(offer->resources()));
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
    if (task.has_executor() == task.has_command()) {
      return TaskInfoError::some(
          "Task should have at least one (but not both) of CommandInfo or"
          " ExecutorInfo present");
    }

    if (task.has_executor()) {
      if (slave->hasExecutor(framework->id, task.executor().executor_id())) {
        const ExecutorInfo& executorInfo =
          slave->executors[framework->id][task.executor().executor_id()];
        if (!(task.executor() == executorInfo)) {
          return TaskInfoError::some(
              "Task has invalid ExecutorInfo (existing ExecutorInfo"
              " with same ExecutorID is not compatible).\n"
              "------------------------------------------------------------\n"
              "Existing ExecutorInfo:\n" +
              stringify(executorInfo) + "\n"
              "------------------------------------------------------------\n"
              "Task's ExecutorInfo:\n" +
              stringify(task.executor()) + "\n"
              "------------------------------------------------------------\n");
        }
      }
    }

    return TaskInfoError::none();
  }
};


// Checks that a task that asks for checkpointing is not being
// launched on a slave that has not enabled checkpointing.
// TODO(vinod): Consider not offering resources for non-checkpointing
// slaves to frameworks that need checkpointing.
struct CheckpointChecker : TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      Offer* offer,
      Framework* framework,
      Slave* slave)
  {
    if (framework->info.checkpoint() && !slave->info.checkpoint()) {
      return TaskInfoError::some(
          "Task asked to be checkpointed but the slave "
          "has checkpointing disabled");
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
  CHECK_NOTNULL(offer);
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Processing reply for offer " << offer->id()
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
  visitors.push_back(new CheckpointChecker());

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
      LOG(WARNING) << "Failed to validate task " << task.task_id()
                   << " : " << error.get();

      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id,
          slave->id,
          task.task_id(),
          TASK_LOST,
          error.get());

      LOG(INFO) << "Sending status update " << update << " for invalid task";
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update);
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
    allocator->resourcesUnused(offer->framework_id(),
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
            << " of framework " << framework->id
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


// NOTE: This function is only called when the slave re-registers
// with a master that already knows about it (i.e., not a failed
// over master).
void Master::reconcile(
    Slave* slave,
    const vector<ExecutorInfo>& executors,
    const vector<Task>& tasks)
{
  CHECK_NOTNULL(slave);

  // We convert the 'tasks' into a map for easier lookup below.
  // TODO(vinod): Check if the tasks are known to the master.
  multihashmap<FrameworkID, TaskID> slaveTasks;
  foreach (const Task& task, tasks) {
    slaveTasks.put(task.framework_id(), task.task_id());
  }

  // Send TASK_LOST updates for tasks present in the master but
  // missing from the slave. This could happen if the task was
  // dropped by the slave (e.g., slave exited before getting the
  // task or the task was launched while slave was in recovery).
  foreachvalue (Task* task, utils::copy(slave->tasks)) {
    if (!slaveTasks.contains(task->framework_id(), task->task_id())) {
      LOG(WARNING) << "Sending TASK_LOST for task " << task->task_id()
                   << " of framework " << task->framework_id()
                   << " unknown to the slave " << slave->id
                   << " (" << slave->info.hostname() << ")";

      const StatusUpdate& update = protobuf::createStatusUpdate(
          task->framework_id(),
          slave->id,
          task->task_id(),
          TASK_LOST,
          "Task is unknown to the slave");

      statusUpdate(update, UPID());
    }
  }

  // Likewise, any executors that are present in the master but
  // not present in the slave must be removed to correctly account
  // for resources. First we index the executors for fast lookup below.
  multihashmap<FrameworkID, ExecutorID> slaveExecutors;
  foreach (const ExecutorInfo& executor, executors) {
    // TODO(bmahler): The slave ensures the framework id is set in the
    // framework info when re-registering. This can be killed in 0.15.0
    // as we've added code in 0.14.0 to ensure the framework id is set
    // in the scheduler driver.
    if (!executor.has_framework_id()) {
      LOG(ERROR) << "Slave " << slave->id
                 << " (" << slave->info.hostname() << ") "
                 << "re-registered with executor " << executor.executor_id()
                 << " without setting the framework id";
      continue;
    }
    slaveExecutors.put(executor.framework_id(), executor.executor_id());
  }

  // Now that we have the index for lookup, remove all the executors
  // in the master that are not known to the slave.
  foreachkey (const FrameworkID& frameworkId, utils::copy(slave->executors)) {
    foreachkey (const ExecutorID& executorId,
                utils::copy(slave->executors[frameworkId])) {
      if (!slaveExecutors.contains(frameworkId, executorId)) {
        LOG(WARNING) << "Removing executor " << executorId << " of framework "
                     << frameworkId << " as it is unknown to the slave "
                     << slave->id << " (" << slave->info.hostname() << ")";

        // TODO(bmahler): This is duplicated in several locations, we
        // may benefit from a method for removing an executor from
        // all the relevant data structures and the allocator, akin
        // to removeTask().
        allocator->resourcesRecovered(
            frameworkId,
            slave->id,
            slave->executors[frameworkId][executorId].resources());

        slave->removeExecutor(frameworkId, executorId);

        if (frameworks.contains(frameworkId)) {
          frameworks[frameworkId]->removeExecutor(slave->id, executorId);
        }
      }
    }
  }

  // Send KillTaskMessages for tasks in 'killedTasks' that are
  // still alive on the slave. This could happen if the slave
  // did not receive KillTaskMessage because of a partition or
  // disconnection.
  foreach (const Task& task, tasks) {
    if (!protobuf::isTerminalState(task.state()) &&
        slave->killedTasks.contains(task.framework_id(), task.task_id())) {
      LOG(WARNING) << " Slave " << slave->id << " (" << slave->info.hostname()
                   << ") has non-terminal task " << task.task_id()
                   << " that is supposed to be killed. Killing it now!";

      KillTaskMessage message;
      message.mutable_framework_id()->MergeFrom(task.framework_id());
      message.mutable_task_id()->MergeFrom(task.task_id());
      send(slave->pid, message);
    }
  }

  // Send ShutdownFrameworkMessages for frameworks that are completed.
  // This could happen if the message wasn't received by the slave
  // (e.g., slave was down, partitioned).
  // NOTE: This is a short-term hack because this information is lost
  // when the master fails over. Also, 'completedFrameworks' has a
  // limited capacity.
  // TODO(vinod): Revisit this when registrar is in place. It would
  // likely involve storing this information in the registrar.
  foreach (const shared_ptr<Framework>& framework, completedFrameworks) {
    if (slaveTasks.contains(framework->id)) {
      LOG(WARNING)
        << "Slave " << slave->id << " (" << slave->info.hostname()
        << ") re-registered with completed framework " << framework->id
        << ". Shutting down the framework on the slave";

      ShutdownFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      send(slave->pid, message);
    }
  }
}


void Master::addFramework(Framework* framework)
{
  CHECK(frameworks.count(framework->id) == 0);

  frameworks[framework->id] = framework;

  link(framework->pid);

  // Enforced by Master::registerFramework.
  CHECK(roles.contains(framework->info.role()));
  roles[framework->info.role()]->addFramework(framework);

  FrameworkRegisteredMessage message;
  message.mutable_framework_id()->MergeFrom(framework->id);
  message.mutable_master_info()->MergeFrom(info);
  send(framework->pid, message);

  allocator->frameworkAdded(
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
    allocator->frameworkActivated(framework->id, framework->info);
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
    allocator->resourcesRecovered(offer->framework_id(),
                                  offer->slave_id(),
                                  Resources(offer->resources()));
    removeOffer(offer);
  }
}


void Master::removeFramework(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Removing framework " << framework->id;

  if (framework->active) {
    // Tell the allocator to stop allocating resources to this framework.
    allocator->frameworkDeactivated(framework->id);
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
    // Since we only find out about tasks when the slave re-registers,
    // it must be the case that the slave exists!
    CHECK(slave != NULL);
    removeTask(task);
  }

  // Remove the framework's offers (if they weren't removed before).
  foreach (Offer* offer, utils::copy(framework->offers)) {
    allocator->resourcesRecovered(offer->framework_id(),
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
        allocator->resourcesRecovered(framework->id,
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

  // The completedFramework buffer now owns the framework pointer.
  completedFrameworks.push_back(std::tr1::shared_ptr<Framework>(framework));

  CHECK(roles.contains(framework->info.role()));
  roles[framework->info.role()]->removeFramework(framework);

  // Remove it.
  frameworks.erase(framework->id);
  allocator->frameworkRemoved(framework->id);
}


void Master::removeFramework(Slave* slave, Framework* framework)
{
  CHECK_NOTNULL(slave);
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Removing framework " << framework->id
            << " from slave " << slave->id
            << " (" << slave->info.hostname() << ")";

  // Remove pointers to framework's tasks in slaves, and send status updates.
  foreachvalue (Task* task, utils::copy(slave->tasks)) {
    // Remove tasks that belong to this framework.
    if (task->framework_id() == framework->id) {
      // A framework might not actually exist because the master failed
      // over and the framework hasn't reconnected yet. For more info
      // please see the comments in 'removeFramework(Framework*)'.
      const StatusUpdate& update = protobuf::createStatusUpdate(
        task->framework_id(),
        task->slave_id(),
        task->task_id(),
        TASK_LOST,
        "Slave " + slave->info.hostname() + " disconnected",
        (task->has_executor_id()
            ? Option<ExecutorID>(task->executor_id()) : None()));

      statusUpdate(update, UPID());
    }
  }

  // Remove the framework's executors from the slave and framework
  // for proper resource accounting.
  if (slave->executors.contains(framework->id)) {
    foreachkey (const ExecutorID& executorId,
                utils::copy(slave->executors[framework->id])) {

      allocator->resourcesRecovered(
          framework->id,
          slave->id,
          slave->executors[framework->id][executorId].resources());

      framework->removeExecutor(slave->id, executorId);
      slave->removeExecutor(framework->id, executorId);
    }
  }
}


void Master::addSlave(Slave* slave, bool reregister)
{
  CHECK(slave != NULL);

  LOG(INFO) << "Adding slave " << slave->id
            << " at " << slave->info.hostname()
            << " with " << slave->info.resources();

  deactivatedSlaves.erase(slave->pid);
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
    allocator->slaveAdded(slave->id,
                          slave->info,
                          hashmap<FrameworkID, Resources>());
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
    // TODO(bmahler): ExecutorInfo.framework_id is set by the Scheduler
    // Driver in 0.14.0. Therefore, in 0.15.0, the slave no longer needs
    // to set it, and we could remove this CHECK if desired.
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
    // Ignore tasks that have reached terminal state.
    if (protobuf::isTerminalState(task.state())) {
      continue;
    }

    Task* t = new Task(task);

    // Add the task to the slave.
    slave->addTask(t);

    // Try and add the task to the framework too, but since the
    // framework might not yet be connected we won't be able to
    // add them. However, when the framework connects later we
    // will add them then. Again, we do the same thing
    // if a framework currently isn't registered.
    Framework* framework = getFramework(task.framework_id());
    if (framework != NULL) {
      framework->addTask(t);
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

  allocator->slaveAdded(slave->id, slave->info, resources);
}


// Lose all of a slave's tasks and delete the slave object.
void Master::removeSlave(Slave* slave)
{
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Removing slave " << slave->id
            << " (" << slave->info.hostname() << ")";

  // We do this first, to make sure any of the resources recovered
  // below (e.g., removeTask()) are ignored by the allocator.
  if (!slave->disconnected) {
    allocator->slaveRemoved(slave->id);
  }

  // Remove pointers to slave's tasks in frameworks, and send status updates
  foreachvalue (Task* task, utils::copy(slave->tasks)) {
    // A framework might not actually exist because the master failed
    // over and the framework hasn't reconnected. This can be a tricky
    // situation for frameworks that want to have high-availability,
    // because if they eventually do connect they won't ever get a
    // status update about this task.  Perhaps in the future what we
    // want to do is create a local Framework object to represent that
    // framework until it fails over. See the TODO above in
    // Master::reregisterSlave.
    const StatusUpdate& update = protobuf::createStatusUpdate(
        task->framework_id(),
        task->slave_id(),
        task->task_id(),
        TASK_LOST,
        "Slave " + slave->info.hostname() + " removed",
        (task->has_executor_id() ?
            Option<ExecutorID>(task->executor_id()) : None()));

    statusUpdate(update, UPID());
  }

  foreach (Offer* offer, utils::copy(slave->offers)) {
    // TODO(vinod): We don't need to call 'Allocator::resourcesRecovered'
    // once MESOS-621 is fixed.
    allocator->resourcesRecovered(
        offer->framework_id(), slave->id, offer->resources());

    // Remove and rescind offers.
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

  // Mark the slave as deactivated.
  deactivatedSlaves.insert(slave->pid);
  slaves.erase(slave->id);
  delete slave;
}


void Master::removeTask(Task* task)
{
  CHECK_NOTNULL(task);

  // Remove from framework.
  Framework* framework = getFramework(task->framework_id());
  if (framework != NULL) { // A framework might not be re-connected yet.
    framework->removeTask(task);
  }

  // Remove from slave.
  Slave* slave = getSlave(task->slave_id());
  CHECK_NOTNULL(slave);
  slave->removeTask(task);

  // Tell the allocator about the recovered resources.
  allocator->resourcesRecovered(
      task->framework_id(), task->slave_id(), Resources(task->resources()));

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
  return frameworks.contains(frameworkId) ? frameworks[frameworkId] : NULL;
}


Slave* Master::getSlave(const SlaveID& slaveId)
{
  return slaves.contains(slaveId) ? slaves[slaveId] : NULL;
}


Offer* Master::getOffer(const OfferID& offerId)
{
  return offers.contains(offerId) ? offers[offerId] : NULL;
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
