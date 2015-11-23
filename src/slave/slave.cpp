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
#include <stdlib.h> // For random().

#include <algorithm>
#include <iomanip>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <mesos/type_utils.hpp>

#include <mesos/module/authenticatee.hpp>

#include <process/async.hpp>
#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/time.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/fs.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#ifdef __linux__
#include <stout/proc.hpp>
#endif // __linux__
#include <stout/numify.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>
#include <stout/utils.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif // __linux__

#include "authentication/cram_md5/authenticatee.hpp"

#include "common/build.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"
#include "common/status_utils.hpp"

#include "credentials/credentials.hpp"

#include "hook/manager.hpp"

#include "logging/logging.hpp"

#include "module/manager.hpp"

#include "slave/constants.hpp"
#include "slave/flags.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/status_update_manager.hpp"

using mesos::slave::QoSController;
using mesos::slave::QoSCorrection;
using mesos::slave::ResourceEstimator;

using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

using process::async;
using process::wait; // Necessary on some OS's to disambiguate.
using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::Time;
using process::UPID;

namespace mesos {
namespace internal {
namespace slave {

using namespace state;

Slave::Slave(const slave::Flags& _flags,
             MasterDetector* _detector,
             Containerizer* _containerizer,
             Files* _files,
             GarbageCollector* _gc,
             StatusUpdateManager* _statusUpdateManager,
             ResourceEstimator* _resourceEstimator,
             QoSController* _qosController)
  : ProcessBase(process::ID::generate("slave")),
    state(RECOVERING),
    flags(_flags),
    completedFrameworks(MAX_COMPLETED_FRAMEWORKS),
    detector(_detector),
    containerizer(_containerizer),
    files(_files),
    metrics(*this),
    gc(_gc),
    monitor(defer(self(), &Self::usage)),
    statusUpdateManager(_statusUpdateManager),
    masterPingTimeout(DEFAULT_MASTER_PING_TIMEOUT()),
    metaDir(paths::getMetaRootDir(flags.work_dir)),
    recoveryErrors(0),
    credential(None()),
    authenticatee(NULL),
    authenticating(None()),
    authenticated(false),
    reauthenticate(false),
    executorDirectoryMaxAllowedAge(age(0)),
    resourceEstimator(_resourceEstimator),
    qosController(_qosController) {}


Slave::~Slave()
{
  // TODO(benh): Shut down frameworks?

  // TODO(benh): Shut down executors? The executor should get an "exited"
  // event and initiate a shut down itself.

  foreachvalue (Framework* framework, frameworks) {
    delete framework;
  }

  delete authenticatee;
}


lambda::function<void(int, int)>* signaledWrapper = NULL;


static void signalHandler(int sig, siginfo_t* siginfo, void* context)
{
  if (signaledWrapper != NULL) {
    (*signaledWrapper)(sig, siginfo->si_uid);
  }
}


void Slave::signaled(int signal, int uid)
{
  if (signal == SIGUSR1) {
    Result<string> user = os::user(uid);

    shutdown(
        UPID(),
        "Received SIGUSR1 signal" +
        (user.isSome() ? " from user " + user.get() : ""));
  }
}


void Slave::initialize()
{
  LOG(INFO) << "Slave started on " << string(self()).substr(6);
  LOG(INFO) << "Flags at startup: " << flags;

  if (self().address.ip.isLoopback()) {
    LOG(WARNING) << "\n**************************************************\n"
                 << "Slave bound to loopback interface!"
                 << " Cannot communicate with remote master(s)."
                 << " You might want to set '--ip' flag to a routable"
                 << " IP address.\n"
                 << "**************************************************";
  }

#ifdef __linux__
  // Move the slave into its own cgroup for each of the specified
  // subsystems.
  // NOTE: Any subsystem configuration is inherited from the mesos
  // root cgroup for that subsystem, e.g., by default the memory
  // cgroup will be unlimited.
  if (flags.slave_subsystems.isSome()) {
    foreach (const string& subsystem,
            strings::tokenize(flags.slave_subsystems.get(), ",")) {
      LOG(INFO) << "Moving slave process into its own cgroup for"
                << " subsystem: " << subsystem;

      // Ensure the subsystem is mounted and the Mesos root cgroup is
      // present.
      Try<string> hierarchy = cgroups::prepare(
          flags.cgroups_hierarchy,
          subsystem,
          flags.cgroups_root);

      if (hierarchy.isError()) {
        EXIT(1) << "Failed to prepare cgroup " << flags.cgroups_root
                << " for subsystem " << subsystem
                << ": " << hierarchy.error();
      }

      // Create a cgroup for the slave.
      string cgroup = path::join(flags.cgroups_root, "slave");

      Try<bool> exists = cgroups::exists(hierarchy.get(), cgroup);
      if (exists.isError()) {
        EXIT(1) << "Failed to find cgroup " << cgroup
                << " for subsystem " << subsystem
                << " under hierarchy " << hierarchy.get()
                << " for slave: " << exists.error();
      }

      if (!exists.get()) {
        Try<Nothing> create = cgroups::create(hierarchy.get(), cgroup);
        if (create.isError()) {
          EXIT(1) << "Failed to create cgroup " << cgroup
                  << " for subsystem " << subsystem
                  << " under hierarchy " << hierarchy.get()
                  << " for slave: " << create.error();
        }
      }

      // Exit if there are processes running inside the cgroup - this
      // indicates a prior slave (or child process) is still running.
      Try<set<pid_t>> processes = cgroups::processes(hierarchy.get(), cgroup);
      if (processes.isError()) {
        EXIT(1) << "Failed to check for existing threads in cgroup " << cgroup
                << " for subsystem " << subsystem
                << " under hierarchy " << hierarchy.get()
                << " for slave: " << processes.error();
      }

      // Log if there are any processes in the slave's cgroup. They
      // may be transient helper processes like 'perf' or 'du',
      // ancillary processes like 'docker log' or possibly a stuck
      // slave.
      // TODO(idownes): Generally, it's not a problem if there are
      // processes running in the slave's cgroup, though any resources
      // consumed by those processes are accounted to the slave. Where
      // applicable, transient processes should be configured to
      // terminate if the slave exits; see example usage for perf in
      // isolators/cgroups/perf.cpp. Consider moving ancillary
      // processes to a different cgroup, e.g., moving 'docker log' to
      // the container's cgroup.
      if (!processes.get().empty()) {
        // For each process, we print its pid as well as its command
        // to help triaging.
        vector<string> infos;
        foreach (pid_t pid, processes.get()) {
          Result<os::Process> proc = os::process(pid);

          // Only print the command if available.
          if (proc.isSome()) {
            infos.push_back(stringify(pid) + " '" + proc.get().command + "'");
          } else {
            infos.push_back(stringify(pid));
          }
        }

        LOG(INFO) << "A slave (or child process) is still running, please"
                  << " consider checking the following process(es) listed in "
                  << path::join(hierarchy.get(), cgroup, "cgroups.proc")
                  << ":\n" << strings::join("\n", infos);
      }

      // Move all of our threads into the cgroup.
      Try<Nothing> assign = cgroups::assign(hierarchy.get(), cgroup, getpid());
      if (assign.isError()) {
        EXIT(1) << "Failed to move slave into cgroup " << cgroup
                << " for subsystem " << subsystem
                << " under hierarchy " << hierarchy.get()
                << " for slave: " << assign.error();
      }
    }
  }
#endif // __linux__

  if (flags.registration_backoff_factor > REGISTER_RETRY_INTERVAL_MAX) {
    EXIT(1) << "Invalid value '" << flags.registration_backoff_factor << "' "
            << "for --registration_backoff_factor: "
            << "Must be less than " << REGISTER_RETRY_INTERVAL_MAX;
  }

  authenticateeName = flags.authenticatee;

  if (flags.credential.isSome()) {
    Result<Credential> _credential =
      credentials::readCredential(flags.credential.get());
    if (_credential.isError()) {
      EXIT(1) << _credential.error() << " (see --credential flag)";
    } else if (_credential.isNone()) {
      EXIT(1) << "Empty credential file '" << flags.credential.get()
              << "' (see --credential flag)";
    } else {
      credential = _credential.get();
      LOG(INFO) << "Slave using credential for: "
                << credential.get().principal();
    }
  }

  if ((flags.gc_disk_headroom < 0) || (flags.gc_disk_headroom > 1)) {
    EXIT(1) << "Invalid value '" << flags.gc_disk_headroom
            << "' for --gc_disk_headroom. Must be between 0.0 and 1.0.";
  }

  Try<Nothing> initialize =
    resourceEstimator->initialize(defer(self(), &Self::usage));

  if (initialize.isError()) {
    EXIT(1) << "Failed to initialize the resource estimator: "
            << initialize.error();
  }

  initialize = qosController->initialize(defer(self(), &Self::usage));

  if (initialize.isError()) {
    EXIT(1) << "Failed to initialize the QoS Controller: "
            << initialize.error();
  }

  // Ensure slave work directory exists.
  CHECK_SOME(os::mkdir(flags.work_dir))
    << "Failed to create slave work directory '" << flags.work_dir << "'";

  Try<Resources> resources = Containerizer::resources(flags);
  if (resources.isError()) {
    EXIT(1) << "Failed to determine slave resources: " << resources.error();
  }
  LOG(INFO) << "Slave resources: " << resources.get();

  Attributes attributes;
  if (flags.attributes.isSome()) {
    attributes = Attributes::parse(flags.attributes.get());
  }

  // Determine our hostname or use the hostname provided.
  string hostname;

  if (flags.hostname.isNone()) {
    Try<string> result = net::getHostname(self().address.ip);

    if (result.isError()) {
      LOG(FATAL) << "Failed to get hostname: " << result.error();
    }

    hostname = result.get();
  } else {
    hostname = flags.hostname.get();
  }

  // Initialize slave info.
  info.set_hostname(hostname);
  info.set_port(self().address.port);
  info.mutable_resources()->CopyFrom(resources.get());
  info.mutable_attributes()->CopyFrom(attributes);
  // Checkpointing of slaves is always enabled.
  info.set_checkpoint(true);

  LOG(INFO) << "Slave hostname: " << info.hostname();
  // Checkpointing of slaves is always enabled.
  // We keep this line to be compatible with
  // older monitoring tools.
  // TODO(joerg84): Delete after 0.23.
  LOG(INFO) << "Slave checkpoint: " << stringify(true);

  statusUpdateManager->initialize(defer(self(), &Slave::forward, lambda::_1));

  // Start disk monitoring.
  // NOTE: We send a delayed message here instead of directly calling
  // checkDiskUsage, to make disabling this feature easy (e.g by specifying
  // a very large disk_watch_interval).
  delay(flags.disk_watch_interval, self(), &Slave::checkDiskUsage);

  startTime = Clock::now();

  // Install protobuf handlers.
  install<SlaveRegisteredMessage>(
      &Slave::registered,
      &SlaveRegisteredMessage::slave_id,
      &SlaveRegisteredMessage::connection);

  install<SlaveReregisteredMessage>(
      &Slave::reregistered,
      &SlaveReregisteredMessage::slave_id,
      &SlaveReregisteredMessage::reconciliations,
      &SlaveReregisteredMessage::connection);

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

  install<ShutdownExecutorMessage>(
      &Slave::shutdownExecutor,
      &ShutdownExecutorMessage::framework_id,
      &ShutdownExecutorMessage::executor_id);

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

  install<CheckpointResourcesMessage>(
      &Slave::checkpointResources,
      &CheckpointResourcesMessage::resources);

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
      &Slave::shutdown,
      &ShutdownMessage::message);

  // Install the ping message handler.
  // TODO(vinod): Remove this handler in 0.22.0 in favor of the
  // new PingSlaveMessage handler.
  install("PING", &Slave::pingOld);

  install<PingSlaveMessage>(
      &Slave::ping,
      &PingSlaveMessage::connected);

  // Setup HTTP routes.
  Http http = Http(this);

  route("/health",
        Http::HEALTH_HELP,
        [http](const process::http::Request& request) {
          return http.health(request);
        });
  route("/state.json",
        Http::STATE_HELP,
        [http](const process::http::Request& request) {
          Http::log(request);
          return http.state(request);
        });

  // Expose the log file for the webui. Fall back to 'log_dir' if
  // an explicit file was not specified.
  if (flags.external_log_file.isSome()) {
    files->attach(flags.external_log_file.get(), "/slave/log")
      .onAny(defer(self(),
                   &Self::fileAttached,
                   lambda::_1,
                   flags.external_log_file.get()));
  } else if (flags.log_dir.isSome()) {
    Try<string> log =
      logging::getLogFile(logging::getLogSeverity(flags.logging_level));

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

  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));

  // Do not block additional signals while in the handler.
  sigemptyset(&action.sa_mask);

  // The SA_SIGINFO flag tells sigaction() to use
  // the sa_sigaction field, not sa_handler.
  action.sa_flags = SA_SIGINFO;

  signaledWrapper = new lambda::function<void(int, int)>(
      defer(self(), &Slave::signaled, lambda::_1, lambda::_2));

  action.sa_sigaction = signalHandler;

