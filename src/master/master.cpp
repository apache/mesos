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

#include <stdint.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <list>
#include <sstream>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/run.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/check.hpp>
#include <stout/lambda.hpp>
#include <stout/memory.hpp>
#include <stout/multihashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "authorizer/authorizer.hpp"

#include "sasl/authenticator.hpp"

#include "common/build.hpp"
#include "common/date_utils.hpp"
#include "common/protobuf_utils.hpp"

#include "credentials/credentials.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

using std::list;
using std::string;
using std::vector;

using process::wait; // Necessary on some OS's to disambiguate.
using process::Clock;
using process::Failure;
using process::Future;
using process::MessageEvent;
using process::Owned;
using process::PID;
using process::Process;
using process::Promise;
using process::Time;
using process::Timer;
using process::UPID;

using memory::shared_ptr;

namespace mesos {
namespace internal {
namespace master {

using allocator::Allocator;


class WhitelistWatcher : public Process<WhitelistWatcher> {
public:
  WhitelistWatcher(const string& _path, Allocator* _allocator)
  : ProcessBase(process::ID::generate("whitelist")),
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
      VLOG(1) << "No whitelist given. Advertising offers for all slaves";
    } else {
      // Read from local file.
      // TODO(vinod): Add support for reading from ZooKeeper.
      // TODO(vinod): Ensure this read is atomic w.r.t external
      // writes/updates to this file.
      Try<string> read = os::read(
          strings::remove(path, "file://", strings::PREFIX));
      if (read.isError()) {
        LOG(ERROR) << "Error reading whitelist file: " << read.error() << ". "
                   << "Retrying";
        whitelist = lastWhitelist;
      } else if (read.get().empty()) {
        LOG(WARNING) << "Empty whitelist file " << path << ". "
                     << "No offers will be made!";
        whitelist = hashset<string>();
      } else {
        hashset<string> hostnames;
        vector<string> lines = strings::tokenize(read.get(), "\n");
        foreach (const string& hostname, lines) {
          hostnames.insert(hostname);
        }
        whitelist = hostnames;
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
    : ProcessBase(process::ID::generate("slave-observer")),
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
        shutdown();
        return;
      }
    }

    send(slave, "PING");
    pinged = true;
    delay(SLAVE_PING_TIMEOUT, self(), &SlaveObserver::timeout);
  }

  void shutdown()
  {
    dispatch(master, &Master::shutdownSlave, slaveId, "health check timed out");
  }

private:
  const UPID slave;
  const SlaveInfo slaveInfo;
  const SlaveID slaveId;
  const PID<Master> master;
  uint32_t timeouts;
  bool pinged;
};


Master::Master(
    Allocator* _allocator,
    Registrar* _registrar,
    Repairer* _repairer,
    Files* _files,
    MasterContender* _contender,
    MasterDetector* _detector,
    const Flags& _flags)
  : ProcessBase("master"),
    http(*this),
    flags(_flags),
    allocator(_allocator),
    registrar(_registrar),
    repairer(_repairer),
    files(_files),
    contender(_contender),
    detector(_detector),
    metrics(*this),
    electedTime(None())
{
  // NOTE: We populate 'info_' here instead of inside 'initialize()'
  // because 'StandaloneMasterDetector' needs access to the info.

  // The master ID is currently comprised of the current date, the IP
  // address and port from self() and the OS PID.
  Try<string> id =
    strings::format("%s-%u-%u-%d", DateUtils::currentDate(),
                    self().ip, self().port, getpid());

  CHECK(!id.isError()) << id.error();

  info_.set_id(id.get());
  info_.set_ip(self().ip);
  info_.set_port(self().port);
  info_.set_pid(self());

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

  info_.set_hostname(hostname);
}


Master::~Master() {}


void Master::initialize()
{
  LOG(INFO) << "Master " << info_.id() << " (" << info_.hostname() << ")"
            << " started on " << string(self()).substr(7);

  // NOTE: We enforce a minimum slave re-register timeout because the
  // slave bounds its (re-)registration retries based on the minimum.
  if (flags.slave_reregister_timeout < MIN_SLAVE_REREGISTER_TIMEOUT) {
    EXIT(1) << "Invalid value '" << flags.slave_reregister_timeout << "' "
            << "for --slave_reregister_timeout: "
            << "Must be at least " << MIN_SLAVE_REREGISTER_TIMEOUT;
  }

  // Parse the percentage for the slave removal limit.
  // TODO(bmahler): Add a 'Percentage' abstraction.
  if (!strings::endsWith(flags.recovery_slave_removal_limit, "%")) {
    EXIT(1) << "Invalid value '" << flags.recovery_slave_removal_limit << "' "
            << "for --recovery_slave_removal_percent_limit: " << "missing '%'";
  }

  Try<double> limit = numify<double>(
      strings::remove(
          flags.recovery_slave_removal_limit,
          "%",
          strings::SUFFIX));

  if (limit.isError()) {
    EXIT(1) << "Invalid value '" << flags.recovery_slave_removal_limit << "' "
            << "for --recovery_slave_removal_percent_limit: " << limit.error();
  }

  if (limit.get() < 0.0 || limit.get() > 100.0) {
    EXIT(1) << "Invalid value '" << flags.recovery_slave_removal_limit << "' "
            << "for --recovery_slave_removal_percent_limit: "
            << "Must be within [0%-100%]";
  }

  // Log authentication state.
  if (flags.authenticate_frameworks) {
    LOG(INFO) << "Master only allowing authenticated frameworks to register";
  } else {
    LOG(INFO) << "Master allowing unauthenticated frameworks to register";
  }
  if (flags.authenticate_slaves) {
    LOG(INFO) << "Master only allowing authenticated slaves to register";
  } else {
    LOG(INFO) << "Master allowing unauthenticated slaves to register";
  }

  // Load credentials.
  if (flags.credentials.isSome()) {
    const string& path =
      strings::remove(flags.credentials.get(), "file://", strings::PREFIX);

    Result<vector<Credential> > credentials = credentials::read(path);
    if (credentials.isError()) {
      EXIT(1) << credentials.error() << " (see --credentials flag)";
    } else if (credentials.isNone()) {
      EXIT(1) << "Credentials file must contain at least one credential"
              << " (see --credentials flag)";
    }

    // Give Authenticator access to credentials.
    sasl::secrets::load(credentials.get());
  } else if (flags.authenticate_frameworks || flags.authenticate_slaves) {
    EXIT(1) << "Authentication requires a credentials file"
            << " (see --credentials flag)";
  }

  if (flags.acls.isSome()) {
    LOG(INFO) << "Master enabling authorization";
    Try<Owned<Authorizer> > authorizer_ = Authorizer::create(flags.acls.get());
    if (authorizer_.isError()) {
      EXIT(1) << "Failed to initialize the Authorizer: "
              << authorizer_.error() << " (see --acls flag)";
    }
    authorizer = authorizer_.get();
  }

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
      } else if (!roleInfos.contains(pair[0])) {
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
      &LaunchTasksMessage::filters,
      &LaunchTasksMessage::offer_ids);

  install<ReviveOffersMessage>(
      &Master::reviveOffers,
      &ReviveOffersMessage::framework_id);

  install<KillTaskMessage>(
      &Master::killTask,
      &KillTaskMessage::framework_id,
      &KillTaskMessage::task_id);

  install<StatusUpdateAcknowledgementMessage>(
      &Master::statusUpdateAcknowledgement,
      &StatusUpdateAcknowledgementMessage::slave_id,
      &StatusUpdateAcknowledgementMessage::framework_id,
      &StatusUpdateAcknowledgementMessage::task_id,
      &StatusUpdateAcknowledgementMessage::uuid);

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
      &ReregisterSlaveMessage::tasks,
      &ReregisterSlaveMessage::completed_frameworks);

  install<UnregisterSlaveMessage>(
      &Master::unregisterSlave,
      &UnregisterSlaveMessage::slave_id);

  install<StatusUpdateMessage>(
      &Master::statusUpdate,
      &StatusUpdateMessage::update,
      &StatusUpdateMessage::pid);

  install<ReconcileTasksMessage>(
      &Master::reconcileTasks,
      &ReconcileTasksMessage::framework_id,
      &ReconcileTasksMessage::statuses);

  install<ExitedExecutorMessage>(
      &Master::exitedExecutor,
      &ExitedExecutorMessage::slave_id,
      &ExitedExecutorMessage::framework_id,
      &ExitedExecutorMessage::executor_id,
      &ExitedExecutorMessage::status);

  install<AuthenticateMessage>(
      &Master::authenticate,
      &AuthenticateMessage::pid);

  // Setup HTTP routes.
  route("/health",
        Http::HEALTH_HELP,
        lambda::bind(&Http::health, http, lambda::_1));
  route("/observe",
        Http::OBSERVE_HELP,
        lambda::bind(&Http::observe, http, lambda::_1));
  route("/redirect",
        Http::REDIRECT_HELP,
        lambda::bind(&Http::redirect, http, lambda::_1));
  route("/roles.json",
        None(),
        lambda::bind(&Http::roles, http, lambda::_1));
  route("/state.json",
        None(),
        lambda::bind(&Http::state, http, lambda::_1));
  route("/stats.json",
        None(),
        lambda::bind(&Http::stats, http, lambda::_1));
  route("/tasks.json",
        Http::TASKS_HELP,
        lambda::bind(&Http::tasks, http, lambda::_1));

  // Provide HTTP assets from a "webui" directory. This is either
  // specified via flags (which is necessary for running out of the
  // build directory before 'make install') or determined at build
  // time via the preprocessor macro '-DMESOS_WEBUI_DIR' set in the
  // Makefile.
  provide("", path::join(flags.webui_dir, "master/static/index.html"));
  provide("static", path::join(flags.webui_dir, "master/static"));

  if (flags.log_dir.isSome()) {
    Try<string> log = logging::getLogFile(
        logging::getLogSeverity(flags.logging_level));

    if (log.isError()) {
      LOG(ERROR) << "Master log file cannot be found: " << log.error();
    } else {
      files->attach(log.get(), "/master/log")
        .onAny(defer(self(), &Self::fileAttached, lambda::_1, log.get()));
    }
  }

  contender->initialize(info_);

  // Start contending to be a leading master and detecting the current
  // leader.
  contender->contend()
    .onAny(defer(self(), &Master::contended, lambda::_1));
  detector->detect()
    .onAny(defer(self(), &Master::detected, lambda::_1));
}


void Master::finalize()
{
  LOG(INFO) << "Master terminating";

  // Remove the frameworks.
  // Note we are not deleting the pointers to the frameworks from the
  // allocator or the roles because it is unnecessary bookkeeping at
  // this point since we are shutting down.
  foreachvalue (Framework* framework, frameworks.activated) {
    // Remove pointers to the framework's tasks in slaves.
    foreachvalue (Task* task, utils::copy(framework->tasks)) {
      Slave* slave = getSlave(task->slave_id());
      // Since we only find out about tasks when the slave re-registers,
      // it must be the case that the slave exists!
      CHECK(slave != NULL)
        << "Unknown slave " << task->slave_id()
        << " in the task " << task->task_id();

      removeTask(task);
    }

    // Remove the framework's offers (if they weren't removed before).
    foreach (Offer* offer, utils::copy(framework->offers)) {
      removeOffer(offer);
    }

    delete framework;
  }
  frameworks.activated.clear();

  CHECK_EQ(offers.size(), 0UL);

  foreachvalue (Slave* slave, slaves.activated) {
    // Remove tasks that are in the slave but not in any framework.
    // This could happen when the framework has yet to re-register
    // after master failover.
    // NOTE: keys() and values() are used because slave->tasks is
    //       modified by removeTask()!
    foreach (const FrameworkID& frameworkId, slave->tasks.keys()) {
      foreach (Task* task, slave->tasks[frameworkId].values()) {
        removeTask(task);
      }
    }

    // Kill the slave observer.
    terminate(slave->observer);
    wait(slave->observer);

    delete slave->observer;
    delete slave;
  }
  slaves.activated.clear();

  foreachvalue (Future<Nothing> future, authenticating) {
    // NOTE: This is necessary during tests because a copy of
    // this future is used to setup authentication timeout. If a
    // test doesn't discard this future, authentication timeout might
    // fire in a different test and any associated callbacks
    // (e.g., '_authenticate()') would be called. This is because the
    // master pid doesn't change across the tests.
    // TODO(vinod): This seems to be a bug in libprocess or the
    // testing infrastructure.
    future.discard();
  }

  foreachvalue (Role* role, roles) {
    delete role;
  }
  roles.clear();

  // NOTE: This is necessary during tests because we don't want the
  // timer to fire in a different test and invoke the callback.
  // The callback would be invoked because the master pid doesn't
  // change across the tests.
  // TODO(vinod): This seems to be a bug in libprocess or the
  // testing infrastructure.
  if (slaves.recoveredTimer.isSome()) {
    Timer::cancel(slaves.recoveredTimer.get());
  }

  terminate(whitelistWatcher);
  wait(whitelistWatcher);
  delete whitelistWatcher;
}


