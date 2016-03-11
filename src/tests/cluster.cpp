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

#include <string>
#include <vector>

#include "tests/cluster.hpp"

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/id.hpp>
#include <process/limiter.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "master/allocator/mesos/hierarchical.hpp"

namespace mesos {
namespace internal {
namespace tests {

using std::string;
using std::vector;

Cluster::Masters::Masters(
    Cluster* _cluster,
    const Option<zookeeper::URL>& _url)
  : cluster(_cluster),
    url(_url) {}


Cluster::Masters::~Masters()
{
  shutdown();
}


void Cluster::Masters::shutdown()
{
  // TODO(benh): Use utils::copy from stout once namespaced.
  std::map<process::PID<master::Master>, Master> copy(masters);
  foreachkey (const process::PID<master::Master>& pid, copy) {
    stop(pid);
  }
  masters.clear();
}


Try<process::PID<master::Master>> Cluster::Masters::start(
    const master::Flags& flags,
    const Option<mesos::master::allocator::Allocator*>& allocator,
    const Option<Authorizer*>& authorizer,
    const Option<std::shared_ptr<process::RateLimiter>>& slaveRemovalLimiter)
{
  // Disallow multiple masters when not using ZooKeeper.
  if (!masters.empty() && url.isNone()) {
    return Error("Can not start multiple masters when not using ZooKeeper");
  }

  Master master;

  if (allocator.isSome()) {
    master.allocator = allocator.get();
  } else {
    // If allocator is not provided, fall back to the default one,
    // managed by Cluster::Masters.
    Try<mesos::master::allocator::Allocator*> allocator_ =
      master::allocator::HierarchicalDRFAllocator::create();
    if (allocator_.isError()) {
      return Error(
          "Failed to create an instance of HierarchicalDRFAllocator: " +
          allocator_.error());
    }

    master.allocator = allocator_.get();
    master.createdAllocator = true;
  }

  if (flags.registry == "in_memory") {
    if (flags.registry_strict) {
      return Error(
          "Cannot use '--registry_strict' when using in-memory storage based"
          " registry");
    }
    master.storage.reset(new state::InMemoryStorage());
  } else if (flags.registry == "replicated_log") {
    if (flags.work_dir.isNone()) {
      return Error(
          "Need to specify --work_dir for replicated log based registry");
    }

    if (url.isSome()) {
      // Use ZooKeeper based replicated log.
      if (flags.quorum.isNone()) {
        return Error(
            "Need to specify --quorum for replicated log based registry"
            " when using ZooKeeper");
      }
      master.log.reset(new log::Log(
          flags.quorum.get(),
          path::join(flags.work_dir.get(), "replicated_log"),
          url.get().servers,
          flags.zk_session_timeout,
          path::join(url.get().path, "log_replicas"),
          url.get().authentication,
          flags.log_auto_initialize));
    } else {
      master.log.reset(new log::Log(
          1,
          path::join(flags.work_dir.get(), "replicated_log"),
          std::set<process::UPID>(),
          flags.log_auto_initialize));
    }

    master.storage.reset(new state::LogStorage(master.log.get()));
  } else {
    return Error("'" + flags.registry + "' is not a supported option for"
                 " registry persistence");
  }

  CHECK_NOTNULL(master.storage.get());

  master.state.reset(new state::protobuf::State(master.storage.get()));
  master.registrar.reset(new master::Registrar(flags, master.state.get()));
  master.repairer.reset(new master::Repairer());

  if (url.isSome()) {
    master.contender.reset(new ZooKeeperMasterContender(url.get()));
    master.detector.reset(new ZooKeeperMasterDetector(url.get()));
  } else {
    master.contender.reset(new StandaloneMasterContender());
    master.detector.reset(new StandaloneMasterDetector());
  }

  if (authorizer.isSome()) {
    CHECK_NOTNULL(authorizer.get());
  } else {
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
      }
    }

    if (authorizer.isError()) {
      return Error("Could not create '" + authorizerName +
                   "' authorizer: " + authorizer.error());
    } else if (authorizer.isSome()) {
      master.authorizer.reset(authorizer.get());
    }
  }