  if (sigaction(SIGUSR1, &action, NULL) < 0) {
    EXIT(1) << "Failed to set sigaction: " << strerror(errno);
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
    // a shut down/kill phases) we might not actually propagate all
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

  if (state == TERMINATING) {
    // We remove the "latest" symlink in meta directory, so that the
    // slave doesn't recover the state when it restarts and registers
    // as a new slave with the master.
    if (os::exists(paths::getLatestSlavePath(metaDir))) {
      CHECK_SOME(os::rm(paths::getLatestSlavePath(metaDir)));
    }
  }
}


void Slave::shutdown(const UPID& from, const string& message)
{
  if (from && master != from) {
    LOG(WARNING) << "Ignoring shutdown message from " << from
                 << " because it is not from the registered master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  if (from) {
    LOG(INFO) << "Slave asked to shut down by " << from
              << (message.empty() ? "" : " because '" + message + "'");
  } else if (info.has_id()) {
    LOG(INFO) << message << "; unregistering and shutting down";

    UnregisterSlaveMessage message_;
    message_.mutable_slave_id()->MergeFrom(info.id());
    send(master.get(), message_);
  } else {
    LOG(INFO) << message << "; shutting down";
  }

  state = TERMINATING;

  if (frameworks.empty()) { // Terminate slave if there are no frameworks.
    terminate(self());
  } else {
    // NOTE: The slave will terminate after all the executors have
    // terminated.
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


void Slave::detected(const Future<Option<MasterInfo>>& _master)
{
  CHECK(state == DISCONNECTED ||
        state == RUNNING ||
        state == TERMINATING) << state;

  if (state != TERMINATING) {
    state = DISCONNECTED;
  }

  // Pause the status updates.
  statusUpdateManager->pause();

  if (_master.isFailed()) {
    EXIT(1) << "Failed to detect a master: " << _master.failure();
  }

  Option<MasterInfo> latest;

  if (_master.isDiscarded()) {
    LOG(INFO) << "Re-detecting master";
    latest = None();
    master = None();
  } else if (_master.get().isNone()) {
    LOG(INFO) << "Lost leading master";
    latest = None();
    master = None();
  } else {
    latest = _master.get();
    master = UPID(_master.get().get().pid());

    LOG(INFO) << "New master detected at " << master.get();
    link(master.get());

    if (state == TERMINATING) {
      LOG(INFO) << "Skipping registration because slave is terminating";
      return;
    }

    // Wait for a random amount of time before authentication or
    // registration.
    Duration duration =
      flags.registration_backoff_factor * ((double) ::random() / RAND_MAX);

    if (credential.isSome()) {
      // Authenticate with the master.
      // TODO(vinod): Do a backoff for authentication similar to what
      // we do for registration. This is a little tricky because, if
      // we delay 'Slave::authenticate' and a new master is detected
      // before 'authenticate' event is processed the slave tries to
      // authenticate with the new master twice.
      // TODO(vinod): Consider adding an "AUTHENTICATED" state to the
      // slave instead of "authenticate" variable.
      authenticate();
    } else {
      // Proceed with registration without authentication.
      LOG(INFO) << "No credentials provided."
                << " Attempting to register without authentication";

      delay(duration,
            self(),
            &Slave::doReliableRegistration,
            flags.registration_backoff_factor * 2); // Backoff.
    }
  }

  // Keep detecting masters.
  LOG(INFO) << "Detecting new master";
  detection = detector->detect(latest)
    .onAny(defer(self(), &Slave::detected, lambda::_1));
}


void Slave::authenticate()
{
  authenticated = false;

  if (master.isNone()) {
    return;
  }

  if (authenticating.isSome()) {
    // Authentication is in progress. Try to cancel it.
    // Note that it is possible that 'authenticating' is ready
    // and the dispatch to '_authenticate' is enqueued when we
    // are here, making the 'discard' here a no-op. This is ok
    // because we set 'reauthenticate' here which enforces a retry
    // in '_authenticate'.
    Future<bool> authenticating_ = authenticating.get();
    authenticating_.discard();
    reauthenticate = true;
    return;
  }

  LOG(INFO) << "Authenticating with master " << master.get();

  CHECK(authenticatee == NULL);

  if (authenticateeName == DEFAULT_AUTHENTICATEE) {
    LOG(INFO) << "Using default CRAM-MD5 authenticatee";
    authenticatee = new cram_md5::CRAMMD5Authenticatee();
  } else {
    Try<Authenticatee*> module =
      modules::ModuleManager::create<Authenticatee>(authenticateeName);
    if (module.isError()) {
      EXIT(1) << "Could not create authenticatee module '"
              << authenticateeName << "': " << module.error();
    }
    LOG(INFO) << "Using '" << authenticateeName << "' authenticatee";
    authenticatee = module.get();
  }

  CHECK_SOME(credential);

  authenticating =
    authenticatee->authenticate(master.get(), self(), credential.get())
      .onAny(defer(self(), &Self::_authenticate));

  delay(Seconds(5), self(), &Self::authenticationTimeout, authenticating.get());
}


void Slave::_authenticate()
{
  delete CHECK_NOTNULL(authenticatee);
  authenticatee = NULL;

  CHECK_SOME(authenticating);
  const Future<bool>& future = authenticating.get();

  if (master.isNone()) {
    LOG(INFO) << "Ignoring _authenticate because the master is lost";
    authenticating = None();
    // Set it to false because we do not want further retries until
    // a new master is detected.
    // We obviously do not need to reauthenticate either even if
    // 'reauthenticate' is currently true because the master is
    // lost.
    reauthenticate = false;
    return;
  }

  if (reauthenticate || !future.isReady()) {
    LOG(WARNING)
      << "Failed to authenticate with master " << master.get() << ": "
      << (reauthenticate ? "master changed" :
         (future.isFailed() ? future.failure() : "future discarded"));

    authenticating = None();
    reauthenticate = false;

    // TODO(vinod): Add a limit on number of retries.
    dispatch(self(), &Self::authenticate); // Retry.
    return;
  }

  if (!future.get()) {
    // For refused authentication, we exit instead of doing a shutdown
    // to keep possibly active executors running.
    EXIT(1) << "Master " << master.get() << " refused authentication";
  }

  LOG(INFO) << "Successfully authenticated with master " << master.get();

  authenticated = true;
  authenticating = None();

  // Proceed with registration.
  doReliableRegistration(flags.registration_backoff_factor * 2);
}


void Slave::authenticationTimeout(Future<bool> future)
{
  // NOTE: Discarded future results in a retry in '_authenticate()'.
  // Also note that a 'discard' here is safe even if another
  // authenticator is in progress because this copy of the future
  // corresponds to the original authenticator that started the timer.
  if (future.discard()) { // This is a no-op if the future is already ready.
    LOG(WARNING) << "Authentication timed out";
  }
}


void Slave::registered(
    const UPID& from,
    const SlaveID& slaveId,
    const MasterSlaveConnection& connection)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring registration message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  CHECK_SOME(master);

  if (connection.has_total_ping_timeout_seconds()) {
    masterPingTimeout = Seconds(connection.total_ping_timeout_seconds());
  } else {
    masterPingTimeout = DEFAULT_MASTER_PING_TIMEOUT();
  }

  switch(state) {
    case DISCONNECTED: {
      LOG(INFO) << "Registered with master " << master.get()
                << "; given slave ID " << slaveId;

      // TODO(bernd-mesos): Make this an instance method call, see comment
      // in "fetcher.hpp"".
      Try<Nothing> recovered = Fetcher::recover(slaveId, flags);
      if (recovered.isError()) {
        LOG(FATAL) << "Could not initialize fetcher cache: "
                   << recovered.error();
      }

      state = RUNNING;

      statusUpdateManager->resume(); // Resume status updates.

      info.mutable_id()->CopyFrom(slaveId); // Store the slave id.

      // Create the slave meta directory.
      paths::createSlaveDirectory(metaDir, slaveId);

      // Checkpoint slave info.
      const string path = paths::getSlaveInfoPath(metaDir, slaveId);

      VLOG(1) << "Checkpointing SlaveInfo to '" << path << "'";
      CHECK_SOME(state::checkpoint(path, info));

      // If we don't get a ping from the master, trigger a
      // re-registration. This needs to be done once registered,
      // in case we never receive an initial ping.
      Clock::cancel(pingTimer);

      pingTimer = delay(
          masterPingTimeout,
          self(),
          &Slave::pingTimeout,
          detection);

      break;
    }
    case RUNNING:
      // Already registered!
      if (!(info.id() == slaveId)) {
       EXIT(1) << "Registered but got wrong id: " << slaveId
               << "(expected: " << info.id() << "). Committing suicide";
      }
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

  // Send the latest estimate for oversubscribed resources.
  if (oversubscribedResources.isSome()) {
    LOG(INFO) << "Forwarding total oversubscribed resources "
              << oversubscribedResources.get();

    UpdateSlaveMessage message;
    message.mutable_slave_id()->CopyFrom(info.id());
    message.mutable_oversubscribed_resources()->CopyFrom(
        oversubscribedResources.get());

    send(master.get(), message);
  }
}


void Slave::reregistered(
    const UPID& from,
    const SlaveID& slaveId,
    const vector<ReconcileTasksMessage>& reconciliations,
    const MasterSlaveConnection& connection)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring re-registration message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  CHECK_SOME(master);

  if (!(info.id() == slaveId)) {
    EXIT(1) << "Re-registered but got wrong id: " << slaveId
            << "(expected: " << info.id() << "). Committing suicide";
  }

  if (connection.has_total_ping_timeout_seconds()) {
    masterPingTimeout = Seconds(connection.total_ping_timeout_seconds());
  } else {
    masterPingTimeout = DEFAULT_MASTER_PING_TIMEOUT();
  }

  switch(state) {
    case DISCONNECTED:
      LOG(INFO) << "Re-registered with master " << master.get();
      state = RUNNING;
      statusUpdateManager->resume(); // Resume status updates.

      // If we don't get a ping from the master, trigger a
      // re-registration. This needs to be done once re-registered,
      // in case we never receive an initial ping.
      Clock::cancel(pingTimer);

      pingTimer = delay(
          masterPingTimeout,
          self(),
          &Slave::pingTimeout,
          detection);

      break;
    case RUNNING:
      LOG(WARNING) << "Already re-registered with master " << master.get();
      break;
    case TERMINATING:
      LOG(WARNING) << "Ignoring re-registration because slave is terminating";
      return;
    case RECOVERING:
      // It's possible to receive a message intended for the previous
      // run of the slave here. Short term we can leave this as is and
      // crash in this case. Ideally responses can be tied to a
      // particular run of the slave, see:
      // https://issues.apache.org/jira/browse/MESOS-676
      // https://issues.apache.org/jira/browse/MESOS-677
    default:
      LOG(FATAL) << "Unexpected slave state " << state;
      return;
  }

  // Send the latest estimate for oversubscribed resources.
  if (oversubscribedResources.isSome()) {
    LOG(INFO) << "Forwarding total oversubscribed resources "
              << oversubscribedResources.get();

    UpdateSlaveMessage message;
    message.mutable_slave_id()->CopyFrom(info.id());
    message.mutable_oversubscribed_resources()->CopyFrom(
        oversubscribedResources.get());

    send(master.get(), message);
  }

  // Reconcile any tasks per the master's request.
  foreach (const ReconcileTasksMessage& reconcile, reconciliations) {
    Framework* framework = getFramework(reconcile.framework_id());

    foreach (const TaskStatus& status, reconcile.statuses()) {
      const TaskID& taskId = status.task_id();

      bool known = false;

      // Try to locate the task.
      if (framework != NULL) {
        foreachkey (const ExecutorID& executorId, framework->pending) {
          if (framework->pending[executorId].contains(taskId)) {
            known = true;
          }
        }
        foreachvalue (Executor* executor, framework->executors) {
          if (executor->queuedTasks.contains(taskId) ||
              executor->launchedTasks.contains(taskId) ||
              executor->terminatedTasks.contains(taskId)) {
            known = true;
          }
        }
      }

      // We only need to send a TASK_LOST update when the task is
      // unknown (so that the master removes it). Otherwise, the
      // master correctly holds the task and will receive updates.
      if (!known) {
        LOG(WARNING) << "Slave reconciling task " << taskId
                     << " of framework " << reconcile.framework_id()
                     << " in state TASK_LOST: task unknown to the slave";

        const StatusUpdate update = protobuf::createStatusUpdate(
            reconcile.framework_id(),
            info.id(),
            taskId,
            TASK_LOST,
            TaskStatus::SOURCE_SLAVE,
            UUID::random(),
            "Reconciliation: task unknown to the slave",
            TaskStatus::REASON_RECONCILIATION);

        // NOTE: We can't use statusUpdate() here because it drops
        // updates for unknown frameworks.
        statusUpdateManager->update(update, info.id())
          .onAny(defer(self(),
                       &Slave::__statusUpdate,
                       lambda::_1,
                       update,
                       UPID()));
      }
    }
  }
}


void Slave::doReliableRegistration(Duration maxBackoff)
{
  if (master.isNone()) {
    LOG(INFO) << "Skipping registration because no master present";
    return;
  }

  if (credential.isSome() && !authenticated) {
    LOG(INFO) << "Skipping registration because not authenticated";
    return;
  }

  if (state == RUNNING) { // Slave (re-)registered with the master.
    return;
  }

  if (state == TERMINATING) {
    LOG(INFO) << "Skipping registration because slave is terminating";
    return;
  }

  CHECK(state == DISCONNECTED) << state;

  CHECK_NE("cleanup", flags.recover);

  if (!info.has_id()) {
    // Registering for the first time.
    RegisterSlaveMessage message;
    message.set_version(MESOS_VERSION);
    message.mutable_slave()->CopyFrom(info);

    // Include checkpointed resources.
    message.mutable_checkpointed_resources()->CopyFrom(checkpointedResources);

    send(master.get(), message);
  } else {
    // Re-registering, so send tasks running.
    ReregisterSlaveMessage message;
    message.set_version(MESOS_VERSION);

    // Include checkpointed resources.
    message.mutable_checkpointed_resources()->CopyFrom(checkpointedResources);

    message.mutable_slave()->CopyFrom(info);

    foreachvalue (Framework* framework, frameworks) {
      // TODO(bmahler): We need to send the executors for these
      // pending tasks, and we need to send exited events if they
      // cannot be launched: MESOS-1715 MESOS-1720.

      typedef hashmap<TaskID, TaskInfo> TaskMap;
      foreachvalue (const TaskMap& tasks, framework->pending) {
        foreachvalue (const TaskInfo& task, tasks) {
          message.add_tasks()->CopyFrom(protobuf::createTask(
              task, TASK_STAGING, framework->id()));
        }
      }

      foreachvalue (Executor* executor, framework->executors) {
        // Add launched, terminated, and queued tasks.
        // Note that terminated executors will only have terminated
        // unacknowledged tasks.
        // Note that for each task the latest state and status update
        // state (if any) is also included.
        foreach (Task* task, executor->launchedTasks.values()) {
          message.add_tasks()->CopyFrom(*task);
        }

        foreach (Task* task, executor->terminatedTasks.values()) {
          message.add_tasks()->CopyFrom(*task);
        }

        foreach (const TaskInfo& task, executor->queuedTasks.values()) {
          message.add_tasks()->CopyFrom(protobuf::createTask(
              task, TASK_STAGING, framework->id()));
        }

        // Do not re-register with Command Executors because the
        // master doesn't store them; they are generated by the slave.
        if (executor->isCommandExecutor()) {
          // NOTE: We have to unset the executor id here for the task
          // because the master uses the absence of task.executor_id()
          // to detect command executors.
          for (int i = 0; i < message.tasks_size(); ++i) {
            message.mutable_tasks(i)->clear_executor_id();
          }
        } else {
          // Ignore terminated executors because they do not consume
          // any resources.
          if (executor->state != Executor::TERMINATED) {
            ExecutorInfo* executorInfo = message.add_executor_infos();
            executorInfo->MergeFrom(executor->info);

            // Scheduler Driver will ensure the framework id is set in
            // ExecutorInfo, effectively making it a required field.
            CHECK(executorInfo->has_framework_id());
          }
        }
      }
    }

    // Add completed frameworks.
    foreach (const Owned<Framework>& completedFramework, completedFrameworks) {
      VLOG(1) << "Reregistering completed framework "
                << completedFramework->id();

      Archive::Framework* completedFramework_ =
        message.add_completed_frameworks();

      completedFramework_->mutable_framework_info()->CopyFrom(
          completedFramework->info);

      if (completedFramework->pid.isSome()) {
        completedFramework_->set_pid(completedFramework->pid.get());
      }

      foreach (const Owned<Executor>& executor,
               completedFramework->completedExecutors) {
        VLOG(2) << "Reregistering completed executor " << executor->id
                << " with " << executor->terminatedTasks.size()
                << " terminated tasks, " << executor->completedTasks.size()
                << " completed tasks";

        foreach (const Task* task, executor->terminatedTasks.values()) {
          VLOG(2) << "Reregistering terminated task " << task->task_id();
          completedFramework_->add_tasks()->CopyFrom(*task);
        }

        foreach (const std::shared_ptr<Task>& task, executor->completedTasks) {
          VLOG(2) << "Reregistering completed task " << task->task_id();
          completedFramework_->add_tasks()->CopyFrom(*task);
        }
      }
    }

    CHECK_SOME(master);
    send(master.get(), message);
  }

  // Bound the maximum backoff by 'REGISTER_RETRY_INTERVAL_MAX'.
  maxBackoff = std::min(maxBackoff, REGISTER_RETRY_INTERVAL_MAX);

  // Determine the delay for next attempt by picking a random
  // duration between 0 and 'maxBackoff'.
  Duration delay = maxBackoff * ((double) ::random() / RAND_MAX);

  VLOG(1) << "Will retry registration in " << delay << " if necessary";

  // Backoff.
  process::delay(delay, self(), &Slave::doReliableRegistration, maxBackoff * 2);
}


// Helper to unschedule the path.
// TODO(vinod): Can we avoid this helper?
Future<bool> Slave::unschedule(const string& path)
{
  return gc->unschedule(path);
}


// TODO(vinod): Instead of crashing the slave on checkpoint errors,
// send TASK_LOST to the framework.
void Slave::runTask(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const FrameworkID& frameworkId_,
    const UPID& pid,
    TaskInfo task)
{
  if (master != from) {
    LOG(WARNING) << "Ignoring run task message from " << from
                 << " because it is not the expected master: "
                 << (master.isSome() ? stringify(master.get()) : "None");
    return;
  }

  if (!frameworkInfo.has_id()) {
    LOG(ERROR) << "Ignoring run task message from " << from
               << " because it does not have a framework ID";
    return;
  }

  // Create frameworkId alias to use in the rest of the function.
  const FrameworkID frameworkId = frameworkInfo.id();

  LOG(INFO) << "Got assigned task " << task.task_id()
            << " for framework " << frameworkId;

  if (!(task.slave_id() == info.id())) {
    LOG(WARNING)
      << "Slave " << info.id() << " ignoring task " << task.task_id()
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
    // Currently it is tricky because 'statusUpdate()'
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

    Option<UPID> frameworkPid = None();

    if (pid != UPID()) {
      frameworkPid = pid;
    }

    framework = new Framework(this, frameworkInfo, frameworkPid);
    frameworks[frameworkId] = framework;
    if (frameworkInfo.checkpoint()) {
      framework->checkpointFramework();
    }

    // Is this same framework in completedFrameworks? If so, move the completed
    // executors to this framework and remove it from that list.
    // TODO(brenden): Consider using stout/cache.hpp instead of boost
    // circular_buffer.
    for (auto it = completedFrameworks.begin(), end = completedFrameworks.end();
         it != end;
         ++it) {
      if ((*it)->id() == frameworkId) {
        framework->completedExecutors = (*it)->completedExecutors;
        completedFrameworks.erase(it);
        break;
      }
    }
  }

  const ExecutorInfo executorInfo = getExecutorInfo(frameworkId, task);
  const ExecutorID& executorId = executorInfo.executor_id();

  if (HookManager::hooksAvailable()) {
    // Set task labels from run task label decorator.
    task.mutable_labels()->CopyFrom(HookManager::slaveRunTaskLabelDecorator(
        task, executorInfo, frameworkInfo, info));
  }

  // We add the task to 'pending' to ensure the framework is not
  // removed and the framework and top level executor directories
  // are not scheduled for deletion before '_runTask()' is called.
  CHECK_NOTNULL(framework);
  framework->pending[executorId][task.task_id()] = task;

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
    path = paths::getExecutorPath(metaDir, info.id(), frameworkId, executorId);

    if (os::exists(path)) {
      unschedule = unschedule.then(defer(self(), &Self::unschedule, path));
    }
  }

  // Run the task after the unschedules are done.
  unschedule.onAny(
      defer(self(), &Self::_runTask, lambda::_1, frameworkInfo, task));
}


void Slave::_runTask(
    const Future<bool>& future,
    const FrameworkInfo& frameworkInfo,
    const TaskInfo& task)
{
  const FrameworkID frameworkId = frameworkInfo.id();

  LOG(INFO) << "Launching task " << task.task_id()
            << " for framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Ignoring run task " << task.task_id()
                 << " because the framework " << frameworkId
                 << " does not exist";
    return;
  }

