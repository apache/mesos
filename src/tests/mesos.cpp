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
#include <set>
#include <string>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/master/detector.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#include "linux/fs.hpp"
#endif

#ifdef ENABLE_PORT_MAPPING_ISOLATOR
#include "linux/routing/utils.hpp"
#endif

#include "slave/constants.hpp"
#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using mesos::master::detector::MasterDetector;

using std::list;
using std::shared_ptr;
using std::string;
using std::vector;

using testing::_;

using namespace process;

#ifdef ENABLE_PORT_MAPPING_ISOLATOR
using namespace routing;
#endif

namespace mesos {
namespace internal {
namespace tests {

#ifdef MESOS_HAS_JAVA
ZooKeeperTestServer* MesosZooKeeperTest::server = nullptr;
Option<zookeeper::URL> MesosZooKeeperTest::url;
#endif // MESOS_HAS_JAVA

void MesosTest::SetUpTestCase()
{
  // We set the connection delay used by the scheduler library to 0.
  // This is done to speed up the tests.
  os::setenv("MESOS_CONNECTION_DELAY_MAX", "0ms");
}


void MesosTest::TearDownTestCase()
{
  os::unsetenv("MESOS_CONNECTION_DELAY_MAX");

  SSLTemporaryDirectoryTest::TearDownTestCase();
}


MesosTest::MesosTest(const Option<zookeeper::URL>& _zookeeperUrl)
  : zookeeperUrl(_zookeeperUrl) {}


master::Flags MesosTest::CreateMasterFlags()
{
  master::Flags flags;

  // We use the current working directory from TempDirectoryTest
  // to ensure the work directory remains the same within a test.
  flags.work_dir = path::join(os::getcwd(), "master");

  CHECK_SOME(os::mkdir(flags.work_dir.get()));

  flags.authenticate_http_readonly = true;
  flags.authenticate_http_readwrite = true;
  flags.authenticate_frameworks = true;
  flags.authenticate_agents = true;

  flags.authenticate_http_frameworks = true;
  flags.http_framework_authenticators = "basic";

  // Create a default credentials file.
  const string& path = path::join(os::getcwd(), "credentials");

  Try<int_fd> fd = os::open(
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

  // Use the in-memory registry (instead of the replicated log) by default.
  // TODO(josephw): Consider changing this back to `replicated_log` once
  // all platforms support this registrar backend.
  flags.registry = "in_memory";

  // On many test VMs, this default is too small.
  flags.registry_store_timeout = flags.registry_store_timeout * 5;

  flags.authenticators = tests::flags.authenticators;

  return flags;
}


slave::Flags MesosTest::CreateSlaveFlags()
{
  slave::Flags flags;

  // Create a temporary work directory (removed by Environment).
  Try<string> workDir = environment->mkdtemp();
  CHECK_SOME(workDir) << "Failed to create temporary directory";
  flags.work_dir = workDir.get();

  // Create a temporary runtime directory (removed by Environment).
  Try<string> runtimeDir = environment->mkdtemp();
  CHECK_SOME(runtimeDir) << "Failed to create temporary directory";
  flags.runtime_dir = runtimeDir.get();

  Try<string> agentDir = os::mkdtemp(path::join(sandbox.get(), "XXXXXX"));
  CHECK_SOME(agentDir) << "Failed to create temporary directory";

  flags.fetcher_cache_dir = path::join(agentDir.get(), "fetch");

  flags.launcher_dir = getLauncherDir();

  flags.appc_store_dir = path::join(agentDir.get(), "store", "appc");

  flags.docker_store_dir = path::join(agentDir.get(), "store", "docker");

  flags.frameworks_home = path::join(agentDir.get(), "frameworks");

  {
    // Create a default credential file for master/agent authentication.
    const string& path = path::join(agentDir.get(), "credential");

    Try<int_fd> fd = os::open(
        path,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP);

    CHECK_SOME(fd);

    Credential credential;
    credential.set_principal(DEFAULT_CREDENTIAL.principal());
    credential.set_secret(DEFAULT_CREDENTIAL.secret());

    CHECK_SOME(os::write(fd.get(), stringify(JSON::protobuf(credential))))
      << "Failed to write agent credential to '" << path << "'";

    CHECK_SOME(os::close(fd.get()));

    flags.credential = path;

    // Set default (permissive) ACLs.
    flags.acls = ACLs();
  }

  flags.authenticate_http_readonly = true;
  flags.authenticate_http_readwrite = true;

#ifdef USE_SSL_SOCKET
  // Executor authentication currently has SSL as a dependency, so we
  // cannot enable it if Mesos was not built with SSL support.
  flags.authenticate_http_executors = true;

  {
    // Create a secret key for executor authentication.
    const string path = path::join(agentDir.get(), "jwt_secret_key");

    Try<int_fd> fd = os::open(
        path,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP);

    CHECK_SOME(fd);

    CHECK_SOME(os::write(fd.get(), DEFAULT_JWT_SECRET_KEY))
      << "Failed to write executor secret key to '" << path << "'";

    CHECK_SOME(os::close(fd.get()));

    flags.jwt_secret_key = path;
  }
#else // USE_SSL_SOCKET
  // Disable operator API authentication for the default executor. Executor
  // authentication currently has SSL as a dependency, so we cannot require
  // executors to authenticate with the agent operator API if Mesos was not
  // built with SSL support.
  flags.authenticate_http_readwrite = false;
#endif // USE_SSL_SOCKET

  {
    // Create a default HTTP credentials file.
    const string& path = path::join(agentDir.get(), "http_credentials");

    Try<int_fd> fd = os::open(
        path,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP);

    CHECK_SOME(fd);

    Credentials httpCredentials;

    Credential* httpCredential = httpCredentials.add_credentials();
    httpCredential->set_principal(DEFAULT_CREDENTIAL.principal());
    httpCredential->set_secret(DEFAULT_CREDENTIAL.secret());

    httpCredential = httpCredentials.add_credentials();
    httpCredential->set_principal(DEFAULT_CREDENTIAL_2.principal());
    httpCredential->set_secret(DEFAULT_CREDENTIAL_2.secret());

    CHECK_SOME(os::write(fd.get(), stringify(JSON::protobuf(httpCredentials))))
      << "Failed to write HTTP credentials to '" << path << "'";

    CHECK_SOME(os::close(fd.get()));

    flags.http_credentials = path;
  }

  flags.resources = defaultAgentResourcesString;

  flags.registration_backoff_factor = Milliseconds(10);

  // Make sure that the slave uses the same 'docker' as the tests.
  flags.docker = tests::flags.docker;

  if (tests::flags.isolation.isSome()) {
    flags.isolation = tests::flags.isolation.get();
  }

  return flags;
}


Try<Owned<cluster::Master>> MesosTest::StartMaster(
    const Option<master::Flags>& flags)
{
  return cluster::Master::start(
      flags.isNone() ? CreateMasterFlags() : flags.get(),
      zookeeperUrl);
}


Try<Owned<cluster::Master>> MesosTest::StartMaster(
    mesos::allocator::Allocator* allocator,
    const Option<master::Flags>& flags)
{
  return cluster::Master::start(
      flags.isNone() ? CreateMasterFlags() : flags.get(),
      zookeeperUrl,
      allocator);
}


Try<Owned<cluster::Master>> MesosTest::StartMaster(
    Authorizer* authorizer,
    const Option<master::Flags>& flags)
{
  return cluster::Master::start(
      flags.isNone() ? CreateMasterFlags() : flags.get(),
      zookeeperUrl,
      None(),
      authorizer);
}


Try<Owned<cluster::Master>> MesosTest::StartMaster(
    const shared_ptr<MockRateLimiter>& slaveRemovalLimiter,
    const Option<master::Flags>& flags)
{
  return cluster::Master::start(
      flags.isNone() ? CreateMasterFlags() : flags.get(),
      zookeeperUrl,
      None(),
      None(),
      slaveRemovalLimiter);
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(const SlaveOptions& options)
{
  Try<Owned<cluster::Slave>> slave = cluster::Slave::create(
      options.detector,
      options.flags.isNone() ? CreateSlaveFlags() : options.flags.get(),
      options.id,
      options.containerizer,
      options.gc,
      options.taskStatusUpdateManager,
      options.resourceEstimator,
      options.qosController,
      options.secretGenerator,
      options.authorizer,
      options.futureTracker,
      options.csiServer,
      options.mock);

  if (slave.isSome() && !options.mock) {
    slave.get()->start();
  }

  return slave;
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    const Option<slave::Flags>& flags,
    bool mock)
{
  return StartSlave(SlaveOptions(detector, mock)
    .withFlags(flags));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    slave::Containerizer* containerizer,
    const Option<slave::Flags>& flags,
    bool mock)
{
  return StartSlave(SlaveOptions(detector, mock)
    .withFlags(flags)
    .withContainerizer(containerizer));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    const string& id,
    const Option<slave::Flags>& flags,
    bool mock)
{
  return StartSlave(SlaveOptions(detector, mock)
    .withFlags(flags)
    .withId(id));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    slave::Containerizer* containerizer,
    const string& id,
    const Option<slave::Flags>& flags)
{
  return StartSlave(SlaveOptions(detector)
    .withFlags(flags)
    .withId(id)
    .withContainerizer(containerizer));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    slave::GarbageCollector* gc,
    const Option<slave::Flags>& flags,
    bool mock)
{
  return StartSlave(SlaveOptions(detector, mock)
    .withFlags(flags)
    .withGc(gc));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    mesos::slave::ResourceEstimator* resourceEstimator,
    const Option<slave::Flags>& flags)
{
  return StartSlave(SlaveOptions(detector)
    .withFlags(flags)
    .withResourceEstimator(resourceEstimator));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    slave::Containerizer* containerizer,
    mesos::slave::ResourceEstimator* resourceEstimator,
    const Option<slave::Flags>& flags)
{
  return StartSlave(SlaveOptions(detector)
    .withFlags(flags)
    .withContainerizer(containerizer)
    .withResourceEstimator(resourceEstimator));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    mesos::slave::QoSController* qosController,
    const Option<slave::Flags>& flags)
{
  return StartSlave(SlaveOptions(detector)
    .withFlags(flags)
    .withQosController(qosController));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    MasterDetector* detector,
    slave::Containerizer* containerizer,
    mesos::slave::QoSController* qosController,
    const Option<slave::Flags>& flags,
    bool mock)
{
  return StartSlave(SlaveOptions(detector, mock)
    .withFlags(flags)
    .withContainerizer(containerizer)
    .withQosController(qosController));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    mesos::master::detector::MasterDetector* detector,
    mesos::Authorizer* authorizer,
    const Option<slave::Flags>& flags,
    bool mock)
{
  return StartSlave(SlaveOptions(detector, mock)
    .withFlags(flags)
    .withAuthorizer(authorizer));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    mesos::master::detector::MasterDetector* detector,
    slave::Containerizer* containerizer,
    mesos::Authorizer* authorizer,
    const Option<slave::Flags>& flags,
    bool mock)
{
  return StartSlave(SlaveOptions(detector, mock)
    .withFlags(flags)
    .withContainerizer(containerizer)
    .withAuthorizer(authorizer));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    mesos::master::detector::MasterDetector* detector,
    slave::Containerizer* containerizer,
    mesos::SecretGenerator* secretGenerator,
    const Option<mesos::Authorizer*>& authorizer,
    const Option<slave::Flags>& flags,
    bool mock)
{
  return StartSlave(SlaveOptions(detector, mock)
    .withFlags(flags)
    .withContainerizer(containerizer)
    .withSecretGenerator(secretGenerator)
    .withAuthorizer(authorizer));
}


Try<Owned<cluster::Slave>> MesosTest::StartSlave(
    mesos::master::detector::MasterDetector* detector,
    mesos::SecretGenerator* secretGenerator,
    const Option<slave::Flags>& flags)
{
  return StartSlave(SlaveOptions(detector)
    .withFlags(flags)
    .withSecretGenerator(secretGenerator));
}


// Although the constructors and destructors for mock classes are
// often trivial, defining them out-of-line (in a separate compilation
// unit) improves compilation time: see MESOS-3827.

MockScheduler::MockScheduler() {}


MockScheduler::~MockScheduler() {}


MockExecutor::MockExecutor(const ExecutorID& _id) : id(_id) {}


MockExecutor::~MockExecutor() {}


MockAuthorizer::MockAuthorizer()
{
  // NOTE: We use 'EXPECT_CALL' and 'WillRepeatedly' here instead of
  // 'ON_CALL' and 'WillByDefault'. See 'TestContainerizer::SetUp()'
  // for more details.
  EXPECT_CALL(*this, authorized(_))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, getApprover(_, _))
    .WillRepeatedly(Return(std::make_shared<AcceptingObjectApprover>()));
}


MockAuthorizer::~MockAuthorizer() {}


MockGarbageCollector::MockGarbageCollector(const string& workDir)
    : slave::GarbageCollector(workDir)
{
  EXPECT_CALL(*this, unschedule(_)).WillRepeatedly(Return(true));
}


MockGarbageCollector::~MockGarbageCollector() {}


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
    flags.cgroups_root =
      TEST_CGROUPS_ROOT + "_" + id::UUID::random().toString();
  } else {
    flags.isolation = "posix/cpu,posix/mem";
  }
#elif defined(__WINDOWS__)
  flags.isolation = "windows/cpu,windows/mem";
#else
  flags.isolation = "posix/cpu,posix/mem";
#endif

#ifdef ENABLE_PORT_MAPPING_ISOLATOR
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
  MesosTest::SetUpTestCase();

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

