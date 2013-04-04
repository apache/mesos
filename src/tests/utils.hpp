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

#ifndef __TESTS_UTILS_HPP__
#define __TESTS_UTILS_HPP__

#include <unistd.h> // For usleep.

#include <gmock/gmock.h>

#include <fstream>
#include <map>
#include <string>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "common/resources.hpp"
#include "common/type_utils.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif

#include "logging/logging.hpp"

#include "master/allocator.hpp"
#include "master/drf_sorter.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "messages/messages.hpp"

#ifdef __linux__
#include "slave/cgroups_isolator.hpp"
#endif
#include "slave/isolator.hpp"
#include "slave/reaper.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "tests/flags.hpp"

namespace mesos {
namespace internal {
namespace tests {

// Flags used to run the tests.
extern flags::Flags<logging::Flags, Flags> flags;


#ifdef __linux__
// Cgroups hierarchy used by the cgroups related tests.
const static std::string TEST_CGROUPS_HIERARCHY = "/tmp/mesos_test_cgroup";

// Name of the root cgroup used by the cgroups related tests.
const static std::string TEST_CGROUPS_ROOT = "mesos_test";
#endif


// Helper to create a temporary directory based on the current test
// case name and test name (derived from TestInfo via
// ::testing::UnitTest::GetInstance()->current_test_info()).
Try<std::string> mkdtemp();


// Test fixture for creating a temporary directory for each test.
class TemporaryDirectoryTest : public ::testing::Test
{
protected:
  virtual void SetUp();
  virtual void TearDown();

private:
  std::string cwd;
  Option<std::string> sandbox;
};


// Test fixture to setup mesos configuration for tests.
// This fixture creates a new temporary working directory for each test and
// deletes it when a test finishes. It also supports setting slave resources.
// TODO(vinod): Make this fixture more generic to provide a basic set of
// setup abstractions for tests.
class MesosTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    // Create a temporary directory for the test.
    Try<std::string> directory = mkdtemp();

    CHECK(directory.isSome())
      << "Failed to create temporary directory: " << directory.error();

    slaveFlags.work_dir = directory.get();
    slaveFlags.launcher_dir = path::join(tests::flags.build_dir, "src");

    // For locating killtree.sh.
    os::setenv("MESOS_SOURCE_DIR", tests::flags.source_dir);

    setSlaveResources("cpus:2;mem:1024;disk:1024;ports:[31000-32000]");
  }

  virtual void TearDown()
  {
    os::rmdir(slaveFlags.work_dir);

    os::unsetenv("MESOS_SOURCE_DIR");
  }

  void setSlaveResources(const std::string& resources)
  {
    slaveFlags.resources = Option<std::string>::some(resources);
  }

  slave::Flags slaveFlags;
};


template <typename T>
class IsolatorTest : public MesosTest
{};


#ifdef __linux__
template <>
class IsolatorTest<slave::CgroupsIsolator> : public MesosTest
{
public:
  static void SetUpTestCase()
  {
    // Clean up the testing hierarchy, in case it wasn't cleaned up
    // properly from previous tests.
    ASSERT_FUTURE_WILL_SUCCEED(cgroups::cleanup(TEST_CGROUPS_HIERARCHY));
  }

  static void TearDownTestCase()
  {
    ASSERT_FUTURE_WILL_SUCCEED(cgroups::cleanup(TEST_CGROUPS_HIERARCHY));
  }

protected:
  virtual void SetUp()
  {
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

    MesosTest::SetUp();

    // Set slave's cgroup flags.
    slaveFlags.cgroups_hierarchy = hierarchy;
    slaveFlags.cgroups_root = TEST_CGROUPS_ROOT;
  }

