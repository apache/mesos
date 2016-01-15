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
#include <memory>
#include <sstream>

#include <mesos/module.hpp>

#include <mesos/authentication/authenticator.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/master/allocator.hpp>

#include <mesos/module/authenticator.hpp>

#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/limiter.hpp>
#include <process/owned.hpp>
#include <process/run.hpp>
#include <process/shared.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/ip.hpp>
#include <stout/lambda.hpp>
#include <stout/multihashmap.hpp>
#include <stout/net.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "authentication/cram_md5/authenticator.hpp"

#include "common/build.hpp"
#include "common/date_utils.hpp"
#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "credentials/credentials.hpp"

#include "hook/manager.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"

#include "module/manager.hpp"

#include "watcher/whitelist_watcher.hpp"

using std::list;
using std::shared_ptr;
using std::string;
using std::vector;

using process::await;
using process::wait; // Necessary on some OS's to disambiguate.
using process::Clock;
using process::ExitedEvent;
using process::Failure;
using process::Future;
using process::MessageEvent;
using process::Owned;
using process::PID;
using process::Process;
using process::Promise;
using process::RateLimiter;
using process::Shared;
using process::Time;
using process::Timer;
using process::UPID;

using process::http::Pipe;

using process::metrics::Counter;

namespace mesos {
namespace internal {
namespace master {

using mesos::master::RoleInfo;
using mesos::master::allocator::Allocator;


class SlaveObserver : public ProtobufProcess<SlaveObserver>
{
public:
  SlaveObserver(const UPID& _slave,
                const SlaveInfo& _slaveInfo,
                const SlaveID& _slaveId,
                const PID<Master>& _master,
                const Option<shared_ptr<RateLimiter>>& _limiter,
                const shared_ptr<Metrics> _metrics,
                const Duration& _slavePingTimeout,
                const size_t _maxSlavePingTimeouts)
    : ProcessBase(process::ID::generate("slave-observer")),
      slave(_slave),
      slaveInfo(_slaveInfo),
      slaveId(_slaveId),
      master(_master),
      limiter(_limiter),
      metrics(_metrics),
      slavePingTimeout(_slavePingTimeout),
      maxSlavePingTimeouts(_maxSlavePingTimeouts),
      timeouts(0),
      pinged(false),
      connected(true)
  {
    // TODO(Wang Yong Qiao): For backwards compatibility, this handler is kept.
    // Suggest to remove this handler in 0.26.0.
    install("PONG", &SlaveObserver::pongOld);

    install<PongSlaveMessage>(&SlaveObserver::pong);
  }

  void reconnect()
  {
    connected = true;
  }

  void disconnect()
  {
    connected = false;
  }

protected:
  virtual void initialize()
  {
    ping();
  }

  void ping()
  {
    PingSlaveMessage message;
    message.set_connected(connected);
    send(slave, message);

    pinged = true;
    delay(slavePingTimeout, self(), &SlaveObserver::timeout);
  }

  void pongOld(const UPID& from, const string& body)
  {
    pong();
  }

  void pong()
  {
    timeouts = 0;
    pinged = false;

    // Cancel any pending shutdown.
    if (shuttingDown.isSome()) {
      // Need a copy for non-const access.
      Future<Nothing> future = shuttingDown.get();
      future.discard();
    }
  }

  void timeout()
  {
    if (pinged) {
      timeouts++; // No pong has been received before the timeout.
      if (timeouts >= maxSlavePingTimeouts) {
        // No pong has been received for the last
        // 'maxSlavePingTimeouts' pings.
        shutdown();
      }
    }

    // NOTE: We keep pinging even if we schedule a shutdown. This is
    // because if the slave eventually responds to a ping, we can
    // cancel the shutdown.
    ping();
  }

  // NOTE: The shutdown of the slave is rate limited and can be
  // canceled if a pong was received before the actual shutdown is
  // called.
  void shutdown()
  {
    if (shuttingDown.isSome()) {
      return;  // Shutdown is already in progress.
    }

    Future<Nothing> acquire = Nothing();

    if (limiter.isSome()) {
      LOG(INFO) << "Scheduling shutdown of slave " << slaveId
                << " due to health check timeout";

      acquire = limiter.get()->acquire();
    }

    shuttingDown = acquire.onAny(defer(self(), &Self::_shutdown));
    ++metrics->slave_shutdowns_scheduled;
  }

  void _shutdown()
  {
    CHECK_SOME(shuttingDown);

    const Future<Nothing>& future = shuttingDown.get();

    CHECK(!future.isFailed());

    if (future.isReady()) {
      LOG(INFO) << "Shutting down slave " << slaveId
                << " due to health check timeout";

      ++metrics->slave_shutdowns_completed;

      dispatch(master,
               &Master::shutdownSlave,
               slaveId,
               "health check timed out");
    } else if (future.isDiscarded()) {
      LOG(INFO) << "Canceling shutdown of slave " << slaveId
                << " since a pong is received!";

      ++metrics->slave_shutdowns_canceled;
    }

    shuttingDown = None();
  }

private:
  const UPID slave;
  const SlaveInfo slaveInfo;
  const SlaveID slaveId;
  const PID<Master> master;
  const Option<shared_ptr<RateLimiter>> limiter;
  shared_ptr<Metrics> metrics;
  Option<Future<Nothing>> shuttingDown;
  const Duration slavePingTimeout;
  const size_t maxSlavePingTimeouts;
  uint32_t timeouts;
  bool pinged;
  bool connected;
};


Master::Master(
    Allocator* _allocator,
    Registrar* _registrar,
    Repairer* _repairer,
    Files* _files,
    MasterContender* _contender,
    MasterDetector* _detector,
    const Option<Authorizer*>& _authorizer,
    const Option<shared_ptr<RateLimiter>>& _slaveRemovalLimiter,
    const Flags& _flags)
  : ProcessBase("master"),
    flags(_flags),
    allocator(_allocator),
    registrar(_registrar),
    repairer(_repairer),
    files(_files),
    contender(_contender),
    detector(_detector),
    authorizer(_authorizer),
    frameworks(flags),
    authenticator(None()),
    metrics(new Metrics(*this)),
    electedTime(None())
{
  slaves.limiter = _slaveRemovalLimiter;

  // NOTE: We populate 'info_' here instead of inside 'initialize()'
  // because 'StandaloneMasterDetector' needs access to the info.

  // Master ID is generated randomly based on UUID.
  info_.set_id(UUID::random().toString());

  // NOTE: Currently, we store ip in MasterInfo in network order,
  // which should be fixed. See MESOS-1201 for details.
  // TODO(marco): The ip, port, hostname fields above are
  //     being deprecated; the code should be removed once
  //     the deprecation cycle is complete.
  info_.set_ip(self().address.ip.in().get().s_addr);

  info_.set_port(self().address.port);
  info_.set_pid(self());
  info_.set_version(MESOS_VERSION);

  // Determine our hostname or use the hostname provided.
  string hostname;

  if (flags.hostname.isNone()) {
    if (flags.hostname_lookup) {
      Try<string> result = net::getHostname(self().address.ip);

      if (result.isError()) {
        LOG(FATAL) << "Failed to get hostname: " << result.error();
      }

      hostname = result.get();
    } else {
      // We use the IP address for hostname if the user requested us
      // NOT to look it up, and it wasn't explicitly set via --hostname:
      hostname = stringify(self().address.ip);
    }
  } else {
    hostname = flags.hostname.get();
  }

  info_.set_hostname(hostname);

  // This uses the new `Address` message in `MasterInfo`.
  info_.mutable_address()->set_ip(stringify(self().address.ip));
  info_.mutable_address()->set_port(self().address.port);
  info_.mutable_address()->set_hostname(hostname);
}


Master::~Master() {}


// TODO(vinod): Update this interface to return failed futures when
// capacity is reached.
struct BoundedRateLimiter
{
  BoundedRateLimiter(double qps, Option<uint64_t> _capacity)
    : limiter(new process::RateLimiter(qps)),
      capacity(_capacity),
      messages(0) {}

  process::Owned<process::RateLimiter> limiter;
  const Option<uint64_t> capacity;

  // Number of outstanding messages for this RateLimiter.
  // NOTE: ExitedEvents are throttled but not counted towards
  // the capacity here.
  uint64_t messages;
};


void Master::initialize()
{
  LOG(INFO) << "Master " << info_.id() << " (" << info_.hostname() << ")"
            << " started on " << string(self()).substr(7);
  LOG(INFO) << "Flags at startup: " << flags;

  if (process::address().ip.isLoopback()) {
    LOG(WARNING) << "\n**************************************************\n"
                 << "Master bound to loopback interface!"
                 << " Cannot communicate with remote schedulers or slaves."
                 << " You might want to set '--ip' flag to a routable"
                 << " IP address.\n"
                 << "**************************************************";
  }

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
    Result<Credentials> _credentials =
      credentials::read(flags.credentials.get());
    if (_credentials.isError()) {
      EXIT(1) << _credentials.error() << " (see --credentials flag)";
    } else if (_credentials.isNone()) {
      EXIT(1) << "Credentials file must contain at least one credential"
              << " (see --credentials flag)";
    }
    // Store credentials in master to use them in routes.
    credentials = _credentials.get();
  }

  // Extract authenticator names and validate them.
  authenticatorNames = strings::split(flags.authenticators, ",");
  if (authenticatorNames.empty()) {
    EXIT(1) << "No authenticator specified";
  }
  if (authenticatorNames.size() > 1) {
    EXIT(1) << "Multiple authenticators not supported";
  }
  if (authenticatorNames[0] != DEFAULT_AUTHENTICATOR &&
      !modules::ModuleManager::contains<Authenticator>(
          authenticatorNames[0])) {
    EXIT(1) << "Authenticator '" << authenticatorNames[0] << "' not found. "
            << "Check the spelling (compare to '" << DEFAULT_AUTHENTICATOR
            << "'') or verify that the authenticator was loaded successfully "
            << "(see --modules)";
  }

  // TODO(tillt): Allow multiple authenticators to be loaded and enable
  // the authenticatee to select the appropriate one. See MESOS-1939.
  if (authenticatorNames[0] == DEFAULT_AUTHENTICATOR) {
    LOG(INFO) << "Using default '" << DEFAULT_AUTHENTICATOR
              << "' authenticator";
    authenticator = new cram_md5::CRAMMD5Authenticator();
  } else {
    Try<Authenticator*> module =
      modules::ModuleManager::create<Authenticator>(authenticatorNames[0]);
    if (module.isError()) {
      EXIT(1) << "Could not create authenticator module '"
              << authenticatorNames[0] << "': " << module.error();
    }
    LOG(INFO) << "Using '" << authenticatorNames[0] << "' authenticator";
    authenticator = module.get();
  }

  // Give Authenticator access to credentials when needed.
  CHECK_SOME(authenticator);
  Try<Nothing> initialize = authenticator.get()->initialize(credentials);
  if (initialize.isError()) {
    const string error =
      "Failed to initialize authenticator '" + authenticatorNames[0] +
      "': " + initialize.error();
    if (flags.authenticate_frameworks || flags.authenticate_slaves) {
      EXIT(1) << "Failed to start master with authentication enabled: "
              << error;
    } else {
      // A failure to initialize the authenticator does lead to
      // unusable authentication but still allows non authenticating
      // frameworks and slaves to connect.
      LOG(WARNING) << "Only non-authenticating frameworks and slaves are "
                   << "allowed to connect. "
                   << "Authentication is disabled: " << error;
      delete authenticator.get();
      authenticator = None();
    }
  }

  if (authorizer.isSome()) {
    LOG(INFO) << "Authorization enabled";
  }

  if (flags.rate_limits.isSome()) {
    // Add framework rate limiters.
    foreach (const RateLimit& limit_, flags.rate_limits.get().limits()) {
      if (frameworks.limiters.contains(limit_.principal())) {
        EXIT(1) << "Duplicate principal " << limit_.principal()
                << " found in RateLimits configuration";
      }

      if (limit_.has_qps() && limit_.qps() <= 0) {
        EXIT(1) << "Invalid qps: " << limit_.qps()
                << ". It must be a positive number";
      }

      if (limit_.has_qps()) {
        Option<uint64_t> capacity;
        if (limit_.has_capacity()) {
          capacity = limit_.capacity();
        }
        frameworks.limiters.put(
            limit_.principal(),
            Owned<BoundedRateLimiter>(
                new BoundedRateLimiter(limit_.qps(), capacity)));
      } else {
        frameworks.limiters.put(limit_.principal(), None());
      }
    }

    if (flags.rate_limits.get().has_aggregate_default_qps() &&
        flags.rate_limits.get().aggregate_default_qps() <= 0) {
      EXIT(1) << "Invalid aggregate_default_qps: "
              << flags.rate_limits.get().aggregate_default_qps()
              << ". It must be a positive number";
    }

    if (flags.rate_limits.get().has_aggregate_default_qps()) {
      Option<uint64_t> capacity;
      if (flags.rate_limits.get().has_aggregate_default_capacity()) {
        capacity = flags.rate_limits.get().aggregate_default_capacity();
      }
      frameworks.defaultLimiter = Owned<BoundedRateLimiter>(
          new BoundedRateLimiter(
              flags.rate_limits.get().aggregate_default_qps(), capacity));
    }

    LOG(INFO) << "Framework rate limiting enabled";
  }

  // If the rate limiter is injected for testing,
  // the flag may not be set.
  if (slaves.limiter.isSome() && flags.slave_removal_rate_limit.isSome()) {
    LOG(INFO) << "Slave removal is rate limited to "
              << flags.slave_removal_rate_limit.get();
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

  // Verify the timeout is greater than zero.
  if (flags.offer_timeout.isSome() &&
      flags.offer_timeout.get() <= Duration::zero()) {
    EXIT(1) << "Invalid value '" << flags.offer_timeout.get() << "' "
            << "for --offer_timeout: Must be greater than zero.";
  }

  // Initialize the allocator.
  allocator->initialize(
      flags.allocation_interval,
      defer(self(), &Master::offer, lambda::_1, lambda::_2),
      defer(self(), &Master::inverseOffer, lambda::_1, lambda::_2),
      roleInfos);

  // Parse the whitelist. Passing Allocator::updateWhitelist()
  // callback is safe because we shut down the whitelistWatcher in
  // Master::finalize(), while allocator lifetime is greater than
  // masters. Therefore there is no risk of calling into an allocator
  // that has been cleaned up.
  whitelistWatcher = new WhitelistWatcher(
      flags.whitelist,
      WHITELIST_WATCH_INTERVAL,
      [this](const Option<hashset<string>>& whitelist) {
        return allocator->updateWhitelist(whitelist);
      });
  spawn(whitelistWatcher);

  nextFrameworkId = 0;
  nextSlaveId = 0;
  nextOfferId = 0;

  startTime = Clock::now();

  install<scheduler::Call>(&Master::receive);

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
      &RegisterSlaveMessage::slave,
      &RegisterSlaveMessage::checkpointed_resources,
      &RegisterSlaveMessage::version);

  install<ReregisterSlaveMessage>(
      &Master::reregisterSlave,
      &ReregisterSlaveMessage::slave,
      &ReregisterSlaveMessage::checkpointed_resources,
      &ReregisterSlaveMessage::executor_infos,
      &ReregisterSlaveMessage::tasks,
      &ReregisterSlaveMessage::completed_frameworks,
      &ReregisterSlaveMessage::version);

  install<UnregisterSlaveMessage>(
      &Master::unregisterSlave,
      &UnregisterSlaveMessage::slave_id);

  install<StatusUpdateMessage>(
      &Master::statusUpdate,
      &StatusUpdateMessage::update,
      &StatusUpdateMessage::pid);

  // Added in 0.24.0 to support HTTP schedulers. Since
  // these do not have a pid, the slave must forward
  // messages through the master.
  install<ExecutorToFrameworkMessage>(
      &Master::executorMessage,
      &ExecutorToFrameworkMessage::slave_id,
      &ExecutorToFrameworkMessage::framework_id,
      &ExecutorToFrameworkMessage::executor_id,
      &ExecutorToFrameworkMessage::data);

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

  install<UpdateSlaveMessage>(
      &Master::updateSlave,
      &UpdateSlaveMessage::slave_id,
      &UpdateSlaveMessage::oversubscribed_resources);

  install<AuthenticateMessage>(
      &Master::authenticate,
      &AuthenticateMessage::pid);

  // Setup HTTP routes.
  Http http = Http(this);

  route("/api/v1/scheduler",
        Http::SCHEDULER_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.scheduler(request);
        });
  route("/health",
        Http::HEALTH_HELP,
        [http](const process::http::Request& request) {
          return http.health(request);
        });
  route("/observe",
        Http::OBSERVE_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.observe(request);
        });
  route("/redirect",
        Http::REDIRECT_HELP,
        [http](const process::http::Request& request) {
          return http.redirect(request);
        });
  route("/reserve",
        Http::RESERVE_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.reserve(request);
        });
  // TODO(ijimenez): Remove this endpoint at the end of the
  // deprecation cycle on 0.26.
  route("/roles.json",
        Http::ROLES_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.roles(request);
        });
  route("/roles",
        Http::ROLES_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.roles(request);
        });
  route("/teardown",
        Http::TEARDOWN_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.teardown(request);
        });
  route("/slaves",
        Http::SLAVES_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.slaves(request);
        });
  // TODO(ijimenez): Remove this endpoint at the end of the
  // deprecation cycle on 0.26.
  route("/state.json",
        Http::STATE_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.state(request);
        });
  route("/state",
        Http::STATE_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.state(request);
        });
  route("/state-summary",
        Http::STATESUMMARY_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.stateSummary(request);
        });
  // TODO(ijimenez): Remove this endpoint at the end of the
  // deprecation cycle.
  route("/tasks.json",
        Http::TASKS_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.tasks(request);
        });
  route("/tasks",
        Http::TASKS_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.tasks(request);
        });
  route("/maintenance/schedule",
        Http::MAINTENANCE_SCHEDULE_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.maintenanceSchedule(request);
        });
  route("/maintenance/status",
        Http::MAINTENANCE_STATUS_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.maintenanceStatus(request);
        });
  route("/machine/down",
        Http::MACHINE_DOWN_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.machineDown(request);
        });
  route("/machine/up",
        Http::MACHINE_UP_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.machineUp(request);
        });
  route("/unreserve",
        Http::UNRESERVE_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.unreserve(request);
        });

  // Provide HTTP assets from a "webui" directory. This is either
  // specified via flags (which is necessary for running out of the
  // build directory before 'make install') or determined at build
  // time via the preprocessor macro '-DMESOS_WEBUI_DIR' set in the
  // Makefile.
  provide("", path::join(flags.webui_dir, "master/static/index.html"));
  provide("static", path::join(flags.webui_dir, "master/static"));

  // Expose the log file for the webui. Fall back to 'log_dir' if
  // an explicit file was not specified.
  if (flags.external_log_file.isSome()) {
    files->attach(flags.external_log_file.get(), "/master/log")
      .onAny(defer(self(),
                   &Self::fileAttached,
                   lambda::_1,
                   flags.external_log_file.get()));
  } else if (flags.log_dir.isSome()) {
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

  // NOTE: Even though we remove the slave and framework from the
  // allocator, it is possible that offers are already dispatched to
  // this master. In tests, if a new master (with the same PID) is
  // started, it might process the offers from the old master's
  // allocator.
  // TODO(vinod): Fix the above race by changing the allocator
  // interface to return a stream of offer events.

  // Remove the slaves.
  foreachvalue (Slave* slave, slaves.registered) {
    // We first remove the slave from the allocator so that any
    // recovered resources below are not reoffered.
    allocator->removeSlave(slave->id);

    foreachkey (const FrameworkID& frameworkId, utils::copy(slave->tasks)) {
      foreachvalue (Task* task, utils::copy(slave->tasks[frameworkId])) {
        removeTask(task);
      }
    }

    // Remove executors.
    foreachkey (const FrameworkID& frameworkId, utils::copy(slave->executors)) {
      foreachkey (const ExecutorID& executorId,
                  utils::copy(slave->executors[frameworkId])) {
        removeExecutor(slave, frameworkId, executorId);
      }
    }

    // Remove offers.
    foreach (Offer* offer, utils::copy(slave->offers)) {
      removeOffer(offer);
    }

    // Remove inverse offers.
    foreach (InverseOffer* inverseOffer, utils::copy(slave->inverseOffers)) {
      // We don't need to update the allocator because the slave has already
      // been removed.
      removeInverseOffer(inverseOffer);
    }

    // Terminate the slave observer.
    terminate(slave->observer);
    wait(slave->observer);

    delete slave->observer;
    delete slave;
  }
  slaves.registered.clear();

  // Remove the frameworks.
  // Note we are not deleting the pointers to the frameworks from the
  // roles because it is unnecessary bookkeeping at this point since
  // we are shutting down.
  foreachvalue (Framework* framework, frameworks.registered) {
    allocator->removeFramework(framework->id());

    // Remove pending tasks from the framework. Don't bother
    // recovering the resources in the allocator.
    framework->pendingTasks.clear();

    // No tasks/executors/offers should remain since the slaves
    // have been removed.
    CHECK(framework->tasks.empty());
    CHECK(framework->executors.empty());
    CHECK(framework->offers.empty());
    CHECK(framework->inverseOffers.empty());

    delete framework;
  }
  frameworks.registered.clear();

  CHECK(offers.empty());
  CHECK(inverseOffers.empty());

  foreachvalue (Future<Option<string>> future, authenticating) {
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
    Clock::cancel(slaves.recoveredTimer.get());
  }

  terminate(whitelistWatcher);
  wait(whitelistWatcher);
  delete whitelistWatcher;

  if (authenticator.isSome()) {
    delete authenticator.get();
  }
}