void Master::exited(const UPID& pid)
{
  foreachvalue (Framework* framework, frameworks.activated) {
    if (framework->pid == pid) {
      LOG(INFO) << "Framework " << framework->id << " disconnected";

      // Deactivate framework.
      deactivate(framework);

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
  foreachvalue (Slave* slave, slaves.activated) {
    if (slave->pid == pid) {
      LOG(INFO) << "Slave " << *slave << " disconnected";

      if (!slave->info.checkpoint()) {
        // Remove the slave, if it is not checkpointing.
        LOG(INFO) << "Removing disconnected slave " << *slave
                  << " because it is not checkpointing!";
        removeSlave(slave);
        return;
      } else if (!slave->disconnected) {
        // Checkpointing slaves can just be disconnected.
        disconnect(slave);

        // Remove all non-checkpointing frameworks.
        hashset<FrameworkID> frameworkIds =
          slave->tasks.keys() | slave->executors.keys();

        foreach (const FrameworkID& frameworkId, frameworkIds) {
          Framework* framework = getFramework(frameworkId);
          if (framework != NULL && !framework->info.checkpoint()) {
            LOG(INFO) << "Removing framework " << frameworkId
                      << " from disconnected slave " << *slave
                      << " because the framework is not checkpointing";

            removeFramework(slave, framework);
          }
        }
      } else {
        LOG(WARNING) << "Ignoring duplicate exited() notification for "
                     << "checkpointing slave " << *slave;
      }
    }
  }
}


void Master::visit(const MessageEvent& event)
{
  // All messages are filtered when non-leading.
  if (!elected()) {
    LOG(WARNING) << "Dropping '" << event.message->name << "' message since "
                 << "not elected yet";
    ++metrics.dropped_messages;
    return;
  }

  CHECK_SOME(recovered);

  // All messages are filtered while recovering.
  // TODO(bmahler): Consider instead re-enqueing *all* messages
  // through recover(). What are the performance implications of
  // the additional queueing delay and the accumulated backlog
  // of messages post-recovery?
  if (!recovered.get().isReady()) {
    LOG(WARNING) << "Dropping '" << event.message->name << "' message since "
                 << "not recovered yet";
    ++metrics.dropped_messages;
    return;
  }

  ProtobufProcess<Master>::visit(event);
}


void fail(const string& message, const string& failure)
{
  LOG(FATAL) << message << ": " << failure;
}


Future<Nothing> Master::recover()
{
  if (!elected()) {
    return Failure("Not elected as leading master");
  }

  if (recovered.isNone()) {
    LOG(INFO) << "Recovering from registrar";

    recovered = registrar->recover(info_)
      .then(defer(self(), &Self::_recover, lambda::_1));
  }

  return recovered.get();
}


Future<Nothing> Master::_recover(const Registry& registry)
{
  foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
    slaves.recovered.insert(slave.info().id());
  }

  // Set up a timeout for slaves to re-register. This timeout is based
  // on the maximum amount of time the SlaveObserver allows slaves to
  // not respond to health checks.
  // TODO(bmahler): Consider making this configurable.
  slaves.recoveredTimer =
    delay(flags.slave_reregister_timeout,
          self(),
          &Self::recoveredSlavesTimeout,
          registry);

  // Recovery is now complete!
  LOG(INFO) << "Recovered " << registry.slaves().slaves().size() << " slaves"
            << " from the Registry (" << Bytes(registry.ByteSize()) << ")"
            << " ; allowing " << flags.slave_reregister_timeout
            << " for slaves to re-register";

  return Nothing();
}


void Master::recoveredSlavesTimeout(const Registry& registry)
{
  CHECK(elected());

  // TODO(bmahler): Add a 'Percentage' abstraction.
  Try<double> limit_ = numify<double>(
      strings::remove(
          flags.recovery_slave_removal_limit,
          "%",
          strings::SUFFIX));

  CHECK_SOME(limit_);

  double limit = limit_.get() / 100.0;

  // Compute the percentage of slaves to be removed, if it exceeds the
  // safety-net limit, bail!
  double removalPercentage =
    (1.0 * slaves.recovered.size()) /
    (1.0 * registry.slaves().slaves().size());

  if (removalPercentage > limit) {
    EXIT(1) << "Post-recovery slave removal limit exceeded! After "
            << SLAVE_PING_TIMEOUT * MAX_SLAVE_PING_TIMEOUTS
            << " there were " << slaves.recovered.size()
            << " (" << removalPercentage * 100 << "%) slaves recovered from the"
            << " registry that did not re-register: \n"
            << stringify(slaves.recovered) << "\n "
            << " The configured removal limit is " << limit * 100 << "%. Please"
            << " investigate or increase this limit to proceed further";
  }

  foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
    if (!slaves.recovered.contains(slave.info().id())) {
      continue; // Slave re-registered.
    }

    LOG(WARNING) << "Slave " << slave.info().id()
                 << " (" << slave.info().hostname() << ") did not re-register "
                 << "within the timeout; removing it from the registrar";

    ++metrics.recovery_slave_removals;

    slaves.recovered.erase(slave.info().id());

    if (flags.registry_strict) {
      slaves.removing.insert(slave.info().id());

      registrar->apply(Owned<Operation>(new RemoveSlave(slave.info())))
        .onAny(defer(self(),
                     &Self::_removeSlave,
                     slave.info(),
                     vector<StatusUpdate>(), // No TASK_LOST updates to send.
                     lambda::_1));
    } else {
      // When a non-strict registry is in use, we want to ensure the
      // registry is used in a write-only manner. Therefore we remove
      // the slave from the registry but we do not inform the
      // framework.
      const string& message =
        "Failed to remove slave " + stringify(slave.info().id());

      registrar->apply(Owned<Operation>(new RemoveSlave(slave.info())))
        .onFailed(lambda::bind(fail, message, lambda::_1));
    }
  }
}


void Master::fileAttached(const Future<Nothing>& result, const string& path)
{
  if (result.isReady()) {
    LOG(INFO) << "Successfully attached file '" << path << "'";
  } else {
    LOG(ERROR) << "Failed to attach file '" << path << "': "
               << (result.isFailed() ? result.failure() : "discarded");
  }
}


void Master::submitScheduler(const string& name)
{
  LOG(INFO) << "Scheduler submit request for " << name;
  SubmitSchedulerResponse response;
  response.set_okay(false);
  reply(response);
}


void Master::contended(const Future<Future<Nothing> >& candidacy)
{
  CHECK(!candidacy.isDiscarded());

  if (candidacy.isFailed()) {
    EXIT(1) << "Failed to contend: " << candidacy.failure();
  }

  // Watch for candidacy change.
  candidacy.get()
    .onAny(defer(self(), &Master::lostCandidacy, lambda::_1));
}


void Master::lostCandidacy(const Future<Nothing>& lost)
{
  CHECK(!lost.isDiscarded());

  if (lost.isFailed()) {
    EXIT(1) << "Failed to watch for candidacy: " << lost.failure();
  }

  if (elected()) {
    EXIT(1) << "Lost leadership... committing suicide!";
  }

  LOG(INFO) << "Lost candidacy as a follower... Contend again";
  contender->contend()
    .onAny(defer(self(), &Master::contended, lambda::_1));
}


void Master::detected(const Future<Option<MasterInfo> >& _leader)
{
  CHECK(!_leader.isDiscarded());

  if (_leader.isFailed()) {
    EXIT(1) << "Failed to detect the leading master: " << _leader.failure()
            << "; committing suicide!";
  }

  bool wasElected = elected();
  leader = _leader.get();

  LOG(INFO) << "The newly elected leader is "
            << (leader.isSome()
                ? (leader.get().pid() + " with id " + leader.get().id())
                : "None");

  if (wasElected && !elected()) {
    EXIT(1) << "Lost leadership... committing suicide!";
  }

  if (elected()) {
    electedTime = Clock::now();

    if (!wasElected) {
      LOG(INFO) << "Elected as the leading master!";

      // Begin the recovery process, bail if it fails or is discarded.
      recover()
        .onFailed(lambda::bind(fail, "Recovery failed", lambda::_1))
        .onDiscarded(lambda::bind(fail, "Recovery failed", "discarded"));
    } else {
      // This happens if there is a ZK blip that causes a re-election
      // but the same leading master is elected as leader.
      LOG(INFO) << "Re-elected as the leading master";
    }
  }

  // Keep detecting.
  detector->detect(leader)
    .onAny(defer(self(), &Master::detected, lambda::_1));
}


void Master::registerFramework(
    const UPID& from,
    const FrameworkInfo& frameworkInfo)
{
  ++metrics.messages_register_framework;

  if (authenticating.contains(from)) {
    LOG(INFO) << "Queuing up registration request from " << from
              << " because authentication is still in progress";

    authenticating[from]
      .onReady(defer(self(), &Self::registerFramework, from, frameworkInfo));
    return;
  }

  if (flags.authenticate_frameworks) {
    if (!authenticated.contains(from)) {
      // This could happen if another authentication request came
      // through before we are here or if a framework tried to register
      // without authentication.
      LOG(WARNING) << "Refusing registration of framework at " << from
                   << " because it is not authenticated";
      FrameworkErrorMessage message;
      message.set_message("Framework at " + stringify(from) +
                          " is not authenticated.");
      send(from, message);
      return;
    } else if (frameworkInfo.has_principal() &&
               frameworkInfo.principal() != authenticated[from]) {
      LOG(WARNING) << "Refusing registration of framework at " << from
                   << " because its principal '" << frameworkInfo.principal()
                   << "' does not match what it used in authentication: '"
                   << authenticated[from] << "'";
      FrameworkErrorMessage message;
      message.set_message("Framework principal " + frameworkInfo.principal() +
                          " does not match what was used in authentication: " +
                          authenticated[from]);
      send(from, message);
      return;
    } else if (!frameworkInfo.has_principal()) {
      LOG(WARNING) << "Framework at " << from
                   << " does not specify principal in its FrameworkInfo";
    }
  }

  if (!roles.contains(frameworkInfo.role())) {
    FrameworkErrorMessage message;
    message.set_message("Role '" + frameworkInfo.role() + "' is not valid.");
    send(from, message);
    return;
  }

  LOG(INFO) << "Received registration request from " << from;

  // Check if this framework is already registered (because it retries).
  foreachvalue (Framework* framework, frameworks.activated) {
    if (framework->pid == from) {
      LOG(INFO) << "Framework " << framework->id << " (" << framework->pid
                << ") already registered, resending acknowledgement";
      FrameworkRegisteredMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.mutable_master_info()->MergeFrom(info_);
      send(from, message);
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
    send(from, message);
    delete framework;
    return;
  }

  addFramework(framework);
}


void Master::reregisterFramework(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    bool failover)
{
  ++metrics.messages_reregister_framework;

  if (authenticating.contains(from)) {
    LOG(INFO) << "Queuing up re-registration request from " << from
              << " because authentication is still in progress";

    authenticating[from]
      .onReady(defer(self(),
                     &Self::reregisterFramework,
                     from,
                     frameworkInfo,
                     failover));
    return;
  }

  if (!frameworkInfo.has_id() || frameworkInfo.id() == "") {
    LOG(ERROR) << "Framework re-registering without an id!";
    FrameworkErrorMessage message;
    message.set_message("Framework reregistered without a framework id");
    send(from, message);
    return;
  }

  if (flags.authenticate_frameworks) {
    if (!authenticated.contains(from)) {
      // This could happen if another authentication request came
      // through before we are here or if a framework tried to
      // re-register without authentication.
      LOG(WARNING) << "Refusing re-registration of framework at " << from
                   << " because it is not authenticated";
      FrameworkErrorMessage message;
      message.set_message("Framework '" + frameworkInfo.id().value() + "' at " +
                          stringify(from) + " is not authenticated.");
      send(from, message);
      return;
    } else if (frameworkInfo.has_principal() &&
               frameworkInfo.principal() != authenticated[from]) {
      LOG(WARNING) << "Refusing re-registration of framework at " << from
                   << " because its principal '" << frameworkInfo.principal()
                   << "' does not match what it used in authentication: '"
                   << authenticated[from] << "'";
      FrameworkErrorMessage message;
      message.set_message("Framework principal " + frameworkInfo.principal() +
                          " does not match what was used in authentication: " +
                          authenticated[from]);
      send(from, message);
      return;
    } else if (!frameworkInfo.has_principal()) {
      LOG(WARNING) << "Framework at " << from
                   << " does not specify principal in its FrameworkInfo";
    }
  }

  if (!roles.contains(frameworkInfo.role())) {
    FrameworkErrorMessage message;
    message.set_message("Role '" + frameworkInfo.role() + "' is not valid.");
    send(from, message);
    return;
  }

  LOG(INFO) << "Re-registering framework " << frameworkInfo.id()
            << " at " << from;

  if (frameworks.activated.count(frameworkInfo.id()) > 0) {
    // Using the "failover" of the scheduler allows us to keep a
    // scheduler that got partitioned but didn't die (in ZooKeeper
    // speak this means didn't lose their session) and then
    // eventually tried to connect to this master even though
    // another instance of their scheduler has reconnected. This
    // might not be an issue in the future when the
    // master/allocator launches the scheduler can get restarted
    // (if necessary) by the master and the master will always
    // know which scheduler is the correct one.

    Framework* framework = frameworks.activated[frameworkInfo.id()];
    framework->reregisteredTime = Clock::now();

    if (failover) {
      // We do not attempt to detect a duplicate re-registration
      // message here because it is impossible to distinguish between
      // a duplicate message, and a scheduler failover to the same
      // pid, given the existing libprocess primitives (PID does not
      // identify the libprocess Process instance).

      // TODO: Should we check whether the new scheduler has given
      // us a different framework name, user name or executor info?
      LOG(INFO) << "Framework " << frameworkInfo.id() << " failed over";
      failoverFramework(framework, from);
    } else if (from != framework->pid) {
      LOG(ERROR)
        << "Framework " << frameworkInfo.id() << " at " << from
        << " attempted to re-register while a framework at " << framework->pid
        << " is already registered";
      FrameworkErrorMessage message;
      message.set_message("Framework failed over");
      send(from, message);
      return;
    } else {
      LOG(INFO) << "Allowing the Framework " << frameworkInfo.id()
                << " to re-register with an already used id";

      // Make sure we can get offers again.
      // The framework might have been deactivated if it was doing
      // authentication.
      // TODO(vinod): Consider adding 'Master::activate(Framework*)'.
      // TODO(vinod): Do this after we recover resources below.
      if (!framework->active) {
        framework->active = true;
        allocator->frameworkActivated(framework->id, framework->info);
      }

      // Remove any offers sent to this framework.
      // NOTE: We need to do this because the scheduler might have
      // replied to the offers but the driver might have dropped
      // those messages since it wasn't connected to the master.
      foreach (Offer* offer, utils::copy(framework->offers)) {
        allocator->resourcesRecovered(
            offer->framework_id(), offer->slave_id(), offer->resources());
        removeOffer(offer, true); // Rescind.
      }

      FrameworkReregisteredMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkInfo.id());
      message.mutable_master_info()->MergeFrom(info_);
      send(from, message);
      return;
    }
  } else {
    // We don't have a framework with this ID, so we must be a newly
    // elected Mesos master to which either an existing scheduler or a
    // failed-over one is connecting. Create a Framework object and add
    // any tasks it has that have been reported by reconnecting slaves.
    Framework* framework =
      new Framework(frameworkInfo, frameworkInfo.id(), from, Clock::now());
    framework->reregisteredTime = Clock::now();

    // TODO(benh): Check for root submissions like above!

    // Add any running tasks reported by slaves for this framework.
    foreachvalue (Slave* slave, slaves.activated) {
      foreachkey (const FrameworkID& frameworkId, slave->tasks) {
        foreachvalue (Task* task, slave->tasks[frameworkId]) {
          if (framework->id == task->framework_id()) {
            framework->addTask(task);

            // Also add the task's executor for resource accounting
            // if it's still alive on the slave and we've not yet
            // added it to the framework.
            if (task->has_executor_id() &&
                slave->hasExecutor(framework->id, task->executor_id()) &&
                !framework->hasExecutor(slave->id, task->executor_id())) {
              const ExecutorInfo& executorInfo =
                slave->executors[framework->id][task->executor_id()];
              framework->addExecutor(slave->id, executorInfo);
            }
          }
        }
      }
    }

    // N.B. Need to add the framework _after_ we add its tasks
    // (above) so that we can properly determine the resources it's
    // currently using!
    addFramework(framework);
  }

  CHECK(frameworks.activated.contains(frameworkInfo.id()))
    << "Unknown framework " << frameworkInfo.id();

  // Broadcast the new framework pid to all the slaves. We have to
  // broadcast because an executor might be running on a slave but
  // it currently isn't running any tasks. This could be a
  // potential scalability issue ...
  foreachvalue (Slave* slave, slaves.activated) {
    UpdateFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkInfo.id());
    message.set_pid(from);
    send(slave->pid, message);
  }

  return;
}