  if (slaveRemovalLimiter.isNone() &&
      flags.slave_removal_rate_limit.isSome()) {
    // Parse the flag value.
    // TODO(vinod): Move this parsing logic to flags once we have a
    // 'Rate' abstraction in stout.
    std::vector<std::string> tokens =
      strings::tokenize(flags.slave_removal_rate_limit.get(), "/");

    if (tokens.size() != 2) {
      EXIT(1) << "Invalid slave_removal_rate_limit: "
              << flags.slave_removal_rate_limit.get()
              << ". Format is <Number of slaves>/<Duration>";
    }

    Try<int> permits = numify<int>(tokens[0]);
    if (permits.isError()) {
      EXIT(1) << "Invalid slave_removal_rate_limit: "
              << flags.slave_removal_rate_limit.get()
              << ". Format is <Number of slaves>/<Duration>"
              << ": " << permits.error();
    }

    Try<Duration> duration = Duration::parse(tokens[1]);
    if (duration.isError()) {
      EXIT(1) << "Invalid slave_removal_rate_limit: "
              << flags.slave_removal_rate_limit.get()
              << ". Format is <Number of slaves>/<Duration>"
              << ": " << duration.error();
    }

    master.slaveRemovalLimiter = std::shared_ptr<process::RateLimiter>(
        new process::RateLimiter(permits.get(), duration.get()));
  }

  master.master = new master::Master(
      master.allocator,
      master.registrar.get(),
      master.repairer.get(),
      &cluster->files,
      master.contender.get(),
      master.detector.get(),
      authorizer.isSome()
          ? authorizer : master.authorizer.get(),
      slaveRemovalLimiter.isSome()
          ? slaveRemovalLimiter : master.slaveRemovalLimiter,
      flags);

  if (url.isNone()) {
    // This means we are using the StandaloneMasterDetector.
    StandaloneMasterDetector* detector_ = CHECK_NOTNULL(
        dynamic_cast<StandaloneMasterDetector*>(master.detector.get()));

    detector_->appoint(master.master->info());
  }

  process::Future<Nothing> _recover =
    FUTURE_DISPATCH(master.master->self(), &master::Master::_recover);

  process::PID<master::Master> pid = process::spawn(master.master);

  masters[pid] = master;

