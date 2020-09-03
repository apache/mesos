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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/authorizer/authorizer.hpp>

#ifndef __WINDOWS__
#include <mesos/log/log.hpp>
#endif // __WINDOWS__

#include <mesos/allocator/allocator.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include <mesos/state/in_memory.hpp>
#include <mesos/state/log.hpp>
#include <mesos/state/protobuf.hpp>
#include <mesos/state/storage.hpp>

#include <mesos/zookeeper/url.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/limiter.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <process/ssl/flags.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/permissions.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif

#ifdef USE_SSL_SOCKET
#include "authentication/executor/jwt_secret_generator.hpp"
#endif // USE_SSL_SOCKET

#include "authorizer/local/authorizer.hpp"

#include "common/authorization.hpp"

#ifndef __WINDOWS__
#include "common/domain_sockets.hpp"
#endif // __WINDOWS__

#include "common/future_tracker.hpp"
#include "common/http.hpp"

#include "files/files.hpp"

#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/registrar.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "master/contender/standalone.hpp"
#include "master/contender/zookeeper.hpp"

#include "master/detector/standalone.hpp"
#include "master/detector/zookeeper.hpp"

#include "slave/csi_server.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/slave.hpp"
#include "slave/task_status_update_manager.hpp"

#include "slave/containerizer/composing.hpp"
#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/volume_gid_manager/volume_gid_manager.hpp"

#include "tests/cluster.hpp"
#include "tests/mock_registrar.hpp"

using std::string;
using std::vector;

#ifdef USE_SSL_SOCKET
using mesos::authentication::executor::JWTSecretGenerator;
#endif // USE_SSL_SOCKET

using mesos::master::contender::StandaloneMasterContender;
using mesos::master::contender::ZooKeeperMasterContender;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;
using mesos::master::detector::ZooKeeperMasterDetector;

using mesos::slave::ContainerTermination;

using net::IP;

using process::Owned;

#ifndef __WINDOWS__
using process::network::unix::Socket;
#endif // __WINDOWS__

