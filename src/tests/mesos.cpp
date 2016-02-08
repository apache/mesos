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

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/slave/container_logger.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif

#ifdef WITH_NETWORK_ISOLATOR
#include "linux/routing/utils.hpp"
#endif

#include "slave/constants.hpp"
#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using std::list;
using std::shared_ptr;
using std::string;
using testing::_;
using testing::Invoke;

using mesos::fetcher::FetcherInfo;

using mesos::slave::ContainerLogger;

using namespace process;

#ifdef WITH_NETWORK_ISOLATOR
using namespace routing;
#endif

namespace mesos {
namespace internal {
namespace tests {

#ifdef MESOS_HAS_JAVA
ZooKeeperTestServer* MesosZooKeeperTest::server = NULL;
Option<zookeeper::URL> MesosZooKeeperTest::url;
#endif // MESOS_HAS_JAVA

MesosTest::MesosTest(const Option<zookeeper::URL>& url) : cluster(url) {}


void MesosTest::TearDown()
{
  TemporaryDirectoryTest::TearDown();

  // TODO(benh): Fail the test if shutdown hasn't been called?
  Shutdown();
}


master::Flags MesosTest::CreateMasterFlags()
{
  master::Flags flags;

  // We use the current working directory from TempDirectoryTest
  // to ensure the work directory remains the same within a test.
  flags.work_dir = path::join(os::getcwd(), "master");

  CHECK_SOME(os::mkdir(flags.work_dir.get()));

  flags.authenticate_http = true;
  flags.authenticate_frameworks = true;
  flags.authenticate_slaves = true;

  // Create a default credentials file.
  const string& path =  path::join(os::getcwd(), "credentials");

  Try<int> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP);

  CHECK_SOME(fd);

  // JSON default format for credentials.
  Credentials credentials;

  Credential* credential = credentials.add_credentials();
  credential->set_principal(DEFAULT_CREDENTIAL.principal());
  credential->set_secret(DEFAULT_CREDENTIAL.secret());

  credential = credentials.add_credentials();
  credential->set_principal(DEFAULT_CREDENTIAL_2.principal());
  credential->set_secret(DEFAULT_CREDENTIAL_2.secret());

  CHECK_SOME(os::write(fd.get(), stringify(JSON::protobuf(credentials))))
     << "Failed to write credentials to '" << path << "'";
  CHECK_SOME(os::close(fd.get()));

  flags.credentials = path;

  // Set default ACLs.
  flags.acls = ACLs();

  // Use the replicated log (without ZooKeeper) by default.
  flags.registry = "replicated_log";
  flags.registry_strict = true;

  // On many test VMs, this default is too small.
  flags.registry_store_timeout = flags.registry_store_timeout * 5;

  flags.authenticators = tests::flags.authenticators;

  return flags;
}


slave::Flags MesosTest::CreateSlaveFlags()
{
  slave::Flags flags;

  // Create a temporary work directory (removed by Environment).
  Try<string> directory = environment->mkdtemp();
  CHECK_SOME(directory) << "Failed to create temporary directory";

  flags.work_dir = directory.get();
  flags.fetcher_cache_dir = path::join(directory.get(), "fetch");

  flags.launcher_dir = getLauncherDir();

  // Create a default credential file.
  const string& path = path::join(directory.get(), "credential");

  Try<int> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP);

  CHECK_SOME(fd);

  Credential credential;
  credential.set_principal(DEFAULT_CREDENTIAL.principal());
  credential.set_secret(DEFAULT_CREDENTIAL.secret());

  CHECK_SOME(os::write(fd.get(), stringify(JSON::protobuf(credential))))
     << "Failed to write slave credential to '" << path << "'";

  CHECK_SOME(os::close(fd.get()));

  flags.credential = path;

  flags.resources = defaultAgentResourcesString;

  flags.registration_backoff_factor = Milliseconds(10);

  // Make sure that the slave uses the same 'docker' as the tests.
  flags.docker = tests::flags.docker;

  if (tests::flags.isolation.isSome()) {
    flags.isolation = tests::flags.isolation.get();
  }

