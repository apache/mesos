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

#include <gtest/gtest.h>

#include <mesos/executor.hpp>
#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/authentication/authentication.hpp>

#include <process/dispatch.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>

#include <stout/nothing.hpp>

#include "master/detector/standalone.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using testing::_;
using testing::Eq;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {


class AuthenticationTest : public MesosTest {};


// This test verifies that an unauthenticated framework is
// denied registration by the master.
TEST_F(AuthenticationTest, UnauthenticatedFramework)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid);

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureSatisfy(&error));

  driver.start();

  // Scheduler should get error message from the master.
  AWAIT_READY(error);

  driver.stop();
  driver.join();
}


// This test verifies that an unauthenticated slave is
// denied registration by the master, but not shut down.
TEST_F(AuthenticationTest, UnauthenticatedSlave)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Previously, agents were shut down when registration failed due to
  // authorization. We verify that this no longer occurs.
  EXPECT_NO_FUTURE_PROTOBUFS(ShutdownMessage(), _, _);

  // We verify that the agent isn't allowed to register.
  EXPECT_NO_FUTURE_PROTOBUFS(SlaveRegisteredMessage(), _, _);

  Future<RegisterSlaveMessage> registerSlaveMessage1 =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  // Start the slave without credentials.
  slave::Flags flags = CreateSlaveFlags();
  flags.credential = None();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger the first registration attempt.
  // The initial registration delay is between [0, registration_backoff_factor].
  Clock::advance(flags.registration_backoff_factor);
  Clock::settle();

  AWAIT_READY(registerSlaveMessage1);

  Future<RegisterSlaveMessage> registerSlaveMessage2 =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  // Advance the clock to trigger another registration attempt.
  Clock::advance(slave::REGISTER_RETRY_INTERVAL_MAX);
  Clock::settle();

  AWAIT_READY(registerSlaveMessage2);
}


// This test verifies that when the master is started with framework
// authentication disabled, it registers unauthenticated frameworks.
TEST_F(AuthenticationTest, DisableFrameworkAuthentication)
{
  Clock::pause();

  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false; // Disable authentication.

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Start the scheduler without credentials.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that when the master is started with slave
// authentication disabled, it registers unauthenticated slaves.
TEST_F(AuthenticationTest, DisableSlaveAuthentication)
{
  Clock::pause();

  master::Flags flags = CreateMasterFlags();
  flags.authenticate_agents = false; // Disable authentication.

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start the slave without credentials.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.credential = None();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger registration attempt.
  // The initial registration delay is between [0, registration_backoff_factor].
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::settle();

  // Slave should be able to get registered.
  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage->slave_id().value());
}


// This test verifies that an authenticated framework is denied
// registration by the master if it uses a different
// FrameworkInfo.principal than Credential.principal.
TEST_F(AuthenticationTest, MismatchedFrameworkInfoPrincipal)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_principal("mismatched-principal");

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureSatisfy(&error));

  driver.start();

  // Scheduler should get error message from the master.
  AWAIT_READY(error);

  driver.stop();
  driver.join();
}


// This test verifies that an authenticated framework is denied
// registration by the master if it uses a different
// FrameworkInfo::principal than Credential.principal, even
// when authentication is not required.
TEST_F(AuthenticationTest, DisabledFrameworkAuthenticationPrincipalMismatch)
{
  Clock::pause();

  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false; // Authentication not required.

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_principal("mismatched-principal");

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureSatisfy(&error));

  driver.start();

  // Scheduler should get error message from the master.
  AWAIT_READY(error);

  driver.stop();
  driver.join();
}


// This test verifies that if a Framework successfully authenticates
// but does not set FrameworkInfo::principal, it is allowed to
// register.
TEST_F(AuthenticationTest, UnspecifiedFrameworkInfoPrincipal)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.clear_principal();

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that when the master is started with
// authentication disabled, it registers authenticated frameworks.
TEST_F(AuthenticationTest, AuthenticatedFramework)
{
  Clock::pause();

  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false; // Disable authentication.

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Start the scheduler with credentials.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that when the master is started with slave
// authentication disabled, it registers authenticated slaves.
TEST_F(AuthenticationTest, AuthenticatedSlave)
{
  Clock::pause();

  master::Flags flags = CreateMasterFlags();
  flags.authenticate_agents = false; // Disable authentication.

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start the slave with credentials.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Advance the clock to trigger authentication and registration attempt.
  // The initial authentication/registration delay is between
  // [0, registration_backoff_factor].
  Clock::advance(slave::DEFAULT_REGISTRATION_BACKOFF_FACTOR);
  Clock::settle();

  // Slave should be able to get registered.
  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage->slave_id().value());
}


