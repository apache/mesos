#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/uuid.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/mesos_containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using namespace process;

using testing::_;

namespace mesos {
namespace internal {
namespace tests {

MesosTest::MesosTest(const Option<zookeeper::URL>& url) : cluster(url) {}


master::Flags MesosTest::CreateMasterFlags()
{
  master::Flags flags;

  // Enable authentication.
  flags.authenticate = true;

  // Create a temporary work directory (removed by Environment).
  const Try<std::string>& directory = environment->mkdtemp();
  CHECK_SOME(directory) << "Failed to create temporary directory";

  flags.work_dir = directory.get();

  // Create a default credentials file.
  const std::string& path = path::join(directory.get(), "credentials");

  const std::string& credentials =
    DEFAULT_CREDENTIAL.principal() + " " + DEFAULT_CREDENTIAL.secret();

  CHECK_SOME(os::write(path, credentials))
    << "Failed to write credentials to '" << path << "'";

  flags.credentials = "file://" + path;

  return flags;
}


slave::Flags MesosTest::CreateSlaveFlags()
{
  slave::Flags flags;

  // Create a temporary work directory (removed by Environment).
  Try<std::string> directory = environment->mkdtemp();
  CHECK_SOME(directory) << "Failed to create temporary directory";

  flags.work_dir = directory.get();

  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  // TODO(vinod): Consider making this true and fixing the tests.
  flags.checkpoint = false;

  flags.resources = Option<std::string>(
      "cpus:2;mem:1024;disk:1024;ports:[31000-32000]");

  return flags;
}


Try<process::PID<master::Master> > MesosTest::StartMaster(
    const Option<master::Flags>& flags,
    bool wait)
{
  Future<Nothing> detected = FUTURE_DISPATCH(_, &master::Master::detected);

  Try<process::PID<master::Master> > master = cluster.masters.start(
      flags.isNone() ? CreateMasterFlags() : flags.get());

  // Wait until the leader is detected because otherwise this master
  // may reject authentication requests because it doesn't know it's
  // the leader yet [MESOS-881].
  if (wait && master.isSome() && !detected.await(Seconds(10))) {
    return Error("Failed to wait " + stringify(Seconds(10)) +
                 " for master to detect the leader");
  }

  return master;
}


Try<process::PID<master::Master> > MesosTest::StartMaster(
    master::allocator::AllocatorProcess* allocator,
    const Option<master::Flags>& flags,
    bool wait)
{
  Future<Nothing> detected = FUTURE_DISPATCH(_, &master::Master::detected);

  Try<process::PID<master::Master> > master = cluster.masters.start(
      allocator, flags.isNone() ? CreateMasterFlags() : flags.get());

  // Wait until the leader is detected because otherwise this master
  // may reject authentication requests because it doesn't know it's
  // the leader yet [MESOS-881].
  if (wait && master.isSome() && !detected.await(Seconds(10))) {
    return Error("Failed to wait " + stringify(Seconds(10)) +
                 " for master to detect the leader");
  }

  return master;
}


Try<process::PID<slave::Slave> > MesosTest::StartSlave(
    const Option<slave::Flags>& flags)
{
  slave::Containerizer* containerizer = new TestContainerizer();

  Try<process::PID<slave::Slave> > pid = StartSlave(containerizer, flags);

  if (pid.isError()) {
    delete containerizer;
    return pid;
  }

  containerizers[pid.get()] = containerizer;

  return pid;
}


Try<process::PID<slave::Slave> > MesosTest::StartSlave(
    MockExecutor* executor,
    const Option<slave::Flags>& flags)
{
  slave::Containerizer* containerizer = new TestContainerizer(executor);

  Try<process::PID<slave::Slave> > pid = StartSlave(containerizer, flags);

  if (pid.isError()) {
    delete containerizer;
    return pid;
  }

  containerizers[pid.get()] = containerizer;

  return pid;
}


Try<process::PID<slave::Slave> > MesosTest::StartSlave(
    slave::Containerizer* containerizer,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      containerizer, flags.isNone() ? CreateSlaveFlags() : flags.get());
}


Try<process::PID<slave::Slave> > MesosTest::StartSlave(
    slave::Containerizer* containerizer,
    Owned<MasterDetector> detector,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      containerizer,
      detector,
      flags.isNone() ? CreateSlaveFlags() : flags.get());
}


Try<PID<slave::Slave> > MesosTest::StartSlave(
    Owned<MasterDetector> detector,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      detector, flags.isNone() ? CreateSlaveFlags() : flags.get());
}


