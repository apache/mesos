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

#include <unistd.h>

#include <gmock/gmock.h>

#include <map>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>
#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/stringify.hpp>

#include "common/protobuf_utils.hpp"

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::protobuf;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::STATUS_UPDATE_RETRY_INTERVAL_MIN;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::UPID;

using std::string;
using std::map;
using std::vector;

using testing::_;
using testing::AnyOf;
using testing::AtMost;
using testing::DoAll;
using testing::ElementsAre;
using testing::Eq;
using testing::Not;
using testing::Return;
using testing::SaveArg;


class FaultToleranceTest : public MesosTest {};


// This test checks that when a slave is lost,
// its offer(s) is rescinded.
TEST_F(FaultToleranceTest, SlaveLost)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers.get().size());

  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, offers.get()[0].id()))
    .WillOnce(FutureSatisfy(&offerRescinded));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, offers.get()[0].slave_id()))
    .WillOnce(FutureSatisfy(&slaveLost));

  ShutdownSlaves();

  AWAIT_READY(offerRescinded);
  AWAIT_READY(slaveLost);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test checks that a scheduler gets a slave lost
// message for a partioned slave.
TEST_F(FaultToleranceTest, PartitionedSlave)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_MESSAGES(Eq("PONG"), _, _);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(resourceOffers);

  Clock::pause();

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Now advance through the PINGs.
  uint32_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == master::MAX_SLAVE_PING_TIMEOUTS) {
     break;
    }
    ping = FUTURE_MESSAGE(Eq("PING"), _, _);
    Clock::advance(master::SLAVE_PING_TIMEOUT);
  }

  Clock::advance(master::SLAVE_PING_TIMEOUT);

  AWAIT_READY(slaveLost);

  driver.stop();
  driver.join();

  Shutdown();

  Clock::resume();
}


// The purpose of this test is to ensure that when slaves are removed
// from the master, and then attempt to re-register, we deny the
// re-registration by sending a ShutdownMessage to the slave.
// Why? Because during a network partition, the master will remove a
// partitioned slave, thus sending its tasks to LOST. At this point,
// when the partition is removed, the slave will attempt to
// re-register with its running tasks. We've already notified
// frameworks that these tasks were LOST, so we have to have the slave
// slave shut down.
TEST_F(FaultToleranceTest, PartitionedSlaveReregistration)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the SlaveObserver Process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);
  DROP_MESSAGES(Eq("PONG"), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  StandaloneMasterDetector* detector =
    new StandaloneMasterDetector(master.get());

  Try<PID<Slave> > slave = StartSlave(&exec, Owned<MasterDetector>(detector));
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  // Launch a task. This is to ensure the task is killed by the slave,
  // during shutdown.
  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  task.mutable_executor()->mutable_command()->set_value("sleep 60");

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Set up the expectations for launching the task.
  EXPECT_CALL(exec, registered(_, _, _, _));
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck = FUTURE_DISPATCH(
      slave.get(), &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus.get().state());

  // Wait for the slave to have handled the acknowledgment prior
  // to pausing the clock.
  AWAIT_READY(statusUpdateAck);

  // Drop the first shutdown message from the master (simulated
  // partition), allow the second shutdown message to pass when
  // the slave re-registers.
  Future<ShutdownMessage> shutdownMessage =
    DROP_PROTOBUF(ShutdownMessage(), _, slave.get());

  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  uint32_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == master::MAX_SLAVE_PING_TIMEOUTS) {
     break;
    }
    ping = FUTURE_MESSAGE(Eq("PING"), _, _);
    Clock::advance(master::SLAVE_PING_TIMEOUT);
    Clock::settle();
  }

  Clock::advance(master::SLAVE_PING_TIMEOUT);
  Clock::settle();

  // The master will have notified the framework of the lost task.
  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus.get().state());

  // Wait for the master to attempt to shut down the slave.
  AWAIT_READY(shutdownMessage);

  // The master will notify the framework that the slave was lost.
  AWAIT_READY(slaveLost);

  // We now complete the partition on the slave side as well. This
  // is done by simulating a master loss event which would normally
  // occur during a network partition.
  detector->appoint(None());

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  shutdownMessage = FUTURE_PROTOBUF(ShutdownMessage(), _, slave.get());

  // Have the slave re-register with the master.
  detector->appoint(master.get());

  // Upon re-registration, the master will shutdown the slave.
  // The slave will then shut down the executor.
  AWAIT_READY(shutdownMessage);
  AWAIT_READY(shutdown);

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown();
}