  const ExecutorInfo executorInfo = getExecutorInfo(frameworkId, task);
  const ExecutorID& executorId = executorInfo.executor_id();

  if (framework->pending.contains(executorId) &&
      framework->pending[executorId].contains(task.task_id())) {
    framework->pending[executorId].erase(task.task_id());
    if (framework->pending[executorId].empty()) {
      framework->pending.erase(executorId);
      // NOTE: Ideally we would perform the following check here:
      //
      //   if (framework->executors.empty() &&
      //       framework->pending.empty()) {
      //     removeFramework(framework);
      //   }
      //
      // However, we need 'framework' to stay valid for the rest of
      // this function. As such, we perform the check before each of
      // the 'return' statements below.
    }
  } else {
    LOG(WARNING) << "Ignoring run task " << task.task_id()
                 << " of framework " << frameworkId
                 << " because the task has been killed in the meantime";
    return;
  }

  // We don't send a status update here because a terminating
  // framework cannot send acknowledgements.
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring run task " << task.task_id()
                 << " of framework " << frameworkId
                 << " because the framework is terminating";

    // Refer to the comment after 'framework->pending.erase' above
    // for why we need this.
    if (framework->executors.empty() && framework->pending.empty()) {
      removeFramework(framework);
    }

    return;
  }

  if (!future.isReady()) {
    LOG(ERROR) << "Failed to unschedule directories scheduled for gc: "
               << (future.isFailed() ? future.failure() : "future discarded");

    const StatusUpdate update = protobuf::createStatusUpdate(
        frameworkId,
        info.id(),
        task.task_id(),
        TASK_LOST,
        TaskStatus::SOURCE_SLAVE,
        UUID::random(),
        "Could not launch the task because we failed to unschedule directories"
        " scheduled for gc",
        TaskStatus::REASON_GC_ERROR);

    // TODO(vinod): Ensure that the status update manager reliably
    // delivers this update. Currently, we don't guarantee this
    // because removal of the framework causes the status update
    // manager to stop retrying for its un-acked updates.
    statusUpdate(update, UPID());

    // Refer to the comment after 'framework->pending.erase' above
    // for why we need this.
    if (framework->executors.empty() && framework->pending.empty()) {
      removeFramework(framework);
    }

    return;
  }

  // NOTE: If the task or executor uses resources that are
  // checkpointed on the slave (e.g. persistent volumes), we should
  // already know about it. If the slave doesn't know about them (e.g.
  // CheckpointResourcesMessage was dropped or came out of order),
  // we send TASK_LOST status updates here since restarting the task
  // may succeed in the event that CheckpointResourcesMessage arrives
  // out of order.
  Resources checkpointedTaskResources =
    Resources(task.resources()).filter(needCheckpointing);

  foreach (const Resource& resource, checkpointedTaskResources) {
    if (!checkpointedResources.contains(resource)) {
      LOG(WARNING) << "Unknown checkpointed resource " << resource
                   << " for task " << task.task_id()
                   << " of framework " << frameworkId;

      const StatusUpdate update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          task.task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_SLAVE,
          UUID::random(),
         "The checkpointed resources being used by the task are unknown to "
          "the slave",
          TaskStatus::REASON_RESOURCES_UNKNOWN);

      statusUpdate(update, UPID());

      // Refer to the comment after 'framework->pending.erase' above
      // for why we need this.
      if (framework->executors.empty() && framework->pending.empty()) {
        removeFramework(framework);
      }

      return;
    }
  }

  if (task.has_executor()) {
    Resources checkpointedExecutorResources =
      Resources(task.executor().resources()).filter(needCheckpointing);

    foreach (const Resource& resource, checkpointedExecutorResources) {
      if (!checkpointedResources.contains(resource)) {
        LOG(WARNING) << "Unknown checkpointed resource " << resource
                     << " for executor " << task.executor().executor_id()
                     << " of framework " << frameworkId;

        const StatusUpdate update = protobuf::createStatusUpdate(
            frameworkId,
            info.id(),
            task.task_id(),
            TASK_LOST,
            TaskStatus::SOURCE_SLAVE,
            UUID::random(),
            "The checkpointed resources being used by the executor are unknown "
            "to the slave",
            TaskStatus::REASON_RESOURCES_UNKNOWN,
            task.executor().executor_id());

        statusUpdate(update, UPID());

        // Refer to the comment after 'framework->pending.erase' above
        // for why we need this.
        if (framework->executors.empty() && framework->pending.empty()) {
          removeFramework(framework);
        }

        return;
      }
    }
  }

  // NOTE: The slave cannot be in 'RECOVERING' because the task would
  // have been rejected in 'runTask()' in that case.
  CHECK(state == DISCONNECTED || state == RUNNING || state == TERMINATING)
    << state;

  if (state == TERMINATING) {
    LOG(WARNING) << "Ignoring run task " << task.task_id()
                 << " of framework " << frameworkId
                 << " because the slave is terminating";

    // Refer to the comment after 'framework->pending.erase' above
    // for why we need this.
    if (framework->executors.empty() && framework->pending.empty()) {
      removeFramework(framework);
    }

    // We don't send a TASK_LOST here because the slave is
    // terminating.
    return;
  }

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

      const StatusUpdate update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          task.task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_SLAVE,
          UUID::random(),
          "Executor terminating/terminated",
          TaskStatus::REASON_EXECUTOR_TERMINATED);

      statusUpdate(update, UPID());
      break;
    }
    case Executor::REGISTERING:
      // Checkpoint the task before we do anything else.
      if (executor->checkpoint) {
        executor->checkpointTask(task);
      }

      // Queue task if the executor has not yet registered.
      LOG(INFO) << "Queuing task '" << task.task_id()
                  << "' for executor " << executorId
                  << " of framework '" << frameworkId;

      executor->queuedTasks[task.task_id()] = task;
      break;
    case Executor::RUNNING: {
      // Checkpoint the task before we do anything else.
      if (executor->checkpoint) {
        executor->checkpointTask(task);
      }

      // Queue task until the containerizer is updated with new
      // resource limits (MESOS-998).
      LOG(INFO) << "Queuing task '" << task.task_id()
                  << "' for executor " << executorId
                  << " of framework '" << frameworkId;

      executor->queuedTasks[task.task_id()] = task;

      // Update the resource limits for the container. Note that the
      // resource limits include the currently queued tasks because we
      // want the container to have enough resources to hold the
      // upcoming tasks.
      Resources resources = executor->resources;

      // TODO(jieyu): Use foreachvalue instead once LinkedHashmap
      // supports it.
      foreach (const TaskInfo& task, executor->queuedTasks.values()) {
        resources += task.resources();
      }

      containerizer->update(executor->containerId, resources)
        .onAny(defer(self(),
                     &Self::runTasks,
                     lambda::_1,
                     frameworkId,
                     executorId,
                     executor->containerId,
                     list<TaskInfo>({task})));
      break;
    }
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id()
                 << " is in unexpected state " << executor->state;
      break;
  }

  // We don't perform the checks for 'removeFramework' here since
  // we're guaranteed by 'launchExecutor' that 'framework->executors'
  // will be non-empty.
  CHECK(!framework->executors.empty());
}


