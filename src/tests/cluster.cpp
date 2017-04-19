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

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif

#include "authorizer/local/authorizer.hpp"

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

#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/slave.hpp"
#include "slave/status_update_manager.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "tests/cluster.hpp"
#include "tests/mock_registrar.hpp"

using std::string;
using std::vector;

using mesos::master::contender::StandaloneMasterContender;
using mesos::master::contender::ZooKeeperMasterContender;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;
using mesos::master::detector::ZooKeeperMasterDetector;

using mesos::slave::ContainerTermination;

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
      mesos::createAuthorizationCallbacks(authorizer));

  authorizationCallbacksSet = true;
}


Try<process::Owned<Slave>> Slave::start(
    MasterDetector* detector,
    const slave::Flags& flags,
    const Option<string>& id,
    const Option<slave::Containerizer*>& containerizer,
    const Option<slave::GarbageCollector*>& gc,
    const Option<slave::StatusUpdateManager*>& statusUpdateManager,
    const Option<mesos::slave::ResourceEstimator*>& resourceEstimator,
    const Option<mesos::slave::QoSController*>& qosController,
    const Option<Authorizer*>& providedAuthorizer)
{
  process::Owned<Slave> slave(new Slave());

  // TODO(benh): Create a work directory if using the default.

  slave->flags = flags;
  slave->detector = detector;

  // If the containerizer is not provided, create a default one.
  if (containerizer.isSome()) {
    slave->containerizer = containerizer.get();
  } else {
    // Create a new fetcher.
    slave->fetcher.reset(new slave::Fetcher());

    Try<slave::Containerizer*> _containerizer =
      slave::Containerizer::create(flags, true, slave->fetcher.get());

    if (_containerizer.isError()) {
      return Error("Failed to create containerizer: " + _containerizer.error());
    }

    slave->ownedContainerizer.reset(_containerizer.get());
    slave->containerizer = _containerizer.get();
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

  // If the garbage collector is not provided, create a default one.
  if (gc.isNone()) {
    slave->gc.reset(new slave::GarbageCollector());
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

  // If the status update manager is not provided, create a default one.
  if (statusUpdateManager.isNone()) {
    slave->statusUpdateManager.reset(new slave::StatusUpdateManager(flags));
  }

  // Inject all the dependencies.
  slave->slave.reset(new slave::Slave(
      id.isSome() ? id.get() : process::ID::generate("slave"),
      flags,
      detector,
      slave->containerizer,
      &slave->files,
      gc.getOrElse(slave->gc.get()),
      statusUpdateManager.getOrElse(slave->statusUpdateManager.get()),
      resourceEstimator.getOrElse(slave->resourceEstimator.get()),
      qosController.getOrElse(slave->qosController.get()),
      authorizer));

  slave->pid = process::spawn(slave->slave.get());

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

    foreach (const ContainerID& containerId, containers.get()) {
      process::Future<Option<ContainerTermination>> wait =
        containerizer->wait(containerId);

      containerizer->destroy(containerId);

      AWAIT(wait);
    }

    containers = containerizer->containers();
    AWAIT_READY(containers);

    ASSERT_TRUE(containers->empty())
      << "Failed to destroy containers: " << stringify(containers.get());
  }();

  terminate();
}


void Slave::setAuthorizationCallbacks(Authorizer* authorizer)
{
  process::http::authorization::setCallbacks(
      mesos::createAuthorizationCallbacks(authorizer));

  authorizationCallbacksSet = true;
}


void Slave::shutdown()
{
  cleanUpContainersInDestructor = false;

  process::dispatch(slave.get(), &slave::Slave::shutdown, process::UPID(), "");
  wait();
}


void Slave::terminate()
{
  cleanUpContainersInDestructor = false;

  process::terminate(pid);
  wait();
}


void Slave::wait()
{
  process::wait(pid);
}

} // namespace cluster {
} // namespace tests {
} // namespace internal {
} // namespace mesos {