void Master::exited(const FrameworkID& frameworkId, const HttpConnection& http)
{
  foreachvalue (Framework* framework, frameworks.registered) {
    if (framework->http.isSome() &&
        framework->http.get().writer == http.writer) {
      CHECK_EQ(frameworkId, framework->id());
      _exited(framework);
      return;
    }

    // If the framework has reconnected, the writer will not match
    // above, and we will have a framework with a matching id.
    if (frameworkId == framework->id()) {
      LOG(INFO) << "Ignoring disconnection for framework "
                << *framework << " as it has already reconnected";
      return;
    }
  }
}


void Master::exited(const UPID& pid)
{
  foreachvalue (Framework* framework, frameworks.registered) {
    if (framework->pid == pid) {
      _exited(framework);
      return;
    }
  }

  // The semantics when a registered slave gets disconnected are as
  // follows:
  //
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
  if (slaves.registered.contains(pid)) {
    Slave* slave = slaves.registered.get(pid);
    CHECK_NOTNULL(slave);

    LOG(INFO) << "Slave " << *slave << " disconnected";

    if (!slave->info.checkpoint()) {
      // Remove the slave, if it is not checkpointing.
      LOG(INFO) << "Removing disconnected slave " << *slave
                << " because it is not checkpointing!";
      removeSlave(slave, "slave is non-checkpointing and disconnected");
      return;
    } else if (slave->connected) {
      // Checkpointing slaves can just be disconnected.
      disconnect(slave);

      // Remove all non-checkpointing frameworks.
      hashset<FrameworkID> frameworkIds =
        slave->tasks.keys() | slave->executors.keys();

      foreach (const FrameworkID& frameworkId, frameworkIds) {
        Framework* framework = getFramework(frameworkId);
        if (framework != NULL && !framework->info.checkpoint()) {
          LOG(INFO) << "Removing framework " << *framework
                    << " from disconnected slave " << *slave
                    << " because the framework is not checkpointing";

          removeFramework(slave, framework);
        }
      }
    } else {
      // NOTE: A duplicate exited() event is possible for a slave
      // because its PID doesn't change on restart. See MESOS-675
      // for details.
      LOG(WARNING) << "Ignoring duplicate exited() notification for "
                   << "checkpointing slave " << *slave;
    }
  }
}


void Master::_exited(Framework* framework)
{
  LOG(INFO) << "Framework " << *framework << " disconnected";

  // Disconnect the framework.
  disconnect(framework);

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

  LOG(INFO) << "Giving framework " << *framework << " "
            << failoverTimeout << " to failover";

  // Delay dispatching a message to ourselves for the timeout.
  delay(failoverTimeout,
        self(),
        &Master::frameworkFailoverTimeout,
        framework->id(),
        framework->reregisteredTime);
}


void Master::visit(const MessageEvent& event)
{
  // There are three cases about the message's UPID with respect to
  // 'frameworks.principals':
  // 1) if a <UPID, principal> pair exists and the principal is Some,
  //    it's a framework with its principal specified.
  // 2) if a <UPID, principal> pair exists and the principal is None,
  //    it's a framework without a principal.
  // 3) if a <UPID, principal> pair does not exist in the map, it's
  //    either an unregistered framework or not a framework.
  // The logic for framework message counters and rate limiting
  // mainly concerns with whether the UPID is a *registered*
  // framework and whether the framework has a principal so we use
  // these two temp variables to simplify the condition checks below.
  bool isRegisteredFramework =
    frameworks.principals.contains(event.message->from);
  const Option<string> principal = isRegisteredFramework
    ? frameworks.principals[event.message->from]
    : Option<string>::none();

  // Increment the "message_received" counter if the message is from
  // a framework and such a counter is configured for it.
  // See comments for 'Master::Metrics::Frameworks' and
  // 'Master::Frameworks::principals' for details.
  if (principal.isSome()) {
    // If the framework has a principal, the counter must exist.
    CHECK(metrics->frameworks.contains(principal.get()));
    Counter messages_received =
      metrics->frameworks.get(principal.get()).get()->messages_received;
    ++messages_received;
  }

  // All messages are filtered when non-leading.
  if (!elected()) {
    VLOG(1) << "Dropping '" << event.message->name << "' message since "
            << "not elected yet";
    ++metrics->dropped_messages;
    return;
  }

  CHECK_SOME(recovered);

  // All messages are filtered while recovering.
  // TODO(bmahler): Consider instead re-enqueing *all* messages
  // through recover(). What are the performance implications of
  // the additional queueing delay and the accumulated backlog
  // of messages post-recovery?
  if (!recovered.get().isReady()) {
    VLOG(1) << "Dropping '" << event.message->name << "' message since "
            << "not recovered yet";
    ++metrics->dropped_messages;
    return;
  }

  // Throttle the message if it's a framework message and a
  // RateLimiter is configured for the framework's principal.
  // The framework is throttled by the default RateLimiter if:
  // 1) the default RateLimiter is configured (and)
  // 2) the framework doesn't have a principal or its principal is
  //    not specified in 'flags.rate_limits'.
  // The framework is not throttled if:
  // 1) the default RateLimiter is not configured to handle case 2)
  //    above. (or)
  // 2) the principal exists in RateLimits but 'qps' is not set.
  if (principal.isSome() &&
      frameworks.limiters.contains(principal.get()) &&
      frameworks.limiters[principal.get()].isSome()) {
    const Owned<BoundedRateLimiter>& limiter =
      frameworks.limiters[principal.get()].get();

    if (limiter->capacity.isNone() ||
        limiter->messages < limiter->capacity.get()) {
      limiter->messages++;
      limiter->limiter->acquire()
        .onReady(defer(self(), &Self::throttled, event, principal));
    } else {
      exceededCapacity(
          event,
          principal,
          limiter->capacity.get());
    }
  } else if ((principal.isNone() ||
              !frameworks.limiters.contains(principal.get())) &&
             isRegisteredFramework &&
             frameworks.defaultLimiter.isSome()) {
    if (frameworks.defaultLimiter.get()->capacity.isNone() ||
        frameworks.defaultLimiter.get()->messages <
          frameworks.defaultLimiter.get()->capacity.get()) {
      frameworks.defaultLimiter.get()->messages++;
      frameworks.defaultLimiter.get()->limiter->acquire()
        .onReady(defer(self(), &Self::throttled, event, None()));
    } else {
      exceededCapacity(
          event,
          principal,
          frameworks.defaultLimiter.get()->capacity.get());
    }
  } else {
    _visit(event);
  }
}


void Master::visit(const ExitedEvent& event)
{
  // See comments in 'visit(const MessageEvent& event)' for which
  // RateLimiter is used to throttle this UPID and when it is not
  // throttled.
  // Note that throttling ExitedEvent is necessary so the order
  // between MessageEvents and ExitedEvents from the same PID is
  // maintained. Also ExitedEvents are not subject to the capacity.
  bool isRegisteredFramework = frameworks.principals.contains(event.pid);
  const Option<string> principal = isRegisteredFramework
    ? frameworks.principals[event.pid]
    : Option<string>::none();

  // Necessary to disambiguate below.
  typedef void(Self::*F)(const ExitedEvent&);

  if (principal.isSome() &&
      frameworks.limiters.contains(principal.get()) &&
      frameworks.limiters[principal.get()].isSome()) {
    frameworks.limiters[principal.get()].get()->limiter->acquire()
      .onReady(defer(self(), static_cast<F>(&Self::_visit), event));
  } else if ((principal.isNone() ||
              !frameworks.limiters.contains(principal.get())) &&
             isRegisteredFramework &&
             frameworks.defaultLimiter.isSome()) {
    frameworks.defaultLimiter.get()->limiter->acquire()
      .onReady(defer(self(), static_cast<F>(&Self::_visit), event));
  } else {
    _visit(event);
  }
}


void Master::throttled(
    const MessageEvent& event,
    const Option<std::string>& principal)
{
  // We already know a RateLimiter is used to throttle this event so
  // here we only need to determine which.
  if (principal.isSome()) {
    CHECK_SOME(frameworks.limiters[principal.get()]);
    frameworks.limiters[principal.get()].get()->messages--;
  } else {
    CHECK_SOME(frameworks.defaultLimiter);
    frameworks.defaultLimiter.get()->messages--;
  }

  _visit(event);
}


void Master::_visit(const MessageEvent& event)
{
  // Obtain the principal before processing the Message because the
  // mapping may be deleted in handling 'UnregisterFrameworkMessage'
  // but its counter still needs to be incremented for this message.
  const Option<string> principal =
    frameworks.principals.contains(event.message->from)
      ? frameworks.principals[event.message->from]
      : Option<string>::none();

  ProtobufProcess<Master>::visit(event);

  // Increment 'messages_processed' counter if it still exists.
  // Note that it could be removed in handling
  // 'UnregisterFrameworkMessage' if it's the last framework with
  // this principal.
  if (principal.isSome() && metrics->frameworks.contains(principal.get())) {
    Counter messages_processed =
      metrics->frameworks.get(principal.get()).get()->messages_processed;
    ++messages_processed;
  }
}


void Master::exceededCapacity(
    const MessageEvent& event,
    const Option<string>& principal,
    uint64_t capacity)
{
  LOG(WARNING) << "Dropping message " << event.message->name << " from "
               << event.message->from
               << (principal.isSome() ? "(" + principal.get() + ")" : "")
               << ": capacity(" << capacity << ") exceeded";

  // Send an error to the framework which will abort the scheduler
  // driver.
  // NOTE: The scheduler driver will send back a
  // DeactivateFrameworkMessage which may be dropped as well but this
  // should be fine because the scheduler is already informed of an
  // unrecoverable error and should take action to recover.
  FrameworkErrorMessage message;
  message.set_message(
      "Message " + event.message->name +
      " dropped: capacity(" + stringify(capacity) + ") exceeded");
  send(event.message->from, message);
}


void Master::_visit(const ExitedEvent& event)
{
  Process<Master>::visit(event);
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

  // Save the maintenance schedule.
  foreach (const mesos::maintenance::Schedule& schedule, registry.schedules()) {
    maintenance.schedules.push_back(schedule);
  }

  // Save the machine info for each machine.
  foreach (const Registry::Machine& machine, registry.machines().machines()) {
    machines[machine.info().id()] = Machine(machine.info());
  }

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
            << flags.slave_reregister_timeout
            << " there were " << slaves.recovered.size()
            << " (" << removalPercentage * 100 << "%) slaves recovered from the"
            << " registry that did not re-register: \n"
            << stringify(slaves.recovered) << "\n "
            << " The configured removal limit is " << limit * 100 << "%. Please"
            << " investigate or increase this limit to proceed further";
  }

  // Remove the slaves in a rate limited manner, similar to how the
  // SlaveObserver removes slaves.
  foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
    // The slave is removed from 'recovered' when it re-registers.
    if (!slaves.recovered.contains(slave.info().id())) {
      continue;
    }

    Future<Nothing> acquire = Nothing();

    if (slaves.limiter.isSome()) {
      LOG(INFO) << "Scheduling removal of slave "
                << slave.info().id() << " (" << slave.info().hostname() << ")"
                << "; did not re-register within "
                << flags.slave_reregister_timeout << " after master failover";

      acquire = slaves.limiter.get()->acquire();
    }

    // Need to disambiguate for the compiler.
    // TODO(bmahler): With C++11, just call removeSlave from within
    // a lambda function to avoid the need to disambiguate.
    Nothing (Master::*removeSlave)(const Registry::Slave&) = &Self::removeSlave;
    const string failure = "Slave removal rate limit acquisition failed";

    acquire
      .then(defer(self(), removeSlave, slave))
      .onFailed(lambda::bind(fail, failure, lambda::_1))
      .onDiscarded(lambda::bind(fail, failure, "discarded"));

    ++metrics->slave_shutdowns_scheduled;
  }
}


Nothing Master::removeSlave(const Registry::Slave& slave)
{
  // The slave is removed from 'recovered' when it re-registers.
  if (!slaves.recovered.contains(slave.info().id())) {
    LOG(INFO) << "Canceling removal of slave "
              << slave.info().id() << " (" << slave.info().hostname() << ")"
              << " since it re-registered!";

    ++metrics->slave_shutdowns_canceled;

    return Nothing();
  }

  LOG(WARNING) << "Slave " << slave.info().id()
               << " (" << slave.info().hostname() << ") did not re-register"
               << " within " << flags.slave_reregister_timeout
               << " after master failover; removing it from the registrar";

  ++metrics->slave_shutdowns_completed;
  ++metrics->recovery_slave_removals;

  slaves.recovered.erase(slave.info().id());

  if (flags.registry_strict) {
    slaves.removing.insert(slave.info().id());

    registrar->apply(Owned<Operation>(new RemoveSlave(slave.info())))
      .onAny(defer(self(),
                   &Self::_removeSlave,
                   slave.info(),
                   vector<StatusUpdate>(), // No TASK_LOST updates to send.
                   lambda::_1,
                   "did not re-register after master failover",
                   metrics->slave_removals_reason_unhealthy));
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

  return Nothing();
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


void Master::contended(const Future<Future<Nothing>>& candidacy)
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


void Master::detected(const Future<Option<MasterInfo>>& _leader)
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


Future<bool> Master::authorizeFramework(
    const FrameworkInfo& frameworkInfo)
{
  if (authorizer.isNone()) {
    return true; // Authorization is disabled.
  }

  LOG(INFO) << "Authorizing framework principal '" << frameworkInfo.principal()
            << "' to receive offers for role '" << frameworkInfo.role() << "'";

  mesos::ACL::RegisterFramework request;
  if (frameworkInfo.has_principal()) {
    request.mutable_principals()->add_values(frameworkInfo.principal());
  } else {
    // Framework doesn't have a principal set.
    request.mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  }
  request.mutable_roles()->add_values(frameworkInfo.role());

  return authorizer.get()->authorize(request);
}


Option<Error> Master::validateFrameworkAuthentication(
    const FrameworkInfo& frameworkInfo,
    const UPID& from)
{
  if (authenticating.contains(from)) {
    return Error("Re-authentication in progress");
  }

  if (flags.authenticate_frameworks && !authenticated.contains(from)) {
    // This could happen if another authentication request came
    // through before we are here or if a framework tried to
    // (re-)register without authentication.
    return Error("Framework at " + stringify(from) + " is not authenticated");
  }

  // TODO(bmahler): Currently the scheduler driver does not
  // set 'principal', so we allow frameworks to omit it.
  if (frameworkInfo.has_principal() &&
      authenticated.contains(from) &&
      frameworkInfo.principal() != authenticated[from]) {
    return Error("Framework principal '" + frameworkInfo.principal() + "'"
                 " does not match authenticated principal"
                 " '" + authenticated[from]  + "'");
  }

  return None();
}


void Master::drop(
    const UPID& from,
    const scheduler::Call& call,
    const string& message)
{
  // TODO(bmahler): Increment a metric.

  LOG(ERROR) << "Dropping " << call.type() << " call"
             << " from framework " << call.framework_id()
             << " at " << from << ": " << message;
}


void Master::drop(
    Framework* framework,
    const Offer::Operation& operation,
    const string& message)
{
  // TODO(jieyu): Increment a metric.

  // NOTE: There is no direct feedback to the framework when an
  // operation is dropped. The framework will find out that the
  // operation was dropped through subsequent offers.

  LOG(ERROR) << "Dropping " << Offer::Operation::Type_Name(operation.type())
             << " offer operation from framework " << *framework
             << ": " << message;
}


void Master::receive(
    const UPID& from,
    const scheduler::Call& call)
{
  // TODO(vinod): Add metrics for calls.

  Option<Error> error = validation::scheduler::call::validate(call);

  if (error.isSome()) {
    drop(from, call, error.get().message);
    return;
  }

  if (call.type() == scheduler::Call::SUBSCRIBE) {
    subscribe(from, call.subscribe());
    return;
  }

  // We consolidate the framework lookup and pid validation logic here
  // because they are common for all the call handlers.
  Framework* framework = getFramework(call.framework_id());

  if (framework == NULL) {
    drop(from, call, "Framework cannot be found");
    return;
  }

  if (framework->pid != from) {
    drop(from, call, "Call is not from registered framework");
    return;
  }

  switch (call.type()) {
    case scheduler::Call::TEARDOWN:
      teardown(framework);
      break;

    case scheduler::Call::ACCEPT:
      accept(framework, call.accept());
      break;

    case scheduler::Call::DECLINE:
      decline(framework, call.decline());
      break;

    case scheduler::Call::REVIVE:
      revive(framework);
      break;

    case scheduler::Call::KILL:
      kill(framework, call.kill());
      break;

    case scheduler::Call::SHUTDOWN:
      shutdown(framework, call.shutdown());
      break;

    case scheduler::Call::ACKNOWLEDGE:
      acknowledge(framework, call.acknowledge());
      break;

    case scheduler::Call::RECONCILE:
      reconcile(framework, call.reconcile());
      break;

    case scheduler::Call::MESSAGE:
      message(framework, call.message());
      break;

    case scheduler::Call::REQUEST:
      request(framework, call.request());
      break;

    case scheduler::Call::SUPPRESS:
      suppress(framework);
      break;

    default:
      // Should be caught during call validation above.
      LOG(FATAL) << "Unexpected " << call.type() << " call"
                 << " from framework " << call.framework_id() << " at " << from;
      break;
  }
}


void Master::registerFramework(
    const UPID& from,
    const FrameworkInfo& frameworkInfo)
{
  ++metrics->messages_register_framework;

  if (frameworkInfo.has_id() && !frameworkInfo.id().value().empty()) {
    const string error = "Registering with 'id' already set";

    LOG(INFO) << "Refusing registration request of framework"
              << " '" << frameworkInfo.name() << "' at " << from
              << ": " << error;

    FrameworkErrorMessage message;
    message.set_message(error);
    send(from, message);
    return;
  }

  scheduler::Call::Subscribe call;
  call.mutable_framework_info()->CopyFrom(frameworkInfo);

  subscribe(from, call);
}


void Master::reregisterFramework(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    bool failover)
{
  ++metrics->messages_reregister_framework;

  if (!frameworkInfo.has_id() || frameworkInfo.id().value().empty()) {
    const string error = "Re-registering without an 'id'";

    LOG(INFO) << "Refusing re-registration request of framework"
              << " '" << frameworkInfo.name() << "' at " << from
              << ": " << error;

    FrameworkErrorMessage message;
    message.set_message(error);
    send(from, message);
    return;
  }

  scheduler::Call::Subscribe call;
  call.mutable_framework_info()->CopyFrom(frameworkInfo);
  call.set_force(failover);

  subscribe(from, call);
}


void Master::subscribe(
    HttpConnection http,
    const scheduler::Call::Subscribe& subscribe)
{
  // TODO(anand): Authenticate the framework.

  const FrameworkInfo& frameworkInfo = subscribe.framework_info();

  LOG(INFO) << "Received subscription request for"
            << " HTTP framework '" << frameworkInfo.name() << "'";

  Option<Error> validationError = None();

  // TODO(vinod): Deprecate this in favor of ACLs.
  if (validationError.isNone() && !roles.contains(frameworkInfo.role())) {
    validationError = Error("Role '" + frameworkInfo.role() + "' is not" +
                            " present in the master's --roles");
  }

  // TODO(vinod): Deprecate this in favor of authorization.
  if (validationError.isNone() &&
      frameworkInfo.user() == "root" && !flags.root_submissions) {
    validationError = Error("User 'root' is not allowed to run frameworks"
                            " without --root_submissions set");
  }

  if (validationError.isNone() && frameworkInfo.has_id()) {
    foreach (const shared_ptr<Framework>& framework, frameworks.completed) {
      if (framework->id() == frameworkInfo.id()) {
        // This could happen if a framework tries to subscribe after
        // its failover timeout has elapsed or it unregistered itself
        // by calling 'stop()' on the scheduler driver.
        //
        // TODO(vinod): Master should persist admitted frameworks to the
        // registry and remove them from it after failover timeout.
        validationError = Error("Framework has been removed");
        break;
      }
    }
  }

  if (validationError.isSome()) {
    LOG(INFO) << "Refusing subscription of framework"
              << " '" << frameworkInfo.name() << "': "
              << validationError.get().message;

    FrameworkErrorMessage message;
    message.set_message(validationError.get().message);

    http.send(message);
    http.close();
    return;
  }

  // Need to disambiguate for the compiler.
  void (Master::*_subscribe)(
      HttpConnection,
      const scheduler::Call::Subscribe&,
      const Future<bool>&) = &Self::_subscribe;

  authorizeFramework(frameworkInfo)
    .onAny(defer(self(),
                 _subscribe,
                 http,
                 subscribe,
                 lambda::_1));
}


void Master::_subscribe(
    HttpConnection http,
    const scheduler::Call::Subscribe& subscribe,
    const Future<bool>& authorized)
{
  const FrameworkInfo& frameworkInfo = subscribe.framework_info();

  CHECK(!authorized.isDiscarded());

  Option<Error> authorizationError = None();

  if (authorized.isFailed()) {
    authorizationError =
      Error("Authorization failure: " + authorized.failure());
  } else if (!authorized.get()) {
    authorizationError =
      Error("Not authorized to use role '" + frameworkInfo.role() + "'");
  }

  if (authorizationError.isSome()) {
    LOG(INFO) << "Refusing subscription of framework"
              << " '" << frameworkInfo.name() << "'"
              << ": " << authorizationError.get().message;

    FrameworkErrorMessage message;
    message.set_message(authorizationError.get().message);
    http.send(message);
    http.close();
    return;
  }

  LOG(INFO) << "Subscribing framework '" << frameworkInfo.name()
            << "' with checkpointing "
            << (frameworkInfo.checkpoint() ? "enabled" : "disabled")
            << " and capabilities " << frameworkInfo.capabilities();

  if (!frameworkInfo.has_id() || frameworkInfo.id() == "") {
    // If we are here the framework is subscribing for the first time.
    // Assign a new FrameworkID.
    FrameworkInfo frameworkInfo_ = frameworkInfo;
    frameworkInfo_.mutable_id()->CopyFrom(newFrameworkId());

    Framework* framework = new Framework(this, flags, frameworkInfo_, http);

    addFramework(framework);

    FrameworkRegisteredMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id());
    message.mutable_master_info()->MergeFrom(info_);
    framework->send(message);

    // Start the heartbeat after sending SUBSCRIBED event.
    framework->heartbeat();

    return;
  }

  // If we are here framework has already been assigned an id.
  CHECK(!frameworkInfo.id().value().empty());

  if (frameworks.registered.contains(frameworkInfo.id())) {
    // Using the "force" field of the scheduler allows us to keep a
    // scheduler that got partitioned but didn't die (in ZooKeeper
    // speak this means didn't lose their session) and then
    // eventually tried to connect to this master even though
    // another instance of their scheduler has reconnected.

    Framework* framework =
      CHECK_NOTNULL(frameworks.registered[frameworkInfo.id()]);

    // Note that if the scheduler is retrying we expect it
    // to close its old connection. But, the master may not
    // realize that the connection is closed before the retry
    // occurs so we may kick off a scheduler unnecessarily.
    if (framework->connected && !subscribe.force()) {
      LOG(ERROR) << "Disallowing subscription attempt"
                 << " of framework " << *framework
                 << " because it is already connected";

      FrameworkErrorMessage message;
      message.set_message("Framework is already connected");

      http.send(message);
      http.close();
      return;
    }

    // It is now safe to update the framework fields since the request is now
    // guaranteed to be successful. We use the fields passed in during
    // re-registration.
    LOG(INFO) << "Updating info for framework " << framework->id();

    framework->updateFrameworkInfo(frameworkInfo);
    allocator->updateFramework(framework->id(), framework->info);

    framework->reregisteredTime = Clock::now();

    if (subscribe.force()) {
      LOG(INFO) << "Framework " << *framework << " failed over";
      failoverFramework(framework, http);
    } else {
      LOG(INFO) << "Allowing framework " << *framework
                << " to subscribe with an already used id";

      // Convert the framework to an http framework if it was
      // pid based in the past.
      if (framework->pid.isSome()) {
        framework->pid = None();
      }

      framework->connected = true;
      framework->updateConnection(http);

      http.closed()
        .onAny(defer(self(), &Self::exited, framework->id(), http));

      // Reactivate the framework.
      if (!framework->active) {
        framework->active = true;
        allocator->activateFramework(framework->id());
      }

      FrameworkReregisteredMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id());
      message.mutable_master_info()->MergeFrom(info_);
      framework->send(message);

      // Start the heartbeat after sending SUBSCRIBED event.
      framework->heartbeat();
    }
  } else {
    // We don't have a framework with this ID, so we must be a newly
    // elected Mesos master to which either an existing scheduler or a
    // failed-over one is connecting. Create a Framework object and add
    // any tasks it has that have been reported by reconnecting slaves.
    Framework* framework = new Framework(this, flags, frameworkInfo, http);

    // Add active tasks and executors to the framework.
    foreachvalue (Slave* slave, slaves.registered) {
      foreachvalue (Task* task, slave->tasks[framework->id()]) {
        framework->addTask(task);
      }
      foreachvalue (const ExecutorInfo& executor,
                    slave->executors[framework->id()]) {
        framework->addExecutor(slave->id, executor);
      }
    }

    // N.B. Need to add the framework _after_ we add its tasks
    // (above) so that we can properly determine the resources it's
    // currently using!
    addFramework(framework);

    FrameworkReregisteredMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id());
    message.mutable_master_info()->MergeFrom(info_);
    framework->send(message);

    // Start the heartbeat after sending SUBSCRIBED event.
    framework->heartbeat();
  }

  CHECK(frameworks.registered.contains(frameworkInfo.id()))
    << "Unknown framework " << frameworkInfo.id()
    << " (" << frameworkInfo.name() << ")";

  // Broadcast the new framework pid to all the slaves. We have to
  // broadcast because an executor might be running on a slave but
  // it currently isn't running any tasks.
  foreachvalue (Slave* slave, slaves.registered) {
    UpdateFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkInfo.id());

    // TODO(anand): We set 'pid' to UPID() for http frameworks
    // as 'pid' was made optional in 0.24.0. In 0.25.0, we
    // no longer have to set pid here for http frameowrks.
    message.set_pid(UPID());
    send(slave->pid, message);
  }
}