// The purpose of this test is to ensure that when slaves are removed
// from the master, and then attempt to send status updates, we send
// a ShutdownMessage to the slave. Why? Because during a network
// partition, the master will remove a partitioned slave, thus sending
// its tasks to LOST. At this point, when the partition is removed,
// the slave may attempt to send updates if it was unaware that the
// master deactivated it. We've already notified frameworks that these
// tasks were LOST, so we have to have the slave shut down.
TEST_F(FaultToleranceTest, PartitionedSlaveStatusUpdates)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the SlaveObserver Process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);
  DROP_MESSAGES(Eq("PONG"), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage.get().slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(frameworkId);

  // Drop the first shutdown message from the master (simulated
  // partition), allow the second shutdown message to pass when
  // the slave sends an update.
  Future<ShutdownMessage> shutdownMessage =
    DROP_PROTOBUF(ShutdownMessage(), _, slave.get());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  uint32_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == master::MAX_SLAVE_PING_TIMEOUTS) {
     break;
    }
    ping = FUTURE_MESSAGE(Eq("PING"), _, _);
    Clock::advance(master::SLAVE_PING_TIMEOUT);
    Clock::settle();
  }

  Clock::advance(master::SLAVE_PING_TIMEOUT);
  Clock::settle();

  // Wait for the master to attempt to shut down the slave.
  AWAIT_READY(shutdownMessage);

  // The master will notify the framework that the slave was lost.
  AWAIT_READY(slaveLost);

  shutdownMessage = FUTURE_PROTOBUF(ShutdownMessage(), _, slave.get());

  // At this point, the slave still thinks it's registered, so we
  // simulate a status update coming from the slave.
  TaskID taskId;
  taskId.set_value("task_id");
  const StatusUpdate& update = createStatusUpdate(
      frameworkId.get(), slaveId, taskId, TASK_RUNNING);

  StatusUpdateMessage message;
  message.mutable_update()->CopyFrom(update);
  message.set_pid(stringify(slave.get()));

  process::post(master.get(), message);

  // The master should shutdown the slave upon receiving the update.
  AWAIT_READY(shutdownMessage);

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown();
}