void Master::unregisterFramework(
    const UPID& from,
    const FrameworkID& frameworkId)
{
  ++metrics.messages_unregister_framework;

  LOG(INFO) << "Asked to unregister framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    if (framework->pid == from) {
      removeFramework(framework);
    } else {
      LOG(WARNING)
        << "Ignoring unregister framework message for framework " << frameworkId
        << " from " << from << " because it is not from the registered"
        << " framework " << framework->pid;
    }
  }
}


void Master::deactivateFramework(
    const UPID& from,
    const FrameworkID& frameworkId)
{
  ++metrics.messages_deactivate_framework;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring deactivate framework message for framework " << frameworkId
      << " because the framework cannot be found";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring deactivate framework message for framework " << frameworkId
      << " from '" << from << "' because it is not from the registered"
      << " framework '" << framework->pid << "'";
    return;
  }

  deactivate(framework);
}


void Master::deactivate(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Deactivating framework " << framework->id;

  // Stop sending offers here for now.
  framework->active = false;

  // Tell the allocator to stop allocating resources to this framework.
  allocator->frameworkDeactivated(framework->id);

  // Remove the framework from authenticated. This is safe because
  // a framework will always reauthenticate before (re-)registering.
  authenticated.erase(framework->pid);

  // Remove the framework's offers.
  foreach (Offer* offer, utils::copy(framework->offers)) {
    allocator->resourcesRecovered(
        offer->framework_id(), offer->slave_id(), offer->resources());
    removeOffer(offer, true); // Rescind.
  }
}


void Master::disconnect(Slave* slave)
{
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Disconnecting slave " << slave->id;

  // Mark the slave as disconnected and remove it from the allocator.
  slave->disconnected = true;
  allocator->slaveDisconnected(slave->id);

  // Remove the slave from authenticated. This is safe because
  // a slave will always reauthenticate before (re-)registering.
  authenticated.erase(slave->pid);

  // Remove and rescind offers.
  foreach (Offer* offer, utils::copy(slave->offers)) {
    allocator->resourcesRecovered(
        offer->framework_id(), slave->id, offer->resources());

    removeOffer(offer, true); // Rescind!
  }
}


void Master::resourceRequest(
    const UPID& from,
    const FrameworkID& frameworkId,
    const vector<Request>& requests)
{
  ++metrics.messages_resource_request;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
        << "Ignoring resource request message from framework " << frameworkId
        << " because the framework cannot be found";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring resource request message from framework " << frameworkId
      << " from '" << from << "' because it is not from the registered "
      << " framework '" << framework->pid << "'";
    return;
  }

  LOG(INFO) << "Requesting resources for framework " << frameworkId;
  allocator->resourcesRequested(frameworkId, requests);
}


// We use the visitor pattern to abstract the process of performing
// any validations, aggregations, etc. of tasks that a framework
// attempts to run within the resources provided by offers. A
// visitor can return an optional error (typedef'ed as an option of a
// string) which will cause the master to send a failed status update
// back to the framework for only that task description. An instance
// will be reused for each task description from same 'launchTasks()',
// but not for task descriptions from different offers.
typedef Option<string> TaskInfoError;

struct TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      const Resources& resources,
      const Framework& framework,
      const Slave& slave) = 0;

  virtual ~TaskInfoVisitor() {}
};

// Checks that a task id is valid, i.e., contains only valid characters.
struct TaskIDChecker : TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      const Resources& resources,
      const Framework& framework,
      const Slave& slave)
  {
    const string& id = task.task_id().value();

    if (std::count_if(id.begin(), id.end(), invalid) > 0) {
      return "TaskID '" + id + "' contains invalid characters";
    }

    return None();
  }

  static bool invalid(char c)
  {
    return iscntrl(c) || c == '/' || c == '\\';
  }
};


// Checks that the slave ID used by a task is correct.
struct SlaveIDChecker : TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      const Resources& resources,
      const Framework& framework,
      const Slave& slave)
  {
    if (!(task.slave_id() == slave.id)) {
      return "Task uses invalid slave " + task.slave_id().value() +
          " while slave " + slave.id.value() + " is expected";
    }

    return None();
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
      const Resources& resources,
      const Framework& framework,
      const Slave& slave)
  {
    const TaskID& taskId = task.task_id();

    if (ids.contains(taskId) || framework.tasks.contains(taskId)) {
      return "Task has duplicate ID: " + taskId.value();
    }

    ids.insert(taskId);

    return None();
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
      const Resources& resources,
      const Framework& framework,
      const Slave& slave)
  {
    if (task.resources().size() == 0) {
      return stringify("Task uses no resources");
    }

    foreach (const Resource& resource, task.resources()) {
      if (!Resources::isAllocatable(resource)) {
        return "Task uses invalid resources: " + stringify(resource);
      }
    }

    // Check if this task uses more resources than offered.
    Resources taskResources = task.resources();

    if (!((usedResources + taskResources) <= resources)) {
      return "Task " + stringify(task.task_id()) + " attempted to use " +
          stringify(taskResources) + " combined with already used " +
          stringify(usedResources) + " is greater than offered " +
          stringify(resources);
    }

    // Check this task's executor's resources.
    if (task.has_executor()) {
      // TODO(benh): Check that the executor uses some resources.
      foreach (const Resource& resource, task.executor().resources()) {
        if (!Resources::isAllocatable(resource)) {
          // TODO(benh): Send back the invalid resources?
          return "Executor for task " + stringify(task.task_id()) +
              " uses invalid resources " + stringify(resource);
        }
      }

      // Check if this task's executor is running, and if not check if
      // the task + the executor use more resources than offered.
      if (!executors.contains(task.executor().executor_id())) {
        if (!slave.hasExecutor(framework.id, task.executor().executor_id())) {
          taskResources += task.executor().resources();
          if (!((usedResources + taskResources) <= resources)) {
            return "Task " + stringify(task.task_id()) +
                   " + executor attempted to use " + stringify(taskResources) +
                   " combined with already used " + stringify(usedResources) +
                   " is greater than offered " + stringify(resources);
          }
        }
        executors.insert(task.executor().executor_id());
      }
    }

    usedResources += taskResources;

    return None();
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
      const Resources& resources,
      const Framework& framework,
      const Slave& slave)
  {
    if (task.has_executor() == task.has_command()) {
      return stringify(
          "Task should have at least one (but not both) of CommandInfo or"
          " ExecutorInfo present");
    }

    if (task.has_executor()) {
      const ExecutorID& executorId = task.executor().executor_id();
      if (slave.hasExecutor(framework.id, executorId)) {
        const Option<ExecutorInfo> executorInfo =
          slave.executors.get(framework.id).get().get(executorId);

        if (!(task.executor() == executorInfo.get())) {
          return "Task has invalid ExecutorInfo (existing ExecutorInfo"
              " with same ExecutorID is not compatible).\n"
              "------------------------------------------------------------\n"
              "Existing ExecutorInfo:\n" +
              stringify(executorInfo.get()) + "\n"
              "------------------------------------------------------------\n"
              "Task's ExecutorInfo:\n" +
              stringify(task.executor()) + "\n"
              "------------------------------------------------------------\n";
        }
      }
    }

    return None();
  }
};


// Checks that a task that asks for checkpointing is not being
// launched on a slave that has not enabled checkpointing.
struct CheckpointChecker : TaskInfoVisitor
{
  virtual TaskInfoError operator () (
      const TaskInfo& task,
      const Resources& resources,
      const Framework& framework,
      const Slave& slave)
  {
    if (framework.info.checkpoint() && !slave.info.checkpoint()) {
      return "Task asked to be checkpointed but slave " +
          stringify(slave.id) + " has checkpointing disabled";
    }
    return None();
  }
};


// OfferVisitors are similar to the TaskInfoVisitor pattern and
// are used for validation and aggregation of offers.
// The error reporting scheme is also similar to TaskInfoVisitor.
// However, offer processing (and subsequent task processing) is
// aborted altogether if offer visitor reports an error.
typedef Option<string> OfferError;

struct OfferVisitor
{
  virtual OfferError operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master) = 0;

  virtual ~OfferVisitor() {}

  Slave* getSlave(Master* master, const SlaveID& slaveId)
  {
    CHECK_NOTNULL(master);
    return master->getSlave(slaveId);
  }

  Offer* getOffer(Master* master, const OfferID& offerId)
  {
    CHECK_NOTNULL(master);
    return master->getOffer(offerId);
  }
};


// Checks validity/liveness of an offer.
struct ValidOfferChecker : OfferVisitor {
  virtual OfferError operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master)
  {
    Offer* offer = getOffer(master, offerId);
    if (offer == NULL) {
      return "Offer " + stringify(offerId) + " is no longer valid";
    }

    return None();
  }
};