void Master::subscribe(
    const UPID& from,
    const scheduler::Call::Subscribe& subscribe)
{
  const FrameworkInfo& frameworkInfo = subscribe.framework_info();

  if (authenticating.contains(from)) {
    // TODO(vinod): Consider dropping this request and fix the tests
    // to deal with the drop. Currently there is a race between master
    // realizing the framework is authenticated and framework sending
    // a subscribe call. Dropping this message will cause the
    // framework to retry slowing down the tests.
    LOG(INFO) << "Queuing up SUBSCRIBE call for"
              << " framework '" << frameworkInfo.name() << "' at " << from
              << " because authentication is still in progress";

    // Need to disambiguate for the compiler.
    void (Master::*f)(const UPID&, const scheduler::Call::Subscribe&)
      = &Self::subscribe;

    authenticating[from]
      .onReady(defer(self(), f, from, subscribe));
    return;
  }

  Option<Error> validationError = None();

  // TODO(vinod): Deprecate this in favor of ACLs.
  if (validationError.isNone() && !roles.contains(frameworkInfo.role())) {
    validationError = Error("Role '" + frameworkInfo.role() + "' is not" +
                            " present in the master's --roles");
  }

  // TODO(vinod): Deprecate this in favor of authorization.
  if (validationError.isNone() &&
      frameworkInfo.user() == "root" && !flags.root_submissions) {
    validationError = Error("User 'root' is not allowed to run frameworks"
                            " without --root_submissions set");
  }

  if (validationError.isNone() && frameworkInfo.has_id()) {
    foreach (const shared_ptr<Framework>& framework, frameworks.completed) {
      if (framework->id() == frameworkInfo.id()) {
        // This could happen if a framework tries to subscribe after
        // its failover timeout has elapsed or it unregistered itself
        // by calling 'stop()' on the scheduler driver.
        //
        // TODO(vinod): Master should persist admitted frameworks to the
        // registry and remove them from it after failover timeout.
        validationError = Error("Framework has been removed");
        break;
      }
    }
  }

  // Note that re-authentication errors are already handled above.
  if (validationError.isNone()) {
    validationError = validateFrameworkAuthentication(frameworkInfo, from);
  }

  if (validationError.isSome()) {
    LOG(INFO) << "Refusing subscription of framework"
              << " '" << frameworkInfo.name() << "' at " << from << ": "
              << validationError.get().message;

    FrameworkErrorMessage message;
    message.set_message(validationError.get().message);
    send(from, message);
    return;
  }

  LOG(INFO) << "Received SUBSCRIBE call for"
            << " framework '" << frameworkInfo.name() << "' at " << from;

  // We allow an authenticated framework to not specify a principal
  // in FrameworkInfo but we'd prefer if it did so we log a WARNING
  // here when it happens.
  if (!frameworkInfo.has_principal() && authenticated.contains(from)) {
    LOG(WARNING) << "Framework at " << from
                 << " (authenticated as '" << authenticated[from] << "')"
                 << " does not set 'principal' in FrameworkInfo";
  }

  // Need to disambiguate for the compiler.
  void (Master::*_subscribe)(
      const UPID&,
      const scheduler::Call::Subscribe&,
      const Future<bool>&) = &Self::_subscribe;

  authorizeFramework(frameworkInfo)
    .onAny(defer(self(),
                 _subscribe,
                 from,
                 subscribe,
                 lambda::_1));
}


void Master::_subscribe(
    const UPID& from,
    const scheduler::Call::Subscribe& subscribe,
    const Future<bool>& authorized)
{
  const FrameworkInfo& frameworkInfo = subscribe.framework_info();

  CHECK(!authorized.isDiscarded());

  Option<Error> authorizationError = None();

  if (authorized.isFailed()) {
    authorizationError =
      Error("Authorization failure: " + authorized.failure());
  } else if (!authorized.get()) {
    authorizationError =
      Error("Not authorized to use role '" + frameworkInfo.role() + "'");
  }

  if (authorizationError.isSome()) {
    LOG(INFO) << "Refusing subscription of framework"
              << " '" << frameworkInfo.name() << "' at " << from
              << ": " << authorizationError.get().message;

    FrameworkErrorMessage message;
    message.set_message(authorizationError.get().message);

    send(from, message);
    return;
  }

  // At this point, authentications errors will be due to
  // re-authentication during the authorization process,
  // so we drop the subscription.
  Option<Error> authenticationError =
    validateFrameworkAuthentication(frameworkInfo, from);

  if (authenticationError.isSome()) {
    LOG(INFO) << "Dropping SUBSCRIBE call for framework"
              << " '" << frameworkInfo.name() << "' at " << from
              << ": " << authenticationError.get().message;
    return;
  }

  LOG(INFO) << "Subscribing framework " << frameworkInfo.name()
            << " with checkpointing "
            << (frameworkInfo.checkpoint() ? "enabled" : "disabled")
            << " and capabilities " << frameworkInfo.capabilities();

  if (!frameworkInfo.has_id() || frameworkInfo.id().value().empty()) {
    // If we are here the framework is subscribing for the first time.
    // Check if this framework is already subscribed (because it retries).
    foreachvalue (Framework* framework, frameworks.registered) {
      if (framework->pid == from) {
        LOG(INFO) << "Framework " << *framework
                  << " already subscribed, resending acknowledgement";
        FrameworkRegisteredMessage message;
        message.mutable_framework_id()->MergeFrom(framework->id());
        message.mutable_master_info()->MergeFrom(info_);
        framework->send(message);
        return;
      }
    }

    // Assign a new FrameworkID.
    FrameworkInfo frameworkInfo_ = frameworkInfo;
    frameworkInfo_.mutable_id()->CopyFrom(newFrameworkId());

    Framework* framework = new Framework(this, flags, frameworkInfo_, from);

    addFramework(framework);

    FrameworkRegisteredMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id());
    message.mutable_master_info()->MergeFrom(info_);
    framework->send(message);

    return;
  }

  // If we are here framework has already been assigned an id.
  CHECK(!frameworkInfo.id().value().empty());

  if (frameworks.registered.contains(frameworkInfo.id())) {
    // Using the "force" field of the scheduler allows us to keep a
    // scheduler that got partitioned but didn't die (in ZooKeeper
    // speak this means didn't lose their session) and then
    // eventually tried to connect to this master even though
    // another instance of their scheduler has reconnected.

    Framework* framework =
      CHECK_NOTNULL(frameworks.registered[frameworkInfo.id()]);

    // Test for the error case first.
    if ((framework->pid != from) && !subscribe.force()) {
      LOG(ERROR) << "Disallowing subscription attempt of"
                 << " framework " << *framework
                 << " because it is not expected from " << from;

      FrameworkErrorMessage message;
      message.set_message("Framework failed over");
      send(from, message);
      return;
    }

    // It is now safe to update the framework fields since the request is now
    // guaranteed to be successful. We use the fields passed in during
    // re-registration.
    LOG(INFO) << "Updating info for framework " << framework->id();

    framework->updateFrameworkInfo(frameworkInfo);
    allocator->updateFramework(framework->id(), framework->info);

    framework->reregisteredTime = Clock::now();

    if (subscribe.force()) {
      // TODO(vinod): Now that the scheduler pid is unique we don't
      // need to call 'failoverFramework()' if the pid hasn't changed
      // (i.e., duplicate message). Instead we can just send the
      // FrameworkReregisteredMessage back and activate the framework
      // if necesssary.
      LOG(INFO) << "Framework " << *framework << " failed over";
      failoverFramework(framework, from);
    } else {
      LOG(INFO) << "Allowing framework " << *framework
                << " to subscribe with an already used id";

      // Remove any offers sent to this framework.
      // NOTE: We need to do this because the scheduler might have
      // replied to the offers but the driver might have dropped
      // those messages since it wasn't connected to the master.
      foreach (Offer* offer, utils::copy(framework->offers)) {
        allocator->recoverResources(
            offer->framework_id(),
            offer->slave_id(),
            offer->resources(),
            None());
        removeOffer(offer, true); // Rescind.
      }

      // Also remove inverse offers.
      foreach (InverseOffer* inverseOffer,
               utils::copy(framework->inverseOffers)) {
        allocator->updateInverseOffer(
            inverseOffer->slave_id(),
            inverseOffer->framework_id(),
            UnavailableResources{
                inverseOffer->resources(),
                inverseOffer->unavailability()},
            None());

        removeInverseOffer(inverseOffer, true); // Rescind.
      }

      // TODO(bmahler): Shouldn't this re-link with the scheduler?
      framework->connected = true;

      // Reactivate the framework.
      // NOTE: We do this after recovering resources (above) so that
      // the allocator has the correct view of the framework's share.
      if (!framework->active) {
        framework->active = true;
        allocator->activateFramework(framework->id());
      }

      FrameworkReregisteredMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkInfo.id());
      message.mutable_master_info()->MergeFrom(info_);
      framework->send(message);
      return;
    }
  } else {
    // We don't have a framework with this ID, so we must be a newly
    // elected Mesos master to which either an existing scheduler or a
    // failed-over one is connecting. Create a Framework object and add
    // any tasks it has that have been reported by reconnecting slaves.
    Framework* framework = new Framework(this, flags, frameworkInfo, from);

    // Add active tasks and executors to the framework.
    foreachvalue (Slave* slave, slaves.registered) {
      foreachvalue (Task* task, slave->tasks[framework->id()]) {
        framework->addTask(task);
      }
      foreachvalue (const ExecutorInfo& executor,
                    slave->executors[framework->id()]) {
        framework->addExecutor(slave->id, executor);
      }
    }

    // N.B. Need to add the framework _after_ we add its tasks
    // (above) so that we can properly determine the resources it's
    // currently using!
    addFramework(framework);

    // TODO(bmahler): We have to send a registered message here for
    // the re-registering framework, per the API contract. Send
    // re-register here per MESOS-786; requires deprecation or it
    // will break frameworks.
    FrameworkRegisteredMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id());
    message.mutable_master_info()->MergeFrom(info_);
    framework->send(message);
  }

  CHECK(frameworks.registered.contains(frameworkInfo.id()))
    << "Unknown framework " << frameworkInfo.id()
    << " (" << frameworkInfo.name() << ")";

  // Broadcast the new framework pid to all the slaves. We have to
  // broadcast because an executor might be running on a slave but
  // it currently isn't running any tasks.
  foreachvalue (Slave* slave, slaves.registered) {
    UpdateFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkInfo.id());
    message.set_pid(from);
    send(slave->pid, message);
  }
}


void Master::unregisterFramework(
    const UPID& from,
    const FrameworkID& frameworkId)
{
  LOG(INFO) << "Asked to unregister framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    if (framework->pid == from) {
      teardown(framework);
    } else {
      LOG(WARNING)
        << "Ignoring unregister framework message for framework " << *framework
        << " because it is not expected from " << from;
    }
  }
}


void Master::deactivateFramework(
    const UPID& from,
    const FrameworkID& frameworkId)
{
  ++metrics->messages_deactivate_framework;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring deactivate framework message for framework " << frameworkId
      << " because the framework cannot be found";
    return;
  }

  if (framework->pid != from) {
    LOG(WARNING)
      << "Ignoring deactivate framework message for framework " << *framework
      << " because it is not expected from " << from;
    return;
  }

  deactivate(framework);
}


void Master::disconnect(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Disconnecting framework " << *framework;

  framework->connected = false;

  if (framework->pid.isSome()) {
    // Remove the framework from authenticated. This is safe because
    // a framework will always reauthenticate before (re-)registering.
    authenticated.erase(framework->pid.get());
  } else {
    CHECK_SOME(framework->http);

    // Close the HTTP connection, which may already have
    // been closed due to scheduler disconnection.
    framework->http.get().close();
  }

  deactivate(framework);
}


void Master::deactivate(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Deactivating framework " << *framework;

  // Stop sending offers here for now.
  framework->active = false;

  // Tell the allocator to stop allocating resources to this framework.
  allocator->deactivateFramework(framework->id());

  // Remove the framework's offers.
  foreach (Offer* offer, utils::copy(framework->offers)) {
    allocator->recoverResources(
        offer->framework_id(), offer->slave_id(), offer->resources(), None());

    removeOffer(offer, true); // Rescind.
  }

  // Remove the framework's inverse offers.
  foreach (InverseOffer* inverseOffer, utils::copy(framework->inverseOffers)) {
    allocator->updateInverseOffer(
        inverseOffer->slave_id(),
        inverseOffer->framework_id(),
        UnavailableResources{
            inverseOffer->resources(),
            inverseOffer->unavailability()},
        None());

    removeInverseOffer(inverseOffer, true); // Rescind.
  }
}


void Master::disconnect(Slave* slave)
{
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Disconnecting slave " << *slave;

  slave->connected = false;

  // Inform the slave observer.
  dispatch(slave->observer, &SlaveObserver::disconnect);

  // Remove the slave from authenticated. This is safe because
  // a slave will always reauthenticate before (re-)registering.
  authenticated.erase(slave->pid);

  deactivate(slave);
}


void Master::deactivate(Slave* slave)
{
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Deactivating slave " << *slave;

  slave->active = false;

  allocator->deactivateSlave(slave->id);

  // Remove and rescind offers.
  foreach (Offer* offer, utils::copy(slave->offers)) {
    allocator->recoverResources(
        offer->framework_id(), slave->id, offer->resources(), None());

    removeOffer(offer, true); // Rescind!
  }

  // Remove and rescind inverse offers.
  foreach (InverseOffer* inverseOffer, utils::copy(slave->inverseOffers)) {
    allocator->updateInverseOffer(
        slave->id,
        inverseOffer->framework_id(),
        UnavailableResources{
            inverseOffer->resources(),
            inverseOffer->unavailability()},
        None());

    removeInverseOffer(inverseOffer, true); // Rescind!
  }
}


void Master::resourceRequest(
    const UPID& from,
    const FrameworkID& frameworkId,
    const vector<Request>& requests)
{
  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring resource request message from framework " << frameworkId
      << " because the framework cannot be found";
    return;
  }

  if (framework->pid != from) {
    LOG(WARNING)
      << "Ignoring resource request message from framework " << *framework
      << " because it is not expected from " << from;
    return;
  }

  scheduler::Call::Request call;
  foreach (const Request& request, requests) {
    call.add_requests()->CopyFrom(request);
  }

  request(framework, call);
}


void Master::request(
    Framework* framework,
    const scheduler::Call::Request& request)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Processing REQUEST call for framework " << *framework;

  ++metrics->messages_resource_request;

  allocator->requestResources(
      framework->id(),
      google::protobuf::convert(request.requests()));
}


void Master::suppress(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Processing SUPPRESS call for framework " << *framework;

  ++metrics->messages_suppress_offers;

  allocator->suppressOffers(framework->id());
}