// The purpose of this test is to ensure that when slaves are removed
// from the master, and then attempt to send exited executor messages,
// we send a ShutdownMessage to the slave. Why? Because during a
// network partition, the master will remove a partitioned slave, thus
// sending its tasks to LOST. At this point, when the partition is
// removed, the slave may attempt to send exited executor messages if
// it was unaware that the master deactivated it. We've already
// notified frameworks that the tasks under the executors were LOST,
// so we have to have the slave shut down.
TEST_F(FaultToleranceTest, PartitionedSlaveExitedExecutor)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the SlaveObserver Process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);
  DROP_MESSAGES(Eq("PONG"), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));\

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  // Launch a task. This allows us to have the slave send an
  // ExitedExecutorMessage.
  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  task.mutable_executor()->mutable_command()->set_value("sleep 60");

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // Set up the expectations for launching the task.
  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop all the status updates from the slave, so that we can
  // ensure the ExitedExecutorMessage is what triggers the slave
  // shutdown.
  DROP_PROTOBUFS(StatusUpdateMessage(), _, master.get());

  driver.launchTasks(offers.get()[0].id(), tasks);

  // Drop the first shutdown message from the master (simulated
  // partition) and allow the second shutdown message to pass when
  // triggered by the ExitedExecutorMessage.
  Future<ShutdownMessage> shutdownMessage =
    DROP_PROTOBUF(ShutdownMessage(), _, slave.get());

  Future<TaskStatus> lostStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&lostStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  uint32_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == master::MAX_SLAVE_PING_TIMEOUTS) {
     break;
    }
    ping = FUTURE_MESSAGE(Eq("PING"), _, _);
    Clock::advance(master::SLAVE_PING_TIMEOUT);
    Clock::settle();
  }

  Clock::advance(master::SLAVE_PING_TIMEOUT);
  Clock::settle();

  // The master will have notified the framework of the lost task.
  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus.get().state());

  // Wait for the master to attempt to shut down the slave.
  AWAIT_READY(shutdownMessage);

  // The master will notify the framework that the slave was lost.
  AWAIT_READY(slaveLost);

  shutdownMessage = FUTURE_PROTOBUF(ShutdownMessage(), _, slave.get());

  // Induce an ExitedExecutorMessage from the slave.
  containerizer.destroy(
      frameworkId.get(), DEFAULT_EXECUTOR_INFO.executor_id());

  // Upon receiving the message, the master will shutdown the slave.
  AWAIT_READY(shutdownMessage);

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that a framework connecting with a
// failed over master gets a registered callback.
// Note that this behavior might change in the future and
// the scheduler might receive a re-registered callback.
TEST_F(FaultToleranceTest, MasterFailover)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  StandaloneMasterDetector* detector =
    new StandaloneMasterDetector(master.get());
  TestingMesosSchedulerDriver driver(&sched, detector);

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  Future<Nothing> registered1;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered1));

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);

  AWAIT_READY(registered1);

  // Simulate failed over master by restarting the master.
  Stop(master.get());
  master = StartMaster();
  ASSERT_SOME(master);

  EXPECT_CALL(sched, disconnected(&driver));

  Future<AuthenticateMessage> authenticateMessage =
    FUTURE_PROTOBUF(AuthenticateMessage(), _, _);

  Future<Nothing> registered2;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered2));

  // Simulate a new master detected message to the scheduler.
  detector->appoint(master.get());

  // Scheduler should retry authentication.
  AWAIT_READY(authenticateMessage);

  // Framework should get a registered callback.
  AWAIT_READY(registered2);

  driver.stop();
  driver.join();

  delete detector;

  Shutdown();
}


TEST_F(FaultToleranceTest, SchedulerFailover)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillRepeatedly(Return());

  driver1.start();

  AWAIT_READY(frameworkId);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  // Scheduler1's expectations.
  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  driver2.start();

  AWAIT_READY(sched2Registered);

  AWAIT_READY(sched1Error);

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());

  Shutdown();
}


// This test was added to cover a fix for MESOS-659.
// Here, we drop the initial FrameworkReregisteredMessage from the
// master, so that the scheduler driver retries the initial failover
// re-registration. Previously, this caused a "Framework failed over"
// to be sent to the new scheduler driver!
TEST_F(FaultToleranceTest, SchedulerFailoverRetriedReregistration)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver1.start();

  AWAIT_READY(frameworkId);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get(), DEFAULT_CREDENTIAL);

  Clock::pause();

  // Drop the initial FrameworkRegisteredMessage to the failed over
  // scheduler. This ensures the scheduler driver will retry the
  // registration.
  Future<process::Message> reregistrationMessage = DROP_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  // There should be no error received, the master sends the error
  // prior to sending the FrameworkRegisteredMessage so we don't
  // need to wait to ensure this does not occur.
  EXPECT_CALL(sched2, error(&driver2, "Framework failed over"))
    .Times(0);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  driver2.start();

  AWAIT_READY(reregistrationMessage);

  // Trigger the re-registration retry.
  Clock::advance(Seconds(1));

  AWAIT_READY(sched2Registered);

  AWAIT_READY(sched1Error);

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());

  Shutdown();
  Clock::resume();
}