  virtual void TearDown()
  {
    MesosTest::TearDown();

    Try<bool> exists = cgroups::exists(hierarchy, TEST_CGROUPS_ROOT);
    ASSERT_SOME(exists);
    if (exists.get()) {
     ASSERT_FUTURE_WILL_SUCCEED(cgroups::destroy(hierarchy, TEST_CGROUPS_ROOT));
    }
  }

private:
  std::string hierarchy;
};
#endif


// Macros to get/create (default) ExecutorInfos and FrameworkInfos.
#define DEFAULT_EXECUTOR_INFO                                           \
      ({ ExecutorInfo executor;                                         \
        executor.mutable_executor_id()->set_value("default");           \
        executor.mutable_command()->set_value("exit 1");                \
        executor; })


#define CREATE_EXECUTOR_INFO(executorId, command)                       \
      ({ ExecutorInfo executor;                                         \
        executor.mutable_executor_id()->MergeFrom(executorId);          \
        executor.mutable_command()->set_value(command);                 \
        executor; })


#define DEFAULT_FRAMEWORK_INFO                                          \
     ({ FrameworkInfo framework;                                        \
        framework.set_name("default");                                  \
        framework; })


#define DEFAULT_EXECUTOR_ID						\
      DEFAULT_EXECUTOR_INFO.executor_id()


inline TaskInfo createTask(
    const Offer& offer,
    const std::string& command,
    const std::string& name = "test-task",
    const std::string& id = UUID::random().toString())
{
  TaskInfo task;
  task.set_name(name);
  task.mutable_task_id()->set_value(id);
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_command()->set_value(command);

  return task;
}


// Definition of a mock Scheduler to be used in tests with gmock.
class MockScheduler : public Scheduler
{
public:
  MOCK_METHOD3(registered, void(SchedulerDriver*,
                                const FrameworkID&,
                                const MasterInfo&));
  MOCK_METHOD2(reregistered, void(SchedulerDriver*, const MasterInfo&));
  MOCK_METHOD1(disconnected, void(SchedulerDriver*));
  MOCK_METHOD2(resourceOffers, void(SchedulerDriver*,
                                    const std::vector<Offer>&));
  MOCK_METHOD2(offerRescinded, void(SchedulerDriver*, const OfferID&));
  MOCK_METHOD2(statusUpdate, void(SchedulerDriver*, const TaskStatus&));
  MOCK_METHOD4(frameworkMessage, void(SchedulerDriver*,
                                      const ExecutorID&,
                                      const SlaveID&,
                                      const std::string&));
  MOCK_METHOD2(slaveLost, void(SchedulerDriver*, const SlaveID&));
  MOCK_METHOD4(executorLost, void(SchedulerDriver*,
                                  const ExecutorID&,
                                  const SlaveID&,
                                  int));
  MOCK_METHOD2(error, void(SchedulerDriver*, const std::string&));
};

// For use with a MockScheduler, for example:
// EXPECT_CALL(sched, resourceOffers(_, _))
//   .WillOnce(LaunchTasks(TASKS, CPUS, MEM));
// Launches up to TASKS no-op tasks, if possible,
// each with CPUS cpus and MEM memory.
ACTION_P3(LaunchTasks, tasks, cpus, mem)
{
  SchedulerDriver* driver = arg0;
  std::vector<Offer> offers = arg1;
  int numTasks = tasks;

  int launched = 0;
  for (size_t i = 0; i < offers.size(); i++) {
    const Offer& offer = offers[i];
    double offeredCpus = 0;
    double offeredMem = 0;

    for (int j = 0; j < offer.resources_size(); j++) {
      const Resource& resource = offer.resources(j);
      if (resource.name() == "cpus" &&
	        resource.type() == Value::SCALAR) {
	      offeredCpus = resource.scalar().value();
      } else if (resource.name() == "mem" &&
		             resource.type() == Value::SCALAR) {
	      offeredMem = resource.scalar().value();
      }
    }

    int nextTaskId = 0;
    std::vector<TaskInfo> tasks;

    while (offeredCpus >= cpus && offeredMem >= mem && launched < numTasks) {
      TaskInfo task;
      task.set_name("TestTask");
      task.mutable_task_id()->set_value(stringify(nextTaskId++));
      task.mutable_slave_id()->MergeFrom(offer.slave_id());

      ExecutorInfo executor;
      executor.mutable_executor_id()->set_value("default");
      executor.mutable_command()->set_value(":");
      task.mutable_executor()->MergeFrom(executor);

      Resource* resource;
      resource = task.add_resources();
      resource->set_name("cpus");
      resource->set_type(Value::SCALAR);
      resource->mutable_scalar()->set_value(cpus);

      resource = task.add_resources();
      resource->set_name("mem");
      resource->set_type(Value::SCALAR);
      resource->mutable_scalar()->set_value(mem);

      tasks.push_back(task);
      launched++;
      offeredCpus -= cpus;
      offeredMem -= mem;
    }

    driver->launchTasks(offer.id(), tasks);
  }
}


// Like LaunchTasks, but decline the entire offer and
// don't launch any tasks.
ACTION(DeclineOffers)
{
  SchedulerDriver* driver = arg0;
  std::vector<Offer> offers = arg1;

  for (size_t i = 0; i < offers.size(); i++) {
    driver->declineOffer(offers[i].id());
  }
}


// Definition of a mock Executor to be used in tests with gmock.
class MockExecutor : public Executor
{
public:
  MOCK_METHOD4(registered, void(ExecutorDriver*,
                                const ExecutorInfo&,
                                const FrameworkInfo&,
                                const SlaveInfo&));
  MOCK_METHOD2(reregistered, void(ExecutorDriver*, const SlaveInfo&));
  MOCK_METHOD1(disconnected, void(ExecutorDriver*));
  MOCK_METHOD2(launchTask, void(ExecutorDriver*, const TaskInfo&));
  MOCK_METHOD2(killTask, void(ExecutorDriver*, const TaskID&));
  MOCK_METHOD2(frameworkMessage, void(ExecutorDriver*, const std::string&));
  MOCK_METHOD1(shutdown, void(ExecutorDriver*));
  MOCK_METHOD2(error, void(ExecutorDriver*, const std::string&));
};


class MockProcessListener : public slave::ProcessExitedListener
{
public:
  MOCK_METHOD2(processExited, void(pid_t, int));
};


template <typename T = master::AllocatorProcess>
class MockAllocatorProcess : public master::AllocatorProcess
{
public:
  MockAllocatorProcess()
  {
    // Spawn the underlying allocator process.
    process::spawn(real);

    using ::testing::_;

    ON_CALL(*this, initialize(_, _))
      .WillByDefault(InvokeInitialize(this));

    ON_CALL(*this, frameworkAdded(_, _, _))
      .WillByDefault(InvokeFrameworkAdded(this));

    ON_CALL(*this, frameworkRemoved(_))
      .WillByDefault(InvokeFrameworkRemoved(this));

    ON_CALL(*this, frameworkActivated(_, _))
      .WillByDefault(InvokeFrameworkActivated(this));

    ON_CALL(*this, frameworkDeactivated(_))
      .WillByDefault(InvokeFrameworkDeactivated(this));

    ON_CALL(*this, slaveAdded(_, _, _))
      .WillByDefault(InvokeSlaveAdded(this));

    ON_CALL(*this, slaveRemoved(_))
      .WillByDefault(InvokeSlaveRemoved(this));

    ON_CALL(*this, updateWhitelist(_))
      .WillByDefault(InvokeUpdateWhitelist(this));

    ON_CALL(*this, resourcesRequested(_, _))
      .WillByDefault(InvokeResourcesRequested(this));

    ON_CALL(*this, resourcesUnused(_, _, _, _))
      .WillByDefault(InvokeResourcesUnused(this));

    ON_CALL(*this, resourcesRecovered(_, _, _))
      .WillByDefault(InvokeResourcesRecovered(this));

    ON_CALL(*this, offersRevived(_))
      .WillByDefault(InvokeOffersRevived(this));
  }

