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

#include <list>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/resources.hpp>

#include <mesos/slave/qos_controller.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "common/resources_utils.hpp"

#include "master/master.hpp"

#include "messages/messages.hpp"

#include "module/manager.hpp"

#include "slave/flags.hpp"
#include "slave/monitor.hpp"
#include "slave/slave.hpp"

#include "tests/flags.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::ResourceMonitor;
using mesos::internal::slave::Slave;

using mesos::slave::QoSCorrection;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::InvokeWithoutArgs;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

const char FIXED_RESOURCE_ESTIMATOR_NAME[] =
  "org_apache_mesos_FixedResourceEstimator";


class OversubscriptionTest : public MesosTest
{
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    // Get the current value of LD_LIBRARY_PATH.
    originalLDLibraryPath = os::libraries::paths();

    // Append our library path to LD_LIBRARY_PATH so that dlopen can
    // search the library directory for module libraries.
    os::libraries::appendPaths(
        path::join(tests::flags.build_dir, "src", ".libs"));
  }

  virtual void TearDown()
  {
    // Unload modules.
    foreach (const Modules::Library& library, modules.libraries()) {
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (module.has_name()) {
          ASSERT_SOME(modules::ModuleManager::unload(module.name()));
        }
      }
    }

    // Restore LD_LIBRARY_PATH environment variable.
    os::libraries::setPaths(originalLDLibraryPath);

    MesosTest::TearDown();
  }

  void loadFixedResourceEstimatorModule(const string& resources)
  {
    Modules::Library* library = modules.add_libraries();
    library->set_name("fixed_resource_estimator");

    Modules::Library::Module* module = library->add_modules();
    module->set_name(FIXED_RESOURCE_ESTIMATOR_NAME);

    Parameter* parameter = module->add_parameters();
    parameter->set_key("resources");
    parameter->set_value(resources);

    ASSERT_SOME(modules::ModuleManager::load(modules));
  }

  // TODO(vinod): Make this a global helper that other tests (e.g.,
  // hierarchical allocator tests) can use.
  Resources createRevocableResources(
      const string& name,
      const string& value,
      const string& role = "*")
  {
    Resource resource = Resources::parse(name, value, role).get();
    resource.mutable_revocable();
    return resource;
  }

  ResourceStatistics createResourceStatistics()
  {
    ResourceStatistics statistics;
    statistics.set_cpus_nr_periods(100);
    statistics.set_cpus_nr_throttled(2);
    statistics.set_cpus_user_time_secs(4);
    statistics.set_cpus_system_time_secs(1);
    statistics.set_cpus_throttled_time_secs(0.5);
    statistics.set_cpus_limit(1.0);
    statistics.set_mem_file_bytes(0);
    statistics.set_mem_anon_bytes(0);
    statistics.set_mem_mapped_file_bytes(0);
    statistics.set_mem_rss_bytes(1024);
    statistics.set_mem_limit_bytes(2048);
    statistics.set_timestamp(0);

    return statistics;
  }

private:
  string originalLDLibraryPath;
  Modules modules;
};


// This test verifies that the ResourceEstimator is able to fetch
// ResourceUsage statistics about running executor from
// the ResourceMonitor.
TEST_F(OversubscriptionTest, FetchResourceUsageFromMonitor)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  const ResourceStatistics statistics = createResourceStatistics();

  // Make sure that containerizer will report stub statistics.
  EXPECT_CALL(containerizer, usage(_))
    .WillOnce(Return(statistics));

  MockResourceEstimator resourceEstimator;

  Future<lambda::function<Future<ResourceUsage>()>> usageCallback;

  // Catching callback which is passed to the ResourceEstimator.
  EXPECT_CALL(resourceEstimator, initialize(_))
    .WillOnce(DoAll(FutureArg<0>(&usageCallback), Return(Nothing())));

  Try<PID<Slave>> slave = StartSlave(
      &containerizer,
      &resourceEstimator,
      CreateSlaveFlags());

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 10", DEFAULT_EXECUTOR_ID);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(usageCallback);

  Future<ResourceUsage> usage = usageCallback.get()();
  AWAIT_READY(usage);

  // Expecting the same statistics as these returned by mocked containerizer.
  ASSERT_EQ(1, usage.get().executors_size());
  EXPECT_EQ(usage.get().executors(0).executor_info().executor_id(),
            DEFAULT_EXECUTOR_ID);
  ASSERT_EQ(usage.get().executors(0).statistics(), statistics);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that slave will forward the estimation of the