  return flags;
}


Try<PID<master::Master>> MesosTest::StartMaster(
    const Option<master::Flags>& flags)
{
  return cluster.masters.start(
      flags.isNone() ? CreateMasterFlags() : flags.get());
}


Try<PID<master::Master>> MesosTest::StartMaster(
    mesos::master::allocator::Allocator* allocator,
    const Option<master::Flags>& flags)
{
  return cluster.masters.start(
      flags.isNone() ? CreateMasterFlags() : flags.get(),
      allocator);
}


Try<PID<master::Master>> MesosTest::StartMaster(
    Authorizer* authorizer,
    const Option<master::Flags>& flags)
{
  return cluster.masters.start(
      flags.isNone() ? CreateMasterFlags() : flags.get(),
      None(),
      authorizer);
}


Try<PID<master::Master>> MesosTest::StartMaster(
    const shared_ptr<MockRateLimiter>& slaveRemovalLimiter,
    const Option<master::Flags>& flags)
{
  return cluster.masters.start(
      flags.isNone() ? CreateMasterFlags() : flags.get(),
      None(),
      None(),
      slaveRemovalLimiter);
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get());
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    MockExecutor* executor,
    const Option<slave::Flags>& flags)
{
  slave::Containerizer* containerizer = new TestContainerizer(executor);

  Try<PID<slave::Slave>> pid = StartSlave(containerizer, flags);

  if (pid.isError()) {
    delete containerizer;
    return pid;
  }

  containerizers[pid.get()] = containerizer;

  return pid;
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    slave::Containerizer* containerizer,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      containerizer);
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    slave::Containerizer* containerizer,
    const std::string& id,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      id,
      containerizer);
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    slave::Containerizer* containerizer,
    MasterDetector* detector,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      containerizer,
      detector);
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      None(),
      detector);
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    slave::GarbageCollector* gc,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      None(),
      detector,
      gc);
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    MockExecutor* executor,
    MasterDetector* detector,
    const Option<slave::Flags>& flags)
{
  slave::Containerizer* containerizer = new TestContainerizer(executor);

  Try<PID<slave::Slave>> pid = cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      containerizer,
      detector);

  if (pid.isError()) {
    delete containerizer;
    return pid;
  }

  containerizers[pid.get()] = containerizer;

  return pid;
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    mesos::slave::ResourceEstimator* resourceEstimator,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      None(),
      None(),
      None(),
      None(),
      resourceEstimator);
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    MockExecutor* executor,
    mesos::slave::ResourceEstimator* resourceEstimator,
    const Option<slave::Flags>& flags)
{
  slave::Containerizer* containerizer = new TestContainerizer(executor);

  Try<PID<slave::Slave>> pid = cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      containerizer,
      None(),
      None(),
      None(),
      resourceEstimator);

  if (pid.isError()) {
    delete containerizer;
    return pid;
  }

  containerizers[pid.get()] = containerizer;

  return pid;
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
  slave::Containerizer* containerizer,
  mesos::slave::ResourceEstimator* resourceEstimator,
  const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      containerizer,
      None(),
      None(),
      None(),
      resourceEstimator);
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    mesos::slave::QoSController* qoSController,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      None(),
      None(),
      None(),
      None(),
      None(),
      qoSController);
}


Try<PID<slave::Slave>> MesosTest::StartSlave(
    slave::Containerizer* containerizer,
    mesos::slave::QoSController* qoSController,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      flags.isNone() ? CreateSlaveFlags() : flags.get(),
      None(),
      containerizer,
      None(),
      None(),
      None(),
      None(),
      qoSController);
}


void MesosTest::Stop(const PID<master::Master>& pid)
{
  cluster.masters.stop(pid);
}


void MesosTest::Stop(const PID<slave::Slave>& pid, bool shutdown)
{
  cluster.slaves.stop(pid, shutdown);
  if (containerizers.count(pid) > 0) {
    slave::Containerizer* containerizer = containerizers[pid];
    containerizers.erase(pid);
    delete containerizer;
  }
}


void MesosTest::Shutdown()
{
  // TODO(arojas): Authenticators' lifetimes are tied to libprocess's lifetime.
  // Consider unsetting the authenticator in the master shutdown.
  // NOTE: This means that multiple masters in tests are not supported.
  process::http::authentication::unsetAuthenticator(
      master::DEFAULT_HTTP_AUTHENTICATION_REALM);
  ShutdownMasters();
  ShutdownSlaves();
}