  Try<std::set<string>> supportedSubsystems = cgroups::subsystems();
  ASSERT_SOME(supportedSubsystems);

  subsystems = supportedSubsystems.get();

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
          // Cgroup destruction relies on `delay`s,
          // so we must ensure the clock is resumed.
          bool paused = Clock::paused();

          if (paused) {
            Clock::resume();
          }

          // Since we are tearing down the tests, kill any processes
          // that might remain. Any remaining zombie processes will
          // not prevent the destroy from succeeding.
          EXPECT_SOME(cgroups::kill(hierarchy, cgroup, SIGKILL));
          AWAIT_READY(cgroups::destroy(hierarchy, cgroup));

          if (paused) {
            Clock::pause();
          }
        }
      }
    }
  }
}
#endif // __linux__


string ParamDiskQuota::Printer::operator()(
  const ::testing::TestParamInfo<ParamDiskQuota::Type>& info) const
{
  switch (info.param) {
    case SANDBOX:
      return "Sandbox";
    case ROOTFS:
      return "Rootfs";
    default:
      UNREACHABLE();
  }
}


vector<ParamDiskQuota::Type> ParamDiskQuota::parameters()
{
  vector<Type> params{SANDBOX};

  // ROOTFS tests depend on overlayfs being available, since that is
  // the only provisioner backend that supports ephemeral volumes.
#if __linux__
  Try<bool> overlayfsSupported = fs::supported("overlayfs");
  if (overlayfsSupported.isSome() && overlayfsSupported.get()) {
    params.push_back(ROOTFS);
  }
#endif

  return params;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
