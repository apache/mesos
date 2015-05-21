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

#include <gmock/gmock.h>

#include <mesos/resources.hpp>

#include <process/clock.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>

#include "master/master.hpp"

#include "messages/messages.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

namespace mesos {
namespace internal {
namespace tests {

class OversubscriptionTest : public MesosTest {};


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
  Resources resources = Resources::parse("cpus:1;mem:32").get();
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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