void Slave::runTasks(
    const Future<Nothing>& future,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const list<TaskInfo>& tasks)
{
  // Store all task IDs for logging.
  vector<TaskID> taskIds;
  foreach (const TaskInfo& task, tasks) {
    taskIds.push_back(task.task_id());
  }

  if (!future.isReady()) {
    LOG(ERROR) << "Failed to update resources for container " << containerId
               << " of executor '" << executorId
               << "' of framework " << frameworkId
               << ", destroying container: "
               << (future.isFailed() ? future.failure() : "discarded");

    containerizer->destroy(containerId);
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Ignoring sending queued tasks " << taskIds
                 << " to executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the framework does not exist";
    return;
  }

  // No need to send the task to the executor because the framework is
  // being shutdown. No need to send status update for the task as
  // well because the framework is terminating!
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring sending queued tasks " << taskIds
                 << " to executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the framework is terminating";
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Ignoring sending queued tasks " << taskIds
                 << " to executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the executor does not exist";
    return;
  }

  // This is the case where the original instance of the executor has
  // been shutdown and a new instance is brought up. No need to send
  // status update as well because it should have already been sent
  // when the original instance of the executor was shutting down.
  if (executor->containerId != containerId) {
    LOG(WARNING) << "Ignoring sending queued tasks '" << taskIds
                 << " to executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the target container " << containerId
                 << " has exited";
    return;
  }

  CHECK(executor->state == Executor::RUNNING ||
        executor->state == Executor::TERMINATING ||
        executor->state == Executor::TERMINATED)
    << executor->state;

  // No need to send the task to the executor because the executor is
  // terminating or has been terminated. No need to send status update
  // for the task as well because it will be properly handled by
  // 'executorTerminated'.
  if (executor->state != Executor::RUNNING) {
    LOG(WARNING) << "Ignoring sending queued tasks " << taskIds
                 << " to executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the executor is in "
                 << executor->state << " state";
    return;
  }

  foreach (const TaskInfo& task, tasks) {
    // This is the case where the task is killed. No need to send
    // status update because it should be handled in 'killTask'.
    if (!executor->queuedTasks.contains(task.task_id())) {
      LOG(WARNING) << "Ignoring sending queued task '" << task.task_id()
                   << "' to executor '" << executorId
                   << "' of framework " << frameworkId
                   << " because the task has been killed";
      continue;
    }

    executor->queuedTasks.erase(task.task_id());

    // Add the task and send it to the executor.
    executor->addTask(task);

    LOG(INFO) << "Sending queued task '" << task.task_id()
              << "' to executor '" << executor->id
              << "' of framework " << framework->id();

    RunTaskMessage message;
    message.mutable_framework()->MergeFrom(framework->info);
    message.mutable_task()->MergeFrom(task);

    // Note that 0.23.x executors require the 'pid' to be set
    // to decode the message, but do not use the field.
    message.set_pid(framework->pid.getOrElse(UPID()));

    send(executor->pid, message);
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

  foreachkey (const ExecutorID& executorId, framework->pending) {
    if (framework->pending[executorId].contains(taskId)) {
      LOG(WARNING) << "Killing task " << taskId
                   << " of framework " << frameworkId
                   << " before it was launched";

      const StatusUpdate update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          taskId,
          TASK_KILLED,
          TaskStatus::SOURCE_SLAVE,
          UUID::random(),
          "Task killed before it was launched");
      statusUpdate(update, UPID());

      framework->pending[executorId].erase(taskId);
      if (framework->pending[executorId].empty()) {
        framework->pending.erase(executorId);
        if (framework->pending.empty() && framework->executors.empty()) {
          removeFramework(framework);
        }
      }
      return;
    }
  }

  Executor* executor = framework->getExecutor(taskId);
  if (executor == NULL) {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no corresponding executor is running";
    // We send a TASK_LOST update because this task has never
    // been launched on this slave.
    const StatusUpdate update = protobuf::createStatusUpdate(
        frameworkId,
        info.id(),
        taskId,
        TASK_LOST,
        TaskStatus::SOURCE_SLAVE,
        UUID::random(),
        "Cannot find executor",
        TaskStatus::REASON_EXECUTOR_TERMINATED);

    statusUpdate(update, UPID());
    return;
  }

  switch (executor->state) {
    case Executor::REGISTERING: {
      // The executor hasn't registered yet.
      const StatusUpdate update = protobuf::createStatusUpdate(
          frameworkId,
          info.id(),
          taskId,
          TASK_KILLED,
          TaskStatus::SOURCE_SLAVE,
          UUID::random(),
          "Unregistered executor",
          TaskStatus::REASON_EXECUTOR_UNREGISTERED,
          executor->id);

      // NOTE: Sending a terminal update (TASK_KILLED) removes the
      // task from 'executor->queuedTasks', so that if the executor
      // registers at a later point in time, it won't get this task.
      statusUpdate(update, UPID());

      // TODO(jieyu): Here, we kill the executor if it no longer has
      // any task to run and has not yet registered. This is a
      // workaround for those single task executors that do not have a
      // proper self terminating logic when they haven't received the
      // task within a timeout.
      if (executor->queuedTasks.empty()) {
        CHECK(executor->launchedTasks.empty())
            << " Unregistered executor " << executor->id
            << " has launched tasks";

        LOG(WARNING) << "Killing the unregistered executor '" << executor->id
                     << "' of framework " << framework->id()
                     << " because it has no tasks";

        executor->state = Executor::TERMINATING;

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
      if (executor->queuedTasks.contains(taskId)) {
        // This is the case where the task has not yet been sent to
        // the executor (e.g., waiting for containerizer update to
        // finish).
        const StatusUpdate update = protobuf::createStatusUpdate(
            frameworkId,
            info.id(),
            taskId,
            TASK_KILLED,
            TaskStatus::SOURCE_SLAVE,
            UUID::random(),
            "Task killed when it was queued",
            None(),
            executor->id);

        // NOTE: Sending a terminal update (TASK_KILLED) removes the
        // task from 'executor->queuedTasks'.
        statusUpdate(update, UPID());
      } else {
        // Send a message to the executor and wait for
        // it to send us a status update.
        KillTaskMessage message;
        message.mutable_framework_id()->MergeFrom(frameworkId);
        message.mutable_task_id()->MergeFrom(taskId);
        send(executor->pid, message);
      }
      break;
    }
    default:
      LOG(FATAL) << " Executor '" << executor->id
                 << "' of framework " << framework->id()
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
      LOG(WARNING) << "Ignoring shutdown framework " << framework->id()
                   << " because it is terminating";
      break;
    case Framework::RUNNING:
      LOG(INFO) << "Shutting down framework " << framework->id();

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
          _shutdownExecutor(framework, executor);
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
    metrics.invalid_framework_messages++;
    return;
  }


  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Dropping message from framework "<< frameworkId
                 << " because framework does not exist";
    metrics.invalid_framework_messages++;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Dropping message from framework "<< frameworkId
                 << " because framework is terminating";
    metrics.invalid_framework_messages++;
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Dropping message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because executor does not exist";
    metrics.invalid_framework_messages++;
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
      metrics.invalid_framework_messages++;
      break;
    case Executor::RUNNING: {
      FrameworkToExecutorMessage message;
      message.mutable_slave_id()->MergeFrom(slaveId);
      message.mutable_framework_id()->MergeFrom(frameworkId);
      message.mutable_executor_id()->MergeFrom(executorId);
      message.set_data(data);
      send(executor->pid, message);
      metrics.valid_framework_messages++;
      break;
    }
    default:
      LOG(FATAL) << " Executor '" << executor->id
                 << "' of framework " << framework->id()
                 << " is in unexpected state " << executor->state;
      break;
  }
}


void Slave::updateFramework(
    const FrameworkID& frameworkId,
    const UPID& pid)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state != RUNNING) {
    LOG(WARNING) << "Dropping updateFramework message for "<< frameworkId
                 << " because the slave is in " << state << " state";
    metrics.invalid_framework_messages++;
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

      if (pid == UPID()) {
        framework->pid = None();
      } else {
        framework->pid = pid;
      }

      if (framework->info.checkpoint()) {
        // Checkpoint the framework pid, note that when the 'pid'
        // is None, we checkpoint a default UPID() because
        // 0.23.x slaves consider a missing pid file to be an
        // error.
        const string path = paths::getFrameworkPidPath(
            metaDir, info.id(), frameworkId);

        VLOG(1) << "Checkpointing framework pid"
                << " '" << framework->pid.getOrElse(UPID()) << "'"
                << " to '" << path << "'";

        CHECK_SOME(state::checkpoint(path, framework->pid.getOrElse(UPID())));
      }

      // Inform status update manager to immediately resend any pending
      // updates.
      statusUpdateManager->resume();

      break;
    }
    default:
      LOG(FATAL) << "Framework " << framework->id()
                << " is in unexpected state " << framework->state;
      break;
  }
}


void Slave::checkpointResources(const vector<Resource>& _checkpointedResources)
{
  // TODO(jieyu): Here we assume that CheckpointResourcesMessages are
  // ordered (i.e., slave receives them in the same order master sends
  // them). This should be true in most of the cases because TCP
  // enforces in order delivery per connection. However, the ordering
  // is technically not guaranteed because master creates multiple
  // connections to the slave in some cases (e.g., persistent socket
  // to slave breaks and master uses ephemeral socket). This could
  // potentially be solved by using a version number and rejecting
  // stale messages according to the version number.
  //
  // If CheckpointResourcesMessages are delivered out-of-order, there
  // are two cases to consider:
  //  (1) If master does not fail over, it will reconcile the state
  //      with the slave if the framework later changes the
  //      checkpointed resources. Since master is the source of truth
  //      for reservations, the inconsistency is not exposed to
  //      frameworks.
  //  (2) If master does fail over, the slave will inform the new
  //      master about the incorrect checkpointed resources. When that
  //      happens, we expect framework to reconcile based on the
  //      offers they get.
  Resources newCheckpointedResources = _checkpointedResources;

  CHECK_SOME(state::checkpoint(
      paths::getResourcesInfoPath(metaDir),
      newCheckpointedResources))
    << "Failed to checkpoint resources " << newCheckpointedResources;

  // Creates persistent volumes that do not exist and schedules
  // releasing those persistent volumes that are no longer needed.
  //
  // TODO(jieyu): Consider introducing a volume manager once we start
  // to support multiple disks, or raw disks. Depending on the
  // DiskInfo, we may want to create either directories under a root
  // directory, or LVM volumes from a given device.
  Resources volumes = newCheckpointedResources.persistentVolumes();

  foreach (const Resource& volume, volumes) {
    // This is validated in master.
    CHECK_NE(volume.role(), "*");

    string path = paths::getPersistentVolumePath(
        flags.work_dir,
        volume.role(),
        volume.disk().persistence().id());

    if (!os::exists(path)) {
      CHECK_SOME(os::mkdir(path, true))
        << "Failed to create persistent volume at '" << path << "'";
    }
  }

  // TODO(jieyu): Schedule gc for released persistent volumes. We need
  // to consider dynamic reservation here because the framework can
  // release dynamic reservation while still wants to keep the
  // persistent volume.

  LOG(INFO) << "Updated checkpointed resources from "
            << checkpointedResources << " to "
            << newCheckpointedResources;

  checkpointedResources = newCheckpointedResources;
}


void Slave::statusUpdateAcknowledgement(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const TaskID& taskId,
    const string& uuid)
{
  // Originally, all status update acknowledgements were sent from the
  // scheduler driver. We'd like to have all acknowledgements sent by
  // the master instead. See: MESOS-1389.
  // For now, we handle acknowledgements from the leading master and
  // from the scheduler driver, for backwards compatibility.
  // TODO(bmahler): Aim to have the scheduler driver no longer
  // sending acknowledgements in 0.20.0. Stop handling those messages
  // here in 0.21.0.
  // NOTE: We must reject those acknowledgements coming from
  // non-leading masters because we may have already sent the terminal
  // un-acknowledged task to the leading master! Unfortunately, the
  // master's pid will not change across runs on the same machine, so
  // we may process a message from the old master on the same machine,
  // but this is a more general problem!
  if (strings::startsWith(from.id, "master")) {
    if (state != RUNNING) {
      LOG(WARNING) << "Dropping status update acknowledgement message for "
                   << frameworkId << " because the slave is in "
                   << state << " state";
      return;
    }

    if (master != from) {
      LOG(WARNING) << "Ignoring status update acknowledgement message from "
                   << from << " because it is not the expected master: "
                   << (master.isSome() ? stringify(master.get()) : "None");
      return;
    }
  }

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
            << "' of framework " << frameworkId << " from "
            << stringify(from);

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

        VLOG(1) << "Checkpointing executor pid '"
                << executor->pid << "' to '" << path << "'";
        CHECK_SOME(state::checkpoint(path, executor->pid));
      }

      // Tell executor it's registered and give it any queued tasks.
      ExecutorRegisteredMessage message;
      message.mutable_executor_info()->MergeFrom(executor->info);
      message.mutable_framework_id()->MergeFrom(framework->id());
      message.mutable_framework_info()->MergeFrom(framework->info);
      message.mutable_slave_id()->MergeFrom(info.id());
      message.mutable_slave_info()->MergeFrom(info);
      send(executor->pid, message);

      // Update the resource limits for the container. Note that the
      // resource limits include the currently queued tasks because we
      // want the container to have enough resources to hold the
      // upcoming tasks.
      Resources resources = executor->resources;

      // TODO(jieyu): Use foreachvalue instead once LinkedHashmap
      // supports it.
      foreach (const TaskInfo& task, executor->queuedTasks.values()) {
        resources += task.resources();
      }

      containerizer->update(executor->containerId, resources)
        .onAny(defer(self(),
                     &Self::runTasks,
                     lambda::_1,
                     frameworkId,
                     executorId,
                     executor->containerId,
                     executor->queuedTasks.values()));
      break;
    }
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id()
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
      containerizer->update(executor->containerId, executor->resources)
        .onAny(defer(self(),
                     &Self::_reregisterExecutor,
                     lambda::_1,
                     frameworkId,
                     executorId,
                     executor->containerId));

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
          LOG(INFO) << "Transitioning STAGED task " << task->task_id()
                    << " to LOST because it is unknown to the executor "
                    << executorId;

          const StatusUpdate update = protobuf::createStatusUpdate(
              frameworkId,
              info.id(),
              task->task_id(),
              TASK_LOST,
              TaskStatus::SOURCE_SLAVE,
              UUID::random(),
              "Task launched during slave restart",
              TaskStatus::REASON_SLAVE_RESTARTED,
              executorId);

          statusUpdate(update, UPID());
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id()
                 << " is in unexpected state " << executor->state;
      break;
  }
}