void MesosTest::ShutdownMasters()
{
  cluster.masters.shutdown();
}


void MesosTest::ShutdownSlaves()
{
  cluster.slaves.shutdown();

  foreachvalue (slave::Containerizer* containerizer, containerizers) {
    delete containerizer;
  }
  containerizers.clear();
}

// Although the constructors and destructors for mock classes are
// often trivial, defining them out-of-line (in a separate compilation
// unit) improves compilation time: see MESOS-3827.

MockScheduler::MockScheduler() {}


MockScheduler::~MockScheduler() {}


MockExecutor::MockExecutor(const ExecutorID& _id) : id(_id) {}


MockExecutor::~MockExecutor() {}


MockGarbageCollector::MockGarbageCollector()
{
  // NOTE: We use 'EXPECT_CALL' and 'WillRepeatedly' here instead of
  // 'ON_CALL' and 'WillByDefault'. See 'TestContainerizer::SetUp()'
  // for more details.
  EXPECT_CALL(*this, schedule(_, _))
    .WillRepeatedly(Return(Nothing()));

  EXPECT_CALL(*this, unschedule(_))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, prune(_))
    .WillRepeatedly(Return());
}


MockGarbageCollector::~MockGarbageCollector() {}


MockResourceEstimator::MockResourceEstimator()
{
  ON_CALL(*this, initialize(_))
    .WillByDefault(Return(Nothing()));
  EXPECT_CALL(*this, initialize(_))
    .WillRepeatedly(DoDefault());

  ON_CALL(*this, oversubscribable())
    .WillByDefault(Return(process::Future<Resources>()));
  EXPECT_CALL(*this, oversubscribable())
    .WillRepeatedly(DoDefault());
}

MockResourceEstimator::~MockResourceEstimator() {}


MockQoSController::MockQoSController()
{
  ON_CALL(*this, initialize(_))
    .WillByDefault(Return(Nothing()));
  EXPECT_CALL(*this, initialize(_))
    .WillRepeatedly(DoDefault());

  ON_CALL(*this, corrections())
    .WillByDefault(
        Return(process::Future<list<mesos::slave::QoSCorrection>>()));
  EXPECT_CALL(*this, corrections())
    .WillRepeatedly(DoDefault());
}


MockQoSController::~MockQoSController() {}


MockSlave::MockSlave(const slave::Flags& flags,
                     MasterDetector* detector,
                     slave::Containerizer* containerizer,
                     const Option<mesos::slave::QoSController*>& _qosController)
  : slave::Slave(
      process::ID::generate("slave"),
      flags,
      detector,
      containerizer,
      &files,
      &gc,
      statusUpdateManager = new slave::StatusUpdateManager(flags),
      &resourceEstimator,
      _qosController.isSome() ? _qosController.get() : &qosController)
{
  // Set up default behaviors, calling the original methods.
  EXPECT_CALL(*this, runTask(_, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_runTask));
  EXPECT_CALL(*this, _runTask(_, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked__runTask));
  EXPECT_CALL(*this, killTask(_, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_killTask));
  EXPECT_CALL(*this, removeFramework(_))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_removeFramework));
  EXPECT_CALL(*this, __recover(_))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked___recover));
  EXPECT_CALL(*this, qosCorrections())
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_qosCorrections));
}


MockSlave::~MockSlave()
{
  delete statusUpdateManager;
}


void MockSlave::unmocked_runTask(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const FrameworkID& frameworkId,
    const UPID& pid,
    TaskInfo task)
{
  slave::Slave::runTask(from, frameworkInfo, frameworkInfo.id(), pid, task);
}


void MockSlave::unmocked__runTask(
      const Future<bool>& future,
      const FrameworkInfo& frameworkInfo,
      const TaskInfo& task)
{
  slave::Slave::_runTask(future, frameworkInfo, task);
}


void MockSlave::unmocked_killTask(
      const UPID& from,
      const FrameworkID& frameworkId,
      const TaskID& taskId)
{
  slave::Slave::killTask(from, frameworkId, taskId);
}


void MockSlave::unmocked_removeFramework(slave::Framework* framework)
{
  slave::Slave::removeFramework(framework);
}


