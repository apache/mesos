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

#ifndef __TESTS_CLUSTER_HPP__
#define __TESTS_CLUSTER_HPP__

#include <map>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "files/files.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif // __linux__

#include "log/log.hpp"

#include "log/tool/initialize.hpp"

#include "master/allocator.hpp"
#include "master/contender.hpp"
#include "master/detector.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/registrar.hpp"
#include "master/repairer.hpp"

#include "slave/flags.hpp"
#include "slave/containerizer/containerizer.hpp"
#include "slave/slave.hpp"

#include "state/in_memory.hpp"
#include "state/log.hpp"
#include "state/protobuf.hpp"
#include "state/storage.hpp"

#include "zookeeper/url.hpp"

namespace mesos {
namespace internal {
namespace tests {

class Cluster
{
public:
  Cluster(const Option<zookeeper::URL>& url = None())
    : masters(this, url),
      slaves(this, &masters) {}

  // Abstracts the masters of a cluster.
  class Masters
  {
  public:
    Masters(Cluster* _cluster, const Option<zookeeper::URL>& _url);
    ~Masters();

    void shutdown();

    // Start and manage a new master using the specified flags.
    // This overload is shorthand to specify that you want default
    // master objects and is equivalent to passing None to all of
    // the required arguments of the other overload.
    Try<process::PID<master::Master> > start(
        const master::Flags& flags = master::Flags());

    // Start and manage a new master using the specified flags.
    // An allocator process may be specified in which case it will outlive
    // the launched master.  If no allocator process is specified then
    // the default allocator will be instantiated.
    Try<process::PID<master::Master> > start(
        const Option<master::allocator::AllocatorProcess*>& allocatorProcess,
        const master::Flags& flags = master::Flags());

    // Stops and cleans up a master at the specified PID.
    Try<Nothing> stop(const process::PID<master::Master>& pid);

    // Returns a new master detector for this instance of masters.
    process::Owned<MasterDetector> detector();

  private:
    // Not copyable, not assignable.
    Masters(const Masters&);
    Masters& operator = (const Masters&);

    Cluster* cluster; // Enclosing class.
    Option<zookeeper::URL> url;

    // Encapsulates a single master's dependencies.
    struct Master
    {
      Master()
        : master(NULL),
          allocator(NULL),
          allocatorProcess(NULL),
          log(NULL),
          storage(NULL),
          state(NULL),
          registrar(NULL),
          repairer(NULL),
          contender(NULL),
          detector(NULL) {}

      master::Master* master;
      master::allocator::Allocator* allocator;
      master::allocator::AllocatorProcess* allocatorProcess;
      log::Log* log;
      state::Storage* storage;
      state::protobuf::State* state;
      master::Registrar* registrar;
      master::Repairer* repairer;
      MasterContender* contender;
      MasterDetector* detector;
    };

    std::map<process::PID<master::Master>, Master> masters;
  };

  // Abstracts the slaves of a cluster.
  class Slaves
  {
  public:
    Slaves(Cluster* _cluster, Masters* _masters);
    ~Slaves();

    // Stop and clean up all slaves.
    void shutdown();

    // Start and manage a new slave with a process isolator using the
    // specified flags.
    Try<process::PID<slave::Slave> > start(
        const slave::Flags& flags = slave::Flags());

    // Start and manage a new slave injecting the specified isolator.
    // The isolator is expected to outlive the launched slave (i.e.,
    // until it is stopped via Slaves::stop).
    Try<process::PID<slave::Slave> > start(
        slave::Containerizer* containerizer,
        const slave::Flags& flags = slave::Flags());

    // Start and manage a new slave injecting the specified Master
    // Detector. The detector is expected to outlive the launched
    // slave (i.e., until it is stopped via Slaves::stop).
    Try<process::PID<slave::Slave> > start(
        const Option<MasterDetector*>& detector,
        const slave::Flags& flags = slave::Flags());

    Try<process::PID<slave::Slave> > start(
        slave::Containerizer* containerizer,
        const Option<MasterDetector*>& detector,
        const slave::Flags& flags = slave::Flags());

    // Stops and cleans up a slave at the specified PID. If 'shutdown'
    // is true than the slave is sent a shutdown message instead of
    // being terminated.
    Try<Nothing> stop(
        const process::PID<slave::Slave>& pid,
        bool shutdown = false);

  private:
    // Not copyable, not assignable.
    Slaves(const Slaves&);
    Slaves& operator = (const Slaves&);

    Cluster* cluster; // Enclosing class.
    Masters* masters; // Used to create MasterDetector instances.

    // Encapsulates a single slave's dependencies.
    struct Slave
    {
      Slave()
        : containerizer(NULL),
          createdContainerizer(false),
          slave(NULL),
          detector(NULL) {}

