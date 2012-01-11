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

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include "local/local.hpp"

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::PID;

using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::ElementsAre;
using testing::Return;
using testing::SaveArg;


TEST(ResourceOffersTest, ResourceOfferWithMultipleSlaves)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(10, 2, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());
  EXPECT_GE(10, offers.size());

  Resources resources(offers[0].resources());
  EXPECT_EQ(2, resources.get("cpus", Value::Scalar()).value());
  EXPECT_EQ(1024, resources.get("mem", Value::Scalar()).value());

  driver.stop();
  driver.join();

  local::shutdown();
}


TEST(ResourceOffersTest, TaskUsesNoResources)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  TaskStatus status;

  trigger statusUpdateCall;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(task.task_id(), status.task_id());
  EXPECT_EQ(TASK_LOST, status.state());
  EXPECT_TRUE(status.has_message());
  EXPECT_EQ("Task uses no resources", status.message());

  driver.stop();
  driver.join();

  local::shutdown();
}


TEST(ResourceOffersTest, TaskUsesInvalidResources)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Value::SCALAR);
  cpus->mutable_scalar()->set_value(0);

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  TaskStatus status;

  trigger statusUpdateCall;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(task.task_id(), status.task_id());
  EXPECT_EQ(TASK_LOST, status.state());
  EXPECT_TRUE(status.has_message());
  EXPECT_EQ("Task uses invalid resources", status.message());

  driver.stop();
  driver.join();

  local::shutdown();
}


TEST(ResourceOffersTest, TaskUsesMoreResourcesThanOffered)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Value::SCALAR);
  cpus->mutable_scalar()->set_value(2.01);

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  TaskStatus status;

  trigger statusUpdateCall;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(task.task_id(), status.task_id());
  EXPECT_EQ(TASK_LOST, status.state());
  EXPECT_TRUE(status.has_message());
  EXPECT_EQ("Task uses more resources than offered", status.message());

  driver.stop();
  driver.join();

  local::shutdown();
}


TEST(ResourceOffersTest, ResourcesGetReofferedWhenUnused)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;

  trigger sched1ResourceOfferCall;

  EXPECT_CALL(sched1, registered(&driver1, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&sched1ResourceOfferCall)))
    .WillRepeatedly(Return());

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  EXPECT_NE(0, offers.size());

  vector<TaskDescription> tasks; // Use nothing!

  driver1.launchTasks(offers[0].id(), tasks);

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, "", DEFAULT_EXECUTOR_INFO, master);

  trigger sched2ResourceOfferCall;

  EXPECT_CALL(sched2, registered(&driver2, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(Trigger(&sched2ResourceOfferCall))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2ResourceOfferCall);

  driver2.stop();
  driver2.join();

  local::shutdown();
}


TEST(ResourceOffersTest, ResourcesGetReofferedAfterTaskDescriptionError)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, "", DEFAULT_EXECUTOR_INFO, master);

  vector<Offer> offers;

  trigger sched1ResourceOffersCall;

  EXPECT_CALL(sched1, registered(&driver1, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&sched1ResourceOffersCall)))
    .WillRepeatedly(Return());

  driver1.start();

  WAIT_UNTIL(sched1ResourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskDescription task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Value::SCALAR);
  cpus->mutable_scalar()->set_value(0);

  Resource* mem = task.add_resources();
  mem->set_name("mem");
  mem->set_type(Value::SCALAR);
  mem->mutable_scalar()->set_value(1 * Gigabyte);

  vector<TaskDescription> tasks;
  tasks.push_back(task);

  TaskStatus status;

  trigger sched1StatusUpdateCall;

  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&sched1StatusUpdateCall)));

  driver1.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(sched1StatusUpdateCall);

  EXPECT_EQ(task.task_id(), status.task_id());
  EXPECT_EQ(TASK_LOST, status.state());
  EXPECT_TRUE(status.has_message());
  EXPECT_EQ("Task uses invalid resources", status.message());

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, "", DEFAULT_EXECUTOR_INFO, master);

  trigger sched2ResourceOffersCall;

  EXPECT_CALL(sched2, registered(&driver2, _))
    .Times(1);

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillOnce(Trigger(&sched2ResourceOffersCall))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  driver2.start();

  WAIT_UNTIL(sched2ResourceOffersCall);

  driver2.stop();
  driver2.join();

  local::shutdown();
}

// TODO(benh): Add tests for checking correct slave IDs.

// TODO(benh): Add tests for checking executor resource usage.

// TODO(benh): Add tests which launch multiple tasks and check for
// unique task IDs and aggregate resource usage.


TEST(ResourceOffersTest, ResourceRequest)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockScheduler sched;
  MockAllocator allocator;

  EXPECT_CALL(allocator, initialize(_))
    .WillOnce(Return());

  EXPECT_CALL(allocator, frameworkAdded(_))
    .WillOnce(Return());

  EXPECT_CALL(allocator, frameworkRemoved(_))
    .WillOnce(Return());

  EXPECT_CALL(allocator, slaveAdded(_))
    .WillRepeatedly(Return());

  EXPECT_CALL(allocator, slaveRemoved(_))
    .WillRepeatedly(Return());

  EXPECT_CALL(allocator, timerTick())
    .WillRepeatedly(Return());

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false, &allocator);

  MesosSchedulerDriver driver(&sched, "", DEFAULT_EXECUTOR_INFO, master);

  trigger registeredCall;

  EXPECT_CALL(sched, registered(&driver, _))
    .WillOnce(Trigger(&registeredCall));

  driver.start();

  WAIT_UNTIL(registeredCall);

  vector<ResourceRequest> requestsSent;
  vector<ResourceRequest> requestsReceived;
  ResourceRequest request;
  request.mutable_slave_id()->set_value("test");
  requestsSent.push_back(request);

  trigger resourcesRequestedCall;

  EXPECT_CALL(allocator, resourcesRequested(_, _))
    .WillOnce(DoAll(SaveArg<1>(&requestsReceived),
                    Trigger(&resourcesRequestedCall)));

  driver.requestResources(requestsSent);

  WAIT_UNTIL(resourcesRequestedCall);

  EXPECT_EQ(requestsSent.size(), requestsReceived.size());
  EXPECT_NE(0, requestsReceived.size());
  EXPECT_EQ(request.slave_id(), requestsReceived[0].slave_id());

  driver.stop();
  driver.join();

  local::shutdown();
}