void MockSlave::unmocked___recover(const Future<Nothing>& future)
{
  slave::Slave::__recover(future);
}


void MockSlave::unmocked_qosCorrections()
{
  slave::Slave::qosCorrections();
}


MockFetcherProcess::MockFetcherProcess()
{
  // Set up default behaviors, calling the original methods.
  EXPECT_CALL(*this, _fetch(_, _, _, _, _, _)).
    WillRepeatedly(
        Invoke(this, &MockFetcherProcess::unmocked__fetch));
  EXPECT_CALL(*this, run(_, _, _, _, _)).
    WillRepeatedly(Invoke(this, &MockFetcherProcess::unmocked_run));
}


MockFetcherProcess::~MockFetcherProcess() {}


MockContainerLogger::MockContainerLogger()
{
  // Set up default behaviors.
  EXPECT_CALL(*this, initialize())
    .WillRepeatedly(Return(Nothing()));

  EXPECT_CALL(*this, recover(_, _))
    .WillRepeatedly(Return(Nothing()));

  // All output is redirected to STDOUT_FILENO and STDERR_FILENO.
  EXPECT_CALL(*this, prepare(_, _))
    .WillRepeatedly(Return(mesos::slave::ContainerLogger::SubprocessInfo()));
}

MockContainerLogger::~MockContainerLogger() {}


MockDocker::MockDocker(
    const string& path,
    const string &socket)
  : Docker(path, socket)
{
  EXPECT_CALL(*this, ps(_, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_ps));

  EXPECT_CALL(*this, pull(_, _, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_pull));

  EXPECT_CALL(*this, stop(_, _, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_stop));

  EXPECT_CALL(*this, run(_, _, _, _, _, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_run));

  EXPECT_CALL(*this, inspect(_, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_inspect));
}


MockDocker::~MockDocker() {}


MockDockerContainerizer::MockDockerContainerizer(
    const slave::Flags& flags,
    slave::Fetcher* fetcher,
    const process::Owned<ContainerLogger>& logger,
    process::Shared<Docker> docker)
  : slave::DockerContainerizer(flags, fetcher, logger, docker)
{
  initialize();
}


MockDockerContainerizer::MockDockerContainerizer(
    const process::Owned<slave::DockerContainerizerProcess>& process)
  : slave::DockerContainerizer(process)
{
  initialize();
}


MockDockerContainerizer::~MockDockerContainerizer() {}


MockDockerContainerizerProcess::MockDockerContainerizerProcess(
    const slave::Flags& flags,
    slave::Fetcher* fetcher,
    const process::Owned<ContainerLogger>& logger,
    const process::Shared<Docker>& docker)
  : slave::DockerContainerizerProcess(flags, fetcher, logger, docker)
{
  EXPECT_CALL(*this, fetch(_, _))
    .WillRepeatedly(Invoke(this, &MockDockerContainerizerProcess::_fetch));

  EXPECT_CALL(*this, pull(_))
    .WillRepeatedly(Invoke(this, &MockDockerContainerizerProcess::_pull));
}


MockDockerContainerizerProcess::~MockDockerContainerizerProcess() {}


MockAuthorizer::MockAuthorizer()
{
  // NOTE: We use 'EXPECT_CALL' and 'WillRepeatedly' here instead of
  // 'ON_CALL' and 'WillByDefault'. See 'TestContainerizer::SetUp()'
  // for more details.
  EXPECT_CALL(*this, authorize(An<const mesos::ACL::RegisterFramework&>()))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, authorize(An<const mesos::ACL::RunTask&>()))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, authorize(An<const mesos::ACL::ShutdownFramework&>()))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, authorize(An<const mesos::ACL::ReserveResources&>()))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, authorize(An<const mesos::ACL::UnreserveResources&>()))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, authorize(An<const mesos::ACL::CreateVolume&>()))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, authorize(An<const mesos::ACL::DestroyVolume&>()))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, authorize(An<const mesos::ACL::SetQuota&>()))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, authorize(An<const mesos::ACL::RemoveQuota&>()))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, initialize(An<const Option<ACLs>&>()))
    .WillRepeatedly(Return(Nothing()));
}


MockAuthorizer::~MockAuthorizer() {}