// Checks that an offer belongs to the expected framework.
struct FrameworkChecker : OfferVisitor {
  virtual OfferError operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master)
  {
    Offer* offer = getOffer(master, offerId);
    if (offer == NULL) {
      return "Offer " + stringify(offerId) + " is no longer valid";
    }

    if (!(framework.id == offer->framework_id())) {
      return "Offer " + stringify(offer->id()) +
          " has invalid framework " + stringify(offer->framework_id()) +
          " while framework " + stringify(framework.id) + " is expected";
    }

    return None();
  }
};


// Checks that the slave is valid and ensures that all offers belong to
// the same slave.
struct SlaveChecker : OfferVisitor
{
  virtual OfferError operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master)
  {
    Offer* offer = getOffer(master, offerId);
    if (offer == NULL) {
      return "Offer " + stringify(offerId) + " is no longer valid";
    }

    Slave* slave = getSlave(master, offer->slave_id());

    // This is not possible because the offer should've been removed.
    CHECK(slave != NULL)
      << "Offer " << offerId
      << " outlived slave " << offer->slave_id();

    // This is not possible because the offer should've been removed.
    CHECK(!slave->disconnected)
      << "Offer " << offerId
      << " outlived disconnected slave " << offer->slave_id();

    if (slaveId.isNone()) {
      // Set slave id and use as base case for validation.
      slaveId = slave->id;
    } else if (!(slave->id == slaveId.get())) {
      return "Aggregated offers must belong to one single slave. Offer " +
          stringify(offerId) + " uses slave " +
          stringify(slave->id) + " and slave " +
          stringify(slaveId.get());
    }

    return None();
  }

  Option<const SlaveID> slaveId;
};


// Checks that an offer only appears once in offer list.
struct UniqueOfferIDChecker : OfferVisitor
{
  virtual OfferError operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master)
  {
    if (offers.contains(offerId)) {
      return "Duplicate offer " + stringify(offerId) + " in offer list";
    }
    offers.insert(offerId);

    return None();
  }

  hashset<OfferID> offers;
};


void Master::launchTasks(
    const UPID& from,
    const FrameworkID& frameworkId,
    const OfferID& offerId,
    const vector<TaskInfo>& tasks,
    const Filters& filters,
    const vector<OfferID>& _offerIds)
{
  ++metrics.messages_launch_tasks;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring launch tasks message for offer "
      << stringify(_offerIds.empty() ? stringify(offerId)
                                     : stringify(_offerIds))
      << " of framework " << frameworkId
      << " because the framework cannot be found";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring launch tasks message for offer "
      << stringify(_offerIds.empty() ? stringify(offerId)
                                     : stringify(_offerIds))
      << " of framework " << frameworkId << " from '" << from
      << "' because it is not from the registered framework '"
      << framework->pid << "'";
    return;
  }

  // Support single offerId for backward compatibility.
  // OfferIds will be ignored if offerId is set.
  vector<OfferID> offerIds;
  if (offerId.has_value()) {
    offerIds.push_back(offerId);
  } else if (_offerIds.size() > 0) {
    offerIds = _offerIds;
  } else {
    LOG(WARNING) << "No offers to launch tasks on";

    foreach (const TaskInfo& task, tasks) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id,
          task.slave_id(),
          task.task_id(),
          TASK_LOST,
          "Task launched without offers");

      LOG(INFO) << "Sending status update " << update
                << " for launch task attempt without offers";
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update);
      send(framework->pid, message);
    }
    return;
  }

  // Common slave id for task validation.
  Option<SlaveID> slaveId;

  // Create offer visitors.
  list<OfferVisitor*> offerVisitors;
  offerVisitors.push_back(new ValidOfferChecker());
  offerVisitors.push_back(new FrameworkChecker());
  offerVisitors.push_back(new SlaveChecker());
  offerVisitors.push_back(new UniqueOfferIDChecker());

  // Verify and aggregate all offers.
  // Abort offer and task processing if any offer validation failed.
  Resources totalResources;
  OfferError offerError = None();
  foreach (const OfferID& offerId, offerIds) {
    foreach (OfferVisitor* visitor, offerVisitors) {
      offerError = (*visitor)(offerId, *framework, this);
      if (offerError.isSome()) {
        break;
      }
    }
    // Offer validation error needs to be propagated from visitor
    // loop above.
    if (offerError.isSome()) {
      break;
    }

    // If offer validation succeeds, we need to pass along the common
    // slave. So optimistically, we store the first slave id we see.
    // In case of invalid offers (different slaves for example), we
    // report error and return from launchTask before slaveId is used.
    if (slaveId.isNone()) {
      slaveId = getOffer(offerId)->slave_id();
    }

    totalResources += getOffer(offerId)->resources();
  }

  // Cleanup visitors.
  while (!offerVisitors.empty()) {
    OfferVisitor* visitor = offerVisitors.front();
    offerVisitors.pop_front();
    delete visitor;
  };

  // Remove offers and recover resources if any of the offers are
  // invalid.
  foreach (const OfferID& offerId, offerIds) {
    Offer* offer = getOffer(offerId);
    if (offer != NULL) {
      if (offerError.isSome()) {
        allocator->resourcesRecovered(
            offer->framework_id(), offer->slave_id(), offer->resources());
      }
      removeOffer(offer);
    }
  }

  if (offerError.isSome()) {
    LOG(WARNING) << "Failed to validate offer " << offerId
                   << ": " << offerError.get();

    foreach (const TaskInfo& task, tasks) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id,
          task.slave_id(),
          task.task_id(),
          TASK_LOST,
          "Task launched with invalid offers: " + offerError.get());

      LOG(INFO) << "Sending status update " << update
                << " for launch task attempt on invalid offers: "
                << stringify(offerIds);
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update);
      send(framework->pid, message);
    }
    return;
  }

  CHECK(slaveId.isSome()) << "Slave id not found";
  Slave* slave = CHECK_NOTNULL(getSlave(slaveId.get()));

  LOG(INFO) << "Processing reply for offers: "
            << stringify(offerIds)
            << " on slave " << *slave
            << " for framework " << framework->id;

  Resources usedResources; // Accumulated resources used.

  // Create task visitors.
  list<TaskInfoVisitor*> taskVisitors;
  taskVisitors.push_back(new TaskIDChecker());
  taskVisitors.push_back(new SlaveIDChecker());
  taskVisitors.push_back(new UniqueTaskIDChecker());
  taskVisitors.push_back(new ResourceUsageChecker());
  taskVisitors.push_back(new ExecutorInfoChecker());
  taskVisitors.push_back(new CheckpointChecker());

  // TODO(benh): Add a HealthCheckChecker visitor.

  // Loop through each task and check it's validity.
  foreach (const TaskInfo& task, tasks) {
    // Possible error found while checking task's validity.
    TaskInfoError error = None();

    // Invoke each visitor.
    foreach (TaskInfoVisitor* visitor, taskVisitors) {
      error = (*visitor)(task, totalResources, *framework, *slave);
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

      LOG(INFO) << "Sending status update "
                << update << " for invalid task";
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update);
      send(framework->pid, message);
    }
  }

  // All used resources should be allocatable, enforced by our validators.
  CHECK_EQ(usedResources, usedResources.allocatable());

  // Calculate unused resources.
  Resources unusedResources = totalResources - usedResources;

  if (unusedResources.allocatable().size() > 0) {
    // Tell the allocator about the unused (e.g., refused) resources.
    allocator->resourcesUnused(
        framework->id,
        slave->id,
        unusedResources,
        filters);
  }

  // Cleanup visitors.
  while (!taskVisitors.empty()) {
    TaskInfoVisitor* visitor = taskVisitors.front();
    taskVisitors.pop_front();
    delete visitor;
  };
}


void Master::reviveOffers(const UPID& from, const FrameworkID& frameworkId)
{
  ++metrics.messages_revive_offers;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring revive offers message for framework " << frameworkId
      << " because the framework cannot be found";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring revive offers message for framework " << frameworkId
      << " from '" << from << "' because it is not from the registered"
      << " framework '" << framework->pid << "'";
    return;
  }

  LOG(INFO) << "Reviving offers for framework " << framework->id;
  allocator->offersRevived(framework->id);
}


void Master::killTask(
    const UPID& from,
    const FrameworkID& frameworkId,
    const TaskID& taskId)
{
  ++metrics.messages_kill_task;

  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring kill task message for task " << taskId << " of framework "
      << frameworkId << " because the framework cannot be found";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring kill task message for task " << taskId
      << " of framework " << frameworkId << " from '" << from
      << "' because it is not from the registered framework '"
      << framework->pid << "'";
    return;
  }

  Task* task = framework->getTask(taskId);
  if (task == NULL) {
    // TODO(bmahler): Per MESOS-1200, if we knew the SlaveID here we
    // could reply more frequently in the presence of slaves in a
    // transitionary state.
    if (!slaves.recovered.empty()) {
      LOG(WARNING)
        << "Cannot kill task " << taskId << " of framework " << frameworkId
        << " because the slave containing this task may not have re-registered"
        << " yet with this master";
    } else if (!slaves.reregistering.empty()) {
      LOG(WARNING)
        << "Cannot kill task " << taskId << " of framework " << frameworkId
        << " because the slave may be in the process of being re-admitted by"
        << " the registrar";
    } else if (!slaves.removing.empty()) {
      LOG(WARNING)
        << "Cannot kill task " << taskId << " of framework " << frameworkId
        << " because the slave may be in the process of being removed from the"
        << " registrar, it is likely TASK_LOST updates will occur when the"
        << " slave is removed";
    } else if (flags.registry_strict) {
      // For a strict registry, if there are no slaves transitioning
      // between states, then this task is definitely unknown!
      LOG(WARNING)
        << "Cannot kill task " << taskId << " of framework " << frameworkId
        << " because it cannot be found; sending TASK_LOST since there are"
        << " no transitionary slaves";

      StatusUpdateMessage message;
      StatusUpdate* update = message.mutable_update();
      update->mutable_framework_id()->MergeFrom(frameworkId);
      TaskStatus* status = update->mutable_status();
      status->mutable_task_id()->MergeFrom(taskId);
      status->set_state(TASK_LOST);
      status->set_message("Attempted to kill an unknown task");
      update->set_timestamp(Clock::now().secs());
      update->set_uuid(UUID::random().toBytes());
      send(framework->pid, message);
    } else {
      // For a non-strict registry, the slave holding this task could
      // be readmitted even if we have no knowledge of it.
      LOG(WARNING)
        << "Cannot kill task " << taskId << " of framework " << frameworkId
        << " because it cannot be found; cannot send TASK_LOST since a"
        << " non-strict registry is in use";
    }

    return;
  }

  Slave* slave = getSlave(task->slave_id());
  CHECK(slave != NULL) << "Unknown slave " << task->slave_id();

  // We add the task to 'killedTasks' here because the slave
  // might be partitioned or disconnected but the master
  // doesn't know it yet.
  slave->killedTasks.put(frameworkId, taskId);

  // NOTE: This task will be properly reconciled when the
  // disconnected slave re-registers with the master.
  if (!slave->disconnected) {
    LOG(INFO) << "Telling slave " << *slave
              << " to kill task " << taskId
              << " of framework " << frameworkId;

    KillTaskMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_task_id()->MergeFrom(taskId);
    send(slave->pid, message);
  }
}


void Master::statusUpdateAcknowledgement(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const TaskID& taskId,
    const string& uuid)
{
  metrics.messages_status_update_acknowledgement++;

  // TODO(bmahler): Consider adding a message validator abstraction
  // for the master that takes care of all this boilerplate. Ideally
  // by the time we process messages in the critical master code, we
  // can assume that they are valid. This will become especially
  // important as validation logic is moved out of the scheduler
  // driver and into the master.

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring status update acknowledgement message for task " << taskId
      << " of framework " << frameworkId << " on slave " << slaveId
      << " because the framework cannot be found";
    metrics.invalid_status_update_acknowledgements++;
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring status update acknowledgement message for task " << taskId
      << " of framework " << frameworkId << " on slave " << slaveId
      << " from " << from << " because it is not from the registered framework "
      << framework->pid;
    metrics.invalid_status_update_acknowledgements++;
    return;
  }

  Slave* slave = getSlave(slaveId);

  if (slave == NULL) {
    LOG(WARNING)
      << "Cannot send status update acknowledgement message for task " << taskId
      << " of framework " << frameworkId << " to slave " << slaveId
      << " because slave is not activated";
    metrics.invalid_status_update_acknowledgements++;
    return;
  }

  if (slave->disconnected) {
    LOG(WARNING)
      << "Cannot send status update acknowledgement message for task " << taskId
      << " of framework " << frameworkId << " to slave " << *slave
      << " because slave is disconnected";
    metrics.invalid_status_update_acknowledgements++;
    return;
  }

  LOG(INFO) << "Forwarding status update acknowledgement "
            << UUID::fromBytes(uuid) << " for task " << taskId
            << " of framework " << frameworkId << " to slave " << *slave;

  // TODO(bmahler): Once we store terminal unacknowledged updates in
  // the master per MESOS-1410, this is where we'll find the
  // unacknowledged task and remove it if present.
  // Also, be sure to confirm Master::reconcile is still correct!

  StatusUpdateAcknowledgementMessage message;
  message.mutable_slave_id()->CopyFrom(slaveId);
  message.mutable_framework_id()->CopyFrom(frameworkId);
  message.mutable_task_id()->CopyFrom(taskId);
  message.set_uuid(uuid);

  send(slave->pid, message);

  metrics.valid_status_update_acknowledgements++;
}


