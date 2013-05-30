#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/isolator.hpp"
#include "tests/mesos.hpp"


namespace mesos {
namespace internal {
namespace tests {

MesosTest::MesosTest(const Option<zookeeper::URL>& url) : cluster(url) {}


master::Flags MesosTest::CreateMasterFlags()
{
  return master::Flags();
}


slave::Flags MesosTest::CreateSlaveFlags()
{
  slave::Flags flags;

  // Create a temporary work directory (removed by Environment).
  Try<std::string> directory = environment->mkdtemp();
  CHECK_SOME(directory) << "Failed to create temporary directory";

  flags.work_dir = directory.get();

  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  flags.resources = Option<std::string>(
      "cpus:2;mem:1024;disk:1024;ports:[31000-32000]");

  return flags;
}


Try<process::PID<master::Master> > MesosTest::StartMaster(
    const Option<master::Flags>& flags)
{
  return cluster.masters.start(
      flags.isNone() ? CreateMasterFlags() : flags.get());
}


Try<process::PID<master::Master> > MesosTest::StartMaster(
    master::allocator::AllocatorProcess* allocator,
    const Option<master::Flags>& flags)
{
  return cluster.masters.start(
      allocator, flags.isNone() ? CreateMasterFlags() : flags.get());
}


Try<process::PID<slave::Slave> > MesosTest::StartSlave(
    const Option<slave::Flags>& flags)
{
  TestingIsolator* isolator = new TestingIsolator();

  Try<process::PID<slave::Slave> > pid = StartSlave(isolator, flags);

  if (pid.isError()) {
    delete isolator;
    return pid;
  }

  isolators[pid.get()] = isolator;

  return pid;
}


Try<process::PID<slave::Slave> > MesosTest::StartSlave(
    MockExecutor* executor,
    const Option<slave::Flags>& flags)
{
  TestingIsolator* isolator = new TestingIsolator(executor);
    
  Try<process::PID<slave::Slave> > pid = StartSlave(isolator, flags);

  if (pid.isError()) {
    delete isolator;
    return pid;
  }

  isolators[pid.get()] = isolator;

  return pid;
}


Try<process::PID<slave::Slave> > MesosTest::StartSlave(
    slave::Isolator* isolator,
    const Option<slave::Flags>& flags)
{
  return cluster.slaves.start(
      isolator, flags.isNone() ? CreateSlaveFlags() : flags.get());
}


void MesosTest::Stop(const process::PID<master::Master>& pid)
{
  cluster.masters.stop(pid);
}


void MesosTest::Stop(const process::PID<slave::Slave>& pid, bool shutdown)
{
  cluster.slaves.stop(pid, shutdown);
  if (isolators.count(pid) > 0) {
    TestingIsolator* isolator = isolators[pid];
    isolators.erase(pid);
    delete isolator;
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

  foreachvalue (TestingIsolator* isolator, isolators) {
    delete isolator;
  }
  isolators.clear();
}


void MesosTest::SetUp()
{
  // For locating killtree.sh.
  os::setenv("MESOS_SOURCE_DIR", tests::flags.source_dir);
}


void MesosTest::TearDown()
{
  os::unsetenv("MESOS_SOURCE_DIR");

  // TODO(benh): Fail the test if shutdown hasn't been called?
  Shutdown();
}


#ifdef __linux__
void IsolatorTest<slave::CgroupsIsolator>::SetUpTestCase()
{
  // Clean up the testing hierarchy, in case it wasn't cleaned up
  // properly from previous tests.
  AWAIT_READY(cgroups::cleanup(TEST_CGROUPS_HIERARCHY));
}


void IsolatorTest<slave::CgroupsIsolator>::TearDownTestCase()
{
  AWAIT_READY(cgroups::cleanup(TEST_CGROUPS_HIERARCHY));
}


slave::Flags IsolatorTest<slave::CgroupsIsolator>::CreateSlaveFlags()
{
  slave::Flags flags = MesosTest::CreateSlaveFlags();

  flags.cgroups_hierarchy = hierarchy;

  // TODO(benh): Create a different cgroups root for each slave.
  flags.cgroups_root = TEST_CGROUPS_ROOT;

  return flags;
}


void IsolatorTest<slave::CgroupsIsolator>::SetUp()
{
  MesosTest::SetUp();

  const std::string subsystems = "cpu,cpuacct,memory,freezer";
  Result<std::string> hierarchy_ = cgroups::hierarchy(subsystems);
  ASSERT_FALSE(hierarchy_.isError());
  if (hierarchy_.isNone()) {
    // Try to mount a hierarchy for testing.
    ASSERT_SOME(cgroups::mount(TEST_CGROUPS_HIERARCHY, subsystems))
      << "-------------------------------------------------------------\n"
      << "We cannot run any cgroups tests that require\n"
      << "a hierarchy with subsystems '" << subsystems << "'\n"
      << "because we failed to find an existing hierarchy\n"
      << "or create a new one. You can either remove all existing\n"
      << "hierarchies, or disable this test case\n"
      << "(i.e., --gtest_filter=-"
      << ::testing::UnitTest::GetInstance()
           ->current_test_info()
           ->test_case_name() << ".*).\n"
      << "-------------------------------------------------------------";

    hierarchy = TEST_CGROUPS_HIERARCHY;
  } else {
    hierarchy = hierarchy_.get();
  }
}


void IsolatorTest<slave::CgroupsIsolator>::TearDown()
{
  MesosTest::TearDown();

  Try<bool> exists = cgroups::exists(hierarchy, TEST_CGROUPS_ROOT);
  ASSERT_SOME(exists);
  if (exists.get()) {
    AWAIT_READY(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));
  }
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
