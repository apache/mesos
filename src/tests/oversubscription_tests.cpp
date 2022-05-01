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

#include "master/detector/standalone.hpp"

#include "messages/messages.hpp"

#include "module/manager.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"
#include "slave/qos_controllers/load.hpp"

#include "tests/flags.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"
#include "tests/resources_utils.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::protobuf::createLabel;

using mesos::internal::slave::LoadQoSController;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

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
  void SetUp() override
  {
    MesosTest::SetUp();
  }

  void TearDown() override
  {
    // Unload modules.
    foreach (const Modules::Library& library, modules.libraries()) {
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (module.has_name()) {
          ASSERT_SOME(modules::ModuleManager::unload(module.name()));
        }
      }
    }

    MesosTest::TearDown();
  }

  void loadFixedResourceEstimatorModule(const string& resources)
  {
    string libraryPath = getModulePath("fixed_resource_estimator");

    Modules::Library* library = modules.add_libraries();
    library->set_name("fixed_resource_estimator");
    library->set_file(libraryPath);

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

  ExecutorInfo createExecutorInfo(
      const string& _frameworkId,
      const string& _executorId)
  {
    FrameworkID frameworkId;
    frameworkId.set_value(_frameworkId);

    ExecutorID executorId;
    executorId.set_value(_executorId);

    ExecutorInfo executorInfo;
    executorInfo.mutable_executor_id()->CopyFrom(executorId);
    executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

    return executorInfo;
  }

private:
  Modules modules;
};


// This test verifies that the ResourceEstimator is able to fetch
// ResourceUsage statistics about running executor.
TEST_F(OversubscriptionTest, FetchResourceUsage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
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

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      &containerizer,
      &resourceEstimator,
      CreateSlaveFlags());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 10", DEFAULT_EXECUTOR_ID);
  task.mutable_labels()->add_labels()->CopyFrom(
      createLabel("key1", "value1"));
  task.mutable_executor()->mutable_labels()->add_labels()->CopyFrom(
      createLabel("key2", "value2"));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(usageCallback);

  Future<ResourceUsage> usage = usageCallback.get()();
  AWAIT_READY(usage);

  // Expecting the same statistics as these returned by mocked containerizer.
  ASSERT_EQ(1, usage->executors_size());
  EXPECT_EQ(usage->executors(0).executor_info().executor_id(),
            DEFAULT_EXECUTOR_ID);
  ASSERT_EQ(usage->executors(0).statistics(), statistics);
  ASSERT_EQ(task.executor().labels(),
            usage->executors(0).executor_info().labels());
  ASSERT_EQ(1, usage->executors(0).tasks().size());
  ASSERT_EQ(task.name(),
            usage->executors(0).tasks(0).name());
  ASSERT_EQ(task.task_id(),
            usage->executors(0).tasks(0).id());
  ASSERT_EQ(task.resources(),
            usage->executors(0).tasks(0).resources());
  ASSERT_EQ(task.labels(),
            usage->executors(0).tasks(0).labels());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that slave will forward the estimation of the
// oversubscribed resources to the master.
TEST_F(OversubscriptionTest, ForwardUpdateSlaveMessage)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // The agent will send a single `UpdateSlaveMessage` after registration.
  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  MockResourceEstimator resourceEstimator;

  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .WillOnce(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &resourceEstimator, flags);
  ASSERT_SOME(slave);

  Clock::advance(flags.registration_backoff_factor);
  Clock::advance(flags.authentication_backoff_factor);
  Clock::settle();

  AWAIT_READY(updateSlaveMessage);

  Future<UpdateSlaveMessage> update =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // No update should be sent until there is an estimate.
  Clock::advance(flags.oversubscribed_resources_interval);
  Clock::settle();

  ASSERT_FALSE(update.isReady());

  // Inject an estimation of oversubscribable resources.
  Resources resources = createRevocableResources("cpus", "1");
  estimations.put(resources);

  AWAIT_READY(update);

  EXPECT_TRUE(update->has_update_oversubscribed_resources());
  EXPECT_TRUE(update->update_oversubscribed_resources());
  EXPECT_EQ(update->oversubscribed_resources(), resources);

  // Ensure the metric is updated.
  JSON::Object metrics = Metrics();
  ASSERT_EQ(
      1u,
      metrics.values.count("master/messages_update_slave"));
  ASSERT_EQ(
      2u,
      metrics.values["master/messages_update_slave"]);

  ASSERT_EQ(
      1u,
      metrics.values.count("master/cpus_revocable_total"));
  ASSERT_EQ(
      1.0,
      metrics.values["master/cpus_revocable_total"]);
}


