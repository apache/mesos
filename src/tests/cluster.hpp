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

#ifndef __TESTS_CLUSTER_HPP__
#define __TESTS_CLUSTER_HPP__

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/master/allocator.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "authorizer/local/authorizer.hpp"

#include "files/files.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif // __linux__

#include "log/log.hpp"

#include "master/constants.hpp"
#include "master/contender.hpp"
#include "master/detector.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/registrar.hpp"
#include "master/repairer.hpp"

#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/slave.hpp"
#include "slave/status_update_manager.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

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

    // Start a new master with the provided flags and injections.
    Try<process::PID<master::Master>> start(
        const master::Flags& flags = master::Flags(),
        const Option<mesos::master::allocator::Allocator*>& allocator = None(),
        const Option<Authorizer*>& authorizer = None(),
        const Option<std::shared_ptr<process::RateLimiter>>&
          slaveRemovalLimiter = None());

    // Stops and cleans up a master at the specified PID.
    Try<Nothing> stop(const process::PID<master::Master>& pid);

    // Returns a new master detector for this instance of masters.
    process::Owned<MasterDetector> detector();

    /**
     * The internal map from UPID to Master processes is not available
     * externally to test methods; this lookup method helps to expose this to
     * `MesosTest` classes.
     *
     * @param pid the PID for the Master process being looked up.
     * @return a pointer to the `master::Master` process, whose UPID corresponds
     *     to the given value, if any.
     */
    Option<master::Master*> find(const process::PID<master::Master>& pid)
    {
      if (masters.count(pid) != 0) {
        return masters[pid].master;
      }

      return None();
    }

  private:
    // Not copyable, not assignable.
    Masters(const Masters&);
    Masters& operator=(const Masters&);

    Cluster* cluster; // Enclosing class.
    Option<zookeeper::URL> url;

    // Encapsulates a single master's dependencies.
    struct Master
    {
      Master() : allocator(NULL), createdAllocator(false), master(NULL) {}

      mesos::master::allocator::Allocator* allocator;
      bool createdAllocator; // Whether we own the allocator.

      process::Owned<log::Log> log;
      process::Owned<state::Storage> storage;
      process::Owned<state::protobuf::State> state;
      process::Owned<master::Registrar> registrar;

      process::Owned<master::Repairer> repairer;

      process::Owned<MasterContender> contender;
      process::Owned<MasterDetector> detector;

      process::Owned<Authorizer> authorizer;

      Option<std::shared_ptr<process::RateLimiter>> slaveRemovalLimiter;

      master::Master* master;
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

    // Start a new slave with the provided flags and injections.
    Try<process::PID<slave::Slave>> start(
        const slave::Flags& flags = slave::Flags(),
        const Option<slave::Containerizer*>& containerizer = None(),
        const Option<MasterDetector*>& detector = None(),
        const Option<slave::GarbageCollector*>& gc = None(),
        const Option<slave::StatusUpdateManager*>& statusUpdateManager = None(),
        const Option<mesos::slave::ResourceEstimator*>& resourceEstimator =
          None(),
        const Option<mesos::slave::QoSController*>& qosController =
          None());

    // Stops and cleans up a slave at the specified PID. If 'shutdown'
    // is true than the slave is sent a shutdown message instead of
    // being terminated.
    Try<Nothing> stop(
        const process::PID<slave::Slave>& pid,
        bool shutdown = false);

  private:
    // Not copyable, not assignable.
    Slaves(const Slaves&);
    Slaves& operator=(const Slaves&);

    Cluster* cluster; // Enclosing class.
    Masters* masters; // Used to create MasterDetector instances.

    // Encapsulates a single slave's dependencies.
    struct Slave
    {
      Slave()
        : containerizer(NULL),
          createdContainerizer(false),
          slave(NULL) {}

      slave::Containerizer* containerizer;
      bool createdContainerizer; // Whether we own the containerizer.

      process::Owned<mesos::slave::ResourceEstimator> resourceEstimator;
      process::Owned<mesos::slave::QoSController> qosController;
      process::Owned<slave::Fetcher> fetcher;
      process::Owned<slave::StatusUpdateManager> statusUpdateManager;
      process::Owned<slave::GarbageCollector> gc;
      process::Owned<MasterDetector> detector;
      slave::Flags flags;
      slave::Slave* slave;
    };

    std::map<process::PID<slave::Slave>, Slave> slaves;
  };

  // Shuts down all masters and slaves.
  void shutdown()
  {
    masters.shutdown();
    slaves.shutdown();
  }

  /**
   * Thin wrapper around the internal `Masters::find()` lookup method, which is
   * otherwise inaccessible from test methods.
   *
   * @param pid the PID for the Master process being looked up.
   * @return a pointer to the `master::Master` process, whose PID corresponds
   *     to the given value, if any.
   */
  Option<master::Master*> find(const process::PID<master::Master>& pid)
  {
    return masters.find(pid);
  }


  // Cluster wide shared abstractions.
  Files files;

  Masters masters;
  Slaves slaves;

private:
  // Not copyable, not assignable.
  Cluster(const Cluster&);
  Cluster& operator=(const Cluster&);
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_CLUSTER_HPP__