void Slave::_reregisterExecutor(
    const Future<Nothing>& future,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId)
{
  if (!future.isReady()) {
    LOG(ERROR) << "Failed to update resources for container " << containerId
               << " of executor '" << executorId
               << "' of framework " << frameworkId
               << ", destroying container: "
               << (future.isFailed() ? future.failure() : "discarded");

    containerizer->destroy(containerId);
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
                    << "' of framework " << framework->id();

          executor->state = Executor::TERMINATING;

          containerizer->destroy(executor->containerId);
          break;
        default:
          LOG(FATAL) << "Executor '" << executor->id
                     << "' of framework " << framework->id()
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
// NOTE: We set the pid in 'Slave::__statusUpdate()' to 'pid' so that
// whoever sent this update will get an ACK. This is important because
// we allow executors to send updates for tasks that belong to other
// executors. Currently we allow this because we cannot guarantee
// reliable delivery of status updates. Since executor driver caches
// unacked updates it is important that whoever sent the update gets
// acknowledgement for it.
void Slave::statusUpdate(StatusUpdate update, const UPID& pid)
{
  LOG(INFO) << "Handling status update " << update << " from " << pid;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (!update.has_uuid()) {
    LOG(WARNING) << "Ignoring status update " << update << " without 'uuid'";
    metrics.invalid_status_updates++;
    return;
  }

  // TODO(bmahler): With the HTTP API, we must validate the UUID
  // inside the TaskStatus. For now, we ensure that the uuid of task
  // status matches the update's uuid, in case the executor is using
  // pre 0.23.x driver.
  update.mutable_status()->set_uuid(update.uuid());

  // Set the source and UUID before forwarding the status update.
  update.mutable_status()->set_source(
      pid == UPID() ? TaskStatus::SOURCE_SLAVE : TaskStatus::SOURCE_EXECUTOR);

  // Set TaskStatus.executor_id if not already set; overwrite existing
  // value if already set.
  if (update.has_executor_id()) {
    if (update.status().has_executor_id() &&
        update.status().executor_id() != update.executor_id()) {
      LOG(WARNING) << "Executor ID mismatch in status update from " << pid
                   << "; overwriting received '"
                   << update.status().executor_id() << "' with expected'"
                   << update.executor_id() << "'";
    }
    update.mutable_status()->mutable_executor_id()->CopyFrom(
        update.executor_id());
  }

  Framework* framework = getFramework(update.framework_id());
  if (framework == NULL) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " for unknown framework " << update.framework_id();
    metrics.invalid_status_updates++;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // We don't send update when a framework is terminating because
  // it cannot send acknowledgements.
  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " for terminating framework " << framework->id();
    metrics.invalid_status_updates++;
    return;
  }

  if (HookManager::hooksAvailable()) {
    // Set TaskStatus labels from run task label decorator.
    update.mutable_status()->mutable_labels()->CopyFrom(
        HookManager::slaveTaskStatusLabelDecorator(
            update.framework_id(), update.status()));
  }

  TaskStatus status = update.status();

  Executor* executor = framework->getExecutor(status.task_id());
  if (executor == NULL) {
    LOG(WARNING)  << "Could not find the executor for "
                  << "status update " << update;
    metrics.valid_status_updates++;

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
      .onAny(defer(self(), &Slave::__statusUpdate, lambda::_1, update, pid));

    return;
  }

  CHECK(executor->state == Executor::REGISTERING ||
        executor->state == Executor::RUNNING ||
        executor->state == Executor::TERMINATING ||
        executor->state == Executor::TERMINATED)
    << executor->state;

  // Failing this validation on the executor driver used to cause the
  // driver to abort. Now that the validation is done by the slave, it
  // should shutdown the executor to be consistent.
  // TODO(arojas): Once the HTTP API is the default, return a
  // 400 Bad Request response, indicating the reason in the body.
  if (status.source() == TaskStatus::SOURCE_EXECUTOR &&
      status.state() == TASK_STAGING) {
    LOG(ERROR) << "Received TASK_STAGING from executor " << executor->pid
               << " of framework " << update.framework_id()
               << " which is not allowed. Shutting down the executor";

    _shutdownExecutor(framework, executor);

    return;
  }

  // TODO(vinod): Revisit these semantics when we disallow executors
  // from sending updates for tasks that belong to other executors.
  if (pid != UPID() && executor->pid != pid) {
    LOG(WARNING) << "Received status update " << update << " from " << pid
                 << " on behalf of a different executor " << executor->id
                 << " (" << executor->pid << ")";
  }

  metrics.valid_status_updates++;

  // We set the latest state of the task here so that the slave can
  // inform the master about the latest state (via status update or
  // ReregisterSlaveMessage message) as soon as possible. Master can
  // use this information, for example, to release resources as soon
  // as the latest state of the task reaches a terminal state. This
  // is important because status update manager queues updates and
  // only sends one update per task at a time; the next update for a
  // task is sent only after the acknowledgement for the previous one
  // is received, which could take a long time if the framework is
  // backed up or is down.
  executor->updateTaskState(status);

  // Handle the task appropriately if it is terminated.
  // TODO(vinod): Revisit these semantics when we disallow duplicate
  // terminal updates (e.g., when slave recovery is always enabled).
  if (protobuf::isTerminalState(status.state()) &&
      (executor->queuedTasks.contains(status.task_id()) ||
       executor->launchedTasks.contains(status.task_id()))) {
    executor->terminateTask(status.task_id(), status);

    // Wait until the container's resources have been updated before
    // sending the status update.
    containerizer->update(executor->containerId, executor->resources)
      .onAny(defer(self(),
                   &Slave::_statusUpdate,
                   lambda::_1,
                   update,
                   pid,
                   executor->id,
                   executor->containerId,
                   executor->checkpoint));
  } else {
    // Immediately send the status update.
    _statusUpdate(None(),
                  update,
                  pid,
                  executor->id,
                  executor->containerId,
                  executor->checkpoint);
  }
}


void Slave::_statusUpdate(
    const Option<Future<Nothing>>& future,
    const StatusUpdate& update,
    const UPID& pid,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    bool checkpoint)
{
  if (future.isSome() && !future.get().isReady()) {
    LOG(ERROR) << "Failed to update resources for container " << containerId
               << " of executor " << executorId
               << " running task " << update.status().task_id()
               << " on status update for terminal task, destroying container: "
               << (future.get().isFailed() ? future.get().failure()
                                           : "discarded");

    containerizer->destroy(containerId);
  }

  if (checkpoint) {
    // Ask the status update manager to checkpoint and reliably send the update.
    statusUpdateManager->update(update, info.id(), executorId, containerId)
      .onAny(defer(self(), &Slave::__statusUpdate, lambda::_1, update, pid));
  } else {
    // Ask the status update manager to just retry the update.
    statusUpdateManager->update(update, info.id())
      .onAny(defer(self(), &Slave::__statusUpdate, lambda::_1, update, pid));
  }
}


void Slave::__statusUpdate(
    const Future<Nothing>& future,
    const StatusUpdate& update,
    const UPID& pid)
{
  CHECK_READY(future) << "Failed to handle status update " << update;

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


// NOTE: An acknowledgement for this update might have already been
// processed by the slave but not the status update manager.
void Slave::forward(StatusUpdate update)
{
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state != RUNNING) {
    LOG(WARNING) << "Dropping status update " << update
                 << " sent by status update manager because the slave"
                 << " is in " << state << " state";
    return;
  }

  // Update the status update state of the task and include the latest
  // state of the task in the status update.
  Framework* framework = getFramework(update.framework_id());
  if (framework != NULL) {
    const TaskID& taskId = update.status().task_id();
    Executor* executor = framework->getExecutor(taskId);
    if (executor != NULL) {
      // NOTE: We do not look for the task in queued tasks because
      // no update is expected for it until it's launched. Similarly,
      // we do not look for completed tasks because the state for a
      // completed task shouldn't be changed.
      Task* task = NULL;
      if (executor->launchedTasks.contains(taskId)) {
        task = executor->launchedTasks[taskId];
      } else if (executor->terminatedTasks.contains(taskId)) {
        task = executor->terminatedTasks[taskId];
      }

      if (task != NULL) {
        CHECK(update.has_uuid())
          << "Expecting updates without 'uuid' to have been rejected";

        // We set the status update state of the task here because in
        // steady state master updates the status update state of the
        // task when it receives this update. If the master fails over,
        // slave re-registers with this task in this status update
        // state. Note that an acknowledgement for this update might
        // be enqueued on status update manager when we are here. But
        // that is ok because the status update state will be updated
        // when the next update is forwarded to the slave.
        task->set_status_update_state(update.status().state());
        task->set_status_update_uuid(update.uuid());

        // Include the latest state of task in the update. See the
        // comments in 'statusUpdate()' on why informing the master
        // about the latest state of the task is important.
        update.set_latest_state(task->state());
      }
    }
  }

  CHECK_SOME(master);
  LOG(INFO) << "Forwarding the update " << update << " to " << master.get();

  // NOTE: We forward the update even if framework/executor/task
  // doesn't exist because the status update manager will be expecting
  // an acknowledgement for the update. This could happen for example
  // if this is a retried terminal update and before we are here the
  // slave has already processed the acknowledgement of the original
  // update and removed the framework/executor/task. Also, slave
  // re-registration can generate updates when framework/executor/task
  // are unknown.

  // Forward the update to master.
  StatusUpdateMessage message;
  message.mutable_update()->MergeFrom(update);
  message.set_pid(self()); // The ACK will be first received by the slave.

  send(master.get(), message);
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
    metrics.invalid_framework_messages++;
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Cannot send framework message from executor "
                 << executorId << " to framework " << frameworkId
                 << " because framework does not exist";
    metrics.invalid_framework_messages++;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring framework message from executor "
                 << executorId << " to framework " << frameworkId
                 << " because framework is terminating";
    metrics.invalid_framework_messages++;
    return;
  }

  ExecutorToFrameworkMessage message;
  message.mutable_slave_id()->MergeFrom(slaveId);
  message.mutable_framework_id()->MergeFrom(frameworkId);
  message.mutable_executor_id()->MergeFrom(executorId);
  message.set_data(data);

  CHECK_SOME(master);

  if (framework->pid.isSome()) {
    LOG(INFO) << "Sending message for framework " << frameworkId
              << " to " << framework->pid.get();
    send(framework->pid.get(), message);
  } else {
    LOG(INFO) << "Sending message for framework " << frameworkId
              << " through the master " << master.get();
    send(master.get(), message);
  }

  metrics.valid_framework_messages++;
}


void Slave::pingOld(const UPID& from, const string& body)
{
  VLOG(1) << "Received ping from " << from;

  if (!body.empty()) {
    // This must be a ping from 0.21.0 master.
    PingSlaveMessage message;
    CHECK(message.ParseFromString(body))
      << "Invalid ping message '" << body << "' from " << from;

    if (!message.connected() && state == RUNNING) {
      // This could happen if there is a one way partition between
      // the master and slave, causing the master to get an exited
      // event and marking the slave disconnected but the slave
      // thinking it is still connected. Force a re-registration with
      // the master to reconcile.
      LOG(INFO) << "Master marked the slave as disconnected but the slave"
                << " considers itself registered! Forcing re-registration.";
      detection.discard();
    }
  }

  // If we don't get a ping from the master, trigger a
  // re-registration. This can occur when the master no
  // longer considers the slave to be registered, so it is
  // essential for the slave to attempt a re-registration
  // when this occurs.
  Clock::cancel(pingTimer);

  pingTimer = delay(
      masterPingTimeout,
      self(),
      &Slave::pingTimeout,
      detection);

  send(from, "PONG");
}


void Slave::ping(const UPID& from, bool connected)
{
  VLOG(1) << "Received ping from " << from;

  if (!connected && state == RUNNING) {
    // This could happen if there is a one way partition between
    // the master and slave, causing the master to get an exited
    // event and marking the slave disconnected but the slave
    // thinking it is still connected. Force a re-registration with
    // the master to reconcile.
    LOG(INFO) << "Master marked the slave as disconnected but the slave"
              << " considers itself registered! Forcing re-registration.";
    detection.discard();
  }

  // If we don't get a ping from the master, trigger a
  // re-registration. This can occur when the master no
  // longer considers the slave to be registered, so it is
  // essential for the slave to attempt a re-registration
  // when this occurs.
  Clock::cancel(pingTimer);

  pingTimer = delay(
      masterPingTimeout,
      self(),
      &Slave::pingTimeout,
      detection);

  send(from, PongSlaveMessage());
}


void Slave::pingTimeout(Future<Option<MasterInfo>> future)
{
  // It's possible that a new ping arrived since the timeout fired
  // and we were unable to cancel this timeout. If this occurs, don't
  // bother trying to re-detect.
  if (pingTimer.timeout().expired()) {
    LOG(INFO) << "No pings from master received within "
              << masterPingTimeout;

    future.discard();
  }
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

    if (task.has_container() &&
        task.container().type() != ContainerInfo::MESOS) {
      // Store the container info in the executor info so it will
      // be checkpointed. This allows the correct containerizer to
      // recover this task on restart.
      executor.mutable_container()->CopyFrom(task.container());
    }

    // Prepare an executor name which includes information on the
    // command being launched.
    string name = "(Task: " + task.task_id().value() + ") ";

    if (task.command().shell()) {
      if (!task.command().has_value()) {
        name += "(Command: NO COMMAND)";
      } else {
        name += "(Command: sh -c '";
        if (task.command().value().length() > 15) {
          name += task.command().value().substr(0, 12) + "...')";
        } else {
          name += task.command().value() + "')";
        }
      }
    } else {
      if (!task.command().has_value()) {
        name += "(Command: NO EXECUTABLE)";
      } else {
        string args =
          task.command().value() + ", " +
          strings::join(", ", task.command().arguments());

        if (args.length() > 15) {
          name += "(Command: [" + args.substr(0, 12) + "...])";
        } else {
          name += "(Command: [" + args + "])";
        }
      }
    }

    executor.set_name("Command Executor " + name);
    executor.set_source(task.task_id().value());

    // Copy the [uris, environment, container, user] fields from the
    // CommandInfo to get the URIs we need to download, the
    // environment variables that should get set, the necessary
    // container information, and the user to run the executor as but
    // nothing else because we need to set up the rest of the executor
    // command ourselves in order to invoke 'mesos-executor'.
    executor.mutable_command()->mutable_uris()->MergeFrom(
        task.command().uris());

    if (task.command().has_environment()) {
      executor.mutable_command()->mutable_environment()->MergeFrom(
          task.command().environment());
    }

    if (task.command().has_container()) {
      executor.mutable_command()->mutable_container()->MergeFrom(
          task.command().container());
    }

    if (task.command().has_user()) {
      executor.mutable_command()->set_user(task.command().user());
    }

    Result<string> path =
      os::realpath(path::join(flags.launcher_dir, "mesos-executor"));

    // Explicitly set 'shell' to true since we want to use the shell
    // for running the mesos-executor (and even though this is the
    // default we want to be explicit).
    executor.mutable_command()->set_shell(true);

    if (path.isSome()) {
      executor.mutable_command()->set_value(path.get());
    } else {
      executor.mutable_command()->set_value(
          "echo '" +
          (path.isError() ? path.error() : "No such file or directory") +
          "'; exit 1");
    }

    // Add an allowance for the command executor. This does lead to a
    // small overcommit of resources.
    // TODO(vinod): If a task is using revocable resources, mark the
    // corresponding executor resource (e.g., cpus) to be also
    // revocable. Currently, it is OK because the containerizer is
    // given task + executor resources on task launch resulting in
    // the container being correctly marked as revocable.
    executor.mutable_resources()->MergeFrom(
        Resources::parse(
          "cpus:" + stringify(DEFAULT_EXECUTOR_CPUS) + ";" +
          "mem:" + stringify(DEFAULT_EXECUTOR_MEM.megabytes())).get());

    return executor;
  }

  return task.executor();
}