// This test verifies that a framework that accepts revocable
// resources can launch a task with revocable resources.
TEST_F(OversubscriptionTest, RevocableOffer)
{
  // Start the master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start the slave with mock executor and test resource estimator.
  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  MockResourceEstimator resourceEstimator;

  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .WillOnce(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, &resourceEstimator, flags);
  ASSERT_SOME(slave);

  // Start the framework which accepts revocable resources.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  // Initially the framework will get all regular resources.
  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  EXPECT_TRUE(Resources(offers1.get()[0].resources()).revocable().empty());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Inject an estimation of oversubscribable cpu resources.
  estimations.put(createRevocableResources("cpus", "2"));

  Resources taskResources = createRevocableResources("cpus", "1");
  taskResources.allocate(framework.roles(0));

  Resources executorResources = createRevocableResources("cpus", "1");
  executorResources.allocate(framework.roles(0));

  // Now the framework will get revocable resources.
  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
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
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that when the master receives a new estimate for
// oversubscribed resources it rescinds outstanding revocable offers.
// In this test the oversubscribed resources are increased, so the master
// will send out new offers with increased revocable resources.
TEST_F(OversubscriptionTest, RescindRevocableOfferWithIncreasedRevocable)
{
  // Pause the clock because we want to manually drive the allocations.
  Clock::pause();

  // Start the master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start the slave with test resource estimator.
  MockResourceEstimator resourceEstimator;

  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  // We expect 2 calls for 2 estimations.
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .Times(2)
    .WillRepeatedly(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  slave::Flags agentFlags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &resourceEstimator, agentFlags);
  ASSERT_SOME(slave);

  // Start the framework which desires revocable resources.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Queue<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(EnqueueOffers(&offers));

  driver.start();

  // Initially the framework will get all regular resources.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  ASSERT_EQ(1u, offers.size());
  EXPECT_TRUE(Resources(offers.get()->resources()).revocable().empty());

  // Inject an estimation of oversubscribable resources.
  Resources resources1 = createRevocableResources("cpus", "1");
  estimations.put(resources1);

  // Now the framework will get revocable resources.
  Clock::settle();

  EXPECT_EQ(1u, offers.size());
  Future<Offer> offer = offers.get();
  AWAIT_READY(offer);
  EXPECT_EQ(allocatedResources(resources1, framework.roles(0)),
            Resources(offer->resources()));

  Future<OfferID> offerId;
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&offerId));

  // Inject another estimation of increased oversubscribable resources
  // while the previous revocable offer is outstanding.
  Resources resources2 = createRevocableResources("cpus", "3");
  estimations.put(resources2);

  // Advance the clock for the slave to send the new estimate.
  Clock::advance(agentFlags.oversubscribed_resources_interval);
  Clock::settle();

  // The previous revocable offer should be rescinded.
  AWAIT_EXPECT_EQ(offer->id(), offerId);

  // The allocation run triggered by the agent resource update may or
  // may not take into account the rescinded offer due to a race
  // between the dispatched allocation and the recovery of the resources
  // from the recinded offer. Therefore we advance the clock after these
  // resources are recovered to trigger a batch allocation to make sure
  // all resources are allocated.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  ASSERT_GT(offers.size(), 0u);

  // The total offered resources after the latest estimate.
  Resources resources3;
  while (offers.size() != 0) {
    resources3 += offers.get()->resources();
  }

  // The offered resources should match the resource estimate.
  EXPECT_EQ(allocatedResources(resources2, framework.roles(0)), resources3);

  driver.stop();
  driver.join();
}


