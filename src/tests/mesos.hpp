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
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/master/allocator.hpp>

#include <mesos/fetcher/fetcher.hpp>

#include <mesos/slave/qos_controller.hpp>
#include <mesos/slave/resource_estimator.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/queue.hpp>

#include <stout/bytes.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp" // For google::protobuf::Message.

#include "master/detector.hpp"
#include "master/master.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/cluster.hpp"
#include "tests/limiter.hpp"
#include "tests/utils.hpp"

#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper.hpp"
#endif // MESOS_HAS_JAVA

using ::testing::_;
using ::testing::An;
using ::testing::DoDefault;
using ::testing::Return;

namespace mesos {
namespace internal {
namespace tests {

// Forward declarations.
class MockExecutor;


class MesosTest : public TemporaryDirectoryTest
{
protected:
  MesosTest(const Option<zookeeper::URL>& url = None());

  virtual void TearDown();

  // Returns the flags used to create masters.
  virtual master::Flags CreateMasterFlags();

  // Returns the flags used to create slaves.
  virtual slave::Flags CreateSlaveFlags();

  // Starts a master with the specified flags.
  virtual Try<process::PID<master::Master> > StartMaster(
      const Option<master::Flags>& flags = None());

  // Starts a master with the specified allocator process and flags.
  virtual Try<process::PID<master::Master> > StartMaster(
      mesos::master::allocator::Allocator* allocator,
      const Option<master::Flags>& flags = None());

  // Starts a master with the specified authorizer and flags.
  // Waits for the master to detect a leader (could be itself) before
  // returning if 'wait' is set to true.
  virtual Try<process::PID<master::Master> > StartMaster(
      Authorizer* authorizer,
      const Option<master::Flags>& flags = None());

  // Starts a master with a slave removal rate limiter and flags.
  virtual Try<process::PID<master::Master> > StartMaster(
      const std::shared_ptr<MockRateLimiter>& slaveRemovalLimiter,
      const Option<master::Flags>& flags = None());