TEST_F(FaultToleranceTest, FrameworkReliableRegistration)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<AuthenticateMessage> authenticateMessage =
    FUTURE_PROTOBUF(AuthenticateMessage(), _, master.get());

  // Drop the first framework registered message, allow subsequent messages.
  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    DROP_PROTOBUF(FrameworkRegisteredMessage(), master.get(), _);

  driver.start();

  // Ensure authentication occurs.
  AWAIT_READY(authenticateMessage);

  AWAIT_READY(frameworkRegisteredMessage);

  // TODO(benh): Pull out constant from SchedulerProcess.
  Clock::pause();
  Clock::advance(Seconds(1));

  AWAIT_READY(registered); // Ensures registered message is received.

  driver.stop();
  driver.join();

  Shutdown();

  Clock::resume();
}


TEST_F(FaultToleranceTest, FrameworkReregister)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> slaveDetector(
      new StandaloneMasterDetector(master.get()));
  Try<PID<Slave> > slave = StartSlave(slaveDetector);
  ASSERT_SOME(slave);


  // Create a detector for the scheduler driver because we want the
  // spurious leading master change to be known by the scheduler
  // driver only.
  Owned<MasterDetector> schedDetector(
      new StandaloneMasterDetector(master.get()));
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, schedDetector.get());

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message); // Framework registered message, to get the pid.
  AWAIT_READY(registered); // Framework registered call.

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  Future<Nothing> reregistered;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureSatisfy(&reregistered));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  // Simulate a spurious leading master change at the scheduler.
  dynamic_cast<StandaloneMasterDetector*>(schedDetector.get())->appoint(
      master.get());

  AWAIT_READY(disconnected);

  AWAIT_READY(reregistered);

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(FaultToleranceTest, TaskLost)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  StandaloneMasterDetector* detector =
    new StandaloneMasterDetector(master.get());
  TestingMesosSchedulerDriver driver(&sched, detector);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  AWAIT_READY(message);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious master loss event at the scheduler.
  detector->appoint(None());

  AWAIT_READY(disconnected);

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test checks that a failover scheduler gets the
// retried status update.
TEST_F(FaultToleranceTest, SchedulerFailoverStatusUpdate)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver1.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop the first status update message
  // between master and the scheduler.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), _, Not(AnyOf(Eq(master.get()), Eq(slave.get()))));

  driver1.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusUpdateMessage);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // registers.

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered2));

  // Scheduler1 should get an error due to failover.
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"));

  // Scheduler2 should receive retried status updates.
  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureSatisfy(&statusUpdate))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver2.start();

  AWAIT_READY(registered2);

  Clock::pause();

  // Now advance time enough for the reliable timeout
  // to kick in and another status update is sent.
  Clock::advance(STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(statusUpdate);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  Shutdown();

  Clock::resume();
}


