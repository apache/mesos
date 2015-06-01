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
#include "slave/slave.hpp"

#include "tests/flags.hpp"
#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::slave::QoSCorrection;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;
using testing::Invoke;

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

private:
  string originalLDLibraryPath;
  Modules modules;
};


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
    .WillOnce(Invoke(&estimations, &Queue<Resources>::get));

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
    .WillOnce(Invoke(&estimations, &Queue<Resources>::get));

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
    .WillOnce(FutureArg<1>(&status));

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
    .WillRepeatedly(Invoke(&estimations, &Queue<Resources>::get));

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
    .WillRepeatedly(Invoke(&corrections, &Queue<list<QoSCorrection>>::get));

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer, &controller);

  Future<list<QoSCorrection>> qosCorrections;
  EXPECT_CALL(slave, qosCorrections(_))
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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