// oversubscribed resources to the master.
TEST_F(OversubscriptionTest, ForwardUpdateSlaveMessage)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegistered =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockResourceEstimator resourceEstimator;

  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .WillOnce(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  slave::Flags flags = CreateSlaveFlags();
  Try<PID<Slave>> slave = StartSlave(&resourceEstimator, flags);

  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegistered);

  Future<UpdateSlaveMessage> update =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Clock::pause();
  // No update should be sent until there is an estimate.
  Clock::advance(flags.oversubscribed_resources_interval);
  Clock::settle();

  ASSERT_FALSE(update.isReady());

  // Inject an estimation of oversubscribable resources.
  Resources resources = createRevocableResources("cpus", "1");
  estimations.put(resources);

  AWAIT_READY(update);

  EXPECT_EQ(update.get().oversubscribed_resources(), resources);

  // Ensure the metric is updated.
  JSON::Object metrics = Metrics();
  ASSERT_EQ(
      1u,
      metrics.values.count("master/messages_update_slave"));
  ASSERT_EQ(
      1u,
      metrics.values["master/messages_update_slave"]);

  ASSERT_EQ(
      1u,
      metrics.values.count("master/cpus_revocable_total"));
  ASSERT_EQ(
      1.0,
      metrics.values["master/cpus_revocable_total"]);

  Shutdown();
}


// This test verifies that a framework that accepts revocable
// resources can launch a task with revocable resources.
TEST_F(OversubscriptionTest, RevocableOffer)
{
  // Start the master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start the slave with mock executor and test resource estimator.
  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  MockResourceEstimator resourceEstimator;

  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .WillOnce(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  slave::Flags flags = CreateSlaveFlags();

  Try<PID<Slave>> slave = StartSlave(&exec, &resourceEstimator, flags);
  ASSERT_SOME(slave);

  // Start the framework which accepts revocable resources.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  // Initially the framework will get all regular resources.
  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());
  EXPECT_TRUE(Resources(offers1.get()[0].resources()).revocable().empty());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Inject an estimation of oversubscribable cpu resources.
  Resources taskResources = createRevocableResources("cpus", "1");
  Resources executorResources = createRevocableResources("cpus", "1");
  estimations.put(taskResources + executorResources);

  // Now the framework will get revocable resources.
  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());
  EXPECT_EQ(
      taskResources + executorResources,
      Resources(offers2.get()[0].resources()));

  // Now launch a task that uses revocable resources.
  TaskInfo task =
    createTask(offers2.get()[0].slave_id(), taskResources, "", exec.id);

  task.mutable_executor()->mutable_resources()->CopyFrom(executorResources);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks({offers1.get()[0].id(), offers2.get()[0].id()}, {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that when the master receives a new estimate for
// oversubscribed resources it rescinds outstanding revocable offers.
TEST_F(OversubscriptionTest, RescindRevocableOffer)
{
  // Start the master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start the slave with test resource estimator.
  MockResourceEstimator resourceEstimator;

  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  // We expect 2 calls for 2 estimations.
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .Times(2)
    .WillRepeatedly(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  slave::Flags flags = CreateSlaveFlags();

  Try<PID<Slave>> slave = StartSlave(&resourceEstimator, flags);
  ASSERT_SOME(slave);

  // Start the framework which desires revocable resources.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  // Initially the framework will get all regular resources.
  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());
  EXPECT_TRUE(Resources(offers1.get()[0].resources()).revocable().empty());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  // Inject an estimation of oversubscribable resources.
  Resources resources = createRevocableResources("cpus", "1");
  estimations.put(resources);

  // Now the framework will get revocable resources.
  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());
  EXPECT_EQ(resources, Resources(offers2.get()[0].resources()));

  Future<OfferID> offerId;
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&offerId));

  Future<vector<Offer>> offers3;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers3))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Inject another estimation of oversubscribable resources while the
  // previous revocable offer is oustanding.
  Resources resources2 = createRevocableResources("cpus", "2");
  estimations.put(resources2);

  // Advance the clock for the slave to send the new estimate.
  Clock::pause();
  Clock::advance(flags.oversubscribed_resources_interval);
  Clock::settle();

  // The previous revocable offer should be rescinded.
  AWAIT_EXPECT_EQ(offers2.get()[0].id(), offerId);

  // Resume the clock for next allocation.
  Clock::resume();

  // The new offer should include the latest oversubscribed resources.
  AWAIT_READY(offers3);
  EXPECT_NE(0u, offers3.get().size());
  EXPECT_EQ(resources2, Resources(offers3.get()[0].resources()));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies the functionality of the fixed resource