void Slave::executorLaunched(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const Future<bool>& future)
{
  // Set up callback for executor termination. Note that we do this
  // regardless of whether or not we have successfully launched the
  // executor because even if we failed to launch the executor the
  // result of calling 'wait' will make sure everything gets properly
  // cleaned up. Note that we do this here instead of where we do
  // Containerizer::launch because we want to guarantee the contract
  // with the Containerizer that we won't call 'wait' until after the
  // launch has completed.
  containerizer->wait(containerId)
    .onAny(defer(self(),
                 &Self::executorTerminated,
                 frameworkId,
                 executorId,
                 lambda::_1));

  if (!future.isReady()) {
    LOG(ERROR) << "Container '" << containerId
               << "' for executor '" << executorId
               << "' of framework '" << frameworkId
               << "' failed to start: "
               << (future.isFailed() ? future.failure() : " future discarded");

    ++metrics.container_launch_errors;

    containerizer->destroy(containerId);
    return;
  } else if (!future.get()) {
    LOG(ERROR) << "Container '" << containerId
               << "' for executor '" << executorId
               << "' of framework '" << frameworkId
               << "' failed to start: None of the enabled containerizers ("
               << flags.containerizers << ") could create a container for the "
               << "provided TaskInfo/ExecutorInfo message.";

     ++metrics.container_launch_errors;
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
      break;
    case Executor::TERMINATED:
    default:
      LOG(FATAL) << " Executor '" << executorId
                 << "' of framework '" << frameworkId
                 << "' is in an unexpected state " << executor->state;
      break;
  }
}


// Called by the isolator when an executor process terminates.
void Slave::executorTerminated(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Future<containerizer::Termination>& termination)
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
  } else if (!termination.get().has_status()) {
    LOG(INFO) << "Executor '" << executorId
              << "' of framework " << frameworkId
              << " has terminated with unknown status";
    // Set a special status for None.
    status = -1;
  } else {
    status = termination.get().status();
    LOG(INFO) << "Executor '" << executorId
              << "' of framework " << frameworkId << " "
              << WSTRINGIFY(status);
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
      ++metrics.executors_terminated;

      executor->state = Executor::TERMINATED;

      // Transition all live tasks to TASK_LOST/TASK_FAILED.
      // If the containerizer killed the executor (e.g., due to OOM event)
      // or if this is a command executor, we send TASK_FAILED status updates
      // instead of TASK_LOST.
      // NOTE: We don't send updates if the framework is terminating
      // because we don't want the status update manager to keep retrying
      // these updates since it won't receive ACKs from the scheduler.  Also,
      // the status update manager should have already cleaned up all the
      // status update streams for a framework that is terminating.
      if (framework->state != Framework::TERMINATING) {
        // Transition all live launched tasks.
        // TODO(vinod): Use foreachvalue instead once LinkedHashmap
        // supports it.
        foreach (Task* task, executor->launchedTasks.values()) {
          if (!protobuf::isTerminalState(task->state())) {
            sendExecutorTerminatedStatusUpdate(
                task->task_id(), termination, frameworkId, executor);
          }
        }

        // Transition all queued tasks.
        // TODO(vinod): Use foreachvalue instead once LinkedHashmap
        // supports it.
        foreach (const TaskInfo& task, executor->queuedTasks.values()) {
          sendExecutorTerminatedStatusUpdate(
              task.task_id(), termination, frameworkId, executor);
        }
      }

      // Only send ExitedExecutorMessage if it is not a Command
      // Executor because the master doesn't store them; they are
      // generated by the slave.
      // TODO(vinod): Reliably forward this message to the master.
      if (!executor->isCommandExecutor()) {
        ExitedExecutorMessage message;
        message.mutable_slave_id()->MergeFrom(info.id());
        message.mutable_framework_id()->MergeFrom(frameworkId);
        message.mutable_executor_id()->MergeFrom(executorId);
        message.set_status(status);

        if (master.isSome()) { send(master.get(), message); }
      }

      // Remove the executor if either the slave or framework is
      // terminating or there are no incomplete tasks.
      if (state == TERMINATING ||
          framework->state == Framework::TERMINATING ||
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
                 << "' of framework " << framework->id()
                 << " in unexpected state " << executor->state;
      break;
  }
}


void Slave::removeExecutor(Framework* framework, Executor* executor)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(executor);

  LOG(INFO) << "Cleaning up executor '" << executor->id
            << "' of framework " << framework->id();

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  // Check that this executor has terminated.
  CHECK(executor->state == Executor::TERMINATED) << executor->state;

  // Check that either 1) the executor has no tasks with pending
  // updates or 2) the slave/framework is terminating, because no
  // acknowledgements might be received.
  CHECK(!executor->incompleteTasks() ||
        state == TERMINATING ||
        framework->state == Framework::TERMINATING);

  // Write a sentinel file to indicate that this executor
  // is completed.
  if (executor->checkpoint) {
    const string path = paths::getExecutorSentinelPath(
        metaDir,
        info.id(),
        framework->id(),
        executor->id,
        executor->containerId);
    CHECK_SOME(os::touch(path));
  }

  // TODO(vinod): Move the responsibility of gc'ing to the
  // Executor struct.

  // Schedule the executor run work directory to get garbage collected.
  const string path = paths::getExecutorRunPath(
      flags.work_dir,
      info.id(),
      framework->id(),
      executor->id,
      executor->containerId);

  os::utime(path); // Update the modification time.
  garbageCollect(path)
    .then(defer(self(), &Self::detachFile, path));

  // Schedule the top level executor work directory, only if the
  // framework doesn't have any 'pending' tasks for this executor.
  if (!framework->pending.contains(executor->id)) {
    const string path = paths::getExecutorPath(
        flags.work_dir, info.id(), framework->id(), executor->id);

    os::utime(path); // Update the modification time.
    garbageCollect(path);
  }

  if (executor->checkpoint) {
    // Schedule the executor run meta directory to get garbage collected.
    const string path = paths::getExecutorRunPath(
        metaDir,
        info.id(),
        framework->id(),
        executor->id,
        executor->containerId);

    os::utime(path); // Update the modification time.
    garbageCollect(path);

    // Schedule the top level executor meta directory, only if the
    // framework doesn't have any 'pending' tasks for this executor.
    if (!framework->pending.contains(executor->id)) {
      const string path = paths::getExecutorPath(
          metaDir, info.id(), framework->id(), executor->id);

      os::utime(path); // Update the modification time.
      garbageCollect(path);
    }
  }

  if (HookManager::hooksAvailable()) {
    HookManager::slaveRemoveExecutorHook(framework->info, executor->info);
  }

  framework->destroyExecutor(executor->id);
}


void Slave::removeFramework(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO)<< "Cleaning up framework " << framework->id();

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING);

  // The invariant here is that a framework should not be removed
  // if it has either pending executors or pending tasks.
  CHECK(framework->executors.empty());
  CHECK(framework->pending.empty());

  // Close all status update streams for this framework.
  statusUpdateManager->cleanup(framework->id());

  // Schedule the framework work and meta directories for garbage
  // collection.
  // TODO(vinod): Move the responsibility of gc'ing to the
  // Framework struct.

  const string path = paths::getFrameworkPath(
      flags.work_dir, info.id(), framework->id());

  os::utime(path); // Update the modification time.
  garbageCollect(path);

  if (framework->info.checkpoint()) {
    // Schedule the framework meta directory to get garbage collected.
    const string path = paths::getFrameworkPath(
        metaDir, info.id(), framework->id());

    os::utime(path); // Update the modification time.
    garbageCollect(path);
  }

  frameworks.erase(framework->id());

  // Pass ownership of the framework pointer.
  completedFrameworks.push_back(Owned<Framework>(framework));

  if (state == TERMINATING && frameworks.empty()) {
    terminate(self());
  }
}


void Slave::shutdownExecutor(
    const UPID& from,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (from && master != from) {
    LOG(WARNING) << "Ignoring shutdown executor message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " from " << from << " because it is not from the"
                 << " registered master ("
                 << (master.isSome() ? stringify(master.get()) : "None") << ")";
    return;
  }

  LOG(INFO) << "Asked to shut down executor '" << executorId
            << "' of framework "<< frameworkId << " by " << from;

  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state == RECOVERING || state == DISCONNECTED) {
    LOG(WARNING) << "Ignoring shutdown executor message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because the slave has not yet registered with the master";
    return;
  }

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Cannot shut down executor '" << executorId
                 << "' of unknown framework " << frameworkId;
    return;
  }

  CHECK(framework->state == Framework::RUNNING ||
        framework->state == Framework::TERMINATING)
    << framework->state;

  if (framework->state == Framework::TERMINATING) {
    LOG(WARNING) << "Ignoring shutdown executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the framework is terminating";
    return;
  }

  if (!framework->executors.contains(executorId)) {
    LOG(WARNING) << "Ignoring shutdown of unknown executor '" << executorId
                 << "' of framework " << frameworkId;
  }

  Executor* executor = framework->executors[executorId];
  CHECK(executor->state == Executor::REGISTERING ||
        executor->state == Executor::RUNNING ||
        executor->state == Executor::TERMINATING ||
        executor->state == Executor::TERMINATED)
    << executor->state;

  if (executor->state == Executor::TERMINATING ||
      executor->state == Executor::TERMINATED) {
    LOG(WARNING) << "Ignoring shutdown executor '" << executorId
                 << "' of framework " << frameworkId
                 << " because the executor is terminating/terminated";
  }

  _shutdownExecutor(framework, executor);
}


void Slave::_shutdownExecutor(Framework* framework, Executor* executor)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(executor);

  LOG(INFO) << "Shutting down executor '" << executor->id
            << "' of framework " << framework->id();

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
        framework->id(),
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

  // Make sure this timeout is valid.
  if (executor->containerId != containerId) {
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
                << "' of framework " << framework->id();

      containerizer->destroy(executor->containerId);
      break;
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id()
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

  if (executor->containerId != containerId) {
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
                << " of framework " << framework->id()
                << " because it did not register within "
                << flags.executor_registration_timeout;

      executor->state = Executor::TERMINATING;

      // Immediately kill the executor.
      containerizer->destroy(executor->containerId);
      break;
    default:
      LOG(FATAL) << "Executor '" << executor->id
                 << "' of framework " << framework->id()
                 << " is in unexpected state " << executor->state;
      break;
  }
}


// TODO(vinod): Figure out a way to express this function via cmd line.
Duration Slave::age(double usage)
{
  return flags.gc_delay * std::max(0.0, (1.0 - flags.gc_disk_headroom - usage));
}


void Slave::checkDiskUsage()
{
  // TODO(vinod): We are making usage a Future, so that we can plug in
  // fs::usage() into async.
  // NOTE: We calculate disk usage of the file system on which the
  // slave work directory is mounted.
  Future<double>(fs::usage(flags.work_dir))
    .onAny(defer(self(), &Slave::_checkDiskUsage, lambda::_1));
}


void Slave::_checkDiskUsage(const Future<double>& usage)
{
  if (!usage.isReady()) {
    LOG(ERROR) << "Failed to get disk usage: "
               << (usage.isFailed() ? usage.failure() : "future discarded");
  } else {
    executorDirectoryMaxAllowedAge = age(usage.get());
    LOG(INFO) << "Current disk usage " << std::setiosflags(std::ios::fixed)
              << std::setprecision(2) << 100 * usage.get() << "%."
              << " Max allowed age: " << executorDirectoryMaxAllowedAge;

    // We prune all directories whose deletion time is within
    // the next 'gc_delay - age'. Since a directory is always
    // scheduled for deletion 'gc_delay' into the future, only directories
    // that are at least 'age' old are deleted.
    gc->prune(flags.gc_delay - executorDirectoryMaxAllowedAge);
  }
  delay(flags.disk_watch_interval, self(), &Slave::checkDiskUsage);
}