void Master::schedulerMessage(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  ++metrics.messages_framework_to_executor;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring framework message for executor " << executorId
      << " of framework " << frameworkId
      << " because the framework cannot be found";
    stats.invalidFrameworkMessages++;
    metrics.invalid_framework_to_executor_messages++;
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring framework message for executor " << executorId
      << " of framework " << frameworkId << " from " << from
      << " because it is not from the registered framework "
      << framework->pid;
    stats.invalidFrameworkMessages++;
    metrics.invalid_framework_to_executor_messages++;
    return;
  }

  Slave* slave = getSlave(slaveId);
  if (slave == NULL) {
    LOG(WARNING) << "Cannot send framework message for framework "
                 << frameworkId << " to slave " << slaveId
                 << " because slave is not activated";
    stats.invalidFrameworkMessages++;
    metrics.invalid_framework_to_executor_messages++;
    return;
  }

  if (slave->disconnected) {
    LOG(WARNING) << "Cannot send framework message for framework "
                 << frameworkId << " to slave " << *slave
                 << " because slave is disconnected";
    stats.invalidFrameworkMessages++;
    metrics.invalid_framework_to_executor_messages++;
    return;
  }

  LOG(INFO) << "Sending framework message for framework "
            << frameworkId << " to slave " << *slave;

  FrameworkToExecutorMessage message;
  message.mutable_slave_id()->MergeFrom(slaveId);
  message.mutable_framework_id()->MergeFrom(frameworkId);
  message.mutable_executor_id()->MergeFrom(executorId);
  message.set_data(data);
  send(slave->pid, message);

  stats.validFrameworkMessages++;
  metrics.valid_framework_to_executor_messages++;
}


void Master::registerSlave(const UPID& from, const SlaveInfo& slaveInfo)
{
  ++metrics.messages_register_slave;

  if (authenticating.contains(from)) {
    LOG(INFO) << "Queuing up registration request from " << from
              << " because authentication is still in progress";

    authenticating[from]
      .onReady(defer(self(), &Self::registerSlave, from, slaveInfo));
    return;
  }

  if (flags.authenticate_slaves && !authenticated.contains(from)) {
    // This could happen if another authentication request came
    // through before we are here or if a slave tried to register
    // without authentication.
    LOG(WARNING) << "Refusing registration of slave at " << from
                 << " because it is not authenticated";
    ShutdownMessage message;
    message.set_message("Slave is not authenticated");
    send(from, message);
    return;
  }

  // Check if this slave is already registered (because it retries).
  foreachvalue (Slave* slave, slaves.activated) {
    if (slave->pid == from) {
      if (slave->disconnected) {
        // The slave was previously disconnected but it is now trying
        // to register as a new slave. This could happen if the slave
        // failed recovery and hence registering as a new slave before
        // the master removed the old slave from its map.
        LOG(INFO)
          << "Removing old disconnected slave " << *slave
          << " because a registration attempt is being made from " << from;
        removeSlave(slave);
        break;
      } else {
        LOG(INFO) << "Slave " << *slave << " already registered,"
                  << " resending acknowledgement";
        SlaveRegisteredMessage message;
        message.mutable_slave_id()->MergeFrom(slave->id);
        send(from, message);
        return;
      }
    }
  }

  // We need to generate a SlaveID and admit this slave only *once*.
  if (slaves.registering.contains(from)) {
    LOG(INFO) << "Ignoring register slave message from " << from
              << " (" << slaveInfo.hostname() << ") as admission is"
              << " already in progress";
    return;
  }

  slaves.registering.insert(from);

  // Create and add the slave id.
  SlaveInfo slaveInfo_ = slaveInfo;
  slaveInfo_.mutable_id()->CopyFrom(newSlaveId());

  LOG(INFO) << "Registering slave at " << from << " ("
            << slaveInfo.hostname() << ") with id " << slaveInfo_.id();

  registrar->apply(Owned<Operation>(new AdmitSlave(slaveInfo_)))
    .onAny(defer(self(),
                 &Self::_registerSlave,
                 slaveInfo_,
                 from,
                 lambda::_1));
}


void Master::_registerSlave(
    const SlaveInfo& slaveInfo,
    const UPID& pid,
    const Future<bool>& admit)
{
  slaves.registering.erase(pid);

  CHECK(!admit.isDiscarded());

  if (admit.isFailed()) {
    LOG(FATAL) << "Failed to admit slave " << slaveInfo.id() << " at " << pid
               << " (" << slaveInfo.hostname() << "): " << admit.failure();
  } else if (!admit.get()) {
    // This means the slave is already present in the registrar, it's
    // likely we generated a duplicate slave id!
    LOG(ERROR) << "Slave " << slaveInfo.id() << " at " << pid
               << " (" << slaveInfo.hostname() << ") was not admitted, "
               << "asking to shut down";
    slaves.deactivated.put(slaveInfo.id(), Nothing());

    ShutdownMessage message;
    message.set_message(
        "Slave attempted to register but got duplicate slave id " +
        stringify(slaveInfo.id()));
    send(pid, message);
  } else {
    Slave* slave = new Slave(slaveInfo, slaveInfo.id(), pid, Clock::now());

    LOG(INFO) << "Registered slave " << *slave;
    ++metrics.slave_registrations;

    addSlave(slave);
  }
}


void Master::reregisterSlave(
    const UPID& from,
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const vector<ExecutorInfo>& executorInfos,
    const vector<Task>& tasks,
    const vector<Archive::Framework>& completedFrameworks)
{
  ++metrics.messages_reregister_slave;

  if (authenticating.contains(from)) {
    LOG(INFO) << "Queuing up re-registration request from " << from
              << " because authentication is still in progress";

    authenticating[from]
      .onReady(defer(self(),
                     &Self::reregisterSlave,
                     from,
                     slaveId,
                     slaveInfo,
                     executorInfos,
                     tasks,
                     completedFrameworks));
    return;
  }

  if (flags.authenticate_slaves && !authenticated.contains(from)) {
    // This could happen if another authentication request came
    // through before we are here or if a slave tried to
    // re-register without authentication.
    LOG(WARNING) << "Refusing re-registration of slave at " << from
                 << " because it is not authenticated";
    ShutdownMessage message;
    message.set_message("Slave is not authenticated");
    send(from, message);
    return;
  }


  if (slaves.deactivated.get(slaveInfo.id()).isSome()) {
    // To compensate for the case where a non-strict registrar is
    // being used, we explicitly deny deactivated slaves from
    // re-registering. This is because a non-strict registrar cannot
    // enforce this. We've already told frameworks the tasks were
    // lost so it's important to deny the slave from re-registering.
    LOG(WARNING) << "Slave " << slaveId << " at " << from
                 << " (" << slaveInfo.hostname() << ") attempted to "
                 << "re-register after deactivation; shutting it down";

    ShutdownMessage message;
    message.set_message("Slave attempted to re-register after deactivation");
    send(from, message);
    return;
  }

  Slave* slave = getSlave(slaveInfo.id());

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
                 << " in use id (" << slave->id << ")";

    // TODO(bmahler): There's an implicit assumption here that when
    // the master already knows about this slave, the slave cannot
    // have tasks unknown to the master. This _should_ be the case
    // since the causal relationship is:
    //   slave removes task -> master removes task
    // We should enforce this via a CHECK (dangerous), or by shutting
    // down slaves that are found to violate this assumption.

    SlaveReregisteredMessage message;
    message.mutable_slave_id()->MergeFrom(slave->id);
    send(from, message);

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

    // If this is a disconnected slave, add it back to the allocator.
    // This is done after reconciliation to ensure the allocator's
    // offers include the recovered resources initially on this
    // slave.
    if (slave->disconnected) {
      slave->disconnected = false; // Reset the flag.
      allocator->slaveReconnected(slave->id);
    }

    // Inform the slave of the new framework pids for its tasks.
    __reregisterSlave(slave, tasks);

    return;
  }

  // Ensure we don't remove the slave for not re-registering after
  // we've recovered it from the registry.
  slaves.recovered.erase(slaveInfo.id());

  // If we're already re-registering this slave, then no need to ask
  // the registrar again.
  if (slaves.reregistering.contains(slaveInfo.id())) {
    LOG(INFO)
      << "Ignoring re-register slave message from slave "
      << slaveInfo.id() << " at " << from << " ("
      << slaveInfo.hostname() << ") as readmission is already in progress";
    return;
  }

  LOG(INFO) << "Re-registering slave " << slaveInfo.id() << " at " << from
            << " (" << slaveInfo.hostname() << ")";

  slaves.reregistering.insert(slaveInfo.id());

  // This handles the case when the slave tries to re-register with
  // a failed over master, in which case we must consult the
  // registrar.
  registrar->apply(Owned<Operation>(new ReadmitSlave(slaveInfo)))
    .onAny(defer(self(),
           &Self::_reregisterSlave,
           slaveInfo,
           from,
           executorInfos,
           tasks,
           completedFrameworks,
           lambda::_1));
}


void Master::_reregisterSlave(
    const SlaveInfo& slaveInfo,
    const UPID& pid,
    const vector<ExecutorInfo>& executorInfos,
    const vector<Task>& tasks,
    const vector<Archive::Framework>& completedFrameworks,
    const Future<bool>& readmit)
{
  slaves.reregistering.erase(slaveInfo.id());

  CHECK(!readmit.isDiscarded());

  if (readmit.isFailed()) {
    LOG(FATAL) << "Failed to readmit slave " << slaveInfo.id() << " at " << pid
               << " (" << slaveInfo.hostname() << "): " << readmit.failure();
  } else if (!readmit.get()) {
    LOG(WARNING) << "The slave " << slaveInfo.id() << " at "
                 << pid << " (" << slaveInfo.hostname() << ") could not be"
                 << " readmitted; shutting it down";
    slaves.deactivated.put(slaveInfo.id(), Nothing());

    ShutdownMessage message;
    message.set_message(
        "Slave attempted to re-register with unknown slave id " +
        stringify(slaveInfo.id()));
    send(pid, message);
  } else {
    // Re-admission succeeded.
    Slave* slave = new Slave(slaveInfo, slaveInfo.id(), pid, Clock::now());
    slave->reregisteredTime = Clock::now();

    LOG(INFO) << "Re-registered slave " << *slave;
    ++metrics.slave_reregistrations;

    readdSlave(slave, executorInfos, tasks, completedFrameworks);

    __reregisterSlave(slave, tasks);
  }
}


void Master::__reregisterSlave(Slave* slave, const vector<Task>& tasks)
{
  // Send the latest framework pids to the slave.
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


void Master::unregisterSlave(const SlaveID& slaveId)
{
  ++metrics.messages_unregister_slave;

  LOG(INFO) << "Asked to unregister slave " << slaveId;

  // TODO(benh): Check that only the slave is asking to unregister?
  if (slaves.activated.contains(slaveId)) {
    removeSlave(slaves.activated[slaveId]);
  }
}



// NOTE: We cannot use 'from' here to identify the slave as this is
// now sent by the StatusUpdateManagerProcess and master itself when
// it generates TASK_LOST messages. Only 'pid' can be used to identify
// the slave.
void Master::statusUpdate(const StatusUpdate& update, const UPID& pid)
{
  ++metrics.messages_status_update;

  if (slaves.deactivated.get(update.slave_id()).isSome()) {
    // If the slave is deactivated, we have already informed
    // frameworks that its tasks were LOST, so the slave should
    // shut down.
    LOG(WARNING) << "Ignoring status update " << update
                 << " from deactivated slave " << pid
                 << " with id " << update.slave_id() << " ; asking slave "
                 << " to shutdown";

    ShutdownMessage message;
    message.set_message("Status update from unknown slave");
    send(pid, message);

    stats.invalidStatusUpdates++;
    metrics.invalid_status_updates++;
    return;
  }

  if (!slaves.activated.contains(update.slave_id())) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " from unknown slave " << pid
                 << " with id " << update.slave_id();
    stats.invalidStatusUpdates++;
    metrics.invalid_status_updates++;
    return;
  }

  Slave* slave = CHECK_NOTNULL(slaves.activated[update.slave_id()]);

  // Forward the update to the framework.
  Try<Nothing> _forward = forward(update, pid);
  if (_forward.isError()) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " from slave " << *slave << ": " << _forward.error();
    stats.invalidStatusUpdates++;
    metrics.invalid_status_updates++;
    return;
  }

  // Lookup the task and see if we need to update anything locally.
  const TaskStatus& status = update.status();
  Task* task = slave->getTask(update.framework_id(), status.task_id());
  if (task == NULL) {
    LOG(WARNING) << "Could not lookup task for status update " << update
                 << " from slave " << *slave;
    stats.invalidStatusUpdates++;
    metrics.invalid_status_updates++;
    return;
  }

  LOG(INFO) << "Status update " << update << " from slave " << *slave;

  // TODO(brenden) Consider wiping the `data` and `message` fields?
  if (task->statuses_size() > 0 &&
      task->statuses(task->statuses_size() - 1).state() == status.state()) {
    task->mutable_statuses()->RemoveLast();
  }
  task->add_statuses()->CopyFrom(status);
  task->set_state(status.state());

  // Handle the task appropriately if it's terminated.
  if (protobuf::isTerminalState(status.state())) {
    removeTask(task);
  }

  stats.tasks[status.state()]++;
  stats.validStatusUpdates++;
  metrics.valid_status_updates++;
}


