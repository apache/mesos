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

#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/owned.hpp>
#include <stout/try.hpp>

#include "detector/detector.hpp"

#include "files/files.hpp"

#include "master/allocator.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/isolator.hpp"
#include "slave/slave.hpp"

#include "tests/isolator.hpp" // For TestingIsolator.

#include "zookeeper/url.hpp"

namespace mesos {
namespace internal {
namespace tests {

class Cluster
{
public:
  // TODO(benh): Take flags and make const in Masters and Slaves.
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

    // Start and manage a new master.
    Try<process::PID<master::Master> > start();
    Try<process::PID<master::Master> > start(const master::Flags& flags);
    Try<process::PID<master::Master> > start(
        master::AllocatorProcess* allocatorProcess);

    // Stops and cleans up a master at the specified PID.
    Try<Nothing> stop(const process::PID<master::Master>& pid);

    // Returns a new master detector for this instance of masters.
    Owned<MasterDetector> detector(
        const process::PID<slave::Slave>& pid,
        bool quiet);

    // "Default" flags used for creating masters.
    master::Flags flags;

  private:
    // Not copyable, not assignable.
    Masters(const Masters&);
    Masters& operator = (const Masters&);

    Cluster* cluster; // Enclosing class.
    Option<zookeeper::URL> url;

    struct Master
    {
      Master()
        : master(NULL),
          allocator(NULL),
          allocatorProcess(NULL),
          detector(NULL) {}

      master::Master* master;
      master::Allocator* allocator;
      master::AllocatorProcess* allocatorProcess;
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

    // Start and manage a new slave.
    Try<process::PID<slave::Slave> > start();
    Try<process::PID<slave::Slave> > start(const slave::Flags& flags);
    Try<process::PID<slave::Slave> > start(
        const ExecutorID& executorId,
        Executor* executor);
    Try<process::PID<slave::Slave> > start(slave::Isolator* isolator);
    Try<process::PID<slave::Slave> > start(
        const slave::Flags& flags,
        slave::Isolator* isolator);

    // Stops and cleans up a slave at the specified PID.
    Try<Nothing> stop(const process::PID<slave::Slave>& pid);

    // "Default" flags used for creating slaves.
    slave::Flags flags;

  private:
    // Not copyable, not assignable.
    Slaves(const Slaves&);
    Slaves& operator = (const Slaves&);

    Cluster* cluster; // Enclosing class.
    Masters* masters; // Used to create MasterDetector instances.

    struct Slave
    {
      Slave()
        : slave(NULL),
          isolator(NULL),
          detector(NULL) {}