  // TODO(bmahler): Consider adding a builder style interface, e.g.
  //
  // Try<PID<Slave> > slave =
  //   Slave().With(flags)
  //          .With(executor)
  //          .With(containerizer)
  //          .With(detector)
  //          .With(gc)
  //          .Start();
  //
  // Or options:
  //
  // Injections injections;
  // injections.executor = executor;
  // injections.containerizer = containerizer;
  // injections.detector = detector;
  // injections.gc = gc;
  // Try<PID<Slave> > slave = StartSlave(injections);

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
      MasterDetector* detector,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified MasterDetector and flags.
  virtual Try<process::PID<slave::Slave> > StartSlave(
      MasterDetector* detector,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified MasterDetector, GC, and flags.
  virtual Try<process::PID<slave::Slave> > StartSlave(
      MasterDetector* detector,
      slave::GarbageCollector* gc,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified mock executor, MasterDetector
  // and flags.
  virtual Try<process::PID<slave::Slave> > StartSlave(
      MockExecutor* executor,
      MasterDetector* detector,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified resource estimator and flags.
  virtual Try<process::PID<slave::Slave>> StartSlave(
      mesos::slave::ResourceEstimator* resourceEstimator,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified mock executor, resource
  // estimator and flags.
  virtual Try<process::PID<slave::Slave>> StartSlave(
      MockExecutor* executor,
      mesos::slave::ResourceEstimator* resourceEstimator,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified resource estimator,
  // containerizer and flags.
  virtual Try<process::PID<slave::Slave>> StartSlave(
      slave::Containerizer* containerizer,
      mesos::slave::ResourceEstimator* resourceEstimator,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified QoS Controller and flags.
  virtual Try<process::PID<slave::Slave>> StartSlave(
      mesos::slave::QoSController* qosController,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified QoS Controller,
  // containerizer and flags.
  virtual Try<process::PID<slave::Slave>> StartSlave(
      slave::Containerizer* containerizer,
      mesos::slave::QoSController* qosController,
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


#ifdef MESOS_HAS_JAVA

class MesosZooKeeperTest : public MesosTest
{
public:
  static void SetUpTestCase()
  {
    // Make sure the JVM is created.
    ZooKeeperTest::SetUpTestCase();

    // Launch the ZooKeeper test server.
    server = new ZooKeeperTestServer();
    server->startNetwork();

    Try<zookeeper::URL> parse = zookeeper::URL::parse(
        "zk://" + server->connectString() + "/znode");
    ASSERT_SOME(parse);

    url = parse.get();
  }

  static void TearDownTestCase()
  {
    delete server;
    server = NULL;
  }

  virtual void SetUp()
  {
    MesosTest::SetUp();
    server->startNetwork();
  }

  virtual void TearDown()
  {
    server->shutdownNetwork();
    MesosTest::TearDown();
  }

protected:
  MesosZooKeeperTest() : MesosTest(url) {}

  virtual master::Flags CreateMasterFlags()
  {
    master::Flags flags = MesosTest::CreateMasterFlags();

    // NOTE: Since we are using the replicated log with ZooKeeper
    // (default storage in MesosTest), we need to specify the quorum.
    flags.quorum = 1;

    return flags;
  }

  static ZooKeeperTestServer* server;
  static Option<zookeeper::URL> url;
};

#endif // MESOS_HAS_JAVA


// Macros to get/create (default) ExecutorInfos and FrameworkInfos.
#define DEFAULT_EXECUTOR_INFO                                           \
      ({ ExecutorInfo executor;                                         \
        executor.mutable_executor_id()->set_value("default");           \
        executor.mutable_command()->set_value("exit 1");                \
        executor; })


#define DEFAULT_V1_EXECUTOR_INFO                                        \
     ({ v1::ExecutorInfo executor;                                      \
        executor.mutable_executor_id()->set_value("default");           \
        executor.mutable_command()->set_value("exit 1");                \
        executor; })


#define CREATE_EXECUTOR_INFO(executorId, command)                       \
      ({ ExecutorInfo executor;                                         \
        executor.mutable_executor_id()->set_value(executorId);          \
        executor.mutable_command()->set_value(command);                 \
        executor; })


#define DEFAULT_CREDENTIAL                                             \
     ({ Credential credential;                                         \
        credential.set_principal("test-principal");                    \
        credential.set_secret("test-secret");                          \
        credential; })


#define DEFAULT_V1_CREDENTIAL                                          \
     ({ v1::Credential credential;                                     \
        credential.set_principal("test-principal");                    \
        credential.set_secret("test-secret");                          \
        credential; })


#define DEFAULT_FRAMEWORK_INFO                                          \
     ({ FrameworkInfo framework;                                        \
        framework.set_name("default");                                  \
        framework.set_user(os::user().get());                           \
        framework.set_principal(DEFAULT_CREDENTIAL.principal());        \
        framework; })


#define DEFAULT_V1_FRAMEWORK_INFO                                       \
     ({ v1::FrameworkInfo framework;                                    \
        framework.set_name("default");                                  \
        framework.set_user(os::user().get());                           \
        framework.set_principal(DEFAULT_CREDENTIAL.principal());        \
        framework; })


#define DEFAULT_EXECUTOR_ID           \
      DEFAULT_EXECUTOR_INFO.executor_id()


#define DEFAULT_V1_EXECUTOR_ID         \
      DEFAULT_V1_EXECUTOR_INFO.executor_id()


#define DEFAULT_CONTAINER_ID                                          \
     ({ ContainerID containerId;                                      \
        containerId.set_value("container");                           \
        containerId; })


#define CREATE_COMMAND_INFO(command)                                  \
  ({ CommandInfo commandInfo;                                         \
     commandInfo.set_value(command);                                  \
     commandInfo; })


#define CREATE_VOLUME(containerPath, hostPath, mode)                  \
      ({ Volume volume;                                               \
         volume.set_container_path(containerPath);                    \
         volume.set_host_path(hostPath);                              \
         volume.set_mode(mode);                                       \
         volume; })


// TODO(bmahler): Refactor this to make the distinction between
// command tasks and executor tasks clearer.
inline TaskInfo createTask(
    const SlaveID& slaveId,
    const Resources& resources,
    const std::string& command,
    const Option<mesos::ExecutorID>& executorId = None(),
    const std::string& name = "test-task",
    const std::string& id = UUID::random().toString())
{
  TaskInfo task;
  task.set_name(name);
  task.mutable_task_id()->set_value(id);
  task.mutable_slave_id()->CopyFrom(slaveId);
  task.mutable_resources()->CopyFrom(resources);
  if (executorId.isSome()) {
    ExecutorInfo executor;
    executor.mutable_executor_id()->CopyFrom(executorId.get());
    executor.mutable_command()->set_value(command);
    task.mutable_executor()->CopyFrom(executor);
  } else {
    task.mutable_command()->set_value(command);
  }

  return task;
}


inline TaskInfo createTask(
    const Offer& offer,
    const std::string& command,
    const Option<mesos::ExecutorID>& executorId = None(),
    const std::string& name = "test-task",
    const std::string& id = UUID::random().toString())
{
  return createTask(
      offer.slave_id(), offer.resources(), command, executorId, name, id);
}


inline Resource::ReservationInfo createReservationInfo(
    const std::string& principal)
{
  Resource::ReservationInfo info;
  info.set_principal(principal);
  return info;
}


// NOTE: We only set the volume in DiskInfo if 'containerPath' is set.
// If volume mode is not specified, Volume::RW will be used (assuming
// 'containerPath' is set).
inline Resource::DiskInfo createDiskInfo(
    const Option<std::string>& persistenceId,
    const Option<std::string>& containerPath,
    const Option<Volume::Mode>& mode = None(),
    const Option<std::string>& hostPath = None())
{
  Resource::DiskInfo info;

  if (persistenceId.isSome()) {
    info.mutable_persistence()->set_id(persistenceId.get());
  }

  if (containerPath.isSome()) {
    Volume volume;
    volume.set_container_path(containerPath.get());
    volume.set_mode(mode.isSome() ? mode.get() : Volume::RW);

    if (hostPath.isSome()) {
      volume.set_host_path(hostPath.get());
    }

    info.mutable_volume()->CopyFrom(volume);
  }

  return info;
}


inline Resource createPersistentVolume(
    const Bytes& size,
    const std::string& role,
    const std::string& persistenceId,
    const std::string& containerPath)
{
  Resource volume = Resources::parse(
      "disk",
      stringify(size.megabytes()),
      role).get();

  volume.mutable_disk()->CopyFrom(
      createDiskInfo(persistenceId, containerPath));

  return volume;
}


// Helpers for creating reserve operations.
inline Offer::Operation RESERVE(const Resources& resources)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::RESERVE);
  operation.mutable_reserve()->mutable_resources()->CopyFrom(resources);
  return operation;
}


// Helpers for creating unreserve operations.
inline Offer::Operation UNRESERVE(const Resources& resources)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::UNRESERVE);
  operation.mutable_unreserve()->mutable_resources()->CopyFrom(resources);
  return operation;
}


// Helpers for creating offer operations.
inline Offer::Operation CREATE(const Resources& volumes)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::CREATE);
  operation.mutable_create()->mutable_volumes()->CopyFrom(volumes);
  return operation;
}


inline Offer::Operation DESTROY(const Resources& volumes)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::DESTROY);
  operation.mutable_destroy()->mutable_volumes()->CopyFrom(volumes);
  return operation;
}


inline Offer::Operation LAUNCH(const std::vector<TaskInfo>& tasks)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH);

