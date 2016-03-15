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

#include <memory>
#include <string>

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
namespace cluster {

class Master
{
public:
  // Factory method that starts a new master with the provided flags and
  // injections. The destructor of this object will cleanup some, but not
  // all of its state by calling the injections. Cleanup includes:
  //   * All destructors of each master dependency created in this factory.
  //   * Unsetting the libprocess authenticator.
  //   * Terminating the master process.
  static Try<process::Owned<Master>> start(
      const master::Flags& flags = master::Flags(),
      const Option<zookeeper::URL>& zookeeperUrl = None(),
      const Option<mesos::master::allocator::Allocator*>& allocator = None(),
      const Option<Authorizer*>& authorizer = None(),
      const Option<std::shared_ptr<process::RateLimiter>>&
        slaveRemovalLimiter = None());

  ~Master();

  // Returns a new master detector for this instance of master.
  process::Owned<MasterDetector> createDetector();

  // Returns the `MasterInfo` associated with the underlying master process.
  MasterInfo getMasterInfo();

  // The underlying master process.
  process::PID<master::Master> pid;

private:
  Master() = default;

  // Not copyable, not assignable.
  Master(const Master&) = delete;
  Master& operator=(const Master&) = delete;

  Option<zookeeper::URL> zookeeperUrl;
  Files files;

  // Dependencies that are created by the factory method.
  process::Owned<mesos::master::allocator::Allocator> allocator;
  process::Owned<Authorizer> authorizer;
  process::Owned<MasterContender> contender;
  process::Owned<MasterDetector> detector;
  process::Owned<log::Log> log;
  process::Owned<master::Registrar> registrar;
  process::Owned<master::Repairer> repairer;
  process::Owned<state::protobuf::State> state;
  process::Owned<state::Storage> storage;

  Option<std::shared_ptr<process::RateLimiter>> slaveRemovalLimiter;

  // The underlying master object.
  process::Owned<master::Master> master;
};


class Slave
{
public:
  // Factory method that starts a new slave with the provided flags and
  // injections. The destructor of this object will cleanup some, but not
  // all of its state by calling the injections. Cleanup includes:
  //   * All destructors of each slave dependency created in this factory.
  //   * If neither `terminate` nor `shutdown` are called, all containers
  //     will be destroyed before termination.
  //   * Terminating the slave process.
  //   * On Linux, we will simulate an OS process exiting.
  static Try<process::Owned<Slave>> start(
      MasterDetector* detector,
      const slave::Flags& flags = slave::Flags(),
      const Option<std::string>& id = None(),
      const Option<slave::Containerizer*>& containerizer = None(),
      const Option<slave::GarbageCollector*>& gc = None(),
      const Option<slave::StatusUpdateManager*>& statusUpdateManager = None(),
      const Option<mesos::slave::ResourceEstimator*>& resourceEstimator =
        None(),
      const Option<mesos::slave::QoSController*>& qosController = None());

  ~Slave();

  // Stops this slave by either dispatching a shutdown call to the underlying
  // slave process or terminating it. If either of these methods are called,
  // this wrapper object will not clean up containers during its destruction.
  // NOTE: Destroying the containerizer does not clean up containers.
  //
  // These methods are useful if the test wants to emulate the slave's
  // shutdown/termination logic. For example, the slave-recovery tests do
  // not want to destroy all containers when restarting the agent.
  void shutdown();
  void terminate();

  // The underlying slave process.
  process::PID<slave::Slave> pid;

private:
  Slave() = default;

  // Not copyable, not assignable.
  Slave(const Slave&) = delete;
  Slave& operator=(const Slave&) = delete;

  // Helper for `shutdown` and `terminate`.
  // Waits for the underlying slave process to finish and then
  // (Linux-only) simulates an OS process exiting.
  void wait();

  slave::Flags flags;
  Files files;

  // This is set to `false` if either `shutdown()` or `terminate()` are called.
  // If false, the destructor of this `Slave` will not clean up containers.
  bool cleanUpContainersInDestructor = true;

  // Master detector that is not managed by this object.
  MasterDetector* detector;

  // Containerizer that is either owned outside of this `Slave` object
  // or by `ownedContainerizer`.  We keep a copy of this pointer
  // because the cleanup logic acts upon the containerizer (regardless
  // of who created it).
  slave::Containerizer* containerizer;

  // Dependencies that are created by the factory method.
  process::Owned<slave::Containerizer> ownedContainerizer;
  process::Owned<slave::Fetcher> fetcher;
  process::Owned<slave::GarbageCollector> gc;
  process::Owned<mesos::slave::QoSController> qosController;
  process::Owned<mesos::slave::ResourceEstimator> resourceEstimator;
  process::Owned<slave::StatusUpdateManager> statusUpdateManager;

  // The underlying slave object.
  process::Owned<slave::Slave> slave;
};

} // namespace cluster {
} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_CLUSTER_HPP__