      slave::Slave* slave;
      slave::Isolator* isolator;
      Owned<MasterDetector> detector;
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


inline Try<process::PID<master::Master> > Cluster::Masters::start()
{
  // Disallow multiple masters when not using ZooKeeper.
  if (!masters.empty() && url.isNone()) {
    return Error("Can not start multiple masters when not using ZooKeeper");
  }

  Master master;
  master.allocatorProcess = new master::HierarchicalDRFAllocatorProcess();
  master.allocator = new master::Allocator(master.allocatorProcess);
  master.master = new master::Master(master.allocator, &cluster->files, flags);

  process::PID<master::Master> pid = process::spawn(master.master);

  if (url.isSome()) {
    master.detector = new ZooKeeperMasterDetector(url.get(), pid, true, true);
  } else {
    master.detector = new BasicMasterDetector(pid);
  }

  masters[pid] = master;

  return pid;
}


inline Try<process::PID<master::Master> > Cluster::Masters::start(
    const master::Flags& flags)
{
  // Disallow multiple masters when not using ZooKeeper.
  if (!masters.empty() && url.isNone()) {
    return Error("Can not start multiple masters when not using ZooKeeper");
  }

  Master master;
  master.allocatorProcess = new master::HierarchicalDRFAllocatorProcess();
  master.allocator = new master::Allocator(master.allocatorProcess);
  master.master = new master::Master(master.allocator, &cluster->files, flags);

  process::PID<master::Master> pid = process::spawn(master.master);

  if (url.isSome()) {
    master.detector = new ZooKeeperMasterDetector(url.get(), pid, true, true);
  } else {
    master.detector = new BasicMasterDetector(pid);
  }

  masters[pid] = master;

  return pid;
}


inline Try<process::PID<master::Master> > Cluster::Masters::start(
    master::AllocatorProcess* allocatorProcess)
{
  // Disallow multiple masters when not using ZooKeeper.
  if (!masters.empty() && url.isNone()) {
    return Error("Can not start multiple masters when not using ZooKeeper");
  }

  Master master;
  master.allocatorProcess = NULL;
  master.allocator = new master::Allocator(allocatorProcess);
  master.master = new master::Master(master.allocator, &cluster->files, flags);

  process::PID<master::Master> pid = process::spawn(master.master);

  if (url.isSome()) {
    master.detector = new ZooKeeperMasterDetector(url.get(), pid, true, true);
  } else {
    master.detector = new BasicMasterDetector(pid);
  }

  masters[pid] = master;

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

  delete master.allocator; // Terminates and waits for the allocator process.

  if (master.allocatorProcess != NULL) {
    delete master.allocatorProcess;
  }

  delete master.detector;

  masters.erase(pid);

  return Nothing();
}


inline Owned<MasterDetector> Cluster::Masters::detector(
    const process::PID<slave::Slave>& pid,
    bool quiet)
{
  if (url.isSome()) {
    return new ZooKeeperMasterDetector(url.get(), pid, false, quiet);
  }

  CHECK(masters.size() == 1);
  return new BasicMasterDetector(masters.begin()->first, pid);
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
  foreachkey (const process::PID<slave::Slave>& pid, copy) {
    stop(pid);
  }
  slaves.clear();
}


inline Try<process::PID<slave::Slave> > Cluster::Slaves::start()
{
  // TODO(benh): Check that we dont have another slave already running
  // with flags that conflict (e.g., work_dir).

  Slave slave;

  slave.isolator = new TestingIsolator();

  process::spawn(slave.isolator);

  // TODO(benh): Create a work directory for each slave.

  slave.slave = new slave::Slave(flags, true, slave.isolator, &cluster->files);

  process::PID<slave::Slave> pid = process::spawn(slave.slave);

  // Get a detector for the master(s).
  slave.detector = masters->detector(pid, flags.quiet);

  slaves[pid] = slave;

  return pid;
}


inline Try<process::PID<slave::Slave> > Cluster::Slaves::start(
    const slave::Flags& flags)
{
  // TODO(benh): Check that we dont have another slave already running
  // with flags that conflict (e.g., work_dir).

  Slave slave;

  slave.isolator = new TestingIsolator();

  process::spawn(slave.isolator);

  // TODO(benh): Create a work directory for each slave.

  slave.slave = new slave::Slave(flags, true, slave.isolator, &cluster->files);

  process::PID<slave::Slave> pid = process::spawn(slave.slave);

  // Get a detector for the master(s).
  slave.detector = masters->detector(pid, flags.quiet);

  slaves[pid] = slave;

  return pid;
}


inline Try<process::PID<slave::Slave> > Cluster::Slaves::start(
    const ExecutorID& executorId,
    Executor* executor)
{
  // TODO(benh): Check that we dont have another slave already running
  // with flags that conflict (e.g., work_dir).

  Slave slave;

  slave.isolator = new TestingIsolator(executorId, executor);

  process::spawn(slave.isolator);

  // TODO(benh): Create a work directory for each slave.

  slave.slave = new slave::Slave(flags, true, slave.isolator, &cluster->files);

  process::PID<slave::Slave> pid = process::spawn(slave.slave);

  // Get a detector for the master(s).
  slave.detector = masters->detector(pid, flags.quiet);

  slaves[pid] = slave;

  return pid;
}


inline Try<process::PID<slave::Slave> > Cluster::Slaves::start(
    slave::Isolator* isolator)
{
  // TODO(benh): Check that we dont have another slave already running
  // with flags that conflict (e.g., work_dir).

  Slave slave;

  // TODO(benh): Create a work directory for each slave.

  slave.slave = new slave::Slave(flags, true, isolator, &cluster->files);

  process::PID<slave::Slave> pid = process::spawn(slave.slave);

  // Get a detector for the master(s).
  slave.detector = masters->detector(pid, flags.quiet);

  slaves[pid] = slave;

  return pid;
}


inline Try<process::PID<slave::Slave> > Cluster::Slaves::start(
    const slave::Flags& flags,
    slave::Isolator* isolator)
{
  // TODO(benh): Check that we dont have another slave already running
  // with flags that conflict (e.g., work_dir).

  Slave slave;

  // TODO(benh): Create a work directory for each slave.

  slave.slave = new slave::Slave(flags, true, isolator, &cluster->files);

  process::PID<slave::Slave> pid = process::spawn(slave.slave);

  // Get a detector for the master(s).
  slave.detector = masters->detector(pid, flags.quiet);

  slaves[pid] = slave;

  return pid;
}


inline Try<Nothing> Cluster::Slaves::stop(
    const process::PID<slave::Slave>& pid)
{
  if (slaves.count(pid) == 0) {
    return Error("No slave found to stop");
  }

  Slave slave = slaves[pid];

  process::terminate(slave.slave);
  process::wait(slave.slave);
  delete slave.slave;

  if (slave.isolator != NULL) {
    // TODO(benh): Terminate and wait for the isolator once the slave
    // is no longer doing so.
    // process::terminate(slave.isolator);
    // process::wait(slave.isolator);
    delete slave.isolator;
  }

  slaves.erase(pid);

  return Nothing();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_CLUSTER_HPP__