Future<Nothing> Slave::recover(const Result<state::State>& state)
{
  if (state.isError()) {
    return Failure(state.error());
  }

  Option<ResourcesState> resourcesState;
  Option<SlaveState> slaveState;
  if (state.isSome()) {
    resourcesState = state.get().resources;
    slaveState = state.get().slave;
  }

  // Recover checkpointed resources.
  // NOTE: 'resourcesState' is None if the slave rootDir does not
  // exist or the resources checkpoint file cannot be found.
  if (resourcesState.isSome()) {
    if (resourcesState.get().errors > 0) {
      LOG(WARNING) << "Errors encountered during resources recovery: "
                   << resourcesState.get().errors;

      metrics.recovery_errors += resourcesState.get().errors;
    }

    // This is to verify that the checkpointed resources are
    // compatible with the slave resources specified through the
    // '--resources' command line flag.
    Try<Resources> totalResources = applyCheckpointedResources(
        info.resources(),
        resourcesState.get().resources);

    if (totalResources.isError()) {
      return Failure(
          "Checkpointed resources " +
          stringify(resourcesState.get().resources) +
          " are incompatible with slave resources " +
          stringify(info.resources()) + ": " +
          totalResources.error());
    }

    checkpointedResources = resourcesState.get().resources;
  }

  if (slaveState.isSome() && slaveState.get().info.isSome()) {
    // Check for SlaveInfo compatibility.
    // TODO(vinod): Also check for version compatibility.
    // NOTE: We set the 'id' field in 'info' from the recovered slave,
    // as a hack to compare the info created from options/flags with
    // the recovered info.
    info.mutable_id()->CopyFrom(slaveState.get().id);
    if (flags.recover == "reconnect" &&
        !(info == slaveState.get().info.get())) {
      return Failure(strings::join(
          "\n",
          "Incompatible slave info detected.",
          "------------------------------------------------------------",
          "Old slave info:\n" + stringify(slaveState.get().info.get()),
          "------------------------------------------------------------",
          "New slave info:\n" + stringify(info),
          "------------------------------------------------------------"));
    }

    info = slaveState.get().info.get(); // Recover the slave info.

    if (slaveState.get().errors > 0) {
      LOG(WARNING) << "Errors encountered during slave recovery: "
                   << slaveState.get().errors;

      metrics.recovery_errors += slaveState.get().errors;
    }

    // TODO(bernd-mesos): Make this an instance method call, see comment
    // in "fetcher.hpp"".
    Try<Nothing> recovered = Fetcher::recover(slaveState.get().id, flags);
    if (recovered.isError()) {
      return Failure(recovered.error());
    }

    // Recover the frameworks.
    foreachvalue (const FrameworkState& frameworkState,
                  slaveState.get().frameworks) {
      recoverFramework(frameworkState);
    }
  }

  return statusUpdateManager->recover(metaDir, slaveState)
    .then(defer(self(), &Slave::_recoverContainerizer, slaveState));
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
      // Set up callback for executor termination.
      containerizer->wait(executor->containerId)
        .onAny(defer(self(),
                     &Self::executorTerminated,
                     framework->id(),
                     executor->id,
                     lambda::_1));

      if (flags.recover == "reconnect") {
        if (executor->pid) {
          LOG(INFO) << "Sending reconnect request to executor " << executor->id
                    << " of framework " << framework->id()
                    << " at " << executor->pid;

          ReconnectExecutorMessage message;
          message.mutable_slave_id()->MergeFrom(info.id());
          send(executor->pid, message);
        } else {
          LOG(INFO) << "Unable to reconnect to executor '" << executor->id
                    << "' of framework " << framework->id()
                    << " because no libprocess PID was found";
        }
      } else {
        if (executor->pid) {
          // Cleanup executors.
          LOG(INFO) << "Sending shutdown to executor '" << executor->id
                    << "' of framework " << framework->id()
                    << " to " << executor->pid;

          _shutdownExecutor(framework, executor);
        } else {
          LOG(INFO) << "Killing executor '" << executor->id
                    << "' of framework " << framework->id()
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

  // Checkpoint boot ID.
  Try<string> bootId = os::bootId();
  if (bootId.isError()) {
    LOG(ERROR) << "Could not retrieve boot id: " << bootId.error();
  } else {
    const string path = paths::getBootIdPath(metaDir);
    CHECK_SOME(state::checkpoint(path, bootId.get()));
  }

  // Schedule all old slave directories for garbage collection.
  // TODO(vinod): Do this as part of recovery. This needs a fix
  // in the recovery code, to recover all slaves instead of only
  // the latest slave.
  const string directory = path::join(flags.work_dir, "slaves");
  Try<list<string>> entries = os::ls(directory);
  if (entries.isSome()) {
    foreach (const string& entry, entries.get()) {
      string path = path::join(directory, entry);
      // Ignore non-directory entries.
      if (!os::stat::isdir(path)) {
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
  }

  if (flags.recover == "reconnect") {
    state = DISCONNECTED;

    // Start detecting masters.
    detection = detector->detect()
      .onAny(defer(self(), &Slave::detected, lambda::_1));

    // Forward oversubscribed resources.
    forwardOversubscribed();

    // Start acting on correction from QoS Controller.
    qosCorrections();
  } else {
    // Slave started in cleanup mode.
    CHECK_EQ("cleanup", flags.recover);
    state = TERMINATING;

    if (frameworks.empty()) {
      terminate(self());
    }

    // If there are active executors/frameworks, the slave will
    // shutdown when all the executors are terminated. Note that
    // the executors are guaranteed to terminate because they
    // are sent shutdown signal in '_recover()' which results in
    // 'Containerizer::destroy()' being called if the termination
    // doesn't happen within a timeout.
  }

  recovered.set(Nothing()); // Signal recovery.
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

  CHECK_SOME(state.info);
  FrameworkInfo frameworkInfo = state.info.get();

  // Mesos 0.22 and earlier didn't write the FrameworkID into the FrameworkInfo.
  // In this case, we we update FrameworkInfo.framework_id from directory name,
  // and rewrite the new format when we are done.
  bool recheckpoint = false;
  if (!frameworkInfo.has_id()) {
    frameworkInfo.mutable_id()->CopyFrom(state.id);
    recheckpoint = true;
  }

  CHECK(frameworkInfo.has_id());
  CHECK(frameworkInfo.checkpoint());

  // In 0.24.0, HTTP schedulers are supported and these do not
  // have a 'pid'. In this case, the slave will checkpoint UPID().
  CHECK_SOME(state.pid);

  Option<UPID> pid = state.pid.get();

  if (pid.get() == UPID()) {
    pid = None();
  }

  Framework* framework = new Framework(this, frameworkInfo, pid);
  frameworks[framework->id()] = framework;

  if (recheckpoint) {
    framework->checkpointFramework();
  }

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
  Try<long> mtime = os::stat::mtime(path);
  if (mtime.isError()) {
    LOG(ERROR) << "Failed to find the mtime of '" << path
               << "': " << mtime.error();
    return Failure(mtime.error());
  }

  // It is unsafe for testing to use unix time directly, we must use
  // Time::create to convert into a Time object that reflects the
  // possibly advanced state of the libprocess Clock.
  Try<Time> time = Time::create(mtime.get());
  CHECK_SOME(time);

  // GC based on the modification time.
  Duration delay = flags.gc_delay - (Clock::now() - time.get());

  return gc->schedule(delay, path);
}


void Slave::forwardOversubscribed()
{
  VLOG(1) << "Querying resource estimator for oversubscribable resources";

  resourceEstimator->oversubscribable()
    .onAny(defer(self(), &Self::_forwardOversubscribed, lambda::_1));
}


void Slave::_forwardOversubscribed(const Future<Resources>& oversubscribable)
{
  if (!oversubscribable.isReady()) {
    LOG(ERROR) << "Failed to get oversubscribable resources: "
               << (oversubscribable.isFailed()
                   ? oversubscribable.failure() : "future discarded");
  } else {
    VLOG(1) << "Received oversubscribable resources "
            << oversubscribable.get() << " from the resource estimator";

    // Calculate the latest allocation of oversubscribed resources.
    // Note that this allocation value might be different from the
    // master's view because new task/executor might be in flight from
    // the master or pending on the slave etc. This is ok because the
    // allocator only considers the slave's view of allocation when
    // calculating the available oversubscribed resources to offer.
    Resources oversubscribed;
    foreachvalue (Framework* framework, frameworks) {
      foreachvalue (Executor* executor, framework->executors) {
        oversubscribed += executor->resources.revocable();
      }
    }

    // Add oversubscribable resources to the total.
    oversubscribed += oversubscribable.get();

    // Only forward the estimate if it's different from the previous
    // estimate. We also send this whenever we get (re-)registered
    // (i.e. whenever we transition into the RUNNING state).
    if (state == RUNNING && oversubscribedResources != oversubscribed) {
      LOG(INFO) << "Forwarding total oversubscribed resources "
                << oversubscribed;

      UpdateSlaveMessage message;
      message.mutable_slave_id()->CopyFrom(info.id());
      message.mutable_oversubscribed_resources()->CopyFrom(oversubscribed);

      CHECK_SOME(master);
      send(master.get(), message);
    }

    // Update the estimate.
    oversubscribedResources = oversubscribed;
  }

  delay(flags.oversubscribed_resources_interval,
        self(),
        &Self::forwardOversubscribed);
}


void Slave::qosCorrections()
{
  qosController->corrections()
    .onAny(defer(self(), &Self::_qosCorrections, lambda::_1));
}


void Slave::_qosCorrections(const Future<list<QoSCorrection>>& future)
{
  // Make sure correction handler is scheduled again.
  delay(flags.qos_correction_interval_min, self(), &Self::qosCorrections);

  // Verify slave state.
  CHECK(state == RECOVERING || state == DISCONNECTED ||
        state == RUNNING || state == TERMINATING)
    << state;

  if (state == RECOVERING || state == TERMINATING) {
    LOG(WARNING) << "Cannot perform QoS corrections because the slave is "
                 << state;
    return;
  }

  if (!future.isReady()) {
    LOG(WARNING) << "Failed to get corrections from QoS Controller: "
                  << (future.isFailed() ? future.failure() : "discarded");
    return;
  }

  const list<QoSCorrection>& corrections = future.get();

  LOG(INFO) << "Received " << corrections.size() << " QoS corrections";

  foreach (const QoSCorrection& correction, corrections) {
    // TODO(nnielsen): Print correction, once the operator overload
    // for QoSCorrection has been implemented.
    if (correction.type() == QoSCorrection::KILL) {
      const QoSCorrection::Kill& kill = correction.kill();

      if (!kill.has_framework_id()) {
        LOG(WARNING) << "Ignoring QoS correction KILL: "
                     << "framework id not specified.";
        continue;
      }

      const FrameworkID& frameworkId = kill.framework_id();

      if (!kill.has_executor_id()) {
        // TODO(nnielsen): For now, only executor killing is supported. Check
        // can be removed when task killing is supported as well.
        LOG(WARNING) << "Ignoring QoS correction KILL on framework "
                     << frameworkId << ": executor id not specified";
        continue;
      }

      const ExecutorID& executorId = kill.executor_id();

      Framework* framework = getFramework(frameworkId);
      if (framework == NULL) {
        LOG(WARNING) << "Ignoring QoS correction KILL on framework "
                     << frameworkId << ": framework cannot be found";
        continue;
      }

      // Verify framework state.
      CHECK(framework->state == Framework::RUNNING ||
            framework->state == Framework::TERMINATING)
        << framework->state;

      if (framework->state == Framework::TERMINATING) {
        LOG(WARNING) << "Ignoring QoS correction KILL on framework "
                     << frameworkId << ": framework is terminating.";
        continue;
      }

      Executor* executor = framework->getExecutor(executorId);
      if (executor == NULL) {
        LOG(WARNING) << "Ignoring QoS correction KILL on executor '"
                     << executorId << "' of framework " << frameworkId
                     << ": executor cannot be found";
        continue;
      }

      switch (executor->state) {
        case Executor::REGISTERING:
        case Executor::RUNNING: {
          LOG(INFO) << "Killing executor '" << executorId
                    << "' of framework " << frameworkId
                    << " as QoS correction";

          // TODO(nnielsen): We should ensure that we are addressing
          // the _container_ which the QoS controller intended to
          // kill. Without this check, we may run into a scenario
          // where the executor has terminated and one with the same
          // id has started in the interim i.e. running in a different
          // container than the one the QoS controller targeted
          // (MESOS-2875).
          executor->state = Executor::TERMINATING;
          executor->reason = TaskStatus::REASON_EXECUTOR_PREEMPTED;
          containerizer->destroy(executor->containerId);

          ++metrics.executors_preempted;
          break;
        }
        case Executor::TERMINATING:
        case Executor::TERMINATED:
          LOG(WARNING) << "Ignoring QoS correction KILL on executor '"
                       << executorId << "' of framework " << frameworkId
                       << ": executor is " << executor->state;
          break;
        default:
          LOG(FATAL) << " Executor '" << executor->id
                     << "' of framework " << framework->id()
                     << " is in unexpected state " << executor->state;
          break;
      }
    } else {
      LOG(WARNING) << "QoS correction type " << correction.type()
                   << " is not supported";
    }
  }
}


Future<ResourceUsage> Slave::usage()
{
  // NOTE: We use 'Owned' here trying to avoid the expensive copy.
  // C++11 lambda only supports capturing variables that have copy
  // constructors. Revisit once we remove the copy constructor for
  // Owned (or C++14 lambda generalized capture is supported).
  Owned<ResourceUsage> usage(new ResourceUsage());
  list<Future<ResourceStatistics>> futures;

  foreachvalue (const Framework* framework, frameworks) {
    foreachvalue (const Executor* executor, framework->executors) {
      ResourceUsage::Executor* entry = usage->add_executors();
      entry->mutable_executor_info()->CopyFrom(executor->info);
      entry->mutable_allocated()->CopyFrom(executor->resources);

      futures.push_back(containerizer->usage(executor->containerId));
    }
  }

  Try<Resources> totalResources = applyCheckpointedResources(
      info.resources(),
      checkpointedResources);

  CHECK_SOME(totalResources)
    << "Failed to apply checkpointed resources "
    << checkpointedResources << " to slave's resources "
    << info.resources();

  usage->mutable_total()->CopyFrom(totalResources.get());

  return await(futures).then(
      [usage](const list<Future<ResourceStatistics>>& futures) {
        // NOTE: We add ResourceUsage::Executor to 'usage' the same
        // order as we push future to 'futures'. So the variables
        // 'future' and 'executor' below should be in sync.
        CHECK_EQ(futures.size(), (size_t) usage->executors_size());

        size_t i = 0;
        foreach (const Future<ResourceStatistics>& future, futures) {
          ResourceUsage::Executor* executor = usage->mutable_executors(i++);

          if (future.isReady()) {
            executor->mutable_statistics()->CopyFrom(future.get());
          } else {
            LOG(WARNING) << "Failed to get resource statistics for executor '"
                         << executor->executor_info().executor_id() << "'"
                         << " of framework "
                         << executor->executor_info().framework_id() << ": "
                         << (future.isFailed() ? future.failure()
                                               : "discarded");
          }
        }

        return Future<ResourceUsage>(*usage);
      });
}


// TODO(dhamon): Move these to their own metrics.hpp|cpp.
double Slave::_tasks_staging()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    typedef hashmap<TaskID, TaskInfo> TaskMap;
    foreachvalue (const TaskMap& tasks, framework->pending) {
      count += tasks.size();
    }

    foreachvalue (Executor* executor, framework->executors) {
      count += executor->queuedTasks.size();

      foreach (Task* task, executor->launchedTasks.values()) {
        if (task->state() == TASK_STAGING) {
          count++;
        }
      }
    }
  }
  return count;
}


double Slave::_tasks_starting()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      foreach (Task* task, executor->launchedTasks.values()) {
        if (task->state() == TASK_STARTING) {
          count++;
        }
      }
    }
  }
  return count;
}


double Slave::_tasks_running()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      foreach (Task* task, executor->launchedTasks.values()) {
        if (task->state() == TASK_RUNNING) {
          count++;
        }
      }
    }
  }
  return count;
}


double Slave::_executors_registering()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      if (executor->state == Executor::REGISTERING) {
        count++;
      }
    }
  }
  return count;
}


double Slave::_executors_running()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      if (executor->state == Executor::RUNNING) {
        count++;
      }
    }
  }
  return count;
}


double Slave::_executors_terminating()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      if (executor->state == Executor::TERMINATING) {
        count++;
      }
    }
  }
  return count;
}


double Slave::_executor_directory_max_allowed_age_secs()
{
  return executorDirectoryMaxAllowedAge.secs();
}