// estimator. The total oversubscribed resources on the slave that
// uses a fixed resource estimator should stay the same.
TEST_F(OversubscriptionTest, FixedResourceEstimator)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegistered =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Future<UpdateSlaveMessage> update =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  loadFixedResourceEstimatorModule("cpus(*):2");

  slave::Flags flags = CreateSlaveFlags();
  flags.resource_estimator = FIXED_RESOURCE_ESTIMATOR_NAME;

  Try<PID<Slave>> slave = StartSlave(flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegistered);

  // Advance the clock for the slave to send the estimate.
  Clock::pause();
  Clock::advance(flags.oversubscribed_resources_interval);
  Clock::settle();

  AWAIT_READY(update);

  Resources resources = update.get().oversubscribed_resources();
  EXPECT_SOME_EQ(2.0, resources.cpus());

  Clock::resume();

  // Launch a task that uses revocable resources and verify that the
  // total oversubscribed resources does not change.

  // We don't expect to receive an UpdateSlaveMessage because the
  // total oversubscribed resources does not change.
  EXPECT_NO_FUTURE_PROTOBUFS(UpdateSlaveMessage(), _, _);

  // Start the framework which desires revocable resources.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  const Offer offer = offers.get()[0];

  // The offer should contain revocable resources.
  ASSERT_SOME_EQ(2.0, Resources(offer.resources()).revocable().cpus());

  // Now, launch a task that uses revocable resources.
  Resources taskResources = createRevocableResources("cpus", "1");
  taskResources += Resources::parse("mem:32").get();

  TaskInfo task = createTask(
      offer.slave_id(),
      taskResources,
      "sleep 1000");

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status.get().task_id());
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Advance the clock for the slave to trigger the calculation of the
  // total oversubscribed resources. As we described above, we don't
  // expect a new UpdateSlaveMessage being generated.
  Clock::pause();
  Clock::advance(flags.oversubscribed_resources_interval);
  Clock::settle();
  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the QoS Controller is able to fetch
// ResourceUsage statistics about running executor from
// the ResourceMonitor.
TEST_F(OversubscriptionTest, QoSFetchResourceUsageFromMonitor)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  const ResourceStatistics statistics = createResourceStatistics();

  // Make sure that containerizer will report stub statistics.
  EXPECT_CALL(containerizer, usage(_))
    .WillOnce(Return(statistics));

  MockQoSController controller;

  Future<lambda::function<Future<ResourceUsage>()>> usageCallback;

  // Catching callback which is passed to QoS Controller.
  EXPECT_CALL(controller, initialize(_))
    .WillOnce(DoAll(FutureArg<0>(&usageCallback), Return(Nothing())));

  Try<PID<Slave>> slave = StartSlave(
      &containerizer,
      &controller,
      CreateSlaveFlags());

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 10", DEFAULT_EXECUTOR_ID);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(usageCallback);

  Future<ResourceUsage> usage = usageCallback.get()();
  AWAIT_READY(usage);

  // Expecting the same statistics as these returned by mocked containerizer.
  ASSERT_EQ(1, usage.get().executors_size());
  EXPECT_EQ(usage.get().executors(0).executor_info().executor_id(),
            DEFAULT_EXECUTOR_ID);
  ASSERT_EQ(usage.get().executors(0).statistics(), statistics);

  driver.stop();
  driver.join();

  Shutdown();
}