// This test verifies that when the master receives a new estimate for
// oversubscribed resources it rescinds outstanding revocable offers.
// In this test the oversubscribed resources are decreased, so the
// master will send out only one offer with the latest oversubscribed
// resources from the resource estimator.
TEST_F(OversubscriptionTest, RescindRevocableOfferWithDecreasedRevocable)
{
  // Pause the clock because we want to manually drive the allocations.
  Clock::pause();

  // Start the master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start the slave with test resource estimator.
  MockResourceEstimator resourceEstimator;

  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  // We expect 2 calls for 2 estimations.
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .Times(2)
    .WillRepeatedly(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  slave::Flags agentFlags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &resourceEstimator, agentFlags);
  ASSERT_SOME(slave);

  // Start the framework which desires revocable resources.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  // Initially the framework will get all regular resources.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);
  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  EXPECT_TRUE(Resources(offers1.get()[0].resources()).revocable().empty());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  // Inject an estimation of oversubscribable resources.
  Resources resources1 = createRevocableResources("cpus", "3");
  estimations.put(resources1);

  // Now the framework will get revocable resources.
  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
  EXPECT_EQ(allocatedResources(resources1, framework.roles(0)),
            Resources(offers2.get()[0].resources()));

  Future<OfferID> offerId;
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&offerId));

  Future<vector<Offer>> offers3;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers3));

  // Inject another estimation of decreased oversubscribable resources
  // while the previous revocable offer is outstanding.
  Resources resources2 = createRevocableResources("cpus", "1");
  estimations.put(resources2);

  // Advance the clock for the slave to send the new estimate.
  Clock::advance(agentFlags.oversubscribed_resources_interval);
  Clock::settle();

  // The previous revocable offer should be rescinded.
  AWAIT_EXPECT_EQ(offers2.get()[0].id(), offerId);

  // Advance the clock to trigger a batch allocation, this will
  // allocate the oversubscribed resources that were rescinded.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  // The new offer should include the latest oversubscribed resources.
  AWAIT_READY(offers3);
  ASSERT_FALSE(offers3->empty());
  EXPECT_EQ(allocatedResources(resources2, framework.roles(0)),
            Resources(offers3.get()[0].resources()));

  driver.stop();
  driver.join();
}


// This test verifies the functionality of the fixed resource
// estimator. The total oversubscribed resources on the slave that
// uses a fixed resource estimator should stay the same.
TEST_F(OversubscriptionTest, FixedResourceEstimator)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegistered =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Future<UpdateSlaveMessage> update =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  loadFixedResourceEstimatorModule("cpus(*):2");

  slave::Flags flags = CreateSlaveFlags();
  flags.resource_estimator = FIXED_RESOURCE_ESTIMATOR_NAME;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegistered);

  // Advance the clock for the slave to send the estimate.
  Clock::pause();
  Clock::advance(flags.oversubscribed_resources_interval);
  Clock::settle();

  AWAIT_READY(update);

  // Depending on the recovery timing, the slave could send an
  // UpdateSlaveMessage message before the resource estimator is
  // ready, so if that's the case, wait for another update.
  if (!update->has_update_oversubscribed_resources() ||
      !update->update_oversubscribed_resources()) {
    Future<UpdateSlaveMessage> update1 =
      FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

    Clock::advance(flags.oversubscribed_resources_interval);
    Clock::settle();

    AWAIT_READY(update1);
    update = update1;
  }

  ASSERT_TRUE(update->has_update_oversubscribed_resources());
  ASSERT_TRUE(update->update_oversubscribed_resources());

  Resources resources = update->oversubscribed_resources();
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
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
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
  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_STARTING, status->state());

  // Advance the clock for the slave to trigger the calculation of the
  // total oversubscribed resources. As we described above, we don't
  // expect a new UpdateSlaveMessage being generated.
  Clock::pause();
  Clock::advance(flags.oversubscribed_resources_interval);
  Clock::settle();
  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that the QoS Controller is able to fetch
// ResourceUsage statistics about running executor.
TEST_F(OversubscriptionTest, QoSFetchResourceUsage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
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

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      &containerizer,
      &controller,
      CreateSlaveFlags());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 10", DEFAULT_EXECUTOR_ID);
  task.mutable_executor()->mutable_labels()->add_labels()->CopyFrom(
      createLabel("key", "value"));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(usageCallback);

  Future<ResourceUsage> usage = usageCallback.get()();
  AWAIT_READY(usage);

  // Expecting the same statistics as these returned by mocked containerizer.
  ASSERT_EQ(1, usage->executors_size());
  EXPECT_EQ(usage->executors(0).executor_info().executor_id(),
            DEFAULT_EXECUTOR_ID);
  ASSERT_EQ(usage->executors(0).statistics(), statistics);
  ASSERT_EQ(task.executor().labels(),
            usage->executors(0).executor_info().labels());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Ensures the slave forwards the estimation whenever receiving
