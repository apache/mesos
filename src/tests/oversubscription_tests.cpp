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

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/resources.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>

#include "common/resources_utils.hpp"

#include "master/master.hpp"

#include "messages/messages.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

class OversubscriptionTest : public MesosTest
{
protected:
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
};


// This test verifies that slave will forward the estimation of the
// oversubscribed resources to the master.
TEST_F(OversubscriptionTest, ForwardUpdateSlaveMessage)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegistered =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  TestResourceEstimator resourceEstimator;

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
  resourceEstimator.estimate(resources);

  AWAIT_READY(update);
  EXPECT_EQ(Resources(update.get().oversubscribed_resources()), resources);

  // Ensure the metric is updated.
  JSON::Object metrics = Metrics();
  ASSERT_EQ(
      1u,
      metrics.values.count("master/messages_update_slave"));
  ASSERT_EQ(
      1u,
      metrics.values["master/messages_update_slave"]);

  Shutdown();
}


// This test verifies that a framework that desires revocable
// resources gets an offer with revocable resources.
TEST_F(OversubscriptionTest, RevocableOffer)
{
  // Start the master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start the slave with test resource estimator.
  TestResourceEstimator resourceEstimator;
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
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Inject an estimation of oversubscribable resources.
  Resources resources = createRevocableResources("cpus", "1");
  resourceEstimator.estimate(resources);

  // Now the framework will get revocable resources.
  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());
  EXPECT_EQ(resources, Resources(offers2.get()[0].resources()));

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
  TestResourceEstimator resourceEstimator;
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
  resourceEstimator.estimate(resources);

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
  resourceEstimator.estimate(resources2);

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