      // Register the slave's containerizer here.
      slave::Containerizer* containerizer;
      // Record if we created the containerizer so we know to delete it when
      // stopping the slave.
      bool createdContainerizer;
      slave::Slave* slave;
      process::Owned<MasterDetector> detector;

      // Set to the slave::flags used for the slave.
      slave::Flags flags;
    };

    std::map<process::PID<slave::Slave>, Slave> slaves;
  };

  // Shuts down all masters and slaves.
  void shutdown()
  {
    masters.shutdown();
    slaves.shutdown();
  }

  // Cluster wide shared abstractions.
  Files files;

  Masters masters;
  Slaves slaves;

private:
  // Not copyable, not assignable.
  Cluster(const Cluster&);
  Cluster& operator = (const Cluster&);
};


inline Cluster::Masters::Masters(
    Cluster* _cluster,
    const Option<zookeeper::URL>& _url)
  : cluster(_cluster),
    url(_url) {}


inline Cluster::Masters::~Masters()
{
  shutdown();
}


inline void Cluster::Masters::shutdown()
{
  // TODO(benh): Use utils::copy from stout once namespaced.
  std::map<process::PID<master::Master>, Master> copy(masters);
  foreachkey (const process::PID<master::Master>& pid, copy) {
    stop(pid);
  }
  masters.clear();
}


inline Try<process::PID<master::Master> > Cluster::Masters::start(
    const master::Flags& flags)
{
  return start(None(), flags);
}


inline Try<process::PID<master::Master> > Cluster::Masters::start(
    const Option<master::allocator::AllocatorProcess*>& allocatorProcess,
    const master::Flags& flags)
{
  // Disallow multiple masters when not using ZooKeeper.
  if (!masters.empty() && url.isNone()) {
    return Error("Can not start multiple masters when not using ZooKeeper");
  }

  Master master;

  if (allocatorProcess.isNone()) {
    master.allocatorProcess =
        new master::allocator::HierarchicalDRFAllocatorProcess();
    master.allocator =
        new master::allocator::Allocator(master.allocatorProcess);
  } else {
    master.allocatorProcess = NULL;
    master.allocator =
        new master::allocator::Allocator(allocatorProcess.get());
  }

  if (flags.registry == "in_memory") {
    if (flags.registry_strict) {
      return Error(
          "Cannot use '--registry_strict' when using in-memory storage based"
          " registry");
    }
    master.storage = new state::InMemoryStorage();
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
      master.log = new log::Log(
          flags.quorum.get(),
          path::join(flags.work_dir.get(), "replicated_log"),
          url.get().servers,
          flags.zk_session_timeout,
          path::join(url.get().path, "log_replicas"),
          url.get().authentication,
          flags.log_auto_initialize);
    } else {
      master.log = new log::Log(
          1,
          path::join(flags.work_dir.get(), "replicated_log"),
          std::set<process::UPID>(),
          flags.log_auto_initialize);
    }

    master.storage = new state::LogStorage(master.log);
  } else {
    return Error("'" + flags.registry + "' is not a supported option for"
                 " registry persistence");
  }

  CHECK_NOTNULL(master.storage);

  master.state = new state::protobuf::State(master.storage);
  master.registrar = new master::Registrar(flags, master.state);
  master.repairer = new master::Repairer();

  if (url.isSome()) {
    master.contender = new ZooKeeperMasterContender(url.get());
    master.detector = new ZooKeeperMasterDetector(url.get());
  } else {
    master.contender = new StandaloneMasterContender();
    master.detector = new StandaloneMasterDetector();
  }

  master.master = new master::Master(
      master.allocator,
      master.registrar,
      master.repairer,
      &cluster->files,
      master.contender,
      master.detector,
      flags);

  if (url.isNone()) {
    // This means we are using the StandaloneMasterDetector.
    CHECK_NOTNULL(dynamic_cast<StandaloneMasterDetector*>(master.detector))
        ->appoint(master.master->info());
  }

  process::Future<Nothing> _recover =
    FUTURE_DISPATCH(master.master->self(), &master::Master::_recover);

  process::PID<master::Master> pid = process::spawn(master.master);

  masters[pid] = master;

  // Speed up the tests by ensuring that the Master is recovered
  // before the test proceeds. Otherwise, authentication and
  // registration messages may be dropped, causing delayed retries.
  if (!_recover.await(Seconds(10))) {
    LOG(FATAL) << "Failed to wait for _recover";
  }