Try<PID<slave::Slave> > MesosTest::StartSlave(
    MockExecutor* executor,
    Owned<MasterDetector> detector,
    const Option<slave::Flags>& flags)
{
  slave::Containerizer* containerizer = new TestContainerizer(executor);

  Try<process::PID<slave::Slave> > pid = cluster.slaves.start(
      containerizer,
      detector,
      flags.isNone() ? CreateSlaveFlags() : flags.get());

  if (pid.isError()) {
    delete containerizer;
    return pid;
  }

  containerizers[pid.get()] = containerizer;

  return pid;
}


void MesosTest::Stop(const process::PID<master::Master>& pid)
{
  cluster.masters.stop(pid);
}


void MesosTest::Stop(const process::PID<slave::Slave>& pid, bool shutdown)
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


void MesosTest::TearDown()
{
  // TODO(benh): Fail the test if shutdown hasn't been called?
  Shutdown();
}


slave::Flags ContainerizerTest<slave::MesosContainerizer>::CreateSlaveFlags()
{
  slave::Flags flags = MesosTest::CreateSlaveFlags();

#ifdef __linux__
  // Use cgroup isolators if they're available and we're root.
  // TODO(idownes): Refactor the cgroups/non-cgroups code.
  if (os::exists("/proc/cgroups") && os::user() == "root") {
    flags.isolation = "cgroups/cpu,cgroups/mem";
    flags.cgroups_hierarchy = baseHierarchy;
    flags.cgroups_root = TEST_CGROUPS_ROOT + "_" + UUID::random().toString();
  } else {
    flags.isolation = "posix/cpu,posix/mem";
  }
#else
  flags.isolation = "posix/cpu,posix/mem";
#endif

  return flags;
}


#ifdef __linux__
void ContainerizerTest<slave::MesosContainerizer>::SetUpTestCase()
{
  if (os::exists("/proc/cgroups") && os::user() == "root") {
    // Clean up any testing hierarchies.
    Try<std::set<std::string> > hierarchies = cgroups::hierarchies();
    ASSERT_SOME(hierarchies);
    foreach (const std::string& hierarchy, hierarchies.get()) {
      if (strings::startsWith(hierarchy, TEST_CGROUPS_HIERARCHY)) {
        AWAIT_READY(cgroups::cleanup(hierarchy));
      }
    }
  }
}


void ContainerizerTest<slave::MesosContainerizer>::TearDownTestCase()
{
  if (os::exists("/proc/cgroups") && os::user() == "root") {
    // Clean up any testing hierarchies.
    Try<std::set<std::string> > hierarchies = cgroups::hierarchies();
    ASSERT_SOME(hierarchies);
    foreach (const std::string& hierarchy, hierarchies.get()) {
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

  if (os::exists("/proc/cgroups") && os::user() == "root") {
    foreach (const std::string& subsystem, subsystems) {
      // Establish the base hierarchy if this is the first subsystem checked.
      if (baseHierarchy.empty()) {
        Result<std::string> hierarchy = cgroups::hierarchy(subsystem);
        ASSERT_FALSE(hierarchy.isError());

        if (hierarchy.isNone()) {
          baseHierarchy = TEST_CGROUPS_HIERARCHY;
        } else {
          // Strip the subsystem to get the base hierarchy.
          baseHierarchy = strings::remove(
              hierarchy.get(),
              subsystem,
              strings::SUFFIX);
        }
      }

      // Mount the subsystem if necessary.
      std::string hierarchy = path::join(baseHierarchy, subsystem);
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
      }
    }
  }
}


void ContainerizerTest<slave::MesosContainerizer>::TearDown()
{
  MesosTest::TearDown();

  if (os::exists("/proc/cgroups") && os::user() == "root") {
    foreach (const std::string& subsystem, subsystems) {
      std::string hierarchy = path::join(baseHierarchy, subsystem);

      Try<std::vector<std::string> > cgroups = cgroups::get(hierarchy);
      CHECK_SOME(cgroups);

      foreach (const std::string& cgroup, cgroups.get()) {
        // Remove any cgroups that start with TEST_CGROUPS_ROOT
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