Try<Nothing> Master::forward(const StatusUpdate& update, const UPID& pid)
{
  Framework* framework = getFramework(update.framework_id());
  if (framework == NULL) {
    return Error("Unknown framework " + stringify(update.framework_id()));
  }

  // Pass on the (transformed) status update to the framework.
  StatusUpdateMessage message;
  message.mutable_update()->MergeFrom(update);
  message.set_pid(pid);
  send(framework->pid, message);
  return Nothing();
}


void Master::exitedExecutor(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    int32_t status)
{
  ++metrics.messages_exited_executor;

  if (slaves.deactivated.get(slaveId).isSome()) {
    // If the slave is deactivated, we have already informed
    // frameworks that its tasks were LOST, so the slave should
    // shut down.
    LOG(WARNING) << "Ignoring exited executor '" << executorId
                 << "' of framework " << frameworkId
                 << " on deactivated slave " << slaveId
                 << " ; asking slave to shutdown";

    ShutdownMessage message;
    message.set_message("Executor exited message from unknown slave");
    reply(message);
    return;
  }

  // Only update master's internal data structures here for proper
  // accounting. The TASK_LOST updates are handled by the slave.
  if (!slaves.activated.contains(slaveId)) {
    LOG(WARNING) << "Ignoring exited executor '" << executorId
                 << "' of framework " << frameworkId
                 << " on unknown slave " << slaveId;
    return;
  }

  Slave* slave = CHECK_NOTNULL(slaves.activated[slaveId]);

  // Tell the allocator about the recovered resources.
  if (slave->hasExecutor(frameworkId, executorId)) {
    ExecutorInfo executor = slave->executors[frameworkId][executorId];

    LOG(INFO) << "Executor " << executorId
              << " of framework " << frameworkId
              << " on slave " << *slave
              << (WIFEXITED(status) ? " has exited with status "
                                     : " has terminated with signal ")
              << (WIFEXITED(status) ? stringify(WEXITSTATUS(status))
                                     : strsignal(WTERMSIG(status)));

    allocator->resourcesRecovered(
        frameworkId, slaveId, Resources(executor.resources()));

    // Remove executor from slave and framework.
    slave->removeExecutor(frameworkId, executorId);
  } else {
    LOG(WARNING) << "Ignoring unknown exited executor "
                 << executorId << " on slave " << *slave;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    framework->removeExecutor(slave->id, executorId);

    // TODO(benh): Send the framework its executor's exit status?
    // Or maybe at least have something like
    // Scheduler::executorLost?
  }
}


void Master::shutdownSlave(const SlaveID& slaveId, const string& message)
{
  if (!slaves.activated.contains(slaveId)) {
    // Possible when the SlaveObserver dispatched to shutdown a slave,
    // but exited() was already called for this slave.
    LOG(WARNING) << "Unable to shutdown unknown slave " << slaveId;
    return;
  }

  Slave* slave = slaves.activated[slaveId];
  CHECK_NOTNULL(slave);

  LOG(WARNING) << "Shutting down slave " << *slave << " with message '"
               << message << "'";

  ShutdownMessage message_;
  message_.set_message(message);
  send(slave->pid, message_);

  removeSlave(slave);
}


void Master::reconcileTasks(
    const UPID& from,
    const FrameworkID& frameworkId,
    const std::vector<TaskStatus>& statuses)
{
  ++metrics.messages_reconcile_tasks;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Unknown framework " << frameworkId << " at " << from
                 << " attempted to reconcile tasks";
    return;
  }

  LOG(INFO) << "Performing task state reconciliation for " << statuses.size()
            << " task statuses of framework " << frameworkId;

  // Reconciliation occurs for the following cases:
  //   (1) If the slave is unknown, we send TASK_LOST.
  //   (2) If the task is missing on the slave, we send TASK_LOST.
  //   (3) If the task state differs, we send the latest state.
  //
  // (1) is applicable only when operating with a strict registry.
  foreach (const TaskStatus& status, statuses) {
    if (!status.has_slave_id()) {
      LOG(WARNING) << "Status for task " << status.task_id()
                   << " from framework " << frameworkId
                   << " does not include slave id; cannot reconcile";
      continue;
    }

    Option<StatusUpdate> update;

    // Check for a removed slave (case 1).
    if (flags.registry_strict &&
        !slaves.recovered.contains(status.slave_id()) &&
        !slaves.reregistering.contains(status.slave_id()) &&
        !slaves.activated.contains(status.slave_id()) &&
        !slaves.removing.contains(status.slave_id())) {
      // Slave is unknown or removed!
      update = protobuf::createStatusUpdate(
          frameworkId,
          status.slave_id(),
          status.task_id(),
          TASK_LOST,
          "Reconciliation: Slave is unknown/removed");
    }

    // Check for a known slave / task (cases (2) and (3)).
    if (slaves.activated.contains(status.slave_id())) {
      Slave* slave = CHECK_NOTNULL(slaves.activated[status.slave_id()]);
      Task* task = slave->getTask(frameworkId, status.task_id());

      if (task == NULL) {
        // Case (2).
        // TODO(bmahler): Leverage completed tasks if we track these
        // in the future.
        update = protobuf::createStatusUpdate(
            frameworkId,
            status.slave_id(),
            status.task_id(),
            TASK_LOST,
            "Reconciliation: Task is unknown to the slave");
      } else if (task->state() != status.state()) {
        // Case (3).
        update = protobuf::createStatusUpdate(
            frameworkId,
            task->slave_id(),
            task->task_id(),
            task->state(),
            "Reconciliation: Task state changed");
      }
    }

    if (update.isSome()) {
      CHECK_NOTNULL(framework);
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update.get());
      send(framework->pid, message);
    }
  }
}


void Master::frameworkFailoverTimeout(const FrameworkID& frameworkId,
                                      const Time& reregisteredTime)
{
  Framework* framework = getFramework(frameworkId);

  if (framework != NULL && !framework->active) {
    // If the re-registration time has not changed, then the framework
    // has not re-registered within the failover timeout.
    if (framework->reregisteredTime == reregisteredTime) {
      LOG(INFO) << "Framework failover timeout, removing framework "
                << framework->id;
      removeFramework(framework);
    }
  }
}