process::Future<Nothing> MockFetcherProcess::unmocked__fetch(
  const hashmap<CommandInfo::URI, Option<Future<shared_ptr<Cache::Entry>>>>&
    entries,
  const ContainerID& containerId,
  const string& sandboxDirectory,
  const string& cacheDirectory,
  const Option<string>& user,
  const slave::Flags& flags)
{
  return slave::FetcherProcess::_fetch(
      entries,
      containerId,
      sandboxDirectory,
      cacheDirectory,
      user,
      flags);
}


process::Future<Nothing> MockFetcherProcess::unmocked_run(
    const ContainerID& containerId,
    const string& sandboxDirectory,
    const Option<string>& user,
    const FetcherInfo& info,
    const slave::Flags& flags)
{
  return slave::FetcherProcess::run(
      containerId,
      sandboxDirectory,
      user,
      info,
      flags);
}


slave::Flags ContainerizerTest<slave::MesosContainerizer>::CreateSlaveFlags()
{
  slave::Flags flags = MesosTest::CreateSlaveFlags();

  // If the user has specified isolation on command-line, we better
  // use it.
  if (tests::flags.isolation.isSome()) {
    flags.isolation = tests::flags.isolation.get();
    return flags;
  }

#ifdef __linux__
  Result<string> user = os::user();
  EXPECT_SOME(user);

  // Use cgroup isolators if they're available and we're root.
  // TODO(idownes): Refactor the cgroups/non-cgroups code.
  if (cgroups::enabled() && user.get() == "root") {
    flags.isolation = "cgroups/cpu,cgroups/mem";
    flags.cgroups_hierarchy = baseHierarchy;
    flags.cgroups_root = TEST_CGROUPS_ROOT + "_" + UUID::random().toString();

    // Enable putting the slave into memory and cpuacct cgroups.
    flags.slave_subsystems = "memory,cpuacct";
  } else {
    flags.isolation = "posix/cpu,posix/mem";
  }
#else
  flags.isolation = "posix/cpu,posix/mem";
#endif

#ifdef WITH_NETWORK_ISOLATOR
  if (user.get() == "root" && routing::check().isSome()) {
    flags.isolation = strings::join(
        ",",
        flags.isolation,
        "network/port_mapping");

    // NOTE: By default, Linux sets host ip local port range to
    // [32768, 61000]. We set 'ephemeral_ports' resource so that it
    // does not overlap with the host ip local port range.
    flags.resources = strings::join(
        ";",
        flags.resources.get(),
        "ephemeral_ports:[30001-30999]");

    // NOTE: '16' should be enough for all our tests.
    flags.ephemeral_ports_per_container = 16;
  }
#endif

  return flags;
}


#ifdef __linux__
void ContainerizerTest<slave::MesosContainerizer>::SetUpTestCase()
{
  Result<string> user = os::user();
  EXPECT_SOME(user);

  if (cgroups::enabled() && user.get() == "root") {
    // Clean up any testing hierarchies.
    Try<std::set<string>> hierarchies = cgroups::hierarchies();
    ASSERT_SOME(hierarchies);
    foreach (const string& hierarchy, hierarchies.get()) {
      if (strings::startsWith(hierarchy, TEST_CGROUPS_HIERARCHY)) {
        AWAIT_READY(cgroups::cleanup(hierarchy));
      }
    }
  }
}


void ContainerizerTest<slave::MesosContainerizer>::TearDownTestCase()
{
  MesosTest::TearDownTestCase();

  Result<string> user = os::user();
  EXPECT_SOME(user);

  if (cgroups::enabled() && user.get() == "root") {
    // Clean up any testing hierarchies.
    Try<std::set<string>> hierarchies = cgroups::hierarchies();
    ASSERT_SOME(hierarchies);
    foreach (const string& hierarchy, hierarchies.get()) {
      if (strings::startsWith(hierarchy, TEST_CGROUPS_HIERARCHY)) {
        AWAIT_READY(cgroups::cleanup(hierarchy));
      }
    }
  }
}