// This test verifies that the framework properly retries
// authentication when authenticate message is lost.
TEST_F(AuthenticationTest, RetryFrameworkAuthentication)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  // Drop the first authenticate message from the scheduler.
  Future<AuthenticateMessage> authenticateMessage =
    DROP_PROTOBUF(AuthenticateMessage(), _, _);

  driver.start();

  AWAIT_READY(authenticateMessage);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Advance the clock for the scheduler to retry.
  Clock::advance(
      mesos::internal::scheduler::DEFAULT_AUTHENTICATION_TIMEOUT_MAX);
  Clock::settle();

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that the slave properly retries
// authentication when authenticate message is lost.
TEST_F(AuthenticationTest, RetrySlaveAuthentication)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Drop the first authenticate message from the slave.
  Future<AuthenticateMessage> authenticateMessage =
    DROP_PROTOBUF(AuthenticateMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Advance the clock to trigger authentication attempt.
  // Currently the initial backoff is [0, registration_backoff_factor]
  // for agents both with or without authentication. See MESOS-9173.
  Clock::advance(slave::DEFAULT_REGISTRATION_BACKOFF_FACTOR);
  Clock::settle();

  AWAIT_READY(authenticateMessage);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Advance the clock for the slave to retry.
  Clock::advance(slave::DEFAULT_AUTHENTICATION_TIMEOUT_MAX);
  Clock::settle();

  // Slave should be able to get registered.
  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage->slave_id().value());
}

// Verify that the agent backs off properly when retrying authentication
// according to the configured parameters.
TEST_F(AuthenticationTest, SlaveAuthenticationRetryBackoff)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.authentication_timeout_min = Seconds(5);
  slaveFlags.authentication_timeout_max = Minutes(1);
  slaveFlags.authentication_backoff_factor = Seconds(1);

  // Expected retry timeout range:
  //
  //   [min, min + factor * 2^1]
  //   [min, min + factor * 2^2]
  //   ...
  //   [min, min + factor * 2^N]
  //   ...
  //   [min, max] // Stop at max.
  Duration expected[7][2] = {
    {Seconds(5), Seconds(7)},
    {Seconds(5), Seconds(9)},
    {Seconds(5), Seconds(13)},
    {Seconds(5), Seconds(21)},
    {Seconds(5), Seconds(37)},
    {Seconds(5), Minutes(1)},
    {Seconds(5), Minutes(1)}
  };

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), slaveFlags, true);
  ASSERT_SOME(slave);

  // Drop the first authentication attempt.
  Future<Nothing> authenticate;
  Duration minTimeout, maxTimeout;
  EXPECT_CALL(*slave.get()->mock(), authenticate(_, _))
    .WillOnce(DoAll(
        SaveArg<0>(&minTimeout),
        SaveArg<1>(&maxTimeout),
        FutureSatisfy(&authenticate)))
    .RetiresOnSaturation();

  slave.get()->start();

  // Trigger the first authentication request.
  Clock::advance(slaveFlags.authentication_backoff_factor);
  AWAIT_READY(authenticate);

  for (int i = 0; i < 7; i++) {
    EXPECT_EQ(minTimeout, expected[i][0]);
    EXPECT_EQ(maxTimeout, expected[i][1]);

    // Drop the authenticate message from the slave to incur retry.
    Future<AuthenticateMessage> authenticateMessage =
      DROP_PROTOBUF(AuthenticateMessage(), _, _);

    // Drop the retry call and manually issue the retry call (instead
    // of invoking the unmocked call directly in the expectation. We do this
    // so that, even though clock is advanced for `authentication_timeout_max`
    // in each iteration, we can still guarantee:
    //
    // (1) Retry only happens once in each iteration;
    //
    // (2) At the end of each iteration, the authentication timeout timer is
    // not started. The timer will start only when we manually issue the
    // retry call in the next iteration.
    EXPECT_CALL(*slave.get()->mock(), authenticate(_, _))
      .WillOnce(DoAll(
          SaveArg<0>(&minTimeout),
          SaveArg<1>(&maxTimeout),
          FutureSatisfy(&authenticate)))
      .RetiresOnSaturation();

    process::dispatch(slave.get()->pid, [=] {
      slave.get()->mock()->unmocked_authenticate(minTimeout, maxTimeout);
    });

    // Slave should not retry until `slaveFlags.authentication_timeout_min`.
    Clock::advance(slaveFlags.authentication_timeout_min - Milliseconds(1));
    Clock::settle();
    EXPECT_TRUE(authenticate.isPending());

    // Slave will retry at least once in
    // `slaveFlags.authentication_timeout_max`.
    Clock::advance(
        slaveFlags.authentication_timeout_max -
        slaveFlags.authentication_timeout_min + Milliseconds(1));
    Clock::settle();

    AWAIT_READY(authenticateMessage);
    AWAIT_READY(authenticate);
  }

  Future<AuthenticationCompletedMessage> authenticationCompletedMessage =
    FUTURE_PROTOBUF(AuthenticationCompletedMessage(), _, _);

  process::dispatch(slave.get()->pid, [=] {
    slave.get()->mock()->unmocked_authenticate(minTimeout, maxTimeout);
  });

  Clock::advance(slaveFlags.authentication_timeout_max);

  // Slave should be able to get authenticated.
  AWAIT_READY(authenticationCompletedMessage);
}