// a registered or reregistered message from the master, even
// if the total oversubscribable resources does not change.
TEST_F(OversubscriptionTest, Reregistration)
{
  loadFixedResourceEstimatorModule("cpus(*):2");

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resource_estimator = FIXED_RESOURCE_ESTIMATOR_NAME;

  Future<Nothing> slaveRecover = FUTURE_DISPATCH(_, &Slave::recover);

  StandaloneMasterDetector detector;

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, agentFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRecover);

  // Advance the clock for the slave to compute an estimate.
  Clock::pause();
  Clock::advance(agentFlags.oversubscribed_resources_interval);
  Clock::settle();

  // Start a master, we expect the slave to send the update
  // message after registering!
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegistered =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Future<UpdateSlaveMessage> update =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveRegistered);
  AWAIT_READY(update);
  ASSERT_TRUE(update->has_update_oversubscribed_resources());
  ASSERT_TRUE(update->update_oversubscribed_resources());

  Resources resources = update->oversubscribed_resources();
  EXPECT_SOME_EQ(2.0, resources.cpus());

  // Trigger a re-registration and expect another update message.
  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  update = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  detector.appoint(master.get()->pid);

  // Clock::settle();
  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregistered);
  AWAIT_READY(update);
  EXPECT_TRUE(update->has_update_oversubscribed_resources());
  EXPECT_TRUE(update->update_oversubscribed_resources());
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

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      &controller,
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  Future<list<QoSCorrection>> qosCorrections;
  EXPECT_CALL(*slave.get()->mock(), _qosCorrections(_))
    .WillOnce(FutureArg<0>(&qosCorrections));

  slave.get()->start();

  list<QoSCorrection> expected = { QoSCorrection() };
  corrections.put(expected);

  AWAIT_READY(qosCorrections);

  ASSERT_EQ(qosCorrections->size(), 1u);

  // TODO(nnielsen): Test for equality of QoSCorrections.
}


// This test verifies that a QoS controller can kill a running task,
// and that this results in sending a TASK_LOST status update with
// REASON_EXECUTOR_PREEMPTED if the framework is not partition-aware.
TEST_F(OversubscriptionTest, QoSCorrectionKill)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockQoSController controller;

  Queue<list<mesos::slave::QoSCorrection>> corrections;

  EXPECT_CALL(controller, corrections())
    .WillRepeatedly(InvokeWithoutArgs(
        &corrections,
        &Queue<list<mesos::slave::QoSCorrection>>::get));

  Future<lambda::function<Future<ResourceUsage>()>> usageCallback;

  // Catching callback which is passed to the QoS Controller.
  EXPECT_CALL(controller, initialize(_))
    .WillOnce(DoAll(FutureArg<0>(&usageCallback), Return(Nothing())));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &controller, CreateSlaveFlags());
  ASSERT_SOME(slave);

  // Verify presence and initial value of counter for preempted
  // executors.
  JSON::Object snapshot = Metrics();
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_preempted"));
  EXPECT_EQ(0u, snapshot.values["slave/executors_preempted"]);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 10");

  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status0);
  ASSERT_EQ(TASK_STARTING, status0->state());

  AWAIT_READY(status1);
  ASSERT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(usageCallback);

  Future<ResourceUsage> usage = usageCallback.get()();
  AWAIT_READY(usage);

  // Expecting the same statistics as these returned by mocked containerizer.
  ASSERT_EQ(1, usage->executors_size());

  const ResourceUsage::Executor& executor = usage->executors(0);
  // Carry out kill correction.
  QoSCorrection killCorrection;

  QoSCorrection::Kill* kill = killCorrection.mutable_kill();
  kill->mutable_framework_id()->CopyFrom(frameworkId.get());
  kill->mutable_executor_id()->CopyFrom(executor.executor_info().executor_id());
  kill->mutable_container_id()->CopyFrom(executor.container_id());

  corrections.put({killCorrection});

  // Verify task status is TASK_LOST.
  AWAIT_READY(status2);
  EXPECT_EQ(TASK_LOST, status2->state());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_PREEMPTED, status2->reason());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status2->source());

  // Verify that slave incremented metrics appropriately.
  snapshot = Metrics();
  EXPECT_EQ(1u, snapshot.values["slave/executors_preempted"]);
  EXPECT_EQ(1u, snapshot.values["slave/tasks_lost"]);
  EXPECT_EQ(0u, snapshot.values["slave/tasks_gone"]);

  driver.stop();
  driver.join();
}