  foreach (const TaskInfo& task, tasks) {
    operation.mutable_launch()->add_task_infos()->CopyFrom(task);
  }

  return operation;
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

    while (remaining.flatten().contains(TASK_RESOURCES) &&
           launched < numTasks) {
      TaskInfo task;
      task.set_name("TestTask");
      task.mutable_task_id()->set_value(stringify(nextTaskId++));
      task.mutable_slave_id()->MergeFrom(offer.slave_id());
      task.mutable_executor()->MergeFrom(executor);

      Option<Resources> resources =
        remaining.find(TASK_RESOURCES.flatten(role));

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


// Like DeclineOffers, but takes a custom filters object.
ACTION_P(DeclineOffers, filters)
{
  SchedulerDriver* driver = arg0;
  std::vector<Offer> offers = arg1;

  for (size_t i = 0; i < offers.size(); i++) {
    driver->declineOffer(offers[i].id(), filters);
  }
}


// For use with a MockScheduler, for example:
// process::Queue<Offer> offers;
// EXPECT_CALL(sched, resourceOffers(_, _))
//   .WillRepeatedly(EnqueueOffers(&offers));
// Enqueues all received offers into the provided queue.
ACTION_P(EnqueueOffers, queue)
{
  std::vector<Offer> offers = arg1;
  foreach (const Offer& offer, offers) {
    queue->put(offer);
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
      MasterDetector* _detector)
    : MesosSchedulerDriver(
          scheduler,
          DEFAULT_FRAMEWORK_INFO,
          "",
          true,
          DEFAULT_CREDENTIAL)
  {
    detector = _detector;
  }

  TestingMesosSchedulerDriver(
      Scheduler* scheduler,
      MasterDetector* _detector,
      const FrameworkInfo& framework,
      bool implicitAcknowledgements = true)
    : MesosSchedulerDriver(
          scheduler,
          framework,
          "",
          implicitAcknowledgements,
          DEFAULT_CREDENTIAL)
  {
    detector = _detector;
  }

  TestingMesosSchedulerDriver(
      Scheduler* scheduler,
      MasterDetector* _detector,
      const FrameworkInfo& framework,
      bool implicitAcknowledgements,
      const Credential& credential)
    : MesosSchedulerDriver(
          scheduler,
          framework,
          "",
          implicitAcknowledgements,
          credential)
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


class MockGarbageCollector : public slave::GarbageCollector
{
public:
  MockGarbageCollector()
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

  MOCK_METHOD2(
      schedule,
      process::Future<Nothing>(const Duration& d, const std::string& path));
  MOCK_METHOD1(
      unschedule,
      process::Future<bool>(const std::string& path));
  MOCK_METHOD1(
      prune,
      void(const Duration& d));
};


class MockResourceEstimator : public mesos::slave::ResourceEstimator
{
public:
  MockResourceEstimator()
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

  virtual ~MockResourceEstimator() {}

  MOCK_METHOD1(
      initialize,
      Try<Nothing>(const lambda::function<process::Future<ResourceUsage>()>&));

  MOCK_METHOD0(
      oversubscribable,
      process::Future<Resources>());
};


// The MockQoSController is a stub which lets tests fill the
// correction queue for a slave.
class MockQoSController : public mesos::slave::QoSController
{
public:
  MockQoSController()
  {
    ON_CALL(*this, initialize(_))
      .WillByDefault(Return(Nothing()));
    EXPECT_CALL(*this, initialize(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, corrections())
      .WillByDefault(
          Return(process::Future<std::list<mesos::slave::QoSCorrection>>()));
    EXPECT_CALL(*this, corrections())
      .WillRepeatedly(DoDefault());
  }

  MOCK_METHOD1(
      initialize,
      Try<Nothing>(const lambda::function<process::Future<ResourceUsage>()>&));

  MOCK_METHOD0(
      corrections, process::Future<std::list<mesos::slave::QoSCorrection>>());
};


// Definition of a mock Slave to be used in tests with gmock, covering
// potential races between runTask and killTask.
class MockSlave : public slave::Slave
{
public:
  MockSlave(
      const slave::Flags& flags,
      MasterDetector* detector,
      slave::Containerizer* containerizer,
      const Option<mesos::slave::QoSController*>& qosController = None());

  virtual ~MockSlave();

  MOCK_METHOD5(runTask, void(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const FrameworkID& frameworkId,
      const process::UPID& pid,
      TaskInfo task));

  void unmocked_runTask(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const FrameworkID& frameworkId,
      const process::UPID& pid,
      TaskInfo task);

  MOCK_METHOD3(_runTask, void(
      const process::Future<bool>& future,
      const FrameworkInfo& frameworkInfo,
      const TaskInfo& task));

  void unmocked__runTask(
      const process::Future<bool>& future,
      const FrameworkInfo& frameworkInfo,
      const TaskInfo& task);

  MOCK_METHOD3(killTask, void(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const TaskID& taskId));

  void unmocked_killTask(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const TaskID& taskId);

  MOCK_METHOD1(removeFramework, void(
      slave::Framework* framework));

  void unmocked_removeFramework(
      slave::Framework* framework);

  MOCK_METHOD1(__recover, void(
      const process::Future<Nothing>& future));

  void unmocked___recover(
      const process::Future<Nothing>& future);

  MOCK_METHOD0(qosCorrections, void());

  void unmocked_qosCorrections();

  MOCK_METHOD1(_qosCorrections, void(
      const process::Future<std::list<
          mesos::slave::QoSCorrection>>& correction));

private:
  Files files;
  MockGarbageCollector gc;
  MockResourceEstimator resourceEstimator;
  MockQoSController qosController;
  slave::StatusUpdateManager* statusUpdateManager;
};


// Definition of a mock FetcherProcess to be used in tests with gmock.
class MockFetcherProcess : public slave::FetcherProcess
{
public:
  MockFetcherProcess();

  virtual ~MockFetcherProcess() {}

  MOCK_METHOD6(_fetch, process::Future<Nothing>(
      const hashmap<
          CommandInfo::URI,
          Option<process::Future<std::shared_ptr<Cache::Entry>>>>&
        entries,
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const std::string& cacheDirectory,
      const Option<std::string>& user,
      const slave::Flags& flags));

  process::Future<Nothing> unmocked__fetch(
      const hashmap<
          CommandInfo::URI,
          Option<process::Future<std::shared_ptr<Cache::Entry>>>>&
        entries,
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const std::string& cacheDirectory,
      const Option<std::string>& user,
      const slave::Flags& flags);

  MOCK_METHOD5(run, process::Future<Nothing>(
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const FetcherInfo& info,
      const slave::Flags& flags));

  process::Future<Nothing> unmocked_run(
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const FetcherInfo& info,
      const slave::Flags& flags);
};


// Definition of a MockAuthozier that can be used in tests with gmock.
class MockAuthorizer : public Authorizer
{
public:
  MockAuthorizer()
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

    EXPECT_CALL(*this, initialize(An<const Option<ACLs>&>()))
      .WillRepeatedly(Return(Nothing()));
  }

  MOCK_METHOD1(
      initialize, Try<Nothing>(const Option<ACLs>& acls));
  MOCK_METHOD1(
      authorize, process::Future<bool>(const ACL::RegisterFramework& request));
  MOCK_METHOD1(
      authorize, process::Future<bool>(const ACL::RunTask& request));
  MOCK_METHOD1(
      authorize, process::Future<bool>(const ACL::ShutdownFramework& request));
};


// The following actions make up for the fact that DoDefault
// cannot be used inside a DoAll, for example:
// EXPECT_CALL(allocator, addFramework(_, _, _))
//   .WillOnce(DoAll(InvokeAddFramework(&allocator),
//                   FutureSatisfy(&addFramework)));

ACTION_P(InvokeInitialize, allocator)
{
  allocator->real->initialize(arg0, arg1, arg2);
}


ACTION_P(InvokeAddFramework, allocator)
{
  allocator->real->addFramework(arg0, arg1, arg2);
}


ACTION_P(InvokeRemoveFramework, allocator)
{
  allocator->real->removeFramework(arg0);
}


ACTION_P(InvokeActivateFramework, allocator)
{
  allocator->real->activateFramework(arg0);
}


ACTION_P(InvokeDeactivateFramework, allocator)
{
  allocator->real->deactivateFramework(arg0);
}


ACTION_P(InvokeUpdateFramework, allocator)
{
  allocator->real->updateFramework(arg0, arg1);
}


ACTION_P(InvokeAddSlave, allocator)
{
  allocator->real->addSlave(arg0, arg1, arg2, arg3);
}


ACTION_P(InvokeRemoveSlave, allocator)
{
  allocator->real->removeSlave(arg0);
}


ACTION_P(InvokeUpdateSlave, allocator)
{
  allocator->real->updateSlave(arg0, arg1);
}


ACTION_P(InvokeActivateSlave, allocator)
{
  allocator->real->activateSlave(arg0);
}


ACTION_P(InvokeDeactivateSlave, allocator)
{
  allocator->real->deactivateSlave(arg0);
}


ACTION_P(InvokeUpdateWhitelist, allocator)
{
  allocator->real->updateWhitelist(arg0);
}


ACTION_P(InvokeRequestResources, allocator)
{
  allocator->real->requestResources(arg0, arg1);
}


ACTION_P(InvokeUpdateAllocation, allocator)
{
  allocator->real->updateAllocation(arg0, arg1, arg2);
}


ACTION_P(InvokeUpdateResources, allocator)
{
  return allocator->real->updateAvailable(arg0, arg1);
}


ACTION_P(InvokeRecoverResources, allocator)
{
  allocator->real->recoverResources(arg0, arg1, arg2, arg3);
}


ACTION_P2(InvokeRecoverResourcesWithFilters, allocator, timeout)
{
  Filters filters;
  filters.set_refuse_seconds(timeout);

  allocator->real->recoverResources(arg0, arg1, arg2, filters);
}


ACTION_P(InvokeReviveOffers, allocator)
{
  allocator->real->reviveOffers(arg0);
}


template <typename T = master::allocator::HierarchicalDRFAllocator>
mesos::master::allocator::Allocator* createAllocator()
{
  // T represents the allocator type. It can be a default built-in
  // allocator, or one provided by an allocator module.
  Try<mesos::master::allocator::Allocator*> instance = T::create();
  CHECK_SOME(instance);
  return CHECK_NOTNULL(instance.get());
}

template <typename T = master::allocator::HierarchicalDRFAllocator>
class TestAllocator : public mesos::master::allocator::Allocator
{
public:
  // Actual allocation is done by an instance of real allocator,
  // which is specified by the template parameter.
  TestAllocator() : real(createAllocator<T>())
  {
    // We use 'ON_CALL' and 'WillByDefault' here to specify the
    // default actions (call in to the real allocator). This allows
    // the tests to leverage the 'DoDefault' action.
    // However, 'ON_CALL' results in a "Uninteresting mock function
    // call" warning unless each test puts expectations in place.
    // As a result, we also use 'EXPECT_CALL' and 'WillRepeatedly'
    // to get the best of both worlds: the ability to use 'DoDefault'
    // and no warnings when expectations are not explicit.

    ON_CALL(*this, initialize(_, _, _))
      .WillByDefault(InvokeInitialize(this));
    EXPECT_CALL(*this, initialize(_, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, addFramework(_, _, _))
      .WillByDefault(InvokeAddFramework(this));
    EXPECT_CALL(*this, addFramework(_, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, removeFramework(_))
      .WillByDefault(InvokeRemoveFramework(this));
    EXPECT_CALL(*this, removeFramework(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, activateFramework(_))
      .WillByDefault(InvokeActivateFramework(this));
    EXPECT_CALL(*this, activateFramework(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, deactivateFramework(_))
      .WillByDefault(InvokeDeactivateFramework(this));
    EXPECT_CALL(*this, deactivateFramework(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateFramework(_, _))
      .WillByDefault(InvokeUpdateFramework(this));
    EXPECT_CALL(*this, updateFramework(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, addSlave(_, _, _, _))
      .WillByDefault(InvokeAddSlave(this));
    EXPECT_CALL(*this, addSlave(_, _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, removeSlave(_))
      .WillByDefault(InvokeRemoveSlave(this));
    EXPECT_CALL(*this, removeSlave(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateSlave(_, _))
      .WillByDefault(InvokeUpdateSlave(this));
    EXPECT_CALL(*this, updateSlave(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, activateSlave(_))
      .WillByDefault(InvokeActivateSlave(this));
    EXPECT_CALL(*this, activateSlave(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, deactivateSlave(_))
      .WillByDefault(InvokeDeactivateSlave(this));
    EXPECT_CALL(*this, deactivateSlave(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateWhitelist(_))
      .WillByDefault(InvokeUpdateWhitelist(this));
    EXPECT_CALL(*this, updateWhitelist(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, requestResources(_, _))
      .WillByDefault(InvokeRequestResources(this));
    EXPECT_CALL(*this, requestResources(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateAllocation(_, _, _))
      .WillByDefault(InvokeUpdateAllocation(this));
    EXPECT_CALL(*this, updateAllocation(_, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, updateAvailable(_, _))
      .WillByDefault(InvokeUpdateResources(this));
    EXPECT_CALL(*this, updateAvailable(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, recoverResources(_, _, _, _))
      .WillByDefault(InvokeRecoverResources(this));
    EXPECT_CALL(*this, recoverResources(_, _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, reviveOffers(_))
      .WillByDefault(InvokeReviveOffers(this));
    EXPECT_CALL(*this, reviveOffers(_))
      .WillRepeatedly(DoDefault());
  }

  virtual ~TestAllocator() {}

  MOCK_METHOD3(initialize, void(
      const Duration&,
      const lambda::function<
          void(const FrameworkID&,
               const hashmap<SlaveID, Resources>&)>&,
      const hashmap<std::string, mesos::master::RoleInfo>&));

  MOCK_METHOD3(addFramework, void(
      const FrameworkID&,
      const FrameworkInfo&,
      const hashmap<SlaveID, Resources>&));

  MOCK_METHOD1(removeFramework, void(
      const FrameworkID&));

  MOCK_METHOD1(activateFramework, void(
      const FrameworkID&));

  MOCK_METHOD1(deactivateFramework, void(
      const FrameworkID&));

  MOCK_METHOD2(updateFramework, void(
      const FrameworkID&,
      const FrameworkInfo&));

  MOCK_METHOD4(addSlave, void(
      const SlaveID&,
      const SlaveInfo&,
      const Resources&,
      const hashmap<FrameworkID, Resources>&));

  MOCK_METHOD1(removeSlave, void(
      const SlaveID&));

  MOCK_METHOD2(updateSlave, void(
      const SlaveID&,
      const Resources&));

  MOCK_METHOD1(activateSlave, void(
      const SlaveID&));

  MOCK_METHOD1(deactivateSlave, void(
      const SlaveID&));

  MOCK_METHOD1(updateWhitelist, void(
      const Option<hashset<std::string> >&));

  MOCK_METHOD2(requestResources, void(
      const FrameworkID&,
      const std::vector<Request>&));

  MOCK_METHOD3(updateAllocation, void(
      const FrameworkID&,
      const SlaveID&,
      const std::vector<Offer::Operation>&));

  MOCK_METHOD2(updateAvailable, process::Future<Nothing>(
      const SlaveID&,
      const std::vector<Offer::Operation>&));

  MOCK_METHOD4(recoverResources, void(
      const FrameworkID&,
      const SlaveID&,
      const Resources&,
      const Option<Filters>& filters));

  MOCK_METHOD1(reviveOffers, void(const FrameworkID&));

  process::Owned<mesos::master::allocator::Allocator> real;
};


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


inline
const ::testing::Matcher<const std::vector<Offer>& > OfferEq(int cpus, int mem)
{
  return MakeMatcher(new OfferEqMatcher(cpus, mem));
}


ACTION_P(SendStatusUpdateFromTask, state)
{
  TaskStatus status;
  status.mutable_task_id()->MergeFrom(arg1.task_id());
  status.set_state(state);
  arg0->sendStatusUpdate(status);
}


ACTION_P(SendStatusUpdateFromTaskID, state)
{
  TaskStatus status;
  status.mutable_task_id()->MergeFrom(arg1);
  status.set_state(state);
  arg0->sendStatusUpdate(status);
}


ACTION_P(SendFrameworkMessage, data)
{
  arg0->sendFrameworkMessage(data);
}


#define FUTURE_PROTOBUF(message, from, to)              \
  FutureProtobuf(message, from, to)


#define DROP_PROTOBUF(message, from, to)              \
  FutureProtobuf(message, from, to, true)


#define DROP_PROTOBUFS(message, from, to)              \
  DropProtobufs(message, from, to)


#define EXPECT_NO_FUTURE_PROTOBUFS(message, from, to)              \
  ExpectNoFutureProtobufs(message, from, to)


// These are specialized versions of {FUTURE,DROP}_PROTOBUF that
// capture a scheduler/executor Call protobuf of the given 'type'.
// Note that we name methods as '*ProtobufUnion()' because these could
// be reused for macros that capture any protobufs that are described
// using the standard protocol buffer "union" trick (e.g.,
// FUTURE_EVENT to capture scheduler::Event), see
// https://developers.google.com/protocol-buffers/docs/techniques#union.

#define FUTURE_CALL(message, unionType, from, to)              \
  FutureUnionProtobuf(message, unionType, from, to)


#define DROP_CALL(message, unionType, from, to)                \
  FutureUnionProtobuf(message, unionType, from, to, true)


#define DROP_CALLS(message, unionType, from, to)               \
  DropUnionProtobufs(message, unionType, from, to)


#define EXPECT_NO_FUTURE_CALLS(message, unionType, from, to)   \
  ExpectNoFutureUnionProtobufs(message, unionType, from, to)


#define FUTURE_CALL_MESSAGE(message, unionType, from, to)          \
  process::FutureUnionMessage(message, unionType, from, to)


#define DROP_CALL_MESSAGE(message, unionType, from, to)            \
  process::FutureUnionMessage(message, unionType, from, to, true)


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


template <typename Message, typename UnionType, typename From, typename To>
process::Future<Message> FutureUnionProtobuf(
    Message message, UnionType unionType, From from, To to, bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  return process::FutureUnionMessage(message, unionType, from, to, drop)
    .then(lambda::bind(&_FutureProtobuf<Message>, lambda::_1));
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


template <typename Message, typename UnionType, typename From, typename To>
void DropUnionProtobufs(Message message, UnionType unionType, From from, To to)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  process::DropUnionMessages(message, unionType, from, to);
}


template <typename T, typename From, typename To>
void ExpectNoFutureProtobufs(T t, From from, To to)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &t; (void) m; }

  process::ExpectNoFutureMessages(testing::Eq(t.GetTypeName()), from, to);
}


template <typename Message, typename UnionType, typename From, typename To>
void ExpectNoFutureUnionProtobufs(
    Message message, UnionType unionType, From from, To to)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  process::ExpectNoFutureUnionMessages(message, unionType, from, to);
}


// This matcher is used to match the task ids of TaskStatus messages.
// Suppose we set up N futures for LaunchTasks and N futures for StatusUpdates.
// (This is a common pattern). We get into a situation where all StatusUpdates
// are satisfied before the LaunchTasks if the master re-sends StatusUpdates.
// We use this matcher to only satisfy the StatusUpdate future if the
// StatusUpdate came from the corresponding task.
MATCHER_P(TaskStatusEq, task, "") { return arg.task_id() == task.task_id(); }

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MESOS_HPP__
