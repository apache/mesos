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

#ifndef __TESTS_MESOS_HPP__
#define __TESTS_MESOS_HPP__

#include <map>
#include <set>
#include <string>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp" // For google::protobuf::Message.

#include "master/allocator.hpp"
#include "master/detector.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/mesos_containerizer.hpp"
#include "slave/slave.hpp"

#include "tests/cluster.hpp"

namespace mesos {
namespace internal {
namespace tests {

// Forward declarations.
class MockExecutor;


class MesosTest : public ::testing::Test
{
protected:
  MesosTest(const Option<zookeeper::URL>& url = None());

  virtual void TearDown();

  // Returns the flags used to create masters.
  virtual master::Flags CreateMasterFlags();

  // Returns the flags used to create slaves.
  virtual slave::Flags CreateSlaveFlags();

  // Starts a master with the specified flags.
  // Waits for the master to detect a leader (could be itself) before
  // returning if 'wait' is set to true.
  // TODO(xujyan): Return a future which becomes ready when the
  // master detects a leader (when wait == true) and have the tests
  // do AWAIT_READY.
  virtual Try<process::PID<master::Master> > StartMaster(
      const Option<master::Flags>& flags = None(),
      bool wait = true);

  // Starts a master with the specified allocator process and flags.
  // Waits for the master to detect a leader (could be itself) before
  // returning if 'wait' is set to true.
  // TODO(xujyan): Return a future which becomes ready when the
  // master detects a leader (when wait == true) and have the tests
  // do AWAIT_READY.
  virtual Try<process::PID<master::Master> > StartMaster(
      master::allocator::AllocatorProcess* allocator,
      const Option<master::Flags>& flags = None(),
      bool wait = true);