// This test ensures that when the master sees a new authentication
// request for a particular agent or scheduler (we just test the
// scheduler case here since the master does not distinguish),
// the master will discard the old one and proceed with the new one.
//
// TODO(bmahler): Use a mock authenticator for this test instead
// of using the default one and dropping the exited message.
TEST_F(AuthenticationTest, MasterRetriedAuthenticationHandling)
{
  Clock::pause();

  // Set the master authentication timeout to a very large value
  // so that we can exercise the case of a new authentication
  // request arriving before the previous one times out.
  master::Flags flags = CreateMasterFlags();
  flags.authentication_v0_timeout = Days(30);

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  // Drop the first step message from the authenticator.
  Future<Message> authenticationStepMessage =
    DROP_MESSAGE(Eq(AuthenticationStepMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(authenticationStepMessage);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Now the master will have a pending authentication and we have
  // the scheduler driver retry. Since the master's authentication
  // timeout is larger than the scheduler's, the scheduler will
  // retry and the master should discard the stale one and let
  // the new one proceed. First, we drop the exited event that
  // the master's authenticator will listen for to ensure the
  // authentication remains outstanding from the master's
  // perspective.
  const UPID& authenticatee = authenticationStepMessage->to;
  const UPID& authenticator = authenticationStepMessage->from;

  Future<Nothing> exited = DROP_EXITED(authenticatee, authenticator);

  Clock::advance(
      mesos::internal::scheduler::DEFAULT_AUTHENTICATION_TIMEOUT_MIN);
  Clock::settle();
  Clock::advance(
      mesos::internal::scheduler::DEFAULT_AUTHENTICATION_BACKOFF_FACTOR * 2);

  // Make sure the exited was dropped.
  AWAIT_READY(exited);

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that the framework properly retries
// authentication when an intermediate message in SASL protocol
// is lost.
TEST_F(AuthenticationTest, DropIntermediateSASLMessage)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  Clock::advance(
      mesos::internal::scheduler::DEFAULT_AUTHENTICATION_TIMEOUT_MAX);
  Clock::settle();

  // Ensure another authentication attempt was made.
  AWAIT_READY(authenticationCompletedMessage);

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that the slave properly retries
// authentication when an intermediate message in SASL protocol
// is lost.
TEST_F(AuthenticationTest, DropIntermediateSASLMessageForSlave)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Drop the AuthenticationStepMessage from authenticator.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Advance the clock to trigger authentication attempt.
  // Currently the initial backoff is [0, registration_backoff_factor]
  // for agents both with or without authentication. See MESOS-9173.
  Clock::advance(slave::DEFAULT_REGISTRATION_BACKOFF_FACTOR);
  Clock::settle();

  AWAIT_READY(authenticationStepMessage);

  Future<AuthenticationCompletedMessage> authenticationCompletedMessage =
    FUTURE_PROTOBUF(AuthenticationCompletedMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Advance the clock for the slave to retry.
  Clock::advance(slave::DEFAULT_AUTHENTICATION_TIMEOUT_MAX);
  Clock::settle();

  // Ensure another authentication attempt was made.
  AWAIT_READY(authenticationCompletedMessage);

  // Slave should be able to get registered.
  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage->slave_id().value());
}


// This test verifies that the framework properly retries
// authentication when the final message in SASL protocol
// is lost. The dropped message causes the master to think
// the framework is authenticated but the framework to think
// otherwise. The framework should retry authentication and
// eventually register.
TEST_F(AuthenticationTest, DropFinalSASLMessage)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  Clock::advance(
      mesos::internal::scheduler::DEFAULT_AUTHENTICATION_TIMEOUT_MAX);
  Clock::settle();

  // Ensure another authentication attempt was made.
  AWAIT_READY(authenticationCompletedMessage);

  // Scheduler should be able to get registered.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that the slave properly retries
// authentication when the final message in SASL protocol
// is lost. The dropped message causes the master to think
// the slave is authenticated but the slave to think
// otherwise. The slave should retry authentication and
// eventually register.
TEST_F(AuthenticationTest, DropFinalSASLMessageForSlave)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Drop the AuthenticationCompletedMessage from authenticator.
  Future<AuthenticationCompletedMessage> authenticationCompletedMessage =
    DROP_PROTOBUF(AuthenticationCompletedMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Advance the clock to trigger authentication attempt.
  // Currently the initial backoff is [0, registration_backoff_factor]
  // for agents both with or without authentication. See MESOS-9173.
  Clock::advance(slave::DEFAULT_REGISTRATION_BACKOFF_FACTOR);
  Clock::settle();

  AWAIT_READY(authenticationCompletedMessage);

  authenticationCompletedMessage =
    FUTURE_PROTOBUF(AuthenticationCompletedMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Advance the clock for the scheduler to retry.
  Clock::advance(slave::DEFAULT_AUTHENTICATION_TIMEOUT_MAX);
  Clock::settle();

  // Ensure another authentication attempt was made.
  AWAIT_READY(authenticationCompletedMessage);

  // Slave should be able to get registered.
  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage->slave_id().value());
}


// This test verifies that when a master fails over while a framework
// authentication attempt is in progress the framework properly
// authenticates.
TEST_F(AuthenticationTest, MasterFailover)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  Owned<StandaloneMasterDetector> detector(
      new StandaloneMasterDetector(master.get()->pid));
  TestingMesosSchedulerDriver driver(&sched, detector.get());

  // Drop the authenticate message from the scheduler.
  Future<AuthenticateMessage> authenticateMessage =
    DROP_PROTOBUF(AuthenticateMessage(), _, _);

  driver.start();

  AWAIT_READY(authenticateMessage);

  // While the authentication is in progress simulate a failed over
  // master by restarting the master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Appoint a new master and inform the scheduler about it.
  detector->appoint(master.get()->pid);

  // Scheduler should successfully register with the new master.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that when a master fails over while a slave
// authentication attempt is in progress the slave properly
// authenticates.
TEST_F(AuthenticationTest, MasterFailoverDuringSlaveAuthentication)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Drop the authenticate message from the slave.
  Future<AuthenticateMessage> authenticateMessage =
    DROP_PROTOBUF(AuthenticateMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger authentication attempt.
  // Currently the initial backoff is [0, registration_backoff_factor]
  // for agents both with or without authentication. See MESOS-9173.
  Clock::advance(slave::DEFAULT_REGISTRATION_BACKOFF_FACTOR);
  Clock::settle();

  AWAIT_READY(authenticateMessage);

  // While the authentication is in progress simulate a failed over
  // master by restarting the master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Appoint a new master and inform the slave about it.
  detector.appoint(master.get()->pid);

  // Advance the clock to trigger authentication and registration attempt.
  // The initial registration delay is between [0, registration_backoff_factor].
  Clock::advance(slave::DEFAULT_REGISTRATION_BACKOFF_FACTOR);
  Clock::settle();

  // Slave should be able to get registered.
  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage->slave_id().value());
}


// This test verifies that if the scheduler retries authentication
// before the original authentication finishes (e.g., new master
// detected due to leader election), it is handled properly.
TEST_F(AuthenticationTest, LeaderElection)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  Owned<StandaloneMasterDetector> detector(
      new StandaloneMasterDetector(master.get()->pid));
  TestingMesosSchedulerDriver driver(&sched, detector.get());

  // Drop the AuthenticationStepMessage from authenticator.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  driver.start();

  // Drop the intermediate SASL message so that authentication fails.
  AWAIT_READY(authenticationStepMessage);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Appoint a new master and inform the scheduler about it.
  detector->appoint(master.get()->pid);

  // Scheduler should successfully register with the new master.
  AWAIT_READY(registered);

  driver.stop();
  driver.join();
}