void Master::offer(const FrameworkID& frameworkId,
                   const hashmap<SlaveID, Resources>& resources)
{
  if (!frameworks.activated.contains(frameworkId) ||
      !frameworks.activated[frameworkId]->active) {
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

  Framework* framework = frameworks.activated[frameworkId];
  foreachpair (const SlaveID& slaveId, const Resources& offered, resources) {
    if (!slaves.activated.contains(slaveId)) {
      LOG(WARNING) << "Master returning resources offered to framework "
                   << frameworkId << " because slave " << slaveId
                   << " is not valid";

      allocator->resourcesRecovered(frameworkId, slaveId, offered);
      continue;
    }

    Slave* slave = slaves.activated[slaveId];

    CHECK(slave->info.checkpoint() || !framework->info.checkpoint())
        << "Resources of non checkpointing slave " << *slave
        << " are being offered to checkpointing framework " << frameworkId;

    // This could happen if the allocator dispatched 'Master::offer' before
    // it received 'Allocator::slaveRemoved' from the master.
    if (slave->disconnected) {
      LOG(WARNING) << "Master returning resources offered because slave "
                   << *slave << " is disconnected";

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


// TODO(vinod): If due to network partition there are two instances
// of the framework that think they are leaders and try to
// authenticate with master they would be stepping on each other's
// toes. Currently it is tricky to detect this case because the
// 'authenticate' message doesn't contain the 'FrameworkID'.
void Master::authenticate(const UPID& from, const UPID& pid)
{
  ++metrics.messages_authenticate;

  // Deactivate the framework/slave if it's already registered.
  // TODO(adam-mesos): MESOS-1081: Do not deactivate the current
  // framework/slave before we find out if the new one is legit.
  bool found = false;
  foreachvalue (Framework* framework, frameworks.activated) {
    if (framework->pid == pid) {
      deactivate(framework);
      found = true;
      break;
    }
  }
  if (!found) {
    foreachvalue (Slave* slave, slaves.activated) {
      if (slave->pid == pid) {
        disconnect(slave);
        break;
      }
    }
  }

  authenticated.erase(pid);

  if (authenticating.contains(pid)) {
    LOG(INFO) << "Queuing up authentication request from " << pid
              << " because authentication is still in progress";

    // Try to cancel the in progress authentication by deleting
    // the authenticator.
    authenticators.erase(pid);

    // Retry after the current authenticator finishes.
    authenticating[pid]
      .onAny(defer(self(), &Self::authenticate, from, pid));

    return;
  }

  LOG(INFO) << "Authenticating " << pid;

  // Create a promise to capture the entire "authenticating"
  // procedure. We'll set this _after_ we finish _authenticate.
  Owned<Promise<Nothing> > promise(new Promise<Nothing>());

  // Create the authenticator.
  Owned<sasl::Authenticator> authenticator(new sasl::Authenticator(from));

  // Start authentication.
  const Future<Option<string> >& future = authenticator->authenticate()
    .onAny(defer(self(), &Self::_authenticate, pid, promise, lambda::_1));

  // Don't wait for authentication to happen for ever.
  delay(Seconds(5),
        self(),
        &Self::authenticationTimeout,
        future);

  // Save our state.
  authenticating[pid] = promise->future();
  authenticators.put(pid, authenticator);
}


void Master::_authenticate(
    const UPID& pid,
    const Owned<Promise<Nothing> >& promise,
    const Future<Option<string> >& future)
{
  if (!future.isReady() || future.get().isNone()) {
    const string& error = future.isReady()
        ? "Refused authentication"
        : (future.isFailed() ? future.failure() : "future discarded");

    LOG(WARNING) << "Failed to authenticate " << pid
                 << ": " << error;

    promise->fail(error);
  } else {
    LOG(INFO) << "Successfully authenticated principal '" << future.get().get()
              << "' at " << pid;

    promise->set(Nothing());
    authenticated.put(pid, future.get().get());
  }

  authenticators.erase(pid);
  authenticating.erase(pid);
}


void Master::authenticationTimeout(Future<Option<string> > future)
{
  // Note that a 'discard' here is safe even if another
  // authenticator is in progress because this copy of the future
  // corresponds to the original authenticator that started the timer.
  if (future.discard()) { // This is a no-op if the future is already ready.
    LOG(WARNING) << "Authentication timed out";
  }
}


// Return connected frameworks that are not in the process of being removed
vector<Framework*> Master::getActiveFrameworks() const
{
  vector <Framework*> result;
  foreachvalue (Framework* framework, frameworks.activated) {
    if (framework->active) {
      result.push_back(framework);
    }
  }
  return result;
}


Resources Master::launchTask(const TaskInfo& task,
                             Framework* framework,
                             Slave* slave)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  Resources resources; // Total resources used on slave by launching this task.

  // Determine if this task launches an executor, and if so make sure
  // the slave and framework state has been updated accordingly.
  Option<ExecutorID> executorId;

  if (task.has_executor()) {
    // TODO(benh): Refactor this code into Slave::addTask.
    if (!slave->hasExecutor(framework->id, task.executor().executor_id())) {
      CHECK(!framework->hasExecutor(slave->id, task.executor().executor_id()))
        << "Executor " << task.executor().executor_id()
        << " known to the framework " << framework->id
        << " but unknown to the slave " << *slave;

      slave->addExecutor(framework->id, task.executor());
      framework->addExecutor(slave->id, task.executor());
      resources += task.executor().resources();
    }

    executorId = task.executor().executor_id();
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
            << " with resources " << task.resources()
            << " on slave " << *slave;

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
  // NOTE: keys() and values() are used since statusUpdate()
  //       modifies slave->tasks.
  foreach (const FrameworkID& frameworkId, slave->tasks.keys()) {
    foreach (Task* task, slave->tasks[frameworkId].values()) {
      if (!slaveTasks.contains(task->framework_id(), task->task_id())) {
        LOG(WARNING) << "Sending TASK_LOST for task " << task->task_id()
                     << " of framework " << task->framework_id()
                     << " unknown to the slave " << *slave;

        const StatusUpdate& update = protobuf::createStatusUpdate(
            task->framework_id(),
            slave->id,
            task->task_id(),
            TASK_LOST,
            "Task is unknown to the slave");

        statusUpdate(update, UPID());
      }
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
      LOG(ERROR) << "Slave " << *slave
                 << " re-registered with executor " << executor.executor_id()
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
                     << *slave;

        // TODO(bmahler): This is duplicated in several locations, we
        // may benefit from a method for removing an executor from
        // all the relevant data structures and the allocator, akin
        // to removeTask().
        allocator->resourcesRecovered(
            frameworkId,
            slave->id,
            slave->executors[frameworkId][executorId].resources());

        slave->removeExecutor(frameworkId, executorId);

        if (frameworks.activated.contains(frameworkId)) {
          frameworks.activated[frameworkId]->removeExecutor(
              slave->id, executorId);
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
      LOG(WARNING) << " Slave " << *slave
                   << " has non-terminal task " << task.task_id()
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
  // when the master fails over. Also, we only store a limited number
  // of completed frameworks.
  // TODO(vinod): Revisit this when registrar is in place. It would
  // likely involve storing this information in the registrar.
  foreach (const shared_ptr<Framework>& framework, frameworks.completed) {
    if (slaveTasks.contains(framework->id)) {
      LOG(WARNING)
        << "Slave " << *slave
        << " re-registered with completed framework " << framework->id
        << ". Shutting down the framework on the slave";

      ShutdownFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      send(slave->pid, message);
    }
  }
}


void Master::addFramework(Framework* framework)
{
  CHECK(!frameworks.activated.contains(framework->id))
    << "Framework " << framework->id << "already exists!";

  frameworks.activated[framework->id] = framework;

  link(framework->pid);

  // Enforced by Master::registerFramework.
  CHECK(roles.contains(framework->info.role()))
    << "Unknown role " << framework->info.role()
    << " of framework " << framework->id;

  roles[framework->info.role()]->addFramework(framework);

  FrameworkRegisteredMessage message;
  message.mutable_framework_id()->MergeFrom(framework->id);
  message.mutable_master_info()->MergeFrom(info_);
  send(framework->pid, message);

  allocator->frameworkAdded(
      framework->id, framework->info, framework->resources);
}


// Replace the scheduler for a framework with a new process ID, in the
// event of a scheduler failover.
void Master::failoverFramework(Framework* framework, const UPID& newPid)
{
  const UPID& oldPid = framework->pid;

  // There are a few failover cases to consider:
  //   1. The pid has changed. In this case we definitely want to
  //      send a FrameworkErrorMessage to shut down the older
  //      scheduler.
  //   2. The pid has not changed.
  //      2.1 The old scheduler on that pid failed over to a new
  //          instance on the same pid. No need to shut down the old
  //          scheduler as it is necessarily dead.
  //      2.2 This is a duplicate message. In this case, the scheduler
  //          has not failed over, so we do not want to shut it down.
  if (oldPid != newPid) {
    FrameworkErrorMessage message;
    message.set_message("Framework failed over");
    send(oldPid, message);
  }

  // TODO(benh): unlink(oldPid);

  framework->pid = newPid;
  link(newPid);

  // Make sure we can get offers again.
  // TODO(vinod): Do this after we recover resources below.
  if (!framework->active) {
    framework->active = true;
    allocator->frameworkActivated(framework->id, framework->info);
  }

  // The scheduler driver safely ignores any duplicate registration
  // messages, so we don't need to compare the old and new pids here.
  {
    FrameworkRegisteredMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id);
    message.mutable_master_info()->MergeFrom(info_);
    send(newPid, message);
  }

  // Remove the framework's offers (if they weren't removed before).
  // We do this after we have updated the pid and sent the framework
  // registered message so that the allocator can immediately re-offer
  // these resources to this framework if it wants.
  foreach (Offer* offer, utils::copy(framework->offers)) {
    allocator->resourcesRecovered(
        offer->framework_id(), offer->slave_id(), offer->resources());
    removeOffer(offer);
  }
}


void Master::removeFramework(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Removing framework " << framework->id;

  if (framework->active) {
    // Tell the allocator to stop allocating resources to this framework.
    // TODO(vinod): Consider setting  framework->active to false here
    // or just calling 'deactivate(Framework*)'.
    allocator->frameworkDeactivated(framework->id);
  }

  // Tell slaves to shutdown the framework.
  foreachvalue (Slave* slave, slaves.activated) {
    ShutdownFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id);
    send(slave->pid, message);
  }

  // Remove pointers to the framework's tasks in slaves.
  foreachvalue (Task* task, utils::copy(framework->tasks)) {
    Slave* slave = getSlave(task->slave_id());
    // Since we only find out about tasks when the slave re-registers,
    // it must be the case that the slave exists!
    CHECK(slave != NULL)
      << "Unknown slave " << task->slave_id()
      << " for task " << task->task_id();

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
  frameworks.completed.push_back(shared_ptr<Framework>(framework));

  CHECK(roles.contains(framework->info.role()))
    << "Unknown role " << framework->info.role()
    << " of framework " << framework->id;

  roles[framework->info.role()]->removeFramework(framework);

  // Remove the framework from authenticated.
  authenticated.erase(framework->pid);

  // Remove it.
  frameworks.activated.erase(framework->id);
  allocator->frameworkRemoved(framework->id);
}


void Master::removeFramework(Slave* slave, Framework* framework)
{
  CHECK_NOTNULL(slave);
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Removing framework " << framework->id
            << " from slave " << *slave;

  // Remove pointers to framework's tasks in slaves, and send status
  // updates.
  // NOTE: values() is used because statusUpdate() modifies
  //       slave->tasks.
  foreach (Task* task, slave->tasks[framework->id].values()) {
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
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Adding slave " << *slave
            << " with " << slave->info.resources();

  slaves.deactivated.erase(slave->id);
  slaves.activated[slave->id] = slave;

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

  // Set up an observer for the slave.
  slave->observer = new SlaveObserver(
      slave->pid, slave->info, slave->id, self());

  spawn(slave->observer);

  if (!reregister) {
    allocator->slaveAdded(
        slave->id, slave->info, hashmap<FrameworkID, Resources>());
  }
}


void Master::readdSlave(
    Slave* slave,
    const vector<ExecutorInfo>& executorInfos,
    const vector<Task>& tasks,
    const vector<Archive::Framework>& completedFrameworks)
{
  CHECK_NOTNULL(slave);

  addSlave(slave, true);

  // Add the executors and tasks to the slave and framework state and
  // determine the resources that have been allocated to frameworks.
  hashmap<FrameworkID, Resources> resources;

  foreach (const ExecutorInfo& executorInfo, executorInfos) {
    // TODO(bmahler): ExecutorInfo.framework_id is set by the Scheduler
    // Driver in 0.14.0. Therefore, in 0.15.0, the slave no longer needs
    // to set it, and we could remove this CHECK if desired.
    CHECK(executorInfo.has_framework_id())
      << "Executor " << executorInfo.executor_id()
      << " doesn't have frameworkId set";

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
                   << " running on slave " << *slave;
    }

    resources[task.framework_id()] += task.resources();
  }

  // Re-add completed tasks reported by the slave.
  // Note that a slave considers a framework completed when it has no
  // tasks/executors running for that framework. But a master considers a
  // framework completed when the framework is removed after a failover timeout.
  // TODO(vinod): Reconcile the notion of a completed framework across the
  // master and slave.
  foreach (const Archive::Framework& completedFramework, completedFrameworks) {
    const FrameworkID& frameworkId = completedFramework.framework_info().id();
    Framework* framework = getFramework(frameworkId);
    foreach (const Task& task, completedFramework.tasks()) {
      if (framework != NULL) {
        VLOG(2) << "Re-adding completed task " << task.task_id()
                << " of framework " << task.framework_id()
                << " that ran on slave " << *slave;
        framework->addCompletedTask(task);
      } else {
        // We could be here if the framework hasn't registered yet.
        // TODO(vinod): Revisit these semantics when we store frameworks'
        // information in the registrar.
        LOG(WARNING) << "Possibly orphaned completed task " << task.task_id()
                     << " of framework " << task.framework_id()
                     << " that ran on slave " << *slave;
      }
    }
  }

  allocator->slaveAdded(slave->id, slave->info, resources);
}


void Master::removeSlave(Slave* slave)
{
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Removing slave " << *slave;

  // We do this first, to make sure any of the resources recovered
  // below (e.g., removeTask()) are ignored by the allocator.
  allocator->slaveRemoved(slave->id);

  // Transition the tasks to lost and remove them, BUT do not send
  // updates. Rather, build up the updates so that we can send them
  // after the slave is removed from the registry.
  vector<StatusUpdate> updates;
  foreach (const FrameworkID& frameworkId, slave->tasks.keys()) {
    foreach (Task* task, slave->tasks[frameworkId].values()) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          task->framework_id(),
          task->slave_id(),
          task->task_id(),
          TASK_LOST,
          "Slave " + slave->info.hostname() + " removed",
          (task->has_executor_id() ?
              Option<ExecutorID>(task->executor_id()) : None()));

      task->add_statuses()->CopyFrom(update.status());
      task->set_state(update.status().state());
      removeTask(task);

      updates.push_back(update);
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

  // Remove executors from the slave for proper resource accounting.
  foreachkey (const FrameworkID& frameworkId, slave->executors) {
    Framework* framework = getFramework(frameworkId);
    if (framework != NULL) {
      foreachkey (const ExecutorID& executorId, slave->executors[frameworkId]) {
        // TODO(vinod): We don't need to call 'Allocator::resourcesRecovered'
        // once MESOS-621 is fixed.
        allocator->resourcesRecovered(
            frameworkId,
            slave->id,
            slave->executors[frameworkId][executorId].resources());

        framework->removeExecutor(slave->id, executorId);
      }
    }
  }

  // Mark the slave as being removed.
  slaves.removing.insert(slave->id);
  slaves.activated.erase(slave->id);
  slaves.deactivated.put(slave->id, Nothing());
  authenticated.erase(slave->pid);

  // Kill the slave observer.
  terminate(slave->observer);
  wait(slave->observer);
  delete slave->observer;

  // TODO(benh): unlink(slave->pid);

  // Remove this slave from the registrar. Once this is completed, we
  // can forward the LOST task updates to the frameworks and notify
  // all frameworks that this slave was lost.
  registrar->apply(Owned<Operation>(new RemoveSlave(slave->info)))
    .onAny(defer(self(),
                 &Self::_removeSlave,
                 slave->info,
                 updates,
                 lambda::_1));

  delete slave;
}


void Master::_removeSlave(
    const SlaveInfo& slaveInfo,
    const vector<StatusUpdate>& updates,
    const Future<bool>& removed)
{
  slaves.removing.erase(slaveInfo.id());

  CHECK(!removed.isDiscarded());

  if (removed.isFailed()) {
    LOG(FATAL) << "Failed to remove slave " << slaveInfo.id()
               << " (" << slaveInfo.hostname() << ")"
               << " from the registrar: " << removed.failure();
  }

  CHECK(removed.get())
    << "Slave " << slaveInfo.id() << " (" << slaveInfo.hostname() << ") "
    << "already removed from the registrar";

  LOG(INFO) << "Removed slave " << slaveInfo.id() << " ("
            << slaveInfo.hostname() << ")";
  ++metrics.slave_removals;

  // Forward the LOST updates on to the framework.
  foreach (const StatusUpdate& update, updates) {
    Try<Nothing> _forward = forward(update, UPID());
    if (_forward.isError()) {
      LOG(WARNING) << "Unable to send update " << update << " on to framework "
                   << update.framework_id() << ": " << _forward.error();
    }
  }

  // Notify all frameworks of the lost slave.
  foreachvalue (Framework* framework, frameworks.activated) {
    LOG(INFO) << "Notifying framework " << framework->id << " of lost slave "
              << slaveInfo.id() << " (" << slaveInfo.hostname() << ") "
              << "after recovering";
    LostSlaveMessage message;
    message.mutable_slave_id()->MergeFrom(slaveInfo.id());
    send(framework->pid, message);
  }
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

  // Update the task state metric.
  switch (task->state()) {
    case TASK_FINISHED: ++metrics.tasks_finished; break;
    case TASK_FAILED:   ++metrics.tasks_failed;   break;
    case TASK_KILLED:   ++metrics.tasks_killed;   break;
    case TASK_LOST:     ++metrics.tasks_lost;     break;
    default:
      LOG(WARNING) << "Removing task " << task->task_id()
                   << " of framework " << task->framework_id()
                   << " and slave " << task->slave_id()
                   << " in non-terminal state " << task->state();
      break;
  }

  delete task;
}


// TODO(vinod): Instead of 'removeOffer()', consider implementing
// 'useOffer()', 'discardOffer()' and 'rescindOffer()' for clarity.
void Master::removeOffer(Offer* offer, bool rescind)
{
  // Remove from framework.
  Framework* framework = getFramework(offer->framework_id());
  CHECK(framework != NULL)
    << "Unknown framework " << offer->framework_id()
    << " in the offer " << offer->id();

  framework->removeOffer(offer);

  // Remove from slave.
  Slave* slave = getSlave(offer->slave_id());
  CHECK(slave != NULL)
    << "Unknown slave " << offer->slave_id()
    << " in the offer " << offer->id();

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


// TODO(bmahler): Consider killing this.
Framework* Master::getFramework(const FrameworkID& frameworkId)
{
  return frameworks.activated.contains(frameworkId)
    ? frameworks.activated[frameworkId]
    : NULL;
}


// TODO(bmahler): Consider killing this.
Slave* Master::getSlave(const SlaveID& slaveId)
{
  return slaves.activated.contains(slaveId)
    ? slaves.activated[slaveId]
    : NULL;
}


// TODO(bmahler): Consider killing this.
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

  out << info_.id() << "-" << std::setw(4)
      << std::setfill('0') << nextFrameworkId++;

  FrameworkID frameworkId;
  frameworkId.set_value(out.str());

  return frameworkId;
}


OfferID Master::newOfferId()
{
  OfferID offerId;
  offerId.set_value(info_.id() + "-" + stringify(nextOfferId++));
  return offerId;
}


SlaveID Master::newSlaveId()
{
  SlaveID slaveId;
  slaveId.set_value(info_.id() + "-" + stringify(nextSlaveId++));
  return slaveId;
}


double Master::_slaves_active()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.activated) {
    if (!slave->disconnected) {
      count++;
    }
  }
  return count;
}


double Master::_slaves_inactive()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.activated) {
    if (slave->disconnected) {
      count++;
    }
  }
  return count;
}


double Master::_tasks_staging()
{
  double count = 0.0;

  foreachvalue (Slave* slave, slaves.activated) {
    typedef hashmap<TaskID, Task*> TaskMap;
    foreachvalue (const TaskMap& tasks, slave->tasks) {
      foreachvalue (const Task* task, tasks) {
        if (task->state() == TASK_STAGING) {
          count++;
        }
      }
    }
  }

  return count;
}


double Master::_tasks_starting()
{
  double count = 0.0;

  foreachvalue (Slave* slave, slaves.activated) {
    typedef hashmap<TaskID, Task*> TaskMap;
    foreachvalue (const TaskMap& tasks, slave->tasks) {
      foreachvalue (const Task* task, tasks) {
        if (task->state() == TASK_STARTING) {
          count++;
        }
      }
    }
  }

  return count;
}


double Master::_tasks_running()
{
  double count = 0.0;

  foreachvalue (Slave* slave, slaves.activated) {
    typedef hashmap<TaskID, Task*> TaskMap;
    foreachvalue (const TaskMap& tasks, slave->tasks) {
      foreachvalue (const Task* task, tasks) {
        if (task->state() == TASK_RUNNING) {
          count++;
        }
      }
    }
  }

  return count;
}


// TODO(dhamon): Consider moving to master/metrics.cpp|hpp.
// Message counters are named with "messages_" prefix so they can
// be grouped together alphabetically in the output.
Master::Metrics::Metrics(const Master& master)
  : uptime_secs(
        "master/uptime_secs",
        defer(master, &Master::_uptime_secs)),
    elected(
        "master/elected",
        defer(master, &Master::_elected)),
    slaves_active(
        "master/slaves_active",
        defer(master, &Master::_slaves_active)),
    slaves_inactive(
        "master/slaves_inactive",
        defer(master, &Master::_slaves_inactive)),
    frameworks_active(
        "master/frameworks_active",
        defer(master, &Master::_frameworks_active)),
    frameworks_inactive(
        "master/frameworks_inactive",
        defer(master, &Master::_frameworks_inactive)),
    outstanding_offers(
        "master/outstanding_offers",
        defer(master, &Master::_outstanding_offers)),
    tasks_staging(
        "master/tasks_staging",
        defer(master, &Master::_tasks_staging)),
    tasks_starting(
        "master/tasks_starting",
        defer(master, &Master::_tasks_starting)),
    tasks_running(
        "master/tasks_running",
        defer(master, &Master::_tasks_running)),
    tasks_finished(
        "master/tasks_finished"),
    tasks_failed(
        "master/tasks_failed"),
    tasks_killed(
        "master/tasks_killed"),
    tasks_lost(
        "master/tasks_lost"),
    dropped_messages(
        "master/dropped_messages"),
    messages_register_framework(
        "master/messages_register_framework"),
    messages_reregister_framework(
        "master/messages_reregister_framework"),
    messages_unregister_framework(
        "master/messages_unregister_framework"),
    messages_deactivate_framework(
        "master/messages_deactivate_framework"),
    messages_kill_task(
        "master/messages_kill_task"),
    messages_status_update_acknowledgement(
        "master/messages_status_update_acknowledgement"),
    messages_resource_request(
        "master/messages_resource_request"),
    messages_launch_tasks(
        "master/messages_launch_tasks"),
    messages_revive_offers(
        "master/messages_revive_offers"),
    messages_reconcile_tasks(
        "master/messages_reconcile_tasks"),
    messages_framework_to_executor(
        "master/messages_framework_to_executor"),
    messages_register_slave(
        "master/messages_register_slave"),
    messages_reregister_slave(
        "master/messages_reregister_slave"),
    messages_unregister_slave(
        "master/messages_unregister_slave"),
    messages_status_update(
        "master/messages_status_update"),
    messages_exited_executor(
        "master/messages_exited_executor"),
    messages_authenticate(
        "master/messages_authenticate"),
    valid_framework_to_executor_messages(
        "master/valid_framework_to_executor_messages"),
    invalid_framework_to_executor_messages(
        "master/invalid_framework_to_executor_messages"),
    valid_status_updates(
        "master/valid_status_updates"),
    invalid_status_updates(
        "master/invalid_status_updates"),
    valid_status_update_acknowledgements(
        "master/valid_status_update_acknowledgements"),
    invalid_status_update_acknowledgements(
        "master/invalid_status_update_acknowledgements"),
    recovery_slave_removals(
        "master/recovery_slave_removals"),
    event_queue_size(
        "master/event_queue_size",
        defer(master, &Master::_event_queue_size)),
    slave_registrations(
        "master/slave_registrations"),
    slave_reregistrations(
        "master/slave_reregistrations"),
    slave_removals(
        "master/slave_removals")
{
  // TODO(dhamon): Check return values of 'add'.
  process::metrics::add(uptime_secs);
  process::metrics::add(elected);

  process::metrics::add(slaves_active);
  process::metrics::add(slaves_inactive);

  process::metrics::add(frameworks_active);
  process::metrics::add(frameworks_inactive);

  process::metrics::add(outstanding_offers);

  process::metrics::add(tasks_staging);
  process::metrics::add(tasks_starting);
  process::metrics::add(tasks_running);
  process::metrics::add(tasks_finished);
  process::metrics::add(tasks_failed);
  process::metrics::add(tasks_killed);
  process::metrics::add(tasks_lost);

  process::metrics::add(dropped_messages);

  // Messages from schedulers.
  process::metrics::add(messages_register_framework);
  process::metrics::add(messages_reregister_framework);
  process::metrics::add(messages_unregister_framework);
  process::metrics::add(messages_deactivate_framework);
  process::metrics::add(messages_kill_task);
  process::metrics::add(messages_status_update_acknowledgement);
  process::metrics::add(messages_resource_request);
  process::metrics::add(messages_launch_tasks);
  process::metrics::add(messages_revive_offers);
  process::metrics::add(messages_reconcile_tasks);
  process::metrics::add(messages_framework_to_executor);

  // Messages from slaves.
  process::metrics::add(messages_register_slave);
  process::metrics::add(messages_reregister_slave);
  process::metrics::add(messages_unregister_slave);
  process::metrics::add(messages_status_update);
  process::metrics::add(messages_exited_executor);

  // Messages from both schedulers and slaves.
  process::metrics::add(messages_authenticate);

  process::metrics::add(valid_framework_to_executor_messages);
  process::metrics::add(invalid_framework_to_executor_messages);

  process::metrics::add(valid_status_updates);
  process::metrics::add(invalid_status_updates);

  process::metrics::add(valid_status_update_acknowledgements);
  process::metrics::add(invalid_status_update_acknowledgements);

  process::metrics::add(recovery_slave_removals);

  process::metrics::add(event_queue_size);

  process::metrics::add(slave_registrations);
  process::metrics::add(slave_reregistrations);
  process::metrics::add(slave_removals);

  // Create resource gauges.
  // TODO(dhamon): Set these up dynamically when adding a slave based on the
  // resources the slave exposes.
  const string resources[] = {"cpus", "mem", "disk"};

  foreach (const string& resource, resources) {
    process::metrics::Gauge totalGauge(
        "master/" + resource + "_total",
        defer(master, &Master::_resources_total, resource));
    resources_total.push_back(totalGauge);
    process::metrics::add(totalGauge);

    process::metrics::Gauge usedGauge(
        "master/" + resource + "_used",
        defer(master, &Master::_resources_used, resource));
    resources_used.push_back(usedGauge);
    process::metrics::add(usedGauge);

    process::metrics::Gauge percentGauge(
        "master/" + resource + "_percent",
        defer(master, &Master::_resources_percent, resource));
    resources_percent.push_back(percentGauge);
    process::metrics::add(percentGauge);
  }
}


Master::Metrics::~Metrics()
{
  // TODO(dhamon): Check return values of 'remove'.
  process::metrics::remove(uptime_secs);
  process::metrics::remove(elected);

  process::metrics::remove(slaves_active);
  process::metrics::remove(slaves_inactive);

  process::metrics::remove(frameworks_active);
  process::metrics::remove(frameworks_inactive);

  process::metrics::remove(outstanding_offers);

  process::metrics::remove(tasks_staging);
  process::metrics::remove(tasks_starting);
  process::metrics::remove(tasks_running);
  process::metrics::remove(tasks_finished);
  process::metrics::remove(tasks_failed);
  process::metrics::remove(tasks_killed);
  process::metrics::remove(tasks_lost);

  process::metrics::remove(dropped_messages);

  // Messages from schedulers.
  process::metrics::remove(messages_register_framework);
  process::metrics::remove(messages_reregister_framework);
  process::metrics::remove(messages_unregister_framework);
  process::metrics::remove(messages_deactivate_framework);
  process::metrics::remove(messages_kill_task);
  process::metrics::remove(messages_status_update_acknowledgement);
  process::metrics::remove(messages_resource_request);
  process::metrics::remove(messages_launch_tasks);
  process::metrics::remove(messages_revive_offers);
  process::metrics::remove(messages_reconcile_tasks);
  process::metrics::remove(messages_framework_to_executor);

  // Messages from slaves.
  process::metrics::remove(messages_register_slave);
  process::metrics::remove(messages_reregister_slave);
  process::metrics::remove(messages_unregister_slave);
  process::metrics::remove(messages_status_update);
  process::metrics::remove(messages_exited_executor);

  // Messages from both schedulers and slaves.
  process::metrics::remove(messages_authenticate);

  process::metrics::remove(valid_framework_to_executor_messages);
  process::metrics::remove(invalid_framework_to_executor_messages);

  process::metrics::remove(valid_status_updates);
  process::metrics::remove(invalid_status_updates);

  process::metrics::remove(valid_status_update_acknowledgements);
  process::metrics::remove(invalid_status_update_acknowledgements);

  process::metrics::remove(recovery_slave_removals);

  process::metrics::remove(event_queue_size);

  process::metrics::remove(slave_registrations);
  process::metrics::remove(slave_reregistrations);
  process::metrics::remove(slave_removals);

  foreach (const process::metrics::Gauge& gauge, resources_total) {
    process::metrics::remove(gauge);
  }
  resources_total.clear();

  foreach (const process::metrics::Gauge& gauge, resources_used) {
    process::metrics::remove(gauge);
  }
  resources_used.clear();

  foreach (const process::metrics::Gauge& gauge, resources_percent) {
    process::metrics::remove(gauge);
  }
  resources_percent.clear();
}


double Master::_resources_total(const std::string& name)
{
  double total = 0.0;

  foreachvalue (Slave* slave, slaves.activated) {
    foreach (const Resource& resource, slave->info.resources()) {
      if (resource.name() == name && resource.type() == Value::SCALAR) {
        total += resource.scalar().value();
      }
    }
  }

  return total;
}


double Master::_resources_used(const std::string& name)
{
  double used = 0.0;

  foreachvalue (Slave* slave, slaves.activated) {
    foreach (const Resource& resource, slave->resourcesInUse) {
      if (resource.name() == name && resource.type() == Value::SCALAR) {
        used += resource.scalar().value();
      }
    }
  }

  return used;
}

double Master::_resources_percent(const std::string& name)
{
  double total = _resources_total(name);

  if (total == 0.0) {
    return total;
  } else {
    return _resources_used(name) / total;
  }
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