void Master::launchTasks(
    const UPID& from,
    const FrameworkID& frameworkId,
    const vector<TaskInfo>& tasks,
    const Filters& filters,
    const vector<OfferID>& offerIds)
{
  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring launch tasks message for offers " << stringify(offerIds)
      << " of framework " << frameworkId
      << " because the framework cannot be found";

    return;
  }

  if (framework->pid != from) {
    LOG(WARNING)
      << "Ignoring launch tasks message for offers " << stringify(offerIds)
      << " from '" << from << "' because it is not from the"
      << " registered framework " << *framework;

    return;
  }

  // Currently when no tasks are specified in the launchTasks message
  // it is implicitly considered a decline of the offers.
  if (!tasks.empty()) {
    scheduler::Call::Accept message;
    message.mutable_filters()->CopyFrom(filters);

    Offer::Operation* operation = message.add_operations();
    operation->set_type(Offer::Operation::LAUNCH);

    foreach (const TaskInfo& task, tasks) {
      operation->mutable_launch()->add_task_infos()->CopyFrom(task);
    }

    foreach (const OfferID& offerId, offerIds) {
      message.add_offer_ids()->CopyFrom(offerId);
    }

    accept(framework, message);
  } else {
    scheduler::Call::Decline message;
    message.mutable_filters()->CopyFrom(filters);

    foreach (const OfferID& offerId, offerIds) {
      message.add_offer_ids()->CopyFrom(offerId);
    }

    decline(framework, message);
  }
}


Future<bool> Master::authorizeTask(
    const TaskInfo& task,
    Framework* framework)
{
  if (authorizer.isNone()) {
    return true; // Authorization is disabled.
  }

  // Authorize the task.
  string user = framework->info.user(); // Default user.
  if (task.has_command() && task.command().has_user()) {
    user = task.command().user();
  } else if (task.has_executor() && task.executor().command().has_user()) {
    user = task.executor().command().user();
  }

  LOG(INFO)
    << "Authorizing framework principal '" << framework->info.principal()
    << "' to launch task " << task.task_id() << " as user '" << user << "'";

  mesos::ACL::RunTask request;
  if (framework->info.has_principal()) {
    request.mutable_principals()->add_values(framework->info.principal());
  } else {
    // Framework doesn't have a principal set.
    request.mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  }
  request.mutable_users()->add_values(user);

  return authorizer.get()->authorize(request);
}


Resources Master::addTask(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);
  CHECK(slave->connected) << "Adding task " << task.task_id()
                          << " to disconnected slave " << *slave;

  // The resources consumed.
  Resources resources = task.resources();

  // Determine if this task launches an executor, and if so make sure
  // the slave and framework state has been updated accordingly.
  Option<ExecutorID> executorId;

  if (task.has_executor()) {
    // TODO(benh): Refactor this code into Slave::addTask.
    if (!slave->hasExecutor(framework->id(), task.executor().executor_id())) {
      CHECK(!framework->hasExecutor(slave->id, task.executor().executor_id()))
        << "Executor " << task.executor().executor_id()
        << " known to the framework " << *framework
        << " but unknown to the slave " << *slave;

      slave->addExecutor(framework->id(), task.executor());
      framework->addExecutor(slave->id, task.executor());

      resources += task.executor().resources();
    }

    executorId = task.executor().executor_id();
  }

  // Add the task to the framework and slave.
  Task* t = new Task();
  t->mutable_framework_id()->MergeFrom(framework->id());
  t->set_state(TASK_STAGING);
  t->set_name(task.name());
  t->mutable_task_id()->MergeFrom(task.task_id());
  t->mutable_slave_id()->MergeFrom(task.slave_id());
  t->mutable_resources()->MergeFrom(task.resources());

  if (executorId.isSome()) {
    t->mutable_executor_id()->MergeFrom(executorId.get());
  }

  if (task.has_labels()) {
    t->mutable_labels()->MergeFrom(task.labels());
  }
  if (task.has_discovery()) {
    t->mutable_discovery()->MergeFrom(task.discovery());
  }

  slave->addTask(t);
  framework->addTask(t);

  return resources;
}


void Master::accept(
    Framework* framework,
    const scheduler::Call::Accept& accept)
{
  CHECK_NOTNULL(framework);

  foreach (const Offer::Operation& operation, accept.operations()) {
    if (operation.type() == Offer::Operation::LAUNCH) {
      if (operation.launch().task_infos().size() > 0) {
        ++metrics->messages_launch_tasks;
      } else {
        ++metrics->messages_decline_offers;
      }
    }

    // TODO(jieyu): Add metrics for non launch operations.
  }

  // TODO(bmahler): We currently only support using multiple offers
  // for a single slave.
  Resources offeredResources;
  Option<SlaveID> slaveId = None();
  Option<Error> error = None();

  if (accept.offer_ids().size() == 0) {
    error = Error("No offers specified");
  } else {
    // Validate the offers.
    error = validation::offer::validate(accept.offer_ids(), this, framework);

    // Compute offered resources and remove the offers. If the
    // validation failed, return resources to the allocator.
    foreach (const OfferID& offerId, accept.offer_ids()) {
      Offer* offer = getOffer(offerId);

      // Since we re-use `OfferID`s, it is possible to arrive here with either
      // a resource offer, or an inverse offer. We first try as a resource offer
      // and if that fails, then we assume it is an inverse offer. This is
      // correct as those are currently the only 2 ways to get an `OfferID`.
      if (offer != NULL) {
        slaveId = offer->slave_id();
        offeredResources += offer->resources();

        if (error.isSome()) {
          allocator->recoverResources(
              offer->framework_id(),
              offer->slave_id(),
              offer->resources(),
              None());
        }
        removeOffer(offer);
        continue;
      }

      // Try it as an inverse offer. If this fails then the offer is no longer
      // valid.
      InverseOffer* inverseOffer = getInverseOffer(offerId);
      if (inverseOffer != NULL) {
        mesos::master::InverseOfferStatus status;
        status.set_status(mesos::master::InverseOfferStatus::ACCEPT);
        status.mutable_framework_id()->CopyFrom(inverseOffer->framework_id());
        status.mutable_timestamp()->CopyFrom(protobuf::getCurrentTime());

        allocator->updateInverseOffer(
            inverseOffer->slave_id(),
            inverseOffer->framework_id(),
            UnavailableResources{
                inverseOffer->resources(),
                inverseOffer->unavailability()},
            status,
            accept.filters());

        removeInverseOffer(inverseOffer);
        continue;
      }

      // If the offer was neither in our offer or inverse offer sets, then this
      // offer is no longer valid.
      LOG(WARNING) << "Ignoring accept of offer " << offerId
                   << " since it is no longer valid";
    }
  }

  // If invalid, send TASK_LOST for the launch attempts.
  // TODO(jieyu): Consider adding a 'drop' overload for ACCEPT call to
  // consistently handle message dropping. It would be ideal if the
  // 'drop' overload can handle both resource recovery and lost task
  // notifications.
  if (error.isSome()) {
    LOG(WARNING) << "ACCEPT call used invalid offers '" << accept.offer_ids()
                 << "': " << error.get().message;

    foreach (const Offer::Operation& operation, accept.operations()) {
      if (operation.type() != Offer::Operation::LAUNCH) {
        continue;
      }

      foreach (const TaskInfo& task, operation.launch().task_infos()) {
        const StatusUpdate& update = protobuf::createStatusUpdate(
            framework->id(),
            task.slave_id(),
            task.task_id(),
            TASK_LOST,
            TaskStatus::SOURCE_MASTER,
            None(),
            "Task launched with invalid offers: " + error.get().message,
            TaskStatus::REASON_INVALID_OFFERS);

        metrics->tasks_lost++;

        metrics->incrementTasksStates(
            TASK_LOST,
            TaskStatus::SOURCE_MASTER,
            TaskStatus::REASON_INVALID_OFFERS);

        forward(update, UPID(), framework);
      }
    }

    return;
  }

  CHECK_SOME(slaveId);
  Slave* slave = slaves.registered.get(slaveId.get());
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Processing ACCEPT call for offers: " << accept.offer_ids()
            << " on slave " << *slave << " for framework " << *framework;

  // If LAUNCH operation is included, authorize the tasks. A task is
  // in 'framework->pendingTasks' before it is authorized.
  //
  // TODO(jieyu): Currently, we only do authorization for the LAUNCH
  // operation. In the future, we might want to introduce
  // authorizations for other offer operations as well.
  //
  // TODO(mpark): Add authorization logic for RESERVE and UNRESERVE
  // when "reserve" and "unreserve" ACLs are being introduced.
  list<Future<bool>> futures;
  foreach (const Offer::Operation& operation, accept.operations()) {
    if (operation.type() != Offer::Operation::LAUNCH) {
      continue;
    }

    foreach (const TaskInfo& task, operation.launch().task_infos()) {
      futures.push_back(authorizeTask(task, framework));

      // Add to pending tasks.
      //
      // NOTE: The task ID here hasn't been validated yet, but it
      // doesn't matter. If the task ID is not valid, the task won't
      // be launched anyway. If two tasks have the same ID, the second
      // one will not be put into 'framework->pendingTasks', therefore
      // will not be launched.
      if (!framework->pendingTasks.contains(task.task_id())) {
        framework->pendingTasks[task.task_id()] = task;
      }
    }
  }

  // Wait for all the tasks to be authorized.
  await(futures)
    .onAny(defer(self(),
                 &Master::_accept,
                 framework->id(),
                 slaveId.get(),
                 offeredResources,
                 accept,
                 lambda::_1));
}


void Master::_accept(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& offeredResources,
    const scheduler::Call::Accept& accept,
    const Future<list<Future<bool>>>& _authorizations)
{
  Framework* framework = getFramework(frameworkId);

  // TODO(jieyu): Consider using the 'drop' overload mentioned in
  // 'accept' to consistently handle dropping ACCEPT calls.
  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring ACCEPT call for framework " << frameworkId
      << " because the framework cannot be found";

    // Tell the allocator about the recovered resources.
    allocator->recoverResources(
        frameworkId,
        slaveId,
        offeredResources,
        None());

    return;
  }

  Slave* slave = slaves.registered.get(slaveId);

  if (slave == NULL || !slave->connected) {
    foreach (const Offer::Operation& operation, accept.operations()) {
      if (operation.type() != Offer::Operation::LAUNCH) {
        continue;
      }

      foreach (const TaskInfo& task, operation.launch().task_infos()) {
        const TaskStatus::Reason reason =
            slave == NULL ? TaskStatus::REASON_SLAVE_REMOVED
                          : TaskStatus::REASON_SLAVE_DISCONNECTED;
        const StatusUpdate& update = protobuf::createStatusUpdate(
            framework->id(),
            task.slave_id(),
            task.task_id(),
            TASK_LOST,
            TaskStatus::SOURCE_MASTER,
            None(),
            slave == NULL ? "Slave removed" : "Slave disconnected",
            reason);

        metrics->tasks_lost++;

        metrics->incrementTasksStates(
            TASK_LOST,
            TaskStatus::SOURCE_MASTER,
            reason);

        forward(update, UPID(), framework);
      }
    }

    // Tell the allocator about the recovered resources.
    allocator->recoverResources(
        frameworkId,
        slaveId,
        offeredResources,
        None());

    return;
  }

  // Some offer operations update the offered resources. We keep
  // updated offered resources here. When a task is successfully
  // launched, we remove its resource from offered resources.
  Resources _offeredResources = offeredResources;

  CHECK_READY(_authorizations);
  list<Future<bool>> authorizations = _authorizations.get();

  foreach (const Offer::Operation& operation, accept.operations()) {
    switch (operation.type()) {
      case Offer::Operation::RESERVE: {
        Option<string> principal = framework->info.has_principal()
          ? framework->info.principal()
          : Option<string>::none();

        Option<Error> error = validation::operation::validate(
            operation.reserve(), framework->info.role(), principal);

        if (error.isSome()) {
          drop(framework, operation, error.get().message);
          continue;
        }

        Try<Resources> resources = _offeredResources.apply(operation);
        if (resources.isError()) {
          drop(framework, operation, resources.error());
          continue;
        }

        _offeredResources = resources.get();

        LOG(INFO) << "Applying RESERVE operation for resources "
                  << operation.reserve().resources() << " from framework "
                  << *framework << " to slave " << *slave;

        apply(framework, slave, operation);
        break;
      }

      case Offer::Operation::UNRESERVE: {
        Option<Error> error = validation::operation::validate(
            operation.unreserve(), framework->info.has_principal());

        if (error.isSome()) {
          drop(framework, operation, error.get().message);
          continue;
        }

        Try<Resources> resources = _offeredResources.apply(operation);
        if (resources.isError()) {
          drop(framework, operation, resources.error());
          continue;
        }

        _offeredResources = resources.get();

        LOG(INFO) << "Applying UNRESERVE operation for resources "
                  << operation.unreserve().resources() << " from framework "
                  << *framework << " to slave " << *slave;

        apply(framework, slave, operation);
        break;
      }

      case Offer::Operation::CREATE: {
        Option<Error> error = validation::operation::validate(
            operation.create(),
            slave->checkpointedResources);

        if (error.isSome()) {
          drop(framework, operation, error.get().message);
          continue;
        }

        Try<Resources> resources = _offeredResources.apply(operation);
        if (resources.isError()) {
          drop(framework, operation, resources.error());
          continue;
        }

        _offeredResources = resources.get();

        LOG(INFO) << "Applying CREATE operation for volumes "
                  << operation.create().volumes() << " from framework "
                  << *framework << " to slave " << *slave;

        apply(framework, slave, operation);
        break;
      }

      case Offer::Operation::DESTROY: {
        Option<Error> error = validation::operation::validate(
            operation.destroy(),
            slave->checkpointedResources);

        if (error.isSome()) {
          drop(framework, operation, error.get().message);
          continue;
        }

        Try<Resources> resources = _offeredResources.apply(operation);
        if (resources.isError()) {
          drop(framework, operation, error.get().message);
          continue;
        }

        _offeredResources = resources.get();

        LOG(INFO) << "Applying DESTROY operation for volumes "
                  << operation.create().volumes() << " from framework "
                  << *framework << " to slave " << *slave;

        apply(framework, slave, operation);
        break;
      }

      case Offer::Operation::LAUNCH: {
        foreach (const TaskInfo& task, operation.launch().task_infos()) {
          Future<bool> authorization = authorizations.front();
          authorizations.pop_front();

          // NOTE: The task will not be in 'pendingTasks' if
          // 'killTask()' for the task was called before we are here.
          // No need to launch the task if it's no longer pending.
          // However, we still need to check the authorization result
          // and do the validation so that we can send status update
          // in case the task has duplicated ID.
          bool pending = framework->pendingTasks.contains(task.task_id());

          // Remove from pending tasks.
          framework->pendingTasks.erase(task.task_id());

          // Check authorization result.
          CHECK(!authorization.isDiscarded());

          if (authorization.isFailed() || !authorization.get()) {
            string user = framework->info.user(); // Default user.
            if (task.has_command() && task.command().has_user()) {
              user = task.command().user();
            } else if (task.has_executor() &&
                       task.executor().command().has_user()) {
              user = task.executor().command().user();
            }

            const StatusUpdate& update = protobuf::createStatusUpdate(
                framework->id(),
                task.slave_id(),
                task.task_id(),
                TASK_ERROR,
                TaskStatus::SOURCE_MASTER,
                None(),
                authorization.isFailed() ?
                    "Authorization failure: " + authorization.failure() :
                    "Not authorized to launch as user '" + user + "'",
                TaskStatus::REASON_TASK_UNAUTHORIZED);

            metrics->tasks_error++;

            metrics->incrementTasksStates(
                TASK_ERROR,
                TaskStatus::SOURCE_MASTER,
                TaskStatus::REASON_TASK_UNAUTHORIZED);

            forward(update, UPID(), framework);

            continue;
          }

          // Validate the task.

          // Make a copy of the original task so that we can
          // fill the missing `framework_id` in ExecutorInfo
          // if needed. This field was added to the API later
          // and thus was made optional.
          TaskInfo task_(task);
          if (task.has_executor() && !task.executor().has_framework_id()) {
            task_.mutable_executor()
                ->mutable_framework_id()->CopyFrom(framework->id());
          }

          const Option<Error>& validationError = validation::task::validate(
              task_,
              framework,
              slave,
              _offeredResources);

          if (validationError.isSome()) {
            const StatusUpdate& update = protobuf::createStatusUpdate(
                framework->id(),
                task_.slave_id(),
                task_.task_id(),
                TASK_ERROR,
                TaskStatus::SOURCE_MASTER,
                None(),
                validationError.get().message,
                TaskStatus::REASON_TASK_INVALID);

            metrics->tasks_error++;

            metrics->incrementTasksStates(
                TASK_ERROR,
                TaskStatus::SOURCE_MASTER,
                TaskStatus::REASON_TASK_INVALID);

            forward(update, UPID(), framework);

            continue;
          }

          // Add task.
          if (pending) {
            _offeredResources -= addTask(task_, framework, slave);

            // TODO(bmahler): Consider updating this log message to
            // indicate when the executor is also being launched.
            LOG(INFO) << "Launching task " << task_.task_id()
                      << " of framework " << *framework
                      << " with resources " << task_.resources()
                      << " on slave " << *slave;

            RunTaskMessage message;
            message.mutable_framework()->MergeFrom(framework->info);

            // TODO(anand): We set 'pid' to UPID() for http frameworks
            // as 'pid' was made optional in 0.24.0. In 0.25.0, we
            // no longer have to set pid here for http frameowrks.
            message.set_pid(framework->pid.getOrElse(UPID()));
            message.mutable_task()->MergeFrom(task_);

            if (HookManager::hooksAvailable()) {
              // Set labels retrieved from label-decorator hooks.
              message.mutable_task()->mutable_labels()->CopyFrom(
                  HookManager::masterLaunchTaskLabelDecorator(
                      task_,
                      framework->info,
                      slave->info));
            }

            send(slave->pid, message);
          }
        }
        break;
      }

      default:
        LOG(ERROR) << "Unsupported offer operation " << operation.type();
        break;
    }
  }

  if (!_offeredResources.empty()) {
    // Tell the allocator about the unused (e.g., refused) resources.
    allocator->recoverResources(
        frameworkId,
        slaveId,
        _offeredResources,
        accept.filters());
  }
}


void Master::decline(
    Framework* framework,
    const scheduler::Call::Decline& decline)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Processing DECLINE call for offers: " << decline.offer_ids()
            << " for framework " << *framework;

  ++metrics->messages_decline_offers;

  //  Return resources to the allocator.
  foreach (const OfferID& offerId, decline.offer_ids()) {
    // Since we re-use `OfferID`s, it is possible to arrive here with either a
    // resource offer, or an inverse offer. We first try as a resource offer and
    // if that fails, then we assume it is an inverse offer. This is correct as
    // those are currently the only 2 ways to get an `OfferID`.
    Offer* offer = getOffer(offerId);
    if (offer != NULL) {
      allocator->recoverResources(
          offer->framework_id(),
          offer->slave_id(),
          offer->resources(),
          decline.filters());

      removeOffer(offer);
      continue;
    }

    // Try it as an inverse offer. If this fails then the offer is no longer
    // valid.
    InverseOffer* inverseOffer = getInverseOffer(offerId);
    if (inverseOffer != NULL) { // If this is an inverse offer.
      mesos::master::InverseOfferStatus status;
      status.set_status(mesos::master::InverseOfferStatus::DECLINE);
      status.mutable_framework_id()->CopyFrom(inverseOffer->framework_id());
      status.mutable_timestamp()->CopyFrom(protobuf::getCurrentTime());

      allocator->updateInverseOffer(
          inverseOffer->slave_id(),
          inverseOffer->framework_id(),
          UnavailableResources{
              inverseOffer->resources(),
              inverseOffer->unavailability()},
          status,
          decline.filters());

      removeInverseOffer(inverseOffer);
      continue;
    }

    // If the offer was neither in our offer or inverse offer sets, then this
    // offer is no longer valid.
    LOG(WARNING) << "Ignoring decline of offer " << offerId
                 << " since it is no longer valid";
  }
}


void Master::reviveOffers(const UPID& from, const FrameworkID& frameworkId)
{
  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring revive offers message for framework " << frameworkId
      << " because the framework cannot be found";
    return;
  }

  if (framework->pid != from) {
    LOG(WARNING)
      << "Ignoring revive offers message for framework " << *framework
      << " because it is not expected from " << from;
    return;
  }

  revive(framework);
}


void Master::revive(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Processing REVIVE call for framework " << *framework;

  ++metrics->messages_revive_offers;

  allocator->reviveOffers(framework->id());
}


void Master::killTask(
    const UPID& from,
    const FrameworkID& frameworkId,
    const TaskID& taskId)
{
  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring kill task message for task " << taskId << " of framework "
      << frameworkId << " because the framework cannot be found";
    return;
  }

  if (framework->pid != from) {
    LOG(WARNING)
      << "Ignoring kill task message for task " << taskId << " of framework "
      << *framework << " because it is not expected from " << from;
    return;
  }

  scheduler::Call::Kill call;
  call.mutable_task_id()->CopyFrom(taskId);

  kill(framework, call);
}