// This test verifies that if the slave retries authentication
// before the original authentication finishes (e.g., new master
// detected due to leader election), it is handled properly.
TEST_F(AuthenticationTest, LeaderElectionDuringSlaveAuthentication)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Drop the AuthenticationStepMessage from authenticator.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger authentication attempt.
  // Currently the initial backoff is [0, registration_backoff_factor]
  // for agents both with or without authentication. See MESOS-9173.
  Clock::advance(slave::DEFAULT_REGISTRATION_BACKOFF_FACTOR);
  Clock::settle();

  // Drop the intermediate SASL message so that authentication fails.
  AWAIT_READY(authenticationStepMessage);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Appoint a new master and inform the slave about it.
  detector.appoint(master.get()->pid);

  // Advance the clock to trigger authentication and registration attempt.
  // The initial registration delay is between [0, registration_backoff_factor].
  Clock::advance(slave::DEFAULT_REGISTRATION_BACKOFF_FACTOR);
  Clock::settle();

  // Slave should be able to get registered.
  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage->slave_id().value());
}


// This test verifies that if a scheduler fails over in the midst of
// authentication it successfully re-authenticates and reregisters
// with the master when it comes back up.
TEST_F(AuthenticationTest, SchedulerFailover)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  Owned<StandaloneMasterDetector> detector(
      new StandaloneMasterDetector(master.get()->pid));
  TestingMesosSchedulerDriver driver1(&sched1, detector.get());

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver1.start();

  AWAIT_READY(frameworkId);

  // Drop the AuthenticationStepMessage from authenticator
  // to stop authentication from succeeding.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  EXPECT_CALL(sched1, disconnected(&driver1));

  // Appoint a new master and inform the scheduler about it.
  detector->appoint(master.get()->pid);

  AWAIT_READY(authenticationStepMessage);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback.

  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  driver2.start();

  AWAIT_READY(sched2Registered);
  AWAIT_READY(sched1Error);

  driver2.stop();
  driver2.join();

  driver1.stop();
  driver1.join();
}


