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

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/log/log.hpp>

#include <mesos/allocator/allocator.hpp>
#include <mesos/master/contender.hpp>
#include <mesos/master/detector.hpp>

#include <mesos/slave/resource_estimator.hpp>

#include <mesos/state/in_memory.hpp>
#include <mesos/state/log.hpp>
#include <mesos/state/state.hpp>
#include <mesos/state/storage.hpp>

#include <mesos/zookeeper/url.hpp>

#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "authorizer/local/authorizer.hpp"

#include "files/files.hpp"

#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/csi_server.hpp"
#include "slave/flags.hpp"
#include "slave/gc.hpp"
#include "slave/slave.hpp"
#include "slave/task_status_update_manager.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "tests/mock_registrar.hpp"
#include "tests/mock_slave.hpp"

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
      const Option<mesos::allocator::Allocator*>& allocator = None(),
      const Option<Authorizer*>& authorizer = None(),
      const Option<std::shared_ptr<process::RateLimiter>>&
        slaveRemovalLimiter = None());

  ~Master();

  // Returns a new master detector for this instance of master.
  process::Owned<mesos::master::detector::MasterDetector> createDetector();

  // Returns the `MasterInfo` associated with the underlying master process.
  MasterInfo getMasterInfo();

  // The underlying master process.
  process::PID<master::Master> pid;

  // Sets authorization callbacks in libprocess.
  void setAuthorizationCallbacks(Authorizer* authorizer);

private:
  Master() : files(master::READONLY_HTTP_AUTHENTICATION_REALM) {};

  // Not copyable, not assignable.
  Master(const Master&) = delete;
  Master& operator=(const Master&) = delete;

  Option<zookeeper::URL> zookeeperUrl;
  Files files;

  // Dependencies that are created by the factory method. The order in
  // which these fields are declared should match the order in which
  // they are initialized by the constructor; this ensures that
  // dependencies between these fields are handled correctly during
  // destruction.

  process::Owned<mesos::allocator::Allocator> allocator;
  process::Owned<Authorizer> authorizer;
  process::Owned<mesos::master::contender::MasterContender> contender;
  process::Owned<mesos::master::detector::MasterDetector> detector;
  process::Owned<mesos::log::Log> log;
  process::Owned<mesos::state::Storage> storage;
  process::Owned<mesos::state::State> state;
public:
  // Exposed for testing and mocking purposes. We always use a
  // `MockRegistrar` in case the test case wants to inspect how the
  // master interacts with the registrar; by default, the mock
  // registrar behaves identically to the normal registrar.
  process::Owned<MockRegistrar> registrar;

  // The underlying master object.
  process::Owned<master::Master> master;

private:
  Option<std::shared_ptr<process::RateLimiter>> slaveRemovalLimiter;

  // Indicates whether or not authorization callbacks were set when this master
  // was constructed.
  bool authorizationCallbacksSet;
};


class Slave
{
public:
  // Factory method that creates a new slave with the provided flags and
  // injections. The destructor of this object will cleanup some, but not
  // all of its state by calling the injections. Cleanup includes:
  //   * All destructors of each slave dependency created in this factory.
  //   * If neither `terminate` nor `shutdown` are called, all containers
  //     will be destroyed before termination.
  //   * Terminating the slave process.
  //   * On Linux, we will simulate an OS process exiting.
  static Try<process::Owned<Slave>> create(
      mesos::master::detector::MasterDetector* detector,
      const slave::Flags& flags = slave::Flags(),
      const Option<std::string>& id = None(),
      const Option<slave::Containerizer*>& containerizer = None(),
      const Option<slave::GarbageCollector*>& gc = None(),
      const Option<slave::TaskStatusUpdateManager*>& taskStatusUpdateManager =
        None(),
      const Option<mesos::slave::ResourceEstimator*>& resourceEstimator =
        None(),
      const Option<mesos::slave::QoSController*>& qosController = None(),
      const Option<mesos::SecretGenerator*>& secretGenerator = None(),
      const Option<Authorizer*>& authorizer = None(),
      const Option<PendingFutureTracker*>& futureTracker = None(),
      const Option<process::Owned<slave::CSIServer>>& csiServer = None(),
      bool mock = false);

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

  // Returns the pointer to the mock slave object. Returns nullptr if
  // this is not a mock slave.
  MockSlave* mock();

  // Start the mock slave as it is not auto started. This is a no-op
  // if it is a real slave.
  void start();

  // The underlying slave process.
  process::PID<slave::Slave> pid;

  // Sets authorization callbacks in libprocess.
  void setAuthorizationCallbacks(Authorizer* authorizer);

private:
  Slave() : files(slave::READONLY_HTTP_AUTHENTICATION_REALM) {};

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
  mesos::master::detector::MasterDetector* detector = nullptr;

  // Containerizer that is either owned outside of this `Slave` object
  // or by `ownedContainerizer`.  We keep a copy of this pointer
  // because the cleanup logic acts upon the containerizer (regardless
  // of who created it).
  slave::Containerizer* containerizer = nullptr;

  // Pending future tracker must be destroyed last since there may be
  // pending requests related to the dependant objects declared below.
  process::Owned<PendingFutureTracker> futureTracker;

  // Dependencies that are created by the factory method.
  process::Owned<Authorizer> authorizer;
  process::Owned<slave::Containerizer> ownedContainerizer;
  process::Owned<slave::Fetcher> fetcher;
  process::Owned<slave::GarbageCollector> gc;
  process::Owned<mesos::slave::QoSController> qosController;
  process::Owned<mesos::slave::ResourceEstimator> resourceEstimator;
  process::Owned<mesos::SecretGenerator> secretGenerator;
  process::Owned<slave::TaskStatusUpdateManager> taskStatusUpdateManager;
  process::Owned<slave::CSIServer> csiServer;

  // Indicates whether or not authorization callbacks were set when this agent
  // was constructed.
  bool authorizationCallbacksSet;

  // The underlying slave object.
  process::Owned<slave::Slave> slave;
};

} // namespace cluster {
} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_CLUSTER_HPP__