void Master::kill(Framework* framework, const scheduler::Call::Kill& kill)
{
  CHECK_NOTNULL(framework);

  ++metrics->messages_kill_task;

  const TaskID& taskId = kill.task_id();
  const Option<SlaveID> slaveId =
    kill.has_slave_id() ? Option<SlaveID>(kill.slave_id()) : None();

  if (framework->pendingTasks.contains(taskId)) {
    // Remove from pending tasks.
    framework->pendingTasks.erase(taskId);

    const StatusUpdate& update = protobuf::createStatusUpdate(
        framework->id(),
        slaveId,
        taskId,
        TASK_KILLED,
        TaskStatus::SOURCE_MASTER,
        None(),
        "Killed pending task");

    forward(update, UPID(), framework);

    return;
  }

  Task* task = framework->getTask(taskId);
  if (task == NULL) {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << *framework
                 << " because it is unknown; performing reconciliation";

    TaskStatus status;
    status.mutable_task_id()->CopyFrom(taskId);
    if (slaveId.isSome()) {
      status.mutable_slave_id()->CopyFrom(slaveId.get());
    }

    _reconcileTasks(framework, {status});
    return;
  }

  if (slaveId.isSome() && !(slaveId.get() == task->slave_id())) {
    LOG(WARNING) << "Cannot kill task " << taskId << " of slave "
                 << slaveId.get() << " of framework " << *framework
                 << " because it belongs to different slave "
                 << task->slave_id();
    // TODO(vinod): Return a "Bad Request" when using HTTP API.
    return;
  }

  Slave* slave = slaves.registered.get(task->slave_id());
  CHECK(slave != NULL) << "Unknown slave " << task->slave_id();

  // We add the task to 'killedTasks' here because the slave
  // might be partitioned or disconnected but the master
  // doesn't know it yet.
  slave->killedTasks.put(framework->id(), taskId);

  // NOTE: This task will be properly reconciled when the
  // disconnected slave re-registers with the master.
  if (slave->connected) {
    LOG(INFO) << "Telling slave " << *slave
              << " to kill task " << taskId
              << " of framework " << *framework;

    KillTaskMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id());
    message.mutable_task_id()->MergeFrom(taskId);
    send(slave->pid, message);
  } else {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << *framework
                 << " because the slave " << *slave << " is disconnected."
                 << " Kill will be retried if the slave re-registers";
  }
}


void Master::statusUpdateAcknowledgement(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const TaskID& taskId,
    const string& uuid)
{
  // TODO(bmahler): Consider adding a message validator abstraction
  // for the master that takes care of all this boilerplate. Ideally
  // by the time we process messages in the critical master code, we
  // can assume that they are valid. This will become especially
  // important as validation logic is moved out of the scheduler
  // driver and into the master.

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring status update acknowledgement " << UUID::fromBytes(uuid)
      << " for task " << taskId << " of framework " << frameworkId
      << " on slave " << slaveId << " because the framework cannot be found";
    metrics->invalid_status_update_acknowledgements++;
    return;
  }

  if (framework->pid != from) {
    LOG(WARNING)
      << "Ignoring status update acknowledgement " << UUID::fromBytes(uuid)
      << " for task " << taskId << " of framework " << *framework
      << " on slave " << slaveId << " because it is not expected from " << from;
    metrics->invalid_status_update_acknowledgements++;
    return;
  }

  scheduler::Call::Acknowledge message;
  message.mutable_slave_id()->CopyFrom(slaveId);
  message.mutable_task_id()->CopyFrom(taskId);
  message.set_uuid(uuid);

  acknowledge(framework, message);
}


void Master::acknowledge(
    Framework* framework,
    const scheduler::Call::Acknowledge& acknowledge)
{
  CHECK_NOTNULL(framework);

  metrics->messages_status_update_acknowledgement++;

  const SlaveID slaveId = acknowledge.slave_id();
  const TaskID taskId = acknowledge.task_id();
  const UUID uuid = UUID::fromBytes(acknowledge.uuid());

  Slave* slave = slaves.registered.get(slaveId);

  if (slave == NULL) {
    LOG(WARNING)
      << "Cannot send status update acknowledgement " << uuid
      << " for task " << taskId << " of framework " << *framework
      << " to slave " << slaveId << " because slave is not registered";
    metrics->invalid_status_update_acknowledgements++;
    return;
  }

  if (!slave->connected) {
    LOG(WARNING)
      << "Cannot send status update acknowledgement " << uuid
      << " for task " << taskId << " of framework " << *framework
      << " to slave " << *slave << " because slave is disconnected";
    metrics->invalid_status_update_acknowledgements++;
    return;
  }

  Task* task = slave->getTask(framework->id(), taskId);

  if (task != NULL) {
    // Status update state and uuid should be either set or unset
    // together.
    CHECK_EQ(task->has_status_update_uuid(), task->has_status_update_state());

    if (!task->has_status_update_state()) {
      // Task should have status update state set because it must have
      // been set when the update corresponding to this
      // acknowledgement was processed by the master. But in case this
      // acknowledgement was intended for the old run of the master
      // and the task belongs to a 0.20.0 slave, we could be here.
      // Dropping the acknowledgement is safe because the slave will
      // retry the update, at which point the master will set the
      // status update state.
      LOG(ERROR)
        << "Ignoring status update acknowledgement " << uuid
        << " for task " << taskId << " of framework " << *framework
        << " to slave " << *slave << " because the update was not"
        << " sent by this master";
      metrics->invalid_status_update_acknowledgements++;
      return;
    }

    // Remove the task once the terminal update is acknowledged.
    if (protobuf::isTerminalState(task->status_update_state()) &&
        UUID::fromBytes(task->status_update_uuid()) == uuid) {
      removeTask(task);
     }
  }

  LOG(INFO) << "Processing ACKNOWLEDGE call " << uuid << " for task " << taskId
            << " of framework " << *framework << " on slave " << slaveId;

  StatusUpdateAcknowledgementMessage message;
  message.mutable_slave_id()->CopyFrom(slaveId);
  message.mutable_framework_id()->CopyFrom(framework->id());
  message.mutable_task_id()->CopyFrom(taskId);
  message.set_uuid(uuid.toBytes());

  send(slave->pid, message);

  metrics->valid_status_update_acknowledgements++;
}


void Master::schedulerMessage(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING) << "Ignoring framework message"
                 << " for executor '" << executorId << "'"
                 << " of framework " << frameworkId
                 << " because the framework cannot be found";
    metrics->invalid_framework_to_executor_messages++;
    return;
  }

  if (framework->pid != from) {
    LOG(WARNING)
      << "Ignoring framework message for executor " << executorId
      << " of framework " << *framework
      << " because it is not expected from " << from;
    metrics->invalid_framework_to_executor_messages++;
    return;
  }

  scheduler::Call::Message message_;
  message_.mutable_slave_id()->CopyFrom(slaveId);
  message_.mutable_executor_id()->CopyFrom(executorId);
  message_.set_data(data);

  message(framework, message_);
}


void Master::executorMessage(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  metrics->messages_executor_to_framework++;

  if (slaves.removed.get(slaveId).isSome()) {
    // If the slave is removed, we have already informed
    // frameworks that its tasks were LOST, so the slave
    // should shut down.
    LOG(WARNING) << "Ignoring executor message"
                 << " from executor" << " '" << executorId << "'"
                 << " of framework " << frameworkId
                 << " on removed slave " << slaveId
                 << " ; asking slave to shutdown";

    ShutdownMessage message;
    message.set_message("Executor message from unknown slave");
    reply(message);
    metrics->invalid_executor_to_framework_messages++;
    return;
  }

  // The slave should (re-)register with the master before
  // forwarding executor messages.
  if (!slaves.registered.contains(slaveId)) {
    LOG(WARNING) << "Ignoring executor message"
                 << " from executor '" << executorId << "'"
                 << " of framework " << frameworkId
                 << " on unknown slave " << slaveId;
    metrics->invalid_executor_to_framework_messages++;
    return;
  }

  Slave* slave = slaves.registered.get(slaveId);
  CHECK_NOTNULL(slave);

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING) << "Not forwarding executor message"
                 << " for executor '" << executorId << "'"
                 << " of framework " << frameworkId
                 << " on slave " << *slave
                 << " because the framework is unknown";
    metrics->invalid_executor_to_framework_messages++;
    return;
  }

  ExecutorToFrameworkMessage message;
  message.mutable_slave_id()->MergeFrom(slaveId);
  message.mutable_framework_id()->MergeFrom(frameworkId);
  message.mutable_executor_id()->MergeFrom(executorId);
  message.set_data(data);

  framework->send(message);

  metrics->valid_executor_to_framework_messages++;
}


void Master::message(
    Framework* framework,
    const scheduler::Call::Message& message)
{
  CHECK_NOTNULL(framework);

  metrics->messages_framework_to_executor++;

  Slave* slave = slaves.registered.get(message.slave_id());

  if (slave == NULL) {
    LOG(WARNING) << "Cannot send framework message for framework "
                 << *framework << " to slave " << message.slave_id()
                 << " because slave is not registered";
    metrics->invalid_framework_to_executor_messages++;
    return;
  }

  if (!slave->connected) {
    LOG(WARNING) << "Cannot send framework message for framework "
                 << *framework << " to slave " << *slave
                 << " because slave is disconnected";
    metrics->invalid_framework_to_executor_messages++;
    return;
  }

  LOG(INFO) << "Processing MESSAGE call from framework "
            << *framework << " to slave " << *slave;

  FrameworkToExecutorMessage message_;
  message_.mutable_slave_id()->MergeFrom(message.slave_id());
  message_.mutable_framework_id()->MergeFrom(framework->id());
  message_.mutable_executor_id()->MergeFrom(message.executor_id());
  message_.set_data(message.data());
  send(slave->pid, message_);

  metrics->valid_framework_to_executor_messages++;
}


void Master::registerSlave(
    const UPID& from,
    const SlaveInfo& slaveInfo,
    const vector<Resource>& checkpointedResources,
    const string& version)
{
  ++metrics->messages_register_slave;

  if (authenticating.contains(from)) {
    LOG(INFO) << "Queuing up registration request from " << from
              << " because authentication is still in progress";

    authenticating[from]
      .onReady(defer(self(),
                     &Self::registerSlave,
                     from,
                     slaveInfo,
                     checkpointedResources,
                     version));
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

  MachineID machineId;
  machineId.set_hostname(slaveInfo.hostname());
  machineId.set_ip(stringify(from.address.ip));

  // Slaves are not allowed to register while the machine they are on is in
  // `DOWN` mode.
  if (machines.contains(machineId) &&
      machines[machineId].info.mode() == MachineInfo::DOWN) {
    LOG(WARNING) << "Refusing registration of slave at " << from
                 << " because the machine '" << machineId << "' that it is "
                 << "running on is `DOWN`";

    ShutdownMessage message;
    message.set_message("Machine is `DOWN`");
    send(from, message);
    return;
  }

  // Check if this slave is already registered (because it retries).
  if (slaves.registered.contains(from)) {
    Slave* slave = slaves.registered.get(from);
    CHECK_NOTNULL(slave);

    if (!slave->connected) {
      // The slave was previously disconnected but it is now trying
      // to register as a new slave. This could happen if the slave
      // failed recovery and hence registering as a new slave before
      // the master removed the old slave from its map.
      LOG(INFO) << "Removing old disconnected slave " << *slave
                << " because a registration attempt occurred";
      removeSlave(slave,
                  "a new slave registered at the same address",
                  metrics->slave_removals_reason_registered);
    } else {
      CHECK(slave->active)
        << "Unexpected connected but deactivated slave " << *slave;

      LOG(INFO) << "Slave " << *slave << " already registered,"
                << " resending acknowledgement";

      Duration pingTimeout =
        flags.slave_ping_timeout * flags.max_slave_ping_timeouts;
      MasterSlaveConnection connection;
      connection.set_total_ping_timeout_seconds(pingTimeout.secs());

      SlaveRegisteredMessage message;
      message.mutable_slave_id()->CopyFrom(slave->id);
      message.mutable_connection()->CopyFrom(connection);
      send(from, message);
      return;
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
                 checkpointedResources,
                 version,
                 lambda::_1));
}


void Master::_registerSlave(
    const SlaveInfo& slaveInfo,
    const UPID& pid,
    const vector<Resource>& checkpointedResources,
    const string& version,
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
    slaves.removed.put(slaveInfo.id(), Nothing());

    ShutdownMessage message;
    message.set_message(
        "Slave attempted to register but got duplicate slave id " +
        stringify(slaveInfo.id()));
    send(pid, message);
  } else {
    MachineID machineId;
    machineId.set_hostname(slaveInfo.hostname());
    machineId.set_ip(stringify(pid.address.ip));

    Slave* slave = new Slave(
        slaveInfo,
        pid,
        machineId,
        version.empty() ? Option<string>::none() : version,
        Clock::now(),
        checkpointedResources);

    ++metrics->slave_registrations;

    addSlave(slave);

    Duration pingTimeout =
      flags.slave_ping_timeout * flags.max_slave_ping_timeouts;
    MasterSlaveConnection connection;
    connection.set_total_ping_timeout_seconds(pingTimeout.secs());

    SlaveRegisteredMessage message;
    message.mutable_slave_id()->CopyFrom(slave->id);
    message.mutable_connection()->CopyFrom(connection);
    send(slave->pid, message);

    LOG(INFO) << "Registered slave " << *slave
              << " with " << slave->info.resources();
  }
}


void Master::reregisterSlave(
    const UPID& from,
    const SlaveInfo& slaveInfo,
    const vector<Resource>& checkpointedResources,
    const vector<ExecutorInfo>& executorInfos,
    const vector<Task>& tasks,
    const vector<Archive::Framework>& completedFrameworks,
    const string& version)
{
  ++metrics->messages_reregister_slave;

  if (authenticating.contains(from)) {
    LOG(INFO) << "Queuing up re-registration request from " << from
              << " because authentication is still in progress";

    authenticating[from]
      .onReady(defer(self(),
                     &Self::reregisterSlave,
                     from,
                     slaveInfo,
                     checkpointedResources,
                     executorInfos,
                     tasks,
                     completedFrameworks,
                     version));
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

  MachineID machineId;
  machineId.set_hostname(slaveInfo.hostname());
  machineId.set_ip(stringify(from.address.ip));

  // Slaves are not allowed to register while the machine they are on is in
  // 'DOWN` mode.
  if (machines.contains(machineId) &&
      machines[machineId].info.mode() == MachineInfo::DOWN) {
    LOG(WARNING) << "Refusing re-registration of slave at " << from
                 << " because the machine '" << machineId << "' that it is "
                 << "running on is `DOWN`";

    ShutdownMessage message;
    message.set_message("Machine is `DOWN`");
    send(from, message);
    return;
  }

  if (slaves.removed.get(slaveInfo.id()).isSome()) {
    // To compensate for the case where a non-strict registrar is
    // being used, we explicitly deny removed slaves from
    // re-registering. This is because a non-strict registrar cannot
    // enforce this. We've already told frameworks the tasks were
    // lost so it's important to deny the slave from re-registering.
    LOG(WARNING) << "Slave " << slaveInfo.id() << " at " << from
                 << " (" << slaveInfo.hostname() << ") attempted to "
                 << "re-register after removal; shutting it down";

    ShutdownMessage message;
    message.set_message("Slave attempted to re-register after removal");
    send(from, message);
    return;
  }

  Slave* slave = slaves.registered.get(slaveInfo.id());

  if (slave != NULL) {
    slave->reregisteredTime = Clock::now();

    // NOTE: This handles the case where a slave tries to
    // re-register with an existing master (e.g. because of a
    // spurious Zookeeper session expiration or after the slave
    // recovers after a restart).
    // For now, we assume this slave is not nefarious (eventually
    // this will be handled by orthogonal security measures like key
    // based authentication).
    LOG(INFO) << "Re-registering slave " << *slave;

    // We don't allow re-registering this way with a different IP or
    // hostname. This is because maintenance is scheduled at the
    // machine level; so we would need to re-validate the slave's
    // unavailability if the machine it is running on changed.
    if (slave->pid.address.ip != from.address.ip ||
        slave->info.hostname() != slaveInfo.hostname()) {
      LOG(WARNING) << "Slave " << slaveInfo.id() << " at " << from
                   << " (" << slaveInfo.hostname() << ") attempted to "
                   << "re-register with different IP / hostname; expected "
                   << slave->pid.address.ip << " (" << slave->info.hostname()
                   << ") shutting it down";

      ShutdownMessage message;
      message.set_message(
          "Slave attempted to re-register with different IP / hostname");

      send(from, message);
      return;
    }

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
    // NOTE: This sends the re-registered message, including tasks
    // that need to be reconciled by the slave.
    reconcile(slave, executorInfos, tasks);

    // If this is a disconnected slave, add it back to the allocator.
    // This is done after reconciliation to ensure the allocator's
    // offers include the recovered resources initially on this
    // slave.
    if (!slave->connected) {
      slave->connected = true;
      dispatch(slave->observer, &SlaveObserver::reconnect);
      slave->active = true;
      allocator->activateSlave(slave->id);
    }

    CHECK(slave->active)
      << "Unexpected connected but deactivated slave " << *slave;

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
                 checkpointedResources,
                 executorInfos,
                 tasks,
                 completedFrameworks,
                 version,
                 lambda::_1));
}


void Master::_reregisterSlave(
    const SlaveInfo& slaveInfo,
    const UPID& pid,
    const vector<Resource>& checkpointedResources,
    const vector<ExecutorInfo>& executorInfos,
    const vector<Task>& tasks,
    const vector<Archive::Framework>& completedFrameworks,
    const string& version,
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
    slaves.removed.put(slaveInfo.id(), Nothing());

    ShutdownMessage message;
    message.set_message(
        "Slave attempted to re-register with unknown slave id " +
        stringify(slaveInfo.id()));
    send(pid, message);
  } else {
    // Re-admission succeeded.
    MachineID machineId;
    machineId.set_hostname(slaveInfo.hostname());
    machineId.set_ip(stringify(pid.address.ip));

    Slave* slave = new Slave(
        slaveInfo,
        pid,
        machineId,
        version.empty() ? Option<string>::none() : version,
        Clock::now(),
        checkpointedResources,
        executorInfos,
        tasks);

    slave->reregisteredTime = Clock::now();

    ++metrics->slave_reregistrations;

    addSlave(slave, completedFrameworks);

    Duration pingTimeout =
      flags.slave_ping_timeout * flags.max_slave_ping_timeouts;
    MasterSlaveConnection connection;
    connection.set_total_ping_timeout_seconds(pingTimeout.secs());

    SlaveReregisteredMessage message;
    message.mutable_slave_id()->CopyFrom(slave->id);
    message.mutable_connection()->CopyFrom(connection);
    send(slave->pid, message);

    LOG(INFO) << "Re-registered slave " << *slave
              << " with " << slave->info.resources();

    __reregisterSlave(slave, tasks);
  }
}


void Master::__reregisterSlave(Slave* slave, const vector<Task>& tasks)
{
  CHECK_NOTNULL(slave);

  // Send the latest framework pids to the slave.
  hashset<FrameworkID> ids;

  foreach (const Task& task, tasks) {
    Framework* framework = getFramework(task.framework_id());

    if (framework != NULL && !ids.contains(framework->id())) {
      UpdateFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id());

      // TODO(anand): We set 'pid' to UPID() for http frameworks
      // as 'pid' was made optional in 0.24.0. In 0.25.0, we
      // no longer have to set pid here for http frameowrks.
      message.set_pid(framework->pid.getOrElse(UPID()));

      send(slave->pid, message);

      ids.insert(framework->id());
    }
  }

  // NOTE: Here we always send the message. Slaves whose version are
  // less than 0.22.0 will drop it silently which is OK.
  LOG(INFO) << "Sending updated checkpointed resources "
            << slave->checkpointedResources
            << " to slave " << *slave;

  CheckpointResourcesMessage message;
  message.mutable_resources()->CopyFrom(slave->checkpointedResources);

  send(slave->pid, message);
}


void Master::unregisterSlave(const UPID& from, const SlaveID& slaveId)
{
  ++metrics->messages_unregister_slave;

  LOG(INFO) << "Asked to unregister slave " << slaveId;

  Slave* slave = slaves.registered.get(slaveId);

  if (slave != NULL) {
    if (slave->pid != from) {
      LOG(WARNING) << "Ignoring unregister slave message from " << from
                   << " because it is not the slave " << slave->pid;
      return;
    }
    removeSlave(slave,
                "the slave unregistered",
                metrics->slave_removals_reason_unregistered);
  }
}


void Master::updateSlave(
    const SlaveID& slaveId,
    const Resources& oversubscribedResources)
{
  ++metrics->messages_update_slave;

  if (slaves.removed.get(slaveId).isSome()) {
    // If the slave is removed, we have already informed
    // frameworks that its tasks were LOST, so the slave should
    // shut down.
    LOG(WARNING)
      << "Ignoring update of slave with total oversubscribed resources "
      << oversubscribedResources << " on removed slave " << slaveId
      << " ; asking slave to shutdown";

    ShutdownMessage message;
    message.set_message("Update slave message from unknown slave");
    reply(message);
    return;
  }

  if (!slaves.registered.contains(slaveId)) {
    LOG(WARNING)
      << "Ignoring update of slave with total oversubscribed resources "
      << oversubscribedResources << " on unknown slave " << slaveId;
    return;
  }

  Slave* slave = CHECK_NOTNULL(slaves.registered.get(slaveId));

  LOG(INFO) << "Received update of slave " << *slave << " with total"
            << " oversubscribed resources " <<  oversubscribedResources;

  // First, rescind any outstanding offers with revocable resources.
  // NOTE: Need a copy of offers because the offers are removed inside the loop.
  foreach (Offer* offer, utils::copy(slave->offers)) {
    const Resources offered = offer->resources();
    if (!offered.revocable().empty()) {
      LOG(INFO) << "Removing offer " << offer->id()
                << " with revocable resources " << offered
                << " on slave " << *slave;

      allocator->recoverResources(
          offer->framework_id(), offer->slave_id(), offer->resources(), None());

      removeOffer(offer, true); // Rescind.
    }
  }

  // NOTE: We don't need to rescind inverse offers here as they are unrelated to
  // oversubscription.

  slave->totalResources -= slave->totalResources.revocable();
  slave->totalResources += oversubscribedResources.revocable();

  // Now, update the allocator with the new estimate.
  allocator->updateSlave(slaveId, oversubscribedResources);
}