// This test verifies that a QoS controller can kill a running task,
// and that this results in sending a TASK_GONE status update with
// REASON_EXECUTOR_PREEMPTED if the framework is partition-aware.
TEST_F(OversubscriptionTest, QoSCorrectionKillPartitionAware)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockQoSController controller;

  Queue<list<mesos::slave::QoSCorrection>> corrections;

  EXPECT_CALL(controller, corrections())
    .WillRepeatedly(InvokeWithoutArgs(
        &corrections,
        &Queue<list<mesos::slave::QoSCorrection>>::get));

  Future<lambda::function<Future<ResourceUsage>()>> usageCallback;

  // Catching callback which is passed to the QoS Controller.
  EXPECT_CALL(controller, initialize(_))
    .WillOnce(DoAll(FutureArg<0>(&usageCallback), Return(Nothing())));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &controller, CreateSlaveFlags());
  ASSERT_SOME(slave);

  // Verify presence and initial value of counter for preempted
  // executors.
  JSON::Object snapshot = Metrics();
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_preempted"));
  EXPECT_EQ(0u, snapshot.values["slave/executors_preempted"]);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 10");

  Future<TaskStatus> status0;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status0))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status0);
  ASSERT_EQ(TASK_STARTING, status0->state());

  AWAIT_READY(status1);
  ASSERT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(usageCallback);

  Future<ResourceUsage> usage = usageCallback.get()();
  AWAIT_READY(usage);

  // Expecting the same statistics as these returned by mocked containerizer.
  ASSERT_EQ(1, usage->executors_size());

  const ResourceUsage::Executor& executor = usage->executors(0);
  // Carry out kill correction.
  QoSCorrection killCorrection;

  QoSCorrection::Kill* kill = killCorrection.mutable_kill();
  kill->mutable_framework_id()->CopyFrom(frameworkId.get());
  kill->mutable_executor_id()->CopyFrom(executor.executor_info().executor_id());
  kill->mutable_container_id()->CopyFrom(executor.container_id());

  corrections.put({killCorrection});

  // Verify task status is TASK_GONE.
  AWAIT_READY(status2);
  EXPECT_EQ(TASK_GONE, status2->state());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_PREEMPTED, status2->reason());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status2->source());

  // Verify that slave incremented metrics appropriately.
  snapshot = Metrics();
  EXPECT_EQ(1u, snapshot.values["slave/executors_preempted"]);
  EXPECT_EQ(1u, snapshot.values["slave/tasks_gone"]);
  EXPECT_EQ(0u, snapshot.values["slave/tasks_lost"]);

  driver.stop();
  driver.join();
}


// This test verifies that when a framework reregisters with updated
// FrameworkInfo, it gets updated in the allocator. The steps involved
// are:
//   1. Launch a master, slave and scheduler.
//   2. Record FrameworkID of launched scheduler.
//   3. Check if revocable offers are being sent to the framework.
//   4. Launch a second scheduler which has the same FrameworkID as
//      the first scheduler and also has updated FrameworkInfo.
//   5. Check if revocable offers are being sent to the framework.
TEST_F(OversubscriptionTest, UpdateAllocatorOnSchedulerFailover)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  MockResourceEstimator resourceEstimator;

  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .WillOnce(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, &resourceEstimator, flags);
  ASSERT_SOME(slave);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler with updated information.

  FrameworkInfo framework1 = DEFAULT_FRAMEWORK_INFO;

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, framework1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  // Framework doesn't receive revocable resources because
  // it doesn't have the capability set.

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  EXPECT_TRUE(Resources(offers1.get()[0].resources()).revocable().empty());

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler, along with the
  // updated FrameworkInfo and wait until it gets a registered
  // callback.

  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());
  auto capabilityType = FrameworkInfo::Capability::REVOCABLE_RESOURCES;
  framework2.add_capabilities()->set_type(capabilityType);

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  // Scheduler1's expectations.

  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers1));

  // Initially the framework will get all regular resources.

  driver2.start();

  AWAIT_READY(sched2Registered);

  AWAIT_READY(sched1Error);

  // Advance the clock and trigger a batch allocation.
  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  EXPECT_TRUE(Resources(offers1.get()[0].resources()).revocable().empty());

  // Check if framework receives revocable offers.
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers2));

  Resources revocable = createRevocableResources("cpus", "2");
  estimations.put(revocable);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
  EXPECT_EQ(allocatedResources(revocable, framework2.roles(0)),
            Resources(offers2.get()[0].resources()));

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());
}