// This test was added to ensure MESOS-420 is fixed.
// We need to make sure that the master correctly handles non-terminal
// tasks with exited executors upon framework re-registration. This is
// possible because the ExitedExecutor message can arrive before the
// terminal status update(s) of its task(s).
TEST_F(FaultToleranceTest, ReregisterFrameworkExitedExecutor)
{
  // First we'll start a master and slave, then register a framework
  // so we can launch a task.
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> slaveDetector(
      new StandaloneMasterDetector(master.get()));
  Try<PID<Slave> > slave = StartSlave(&containerizer, slaveDetector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  Owned<StandaloneMasterDetector> schedDetector(
      new StandaloneMasterDetector(master.get()));
  TestingMesosSchedulerDriver driver(&sched, schedDetector.get());

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&statusUpdate));    // TASK_RUNNING.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);

  // Wait until TASK_RUNNING of the task is received.
  AWAIT_READY(statusUpdate);

  EXPECT_CALL(sched, disconnected(&driver));

  // Now that the task is launched, we need to induce the following:
  //   1. ExitedExecutorMessage received by the master prior to a
  //      terminal status update for the corresponding task. This
  //      means we need to drop the status update coming from the
  //      slave.
  //   2. Framework re-registration.
  //
  // To achieve this, we need to:
  //   1. Restart the master (the slave / framework will not detect
  //      the new master automatically using the BasicMasterDetector).
  //   2. Notify the slave of the new master.
  //   3. Kill the executor.
  //   4. Drop the status update, but allow the ExitedExecutorMessage.
  //   5. Notify the framework of the new master.
  Stop(master.get());
  master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  dynamic_cast<StandaloneMasterDetector*>(slaveDetector.get())->appoint(
      master.get());

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage);

  // Allow the executor exited message and drop the status update,
  // it's possible for a duplicate update to occur if the status
  // update manager is notified of the new master after the task was
  // killed.
  Future<ExitedExecutorMessage> executorExitedMessage =
    FUTURE_PROTOBUF(ExitedExecutorMessage(), _, _);
  DROP_PROTOBUFS(StatusUpdateMessage(), _, _);

  // Now kill the executor.
  containerizer.destroy(frameworkId, DEFAULT_EXECUTOR_ID);

  AWAIT_READY(executorExitedMessage);

  // Now notify the framework of the new master.
  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage2 =
    FUTURE_PROTOBUF(FrameworkRegisteredMessage(), _, _);

  EXPECT_CALL(sched, registered(&driver, _, _));

  schedDetector->appoint(master.get());

  AWAIT_READY(frameworkRegisteredMessage2);

  driver.stop();
  driver.join();
  Shutdown();
}


TEST_F(FaultToleranceTest, ForwardStatusUpdateUnknownExecutor)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&statusUpdate));    // TASK_RUNNING of task1.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offer.id(), tasks);

  // Wait until TASK_RUNNING of task1 is received.
  AWAIT_READY(statusUpdate);

  // Simulate the slave receiving status update from an unknown
  // (e.g. exited) executor of the given framework.
  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));           // TASK_RUNNING of task2.

  TaskID taskId;
  taskId.set_value("task2");

  StatusUpdate statusUpdate2 = createStatusUpdate(
      frameworkId, offer.slave_id(), taskId, TASK_RUNNING, "Dummy update");

  process::dispatch(slave.get(), &Slave::statusUpdate, statusUpdate2, UPID());

  // Ensure that the scheduler receives task2's update.
  AWAIT_READY(status);
  EXPECT_EQ(taskId, status.get().task_id());
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(FaultToleranceTest, SchedulerFailoverFrameworkMessage)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
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
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&status));

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver1.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> frameworkMessage;
  EXPECT_CALL(sched2, frameworkMessage(&driver2, _, _, _))
    .WillOnce(FutureSatisfy(&frameworkMessage));

  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"));

  Future<UpdateFrameworkMessage> updateFrameworkMessage =
    FUTURE_PROTOBUF(UpdateFrameworkMessage(), _, _);

  driver2.start();

  AWAIT_READY(registered);

  // Wait for the slave to get the updated framework pid.
  AWAIT_READY(updateFrameworkMessage);

  execDriver->sendFrameworkMessage("Executor to Framework message");

  AWAIT_READY(frameworkMessage);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  Shutdown();
}