void Master::updateUnavailability(
    const MachineID& machineId,
    const Option<Unavailability>& unavailability)
{
  if (unavailability.isSome()) {
    machines[machineId].info.mutable_unavailability()->CopyFrom(
        unavailability.get());
  } else {
    machines[machineId].info.clear_unavailability();
  }

  // TODO(jmlvanre): Only update allocator and rescind offers if the
  // unavailability has actually changed.
  if (machines.contains(machineId)) {
    // For every slave on this machine, update the allocator.
    foreach (const SlaveID& slaveId, machines[machineId].slaves) {
      // The slave should not be in the machines mapping if it is removed.
      CHECK(slaves.removed.get(slaveId).isNone());

      // The slave should be registered if it is in the machines mapping.
      CHECK(slaves.registered.contains(slaveId));

      Slave* slave = CHECK_NOTNULL(slaves.registered.get(slaveId));

      if (unavailability.isSome()) {
        // TODO(jmlvanre): Add stream operator for unavailability.
        LOG(INFO) << "Updating unavailability of slave " << *slave
                  << ", starting at "
                  << Nanoseconds(unavailability.get().start().nanoseconds());
      } else {
        LOG(INFO) << "Removing unavailability of slave " << *slave;
      }

      // Remove and rescind offers since we want to inform frameworks of the
      // unavailability change as soon as possible.
      foreach (Offer* offer, utils::copy(slave->offers)) {
        allocator->recoverResources(
            offer->framework_id(), slave->id, offer->resources(), None());

        removeOffer(offer, true); // Rescind!
      }

      // Remove and rescind inverse offers since the allocator will send new
      // inverse offers for the updated unavailability.
      foreach (InverseOffer* inverseOffer, utils::copy(slave->inverseOffers)) {
        allocator->updateInverseOffer(
            slave->id,
            inverseOffer->framework_id(),
            UnavailableResources{
                inverseOffer->resources(),
                inverseOffer->unavailability()},
            None());

        removeInverseOffer(inverseOffer, true); // Rescind!
      }

      // We remove / resind all the offers first so that any calls to the
      // allocator to modify its internal state are queued before the update of
      // the unavailability in the allocator. We do this so that the allocator's
      // state can start from a "clean slate" for the new unavailability.
      // NOTE: Any calls from the Allocator back into the master, for example
      // `offer()`, are guaranteed to happen after this function exits due to
      // the Actor pattern.

      allocator->updateUnavailability(slaveId, unavailability);
    }
  }
}


// TODO(vinod): Since 0.22.0, we can use 'from' instead of 'pid'
// because the status updates will be sent by the slave.
void Master::statusUpdate(const StatusUpdate& update, const UPID& pid)
{
  ++metrics->messages_status_update;

  if (slaves.removed.get(update.slave_id()).isSome()) {
    // If the slave is removed, we have already informed
    // frameworks that its tasks were LOST, so the slave should
    // shut down.
    LOG(WARNING) << "Ignoring status update " << update
                 << " from removed slave " << pid
                 << " with id " << update.slave_id() << " ; asking slave "
                 << " to shutdown";

    ShutdownMessage message;
    message.set_message("Status update from unknown slave");
    send(pid, message);

    metrics->invalid_status_updates++;
    return;
  }

  Slave* slave = slaves.registered.get(update.slave_id());

  if (slave == NULL) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " from unknown slave " << pid
                 << " with id " << update.slave_id();
    metrics->invalid_status_updates++;
    return;
  }

  Framework* framework = getFramework(update.framework_id());

  if (framework == NULL) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " from slave " << *slave
                 << " because the framework is unknown";
    metrics->invalid_status_updates++;
    return;
  }

  LOG(INFO) << "Status update " << update << " from slave " << *slave;

  // Forward the update to the framework.
  forward(update, pid, framework);

  // Lookup the task and see if we need to update anything locally.
  Task* task = slave->getTask(update.framework_id(), update.status().task_id());
  if (task == NULL) {
    LOG(WARNING) << "Could not lookup task for status update " << update
                 << " from slave " << *slave;
    metrics->invalid_status_updates++;
    return;
  }

  updateTask(task, update);

  // If the task is terminal and no acknowledgement is needed,
  // then remove the task now.
  if (protobuf::isTerminalState(task->state()) && pid == UPID()) {
    removeTask(task);
  }

  metrics->valid_status_updates++;
}


void Master::forward(
    const StatusUpdate& update,
    const UPID& acknowledgee,
    Framework* framework)
{
  CHECK_NOTNULL(framework);

  if (!acknowledgee) {
    LOG(INFO) << "Sending status update " << update
              << (update.status().has_message()
                  ? " '" + update.status().message() + "'"
                  : "");
  } else {
    LOG(INFO) << "Forwarding status update " << update;
  }

  StatusUpdateMessage message;
  message.mutable_update()->MergeFrom(update);
  message.set_pid(acknowledgee);
  framework->send(message);
}


void Master::exitedExecutor(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    int32_t status)
{
  ++metrics->messages_exited_executor;

  if (slaves.removed.get(slaveId).isSome()) {
    // If the slave is removed, we have already informed
    // frameworks that its tasks were LOST, so the slave should
    // shut down.
    LOG(WARNING) << "Ignoring exited executor '" << executorId
                 << "' of framework " << frameworkId
                 << " on removed slave " << slaveId
                 << " ; asking slave to shutdown";

    ShutdownMessage message;
    message.set_message("Executor exited message from unknown slave");
    reply(message);
    return;
  }

  if (!slaves.registered.contains(slaveId)) {
    LOG(WARNING) << "Ignoring exited executor '" << executorId
                 << "' of framework " << frameworkId
                 << " on unknown slave " << slaveId;
    return;
  }

  // Only update master's internal data structures here for proper
  // accounting. The TASK_LOST updates are handled by the slave.

  Slave* slave = slaves.registered.get(slaveId);
  CHECK_NOTNULL(slave);

  if (!slave->hasExecutor(frameworkId, executorId)) {
    LOG(WARNING) << "Ignoring unknown exited executor '" << executorId
                 << "' of framework " << frameworkId
                 << " on slave " << *slave;
    return;
  }

  LOG(INFO) << "Executor " << executorId
            << " of framework " << frameworkId
            << " on slave " << *slave << ": "
            << WSTRINGIFY(status);

  removeExecutor(slave, frameworkId, executorId);

  // TODO(vinod): Reliably forward this message to the scheduler.
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING)
      << "Not forwarding exited executor message for executor '" << executorId
      << "' of framework " << frameworkId << " on slave " << *slave
      << " because the framework is unknown";

    return;
  }

  ExitedExecutorMessage message;
  message.mutable_executor_id()->CopyFrom(executorId);
  message.mutable_framework_id()->CopyFrom(frameworkId);
  message.mutable_slave_id()->CopyFrom(slaveId);
  message.set_status(status);

  framework->send(message);
}


void Master::shutdown(
    Framework* framework,
    const scheduler::Call::Shutdown& shutdown)
{
  CHECK_NOTNULL(framework);

  // TODO(vinod): Add a metric for executor shutdowns.

  if (!slaves.registered.contains(shutdown.slave_id())) {
    LOG(WARNING) << "Unable to shutdown executor '" << shutdown.executor_id()
                 << "' of framework " << framework->id()
                 << " of unknown slave " << shutdown.slave_id();
    return;
  }

  Slave* slave = slaves.registered.get(shutdown.slave_id());
  CHECK_NOTNULL(slave);

  ShutdownExecutorMessage message;
  message.mutable_executor_id()->CopyFrom(shutdown.executor_id());
  message.mutable_framework_id()->CopyFrom(framework->id());
  send(slave->pid, message);
}


void Master::shutdownSlave(const SlaveID& slaveId, const string& message)
{
  if (!slaves.registered.contains(slaveId)) {
    // Possible when the SlaveObserver dispatched to shutdown a slave,
    // but exited() was already called for this slave.
    LOG(WARNING) << "Unable to shutdown unknown slave " << slaveId;
    return;
  }

  Slave* slave = slaves.registered.get(slaveId);
  CHECK_NOTNULL(slave);

  LOG(WARNING) << "Shutting down slave " << *slave << " with message '"
               << message << "'";

  ShutdownMessage message_;
  message_.set_message(message);
  send(slave->pid, message_);

  removeSlave(slave, message, metrics->slave_removals_reason_unhealthy);
}


void Master::reconcile(
    Framework* framework,
    const scheduler::Call::Reconcile& reconcile)
{
  CHECK_NOTNULL(framework);

  // Construct 'TaskStatus'es from 'Reconcile::Task's.
  vector<TaskStatus> statuses;
  foreach (const scheduler::Call::Reconcile::Task& task, reconcile.tasks()) {
    TaskStatus status;
    status.mutable_task_id()->CopyFrom(task.task_id());
    status.set_state(TASK_RUNNING); // Dummy status.
    if (task.has_slave_id()) {
      status.mutable_slave_id()->CopyFrom(task.slave_id());
    }

    statuses.push_back(status);
  }

  _reconcileTasks(framework, statuses);
}


void Master::reconcileTasks(
    const UPID& from,
    const FrameworkID& frameworkId,
    const std::vector<TaskStatus>& statuses)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Unknown framework " << frameworkId << " at " << from
                 << " attempted to reconcile tasks";
    return;
  }

  if (framework->pid != from) {
    LOG(WARNING)
      << "Ignoring reconcile tasks message for framework " << *framework
      << " because it is not expected from " << from;
    return;
  }

  _reconcileTasks(framework, statuses);
}


void Master::_reconcileTasks(
    Framework* framework,
    const vector<TaskStatus>& statuses)
{
  CHECK_NOTNULL(framework);

  ++metrics->messages_reconcile_tasks;

  if (statuses.empty()) {
    // Implicit reconciliation.
    LOG(INFO) << "Performing implicit task state reconciliation"
                 " for framework " << *framework;

    foreachvalue (const TaskInfo& task, framework->pendingTasks) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id(),
          task.slave_id(),
          task.task_id(),
          TASK_STAGING,
          TaskStatus::SOURCE_MASTER,
          None(),
          "Reconciliation: Latest task state",
          TaskStatus::REASON_RECONCILIATION);

      VLOG(1) << "Sending implicit reconciliation state "
              << update.status().state()
              << " for task " << update.status().task_id()
              << " of framework " << *framework;

      // TODO(bmahler): Consider using forward(); might lead to too
      // much logging.
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update);
      framework->send(message);
    }

    foreachvalue (Task* task, framework->tasks) {
      const TaskState& state = task->has_status_update_state()
          ? task->status_update_state()
          : task->state();

      const Option<ExecutorID>& executorId = task->has_executor_id()
          ? Option<ExecutorID>(task->executor_id())
          : None();

      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id(),
          task->slave_id(),
          task->task_id(),
          state,
          TaskStatus::SOURCE_MASTER,
          None(),
          "Reconciliation: Latest task state",
          TaskStatus::REASON_RECONCILIATION,
          executorId,
          protobuf::getTaskHealth(*task));

      VLOG(1) << "Sending implicit reconciliation state "
              << update.status().state()
              << " for task " << update.status().task_id()
              << " of framework " << *framework;

      // TODO(bmahler): Consider using forward(); might lead to too
      // much logging.
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update);
      framework->send(message);
    }

    return;
  }

  // Explicit reconciliation.
  LOG(INFO) << "Performing explicit task state reconciliation for "
            << statuses.size() << " tasks of framework " << *framework;

  // Explicit reconciliation occurs for the following cases:
  //   (1) Task is known, but pending: TASK_STAGING.
  //   (2) Task is known: send the latest state.
  //   (3) Task is unknown, slave is registered: TASK_LOST.
  //   (4) Task is unknown, slave is transitioning: no-op.
  //   (5) Task is unknown, slave is unknown: TASK_LOST.
  //
  // When using a non-strict registry, case (5) may result in
  // a TASK_LOST for a task that may later be non-terminal. This
  // is better than no reply at all because the framework can take
  // action for TASK_LOST. Later, if the task is running, the
  // framework can discover it with implicit reconciliation and will
  // be able to kill it.
  foreach (const TaskStatus& status, statuses) {
    Option<SlaveID> slaveId = None();
    if (status.has_slave_id()) {
      slaveId = status.slave_id();
    }

    Option<StatusUpdate> update = None();
    Task* task = framework->getTask(status.task_id());

    if (framework->pendingTasks.contains(status.task_id())) {
      // (1) Task is known, but pending: TASK_STAGING.
      const TaskInfo& task_ = framework->pendingTasks[status.task_id()];
      update = protobuf::createStatusUpdate(
          framework->id(),
          task_.slave_id(),
          task_.task_id(),
          TASK_STAGING,
          TaskStatus::SOURCE_MASTER,
          None(),
          "Reconciliation: Latest task state",
          TaskStatus::REASON_RECONCILIATION);
    } else if (task != NULL) {
      // (2) Task is known: send the latest status update state.
      const TaskState& state = task->has_status_update_state()
          ? task->status_update_state()
          : task->state();

      const Option<ExecutorID> executorId = task->has_executor_id()
          ? Option<ExecutorID>(task->executor_id())
          : None();

      update = protobuf::createStatusUpdate(
          framework->id(),
          task->slave_id(),
          task->task_id(),
          state,
          TaskStatus::SOURCE_MASTER,
          None(),
          "Reconciliation: Latest task state",
          TaskStatus::REASON_RECONCILIATION,
          executorId,
          protobuf::getTaskHealth(*task));
    } else if (slaveId.isSome() && slaves.registered.contains(slaveId.get())) {
      // (3) Task is unknown, slave is registered: TASK_LOST.
      update = protobuf::createStatusUpdate(
          framework->id(),
          slaveId.get(),
          status.task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_MASTER,
          None(),
          "Reconciliation: Task is unknown to the slave",
          TaskStatus::REASON_RECONCILIATION);
    } else if (slaves.transitioning(slaveId)) {
      // (4) Task is unknown, slave is transitionary: no-op.
      LOG(INFO) << "Dropping reconciliation of task " << status.task_id()
                << " for framework " << *framework
                << " because there are transitional slaves";
    } else {
      // (5) Task is unknown, slave is unknown: TASK_LOST.
      update = protobuf::createStatusUpdate(
          framework->id(),
          slaveId,
          status.task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_MASTER,
          None(),
          "Reconciliation: Task is unknown",
          TaskStatus::REASON_RECONCILIATION);
    }

    if (update.isSome()) {
      VLOG(1) << "Sending explicit reconciliation state "
              << update.get().status().state()
              << " for task " << update.get().status().task_id()
              << " of framework " << *framework;

      // TODO(bmahler): Consider using forward(); might lead to too
      // much logging.
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update.get());
      framework->send(message);
    }
  }
}


void Master::frameworkFailoverTimeout(const FrameworkID& frameworkId,
                                      const Time& reregisteredTime)
{
  Framework* framework = getFramework(frameworkId);

  if (framework != NULL && !framework->connected) {
    // If the re-registration time has not changed, then the framework
    // has not re-registered within the failover timeout.
    if (framework->reregisteredTime == reregisteredTime) {
      LOG(INFO) << "Framework failover timeout, removing framework "
                << *framework;
      removeFramework(framework);
    }
  }
}


void Master::offer(const FrameworkID& frameworkId,
                   const hashmap<SlaveID, Resources>& resources)
{
  if (!frameworks.registered.contains(frameworkId) ||
      !frameworks.registered[frameworkId]->active) {
    LOG(WARNING) << "Master returning resources offered to framework "
                 << frameworkId << " because the framework"
                 << " has terminated or is inactive";

    foreachpair (const SlaveID& slaveId, const Resources& offered, resources) {
      allocator->recoverResources(frameworkId, slaveId, offered, None());
    }
    return;
  }

  // Create an offer for each slave and add it to the message.
  ResourceOffersMessage message;

  Framework* framework = CHECK_NOTNULL(frameworks.registered[frameworkId]);
  foreachpair (const SlaveID& slaveId, const Resources& offered, resources) {
    if (!slaves.registered.contains(slaveId)) {
      LOG(WARNING)
        << "Master returning resources offered to framework " << *framework
        << " because slave " << slaveId << " is not valid";

      allocator->recoverResources(frameworkId, slaveId, offered, None());
      continue;
    }

    Slave* slave = slaves.registered.get(slaveId);
    CHECK_NOTNULL(slave);

    CHECK(slave->info.checkpoint() || !framework->info.checkpoint())
        << "Resources of non checkpointing slave " << *slave
        << " are being offered to checkpointing framework " << *framework;

    // This could happen if the allocator dispatched 'Master::offer' before
    // the slave was deactivated in the allocator.
    if (!slave->active) {
      LOG(WARNING)
        << "Master returning resources offered because slave " << *slave
        << " is " << (slave->connected ? "deactivated" : "disconnected");

      allocator->recoverResources(frameworkId, slaveId, offered, None());
      continue;
    }

#ifdef WITH_NETWORK_ISOLATOR
    // TODO(dhamon): This flag is required as the static allocation of
    // ephemeral ports leads to a maximum number of containers that can
    // be created on each slave. Once MESOS-1654 is fixed and ephemeral
    // ports are a first class resource, this can be removed.
    if (flags.max_executors_per_slave.isSome()) {
      // Check that we haven't hit the executor limit.
      size_t numExecutors = 0;
      foreachkey (const FrameworkID& frameworkId, slave->executors) {
        numExecutors += slave->executors[frameworkId].keys().size();
      }

      if (numExecutors >= flags.max_executors_per_slave.get()) {
        LOG(WARNING) << "Master returning resources offered because slave "
                     << *slave << " has reached the maximum number of "
                     << "executors";
        // Pass a default filter to avoid getting this same offer immediately
        // from the allocator.
        allocator->recoverResources(frameworkId, slaveId, offered, Filters());
        continue;
      }
    }
#endif // WITH_NETWORK_ISOLATOR

    // TODO(vinod): Split regular and revocable resources into
    // separate offers, so that rescinding offers with revocable
    // resources does not affect offers with regular resources.

    // TODO(bmahler): Set "https" if only "https" is supported.
    mesos::URL url;
    url.set_scheme("http");
    url.mutable_address()->set_hostname(slave->info.hostname());
    url.mutable_address()->set_ip(stringify(slave->pid.address.ip));
    url.mutable_address()->set_port(slave->pid.address.port);
    url.set_path("/" + slave->pid.id);

    Offer* offer = new Offer();
    offer->mutable_id()->MergeFrom(newOfferId());
    offer->mutable_framework_id()->MergeFrom(framework->id());
    offer->mutable_slave_id()->MergeFrom(slave->id);
    offer->set_hostname(slave->info.hostname());
    offer->mutable_url()->MergeFrom(url);
    offer->mutable_resources()->MergeFrom(offered);
    offer->mutable_attributes()->MergeFrom(slave->info.attributes());

    // Add all framework's executors running on this slave.
    if (slave->executors.contains(framework->id())) {
      const hashmap<ExecutorID, ExecutorInfo>& executors =
        slave->executors[framework->id()];
      foreachkey (const ExecutorID& executorId, executors) {
        offer->add_executor_ids()->MergeFrom(executorId);
      }
    }

    // If the slave in this offer is planned to be unavailable due to
    // maintenance in the future, then set the Unavailability.
    CHECK(machines.contains(slave->machineId));
    if (machines[slave->machineId].info.has_unavailability()) {
      offer->mutable_unavailability()->CopyFrom(
          machines[slave->machineId].info.unavailability());
    }

    offers[offer->id()] = offer;

    framework->addOffer(offer);
    slave->addOffer(offer);

    if (flags.offer_timeout.isSome()) {
      // Rescind the offer after the timeout elapses.
      offerTimers[offer->id()] =
        delay(flags.offer_timeout.get(),
              self(),
              &Self::offerTimeout,
              offer->id());
    }

    // TODO(jieyu): For now, we strip 'ephemeral_ports' resource from
    // offers so that frameworks do not see this resource. This is a
    // short term workaround. Revisit this once we resolve MESOS-1654.
    Offer offer_ = *offer;
    offer_.clear_resources();

    foreach (const Resource& resource, offered) {
      if (resource.name() != "ephemeral_ports") {
        offer_.add_resources()->CopyFrom(resource);
      }
    }

    // Add the offer *AND* the corresponding slave's PID.
    message.add_offers()->MergeFrom(offer_);
    message.add_pids(slave->pid);
  }

  if (message.offers().size() == 0) {
    return;
  }

  LOG(INFO) << "Sending " << message.offers().size()
            << " offers to framework " << *framework;

  framework->send(message);
}