  // Speed up the tests by ensuring that the Master is recovered
  // before the test proceeds. Otherwise, authentication and
  // registration messages may be dropped, causing delayed retries.
  // NOTE: We use process::internal::await() to avoid awaiting a
  // Future forever when the Clock is paused.
  if (!process::internal::await(
          _recover,
          flags.registry_fetch_timeout + flags.registry_store_timeout)) {
    LOG(FATAL) << "Failed to wait for _recover";
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

  return pid;
}


Try<Nothing> Cluster::Masters::stop(
    const process::PID<master::Master>& pid)
{
  if (masters.count(pid) == 0) {
    return Error("No master found to stop");
  }

  Master master = masters[pid];

  process::terminate(master.master);
  process::wait(master.master);
  delete master.master;

  if (master.createdAllocator) {
    delete master.allocator;
  }

  masters.erase(pid);

  return Nothing();
}


process::Owned<MasterDetector> Cluster::Masters::detector()
{
  if (url.isSome()) {
    return process::Owned<MasterDetector>(
        new ZooKeeperMasterDetector(url.get()));
  }

  CHECK(masters.size() == 1);

  return process::Owned<MasterDetector>(
      new StandaloneMasterDetector(masters.begin()->first));
}


Cluster::Slaves::Slaves(Cluster* _cluster, Masters* _masters)
  : cluster(_cluster), masters(_masters) {}


Cluster::Slaves::~Slaves()
{
  shutdown();
}


void Cluster::Slaves::shutdown()
{
  // TODO(benh): Use utils::copy from stout once namespaced.
  std::map<process::PID<slave::Slave>, Slave> copy(slaves);
  foreachpair (const process::PID<slave::Slave>& pid,
               const Slave& slave,
               copy) {
    // Destroy the existing containers on the slave. Note that some
    // containers may terminate while we are doing this, so we ignore
    // any 'wait' failures and ensure that there are no containers
    // when we're done destroying.
    process::Future<hashset<ContainerID>> containers =
      slave.containerizer->containers();
    AWAIT_READY(containers);

    foreach (const ContainerID& containerId, containers.get()) {
      process::Future<containerizer::Termination> wait =
        slave.containerizer->wait(containerId);

      slave.containerizer->destroy(containerId);

      AWAIT(wait);
    }

    containers = slave.containerizer->containers();
    AWAIT_READY(containers);

    ASSERT_TRUE(containers.get().empty())
      << "Failed to destroy containers: " << stringify(containers.get());

    stop(pid);
  }
  slaves.clear();
}


Try<process::PID<slave::Slave>> Cluster::Slaves::start(
    const slave::Flags& flags,
    const Option<std::string>& id,
    const Option<slave::Containerizer*>& containerizer,
    const Option<MasterDetector*>& detector,
    const Option<slave::GarbageCollector*>& gc,
    const Option<slave::StatusUpdateManager*>& statusUpdateManager,
    const Option<mesos::slave::ResourceEstimator*>& resourceEstimator,
    const Option<mesos::slave::QoSController*>& qosController)
{
  // TODO(benh): Create a work directory if using the default.

  Slave slave;

  if (containerizer.isSome()) {
    slave.containerizer = containerizer.get();
  } else {
    // Create a new fetcher.
    slave.fetcher.reset(new slave::Fetcher());

    Try<slave::Containerizer*> containerizer =
      slave::Containerizer::create(flags, true, slave.fetcher.get());

    CHECK_SOME(containerizer);

    slave.containerizer = containerizer.get();
    slave.createdContainerizer = true;
  }

  if (resourceEstimator.isNone()) {
    Try<mesos::slave::ResourceEstimator*> _resourceEstimator =
      mesos::slave::ResourceEstimator::create(flags.resource_estimator);

    CHECK_SOME(_resourceEstimator);
    slave.resourceEstimator.reset(_resourceEstimator.get());
  }

  if (qosController.isNone()) {
    Try<mesos::slave::QoSController*> _qosController =
      mesos::slave::QoSController::create(flags.qos_controller);

    CHECK_SOME(_qosController);
    slave.qosController.reset(_qosController.get());
  }

  // Get a detector for the master(s) if one wasn't provided.
  if (detector.isNone()) {
    slave.detector = masters->detector();
  }

  // Create a garbage collector if one wasn't provided.
  if (gc.isNone()) {
    slave.gc.reset(new slave::GarbageCollector());
  }

  // Create a status update manager if one wasn't provided.
  if (statusUpdateManager.isNone()) {
    slave.statusUpdateManager.reset(new slave::StatusUpdateManager(flags));
  }

  slave.flags = flags;

  slave.slave = new slave::Slave(
      id.isSome() ? id.get() : process::ID::generate("slave"),
      flags,
      detector.getOrElse(slave.detector.get()),
      slave.containerizer,
      &cluster->files,
      gc.getOrElse(slave.gc.get()),
      statusUpdateManager.getOrElse(slave.statusUpdateManager.get()),
      resourceEstimator.getOrElse(slave.resourceEstimator.get()),
      qosController.getOrElse(slave.qosController.get()));

  process::PID<slave::Slave> pid = process::spawn(slave.slave);

  slaves[pid] = slave;

  return pid;
}


Try<Nothing> Cluster::Slaves::stop(
    const process::PID<slave::Slave>& pid,
    bool shutdown)
{
  if (slaves.count(pid) == 0) {
    return Error("No slave found to stop");
  }

  Slave slave = slaves[pid];

  if (shutdown) {
    process::dispatch(slave.slave,
                      &slave::Slave::shutdown,
                      process::UPID(),
                      "");
  } else {
    process::terminate(slave.slave);
  }
  process::wait(slave.slave);
  delete slave.slave;

  if (slave.createdContainerizer) {
    delete slave.containerizer;
  }

#ifdef __linux__
  // Remove all of this processes threads into the root cgroups - this
  // simulates the slave process terminating and permits a new slave to start
  // when the --slave_subsystems flag is used.
  if (slave.flags.slave_subsystems.isSome()) {
    foreach (const std::string& subsystem,
             strings::tokenize(slave.flags.slave_subsystems.get(), ",")) {
      std::string hierarchy = path::join(
          slave.flags.cgroups_hierarchy, subsystem);

      std::string cgroup = path::join(slave.flags.cgroups_root, "slave");

      Try<bool> exists = cgroups::exists(hierarchy, cgroup);
      if (exists.isError() || !exists.get()) {
        EXIT(1) << "Failed to find cgroup " << cgroup
                << " for subsystem " << subsystem
                << " under hierarchy " << hierarchy
                << " for slave: " + exists.error();
      }

      // Move all of our threads into the root cgroup.
      Try<Nothing> assign = cgroups::assign(hierarchy, "", getpid());
      if (assign.isError()) {
        EXIT(1) << "Failed to move slave threads into cgroup " << cgroup
                << " for subsystem " << subsystem
                << " under hierarchy " << hierarchy
                << " for slave: " + assign.error();
      }
    }
  }
#endif // __linux__

  slaves.erase(pid);

  return Nothing();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