  ~MockAllocatorProcess()
  {
    process::terminate(real);
    process::wait(real);
  }

  MOCK_METHOD2(initialize, void(const master::Flags&,
                                const process::PID<master::Master>&));
  MOCK_METHOD3(frameworkAdded, void(const FrameworkID&,
                                    const FrameworkInfo&,
                                    const Resources&));
  MOCK_METHOD1(frameworkRemoved, void(const FrameworkID&));
  MOCK_METHOD2(frameworkActivated, void(const FrameworkID&,
                                        const FrameworkInfo&));
  MOCK_METHOD1(frameworkDeactivated, void(const FrameworkID&));
  MOCK_METHOD3(slaveAdded, void(const SlaveID&,
                                const SlaveInfo&,
                                const hashmap<FrameworkID, Resources>&));
  MOCK_METHOD1(slaveRemoved, void(const SlaveID&));
  MOCK_METHOD1(updateWhitelist, void(const Option<hashset<std::string> >&));
  MOCK_METHOD2(resourcesRequested, void(const FrameworkID&,
                                        const std::vector<Request>&));
  MOCK_METHOD4(resourcesUnused, void(const FrameworkID&,
                                     const SlaveID&,
                                     const Resources&,
                                     const Option<Filters>& filters));
  MOCK_METHOD3(resourcesRecovered, void(const FrameworkID&,
                                        const SlaveID&,
                                        const Resources&));
  MOCK_METHOD1(offersRevived, void(const FrameworkID&));

  T real;
};


typedef ::testing::Types<master::HierarchicalDRFAllocatorProcess>
AllocatorTypes;


// The following actions make up for the fact that DoDefault
// cannot be used inside a DoAll, for example:
// EXPECT_CALL(allocator, frameworkAdded(_, _, _))
//   .WillOnce(DoAll(InvokeFrameworkAdded(&allocator),
//                   Trigger(&frameworkAddedTrigger)));

ACTION_P(InvokeInitialize, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::initialize,
      arg0,
      arg1);
}


ACTION_P(InvokeFrameworkAdded, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::frameworkAdded,
      arg0,
      arg1,
      arg2);
}