void Master::inverseOffer(
    const FrameworkID& frameworkId,
    const hashmap<SlaveID, UnavailableResources>& resources)
{
  if (!frameworks.registered.contains(frameworkId) ||
      !frameworks.registered[frameworkId]->active) {
    LOG(INFO) << "Master ignoring inverse offers to framework " << frameworkId
              << " because the framework has terminated or is inactive";
    return;
  }

  // Create an inverse offer for each slave and add it to the message.
  ResourceOffersMessage message;

  Framework* framework = CHECK_NOTNULL(frameworks.registered[frameworkId]);
  foreachpair (const SlaveID& slaveId,
               const UnavailableResources& unavailableResources,
               resources) {
    if (!slaves.registered.contains(slaveId)) {
      LOG(INFO)
        << "Master ignoring inverse offers to framework " << *framework
        << " because slave " << slaveId << " is not valid";
      continue;
    }

    Slave* slave = slaves.registered.get(slaveId);
    CHECK_NOTNULL(slave);

    // This could happen if the allocator dispatched 'Master::inverseOffer'
    // before the slave was deactivated in the allocator.
    if (!slave->active) {
      LOG(INFO)
        << "Master ignoring inverse offers because slave " << *slave
        << " is " << (slave->connected ? "deactivated" : "disconnected");

      continue;
    }

    // TODO(bmahler): Set "https" if only "https" is supported.
    mesos::URL url;
    url.set_scheme("http");
    url.mutable_address()->set_hostname(slave->info.hostname());
    url.mutable_address()->set_ip(stringify(slave->pid.address.ip));
    url.mutable_address()->set_port(slave->pid.address.port);
    url.set_path("/" + slave->pid.id);

    InverseOffer* inverseOffer = new InverseOffer();

    // We use the same id generator as regular offers so that we can have unique
    // ids accross both. This way we can re-use some of the `OfferID` only
    // messages.
    inverseOffer->mutable_id()->CopyFrom(newOfferId());
    inverseOffer->mutable_framework_id()->CopyFrom(framework->id());
    inverseOffer->mutable_slave_id()->CopyFrom(slave->id);
    inverseOffer->mutable_url()->CopyFrom(url);
    inverseOffer->mutable_unavailability()->CopyFrom(
        unavailableResources.unavailability);

    inverseOffers[inverseOffer->id()] = inverseOffer;

    framework->addInverseOffer(inverseOffer);
    slave->addInverseOffer(inverseOffer);

    // TODO(jmlvanre): Do we want a separate flag for inverse offer
    // timeout?
    if (flags.offer_timeout.isSome()) {
      // Rescind the inverse offer after the timeout elapses.
      inverseOfferTimers[inverseOffer->id()] =
        delay(flags.offer_timeout.get(),
              self(),
              &Self::inverseOfferTimeout,
              inverseOffer->id());
    }

    // Add the inverse offer *AND* the corresponding slave's PID.
    message.add_inverse_offers()->CopyFrom(*inverseOffer);
    message.add_pids(slave->pid);
  }

  if (message.inverse_offers().size() == 0) {
    return;
  }

  LOG(INFO) << "Sending " << message.inverse_offers().size()
            << " inverse offers to framework " << *framework;

  framework->send(message);
}


// TODO(vinod): If due to network partition there are two instances
// of the framework that think they are leaders and try to
// authenticate with master they would be stepping on each other's
// toes. Currently it is tricky to detect this case because the
// 'authenticate' message doesn't contain the 'FrameworkID'.
// 'from' is the authenticatee process with which to communicate.
// 'pid' is the framework/slave process being authenticated.
void Master::authenticate(const UPID& from, const UPID& pid)
{
  ++metrics->messages_authenticate;

  // An authentication request is sent by a client (slave/framework)
  // in the following cases:
  //
  // 1. First time the client is connecting.
  //    This is straightforward; just proceed with authentication.
  //
  // 2. Client retried because of ZK expiration / authentication timeout.
  //    If the client is already authenticated, it will be removed from
  //    the 'authenticated' map and authentication is retried.
  //
  // 3. Client restarted.
  //   3.1. We are here after receiving 'exited()' from old client.
  //        This is safe because the client will be first marked as
  //        disconnected and then when it re-registers it will be
  //        marked as connected.
  //
  //  3.2. We are here before receiving 'exited()' from old client.
  //       This is tricky only if the PID of the client doesn't change
  //       after restart; true for slave but not for framework.
  //       If the PID doesn't change the master might mark the client
  //       disconnected *after* the client re-registers.
  //       This is safe because the client (slave) will be informed
  //       about this discrepancy via ping messages so that it can
  //       re-register.

  authenticated.erase(pid);

  if (authenticator.isNone()) {
    // The default authenticator is CRAM-MD5 rather than none.
    // Since the default parameters specify CRAM-MD5 authenticator, no
    // required authentication, and no credentials, we must support
    // this for starting successfully.
    // In this case, we must allow non-authenticating frameworks /
    // slaves to register without authentication, but we will return
    // an AuthenticationError if they actually try to authenticate.

    // TODO(tillt): We need to make sure this does not cause retries.
    // See MESOS-2379.
    LOG(ERROR) << "Received authentication request from " << pid
               << " but authenticator is not loaded";

    AuthenticationErrorMessage message;
    message.set_error("No authenticator loaded");
    send(from, message);

    return;
  }

  if (authenticating.contains(pid)) {
    LOG(INFO) << "Queuing up authentication request from " << pid
              << " because authentication is still in progress";

    // Try to cancel the in progress authentication by discarding the
    // future.
    authenticating[pid].discard();

    // Retry after the current authenticator session finishes.
    authenticating[pid]
      .onAny(defer(self(), &Self::authenticate, from, pid));

    return;
  }

  LOG(INFO) << "Authenticating " << pid;

  // Start authentication.
  const Future<Option<string>> future = authenticator.get()->authenticate(from);

  // Save our state.
  authenticating[pid] = future;

  future.onAny(defer(self(), &Self::_authenticate, pid, lambda::_1));

  // Don't wait for authentication to complete forever.
  delay(Seconds(5),
        self(),
        &Self::authenticationTimeout,
        future);
}


void Master::_authenticate(
    const UPID& pid,
    const Future<Option<string>>& future)
{
  if (!future.isReady() || future.get().isNone()) {
    const string& error = future.isReady()
        ? "Refused authentication"
        : (future.isFailed() ? future.failure() : "future discarded");

    LOG(WARNING) << "Failed to authenticate " << pid
                 << ": " << error;
  } else {
    LOG(INFO) << "Successfully authenticated principal '" << future.get().get()
              << "' at " << pid;

    authenticated.put(pid, future.get().get());
  }

  authenticating.erase(pid);
}


void Master::authenticationTimeout(Future<Option<string>> future)
{
  // Note that a 'discard' here is safe even if another
  // authenticator is in progress because this copy of the future
  // corresponds to the original authenticator that started the timer.
  if (future.discard()) { // This is a no-op if the future is already ready.
    LOG(WARNING) << "Authentication timed out";
  }
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

  // TODO(bmahler): There's an implicit assumption here the slave
  // cannot have tasks unknown to the master. This _should_ be the
  // case since the causal relationship is:
  //   slave removes task -> master removes task
  // Add error logging for any violations of this assumption!

  // We convert the 'tasks' into a map for easier lookup below.
  multihashmap<FrameworkID, TaskID> slaveTasks;
  foreach (const Task& task, tasks) {
    slaveTasks.put(task.framework_id(), task.task_id());
  }

  // Look for tasks missing in the slave's re-registration message.
  // This can occur when:
  //   (1) a launch message was dropped (e.g. slave failed over), or
  //   (2) the slave re-registration raced with a launch message, in
  //       which case the slave actually received the task.
  // To resolve both cases correctly, we must reconcile through the
  // slave. For slaves that do not support reconciliation, we keep
  // the old semantics and cover only case (1) via TASK_LOST.
  Duration pingTimeout =
    flags.slave_ping_timeout * flags.max_slave_ping_timeouts;
  MasterSlaveConnection connection;
  connection.set_total_ping_timeout_seconds(pingTimeout.secs());

  SlaveReregisteredMessage reregistered;
  reregistered.mutable_slave_id()->CopyFrom(slave->id);
  reregistered.mutable_connection()->CopyFrom(connection);

  // NOTE: copies are needed because removeTask modified slave->tasks.
  foreachkey (const FrameworkID& frameworkId, utils::copy(slave->tasks)) {
    ReconcileTasksMessage reconcile;
    reconcile.mutable_framework_id()->CopyFrom(frameworkId);

    foreachvalue (Task* task, utils::copy(slave->tasks[frameworkId])) {
      if (!slaveTasks.contains(task->framework_id(), task->task_id())) {
        LOG(WARNING) << "Task " << task->task_id()
                     << " of framework " << task->framework_id()
                     << " unknown to the slave " << *slave
                     << " during re-registration"
                     << (slave->version.isSome()
                         ? ": reconciling with the slave"
                         : ": sending TASK_LOST");

        if (slave->version.isSome()) {
          // NOTE: Currently the slave doesn't look at the task state
          // when it reconciles the task state; we include the correct
          // state for correctness and consistency.
          const TaskState& state = task->has_status_update_state()
              ? task->status_update_state()
              : task->state();

          TaskStatus* status = reconcile.add_statuses();
          status->mutable_task_id()->CopyFrom(task->task_id());
          status->mutable_slave_id()->CopyFrom(slave->id);
          status->set_state(state);
          status->set_source(TaskStatus::SOURCE_MASTER);
          status->set_message("Reconciliation request");
          status->set_reason(TaskStatus::REASON_RECONCILIATION);
          status->set_timestamp(Clock::now().secs());
        } else {
          // TODO(bmahler): Remove this case in 0.22.0.
          const StatusUpdate& update = protobuf::createStatusUpdate(
              task->framework_id(),
              slave->id,
              task->task_id(),
              TASK_LOST,
              TaskStatus::SOURCE_MASTER,
              None(),
              "Task is unknown to the slave",
              TaskStatus::REASON_TASK_UNKNOWN);

          updateTask(task, update);
          removeTask(task);

          Framework* framework = getFramework(frameworkId);
          if (framework != NULL) {
            forward(update, UPID(), framework);
          }
        }
      }
    }

    if (slave->version.isSome() && reconcile.statuses_size() > 0) {
      reregistered.add_reconciliations()->CopyFrom(reconcile);
    }
  }

  // Re-register the slave.
  send(slave->pid, reregistered);

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
        // TODO(bmahler): Reconcile executors correctly between the
        // master and the slave, see:
        // MESOS-1466, MESOS-1800, and MESOS-1720.
        LOG(WARNING) << "Executor " << executorId
                     << " of framework " << frameworkId
                     << " possibly unknown to the slave " << *slave;

        removeExecutor(slave, frameworkId, executorId);
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
    if (slaveTasks.contains(framework->id())) {
      LOG(WARNING) << "Slave " << *slave
                   << " re-registered with completed framework " << *framework
                   << ". Shutting down the framework on the slave";

      ShutdownFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id());
      send(slave->pid, message);
    }
  }
}


void Master::addFramework(Framework* framework)
{
  CHECK_NOTNULL(framework);

  CHECK(!frameworks.registered.contains(framework->id()))
    << "Framework " << *framework << " already exists!";

  frameworks.registered[framework->id()] = framework;

  if (framework->pid.isSome()) {
    link(framework->pid.get());
  } else {
    CHECK_SOME(framework->http);

    HttpConnection http = framework->http.get();

    http.closed()
      .onAny(defer(self(), &Self::exited, framework->id(), http));
  }

  // Enforced by Master::registerFramework.
  CHECK(roles.contains(framework->info.role()))
    << "Unknown role " << framework->info.role()
    << " of framework " << *framework;

  roles[framework->info.role()]->addFramework(framework);

  // There should be no offered resources yet!
  CHECK_EQ(Resources(), framework->totalOfferedResources);

  allocator->addFramework(
      framework->id(),
      framework->info,
      framework->usedResources);

  // Export framework metrics.

  // If the framework is authenticated, its principal should be in
  // 'authenticated'. Otherwise look if it's supplied in
  // FrameworkInfo.
  if (framework->pid.isSome()) {
    Option<string> principal = authenticated.get(framework->pid.get());
    if (principal.isNone() && framework->info.has_principal()) {
      principal = framework->info.principal();
    }

    CHECK(!frameworks.principals.contains(framework->pid.get()));
    frameworks.principals.put(framework->pid.get(), principal);

    // Export framework metrics if a principal is specified.
    if (principal.isSome()) {
      // Create new framework metrics if this framework is the first
      // one of this principal. Otherwise existing metrics are reused.
      if (!metrics->frameworks.contains(principal.get())) {
        metrics->frameworks.put(
            principal.get(),
            Owned<Metrics::Frameworks>(
              new Metrics::Frameworks(principal.get())));
      }
    }
  }
}


void Master::failoverFramework(Framework* framework, const HttpConnection& http)
{
  // Notify the old connected framework that it has failed over.
  // Note that this may be a retry in which case we'll shut down
  // the scheduler unnecessarily.
  if (framework->connected) {
    FrameworkErrorMessage message;
    message.set_message("Framework failed over");
    framework->send(message);
  }

  // If this is an upgrade, clear the authentication related data.
  if (framework->pid.isSome()) {
    authenticated.erase(framework->pid.get());

    CHECK(frameworks.principals.contains(framework->pid.get()));
    Option<string> principal = frameworks.principals[framework->pid.get()];

    frameworks.principals.erase(framework->pid.get());

    // Remove the metrics for the principal if this framework is the
    // last one with this principal.
    if (principal.isSome() &&
        !frameworks.principals.containsValue(principal.get())) {
      CHECK(metrics->frameworks.contains(principal.get()));
      metrics->frameworks.erase(principal.get());
    }
  }

  framework->updateConnection(http);
  http.closed()
    .onAny(defer(self(), &Self::exited, framework->id(), http));

  _failoverFramework(framework);

  // Start the heartbeat after sending SUBSCRIBED event.
  framework->heartbeat();
}


// Replace the scheduler for a framework with a new process ID, in the
// event of a scheduler failover.
void Master::failoverFramework(Framework* framework, const UPID& newPid)
{
  const Option<UPID> oldPid = framework->pid;

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
  if (oldPid.isSome() && oldPid != newPid) {
    FrameworkErrorMessage message;
    message.set_message("Framework failed over");
    framework->send(message);
  }

  framework->updateConnection(newPid);
  link(newPid);

  _failoverFramework(framework);

  CHECK_SOME(framework->pid);

  // Update the principal mapping for this framework, which is
  // needed to keep the per-principal framework metrics accurate.
  if (oldPid.isSome() && frameworks.principals.contains(oldPid.get())) {
    frameworks.principals.erase(oldPid.get());
  }

  frameworks.principals[newPid] = authenticated.get(newPid);
}


void Master::_failoverFramework(Framework* framework)
{
  // Remove the framework's offers (if they weren't removed before).
  // We do this after we have updated the pid and sent the framework
  // registered message so that the allocator can immediately re-offer
  // these resources to this framework if it wants.
  foreach (Offer* offer, utils::copy(framework->offers)) {
    allocator->recoverResources(
        offer->framework_id(), offer->slave_id(), offer->resources(), None());

    removeOffer(offer);
  }

  // Also remove the inverse offers.
  foreach (InverseOffer* inverseOffer, utils::copy(framework->inverseOffers)) {
    allocator->updateInverseOffer(
        inverseOffer->slave_id(),
        inverseOffer->framework_id(),
        UnavailableResources{
            inverseOffer->resources(),
            inverseOffer->unavailability()},
        None());

    removeInverseOffer(inverseOffer);
  }

  // Reconnect and reactivate the framework.
  framework->connected = true;

  // Reactivate the framework.
  // NOTE: We do this after recovering resources (above) so that
  // the allocator has the correct view of the framework's share.
  if (!framework->active) {
    framework->active = true;
    allocator->activateFramework(framework->id());
  }

  // The scheduler driver safely ignores any duplicate registration
  // messages, so we don't need to compare the old and new pids here.
  FrameworkRegisteredMessage message;
  message.mutable_framework_id()->MergeFrom(framework->id());
  message.mutable_master_info()->MergeFrom(info_);
  framework->send(message);
}


void Master::teardown(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Processing TEARDOWN call for framework " << *framework;

  ++metrics->messages_unregister_framework;

  removeFramework(framework);
}


void Master::removeFramework(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Removing framework " << *framework;

  if (framework->active) {
    // Tell the allocator to stop allocating resources to this framework.
    // TODO(vinod): Consider setting  framework->active to false here
    // or just calling 'deactivate(Framework*)'.
    allocator->deactivateFramework(framework->id());
  }

  // Tell slaves to shutdown the framework.
  foreachvalue (Slave* slave, slaves.registered) {
    ShutdownFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id());
    send(slave->pid, message);
  }

  // Remove the pending tasks from the framework.
  framework->pendingTasks.clear();

  // Remove pointers to the framework's tasks in slaves.
  foreachvalue (Task* task, utils::copy(framework->tasks)) {
    Slave* slave = slaves.registered.get(task->slave_id());

    // Since we only find out about tasks when the slave re-registers,
    // it must be the case that the slave exists!
    CHECK(slave != NULL)
      << "Unknown slave " << task->slave_id()
      << " for task " << task->task_id();

    // The task is implicitly killed, and TASK_KILLED is the closest
    // state we have by now. We mark the task and remove it, without
    // sending the update. However, a task may finish during the
    // executor graceful shutdown period. By marking such task as
    // killed and moving it to completed, we lose the opportunity to
    // collect the possible finished status. We tolerate this,
    // because we expect that if the framework has been asked to shut
    // down, its user is not interested in results anymore.
    // TODO(alex): Consider a more descriptive state, e.g. TASK_ABANDONED.
    const StatusUpdate& update = protobuf::createStatusUpdate(
        task->framework_id(),
        task->slave_id(),
        task->task_id(),
        TASK_KILLED,
        TaskStatus::SOURCE_MASTER,
        None(),
        "Framework " + framework->id().value() + " removed",
        TaskStatus::REASON_FRAMEWORK_REMOVED,
        (task->has_executor_id()
         ? Option<ExecutorID>(task->executor_id())
         : None()));

    updateTask(task, update);
    removeTask(task);
  }

  // Remove the framework's offers (if they weren't removed before).
  foreach (Offer* offer, utils::copy(framework->offers)) {
    allocator->recoverResources(
        offer->framework_id(),
        offer->slave_id(),
        offer->resources(),
        None());

    removeOffer(offer);
  }

  // Also remove the inverse offers.
  foreach (InverseOffer* inverseOffer, utils::copy(framework->inverseOffers)) {
    allocator->updateInverseOffer(
        inverseOffer->slave_id(),
        inverseOffer->framework_id(),
        UnavailableResources{
            inverseOffer->resources(),
            inverseOffer->unavailability()},
        None());

    removeInverseOffer(inverseOffer);
  }

  // Remove the framework's executors for correct resource accounting.
  foreachkey (const SlaveID& slaveId, utils::copy(framework->executors)) {
    Slave* slave = slaves.registered.get(slaveId);

    if (slave != NULL) {
      foreachkey (const ExecutorID& executorId,
                  utils::copy(framework->executors[slaveId])) {
        removeExecutor(slave, framework->id(), executorId);
      }
    }
  }

  // TODO(benh): Similar code between removeFramework and
  // failoverFramework needs to be shared!

  // TODO(benh): unlink(framework->pid);

  // TODO(anand): For http frameworks, close the connection.

  framework->unregisteredTime = Clock::now();

  CHECK(roles.contains(framework->info.role()))
    << "Unknown role " << framework->info.role()
    << " of framework " << *framework;

  roles[framework->info.role()]->removeFramework(framework);

  // TODO(anand): This only works for pid based frameworks. We would
  // need similar authentication logic for http frameworks.
  if (framework->pid.isSome()) {
    authenticated.erase(framework->pid.get());

    CHECK(frameworks.principals.contains(framework->pid.get()));
    Option<string> principal = frameworks.principals[framework->pid.get()];

    frameworks.principals.erase(framework->pid.get());

    // Remove the metrics for the principal if this framework is the
    // last one with this principal.
    if (principal.isSome() &&
        !frameworks.principals.containsValue(principal.get())) {
      CHECK(metrics->frameworks.contains(principal.get()));
      metrics->frameworks.erase(principal.get());
    }
  }

  // Remove the framework.
  frameworks.registered.erase(framework->id());
  allocator->removeFramework(framework->id());

  // The completedFramework buffer now owns the framework pointer.
  frameworks.completed.push_back(shared_ptr<Framework>(framework));
}