// Ensures the slave forwards the estimation whenever receiving
// a registered or re-registered message from the master, even
// if the total oversubscribable resources does not change.
TEST_F(OversubscriptionTest, Reregistration)
{
  loadFixedResourceEstimatorModule("cpus(*):2");

  slave::Flags flags = CreateSlaveFlags();
  flags.resource_estimator = FIXED_RESOURCE_ESTIMATOR_NAME;

  Future<Nothing> slaveRecover = FUTURE_DISPATCH(_, &Slave::recover);

  StandaloneMasterDetector detector;

  Try<PID<Slave>> slave = StartSlave(&detector, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRecover);

  // Advance the clock for the slave to compute an estimate.
  Clock::pause();
  Clock::advance(flags.oversubscribed_resources_interval);
  Clock::settle();

  // Start a master, we expect the slave to send the update
  // message after registering!
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegistered =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Future<UpdateSlaveMessage> update =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  detector.appoint(master.get());

  AWAIT_READY(slaveRegistered);
  AWAIT_READY(update);

  Resources resources = update.get().oversubscribed_resources();
  EXPECT_SOME_EQ(2.0, resources.cpus());

  // Trigger a re-registration and expect another update message.
  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  update = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  detector.appoint(master.get());

  AWAIT_READY(slaveReregistered);
  AWAIT_READY(update);

  // Need to shutdown explicitly because the slave holds
  // a pointer to the detector on our test stack!
  Shutdown();
}


// Tests interactions between QoS Controller and slave. The
// TestQoSController's correction queue is filled and a mocked slave
// is checked for receiving the given correction.
TEST_F(OversubscriptionTest, ReceiveQoSCorrection)
{
  StandaloneMasterDetector detector;
  TestContainerizer containerizer;

  MockQoSController controller;

  Queue<list<QoSCorrection>> corrections;

  EXPECT_CALL(controller, corrections())
    .WillRepeatedly(InvokeWithoutArgs(
        &corrections,
        &Queue<list<QoSCorrection>>::get));

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer, &controller);

  Future<list<QoSCorrection>> qosCorrections;
  EXPECT_CALL(slave, _qosCorrections(_))
    .WillOnce(FutureArg<0>(&qosCorrections));

  spawn(slave);

  list<QoSCorrection> expected = { QoSCorrection() };
  corrections.put(expected);

  AWAIT_READY(qosCorrections);

  ASSERT_EQ(qosCorrections.get().size(), 1u);

  // TODO(nnielsen): Test for equality of QoSCorrections.

  terminate(slave);
  wait(slave);
}


// This test verifies that a QoS controller can kill a running task
// and that a TASK_LOST with REASON_EXECUTOR_PREEMPTED is sent to the
// framework.
TEST_F(OversubscriptionTest, QoSCorrectionKill)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockQoSController controller;

  Queue<list<mesos::slave::QoSCorrection>> corrections;

  EXPECT_CALL(controller, corrections())
    .WillRepeatedly(InvokeWithoutArgs(
        &corrections,
        &Queue<list<mesos::slave::QoSCorrection>>::get));

  Try<PID<Slave>> slave = StartSlave(&controller, CreateSlaveFlags());
  ASSERT_SOME(slave);

  // Verify presence and initial value of counter for preempted
  // executors.
  JSON::Object snapshot = Metrics();
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_preempted"));
  EXPECT_EQ(0u, snapshot.values["slave/executors_preempted"]);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 10");

  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status1);
  ASSERT_EQ(TASK_RUNNING, status1.get().state());

  // Carry out kill correction.
  QoSCorrection killCorrection;

  QoSCorrection::Kill* kill = killCorrection.mutable_kill();
  kill->mutable_framework_id()->CopyFrom(frameworkId.get());

  // As we use a command executor to launch an actual sleep command,
  // the executor id will be the task id.
  kill->mutable_executor_id()->set_value(task.task_id().value());

  corrections.put({killCorrection});

  // Verify task status is TASK_LOST.
  AWAIT_READY(status2);
  ASSERT_EQ(TASK_LOST, status2.get().state());
  ASSERT_EQ(TaskStatus::REASON_EXECUTOR_PREEMPTED, status2.get().reason());

  // Verify that slave incremented counter for preempted executors.
  snapshot = Metrics();
  EXPECT_EQ(1u, snapshot.values["slave/executors_preempted"]);

  driver.stop();
  driver.join();

  Shutdown();
}
} // namespace tests {
} // namespace internal {
} // namespace mesos {