TEST_F(OversubscriptionTest, RemoveCapabilitiesOnSchedulerFailover)
{
  // Start the master.
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start the slave with mock executor and test resource estimator.
  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  MockResourceEstimator resourceEstimator;

  EXPECT_CALL(resourceEstimator, initialize(_));

  Queue<Resources> estimations;
  EXPECT_CALL(resourceEstimator, oversubscribable())
    .WillOnce(InvokeWithoutArgs(&estimations, &Queue<Resources>::get));

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, &resourceEstimator, flags);
  ASSERT_SOME(slave);

  // Start the framework which accepts revocable resources.
  FrameworkInfo framework1 = DEFAULT_FRAMEWORK_INFO;
  framework1.add_capabilities()->set_type(
      FrameworkInfo::Capability::REVOCABLE_RESOURCES);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, framework1, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  // Initially the framework will get all regular resources.
  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  EXPECT_TRUE(Resources(offers1.get()[0].resources()).revocable().empty());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Inject an estimation of oversubscribable cpu resources.
  Resources revocable = createRevocableResources("cpus", "2");
  estimations.put(revocable);

  // Now the framework will get revocable resources.
  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
  EXPECT_EQ(allocatedResources(revocable, framework1.roles(0)),
            Resources(offers2.get()[0].resources()));

  // Reregister the framework with removal of revocable resources capability.
  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, _, _));

  // Scheduler1's expectations.

  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  Future<vector<Offer>> offers3;
  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers3))
    .WillRepeatedly(Return());

  driver2.start();

  // Ensure resources are be recovered before a batch allocation is triggered.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  AWAIT_READY(offers3);
  ASSERT_FALSE(offers3->empty());
  EXPECT_TRUE(Resources(offers3.get()[0].resources()).revocable().empty());

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();
}


// This test verifies the functionality of the Load QoS Controller.
// If the total system load on the agent exceeds the configured threshold then
// it should evict all revocable executors.
// 1. Run first correction iteration with two revocable executors and the system
//    load below the thresholds. Eviction should not appear.
// 2. Run second correction iteration with the same executors and the system
//    5min load above the threshold. QoSCorrection message should appear.
TEST_F(OversubscriptionTest, LoadQoSController)
{
  // Configure Load QoS Controller. Revocable tasks will be killed when
  // the load 5min value will be above 7 or load 15min above 6.
  // This configuration could be a reasonable one for an 8 CPUs machine.
  const double loadThreshold5Min = 7;
  const double loadThreshold15Min = 6;

  // Prepare stubbed os::Load whose values are below thresholds.
  os::Load stubLoad;

  stubLoad.one = 1;
  stubLoad.five = loadThreshold5Min - 0.2;
  stubLoad.fifteen = loadThreshold15Min - 0.2;

  // Construct `LoadQoSController` with configured thresholds and fake
  // loadAverage lambda.
  LoadQoSController controller(loadThreshold5Min,
                               loadThreshold15Min,
                               [&stubLoad]() { return stubLoad; });

  // Prepare lambda creating ResourceUsage stub with two revocable executors.
  controller.initialize([this]() -> Future<ResourceUsage> {
    ResourceUsage usage;
    ResourceStatistics statistics = createResourceStatistics();

    Resources resources = Resources::parse("mem:128").get();
    resources += createRevocableResources("cpus", "1");

    // Prepare first revocable executor.
    ResourceUsage::Executor* executor = usage.add_executors();
    executor->mutable_executor_info()->CopyFrom(
        createExecutorInfo("framework", "executor1"));
    executor->mutable_allocated()->CopyFrom(resources);
    executor->mutable_statistics()->CopyFrom(statistics);

    // Prepare second revocable executor.
    resources = Resources::parse("mem:256").get();
    resources += createRevocableResources("cpus", "7");

    executor = usage.add_executors();
    executor->mutable_executor_info()->CopyFrom(
        createExecutorInfo("framework", "executor2"));
    executor->mutable_allocated()->CopyFrom(resources);
    executor->mutable_statistics()->CopyFrom(statistics);

    return usage;
  });

  // First correction iteration. All system loads are below the threshold.
  Future<list<QoSCorrection>> qosCorrections = controller.corrections();

  AWAIT(qosCorrections);

  // Expect no corrections.
  ASSERT_EQ(qosCorrections->size(), 0u);

  // Second correction iteration. Make system 5 minutes load above the
  // threshold.
  stubLoad.five = loadThreshold5Min + 0.2;
  qosCorrections = controller.corrections();

  AWAIT(qosCorrections);

  // Expect two corrections, since there were two revocable executors.
  ASSERT_EQ(qosCorrections->size(), 2u);
}


} // namespace tests {
} // namespace internal {
} // namespace mesos {
