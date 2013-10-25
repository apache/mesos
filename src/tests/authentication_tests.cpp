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

#include <gtest/gtest.h>

#include <mesos/executor.hpp>
#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <process/dispatch.hpp>
#include <process/gmock.hpp>

#include <stout/nothing.hpp>

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;
using namespace mesos::internal::tests;

using namespace process;

using mesos::internal::master::Master;

using testing::_;
using testing::Eq;
using testing::Return;


class AuthenticationTest : public MesosTest {};


// This test verifies that an unauthenticated framework is
// denied registration by the master.
TEST_F(AuthenticationTest, UnauthenticatedFramework)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureSatisfy(&error));

  driver.start();

  // Scheduler should get error message from the master.
  AWAIT_READY(error);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that when the master is started with
// authentication disabled, it registers unauthenticated frameworks.
TEST_F(AuthenticationTest, DisableAuthentication)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate = false; // Disable authentcation.

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  // Start the scheduler without credentials.
  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that when the master is started with
// authentication disabled, it registers authenticated frameworks.
TEST_F(AuthenticationTest, AuthenticatedFramework)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate = false; // Disable authentcation.

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  // Start the scheduler with credentials.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the framework properly retries
// authentication when authenticate message is lost.
TEST_F(AuthenticationTest, RetryAuthentication)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  // Drop the first authenticate message from the scheduler.
  Future<AuthenticateMessage> authenticateMessage =
    DROP_PROTOBUF(AuthenticateMessage(), _, _);

  driver.start();

  AWAIT_READY(authenticateMessage);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Advance the clock for the scheduler to retry.
  Clock::pause();
  Clock::advance(Seconds(5));
  Clock::settle();
  Clock::resume();

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the framework properly retries
// authentication when an intermediate message in SASL protocol
// is lost.
TEST_F(AuthenticationTest, DropIntermediateSASLMessage)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  // Drop the AuthenticationStepMessage from authenticator.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  driver.start();

  AWAIT_READY(authenticationStepMessage);

  Future<AuthenticationCompletedMessage> authenticationCompletedMessage =
    FUTURE_PROTOBUF(AuthenticationCompletedMessage(), _, _);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Advance the clock for the scheduler to retry.
  Clock::pause();
  Clock::advance(Seconds(5));
  Clock::settle();
  Clock::resume();

  // Ensure another authentication attempt was made.
  AWAIT_READY(authenticationCompletedMessage);

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the framework properly retries
// authentication when the final message in SASL protocol
// is lost. The dropped message causes the master to think
// the framework is authenticated but the framework to think
// otherwise. The framework should retry authentication and
// eventually register.
TEST_F(AuthenticationTest, DropFinalSASLMessage)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  // Drop the AuthenticationCompletedMessage from authenticator.
  Future<AuthenticationCompletedMessage> authenticationCompletedMessage =
    DROP_PROTOBUF(AuthenticationCompletedMessage(), _, _);

  driver.start();

  AWAIT_READY(authenticationCompletedMessage);

  authenticationCompletedMessage =
    FUTURE_PROTOBUF(AuthenticationCompletedMessage(), _, _);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Advance the clock for the scheduler to retry.
  Clock::pause();
  Clock::advance(Seconds(5));
  Clock::settle();
  Clock::resume();

  // Ensure another authentication attempt was made.
  AWAIT_READY(authenticationCompletedMessage);

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that when a master fails over while an
// authentication attempt is in progress the framework properly
// authenticates.
TEST_F(AuthenticationTest, MasterFailover)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  // Drop the authenticate message from the scheduler.
  Future<AuthenticateMessage> authenticateMessage =
    DROP_PROTOBUF(AuthenticateMessage(), _, _);

  driver.start();

  AWAIT_READY(authenticateMessage);
  UPID frameworkPid = authenticateMessage.get().pid();

  // While the authentication is in progress simulate a failed over
  // master by restarting the master.
  Stop(master.get());
  master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Send a new master detected message to inform the scheduler
  // about the new master.
  NewMasterDetectedMessage newMasterDetectedMsg;
  newMasterDetectedMsg.set_pid(master.get());

  process::post(frameworkPid, newMasterDetectedMsg);

  // Scheduler should successfully register with the new master.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that if the scheduler retries authentication
// before the original authentication finishes (e.g., new master
// detected due to leader election), it is handled properly.
TEST_F(AuthenticationTest, LeaderElection)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<AuthenticateMessage> authenticateMessage =
    FUTURE_PROTOBUF(AuthenticateMessage(), _, _);

  // Drop the AuthenticationStepMessage from authenticator.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  driver.start();

  // Grab the framework pid.
  AWAIT_READY(authenticateMessage);
  UPID frameworkPid = authenticateMessage.get().pid();

  // Drop the intermediate SASL message so that authentication fails.
  AWAIT_READY(authenticationStepMessage);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Send a new master detected message to inform the scheduler
  // about the new master after a leader election.
  NewMasterDetectedMessage newMasterDetectedMsg;
  newMasterDetectedMsg.set_pid(master.get());

  process::post(frameworkPid, newMasterDetectedMsg);

  // Scheduler should successfully register with the new master.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that if a scheduler fails over in the midst of
// authentication it successfully re-authenticates and re-registers
// with the master when it comes back up.
TEST_F(AuthenticationTest, SchedulerFailover)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<AuthenticateMessage> authenticateMessage =
    FUTURE_PROTOBUF(AuthenticateMessage(), _, _);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver1.start();

  // Grab the framework pid.
  AWAIT_READY(authenticateMessage);
  UPID frameworkPid = authenticateMessage.get().pid();

  AWAIT_READY(frameworkId);

  // Drop the AuthenticationStepMessage from authenticator
  // to stop authentication from succeeding.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  EXPECT_CALL(sched1, disconnected(&driver1));

  // Send a NewMasterDetected message to elicit authentication.
  NewMasterDetectedMessage newMasterDetectedMsg;
  newMasterDetectedMsg.set_pid(master.get());

  process::post(frameworkPid, newMasterDetectedMsg);

  AWAIT_READY(authenticationStepMessage);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback.

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  driver2.start();

  AWAIT_READY(sched2Registered);

  driver2.stop();
  driver2.join();

  driver1.stop();
  driver1.join();

  Shutdown();
}