ACTION_P(InvokeFrameworkRemoved, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::frameworkRemoved, arg0);
}


ACTION_P(InvokeFrameworkActivated, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::frameworkActivated,
      arg0,
      arg1);
}


ACTION_P(InvokeFrameworkDeactivated, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::frameworkDeactivated,
      arg0);
}


ACTION_P(InvokeSlaveAdded, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::slaveAdded,
      arg0,
      arg1,
      arg2);
}


ACTION_P(InvokeSlaveRemoved, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::slaveRemoved,
      arg0);
}


ACTION_P(InvokeUpdateWhitelist, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::updateWhitelist,
      arg0);
}


ACTION_P(InvokeResourcesRequested, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::resourcesRequested,
      arg0,
      arg1);
}



ACTION_P(InvokeResourcesUnused, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::resourcesUnused,
      arg0,
      arg1,
      arg2,
      arg3);
}


ACTION_P2(InvokeUnusedWithFilters, allocator, timeout)
{
  Filters filters;
  filters.set_refuse_seconds(timeout);

  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::resourcesUnused,
      arg0,
      arg1,
      arg2,
      filters);
}


ACTION_P(InvokeResourcesRecovered, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::resourcesRecovered,
      arg0,
      arg1,
      arg2);
}


ACTION_P(InvokeOffersRevived, allocator)
{
  process::dispatch(
      allocator->real,
      &master::AllocatorProcess::offersRevived,
      arg0);
}


class OfferEqMatcher
  : public ::testing::MatcherInterface<const std::vector<Offer>& >
{
public:
  OfferEqMatcher(int _cpus, int _mem)
    : cpus(_cpus), mem(_mem) {}

  virtual bool MatchAndExplain(const std::vector<Offer>& offers,
			       ::testing::MatchResultListener* listener) const
  {
    double totalCpus = 0;
    double totalMem = 0;

    foreach (const Offer& offer, offers) {
      foreach (const Resource& resource, offer.resources()) {
	if (resource.name() == "cpus") {
	  totalCpus += resource.scalar().value();
	} else if (resource.name() == "mem") {
	  totalMem += resource.scalar().value();
	}
      }
    }

    bool matches = totalCpus == cpus && totalMem == mem;

    if (!matches) {
      *listener << totalCpus << " cpus and " << totalMem << "mem";
    }

    return matches;
  }

  virtual void DescribeTo(::std::ostream* os) const
  {
    *os << "contains " << cpus << " cpus and " << mem << " mem";
  }

  virtual void DescribeNegationTo(::std::ostream* os) const
  {
    *os << "does not contain " << cpus << " cpus and "  << mem << " mem";
  }

private:
  int cpus;
  int mem;
};


inline const ::testing::Matcher<const std::vector<Offer>& > OfferEq(int cpus, int mem)
{
  return MakeMatcher(new OfferEqMatcher(cpus, mem));
}


ACTION_TEMPLATE(SaveArgField,
                HAS_1_TEMPLATE_PARAMS(int, k),
                AND_2_VALUE_PARAMS(field, pointer))
{
  *pointer = *(::std::tr1::get<k>(args).*field);
}


// A trigger is an object that can be used to effectively block a test
// from proceeding until some event has occured. A trigger can get set
// using a gmock action (see below) and you can wait for a trigger to
// occur using the WAIT_UNTIL macro below.
struct trigger
{
  trigger() : value(false) {}
  operator bool () const { return value; }
  bool value;
};


// Definition of the Trigger action to be used with gmock.
ACTION_P(Trigger, trigger)
{
  trigger->value = true;
}


// Definition of an 'increment' action to be used with gmock.
ACTION_P(Increment, variable)
{
  *variable = *variable + 1;
}


// Definition of a 'decrement' action to be used with gmock.
ACTION_P(Decrement, variable)
{
  *variable = *variable - 1;
}


// Definition of the SendStatusUpdateFromTask action to be used with gmock.
ACTION_P(SendStatusUpdateFromTask, state)
{
  TaskStatus status;
  status.mutable_task_id()->MergeFrom(arg1.task_id());
  status.set_state(state);
  arg0->sendStatusUpdate(status);
}


// Definition of the SendStatusUpdateFromTaskID action to be used with gmock.
ACTION_P(SendStatusUpdateFromTaskID, state)
{
  TaskStatus status;
  status.mutable_task_id()->MergeFrom(arg1);
  status.set_state(state);
  arg0->sendStatusUpdate(status);
}


