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

#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <stout/strings.hpp>

#include "master/master.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "slave/slave.hpp"

#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Future;
using process::PID;

using std::vector;

using testing::_;

namespace mesos {
namespace internal {
namespace tests {


class ResourceOffersTest : public MesosTest {};


TEST_F(ResourceOffersTest, ResourceOfferWithMultipleSlaves)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start 10 slaves.
  for (int i = 0; i < 10; i++) {
    slave::Flags flags = CreateSlaveFlags();

    flags.resources = Option<std::string>("cpus:2;mem:1024");

    Try<PID<Slave>> slave = StartSlave(flags);
    ASSERT_SOME(slave);
  }

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // All 10 slaves might not be in first offer.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  EXPECT_GE(10u, offers.get().size());

  Resources resources(offers.get()[0].resources());
  EXPECT_EQ(2, resources.get<Value::Scalar>("cpus").get().value());
  EXPECT_EQ(1024, resources.get<Value::Scalar>("mem").get().value());

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(ResourceOffersTest, ResourcesGetReofferedAfterFrameworkStops)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  driver1.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, _, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  AWAIT_READY(offers);

  driver2.stop();
  driver2.join();

  Shutdown();
}


TEST_F(ResourceOffersTest, ResourcesGetReofferedWhenUnused)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  driver1.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  vector<TaskInfo> tasks; // Use nothing!
  driver1.launchTasks(offers.get()[0].id(), tasks);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, _, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  AWAIT_READY(offers);

  // Stop first framework before second so no offers are sent.
  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  Shutdown();
}


TEST_F(ResourceOffersTest, ResourcesGetReofferedAfterTaskInfoError)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver1.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Value::SCALAR);
  cpus->mutable_scalar()->set_value(-1);

  Resource* mem = task.add_resources();
  mem->set_name("mem");
  mem->set_type(Value::SCALAR);
  mem->mutable_scalar()->set_value(Gigabytes(1).bytes());

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&status));

  driver1.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status.get().task_id());
  EXPECT_EQ(TASK_ERROR, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status.get().reason());
  EXPECT_TRUE(status.get().has_message());
  EXPECT_TRUE(strings::startsWith(
        status.get().message(), "Task uses invalid resources"));

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, _, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver2.start();

  AWAIT_READY(offers);

  driver1.stop();
  driver1.join();

  driver2.stop();
  driver2.join();

  Shutdown();
}


TEST_F(ResourceOffersTest, Request)
{
  TestAllocator<master::allocator::HierarchicalDRFAllocator> allocator;

  EXPECT_CALL(allocator, initialize(_, _, _))
    .Times(1);

  Try<PID<Master>> master = StartMaster(&allocator);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(allocator, addFramework(_, _, _))
    .Times(1);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  vector<Request> sent;
  Request request;
  request.mutable_slave_id()->set_value("test");
  sent.push_back(request);

  Future<vector<Request>> received;
  EXPECT_CALL(allocator, requestResources(_, _))
    .WillOnce(FutureArg<1>(&received));

  driver.requestResources(sent);

  AWAIT_READY(received);
  EXPECT_EQ(sent.size(), received.get().size());
  EXPECT_NE(0u, received.get().size());
  EXPECT_EQ(request.slave_id(), received.get()[0].slave_id());

  driver.stop();
  driver.join();

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
