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

#include <map>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "master/dominant_share_allocator.hpp"
#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;
using mesos::internal::master::DominantShareAllocator;

using mesos::internal::slave::Slave;

using process::PID;

using std::map;
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
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
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
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
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
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Value::SCALAR);
  cpus->mutable_scalar()->set_value(0);

  vector<TaskInfo> tasks;
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
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Value::SCALAR);
  cpus->mutable_scalar()->set_value(2.01);

  vector<TaskInfo> tasks;
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
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;

  trigger sched1ResourceOfferCall;

  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&sched1ResourceOfferCall)))
    .WillRepeatedly(Return());

  driver1.start();

  WAIT_UNTIL(sched1ResourceOfferCall);

  EXPECT_NE(0, offers.size());

  vector<TaskInfo> tasks; // Use nothing!

  driver1.launchTasks(offers[0].id(), tasks);

  driver1.stop();
  driver1.join();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, DEFAULT_FRAMEWORK_INFO, master);

  trigger sched2ResourceOfferCall;

  EXPECT_CALL(sched2, registered(&driver2, _, _))
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


TEST(ResourceOffersTest, ResourcesGetReofferedAfterTaskInfoError)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;

  trigger sched1ResourceOffersCall;

  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .Times(1);

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&sched1ResourceOffersCall)))
    .WillRepeatedly(Return());

  driver1.start();

  WAIT_UNTIL(sched1ResourceOffersCall);

  EXPECT_NE(0, offers.size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Value::SCALAR);
  cpus->mutable_scalar()->set_value(0);

  Resource* mem = task.add_resources();
  mem->set_name("mem");
  mem->set_type(Value::SCALAR);
  mem->mutable_scalar()->set_value(1 * Gigabyte);

  vector<TaskInfo> tasks;
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
  MesosSchedulerDriver driver2(&sched2, DEFAULT_FRAMEWORK_INFO, master);

  trigger sched2ResourceOffersCall;

  EXPECT_CALL(sched2, registered(&driver2, _, _))
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


TEST(ResourceOffersTest, Request)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MockScheduler sched;
  MockAllocator<DominantShareAllocator> allocator;

  EXPECT_CALL(allocator, initialize(_))
    .WillOnce(Return());

  EXPECT_CALL(allocator, frameworkAdded(_, _))
    .WillOnce(Return());

  EXPECT_CALL(allocator, frameworkDeactivated(_))
    .WillRepeatedly(Return());

  EXPECT_CALL(allocator, frameworkRemoved(_))
    .WillRepeatedly(Return());

  EXPECT_CALL(allocator, slaveAdded(_, _, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(allocator, slaveRemoved(_))
    .WillRepeatedly(Return());

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, false, &allocator);

  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger registeredCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(Trigger(&registeredCall));

  driver.start();

  WAIT_UNTIL(registeredCall);

  vector<Request> requestsSent;
  vector<Request> requestsReceived;
  Request request;
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


TEST(ResourceOffersTest, TasksExecutorInfoDiffers)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DominantShareAllocator a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;

  trigger shutdownCall;

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(AtMost(1));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillRepeatedly(Return()); // Test expects we won't send any status updates!

  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(Trigger(&shutdownCall));

  map<ExecutorID, Executor*> execs;
  execs[DEFAULT_EXECUTOR_ID] = &exec;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  vector<Offer> offers;

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  EXPECT_NE(0, offers.size());

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value("exit 1");

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task1.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task1.mutable_executor()->MergeFrom(executor);

  executor.mutable_command()->set_value("exit 2");

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task2.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:512"));
  task2.mutable_executor()->MergeFrom(executor);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  TaskStatus status;

  trigger statusUpdateCall;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(task2.task_id(), status.task_id());
  EXPECT_EQ(TASK_LOST, status.state());
  EXPECT_TRUE(status.has_message());
  EXPECT_EQ("Task has invalid ExecutorInfo (existing ExecutorInfo"
            " with same ExecutorID is not compatible)", status.message());

  driver.stop();
  driver.join();

  WAIT_UNTIL(shutdownCall); // To ensure can deallocate MockExecutor.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}