void ContainerizerTest<slave::MesosContainerizer>::SetUp()
{
  MesosTest::SetUp();

  subsystems.insert("cpu");
  subsystems.insert("cpuacct");
  subsystems.insert("memory");
  subsystems.insert("freezer");
  subsystems.insert("perf_event");

  Result<string> user = os::user();
  EXPECT_SOME(user);

  if (cgroups::enabled() && user.get() == "root") {
    // Determine the base hierarchy.
    foreach (const string& subsystem, subsystems) {
      Result<string> hierarchy = cgroups::hierarchy(subsystem);
      ASSERT_FALSE(hierarchy.isError());

      if (hierarchy.isSome()) {
        Try<string> _baseHierarchy = Path(hierarchy.get()).dirname();
        ASSERT_SOME(_baseHierarchy)
          << "Failed to get the base of hierarchy '" << hierarchy.get() << "'";

        if (baseHierarchy.empty()) {
          baseHierarchy = _baseHierarchy.get();
        } else {
          ASSERT_EQ(baseHierarchy, _baseHierarchy.get())
            << "-------------------------------------------------------------\n"
            << "Multiple cgroups base hierarchies detected:\n"
            << "  '" << baseHierarchy << "'\n"
            << "  '" << _baseHierarchy.get() << "'\n"
            << "Mesos does not support multiple cgroups base hierarchies.\n"
            << "Please unmount the corresponding (or all) subsystems.\n"
            << "-------------------------------------------------------------";
        }
      }
    }

    if (baseHierarchy.empty()) {
      baseHierarchy = TEST_CGROUPS_HIERARCHY;
    }

    // Mount the subsystem if necessary.
    foreach (const string& subsystem, subsystems) {
      const string& hierarchy = path::join(baseHierarchy, subsystem);

      Try<bool> mounted = cgroups::mounted(hierarchy, subsystem);
      ASSERT_SOME(mounted);

      if (!mounted.get()) {
        ASSERT_SOME(cgroups::mount(hierarchy, subsystem))
          << "-------------------------------------------------------------\n"
          << "We cannot run any cgroups tests that require\n"
          << "a hierarchy with subsystem '" << subsystem << "'\n"
          << "because we failed to find an existing hierarchy\n"
          << "or create a new one (tried '" << hierarchy << "').\n"
          << "You can either remove all existing\n"
          << "hierarchies, or disable this test case\n"
          << "(i.e., --gtest_filter=-"
          << ::testing::UnitTest::GetInstance()
              ->current_test_info()
              ->test_case_name() << ".*).\n"
          << "-------------------------------------------------------------";
      } else {
        // If the subsystem is already mounted in the hierarchy make
        // sure that we don't have any existing cgroups that have
        // persisted that match our TEST_CGROUPS_ROOT (because
        // otherwise our tests will fail when we try and clean them up
        // later).
        Try<std::vector<string>> cgroups = cgroups::get(hierarchy);
        ASSERT_SOME(cgroups);

        foreach (const string& cgroup, cgroups.get()) {
          // Remove any cgroups that start with TEST_CGROUPS_ROOT.
          if (strings::startsWith(cgroup, TEST_CGROUPS_ROOT)) {
            AWAIT_READY(cgroups::destroy(hierarchy, cgroup))
              << "-----------------------------------------------------------\n"
              << "We're very sorry but we can't seem to destroy existing\n"
              << "cgroups that we likely created as part of an earlier\n"
              << "invocation of the tests. Please manually destroy the cgroup\n"
              << "at '" << path::join(hierarchy, cgroup) << "' by first\n"
              << "manually killing all the processes found in the file at '"
              << path::join(hierarchy, cgroup, "tasks") << "'\n"
              << "-----------------------------------------------------------";
          }
        }
      }
    }
  }
}


void ContainerizerTest<slave::MesosContainerizer>::TearDown()
{
  MesosTest::TearDown();

  Result<string> user = os::user();
  EXPECT_SOME(user);

  if (cgroups::enabled() && user.get() == "root") {
    foreach (const string& subsystem, subsystems) {
      string hierarchy = path::join(baseHierarchy, subsystem);

      Try<std::vector<string>> cgroups = cgroups::get(hierarchy);
      ASSERT_SOME(cgroups);

      foreach (const string& cgroup, cgroups.get()) {
        // Remove any cgroups that start with TEST_CGROUPS_ROOT.
        if (strings::startsWith(cgroup, TEST_CGROUPS_ROOT)) {
          AWAIT_READY(cgroups::destroy(hierarchy, cgroup));
        }
      }
    }
  }
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