// This test verifies that a partitioned framework that still
// thinks it is registered with the master cannot kill a task because
// the master has re-registered another instance of the framework.
// What this test does:
// 1. Launch a master, slave and scheduler.
// 2. Scheduler launches a task.
// 3. Launch a second failed over scheduler.
// 4. Make the first scheduler believe it is still registered.
// 5. First scheduler attempts to kill the task which is ignored by the master.
TEST_F(FaultToleranceTest, IgnoreKillTaskFromUnregisteredFramework)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  // Start the first scheduler and launch a task.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&status));

  Future<StatusUpdateAcknowledgementMessage> statusUpdateAcknowledgementMessage
    = FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver1.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Wait for the status update acknowledgement to be sent. This
  // ensures the slave doesn't resend the TASK_RUNNING update to the
  // failed over scheduler (below).
  AWAIT_READY(statusUpdateAcknowledgementMessage);

  // Now start the second failed over scheduler.
  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillRepeatedly(Return()); // Ignore any offers.

  // Drop the framework error message from the master to simulate
  // a partitioned framework.
  Future<FrameworkErrorMessage> frameworkErrorMessage =
    DROP_PROTOBUF(FrameworkErrorMessage(), _ , _);

  driver2.start();

  AWAIT_READY(frameworkErrorMessage);

  AWAIT_READY(registered);

  // Now both the frameworks think they are registered with the
  // master, but the master only knows about the second framework.

  // A 'killTask' by first framework should be dropped by the master.
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .Times(0);

  // 'TASK_FINSIHED' by the executor should reach the second framework.
  Future<TaskStatus> status2;
  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureArg<1>(&status2));

  Future<KillTaskMessage> killTaskMessage =
    FUTURE_PROTOBUF(KillTaskMessage(), _, _);

  driver1.killTask(status.get().task_id());

  AWAIT_READY(killTaskMessage);

  // By this point the master must have processed and ignored the
  // 'killTask' message from the first framework. To verify this,
  // the executor sends 'TASK_FINISHED' to ensure the only update
  // received by the scheduler is 'TASK_FINISHED' and not
  // 'TASK_KILLED'.
  TaskStatus finishedStatus;
  finishedStatus = status.get();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_FINISHED, status2.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  Shutdown();
}


// This test checks that a scheduler exit shuts down the executor.
TEST_F(FaultToleranceTest, SchedulerExit)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  AWAIT_READY(offers);

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(FaultToleranceTest, SlaveReliableRegistration)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Drop the first slave registered message, allow subsequent messages.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    DROP_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<PID<Slave> > slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(registered);

  AWAIT_READY(slaveRegisteredMessage);

  Clock::pause();
  Clock::advance(Seconds(1)); // TODO(benh): Pull out constant from Slave.

  AWAIT_READY(resourceOffers);

  driver.stop();
  driver.join();

  Shutdown();

  Clock::resume();
}


TEST_F(FaultToleranceTest, SlaveReregisterOnZKExpiration)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  StandaloneMasterDetector* detector = new StandaloneMasterDetector(master.get());
  Try<PID<Slave> > slave = StartSlave(Owned<MasterDetector>(detector));
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return());  // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(resourceOffers);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave.
  detector->appoint(master.get());

  AWAIT_READY(slaveReregisteredMessage);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that a re-registering slave does not inform