namespace mesos {
namespace internal {
namespace tests {
namespace cluster {

Try<process::Owned<Master>> Master::start(
    const master::Flags& flags,
    const Option<zookeeper::URL>& zookeeperUrl,
    const Option<mesos::allocator::Allocator*>& allocator,
    const Option<Authorizer*>& authorizer,
    const Option<std::shared_ptr<process::RateLimiter>>& slaveRemovalLimiter)
{
  process::Owned<Master> master(new Master());
  master->zookeeperUrl = zookeeperUrl;

  // If the allocator is not provided, create a default one.
  if (allocator.isNone()) {
    Try<mesos::allocator::Allocator*> _allocator =
      master::allocator::HierarchicalDRFAllocator::create();

    if (_allocator.isError()) {
      return Error(
          "Failed to create an instance of HierarchicalDRFAllocator: " +
          _allocator.error());
    }

    master->allocator.reset(_allocator.get());
  }

  // If the authorizer is not provided, create a default one.
  if (authorizer.isNone()) {
    // Indicates whether or not the caller explicitly specified the
    // authorization configuration for this master.
    bool authorizationSpecified = true;

    vector<string> authorizerNames = strings::split(flags.authorizers, ",");

    if (authorizerNames.empty()) {
      return Error("No authorizer specified");
    }

    if (authorizerNames.size() > 1) {
      return Error("Multiple authorizers not supported");
    }

    string authorizerName = authorizerNames[0];

    Result<Authorizer*> authorizer((None()));
    if (authorizerName != master::DEFAULT_AUTHORIZER) {
      LOG(INFO) << "Creating '" << authorizerName << "' authorizer";

      authorizer = Authorizer::create(authorizerName);
    } else {
      // `authorizerName` is `DEFAULT_AUTHORIZER` at this point.
      if (flags.acls.isSome()) {
        LOG(INFO) << "Creating default '" << authorizerName << "' authorizer";

        authorizer = Authorizer::create(flags.acls.get());
        CHECK_SOME(authorizer);
      } else {
        authorizationSpecified = false;
      }
    }

    if (authorizer.isError()) {
      return Error(
          "Could not create '" + authorizerName + "' authorizer: " +
          authorizer.error());
    } else if (authorizer.isSome()) {
      master->authorizer.reset(authorizer.get());

      if (authorizationSpecified) {
        // Authorization config was explicitly provided, so set authorization
        // callbacks in libprocess.
        master->setAuthorizationCallbacks(authorizer.get());
      }
    }
  } else {
    // An authorizer was provided, so set authorization callbacks in libprocess.
    master->setAuthorizationCallbacks(authorizer.get());
  }

  // Create the appropriate master contender/detector.
  if (zookeeperUrl.isSome()) {
    master->contender.reset(new ZooKeeperMasterContender(zookeeperUrl.get()));
    master->detector.reset(new ZooKeeperMasterDetector(zookeeperUrl.get()));
  } else {
    master->contender.reset(new StandaloneMasterContender());
    master->detector.reset(new StandaloneMasterDetector());
  }

  // Check for some invalid flag combinations.
  if (flags.registry_strict) {
    return Error("Support for '--registry_strict' has been removed");
  }

  if (flags.registry == "replicated_log" && flags.work_dir.isNone()) {
    return Error(
        "Need to specify --work_dir for replicated log based registry");
  }

  if (flags.registry == "replicated_log" &&
      zookeeperUrl.isSome() &&
      flags.quorum.isNone()) {
    return Error(
        "Need to specify --quorum for replicated log based registry"
        " when using ZooKeeper");
  }

  // Create the replicated-log-based registry, if specified in the flags.
  if (flags.registry == "replicated_log") {
#ifndef __WINDOWS__
    if (zookeeperUrl.isSome()) {
      // Use ZooKeeper-based replicated log.
      master->log.reset(new mesos::log::Log(
          flags.quorum.get(),
          path::join(flags.work_dir.get(), "replicated_log"),
          zookeeperUrl->servers,
          flags.zk_session_timeout,
          path::join(zookeeperUrl->path, "log_replicas"),
          zookeeperUrl->authentication,
          flags.log_auto_initialize));
    } else {
      master->log.reset(new mesos::log::Log(
          1,
          path::join(flags.work_dir.get(), "replicated_log"),
          std::set<process::UPID>(),
          flags.log_auto_initialize));
    }
#else
    return Error("Windows does not support replicated log");
#endif // __WINDOWS__
  }

  // Create the registry's storage backend.
  if (flags.registry == "in_memory") {
    master->storage.reset(new mesos::state::InMemoryStorage());
  } else if (flags.registry == "replicated_log") {
#ifndef __WINDOWS__
    master->storage.reset(new mesos::state::LogStorage(master->log.get()));
#else
    return Error("Windows does not support replicated log");
#endif // __WINDOWS__

  } else {
    return Error(
        "Unsupported option for registry persistence: " + flags.registry);
  }

  // Instantiate some other master dependencies.
  master->state.reset(new mesos::state::State(master->storage.get()));
  master->registrar.reset(new MockRegistrar(
      flags, master->state.get(), master::READONLY_HTTP_AUTHENTICATION_REALM));

  if (slaveRemovalLimiter.isNone() && flags.agent_removal_rate_limit.isSome()) {
    // Parse the flag value.
    // TODO(vinod): Move this parsing logic to flags once we have a
    // 'Rate' abstraction in stout.
    vector<string> tokens =
      strings::tokenize(flags.agent_removal_rate_limit.get(), "/");

    if (tokens.size() != 2) {
      return Error(
          "Invalid agent_removal_rate_limit: " +
          flags.agent_removal_rate_limit.get() +
          ". Format is <Number of agents>/<Duration>");
    }

    Try<int> permits = numify<int>(tokens[0]);
    if (permits.isError()) {
      return Error(
          "Invalid agent_removal_rate_limit: " +
          flags.agent_removal_rate_limit.get() +
          ". Format is <Number of agents>/<Duration>: " + permits.error());
    }

    Try<Duration> duration = Duration::parse(tokens[1]);
    if (duration.isError()) {
      return Error(
          "Invalid agent_removal_rate_limit: " +
          flags.agent_removal_rate_limit.get() +
          ". Format is <Number of agents>/<Duration>: " + duration.error());
    }

    master->slaveRemovalLimiter =
      std::make_shared<process::RateLimiter>(permits.get(), duration.get());
  }

  // Inject all the dependencies.
  master->master.reset(new master::Master(
      allocator.getOrElse(master->allocator.get()),
      master->registrar.get(),
      &master->files,
      master->contender.get(),
      master->detector.get(),
      authorizer.getOrElse(master->authorizer.get()),
      slaveRemovalLimiter.isSome() ? slaveRemovalLimiter
                                   : master->slaveRemovalLimiter,
      flags));

  // If we are using the `StandaloneMasterDetector`, appoint the only
  // master as the leading master.
  if (zookeeperUrl.isNone()) {
    StandaloneMasterDetector* _detector = CHECK_NOTNULL(
        dynamic_cast<StandaloneMasterDetector*>(master->detector.get()));

    _detector->appoint(master->master->info());
  }

  process::Future<Nothing> _recover =
    FUTURE_DISPATCH(master->master->self(), &master::Master::_recover);

  master->pid = process::spawn(master->master.get());

  // Speed up the tests by ensuring that the Master is recovered
  // before the test proceeds. Otherwise, authentication and
  // registration messages may be dropped, causing delayed retries.
  // NOTE: We use process::internal::await() to avoid awaiting a
  // Future forever when the Clock is paused.
  if (!process::internal::await(
          _recover,
          flags.registry_fetch_timeout + flags.registry_store_timeout)) {
    return Error("Failed to wait for Master::_recover");
  }

  bool paused = process::Clock::paused();

  // Need to settle the Clock to ensure that the Master finishes
  // executing _recover() before we return.
  process::Clock::pause();
  process::Clock::settle();

  // Return the Clock to its original state.
  if (!paused) {
    process::Clock::resume();
  }

  return master;
}


Master::~Master()
{
  // Remove any libprocess authorization callbacks that were installed.
  if (authorizationCallbacksSet) {
    process::http::authorization::unsetCallbacks();
  }

  // NOTE: Authenticators' lifetimes are tied to libprocess's lifetime.
  // This means that multiple masters in tests are not supported.
  process::http::authentication::unsetAuthenticator(
      master::READONLY_HTTP_AUTHENTICATION_REALM);
  process::http::authentication::unsetAuthenticator(
      master::READWRITE_HTTP_AUTHENTICATION_REALM);

  process::terminate(pid);
  process::wait(pid);
}


process::Owned<MasterDetector> Master::createDetector()
{
  if (zookeeperUrl.isSome()) {
    return process::Owned<MasterDetector>(
        new ZooKeeperMasterDetector(zookeeperUrl.get()));
  }

  return process::Owned<MasterDetector>(new StandaloneMasterDetector(pid));
}


MasterInfo Master::getMasterInfo()
{
  return master->info();
}


void Master::setAuthorizationCallbacks(Authorizer* authorizer)
{
  process::http::authorization::setCallbacks(
      authorization::createAuthorizationCallbacks(authorizer));

  authorizationCallbacksSet = true;
}


Try<process::Owned<Slave>> Slave::create(
    MasterDetector* detector,
    const slave::Flags& flags,
    const Option<string>& id,
    const Option<slave::Containerizer*>& containerizer,
    const Option<slave::GarbageCollector*>& gc,
    const Option<slave::TaskStatusUpdateManager*>& taskStatusUpdateManager,
    const Option<mesos::slave::ResourceEstimator*>& resourceEstimator,
    const Option<mesos::slave::QoSController*>& qosController,
    const Option<mesos::SecretGenerator*>& secretGenerator,
    const Option<Authorizer*>& providedAuthorizer,
    const Option<PendingFutureTracker*>& futureTracker,
    const Option<Owned<slave::CSIServer>>& csiServer,
    bool mock)
{
  process::Owned<Slave> slave(new Slave());

  // TODO(benh): Create a work directory if using the default.

  slave->flags = flags;
  slave->detector = detector;

  // If the garbage collector is not provided, create a default one.
  if (gc.isNone()) {
    slave->gc.reset(new slave::GarbageCollector(flags.work_dir));
  }

  // If the flag `--volume_gid_range` is specified, create a volume gid manager.
  slave::VolumeGidManager* volumeGidManager = nullptr;

#ifndef __WINDOWS__
  if (flags.volume_gid_range.isSome()) {
    Try<slave::VolumeGidManager*> _volumeGidManager =
      slave::VolumeGidManager::create(flags);

    if (_volumeGidManager.isError()) {
      return Error(
          "Failed to create volume gid manager: " + _volumeGidManager.error());
    }

    volumeGidManager = _volumeGidManager.get();
  }
#endif // __WINDOWS__

  // If the future tracker is not provided, create a default one.
  if (futureTracker.isNone()) {
    Try<PendingFutureTracker*> _futureTracker = PendingFutureTracker::create();
    if (_futureTracker.isError()) {
      return Error(
          "Failed to create pending future tracker: " + _futureTracker.error());
    }

    slave->futureTracker.reset(_futureTracker.get());
  }

  // If the secret generator is not provided, create a default one.
  if (secretGenerator.isNone()) {
    SecretGenerator* _secretGenerator = nullptr;

#ifdef USE_SSL_SOCKET
    if (flags.jwt_secret_key.isSome()) {
      Try<string> jwtSecretKey = os::read(flags.jwt_secret_key.get());
      if (jwtSecretKey.isError()) {
        return Error("Failed to read the file specified by --jwt_secret_key");
      }

      // TODO(greggomann): Factor the following code out into a common helper,
      // since we also do this when loading credentials.
      Try<os::Permissions> permissions =
        os::permissions(flags.jwt_secret_key.get());
      if (permissions.isError()) {
        LOG(WARNING) << "Failed to stat jwt secret key file '"
                     << flags.jwt_secret_key.get()
                     << "': " << permissions.error();
      } else if (permissions->others.rwx) {
        LOG(WARNING) << "Permissions on executor secret key file '"
                     << flags.jwt_secret_key.get()
                     << "' are too open; it is recommended that your"
                     << " key file is NOT accessible by others";
      }

      _secretGenerator = new JWTSecretGenerator(jwtSecretKey.get());
    }
#endif // USE_SSL_SOCKET

    slave->secretGenerator.reset(_secretGenerator);
  }

  // Create a SecretResolver for use with the CSI server below.
  Try<SecretResolver*> secretResolver =
    mesos::SecretResolver::create(flags.secret_resolver);

  if (secretResolver.isError()) {
    return Error(
        "Failed to initialize secret resolver: " +
        secretResolver.error());
  }

  const string processId =
    id.isSome() ? id.get() : process::ID::generate("slave");

  if (csiServer.isNone() && flags.csi_plugin_config_dir.isSome()) {
    // Initialize the CSI server, which manages any configured CSI plugins.
    string scheme = "http";

#ifdef USE_SSL_SOCKET
    if (process::network::openssl::flags().enabled) {
      scheme = "https";
    }
#endif

    const process::http::URL agentUrl(
        scheme,
        process::address().ip,
        process::address().port,
        processId + "/api/v1");

    Try<Owned<slave::CSIServer>> _csiServer = slave::CSIServer::create(
        flags,
        agentUrl,
        secretGenerator.getOrElse(slave->secretGenerator.get()),
        secretResolver.get());

    if (_csiServer.isError()) {
      return Error(
          "Failed to initialize the CSI server: " + _csiServer.error());
    }

    slave->csiServer = std::move(_csiServer.get());
  }

  // If the containerizer is not provided, create a default one.
  if (containerizer.isSome()) {
    slave->containerizer = containerizer.get();
  } else {
    // Create a new fetcher.
    slave->fetcher.reset(new slave::Fetcher(flags));

    Try<slave::Containerizer*> _containerizer =
      slave::Containerizer::create(
          flags,
          true,
          slave->fetcher.get(),
          gc.getOrElse(slave->gc.get()),
          nullptr,
          volumeGidManager,
          futureTracker.getOrElse(slave->futureTracker.get()),
          (csiServer.getOrElse(slave->csiServer)).get());

    if (_containerizer.isError()) {
      return Error("Failed to create containerizer: " + _containerizer.error());
    }

    slave->containerizer = _containerizer.get();
  }

  // As composing containerizer doesn't affect behaviour of underlying
  // containerizers, we can always use composing containerizer turned on
  // by default in tests.
  if (!dynamic_cast<slave::ComposingContainerizer*>(slave->containerizer)) {
    Try<slave::ComposingContainerizer*> composing =
      slave::ComposingContainerizer::create({slave->containerizer});

    if (composing.isError()) {
      return Error(
          "Failed to create composing containerizer: " + composing.error());
    }

    slave->containerizer = composing.get();
  }

  if (containerizer.isNone()) {
    slave->ownedContainerizer.reset(slave->containerizer);
  }

  Option<Authorizer*> authorizer = providedAuthorizer;

  // If the authorizer is not provided, create a default one.
  if (providedAuthorizer.isNone()) {
    // Indicates whether or not the caller explicitly specified the
    // authorization configuration for this agent.
    bool authorizationSpecified = true;

    string authorizerName = flags.authorizer;

    Result<Authorizer*> createdAuthorizer((None()));
    if (authorizerName != slave::DEFAULT_AUTHORIZER) {
      LOG(INFO) << "Creating '" << authorizerName << "' authorizer";

      // NOTE: The contents of --acls will be ignored.
      createdAuthorizer = Authorizer::create(authorizerName);
    } else {
      // `authorizerName` is `DEFAULT_AUTHORIZER` at this point.
      if (flags.acls.isSome()) {
        LOG(INFO) << "Creating default '" << authorizerName << "' authorizer";

        createdAuthorizer = Authorizer::create(flags.acls.get());
      } else {
        // Neither a non-default authorizer nor a set of ACLs were specified.
        authorizationSpecified = false;
      }
    }

    if (createdAuthorizer.isError()) {
      EXIT(EXIT_FAILURE) << "Could not create '" << authorizerName
                         << "' authorizer: " << createdAuthorizer.error();
    } else if (createdAuthorizer.isSome()) {
      slave->authorizer.reset(createdAuthorizer.get());
      authorizer = createdAuthorizer.get();

      if (authorizationSpecified) {
        // Authorization config was explicitly provided, so set authorization
        // callbacks in libprocess.
        slave->setAuthorizationCallbacks(authorizer.get());
      }
    }
  } else {
    // An authorizer was provided, so set authorization callbacks in libprocess.
    slave->setAuthorizationCallbacks(providedAuthorizer.get());
  }

  // If the resource estimator is not provided, create a default one.
  if (resourceEstimator.isNone()) {
    Try<mesos::slave::ResourceEstimator*> _resourceEstimator =
      mesos::slave::ResourceEstimator::create(flags.resource_estimator);

    if (_resourceEstimator.isError()) {
      return Error(
          "Failed to create resource estimator: " + _resourceEstimator.error());
    }

    slave->resourceEstimator.reset(_resourceEstimator.get());
  }

  // If the QoS controller is not provided, create a default one.
  if (qosController.isNone()) {
    Try<mesos::slave::QoSController*> _qosController =
      mesos::slave::QoSController::create(flags.qos_controller);

    if (_qosController.isError()) {
      return Error(
          "Failed to create QoS controller: " + _qosController.error());
    }

    slave->qosController.reset(_qosController.get());
  }

#ifndef __WINDOWS__
  Option<Socket> executorSocket = None();
  if (flags.http_executor_domain_sockets) {
    // If `http_executor_domain_sockets` is true, then the location should have
    // been set by the user or automatically during startup.
    CHECK_SOME(flags.domain_socket_location);

    LOG(INFO) << "Creating domain socket at " << *flags.domain_socket_location;
    Try<Socket> socket =
      common::createDomainSocket(*flags.domain_socket_location);

    if (socket.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to create domain socket: " << socket.error();
    }

    executorSocket = socket.get();
  }
#endif // __WINDOWS__

  // If the task status update manager is not provided, create a default one.
  if (taskStatusUpdateManager.isNone()) {
    slave->taskStatusUpdateManager.reset(
        new slave::TaskStatusUpdateManager(flags));
  }

  // Inject all the dependencies.
  if (mock) {
    slave->slave.reset(new MockSlave(
        processId,
        flags,
        detector,
        slave->containerizer,
        &slave->files,
        gc.getOrElse(slave->gc.get()),
        taskStatusUpdateManager.getOrElse(slave->taskStatusUpdateManager.get()),
        resourceEstimator.getOrElse(slave->resourceEstimator.get()),
        qosController.getOrElse(slave->qosController.get()),
        secretGenerator.getOrElse(slave->secretGenerator.get()),
        volumeGidManager,
        futureTracker.getOrElse(slave->futureTracker.get()),
        csiServer.getOrElse(slave->csiServer),
        authorizer));
  } else {
    slave->slave.reset(new slave::Slave(
        processId,
        flags,
        detector,
        slave->containerizer,
        &slave->files,
        gc.getOrElse(slave->gc.get()),
        taskStatusUpdateManager.getOrElse(slave->taskStatusUpdateManager.get()),
        resourceEstimator.getOrElse(slave->resourceEstimator.get()),
        qosController.getOrElse(slave->qosController.get()),
        secretGenerator.getOrElse(slave->secretGenerator.get()),
        volumeGidManager,
        futureTracker.getOrElse(slave->futureTracker.get()),
        csiServer.getOrElse(slave->csiServer),
#ifndef __WINDOWS__
        executorSocket,
#endif // __WINDOWS__
        authorizer));
  }

  return slave;
}


Slave::~Slave()
{
  // Remove any libprocess authorization callbacks that were installed.
  if (authorizationCallbacksSet) {
    process::http::authorization::unsetCallbacks();
  }

  process::http::authentication::unsetAuthenticator(
      slave::READONLY_HTTP_AUTHENTICATION_REALM);
  process::http::authentication::unsetAuthenticator(
      slave::READWRITE_HTTP_AUTHENTICATION_REALM);
  process::http::authentication::unsetAuthenticator(
      slave::EXECUTOR_HTTP_AUTHENTICATION_REALM);
  process::http::authentication::unsetAuthenticator(
      slave::RESOURCE_PROVIDER_HTTP_AUTHENTICATION_REALM);

  // If either `shutdown()` or `terminate()` were called already,
  // skip the below container cleanup logic.  Additionally, we can skip
  // termination, as the shutdown/terminate will do this too.
  if (!cleanUpContainersInDestructor) {
    return;
  }

  // Startup didn't complete so don't try to do the full shutdown.
  if (!containerizer) {
    return;
  }

  // We should wait until agent recovery completes to prevent a potential race
  // between containerizer recovery process and the following code that invokes
  // methods of the containerizer, e.g. a test can start an agent that in turn
  // triggers containerizer recovery of orphaned containers, then immediately
  // destroys the agent. Thus, the containerizer might return a different set of
  // containers, depending on whether containerizer recovery has been finished.
  //
  // NOTE: This wait is omitted if a pointer to a containerizer object was
  // passed to the slave's constructor, as it might be a mock containerizer,
  // thereby agent recovery will never be finished.
  if (ownedContainerizer.get() != nullptr) {
    slave->recoveryInfo.recovered.future().await();
  }

  terminate();

  // This extra closure is necessary in order to use `AWAIT` and `ASSERT_*`,
  // as these macros require a void return type.
  [this]() {
    // Destroy the existing containers on the slave. Note that some
    // containers may terminate while we are doing this, so we ignore
    // any 'wait' failures and ensure that there are no containers
    // when we're done destroying.
    process::Future<hashset<ContainerID>> containers =
      containerizer->containers();

    AWAIT_READY(containers);

    // Because the `cgroups::destroy()` code path makes use of `delay()`, the
    // clock must not be paused in order to reliably destroy all remaining
    // containers. If necessary, we resume the clock here and then pause it
    // again when we're done destroying containers.
    bool paused = process::Clock::paused();

    if (paused) {
      process::Clock::resume();
    }

    foreach (const ContainerID& containerId, containers.get()) {
      process::Future<Option<ContainerTermination>> termination =
        containerizer->destroy(containerId);

      AWAIT(termination);

      if (!termination.isReady()) {
        LOG(ERROR) << "Failed to destroy container " << containerId << ": "
                   << (termination.isFailed() ?
                       termination.failure() :
                       "discarded");
      }
    }

    // When using the composing containerizer, the assertion checking
    // `containers->empty()` below is racy, since the containers are
    // not yet removed from that containerizer's internal data structures
    // when the future becomes ready. Instead, an internal function to clean
    // up these internal data structures is dispatched when the future
    // becomes ready.
    // To work around this, we wait for the clock to settle here. This can
    // be removed once MESOS-9413 is resolved.
    process::Clock::pause();
    process::Clock::settle();

    if (!paused) {
      process::Clock::resume();
    }

    containers = containerizer->containers();
    AWAIT_READY(containers);

    ASSERT_TRUE(containers->empty())
      << "Failed to destroy containers: " << stringify(containers.get());
  }();
}


void Slave::setAuthorizationCallbacks(Authorizer* authorizer)
{
  process::http::authorization::setCallbacks(
      authorization::createAuthorizationCallbacks(authorizer));

  authorizationCallbacksSet = true;
}


void Slave::shutdown()
{
  cleanUpContainersInDestructor = false;

  bool paused = process::Clock::paused();

  if (paused) {
    process::Clock::resume();
  }

  process::dispatch(slave.get(), &slave::Slave::shutdown, process::UPID(), "");
  wait();

  if (paused) {
    process::Clock::pause();
  }
}


void Slave::terminate()
{
  cleanUpContainersInDestructor = false;

  process::terminate(pid);
  wait();
}


MockSlave* Slave::mock()
{
  return dynamic_cast<MockSlave*>(slave.get());
}


void Slave::start()
{
  pid = process::spawn(slave.get());
}


void Slave::wait()
{
  process::wait(pid);
}

} // namespace cluster {
} // namespace tests {
} // namespace internal {
} // namespace mesos {