void Master::removeFramework(Slave* slave, Framework* framework)
{
  CHECK_NOTNULL(slave);
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Removing framework " << *framework
            << " from slave " << *slave;

  // Remove pointers to framework's tasks in slaves, and send status
  // updates.
  // NOTE: A copy is needed because removeTask modifies slave->tasks.
  foreachvalue (Task* task, utils::copy(slave->tasks[framework->id()])) {
    // Remove tasks that belong to this framework.
    if (task->framework_id() == framework->id()) {
      // A framework might not actually exist because the master failed
      // over and the framework hasn't reconnected yet. For more info
      // please see the comments in 'removeFramework(Framework*)'.
      const StatusUpdate& update = protobuf::createStatusUpdate(
        task->framework_id(),
        task->slave_id(),
        task->task_id(),
        TASK_LOST,
        TaskStatus::SOURCE_MASTER,
        None(),
        "Slave " + slave->info.hostname() + " disconnected",
        TaskStatus::REASON_SLAVE_DISCONNECTED,
        (task->has_executor_id()
            ? Option<ExecutorID>(task->executor_id()) : None()));

      updateTask(task, update);
      removeTask(task);
      forward(update, UPID(), framework);
    }
  }

  // Remove the framework's executors from the slave and framework
  // for proper resource accounting.
  if (slave->executors.contains(framework->id())) {
    foreachkey (const ExecutorID& executorId,
                utils::copy(slave->executors[framework->id()])) {
      removeExecutor(slave, framework->id(), executorId);
    }
  }
}


void Master::addSlave(
    Slave* slave,
    const vector<Archive::Framework>& completedFrameworks)
{
  CHECK_NOTNULL(slave);

  slaves.removed.erase(slave->id);
  slaves.registered.put(slave);

  link(slave->pid);

  // Map the slave to the machine it is running on.
  CHECK(!machines[slave->machineId].slaves.contains(slave->id));
  machines[slave->machineId].slaves.insert(slave->id);

  // Set up an observer for the slave.
  slave->observer = new SlaveObserver(
      slave->pid,
      slave->info,
      slave->id,
      self(),
      slaves.limiter,
      metrics,
      flags.slave_ping_timeout,
      flags.max_slave_ping_timeouts);

  spawn(slave->observer);

  // Add the slave's executors to the frameworks.
  foreachkey (const FrameworkID& frameworkId, slave->executors) {
    foreachvalue (const ExecutorInfo& executorInfo,
                  slave->executors[frameworkId]) {
      Framework* framework = getFramework(frameworkId);
      if (framework != NULL) { // The framework might not be re-registered yet.
        framework->addExecutor(slave->id, executorInfo);
      }
    }
  }

  // Add the slave's tasks to the frameworks.
  foreachkey (const FrameworkID& frameworkId, slave->tasks) {
    foreachvalue (Task* task, slave->tasks[frameworkId]) {
      Framework* framework = getFramework(task->framework_id());
      if (framework != NULL) { // The framework might not be re-registered yet.
        framework->addTask(task);
      } else {
        // TODO(benh): We should really put a timeout on how long we
        // keep tasks running on a slave that never have frameworks
        // reregister and claim them.
        LOG(WARNING) << "Possibly orphaned task " << task->task_id()
                     << " of framework " << task->framework_id()
                     << " running on slave " << *slave;
      }
    }
  }

  // Re-add completed tasks reported by the slave.
  // Note that a slave considers a framework completed when it has no
  // tasks/executors running for that framework. But a master considers a
  // framework completed when the framework is removed after a failover timeout.
  // TODO(vinod): Reconcile the notion of a completed framework across the
  // master and slave.
  foreach (const Archive::Framework& completedFramework, completedFrameworks) {
    Framework* framework = getFramework(
        completedFramework.framework_info().id());

    foreach (const Task& task, completedFramework.tasks()) {
      if (framework != NULL) {
        VLOG(2) << "Re-adding completed task " << task.task_id()
                << " of framework " << *framework
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

  CHECK(machines.contains(slave->machineId));

  // Only set unavailability if the protobuf has one set.
  Option<Unavailability> unavailability = None();
  if (machines[slave->machineId].info.has_unavailability()) {
    unavailability = machines[slave->machineId].info.unavailability();
  }

  allocator->addSlave(
      slave->id,
      slave->info,
      unavailability,
      slave->totalResources,
      slave->usedResources);
}


void Master::removeSlave(
    Slave* slave,
    const string& message,
    Option<Counter> reason)
{
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Removing slave " << *slave << ": " << message;

  // We want to remove the slave first, to avoid the allocator
  // re-allocating the recovered resources.
  //
  // NOTE: Removing the slave is not sufficient for recovering the
  // resources in the allocator, because the "Sorters" are updated
  // only within recoverResources() (see MESOS-621). The calls to
  // recoverResources() below are therefore required, even though
  // the slave is already removed.
  allocator->removeSlave(slave->id);

  // Transition the tasks to lost and remove them, BUT do not send
  // updates. Rather, build up the updates so that we can send them
  // after the slave is removed from the registry.
  vector<StatusUpdate> updates;
  foreachkey (const FrameworkID& frameworkId, utils::copy(slave->tasks)) {
    foreachvalue (Task* task, utils::copy(slave->tasks[frameworkId])) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          task->framework_id(),
          task->slave_id(),
          task->task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_MASTER,
          None(),
          "Slave " + slave->info.hostname() + " removed: " + message,
          TaskStatus::REASON_SLAVE_REMOVED,
          (task->has_executor_id() ?
              Option<ExecutorID>(task->executor_id()) : None()));

      updateTask(task, update);
      removeTask(task);

      updates.push_back(update);
    }
  }

  // Remove executors from the slave for proper resource accounting.
  foreachkey (const FrameworkID& frameworkId, utils::copy(slave->executors)) {
    foreachkey (const ExecutorID& executorId,
                utils::copy(slave->executors[frameworkId])) {
      removeExecutor(slave, frameworkId, executorId);
    }
  }

  foreach (Offer* offer, utils::copy(slave->offers)) {
    // TODO(vinod): We don't need to call 'Allocator::recoverResources'
    // once MESOS-621 is fixed.
    allocator->recoverResources(
        offer->framework_id(), slave->id, offer->resources(), None());

    // Remove and rescind offers.
    removeOffer(offer, true); // Rescind!
  }

  // Remove inverse offers because sending them for a slave that is
  // gone doesn't make sense.
  foreach (InverseOffer* inverseOffer, utils::copy(slave->inverseOffers)) {
    // We don't need to update the allocator because we've already called
    // `RemoveSlave()`.
    // Remove and rescind inverse offers.
    removeInverseOffer(inverseOffer, true); // Rescind!
  }

  // Mark the slave as being removed.
  slaves.removing.insert(slave->id);
  slaves.registered.remove(slave);
  slaves.removed.put(slave->id, Nothing());
  authenticated.erase(slave->pid);

  // Remove the slave from the `machines` mapping.
  CHECK(machines.contains(slave->machineId));
  CHECK(machines[slave->machineId].slaves.contains(slave->id));
  machines[slave->machineId].slaves.erase(slave->id);

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
                 lambda::_1,
                 message,
                 reason));

  delete slave;
}


void Master::_removeSlave(
    const SlaveInfo& slaveInfo,
    const vector<StatusUpdate>& updates,
    const Future<bool>& removed,
    const string& message,
    Option<Counter> reason)
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
            << slaveInfo.hostname() << "): " << message;

  ++metrics->slave_removals;

  if (reason.isSome()) {
    ++utils::copy(reason.get()); // Remove const.
  }

  // Forward the LOST updates on to the framework.
  foreach (const StatusUpdate& update, updates) {
    Framework* framework = getFramework(update.framework_id());

    if (framework == NULL) {
      LOG(WARNING) << "Dropping update " << update << " from unknown framework "
                   << update.framework_id();
    } else {
      forward(update, UPID(), framework);
    }
  }

  // Notify all frameworks of the lost slave.
  foreachvalue (Framework* framework, frameworks.registered) {
    LOG(INFO) << "Notifying framework " << *framework << " of lost slave "
              << slaveInfo.id() << " (" << slaveInfo.hostname() << ") "
              << "after recovering";
    LostSlaveMessage message;
    message.mutable_slave_id()->MergeFrom(slaveInfo.id());
    framework->send(message);
  }

  // Finally, notify the `SlaveLost` hooks.
  if (HookManager::hooksAvailable()) {
    HookManager::masterSlaveLostHook(slaveInfo);
  }
}


void Master::updateTask(Task* task, const StatusUpdate& update)
{
  CHECK_NOTNULL(task);

  // Get the unacknowledged status.
  const TaskStatus& status = update.status();

  // Out-of-order updates should not occur, however in case they
  // do (e.g., due to bugs), prevent them here to ensure that the
  // resource accounting is not affected.
  if (protobuf::isTerminalState(task->state()) &&
      !protobuf::isTerminalState(status.state())) {
    LOG(ERROR) << "Ignoring out of order status update for task "
               << task->task_id()
               << " (" << task->state() << " -> " << status.state() << ")"
               << " of framework " << task->framework_id();
    return;
  }

  // Get the latest state.
  Option<TaskState> latestState;
  if (update.has_latest_state()) {
    latestState = update.latest_state();
  }

  // Set 'terminated' to true if this is the first time the task
  // transitioned to terminal state. Also set the latest state.
  bool terminated;
  if (latestState.isSome()) {
    // This update must be from >= 0.21.0 slave.
    terminated = !protobuf::isTerminalState(task->state()) &&
                 protobuf::isTerminalState(latestState.get());

    task->set_state(latestState.get());
  } else {
    // This update must be from a pre 0.21.0 slave or generated by the
    // master.
    terminated = !protobuf::isTerminalState(task->state()) &&
                 protobuf::isTerminalState(status.state());

    task->set_state(status.state());
  }

  // Set the status update state and uuid for the task. Note that
  // master-generated updates are terminal and do not have a uuid
  // (in which case the master also calls removeTask()).
  if (update.has_uuid() && update.uuid() != "") {
    task->set_status_update_state(status.state());
    task->set_status_update_uuid(update.uuid());
  }

  // TODO(brenden) Consider wiping the `message` field?
  if (task->statuses_size() > 0 &&
      task->statuses(task->statuses_size() - 1).state() == status.state()) {
    task->mutable_statuses()->RemoveLast();
  }
  task->add_statuses()->CopyFrom(status);

  // Delete data (maybe very large since it's stored by on-top framework) we
  // are not interested in to avoid OOM.
  // For example: mesos-master is running on a machine with 4GB free memory,
  // if every task stores 10MB data into TaskStatus, then mesos-master will be
  // killed by OOM killer after have 400 tasks finished.
  // MESOS-1746.
  task->mutable_statuses(task->statuses_size() - 1)->clear_data();

  LOG(INFO) << "Updating the latest state of task " << task->task_id()
            << " of framework " << task->framework_id()
            << " to " << task->state()
            << (task->state() != status.state()
                ? " (status update state: " + stringify(status.state()) + ")"
                : "");

  // Once the task becomes terminal, we recover the resources.
  if (terminated) {
    allocator->recoverResources(
        task->framework_id(),
        task->slave_id(),
        task->resources(),
        None());

    // The slave owns the Task object and cannot be NULL.
    Slave* slave = slaves.registered.get(task->slave_id());
    CHECK_NOTNULL(slave);

    slave->taskTerminated(task);

    Framework* framework = getFramework(task->framework_id());
    if (framework != NULL) {
      framework->taskTerminated(task);
    }

    switch (status.state()) {
      case TASK_FINISHED: ++metrics->tasks_finished; break;
      case TASK_FAILED:   ++metrics->tasks_failed;   break;
      case TASK_KILLED:   ++metrics->tasks_killed;   break;
      case TASK_LOST:     ++metrics->tasks_lost;     break;
      case TASK_ERROR:    ++metrics->tasks_error;    break;
      default:                                       break;
    }

    if (status.has_reason()) {
      metrics->incrementTasksStates(
          status.state(),
          status.source(),
          status.reason());
    }
  }
}


void Master::removeTask(Task* task)
{
  CHECK_NOTNULL(task);

  // The slave owns the Task object and cannot be NULL.
  Slave* slave = slaves.registered.get(task->slave_id());
  CHECK_NOTNULL(slave);

  if (!protobuf::isTerminalState(task->state())) {
    LOG(WARNING) << "Removing task " << task->task_id()
                 << " with resources " << task->resources()
                 << " of framework " << task->framework_id()
                 << " on slave " << *slave
                 << " in non-terminal state " << task->state();

    // If the task is not terminal, then the resources have
    // not yet been recovered.
    allocator->recoverResources(
        task->framework_id(),
        task->slave_id(),
        task->resources(),
        None());
  } else {
    LOG(INFO) << "Removing task " << task->task_id()
              << " with resources " << task->resources()
              << " of framework " << task->framework_id()
              << " on slave " << *slave;
  }

  // Remove from framework.
  Framework* framework = getFramework(task->framework_id());
  if (framework != NULL) { // A framework might not be re-connected yet.
    framework->removeTask(task);
  }

  // Remove from slave.
  slave->removeTask(task);

  delete task;
}


void Master::removeExecutor(
    Slave* slave,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CHECK_NOTNULL(slave);
  CHECK(slave->hasExecutor(frameworkId, executorId));

  ExecutorInfo executor = slave->executors[frameworkId][executorId];

  LOG(INFO) << "Removing executor '" << executorId
            << "' with resources " << executor.resources()
            << " of framework " << frameworkId << " on slave " << *slave;

  allocator->recoverResources(
    frameworkId, slave->id, executor.resources(), None());

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) { // The framework might not be re-registered yet.
    framework->removeExecutor(slave->id, executorId);
  }

  slave->removeExecutor(frameworkId, executorId);
}


void Master::apply(
    Framework* framework,
    Slave* slave,
    const Offer::Operation& operation)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  allocator->updateAllocation(framework->id(), slave->id, {operation});

  _apply(slave, operation);
}


Future<Nothing> Master::apply(Slave* slave, const Offer::Operation& operation)
{
  CHECK_NOTNULL(slave);

  return allocator->updateAvailable(slave->id, {operation})
    .onReady(defer(self(), &Master::_apply, slave, operation));
}


void Master::_apply(Slave* slave, const Offer::Operation& operation) {
  CHECK_NOTNULL(slave);

  slave->apply(operation);

  LOG(INFO) << "Sending checkpointed resources "
            << slave->checkpointedResources
            << " to slave " << *slave;

  CheckpointResourcesMessage message;
  message.mutable_resources()->CopyFrom(slave->checkpointedResources);

  send(slave->pid, message);
}


void Master::offerTimeout(const OfferID& offerId)
{
  Offer* offer = getOffer(offerId);
  if (offer != NULL) {
    allocator->recoverResources(
        offer->framework_id(), offer->slave_id(), offer->resources(), None());
    removeOffer(offer, true);
  }
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
  Slave* slave = slaves.registered.get(offer->slave_id());

  CHECK(slave != NULL)
    << "Unknown slave " << offer->slave_id()
    << " in the offer " << offer->id();

  slave->removeOffer(offer);

  if (rescind) {
    RescindResourceOfferMessage message;
    message.mutable_offer_id()->MergeFrom(offer->id());
    framework->send(message);
  }

  // Remove and cancel offer removal timers. Canceling the Timers is
  // only done to avoid having too many active Timers in libprocess.
  if (offerTimers.contains(offer->id())) {
    Clock::cancel(offerTimers[offer->id()]);
    offerTimers.erase(offer->id());
  }

  // Delete it.
  offers.erase(offer->id());
  delete offer;
}


void Master::inverseOfferTimeout(const OfferID& inverseOfferId)
{
  InverseOffer* inverseOffer = getInverseOffer(inverseOfferId);
  if (inverseOffer != NULL) {
    allocator->updateInverseOffer(
        inverseOffer->slave_id(),
        inverseOffer->framework_id(),
        UnavailableResources{
            inverseOffer->resources(),
            inverseOffer->unavailability()},
        None());

    removeInverseOffer(inverseOffer, true);
  }
}


void Master::removeInverseOffer(InverseOffer* inverseOffer, bool rescind)
{
  // Remove from framework.
  Framework* framework = getFramework(inverseOffer->framework_id());
  CHECK(framework != NULL)
    << "Unknown framework " << inverseOffer->framework_id()
    << " in the inverse offer " << inverseOffer->id();

  framework->removeInverseOffer(inverseOffer);

  // Remove from slave.
  Slave* slave = slaves.registered.get(inverseOffer->slave_id());

  CHECK(slave != NULL)
    << "Unknown slave " << inverseOffer->slave_id()
    << " in the inverse offer " << inverseOffer->id();

  slave->removeInverseOffer(inverseOffer);

  if (rescind) {
    RescindResourceOfferMessage message;
    message.mutable_offer_id()->CopyFrom(inverseOffer->id());
    framework->send(message);
  }

  // Remove and cancel inverse offer removal timers. Canceling the Timers is
  // only done to avoid having too many active Timers in libprocess.
  if (inverseOfferTimers.contains(inverseOffer->id())) {
    Clock::cancel(inverseOfferTimers[inverseOffer->id()]);
    inverseOfferTimers.erase(inverseOffer->id());
  }

  // Delete it.
  inverseOffers.erase(inverseOffer->id());
  delete inverseOffer;
}


// TODO(bmahler): Consider killing this.
Framework* Master::getFramework(const FrameworkID& frameworkId)
{
  return frameworks.registered.contains(frameworkId)
    ? frameworks.registered[frameworkId]
    : NULL;
}


// TODO(bmahler): Consider killing this.
Offer* Master::getOffer(const OfferID& offerId)
{
  return offers.contains(offerId) ? offers[offerId] : NULL;
}


// TODO(bmahler): Consider killing this.
InverseOffer* Master::getInverseOffer(const OfferID& inverseOfferId)
{
  return inverseOffers.contains(inverseOfferId) ?
    inverseOffers[inverseOfferId] : NULL;
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
  offerId.set_value(info_.id() + "-O" + stringify(nextOfferId++));
  return offerId;
}


SlaveID Master::newSlaveId()
{
  SlaveID slaveId;
  slaveId.set_value(info_.id() + "-S" + stringify(nextSlaveId++));
  return slaveId;
}


double Master::_slaves_active()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.registered) {
    if (slave->active) {
      count++;
    }
  }
  return count;
}


double Master::_slaves_inactive()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.registered) {
    if (!slave->active) {
      count++;
    }
  }
  return count;
}


double Master::_slaves_connected()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.registered) {
    if (slave->connected) {
      count++;
    }
  }
  return count;
}


double Master::_slaves_disconnected()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.registered) {
    if (!slave->connected) {
      count++;
    }
  }
  return count;
}


double Master::_frameworks_connected()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks.registered) {
    if (framework->connected) {
      count++;
    }
  }
  return count;
}


double Master::_frameworks_disconnected()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks.registered) {
    if (!framework->connected) {
      count++;
    }
  }
  return count;
}


double Master::_frameworks_active()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks.registered) {
    if (framework->active) {
      count++;
    }
  }
  return count;
}


double Master::_frameworks_inactive()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks.registered) {
    if (!framework->active) {
      count++;
    }
  }
  return count;
}


double Master::_tasks_staging()
{
  double count = 0.0;

  // Add the tasks pending validation / authorization.
  foreachvalue (Framework* framework, frameworks.registered) {
    count += framework->pendingTasks.size();
  }

  foreachvalue (Slave* slave, slaves.registered) {
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

  foreachvalue (Slave* slave, slaves.registered) {
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

  foreachvalue (Slave* slave, slaves.registered) {
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


double Master::_resources_total(const string& name)
{
  double total = 0.0;

  foreachvalue (Slave* slave, slaves.registered) {
    foreach (const Resource& resource, slave->info.resources()) {
      if (resource.name() == name && resource.type() == Value::SCALAR) {
        total += resource.scalar().value();
      }
    }
  }

  return total;
}


double Master::_resources_used(const string& name)
{
  double used = 0.0;

  foreachvalue (Slave* slave, slaves.registered) {
    foreachvalue (const Resources& resources, slave->usedResources) {
      foreach (const Resource& resource, resources - resources.revocable()) {
        if (resource.name() == name && resource.type() == Value::SCALAR) {
          used += resource.scalar().value();
        }
      }
    }
  }

  return used;
}


double Master::_resources_percent(const string& name)
{
  double total = _resources_total(name);

  if (total == 0.0) {
    return 0.0;
  }

  return _resources_used(name) / total;
}


double Master::_resources_revocable_total(const string& name)
{
  double total = 0.0;

  foreachvalue (Slave* slave, slaves.registered) {
    foreach (const Resource& resource, slave->totalResources.revocable()) {
      if (resource.name() == name && resource.type() == Value::SCALAR) {
        total += resource.scalar().value();
      }
    }
  }

  return total;
}


double Master::_resources_revocable_used(const string& name)
{
  double used = 0.0;

  foreachvalue (Slave* slave, slaves.registered) {
    foreachvalue (const Resources& resources, slave->usedResources) {
      foreach (const Resource& resource, resources.revocable()) {
        if (resource.name() == name && resource.type() == Value::SCALAR) {
          used += resource.scalar().value();
        }
      }
    }
  }

  return used;
}


double Master::_resources_revocable_percent(const string& name)
{
  double total = _resources_revocable_total(name);

  if (total == 0.0) {
    return 0.0;
  }

  return _resources_revocable_used(name) / total;
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