// the master about a terminated executor (and its tasks), when the
// executor has pending updates. We check this by ensuring that the
// master sends a TASK_LOST update for the task belonging to the
// terminated executor.
TEST_F(FaultToleranceTest, SlaveReregisterTerminatedExecutor)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector* detector =
    new StandaloneMasterDetector(master.get());
  Try<PID<Slave> > slave =
    StartSlave(&containerizer, Owned<MasterDetector>(detector));
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Drop the TASK_LOST status update(s) sent to the master.
  // This ensures that the TASK_LOST received by the scheduler
  // is generated by the master.
  DROP_PROTOBUFS(StatusUpdateMessage(), _, master.get());

  Future<ExitedExecutorMessage> executorExitedMessage =
    FUTURE_PROTOBUF(ExitedExecutorMessage(), _, _);

  // Now kill the executor.
  containerizer.destroy(frameworkId.get(), DEFAULT_EXECUTOR_ID);

  AWAIT_READY(executorExitedMessage);

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  detector->appoint(master.get());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_LOST, status2.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the master sends TASK_LOST updates
// for tasks in the master absent from the re-registered slave.
// We do this by dropping RunTaskMessage from master to the slave.
TEST_F(FaultToleranceTest, ReconcileLostTasks)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector* detector =
    new StandaloneMasterDetector(master.get());
  Try<PID<Slave> > slave = StartSlave(Owned<MasterDetector>(detector));
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);

  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  // We now launch a task and drop the corresponding RunTaskMessage on
  // the slave, to ensure that only the master knows about this task.
  Future<RunTaskMessage> runTaskMessage =
    DROP_PROTOBUF(RunTaskMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(runTaskMessage);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector->appoint(master.get());

  AWAIT_READY(slaveReregisteredMessage);

  AWAIT_READY(status);

  ASSERT_EQ(task.task_id(), status.get().task_id());
  ASSERT_EQ(TASK_LOST, status.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that when the slave re-registers, the master
// does not send TASK_LOST update for a task that has reached terminal
// state but is waiting for an acknowledgement.
TEST_F(FaultToleranceTest, ReconcileIncompleteTasks)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  StandaloneMasterDetector* detector =
    new StandaloneMasterDetector(master.get());
  Try<PID<Slave> > slave = StartSlave(&exec, Owned<MasterDetector>(detector));
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Send a terminal update right away.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  // Drop the status update from slave to the master, so that
  // the slave has a pending terminal update when it re-registers.
  DROP_PROTOBUF(StatusUpdateMessage(), _, master.get());

  Future<Nothing> _statusUpdate = FUTURE_DISPATCH(_, &Slave::_statusUpdate);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore retried update due to update framework.

  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(_statusUpdate);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector->appoint(master.get());

  AWAIT_READY(slaveReregisteredMessage);

  // The master should not send a TASK_LOST after the slave
  // re-registers. We check this by calling Clock::settle() so that
  // the only update the scheduler receives is the retried
  // TASK_FINISHED update.
  // NOTE: The status update manager resends the status update when
  // it detects a new master.
  Clock::settle();

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FINISHED, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that if a master incorrectly thinks that it is
// leading, the scheduler driver will drop messages from this master.
// Unfortunately, it is not currently possible to start more than one
// master within the same process. So, this test merely simulates this
// by spoofing messages.
// This test does the following:
//   1. Start a master, scheduler, launch a task.
//   2. Spoof a lost task message for the slave.
//   3. Once the message is sent to the scheduler, kill the task.
//   4. Ensure the task was KILLED rather than LOST.
TEST_F(FaultToleranceTest, SplitBrainMasters)
{
  // 1. Start a master, scheduler, and launch a task.
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave> > slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Message> registered =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&runningStatus));

  driver.start();

  AWAIT_READY(registered);
  AWAIT_READY(frameworkId);
  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus.get().state());

  // 2. Spoof a lost task message for the slave.
  StatusUpdateMessage lostUpdate;
  lostUpdate.mutable_update()->CopyFrom(createStatusUpdate(
      frameworkId.get(),
      runningStatus.get().slave_id(),
      runningStatus.get().task_id(),
      TASK_LOST));

  // Spoof a message from a random master; this should be dropped by
  // the scheduler driver. Since this is delivered locally, it is
  // synchronously placed on the scheduler driver's queue.
  process::post(UPID("master2@127.0.0.1:50"), registered.get().to, lostUpdate);

  // 3. Once the message is sent to the scheduler, kill the task.
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  Future<TaskStatus> killedStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&killedStatus));

  driver.killTask(runningStatus.get().task_id());

  // 4. Ensure the task was KILLED rather than LOST.
  AWAIT_READY(killedStatus);
  EXPECT_EQ(TASK_KILLED, killedStatus.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .WillRepeatedly(Return());

  driver.stop();
  driver.join();

  Shutdown();
}