// These macros can be used to wait until some expression evaluates to true.
#define WAIT_FOR(expression, duration)                                  \
  do {                                                                  \
    unsigned int sleeps = 0;                                            \
    do {                                                                \
      __sync_synchronize();                                             \
      if (expression) {                                                 \
        break;                                                          \
      }                                                                 \
      usleep(10);                                                       \
      sleeps++;                                                         \
      if (Microseconds(10 * sleeps) >= duration) {                      \
        FAIL() << "Waited too long for '" #expression "'";              \
        ::exit(-1); /* TODO(benh): Figure out how not to exit! */       \
        break;                                                          \
      }                                                                 \
    } while (true);                                                     \
  } while (false)


#define WAIT_UNTIL(expression)                  \
  WAIT_FOR(expression, Seconds(2.0))


class TestingIsolator : public slave::Isolator
{
public:
  TestingIsolator()
  {
    setup();
  }

  TestingIsolator(const std::map<ExecutorID, Executor*>& _executors)
    : executors(_executors)
  {
    setup();
  }

  TestingIsolator(const ExecutorID& executorId, Executor* executor)
  {
    executors[executorId] = executor;
    setup();
  }

  virtual ~TestingIsolator() {}

  virtual void initialize(
      const slave::Flags& flags,
      const Resources& resources,
      bool local,
      const process::PID<slave::Slave>& _slave)
  {
    slave = _slave;
  }

  virtual void launchExecutor(
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const UUID& uuid,
      const std::string& directory,
      const Resources& resources,
      const Option<std::string>& path)
  {
    if (executors.count(executorInfo.executor_id()) > 0) {
      Executor* executor = executors[executorInfo.executor_id()];
      MesosExecutorDriver* driver = new MesosExecutorDriver(executor);
      drivers[executorInfo.executor_id()] = driver;

      directories[executorInfo.executor_id()] = directory;

      os::setenv("MESOS_LOCAL", "1");
      os::setenv("MESOS_DIRECTORY", directory);
      os::setenv("MESOS_SLAVE_PID", slave);
      os::setenv("MESOS_SLAVE_ID", slaveId.value());
      os::setenv("MESOS_FRAMEWORK_ID", frameworkId.value());
      os::setenv("MESOS_EXECUTOR_ID", executorInfo.executor_id().value());
      os::setenv("MESOS_CHECKPOINT", frameworkInfo.checkpoint() ? "1" : "0");

      driver->start();

      os::unsetenv("MESOS_LOCAL");
      os::unsetenv("MESOS_DIRECTORY");
      os::unsetenv("MESOS_SLAVE_PID");
      os::unsetenv("MESOS_SLAVE_ID");
      os::unsetenv("MESOS_FRAMEWORK_ID");
      os::unsetenv("MESOS_EXECUTOR_ID");
      os::unsetenv("MESOS_CHECKPOINT");

      process::dispatch(
          slave,
          &slave::Slave::executorStarted,
          frameworkId,
          executorInfo.executor_id(),
          -1);

    } else {
      FAIL() << "Cannot launch executor";
    }
  }

  virtual void killExecutor(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId)
  {
    if (drivers.count(executorId) > 0) {
      MesosExecutorDriver* driver = drivers[executorId];
      driver->stop();
      driver->join();
      delete driver;
      drivers.erase(executorId);

      process::dispatch(
          slave,
          &slave::Slave::executorTerminated,
          frameworkId,
          executorId,
          0,
          false,
          "Killed executor");
    } else {
      FAIL() << "Cannot kill executor";
    }
  }

  // Mocked so tests can check that the resources reflect all started tasks.
  MOCK_METHOD3(resourcesChanged, void(const FrameworkID&,
                                      const ExecutorID&,
                                      const Resources&));

  MOCK_METHOD2(
      usage,
      process::Future<ResourceStatistics>(
          const FrameworkID&,
          const ExecutorID&));

  MOCK_METHOD1(
      recover,
      process::Future<Nothing>(const Option<slave::state::SlaveState>&));

  std::map<ExecutorID, std::string> directories;

private:
  // Helper to setup default expectations.
  void setup()
  {
    EXPECT_CALL(*this, resourcesChanged(testing::_, testing::_, testing::_))
      .Times(testing::AnyNumber());

    EXPECT_CALL(*this, usage(testing::_, testing::_))
      .WillRepeatedly(testing::Return(ResourceStatistics()));

    EXPECT_CALL(*this, recover(testing::_))
      .WillRepeatedly(testing::Return(Nothing()));
  }

  std::map<ExecutorID, Executor*> executors;
  std::map<ExecutorID, MesosExecutorDriver*> drivers;
  process::PID<slave::Slave> slave;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_UTILS_HPP__