  // Starts a slave with the specified flags.
  virtual Try<process::PID<slave::Slave> > StartSlave(
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified mock executor and flags.
  virtual Try<process::PID<slave::Slave> > StartSlave(
      MockExecutor* executor,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified containerizer and flags.
  virtual Try<process::PID<slave::Slave> > StartSlave(
      slave::Containerizer* containerizer,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified containerizer, detector and flags.
  virtual Try<process::PID<slave::Slave> > StartSlave(
      slave::Containerizer* containerizer,
      process::Owned<MasterDetector> detector,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified MasterDetector and flags.
  virtual Try<process::PID<slave::Slave> > StartSlave(
      process::Owned<MasterDetector> detector,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified mock executor, MasterDetector
  // and flags.
  virtual Try<process::PID<slave::Slave> > StartSlave(
      MockExecutor* executor,
      process::Owned<MasterDetector> detector,
      const Option<slave::Flags>& flags = None());

  // Stop the specified master.
  virtual void Stop(
      const process::PID<master::Master>& pid);

  // Stop the specified slave.
  virtual void Stop(
      const process::PID<slave::Slave>& pid,
      bool shutdown = false);

  // Stop all masters and slaves.
  virtual void Shutdown();

  // Stop all masters.
  virtual void ShutdownMasters();

  // Stop all slaves.
  virtual void ShutdownSlaves();

  Cluster cluster;

  // Containerizer(s) created during test that we need to cleanup.
  std::map<process::PID<slave::Slave>, slave::Containerizer*> containerizers;
};


template <typename T>
class ContainerizerTest : public MesosTest {};

#ifdef __linux__
// Cgroups hierarchy used by the cgroups related tests.
const static std::string TEST_CGROUPS_HIERARCHY = "/tmp/mesos_test_cgroup";

// Name of the root cgroup used by the cgroups related tests.
const static std::string TEST_CGROUPS_ROOT = "mesos_test";


template <>
class ContainerizerTest<slave::MesosContainerizer> : public MesosTest
{
public:
  static void SetUpTestCase();
  static void TearDownTestCase();

protected:
  virtual slave::Flags CreateSlaveFlags();
  virtual void SetUp();
  virtual void TearDown();

private:
  // Base hierarchy for separately mounted cgroup controllers, e.g., if the
  // base hierachy is /sys/fs/cgroup then each controller will be mounted to
  // /sys/fs/cgroup/{controller}/.
  std::string baseHierarchy;

  // Set of cgroup subsystems used by the cgroups related tests.
  hashset<std::string> subsystems;

};
#else
template<>
class ContainerizerTest<slave::MesosContainerizer> : public MesosTest
{
protected:
  virtual slave::Flags CreateSlaveFlags();
};
#endif // __linux__


// Macros to get/create (default) ExecutorInfos and FrameworkInfos.
#define DEFAULT_EXECUTOR_INFO                                           \
      ({ ExecutorInfo executor;                                         \
        executor.mutable_executor_id()->set_value("default");           \
        executor.mutable_command()->set_value("exit 1");                \
        executor; })


#define CREATE_EXECUTOR_INFO(executorId, command)                       \
      ({ ExecutorInfo executor;                                         \
        executor.mutable_executor_id()->set_value(executorId);          \
        executor.mutable_command()->set_value(command);                 \
        executor; })


#define DEFAULT_FRAMEWORK_INFO                                          \
     ({ FrameworkInfo framework;                                        \
        framework.set_name("default");                                  \
        framework; })


#define DEFAULT_CREDENTIAL                                             \
     ({ Credential credential;                                         \
        credential.set_principal("test-principal");                    \
        credential.set_secret("test-secret");                          \
        credential; })


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
//   .WillOnce(LaunchTasks(EXECUTOR, TASKS, CPUS, MEM, ROLE));
// Launches up to TASKS no-op tasks, if possible,
// each with CPUS cpus and MEM memory and EXECUTOR executor.
ACTION_P5(LaunchTasks, executor, tasks, cpus, mem, role)
{
  SchedulerDriver* driver = arg0;
  std::vector<Offer> offers = arg1;
  int numTasks = tasks;

  int launched = 0;
  for (size_t i = 0; i < offers.size(); i++) {
    const Offer& offer = offers[i];

    const Resources TASK_RESOURCES = Resources::parse(
        "cpus:" + stringify(cpus) + ";mem:" + stringify(mem)).get();

    int nextTaskId = 0;
    std::vector<TaskInfo> tasks;
    Resources remaining = offer.resources();

    while (TASK_RESOURCES <= remaining.flatten() && launched < numTasks) {
      TaskInfo task;
      task.set_name("TestTask");
      task.mutable_task_id()->set_value(stringify(nextTaskId++));
      task.mutable_slave_id()->MergeFrom(offer.slave_id());
      task.mutable_executor()->MergeFrom(executor);

      Option<Resources> resources = remaining.find(TASK_RESOURCES, role);
      CHECK_SOME(resources);
      task.mutable_resources()->MergeFrom(resources.get());
      remaining -= resources.get();

      tasks.push_back(task);
      launched++;
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
  MockExecutor(const ExecutorID& _id) : id(_id) {}

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

  const ExecutorID id;
};


class TestingMesosSchedulerDriver : public MesosSchedulerDriver
{
public:
  TestingMesosSchedulerDriver(
      Scheduler* scheduler,
      const FrameworkInfo& framework,
      const Credential& credential,
      MasterDetector* _detector)
    : MesosSchedulerDriver(scheduler, framework, "", credential)
  {
    detector = _detector;
  }

  // A constructor that uses the DEFAULT_FRAMEWORK_INFO &
  // DEFAULT_CREDENTIAL.
  TestingMesosSchedulerDriver(
      Scheduler* scheduler,
      MasterDetector* _detector)
    : MesosSchedulerDriver(
          scheduler,
          DEFAULT_FRAMEWORK_INFO,
          "",
          DEFAULT_CREDENTIAL)
  {
    detector = _detector;
  }

  ~TestingMesosSchedulerDriver()
  {
    // This is necessary because in the base class the detector is
    // internally created and deleted whereas in the testing driver
    // it is injected and thus should not be deleted in the
    // destructor. Setting it to null allows the detector to survive
    // MesosSchedulerDriver::~MesosSchedulerDriver().
    detector = NULL;
  }
};


template <typename T = master::allocator::AllocatorProcess>
class MockAllocatorProcess : public master::allocator::AllocatorProcess
{
public:
  MockAllocatorProcess()
  {
    // Spawn the underlying allocator process.
    process::spawn(real);

    using ::testing::_;

    ON_CALL(*this, initialize(_, _, _))
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

    ON_CALL(*this, slaveDisconnected(_))
      .WillByDefault(InvokeSlaveDisconnected(this));

    ON_CALL(*this, slaveReconnected(_))
      .WillByDefault(InvokeSlaveReconnected(this));

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

  MOCK_METHOD3(initialize, void(const master::Flags&,
                                const process::PID<master::Master>&,
                                const hashmap<std::string, RoleInfo>&));
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
  MOCK_METHOD1(slaveDisconnected, void(const SlaveID&));
  MOCK_METHOD1(slaveReconnected, void(const SlaveID&));
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


typedef ::testing::Types<master::allocator::HierarchicalDRFAllocatorProcess>
AllocatorTypes;


// The following actions make up for the fact that DoDefault
// cannot be used inside a DoAll, for example:
// EXPECT_CALL(allocator, frameworkAdded(_, _, _))
//   .WillOnce(DoAll(InvokeFrameworkAdded(&allocator),
//                   FutureSatisfy(&frameworkAdded)));

ACTION_P(InvokeInitialize, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::initialize,
      arg0,
      arg1,
      arg2);
}


ACTION_P(InvokeFrameworkAdded, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::frameworkAdded,
      arg0,
      arg1,
      arg2);
}


ACTION_P(InvokeFrameworkRemoved, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::frameworkRemoved, arg0);
}


ACTION_P(InvokeFrameworkActivated, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::frameworkActivated,
      arg0,
      arg1);
}


ACTION_P(InvokeFrameworkDeactivated, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::frameworkDeactivated,
      arg0);
}


ACTION_P(InvokeSlaveAdded, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::slaveAdded,
      arg0,
      arg1,
      arg2);
}


ACTION_P(InvokeSlaveRemoved, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::slaveRemoved,
      arg0);
}


ACTION_P(InvokeSlaveDisconnected, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::slaveDisconnected,
      arg0);
}


ACTION_P(InvokeSlaveReconnected, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::slaveReconnected,
      arg0);
}


ACTION_P(InvokeUpdateWhitelist, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::updateWhitelist,
      arg0);
}


ACTION_P(InvokeResourcesRequested, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::resourcesRequested,
      arg0,
      arg1);
}



ACTION_P(InvokeResourcesUnused, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::resourcesUnused,
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
      &master::allocator::AllocatorProcess::resourcesUnused,
      arg0,
      arg1,
      arg2,
      filters);
}