  return pid;
}


inline Try<Nothing> Cluster::Masters::stop(
    const process::PID<master::Master>& pid)
{
  if (masters.count(pid) == 0) {
    return Error("No master found to stop");
  }

  Master master = masters[pid];

  process::terminate(master.master);
  process::wait(master.master);
  delete master.master;

  delete master.allocator; // Terminates and waits for allocator process.
  delete master.allocatorProcess; // May be NULL.

  delete master.repairer;
  delete master.registrar;
  delete master.state;
  delete master.storage;
  delete master.log;

  delete master.contender;
  delete master.detector;

  masters.erase(pid);

  return Nothing();
}


inline process::Owned<MasterDetector> Cluster::Masters::detector()
{
  if (url.isSome()) {
    return process::Owned<MasterDetector>(
        new ZooKeeperMasterDetector(url.get()));
  }

  CHECK(masters.size() == 1);

  return process::Owned<MasterDetector>(
      new StandaloneMasterDetector(masters.begin()->first));
}


inline Cluster::Slaves::Slaves(Cluster* _cluster, Masters* _masters)
  : cluster(_cluster), masters(_masters) {}


inline Cluster::Slaves::~Slaves()
{
  shutdown();
}


inline void Cluster::Slaves::shutdown()
{
  // TODO(benh): Use utils::copy from stout once namespaced.
  std::map<process::PID<slave::Slave>, Slave> copy(slaves);
  foreachpair (const process::PID<slave::Slave>& pid,
               const Slave& slave,
               copy) {
    process::Future<hashset<ContainerID> > containers =
      slave.containerizer->containers();
    AWAIT_READY(containers);

    foreach (const ContainerID& containerId, containers.get()) {
      // We need to wait on the container before destroying it in case someone
      // else has already waited on it (and therefore would be immediately
      // 'reaped' before we could wait on it).
      process::Future<containerizer::Termination> wait =
        slave.containerizer->wait(containerId);

      slave.containerizer->destroy(containerId);

      AWAIT_READY(wait);
    }

    stop(pid);
  }
  slaves.clear();
}


inline Try<process::PID<slave::Slave> > Cluster::Slaves::start(
    const slave::Flags& flags)
{
  // TODO(benh): Create a work directory if using the default.

  Slave slave;

  slave.flags = flags;

  // Create a new containerizer for this slave.
  Try<slave::Containerizer*> containerizer =
    slave::Containerizer::create(flags, true);
  CHECK_SOME(containerizer);

  slave.containerizer = containerizer.get();
  slave.createdContainerizer = true;

  // Get a detector for the master(s).
  slave.detector = masters->detector();

  slave.slave = new slave::Slave(
      flags, slave.detector.get(), slave.containerizer, &cluster->files);
  process::PID<slave::Slave> pid = process::spawn(slave.slave);

  slaves[pid] = slave;

  return pid;
}


inline Try<process::PID<slave::Slave> > Cluster::Slaves::start(
    slave::Containerizer* containerizer,
    const slave::Flags& flags)
{
  return start(containerizer, None(), flags);
}


inline Try<process::PID<slave::Slave> > Cluster::Slaves::start(
    const Option<MasterDetector*>& detector,
    const slave::Flags& flags)
{
  // TODO(benh): Create a work directory if using the default.

  Slave slave;

  slave.flags = flags;

  // Create a new containerizer for this slave.
  Try<slave::Containerizer*> containerizer =
    slave::Containerizer::create(flags, true);
  CHECK_SOME(containerizer);

  slave.containerizer = containerizer.get();
  slave.createdContainerizer = true;

  // Get a detector for the master(s) if one wasn't provided.
  if (detector.isNone()) {
    slave.detector = masters->detector();
  }

  slave.slave = new slave::Slave(
      flags,
      detector.get(slave.detector.get()),
      slave.containerizer,
      &cluster->files);

  process::PID<slave::Slave> pid = process::spawn(slave.slave);

  slaves[pid] = slave;

  return pid;
}


inline Try<process::PID<slave::Slave> > Cluster::Slaves::start(
    slave::Containerizer* containerizer,
    const Option<MasterDetector*>& detector,
    const slave::Flags& flags)
{
  // TODO(benh): Create a work directory if using the default.

  Slave slave;

  slave.containerizer = containerizer;

  slave.flags = flags;

  // Get a detector for the master(s) if one wasn't provided.
  if (detector.isNone()) {
    slave.detector = masters->detector();
  }

  slave.slave = new slave::Slave(
      flags,
      detector.get(slave.detector.get()),
      containerizer,
      &cluster->files);

  process::PID<slave::Slave> pid = process::spawn(slave.slave);

  slaves[pid] = slave;

  return pid;
}


inline Try<Nothing> Cluster::Slaves::stop(
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

#endif // __TESTS_CLUSTER_HPP__