void Slave::sendExecutorTerminatedStatusUpdate(
    const TaskID& taskId,
    const Future<containerizer::Termination>& termination,
    const FrameworkID& frameworkId,
    const Executor* executor)
{
  mesos::TaskState taskState = TASK_LOST;
  TaskStatus::Reason reason = TaskStatus::REASON_EXECUTOR_TERMINATED;

  CHECK_NOTNULL(executor);

  if (executor->reason.isSome()) {
    // TODO(nnielsen): We want to dispatch the task status and reason
    // from the termination reason (MESOS-2035) and the executor
    // reason based on a specific policy i.e. if the termination
    // reason is set, this overrides executor->reason. At the moment,
    // we infer the containerizer reason for killing from 'killed'
    // field in 'termination' and are explicitly overriding the task
    // status and reason.
    reason = executor->reason.get();
  } else if (termination.isReady() && termination.get().killed()) {
    taskState = TASK_FAILED;
    // TODO(dhamon): MESOS-2035: Add 'reason' to containerizer::Termination.
    reason = TaskStatus::REASON_MEMORY_LIMIT;
  } else if (executor->isCommandExecutor()) {
    taskState = TASK_FAILED;
    reason = TaskStatus::REASON_COMMAND_EXECUTOR_FAILED;
  }

  statusUpdate(protobuf::createStatusUpdate(
      frameworkId,
      info.id(),
      taskId,
      taskState,
      TaskStatus::SOURCE_SLAVE,
      UUID::random(),
      termination.isReady() ? termination.get().message()
                            : "Abnormal executor termination",
      reason,
      executor->id),
      UPID());
}


double Slave::_resources_total(const string& name)
{
  double total = 0.0;

  foreach (const Resource& resource, info.resources()) {
    if (resource.name() == name && resource.type() == Value::SCALAR) {
      total += resource.scalar().value();
    }
  }

  return total;
}


double Slave::_resources_used(const string& name)
{
  double used = 0.0;

  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      foreach (const Resource& resource,
               executor->resources - executor->resources.revocable()) {
        if (resource.name() == name && resource.type() == Value::SCALAR) {
          used += resource.scalar().value();
        }
      }
    }
  }

  return used;
}


double Slave::_resources_percent(const string& name)
{
  double total = _resources_total(name);

  if (total == 0.0) {
    return 0.0;
  }

  return _resources_used(name) / total;
}


double Slave::_resources_revocable_total(const string& name)
{
  double total = 0.0;

  if (oversubscribedResources.isSome()) {
    foreach (const Resource& resource, oversubscribedResources.get()) {
      if (resource.name() == name && resource.type() == Value::SCALAR) {
        total += resource.scalar().value();
      }
    }
  }

  return total;
}


double Slave::_resources_revocable_used(const string& name)
{
  double used = 0.0;

  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      foreach (const Resource& resource, executor->resources.revocable()) {
        if (resource.name() == name && resource.type() == Value::SCALAR) {
          used += resource.scalar().value();
        }
      }
    }
  }

  return used;
}


double Slave::_resources_revocable_percent(const string& name)
{
  double total = _resources_revocable_total(name);

  if (total == 0.0) {
    return 0.0;
  }

  return _resources_revocable_used(name) / total;
}


Framework::Framework(
    Slave* _slave,
    const FrameworkInfo& _info,
    const Option<UPID>& _pid)
  : state(RUNNING),
    slave(_slave),
    info(_info),
    pid(_pid),
    completedExecutors(MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK) {}


void Framework::checkpointFramework() const
{
  // Checkpoint the framework info.
  string path = paths::getFrameworkInfoPath(
      slave->metaDir, slave->info.id(), id());

  VLOG(1) << "Checkpointing FrameworkInfo to '" << path << "'";

  CHECK_SOME(state::checkpoint(path, info));

  // Checkpoint the framework pid, note that we checkpoint a
  // UPID() when it is None (for HTTP schedulers) because
  // 0.23.x slaves consider a missing pid file to be an
  // error.
  path = paths::getFrameworkPidPath(
      slave->metaDir, slave->info.id(), id());

  VLOG(1) << "Checkpointing framework pid"
          << " '" << pid.getOrElse(UPID()) << "'"
          << " to '" << path << "'";

  CHECK_SOME(state::checkpoint(path, pid.getOrElse(UPID())));
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
  // TODO(idownes) This should be done by the containerizer but we
  // need the ContainerID to create the executor's directory. Fix
  // this when 'launchExecutor()' is handled asynchronously.
  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Option<string> user = None();
  if (slave->flags.switch_user) {
    // The command (either in form of task or executor command) can
    // define a specific user to run as. If present, this precedes the
    // framework user value. The selected user will have been verified by
    // the master at this point through the active ACLs.
    // NOTE: The global invariant is that the executor info at this
    // point is (1) the user provided task.executor() or (2) a command
    // executor constructed by the slave from the task.command().
    // If this changes, we need to check the user in both
    // task.command() and task.executor().command() below.
    user = info.user();
    if (executorInfo.command().has_user()) {
      user = executorInfo.command().user();
    }
  }

  // Create a directory for the executor.
  const string directory = paths::createExecutorDirectory(
      slave->flags.work_dir,
      slave->info.id(),
      id(),
      executorInfo.executor_id(),
      containerId,
      user);

  Executor* executor = new Executor(
      slave, id(), executorInfo, containerId, directory, info.checkpoint());

  if (executor->checkpoint) {
    executor->checkpointExecutor();
  }

  CHECK(!executors.contains(executorInfo.executor_id()))
    << "Unknown executor " << executorInfo.executor_id();

  executors[executorInfo.executor_id()] = executor;

  LOG(INFO) << "Launching executor " << executorInfo.executor_id()
            << " of framework " << id()
            << " with resources " << executorInfo.resources()
            << " in work directory '" << directory << "'";

  slave->files->attach(executor->directory, executor->directory)
    .onAny(defer(slave, &Slave::fileAttached, lambda::_1, executor->directory));

  // Tell the containerizer to launch the executor.
  // NOTE: We modify the ExecutorInfo to include the task's
  // resources when launching the executor so that the containerizer
  // has non-zero resources to work with when the executor has
  // no resources. This should be revisited after MESOS-600.
  ExecutorInfo executorInfo_ = executor->info;
  Resources resources = executorInfo_.resources();
  resources += taskInfo.resources();
  executorInfo_.mutable_resources()->CopyFrom(resources);

  // Launch the container.
  Future<bool> launch;
  if (!executor->isCommandExecutor()) {
    // If the executor is _not_ a command executor, this means that
    // the task will include the executor to run. The actual task to
    // run will be enqueued and subsequently handled by the executor
    // when it has registered to the slave.
    launch = slave->containerizer->launch(
        containerId,
        executorInfo_, // Modified to include the task's resources, see above.
        executor->directory,
        user,
        slave->info.id(),
        slave->self(),
        info.checkpoint());
  } else {
    // An executor has _not_ been provided by the task and will
    // instead define a command and/or container to run. Right now,
    // these tasks will require an executor anyway and the slave
    // creates a command executor. However, it is up to the
    // containerizer how to execute those tasks and the generated
    // executor info works as a placeholder.
    // TODO(nnielsen): Obsolete the requirement for executors to run
    // one-off tasks.
    launch = slave->containerizer->launch(
        containerId,
        taskInfo,
        executorInfo_, // Modified to include the task's resources, see above.
        executor->directory,
        user,
        slave->info.id(),
        slave->self(),
        info.checkpoint());
  }

  launch.onAny(defer(slave,
                     &Slave::executorLaunched,
                     id(),
                     executor->id,
                     containerId,
                     lambda::_1));

  // Make sure the executor registers within the given timeout.
  delay(slave->flags.executor_registration_timeout,
        slave,
        &Slave::registerExecutorTimeout,
        id(),
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
            << "' of framework " << id();

  CHECK_NOTNULL(slave);

  if (state.runs.empty() || state.latest.isNone() || state.info.isNone()) {
    LOG(WARNING) << "Skipping recovery of executor '" << state.id
                 << "' of framework " << id()
                 << " because its latest run or executor info"
                 << " cannot be recovered";

    // GC the top level executor work directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->flags.work_dir, slave->info.id(), id(), state.id));

    // GC the top level executor meta directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->metaDir, slave->info.id(), id(), state.id));

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
          slave->flags.work_dir, slave->info.id(), id(), state.id, runId));

      // GC the executor run's meta directory.
      slave->garbageCollect(paths::getExecutorRunPath(
          slave->metaDir, slave->info.id(), id(), state.id, runId));
    }
  }

  Option<RunState> run = state.runs.get(latest);
  CHECK_SOME(run)
      << "Cannot find latest run " << latest << " for executor " << state.id
      << " of framework " << id();

  // Create executor.
  const string directory = paths::getExecutorRunPath(
      slave->flags.work_dir, slave->info.id(), id(), state.id, latest);

  Executor* executor = new Executor(
      slave, id(), state.info.get(), latest, directory, info.checkpoint());

  // Recover the libprocess PID if possible.
  if (run.get().libprocessPid.isSome()) {
    // When recovering in non-strict mode, the assumption is that the
    // slave can die after checkpointing the forked pid but before the
    // libprocess pid. So, it is not possible for the libprocess pid
    // to exist but not the forked pid. If so, it is a really bad
    // situation (e.g., disk corruption).
    CHECK_SOME(run.get().forkedPid)
      << "Failed to get forked pid for executor " << state.id
      << " of framework " << id();

    executor->pid = run.get().libprocessPid.get();
  }

  // And finally recover all the executor's tasks.
  foreachvalue (const TaskState& taskState, run.get().tasks) {
    executor->recoverTask(taskState);
  }

  // Expose the executor's files.
  slave->files->attach(executor->directory, executor->directory)
    .onAny(defer(slave, &Slave::fileAttached, lambda::_1, executor->directory));

  // Add the executor to the framework.
  executors[executor->id] = executor;

  // If the latest run of the executor was completed (i.e., terminated
  // and all updates are acknowledged) in the previous run, we
  // transition its state to 'TERMINATED' and gc the directories.
  if (run.get().completed) {
    ++slave->metrics.executors_terminated;

    executor->state = Executor::TERMINATED;

    CHECK_SOME(run.get().id);
    const ContainerID& runId = run.get().id.get();

    // GC the executor run's work directory.
    const string path = paths::getExecutorRunPath(
        slave->flags.work_dir, slave->info.id(), id(), state.id, runId);

    slave->garbageCollect(path)
       .then(defer(slave, &Slave::detachFile, path));

    // GC the executor run's meta directory.
    slave->garbageCollect(paths::getExecutorRunPath(
        slave->metaDir, slave->info.id(), id(), state.id, runId));

    // GC the top level executor work directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->flags.work_dir, slave->info.id(), id(), state.id));

    // GC the top level executor meta directory.
    slave->garbageCollect(paths::getExecutorPath(
        slave->metaDir, slave->info.id(), id(), state.id));

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
    pid(UPID()),
    resources(_info.resources()),
    completedTasks(MAX_COMPLETED_TASKS_PER_EXECUTOR)
{
  CHECK_NOTNULL(slave);

  Result<string> executorPath =
    os::realpath(path::join(slave->flags.launcher_dir, "mesos-executor"));

  if (executorPath.isSome()) {
    commandExecutor =
      strings::contains(info.command().value(), executorPath.get());
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

  Task* t = new Task(protobuf::createTask(task, TASK_STAGING, frameworkId));

  launchedTasks[task.task_id()] = t;

  resources += task.resources();

  return t;
}


void Executor::terminateTask(
    const TaskID& taskId,
    const mesos::TaskStatus& status)
{
  VLOG(1) << "Terminating task " << taskId;

  Task* task = NULL;
  // Remove the task if it's queued.
  if (queuedTasks.contains(taskId)) {
    task = new Task(
        protobuf::createTask(queuedTasks[taskId], status.state(), frameworkId));
    queuedTasks.erase(taskId);
  } else if (launchedTasks.contains(taskId)) {
    // Update the resources if it's been launched.
    task = launchedTasks[taskId];
    resources -= task->resources();
    launchedTasks.erase(taskId);
  }

  switch (status.state()) {
    case TASK_FINISHED:
      ++slave->metrics.tasks_finished;
      break;
    case TASK_FAILED:
      ++slave->metrics.tasks_failed;
      break;
    case TASK_KILLED:
      ++slave->metrics.tasks_killed;
      break;
    case TASK_LOST:
      ++slave->metrics.tasks_lost;
      break;
    default:
      LOG(WARNING) << "Unhandled task state " << status.state()
                   << " on completion.";
      break;
  }

  // TODO(dhamon): Update source/reason metrics.
  terminatedTasks[taskId] = CHECK_NOTNULL(task);
}


void Executor::completeTask(const TaskID& taskId)
{
  VLOG(1) << "Completing task " << taskId;

  CHECK(terminatedTasks.contains(taskId))
    << "Failed to find terminated task " << taskId;

  Task* task = terminatedTasks[taskId];
  completedTasks.push_back(std::shared_ptr<Task>(task));
  terminatedTasks.erase(taskId);
}


void Executor::checkpointExecutor()
{
  CHECK(checkpoint);

  CHECK_NE(slave->state, slave->RECOVERING);

  // Checkpoint the executor info.
  const string path = paths::getExecutorInfoPath(
      slave->metaDir, slave->info.id(), frameworkId, id);

  VLOG(1) << "Checkpointing ExecutorInfo to '" << path << "'";
  CHECK_SOME(state::checkpoint(path, info));

  // Create the meta executor directory.
  // NOTE: This creates the 'latest' symlink in the meta directory.
  paths::createExecutorDirectory(
      slave->metaDir, slave->info.id(), frameworkId, id, containerId);
}


void Executor::checkpointTask(const TaskInfo& task)
{
  CHECK(checkpoint);

  const Task t = protobuf::createTask(task, TASK_STAGING, frameworkId);
  const string path = paths::getTaskInfoPath(
      slave->metaDir,
      slave->info.id(),
      frameworkId,
      id,
      containerId,
      t.task_id());

  VLOG(1) << "Checkpointing TaskInfo to '" << path << "'";
  CHECK_SOME(state::checkpoint(path, t));
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
      terminateTask(state.id, update.status());

      CHECK(update.has_uuid())
        << "Expecting updates without 'uuid' to have been rejected";

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


bool Executor::isCommandExecutor() const
{
  return commandExecutor;
}


std::ostream& operator<<(std::ostream& stream, Slave::State state)
{
  switch (state) {
    case Slave::RECOVERING:   return stream << "RECOVERING";
    case Slave::DISCONNECTED: return stream << "DISCONNECTED";
    case Slave::RUNNING:      return stream << "RUNNING";
    case Slave::TERMINATING:  return stream << "TERMINATING";
    default:                  return stream << "UNKNOWN";
  }
}


std::ostream& operator<<(std::ostream& stream, Framework::State state)
{
  switch (state) {
    case Framework::RUNNING:     return stream << "RUNNING";
    case Framework::TERMINATING: return stream << "TERMINATING";
    default:                     return stream << "UNKNOWN";
  }
}


std::ostream& operator<<(std::ostream& stream, Executor::State state)
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