ACTION_P(InvokeResourcesRecovered, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::resourcesRecovered,
      arg0,
      arg1,
      arg2);
}


ACTION_P(InvokeOffersRevived, allocator)
{
  process::dispatch(
      allocator->real,
      &master::allocator::AllocatorProcess::offersRevived,
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


#define FUTURE_PROTOBUF(message, from, to)              \
  FutureProtobuf(message, from, to)


#define DROP_PROTOBUF(message, from, to)              \
  FutureProtobuf(message, from, to, true)


#define DROP_PROTOBUFS(message, from, to)              \
  DropProtobufs(message, from, to)


// Forward declaration.
template <typename T>
T _FutureProtobuf(const process::Message& message);


template <typename T, typename From, typename To>
process::Future<T> FutureProtobuf(T t, From from, To to, bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &t; (void) m; }

  return process::FutureMessage(testing::Eq(t.GetTypeName()), from, to, drop)
    .then(lambda::bind(&_FutureProtobuf<T>, lambda::_1));
}


template <typename T>
T _FutureProtobuf(const process::Message& message)
{
  T t;
  t.ParseFromString(message.body);
  return t;
}


template <typename T, typename From, typename To>
void DropProtobufs(T t, From from, To to)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &t; (void) m; }

  process::DropMessages(testing::Eq(t.GetTypeName()), from, to);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MESOS_HPP__