// This test verifies that a scheduler's re-registration will be
// rejected if it specifies a principal different from what's used in
// authentication.
TEST_F(AuthenticationTest, RejectedSchedulerFailover)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Launch the first scheduler.
  MockScheduler sched1;
  Owned<StandaloneMasterDetector> detector(
      new StandaloneMasterDetector(master.get()->pid));
  TestingMesosSchedulerDriver driver1(&sched1, detector.get());

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  driver1.start();

  AWAIT_READY(frameworkId);

  // Drop the AuthenticationStepMessage from authenticator
  // to stop authentication from succeeding.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  EXPECT_CALL(sched1, disconnected(&driver1));

  // Appoint a new master and inform the scheduler about it.
  detector->appoint(master.get()->pid);

  AWAIT_READY(authenticationStepMessage);

  // Attempt to failover to scheduler 2 while scheduler 1 is still
  // up. We use the framework id recorded from scheduler 1 but change
  // the principal in FrameworInfo and it will be denied. Scheduler 1
  // will not be asked to shutdown.
  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());
  framework2.set_principal("mismatched-principal");

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, _))
    .Times(0);

  Future<Nothing> sched2Error;
  EXPECT_CALL(sched2, error(&driver2, _))
    .WillOnce(FutureSatisfy(&sched2Error));

  driver2.start();

  // Scheduler 2 should get error message from the master.
  AWAIT_READY(sched2Error);

  driver2.stop();
  driver2.join();

  driver1.stop();
  driver1.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
