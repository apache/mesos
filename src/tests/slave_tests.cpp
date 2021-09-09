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

#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/authentication/http/basic_authenticator_factory.hpp>

#include <mesos/v1/mesos.hpp>

#include <mesos/v1/resource_provider/resource_provider.hpp>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#ifdef USE_SSL_SOCKET
#include "authentication/executor/jwt_secret_generator.hpp"
#endif // USE_SSL_SOCKET

#include "common/build.hpp"
#include "common/future_tracker.hpp"
#include "common/http.hpp"
#include "common/protobuf_utils.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/registry_operations.hpp"

#include "master/detector/standalone.hpp"

#include "slave/constants.hpp"
#include "slave/gc.hpp"
#include "slave/gc_process.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"
#include "slave/paths.hpp"

#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/fetcher_process.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/isolator_tracker.hpp"
#include "slave/containerizer/mesos/launcher.hpp"

#include "tests/active_user_test_helper.hpp"
#include "tests/containerizer.hpp"
#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/limiter.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"
#include "tests/resources_utils.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/isolator.hpp"
#include "tests/containerizer/mock_containerizer.hpp"

using namespace mesos::internal::slave;

#ifdef USE_SSL_SOCKET
using mesos::authentication::executor::JWTSecretGenerator;
#endif // USE_SSL_SOCKET

using mesos::internal::master::Master;

using mesos::internal::protobuf::createLabel;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::v1::resource_provider::Event;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerTermination;
using mesos::slave::Isolator;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Mesos;

using process::Clock;
using process::Failure;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::Promise;
using process::UPID;

using process::filter;

using process::http::InternalServerError;
using process::http::OK;
using process::http::Response;
using process::http::ServiceUnavailable;
using process::http::Unauthorized;

using process::http::authentication::Principal;

using std::list;
using std::map;
using std::shared_ptr;
using std::string;
using std::vector;

using testing::_;
using testing::AtLeast;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Exactly;
using testing::StrEq;
using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::Return;
using testing::SaveArg;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

// Those of the overall Mesos master/slave/scheduler/driver tests
// that seem vaguely more slave than master-related are in this file.
// The others are in "master_tests.cpp".

class SlaveTest : public ContainerizerTest<slave::MesosContainerizer>
{
public:
  const std::string defaultIsolators{
#ifdef __WINDOWS__
      "windows/cpu"
#else
      "posix/cpu,posix/mem"
#endif // __WINDOWS__
      };

  CommandInfo echoAuthorCommand()
  {
    CommandInfo command;
    command.set_shell(false);
#ifdef __WINDOWS__
    command.set_value("powershell.exe");
    command.add_arguments("powershell.exe");
    command.add_arguments("-NoProfile");
    command.add_arguments("-Command");
    command.add_arguments("echo --author");
#else
    command.set_value("/bin/echo");
    command.add_arguments("/bin/echo");
    command.add_arguments("--author");
#endif // __WINDOWS__
    return command;
  };
};


// This test ensures that when a slave shuts itself down, it
// unregisters itself and the master notifies the framework
// immediately and rescinds any offers.
TEST_F(SlaveTest, Shutdown)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
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
  EXPECT_EQ(1u, offers->size());

  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, offers.get()[0].id()))
    .WillOnce(FutureSatisfy(&offerRescinded));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, offers.get()[0].slave_id()))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Stop the slave with explicit shutdown message so that the slave
  // unregisters.
  slave.get()->shutdown();
  slave->reset();

  AWAIT_READY(offerRescinded);
  AWAIT_READY(slaveLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unregistered"]);
  EXPECT_EQ(0, stats.values["master/slave_removals/reason_unhealthy"]);

  driver.stop();
  driver.join();
}


// This test verifies that the slave rejects duplicate terminal
// status updates for tasks before the first terminal update is
// acknowledged.
TEST_F(SlaveTest, DuplicateTerminalUpdateBeforeAck)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags agentFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, agentFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  // Send a terminal update right away.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  // Drop the first ACK from the scheduler to the slave.
  Future<StatusUpdateAcknowledgementMessage> statusUpdateAckMessage =
    DROP_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, slave.get()->pid);

  Future<Nothing> ___statusUpdate =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::___statusUpdate);

  TaskInfo task;
  task.set_name("test-task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers->at(0).slave_id());
  task.mutable_resources()->MergeFrom(offers->at(0).resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(status);

  EXPECT_EQ(TASK_FINISHED, status->state());

  AWAIT_READY(statusUpdateAckMessage);

  // At this point the task status update manager has enqueued
  // TASK_FINISHED update.
  AWAIT_READY(___statusUpdate);

  Future<Nothing> _statusUpdate2 =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdate);

  // Now send a TASK_KILLED update for the same task.
  TaskStatus status2 = status.get();
  status2.set_state(TASK_KILLED);
  execDriver->sendStatusUpdate(status2);

  // At this point the slave has handled the TASK_KILLED update.
  AWAIT_READY(_statusUpdate2);

  // After we advance the clock, the scheduler should receive
  // the retried TASK_FINISHED update and acknowledge it.
  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&update));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // Ensure the scheduler receives TASK_FINISHED.
  AWAIT_READY(update);
  EXPECT_EQ(TASK_FINISHED, update->state());

  // Settle the clock to ensure that TASK_KILLED is not sent.
  Clock::settle();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(SlaveTest, ShutdownUnregisteredExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  // Set the isolation flag so we know a MesosContainerizer will
  // be created.
  flags.isolation = defaultIsolators;

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get());
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

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  CommandInfo command;
  command.set_value(SLEEP_COMMAND(10));

  task.mutable_command()->MergeFrom(command);

  // Drop the registration message from the executor to the slave.
  Future<Message> registerExecutor =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(registerExecutor);

  Clock::pause();

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Ensure that the slave times out and kills the executor.
  Future<Nothing> destroyExecutor =
    FUTURE_DISPATCH(_, &MesosContainerizerProcess::destroy);

  Clock::advance(flags.executor_registration_timeout);

  AWAIT_READY(destroyExecutor);

  Clock::settle(); // Wait for Containerizer::destroy to complete.
  Clock::resume();

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_REGISTRATION_TIMEOUT,
            status->reason());

  driver.stop();
  driver.join();
}


#ifndef __WINDOWS__
// This test verifies that mesos agent gets notified of task
// launch failure triggered by the executor register timeout
// caused by slow URI fetching.
TEST_F(SlaveTest, ExecutorTimeoutCausedBySlowFetch)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  string hadoopPath = os::getcwd();
  string hadoopBinPath = path::join(hadoopPath, "bin");

  ASSERT_SOME(os::mkdir(hadoopBinPath));
  ASSERT_SOME(os::chmod(hadoopBinPath, S_IRWXU | S_IRWXG | S_IRWXO));

  // A spurious "hadoop" script that sleeps forever.
  string mockHadoopScript = "#!/usr/bin/env bash\n"
                            "sleep 1000";

  string hadoopCommand = path::join(hadoopBinPath, "hadoop");
  ASSERT_SOME(os::write(hadoopCommand, mockHadoopScript));
  ASSERT_SOME(os::chmod(hadoopCommand,
                        S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH));

  slave::Flags flags = CreateSlaveFlags();
  flags.hadoop_home = hadoopPath;

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer = MesosContainerizer::create(
      flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Launch a task with the command executor.
  // The task uses a URI that needs to be fetched by the HDFS client
  // and will be blocked until the executor registrartion times out.
  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path::join("hdfs://dummyhost/dummypath", "test"));

  // Using a dummy command value as it's a required field. The
  // command won't be invoked.
  commandInfo.set_value(SLEEP_COMMAND(10));

  ExecutorID executorId;
  executorId.set_value("test-executor-staging");

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      offers.get()[0].resources(),
      commandInfo,
      executorId,
      "test-task-staging");

  Future<Nothing> fetch = FUTURE_DISPATCH(
      _, &FetcherProcess::fetch);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Clock::pause();

  driver.launchTasks(offers.get()[0].id(), {task});

  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, executorId, _, _))
    .WillOnce(FutureSatisfy(&executorLost));

  // Ensure that the slave times out and kills the executor.
  Future<Nothing> destroyExecutor = FUTURE_DISPATCH(
      _, &MesosContainerizerProcess::destroy);

  AWAIT_READY(fetch);

  Clock::advance(flags.executor_registration_timeout);

  AWAIT_READY(destroyExecutor);

  Clock::settle(); // Wait for Containerizer::destroy to complete.
  Clock::resume();

  AWAIT_READY(executorLost);

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_LAUNCH_FAILED, status->reason());

  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


// This test verifies that when an executor terminates before
// registering with slave, it is properly cleaned up.
TEST_F(SlaveTest, RemoveUnregisteredTerminatedExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
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

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // Drop the registration message from the executor to the slave.
  Future<Message> registerExecutorMessage =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(registerExecutorMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> schedule =
    FUTURE_DISPATCH(_, &GarbageCollectorProcess::schedule);

  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _));
  // Now kill the executor.
  containerizer.destroy(offers.get()[0].framework_id(), DEFAULT_EXECUTOR_ID);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_FAILED, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_TERMINATED, status->reason());

  // We use 'gc.schedule' as a signal for the executor being cleaned
  // up by the slave.
  AWAIT_READY(schedule);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Test that we don't let task arguments bleed over as
// mesos-executor args. For more details of this see MESOS-1873.
//
// This assumes the ability to execute '/bin/echo --author'.
TEST_F(SlaveTest, CommandTaskWithArguments)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = defaultIsolators;

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get());
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

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  // Command executor will run as user running test.
  task.mutable_command()->MergeFrom(echoAuthorCommand());

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Scheduler should first receive TASK_STARTING, followed by
  // TASK_RUNNING and TASK_FINISHED from the executor.
  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusStarting->source());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusRunning->source());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusFinished->source());

  driver.stop();
  driver.join();
}


// Tests that task's kill policy grace period does not extend the time
// a task responsive to SIGTERM needs to exit and the terminal status
// to be delivered to the master.
TEST_F(SlaveTest, CommandTaskWithKillPolicy)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());

  CommandInfo command;
  command.set_value(SLEEP_COMMAND(1000));
  task.mutable_command()->MergeFrom(command);

  // Set task's kill policy grace period to a large value.
  Duration gracePeriod = Seconds(100);
  task.mutable_kill_policy()->mutable_grace_period()->set_nanoseconds(
      gracePeriod.ns());

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Kill the task.
  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.killTask(statusRunning->task_id());

  // Since "sleep 1000" task is responsive to SIGTERM, we should
  // observe TASK_KILLED update sooner than after `gracePeriod`
  // elapses. This indicates that extended grace period does not
  // influence the time a task and its command executor need to
  // exit. We add a small buffer for a task to clean up and the
  // update to be processed by the master.
  AWAIT_READY_FOR(statusKilled, Seconds(1) + process::MAX_REAP_INTERVAL());

  EXPECT_EQ(TASK_KILLED, statusKilled->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR,
            statusKilled->source());

  driver.stop();
  driver.join();
}


// Don't let args from the CommandInfo struct bleed over into
// mesos-executor forking. For more details of this see MESOS-1873.
TEST_F(SlaveTest, GetExecutorInfo)
{
  TestContainerizer containerizer;
  StandaloneMasterDetector detector;

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  FrameworkID frameworkId;
  frameworkId.set_value("20141010-221431-251662764-60288-32120-0000");

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.mutable_id()->CopyFrom(frameworkId);

  // Launch a task with the command executor.
  Resources taskResources = Resources::parse("cpus:0.1;mem:32").get();
  taskResources.allocate(frameworkInfo.roles(0));

  TaskInfo task;
  task.set_name("task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value(
      "20141010-221431-251662764-60288-32120-0001");
  task.mutable_resources()->MergeFrom(taskResources);
  task.mutable_command()->MergeFrom(echoAuthorCommand());

  DiscoveryInfo* info = task.mutable_discovery();
  info->set_visibility(DiscoveryInfo::EXTERNAL);
  info->set_name("mytask");
  info->set_environment("mytest");
  info->set_location("mylocation");
  info->set_version("v0.1.1");

  Labels* labels = task.mutable_labels();
  labels->add_labels()->CopyFrom(createLabel("label1", "key1"));
  labels->add_labels()->CopyFrom(createLabel("label2", "key2"));

  const ExecutorInfo& executor =
    slave.get()->mock()->getExecutorInfo(frameworkInfo, task);

  // Now assert that it actually is running mesos-executor without any
  // bleedover from the command we intend on running.
  EXPECT_FALSE(executor.command().shell());
  EXPECT_EQ(2, executor.command().arguments_size());
  ASSERT_TRUE(executor.has_labels());
  EXPECT_EQ(2, executor.labels().labels_size());
  ASSERT_TRUE(executor.has_discovery());
  ASSERT_TRUE(executor.discovery().has_name());
  EXPECT_EQ("mytask", executor.discovery().name());
  EXPECT_NE(string::npos, executor.command().value().find("mesos-executor"));
}


// Ensure getExecutorInfo for mesos-executor gets the ContainerInfo,
// if present. This ensures the MesosContainerizer can get the
// NetworkInfo even when using the command executor.
TEST_F(SlaveTest, GetExecutorInfoForTaskWithContainer)
{
  TestContainerizer containerizer;
  StandaloneMasterDetector detector;

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.mutable_id()->set_value(
      "20141010-221431-251662764-60288-12345-0000");

  // Launch a task with the command executor and ContainerInfo with
  // NetworkInfo.
  Resources taskResources = Resources::parse("cpus:0.1;mem:32").get();
  taskResources.allocate(frameworkInfo.roles(0));

  TaskInfo task;
  task.set_name("task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value(
      "20141010-221431-251662764-60288-12345-0001");
  task.mutable_resources()->MergeFrom(taskResources);
  task.mutable_command()->MergeFrom(echoAuthorCommand());

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  NetworkInfo* network = container->add_network_infos();
  network->add_ip_addresses()->set_ip_address("4.3.2.1");
  network->add_groups("public");

  const ExecutorInfo& executor =
    slave.get()->mock()->getExecutorInfo(frameworkInfo, task);

  // Now assert that the executor has both the command and ContainerInfo
  EXPECT_FALSE(executor.command().shell());
  // CommandInfo.container is not included. In this test the ContainerInfo
  // must be included in Executor.container (copied from TaskInfo.container).
  EXPECT_TRUE(executor.has_container());

  EXPECT_EQ(1, executor.container().network_infos(0).ip_addresses_size());

  NetworkInfo::IPAddress ipAddress =
    executor.container().network_infos(0).ip_addresses(0);

  EXPECT_EQ("4.3.2.1", ipAddress.ip_address());

  EXPECT_EQ(1, executor.container().network_infos(0).groups_size());
  EXPECT_EQ("public", executor.container().network_infos(0).groups(0));
}


// This test runs a command without the command user field set. The
// command will verify the assumption that the command is run as the
// slave user (in this case, root).
//
// TODO(andschwa): Enable when user impersonation works on Windows.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
  SlaveTest, ROOT_RunTaskWithCommandInfoWithoutUser)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get());
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

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  Result<string> user = os::user();
  ASSERT_SOME(user) << "Failed to get current user name"
                    << (user.isError() ? ": " + user.error() : "");

  const string helper = getTestHelperPath("test-helper");

  // Command executor will run as user running test.
  CommandInfo command;
  command.set_shell(false);
  command.set_value(helper);
  command.add_arguments(helper);
  command.add_arguments(ActiveUserTestHelper::NAME);
  command.add_arguments("--user=" + user.get());

  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Scheduler should first receive TASK_STARTING followed by
  // TASK_RUNNING and TASK_FINISHED from the executor.
  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusStarting->source());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusRunning->source());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusFinished->source());

  driver.stop();
  driver.join();
}


#ifndef __WINDOWS__
// This test runs a command _with_ the command user field set. The
// command will verify the assumption that the command is run as the
// specified user.
TEST_F(SlaveTest, ROOT_UNPRIVILEGED_USER_RunTaskWithCommandInfoWithUser)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  Result<uid_t> uid = os::getuid(user.get());
  ASSERT_SOME(uid);

  TaskInfo task = createTask(
      offers->at(0),
      "test `id -u` -eq " + stringify(uid.get()));

  task.mutable_command()->set_user(user.get());

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that the agent gracefully drops tasks when
// a scheduler launches as a user that is not present on the agent.
//
// TODO(andschwa): Enable after `flags.switch_user` is added.
TEST_F(SlaveTest, ROOT_RunTaskWithCommandInfoWithInvalidUser)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // Enable `switch_user` so the agent is forced to
  // evaluate the provided user name.
  flags.switch_user = true;

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get());

  ASSERT_SOME(slave);

  // Enable partition awareness so that we can expect `TASK_DROPPED`
  // rather than `TASK_LOST`.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const string taskUser = id::UUID::random().toString();

  // Create a command that would trivially succeed if only
  // the user was valid.
  CommandInfo command;
  command.set_user(taskUser);
  command.set_shell(true);
  command.set_value("true");

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command);

  Future<TaskStatus> statusDropped;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusDropped));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusDropped);
  EXPECT_EQ(TASK_DROPPED, statusDropped->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, statusDropped->source());

  // Since we expect the task to fail because the task user didn't
  // exist, it's reasonable to check that the user was mentioned in
  // the status message.
  EXPECT_TRUE(strings::contains(statusDropped->message(), taskUser))
    << statusDropped->message();

  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


// This test ensures that a status update acknowledgement from a
// non-leading master is ignored.
TEST_F(SlaveTest, IgnoreNonLeaderStatusUpdateAcknowledgement)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&schedDriver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // We need to grab this message to get the scheduler's pid.
  Future<Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get()->pid, _);

  schedDriver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  const UPID schedulerPid = frameworkRegisteredMessage->to;

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> update;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&update));

  // Pause the clock to prevent status update retries on the slave.
  Clock::pause();

  // Intercept the acknowledgement sent to the slave so that we can
  // spoof the master's pid.
  Future<StatusUpdateAcknowledgementMessage> acknowledgementMessage =
    DROP_PROTOBUF(StatusUpdateAcknowledgementMessage(),
                  master.get()->pid,
                  slave.get()->pid);

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  schedDriver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(update);
  EXPECT_EQ(TASK_RUNNING, update->state());

  AWAIT_READY(acknowledgementMessage);

  // Send the acknowledgement to the slave with a non-leading master.
  post(process::UPID("master@localhost:1"),
       slave.get()->pid,
       acknowledgementMessage.get());

  // Make sure the acknowledgement was ignored.
  Clock::settle();
  ASSERT_TRUE(_statusUpdateAcknowledgement.isPending());

  // Make sure the status update gets retried because the slave
  // ignored the acknowledgement.
  Future<TaskStatus> retriedUpdate;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&retriedUpdate));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(retriedUpdate);

  // Ensure the slave receives and properly handles the ACK.
  // Clock::settle() ensures that the slave successfully
  // executes Slave::_statusUpdateAcknowledgement().
  AWAIT_READY(_statusUpdateAcknowledgement);
  Clock::settle();

  Clock::resume();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  schedDriver.stop();
  schedDriver.join();
}


TEST_F(SlaveTest, MetricsInMetricsEndpoint)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Make sure slave finishes recovery.
  Future<RegisterSlaveMessage> registerSlave = FUTURE_PROTOBUF(
      RegisterSlaveMessage(), slave.get()->pid, master.get()->pid);

  AWAIT_READY(registerSlave);

  JSON::Object snapshot = Metrics();

  EXPECT_EQ(1u, snapshot.values.count("slave/uptime_secs"));
  EXPECT_EQ(1u, snapshot.values.count("slave/registered"));

  EXPECT_EQ(1u, snapshot.values.count("slave/recovery_errors"));
  EXPECT_EQ(1u, snapshot.values.count("slave/recovery_time_secs"));

  EXPECT_EQ(1u, snapshot.values.count("slave/frameworks_active"));

  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_staging"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_starting"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_running"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_killing"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_finished"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_failed"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_killed"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_lost"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_gone"));

  EXPECT_EQ(1u, snapshot.values.count("slave/executors_registering"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_running"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_terminating"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_terminated"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_preempted"));

  EXPECT_EQ(1u, snapshot.values.count("slave/valid_status_updates"));
  EXPECT_EQ(1u, snapshot.values.count("slave/invalid_status_updates"));

  EXPECT_EQ(1u, snapshot.values.count("slave/valid_framework_messages"));
  EXPECT_EQ(1u, snapshot.values.count("slave/invalid_framework_messages"));

  EXPECT_EQ(1u, snapshot.values.count(
      "slave/executor_directory_max_allowed_age_secs"));

  EXPECT_EQ(1u, snapshot.values.count("slave/container_launch_errors"));

  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_revocable_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_revocable_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_revocable_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/gpus_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/gpus_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/gpus_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/gpus_revocable_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/gpus_revocable_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/gpus_revocable_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/mem_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/mem_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/mem_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/mem_revocable_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/mem_revocable_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/mem_revocable_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/disk_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/disk_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/disk_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/disk_revocable_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/disk_revocable_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/disk_revocable_percent"));
}


// Test to verify that we increment the container launch errors metric
// when we fail to launch a container.
TEST_F(SlaveTest, MetricsSlaveLaunchErrors)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  TestContainerizer containerizer;

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Offer offer = offers.get()[0];

  // Verify that we start with no launch failures.
  JSON::Object snapshot = Metrics();
  EXPECT_EQ(0, snapshot.values["slave/container_launch_errors"]);

  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(Return(Failure("Injected failure")));

  Future<TaskStatus> failureUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&failureUpdate));

  // The above injected containerizer failure also triggers executorLost.
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _));

  // Try to start a task
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:32").get(),
      SLEEP_COMMAND(1000),
      DEFAULT_EXECUTOR_ID);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(failureUpdate);
  ASSERT_EQ(TASK_FAILED, failureUpdate->state());

  // After failure injection, metrics should report a single failure.
  snapshot = Metrics();
  EXPECT_EQ(1, snapshot.values["slave/container_launch_errors"]);

  driver.stop();
  driver.join();
}


TEST_F(SlaveTest, StateEndpoint)
{
  master::Flags masterFlags = this->CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = this->CreateSlaveFlags();

  agentFlags.hostname = "localhost";
  agentFlags.resources = "cpus:4;gpus:0;mem:2048;disk:512;ports:[33000-34000]";
  agentFlags.attributes = "rack:abc;host:myhost";

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Capture the start time deterministically.
  Clock::pause();

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, agentFlags);
  ASSERT_SOME(slave);

  // Ensure slave has finished recovery.
  AWAIT_READY(__recover);
  Clock::settle();

  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(parse);

  JSON::Object state = parse.get();

  EXPECT_EQ(MESOS_VERSION, state.values["version"]);

  if (build::GIT_SHA.isSome()) {
    EXPECT_EQ(build::GIT_SHA.get(), state.values["git_sha"]);
  }

  if (build::GIT_BRANCH.isSome()) {
    EXPECT_EQ(build::GIT_BRANCH.get(), state.values["git_branch"]);
  }

  if (build::GIT_TAG.isSome()) {
    EXPECT_EQ(build::GIT_TAG.get(), state.values["git_tag"]);
  }

  EXPECT_EQ(build::DATE, state.values["build_date"]);
  EXPECT_EQ(build::TIME, state.values["build_time"]);
  EXPECT_EQ(build::USER, state.values["build_user"]);

  // Even with a paused clock, the value of `start_time` from the
  // state endpoint can differ slightly from the actual start time
  // since the value went through a number of conversions (`double` to
  // `string` to `JSON::Value`).  Since `Clock::now` is a floating
  // point value, the actual maximal possible difference between the
  // real and observed value depends on both the mantissa and the
  // exponent of the compared values; for simplicity we compare with
  // an epsilon of `1` which allows for e.g., changes in the integer
  // part of values close to an integer value.
  ASSERT_TRUE(state.values["start_time"].is<JSON::Number>());
  EXPECT_NEAR(
      Clock::now().secs(),
      state.values["start_time"].as<JSON::Number>().as<double>(),
      1);

  // TODO(bmahler): The slave must register for the 'id'
  // to be non-empty.
  ASSERT_TRUE(state.values["id"].is<JSON::String>());

  EXPECT_EQ(stringify(slave.get()->pid), state.values["pid"]);
  EXPECT_EQ(agentFlags.hostname.get(), state.values["hostname"]);

  ASSERT_TRUE(state.values["capabilities"].is<JSON::Array>());
  EXPECT_FALSE(state.values["capabilities"].as<JSON::Array>().values.empty());
  JSON::Value slaveCapabilities = state.values.at("capabilities");

  // Agents should have the following capabilities in the current
  // implementation.
  Try<JSON::Value> expectedCapabilities = JSON::parse(
    R"~(
      [
        "MULTI_ROLE",
        "HIERARCHICAL_ROLE",
        "RESERVATION_REFINEMENT",
        "RESOURCE_PROVIDER",
        "RESIZE_VOLUME",
        "AGENT_OPERATION_FEEDBACK",
        "AGENT_DRAINING",
        "TASK_RESOURCE_LIMITS"
      ]
    )~");

  ASSERT_SOME(expectedCapabilities);
  EXPECT_TRUE(slaveCapabilities.contains(expectedCapabilities.get()));

  Try<Resources> resources = Resources::parse(
      agentFlags.resources.get(), agentFlags.default_role);

  ASSERT_SOME(resources);

  EXPECT_EQ(model(resources.get()), state.values["resources"]);

  Attributes attributes = Attributes::parse(agentFlags.attributes.get());

  EXPECT_EQ(model(attributes), state.values["attributes"]);

  // TODO(bmahler): Test "master_hostname", "log_dir",
  // "external_log_file".

  ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());
  EXPECT_TRUE(state.values["frameworks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(
      state.values["completed_frameworks"].is<JSON::Array>());
  EXPECT_TRUE(
      state.values["completed_frameworks"].as<JSON::Array>().values.empty());

  // TODO(bmahler): Ensure this contains all the agentFlags.
  ASSERT_TRUE(state.values["flags"].is<JSON::Object>());
  EXPECT_FALSE(state.values["flags"].as<JSON::Object>().values.empty());

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources executorResources = Resources::parse("cpus:0.1;mem:32").get();
  executorResources.allocate("*");

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(
      Resources(offers.get()[0].resources()) - executorResources);

  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  task.mutable_executor()->mutable_resources()->CopyFrom(executorResources);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  state = parse.get();
  ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());
  JSON::Array frameworks = state.values["frameworks"].as<JSON::Array>();
  EXPECT_EQ(1u, frameworks.values.size());

  ASSERT_TRUE(frameworks.values[0].is<JSON::Object>());

  JSON::Object roles = {
    { "roles", JSON::Array { DEFAULT_FRAMEWORK_INFO.roles(0) } }
  };

  EXPECT_TRUE(frameworks.values[0].contains(roles));

  JSON::Object framework = frameworks.values[0].as<JSON::Object>();
  EXPECT_EQ("default", framework.values["name"]);
  EXPECT_EQ(model(resources.get()), state.values["resources"]);

  ASSERT_TRUE(framework.values["executors"].is<JSON::Array>());
  JSON::Array executors = framework.values["executors"].as<JSON::Array>();
  EXPECT_EQ(1u, executors.values.size());

  ASSERT_TRUE(executors.values[0].is<JSON::Object>());
  JSON::Object executor = executors.values[0].as<JSON::Object>();

  EXPECT_EQ("default", executor.values["id"]);
  EXPECT_EQ("", executor.values["source"]);
  EXPECT_EQ("*", executor.values["role"]);
  EXPECT_EQ(
      model(Resources(task.resources()) +
            Resources(task.executor().resources())),
      executor.values["resources"]);

  Result<JSON::Array> tasks = executor.find<JSON::Array>("tasks");
  ASSERT_SOME(tasks);
  EXPECT_EQ(1u, tasks->values.size());

  JSON::Object taskJSON = tasks->values[0].as<JSON::Object>();
  EXPECT_EQ("default", taskJSON.values["executor_id"]);
  EXPECT_EQ("", taskJSON.values["name"]);
  EXPECT_EQ(taskId.value(), taskJSON.values["id"]);
  EXPECT_EQ("TASK_RUNNING", taskJSON.values["state"]);
  EXPECT_EQ("*", taskJSON.values["role"]);
  EXPECT_EQ(model(task.resources()), taskJSON.values["resources"]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Verifies that requests to the agent's '/state' endpoint are successful when
// there are pending tasks from a task group. This test was used to confirm the
// fix for MESOS-7871.
TEST_F(SlaveTest, GetStateTaskGroupPending)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  Resources resources = Resources::parse("cpus:0.1;mem:32;disk:32").get();

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  const ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      &containerizer,
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  process::PID<Slave> slavePid = slave.get()->pid;

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  // Override the default expectation, which forwards calls to the agent's
  // unmocked `_run()` method. Instead, we return a pending future to pause
  // the original continuation so that tasks remain in the framework's
  // 'pending' list.
  Promise<Nothing> promise;
  Future<Nothing> _run;
  EXPECT_CALL(*slave.get()->mock(), _run(_, _, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&_run),
                    Return(promise.future())));

  // The executor should not be launched.
  EXPECT_CALL(*executor, connected(_))
    .Times(0);

  v1::TaskInfo task1 = evolve(createTask(slaveId, resources, ""));

  v1::TaskInfo task2 = evolve(createTask(slaveId, resources, ""));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  // Wait for the tasks to be placed in 'pending'.
  AWAIT_READY(_run);

  Future<Response> response = process::http::get(
      slavePid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  // To confirm the fix for MESOS-7871, we simply verify that the
  // agent doesn't crash when this request is made.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}


// This test checks that when a slave is in RECOVERING state it responds
// to HTTP requests for "/state" endpoint with ServiceUnavailable.
TEST_F(SlaveTest, StateEndpointUnavailableDuringRecovery)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer1(&exec);
  TestContainerizer containerizer2;

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer1, flags);
  ASSERT_SOME(slave);

  // Launch a task so that slave has something to recover after restart.
  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 512, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Need this expectation here because `TestContainerizer` doesn't do recovery
  // and hence sets `MESOS_RECOVERY_TIMEOUT` as '0s' causing the executor driver
  // to exit immediately after slave exit.
  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Restart the slave.
  slave.get()->terminate();

  // Pause the clock to keep slave in RECOVERING state.
  Clock::pause();

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  slave = StartSlave(detector.get(), &containerizer2, flags);
  ASSERT_SOME(slave);

  // Ensure slave has setup the route for "/state".
  AWAIT_READY(_recover);

  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(ServiceUnavailable().status, response);

  driver.stop();
  driver.join();
}


// Tests that a client will receive an `Unauthorized` response when agent HTTP
// authentication is enabled and requests for the `/state` and `/flags`
// endpoints include invalid credentials or no credentials at all.
TEST_F(SlaveTest, HTTPEndpointsBadAuthentication)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // A credential that will not be accepted by the agent.
  Credential badCredential;
  badCredential.set_principal("bad-principal");
  badCredential.set_secret("bad-secret");

  // Capture the start time deterministically.
  Clock::pause();

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // HTTP authentication is enabled by default in `StartSlave`.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Ensure slave has finished recovery.
  AWAIT_READY(recover);
  Clock::settle();

  // Requests containing invalid credentials.
  {
    Future<Response> response = process::http::get(
        slave.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    response = process::http::get(
        slave.get()->pid,
        "flags",
        None(),
        createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // Requests containing no authentication headers.
  {
    Future<Response> response = process::http::get(slave.get()->pid, "state");
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    response = process::http::get(slave.get()->pid, "flags");
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }
}


// Tests that a client can talk to read-only endpoints when read-only
// authentication is disabled.
TEST_F(SlaveTest, ReadonlyHTTPEndpointsNoAuthentication)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Capture the start time deterministically.
  Clock::pause();

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readonly = false;

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Ensure slave has finished recovery.
  AWAIT_READY(recover);
  Clock::settle();

  // Requests containing no authentication headers.
  {
    Future<Response> response = process::http::get(slave.get()->pid, "state");
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    response = process::http::get(slave.get()->pid, "flags");
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    response = process::http::get(slave.get()->pid, "containers");
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  }
}


// Since executor authentication currently has SSL as a dependency, we cannot
// test executor authentication when Mesos has not been built with SSL.
#ifdef USE_SSL_SOCKET
// This test verifies that HTTP executor SUBSCRIBE and LAUNCH_NESTED_CONTAINER
// calls fail if the executor provides an incorrectly-signed authentication
// token with valid claims.
TEST_F(SlaveTest, HTTPExecutorBadAuthentication)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  Owned<TestContainerizer> containerizer(
      new TestContainerizer(devolve(executorInfo.executor_id()), executor));

  Owned<MockSecretGenerator> mockSecretGenerator(new MockSecretGenerator());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      mockSecretGenerator.get());

  ASSERT_SOME(slave);

  process::PID<Slave> slavePid = slave.get()->pid;

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  mesos.send(v1::createCallSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  Future<v1::executor::Mesos*> executorLib;
  EXPECT_CALL(*executor, connected(_))
    .WillOnce(FutureArg<0>(&executorLib));

  Promise<Secret> secret;
  Future<Principal> principal;
  EXPECT_CALL(*mockSecretGenerator, generate(_))
    .WillOnce(DoAll(FutureArg<0>(&principal),
                    Return(secret.future())));

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  {
    v1::TaskInfo taskInfo =
      v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

    v1::TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(taskInfo);

    v1::scheduler::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(v1::scheduler::Call::ACCEPT);

    v1::scheduler::Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(principal);

  // Create a secret generator initialized with an incorrect key.
  Owned<JWTSecretGenerator> jwtSecretGenerator(
      new JWTSecretGenerator("incorrect_key"));

  Future<Secret> authenticationToken =
    jwtSecretGenerator->generate(principal.get());

  AWAIT_READY(authenticationToken);

  secret.set(authenticationToken.get());

  {
    AWAIT_READY(executorLib);

    v1::executor::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);

    call.set_type(v1::executor::Call::SUBSCRIBE);

    call.mutable_subscribe();

    executorLib.get()->send(call);

    Future<v1::executor::Event::Error> error;
    EXPECT_CALL(*executor, error(_, _))
      .WillOnce(FutureArg<1>(&error));

    AWAIT_READY(error);
    EXPECT_EQ(
        error->message(),
        "Received unexpected '401 Unauthorized' () for SUBSCRIBE");
  }

  {
    ASSERT_TRUE(principal->claims.contains("cid"));

    v1::ContainerID parentContainerId;
    parentContainerId.set_value(principal->claims.at("cid"));

    v1::ContainerID containerId;
    containerId.set_value(id::UUID::random().toString());
    containerId.mutable_parent()->CopyFrom(parentContainerId);

    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER);

    call.mutable_launch_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    process::http::Headers headers;
    headers["Authorization"] = "Bearer " + authenticationToken->value().data();

    Future<Response> response = process::http::post(
      slavePid,
      "api/v1",
      headers,
      serialize(ContentType::PROTOBUF, call),
      stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    ASSERT_TRUE(response->headers.contains("WWW-Authenticate"));
    ASSERT_TRUE(strings::contains(
        response->headers.at("WWW-Authenticate"),
        "Invalid JWT: Token signature does not match"));
  }
}
#endif // USE_SSL_SOCKET


// This test verifies correct handling of statistics endpoint when
// there is no exeuctor running.
TEST_F(SlaveTest, StatisticsEndpointNoExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  Future<Response> response = process::http::get(
      slave.get()->pid,
      "/monitor/statistics",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);
}


// This test verifies the correct handling of the statistics
// endpoint when statistics is missing in ResourceUsage.
TEST_F(SlaveTest, StatisticsEndpointMissingStatistics)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      None());

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));
  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      SLEEP_COMMAND(1000),
      exec.id);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Set up the containerizer so the next usage() will fail.
  EXPECT_CALL(containerizer, usage(_))
    .WillOnce(Return(Failure("Injected failure")));

  Future<Response> response = process::http::get(
      slave.get()->pid,
      "monitor/statistics",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies the correct response of /monitor/statistics endpoint
// when ResourceUsage collection fails.
TEST_F(SlaveTest, StatisticsEndpointGetResourceUsageFailed)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  EXPECT_CALL(*slave.get()->mock(), usage())
    .WillOnce(Return(Failure("Resource Collection Failure")));

  slave.get()->start();

  Future<Response> response = process::http::get(
      slave.get()->pid,
      "monitor/statistics",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(InternalServerError().status, response);
}


// This is an end-to-end test that verifies that the slave returns the
// correct ResourceUsage based on the currently running executors, and
// the values returned by the /monitor/statistics endpoint are as expected.
TEST_F(SlaveTest, StatisticsEndpointRunningExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  // Launch a task and wait until it is in RUNNING status.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:32").get(),
      SLEEP_COMMAND(1000));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Hit the statistics endpoint and expect the response contains the
  // resource statistics for the running container.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "monitor/statistics",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  // Verify that the statistics in the response contains the proper
  // resource limits for the container.
  Try<JSON::Value> value = JSON::parse(response->body);
  ASSERT_SOME(value);

  Try<JSON::Value> expected = JSON::parse(strings::format(
      "[{"
          "\"statistics\":{"
              "\"cpus_soft_limit\":%g,"
              "\"mem_limit_bytes\":%lu,"
              "\"mem_soft_limit_bytes\":%lu"
          "}"
      "}]",
      1 + slave::DEFAULT_EXECUTOR_CPUS,
      (Megabytes(32) + slave::DEFAULT_EXECUTOR_MEM).bytes(),
      (Megabytes(32) + slave::DEFAULT_EXECUTOR_MEM).bytes()).get());

  ASSERT_SOME(expected);
  EXPECT_TRUE(value->contains(expected.get()));

  driver.stop();
  driver.join();
}


// This test confirms that an agent's statistics endpoint is
// authenticated. We rely on the agent implicitly having HTTP
// authentication enabled.
TEST_F(SlaveTest, StatisticsEndpointAuthentication)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  const string statisticsEndpoint = "monitor/statistics";

  // Unauthenticated requests are rejected.
  {
    Future<Response> response = process::http::get(
        agent.get()->pid,
        statisticsEndpoint);

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // Incorrectly authenticated requests are rejected.
  {
    Credential badCredential;
    badCredential.set_principal("badPrincipal");
    badCredential.set_secret("badSecret");

    Future<Response> response = process::http::get(
        agent.get()->pid,
        statisticsEndpoint,
        None(),
        createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // Correctly authenticated requests succeed.
  {
    Future<Response> response = process::http::get(
        agent.get()->pid,
        statisticsEndpoint,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  }
}


// This test verifies correct handling of containers endpoint when
// there is no exeuctor running.
TEST_F(SlaveTest, ContainersEndpointNoExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  Future<Response> response = process::http::get(
      slave.get()->pid,
      "containers",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("[]", response);
}


// This is an end-to-end test that verifies that the slave returns the
// correct container status and resource statistics based on the currently
// running executors, and ensures that '/containers' endpoint returns the
// correct container when it is provided a container ID query parameter.
TEST_F(SlaveTest, ContainersEndpoint)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Create two executors so that we can launch tasks in two separate
  // containers.
  ExecutorInfo executor1 = createExecutorInfo("executor-1", "exit 1");
  ExecutorInfo executor2 = createExecutorInfo("executor-2", "exit 1");

  MockExecutor exec1(executor1.executor_id());
  MockExecutor exec2(executor2.executor_id());

  hashmap<ExecutorID, Executor*> execs;
  execs[executor1.executor_id()] = &exec1;
  execs[executor2.executor_id()] = &exec2;

  TestContainerizer containerizer(execs);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
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

  // Launch two tasks, each under a different executor.
  vector<TaskInfo> tasks;

  TaskInfo task1;
  {
    task1.set_name("");
    task1.mutable_task_id()->set_value("1");
    task1.mutable_slave_id()->MergeFrom(offers->front().slave_id());
    task1.mutable_resources()->MergeFrom(
        Resources::parse("cpus:1;mem:512").get());
    task1.mutable_executor()->MergeFrom(executor1);
    tasks.push_back(task1);
  }

  TaskInfo task2;
  {
    task2.set_name("");
    task2.mutable_task_id()->set_value("2");
    task2.mutable_slave_id()->MergeFrom(offers->front().slave_id());
    task2.mutable_resources()->MergeFrom(
        Resources::parse("cpus:1;mem:512").get());
    task2.mutable_executor()->MergeFrom(executor2);
    tasks.push_back(task2);
  }

  EXPECT_CALL(exec1, registered(_, _, _, _));

  Future<TaskInfo> launchedTask1;
  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&launchedTask1)));

  EXPECT_CALL(exec2, registered(_, _, _, _));

  Future<TaskInfo> launchedTask2;
  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&launchedTask2)));

  Future<TaskStatus> status1, status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offers->front().id(), tasks);

  AWAIT_READY(launchedTask1);
  EXPECT_EQ(task1.task_id(), launchedTask1->task_id());

  AWAIT_READY(launchedTask2);
  EXPECT_EQ(task2.task_id(), launchedTask2->task_id());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2->state());

  // Prepare container statistics.
  ResourceStatistics statistics1;
  statistics1.set_mem_limit_bytes(2048);

  ResourceStatistics statistics2;
  statistics2.set_mem_limit_bytes(2048);

  // Get the container ID and return simulated statistics.
  Future<ContainerID> containerId1;
  Future<ContainerID> containerId2;

  // Will be called twice during the first request. We extract the assigned
  // container IDs for use when requesting information on a single container.
  EXPECT_CALL(containerizer, usage(_))
    .WillOnce(DoAll(FutureArg<0>(&containerId1), Return(statistics1)))
    .WillOnce(DoAll(FutureArg<0>(&containerId2), Return(statistics2)));

  // Construct the container statuses to be returned. Note that
  // these container IDs will be different than the actual container
  // IDs assigned by the agent, but creating them here allows us to
  // easily confirm the output of '/containers'.
  ContainerStatus containerStatus1;
  ContainerStatus containerStatus2;

  ContainerID parent;
  parent.set_value("parent");

  {
    ContainerID child;
    child.set_value("child1");
    child.mutable_parent()->CopyFrom(parent);
    containerStatus1.mutable_container_id()->CopyFrom(child);

    CgroupInfo* cgroupInfo = containerStatus1.mutable_cgroup_info();
    CgroupInfo::NetCls* netCls = cgroupInfo->mutable_net_cls();
    netCls->set_classid(42);

    NetworkInfo* networkInfo = containerStatus1.add_network_infos();
    NetworkInfo::IPAddress* ipAddr = networkInfo->add_ip_addresses();
    ipAddr->set_ip_address("192.168.1.20");
  }

  {
    ContainerID child;
    child.set_value("child2");
    child.mutable_parent()->CopyFrom(parent);
    containerStatus2.mutable_container_id()->CopyFrom(child);

    CgroupInfo* cgroupInfo = containerStatus2.mutable_cgroup_info();
    CgroupInfo::NetCls* netCls = cgroupInfo->mutable_net_cls();
    netCls->set_classid(42);

    NetworkInfo* networkInfo = containerStatus2.add_network_infos();
    NetworkInfo::IPAddress* ipAddr = networkInfo->add_ip_addresses();
    ipAddr->set_ip_address("192.168.1.21");
  }

  // Will be called twice during the first request.
  EXPECT_CALL(containerizer, status(_))
    .WillOnce(Return(containerStatus1))
    .WillOnce(Return(containerStatus2));

  // Request information about all containers.
  {
    Future<Response> response = process::http::get(
        slave.get()->pid,
        "containers",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    Try<JSON::Value> value = JSON::parse<JSON::Value>(response->body);
    ASSERT_SOME(value);

    JSON::Array array = value->as<JSON::Array>();

    EXPECT_TRUE(array.values.size() == 2);

    Try<JSON::Value> containerJson1 = JSON::parse(
        "{"
            "\"executor_name\":\"\","
            "\"source\":\"\","
            "\"statistics\":{"
                "\"mem_limit_bytes\":2048"
            "},"
            "\"status\":{"
                "\"container_id\":{"
                  "\"parent\":{\"value\":\"parent\"},"
                  "\"value\":\"child1\""
                "},"
                "\"cgroup_info\":{\"net_cls\":{\"classid\":42}},"
                "\"network_infos\":[{"
                    "\"ip_addresses\":[{\"ip_address\":\"192.168.1.20\"}]"
                "}]"
            "}"
          "}");

    Try<JSON::Value> containerJson2 = JSON::parse(
        "{"
            "\"executor_name\":\"\","
            "\"source\":\"\","
            "\"statistics\":{"
                "\"mem_limit_bytes\":2048"
            "},"
            "\"status\":{"
                "\"container_id\":{"
                  "\"parent\":{\"value\":\"parent\"},"
                  "\"value\":\"child2\""
                "},"
                "\"cgroup_info\":{\"net_cls\":{\"classid\":42}},"
                "\"network_infos\":[{"
                    "\"ip_addresses\":[{\"ip_address\":\"192.168.1.21\"}]"
                "}]"
            "}"
          "}");

    // Since containers are stored in a hashmap, there is no strict guarantee of
    // their ordering when listed. For this reason, we test both possibilities.
    if (array.values[0].contains(containerJson1.get())) {
      ASSERT_TRUE(array.values[1].contains(containerJson2.get()));
    } else {
      ASSERT_TRUE(array.values[0].contains(containerJson2.get()));
      ASSERT_TRUE(array.values[1].contains(containerJson1.get()));
    }
  }

  AWAIT_READY(containerId1);
  AWAIT_READY(containerId2);

  // Will be called once during the second request.
  EXPECT_CALL(containerizer, usage(_))
    .WillOnce(Return(statistics1));

  // Will be called once during the second request and might be called if
  // the `TASK_FAILED` update reaches the agent before the test finishes.
  EXPECT_CALL(containerizer, status(_))
    .WillOnce(Return(containerStatus1))
    .WillRepeatedly(Return(containerStatus1));

  {
    Future<Response> response = process::http::get(
        slave.get()->pid,
        "containers?container_id=" + containerId1->value(),
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    Try<JSON::Value> value = JSON::parse<JSON::Value>(response->body);
    ASSERT_SOME(value);

    JSON::Array array = value->as<JSON::Array>();

    EXPECT_TRUE(array.values.size() == 1);

    Try<JSON::Value> expected = JSON::parse(
        "[{"
            "\"container_id\":\"" + containerId1->value() + "\","
            "\"executor_name\":\"\","
            "\"source\":\"\","
            "\"statistics\":{"
                "\"mem_limit_bytes\":2048"
            "},"
            "\"status\":{"
              "\"container_id\":{"
                "\"parent\":{\"value\":\"parent\"},"
                  "\"value\":\"child1\""
                "},"
                "\"cgroup_info\":{\"net_cls\":{\"classid\":42}},"
                "\"network_infos\":[{"
                    "\"ip_addresses\":[{\"ip_address\":\"192.168.1.20\"}]"
                "}]"
            "}"
        "}]");

    ASSERT_SOME(expected);
    EXPECT_TRUE(value->contains(expected.get()));
  }

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));
  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test ensures that `/containerizer/debug` endpoint returns a non-empty
// list of pending futures when an isolator becomes unresponsive during
// container launch.
TEST_F(SlaveTest, ROOT_ContainerizerDebugEndpoint)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Try<Launcher*> _launcher = SubprocessLauncher::create(flags);
  ASSERT_SOME(_launcher);

  Owned<Launcher> launcher(_launcher.get());

  MockIsolator* mockIsolator = new MockIsolator();

  Future<Nothing> prepare;
  Promise<Option<ContainerLaunchInfo>> promise;

  EXPECT_CALL(*mockIsolator, recover(_, _))
    .WillOnce(Return(Nothing()));

  // Simulate a long prepare from the isolator.
  EXPECT_CALL(*mockIsolator, prepare(_, _))
    .WillOnce(DoAll(FutureSatisfy(&prepare),
                    Return(promise.future())));

  EXPECT_CALL(*mockIsolator, update(_, _, _))
    .WillOnce(Return(Nothing()));

  // Wrap `mockIsolator` in `PendingFutureTracker`.
  Try<PendingFutureTracker*> _futureTracker = PendingFutureTracker::create();
  ASSERT_SOME(_futureTracker);

  Owned<PendingFutureTracker> futureTracker(_futureTracker.get());

  Owned<Isolator> isolator = Owned<Isolator>(new IsolatorTracker(
      Owned<Isolator>(mockIsolator), "MockIsolator", futureTracker.get()));

  Fetcher fetcher(flags);

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher,
      nullptr,
      launcher,
      provisioner->share(),
      {isolator});

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(SlaveOptions(detector.get())
                 .withContainerizer(containerizer.get())
                 .withFutureTracker(futureTracker.get()));

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  // Launch a task and wait until it is in RUNNING status.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:32").get(),
      SLEEP_COMMAND(1000));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(prepare);

  {
    Future<Response> response = process::http::get(
        slave.get()->pid,
        "containerizer/debug",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    ASSERT_FALSE(parse->at<JSON::Array>("pending")->values.empty());
    ASSERT_TRUE(strings::contains(response->body, "MockIsolator::prepare"));
  }

  // Once the future returned by the `prepare` method becomes ready,
  // the task should start successfully and no pending futures should be
  // contained in the output returned by `/containerizer/debug` endpoint.
  promise.set(Option<ContainerLaunchInfo>(ContainerLaunchInfo()));

  AWAIT_READY(statusStarting);
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  {
    Future<Response> response = process::http::get(
        slave.get()->pid,
        "containerizer/debug",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    ASSERT_TRUE(parse->at<JSON::Array>("pending")->values.empty());
  }

  driver.stop();
  driver.join();
}


// This test ensures that when a slave is shutting down, it will not
// try to reregister with the master.
//
// TODO(alexr): Enable after MESOS-3509 is resolved.
TEST_F(SlaveTest, DISABLED_TerminatingSlaveDoesNotReregister)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Create a MockExecutor to enable us to catch
  // ShutdownExecutorMessage later.
  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Create a StandaloneMasterDetector to enable the slave to trigger
  // re-registration later.
  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags flags = CreateSlaveFlags();

  // Make the executor_shutdown_grace_period to be much longer than
  // REGISTER_RETRY_INTERVAL, so that the slave will at least call
  // call doReliableRegistration() once before the slave is actually
  // terminated.
  flags.executor_shutdown_grace_period = slave::REGISTER_RETRY_INTERVAL_MAX * 2;

  // Start a slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &containerizer, flags);
  ASSERT_SOME(slave);

  // Create a task on the slave.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Launch a task that uses less resource than the
  // default(cpus:2, mem:1024).
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Pause the clock here so that after detecting a new master,
  // the slave will not send multiple reregister messages
  // before we change its state to TERMINATING.
  Clock::pause();

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    DROP_PROTOBUF(
        SlaveReregisteredMessage(),
        master.get()->pid,
        slave.get()->pid);

  // Simulate a new master detected event on the slave,
  // so that the slave will do a re-registration.
  detector.appoint(master.get()->pid);

  // Make sure the slave has entered doReliableRegistration()
  // before we change the slave's state.
  AWAIT_READY(slaveReregisteredMessage);

  // Setup an expectation that the master should not receive any
  // ReregisterSlaveMessage in the future.
  EXPECT_NO_FUTURE_PROTOBUFS(
      ReregisterSlaveMessage(),
      slave.get()->pid,
      master.get()->pid);

  // Drop the ShutdownExecutorMessage, so that the slave will
  // stay in TERMINATING for a while.
  DROP_PROTOBUFS(ShutdownExecutorMessage(), slave.get()->pid, _);

  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .WillOnce(FutureSatisfy(&executorLost));

  // Send a ShutdownMessage instead of calling Stop() directly
  // to avoid blocking.
  post(master.get()->pid, slave.get()->pid, ShutdownMessage());

  // Advance the clock to trigger doReliableRegistration().
  Clock::advance(slave::REGISTER_RETRY_INTERVAL_MAX * 2);
  Clock::settle();
  Clock::resume();

  AWAIT_READY(executorLost);

  driver.stop();
  driver.join();
}


// This test verifies the slave will destroy a container if, when
// receiving a terminal status task update, updating the container's
// resources fails. A non-partition-aware framework should receive
// TASK_LOST in this situation.
TEST_F(SlaveTest, TerminalTaskContainerizerUpdateFailsWithLost)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  EXPECT_CALL(exec, registered(_, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  // Connect a non-partition-aware scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  // Start two tasks.
  vector<TaskInfo> tasks;

  tasks.push_back(createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      SLEEP_COMMAND(1000),
      exec.id));

  tasks.push_back(createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      SLEEP_COMMAND(1000),
      exec.id));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1, status2, status3, status4;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4));

  driver.launchTasks(offer.id(), tasks);

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2->state());

  // Set up the containerizer so the next update() will fail.
  EXPECT_CALL(containerizer, update(_, _, _))
    .WillOnce(Return(Failure("update() failed")))
    .WillRepeatedly(Return(Nothing()));

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .WillOnce(FutureSatisfy(&executorLost));

  // Kill one of the tasks. The failed update should result in the
  // second task going lost when the container is destroyed.
  driver.killTask(tasks[0].task_id());

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_KILLED, status3->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, status3->source());

  AWAIT_READY(status4);
  EXPECT_EQ(TASK_LOST, status4->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status4->source());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_UPDATE_FAILED, status4->reason());

  AWAIT_READY(executorLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(0, stats.values["slave/tasks_gone"]);
  EXPECT_EQ(1, stats.values["slave/tasks_lost"]);

  driver.stop();
  driver.join();
}


// This test verifies the slave will destroy a container if, when
// receiving a terminal status task update, updating the container's
// resources fails. A partition-aware framework should receive
// TASK_GONE in this situation.
TEST_F(SlaveTest, TerminalTaskContainerizerUpdateFailsWithGone)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  EXPECT_CALL(exec, registered(_, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  // Connect a partition-aware scheduler.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  // Start two tasks.
  vector<TaskInfo> tasks;

  tasks.push_back(createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      SLEEP_COMMAND(1000),
      exec.id));

  tasks.push_back(createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      SLEEP_COMMAND(1000),
      exec.id));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1, status2, status3, status4;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4));

  driver.launchTasks(offer.id(), tasks);

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2->state());

  // Set up the containerizer so the next update() will fail.
  EXPECT_CALL(containerizer, update(_, _, _))
    .WillOnce(Return(Failure("update() failed")))
    .WillRepeatedly(Return(Nothing()));

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .WillOnce(FutureSatisfy(&executorLost));

  // Kill one of the tasks. The failed update should result in the
  // second task going lost when the container is destroyed.
  driver.killTask(tasks[0].task_id());

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_KILLED, status3->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, status3->source());

  AWAIT_READY(status4);
  EXPECT_EQ(TASK_GONE, status4->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status4->source());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_UPDATE_FAILED, status4->reason());

  AWAIT_READY(executorLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["slave/tasks_gone"]);
  EXPECT_EQ(0, stats.values["slave/tasks_lost"]);

  driver.stop();
  driver.join();
}


// This test verifies that the resources of a container will be
// updated before tasks are sent to the executor.
TEST_F(SlaveTest, ContainerUpdatedBeforeTaskReachesExecutor)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  EXPECT_CALL(exec, registered(_, _, _, _));

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 128, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // This is used to determine which of the following finishes first:
  // `containerizer->update` or `exec->launchTask`. We want to make
  // sure that containerizer update always finishes before the task is
  // sent to the executor.
  testing::Sequence sequence;

  EXPECT_CALL(containerizer, update(_, _, _))
    .InSequence(sequence)
    .WillOnce(Return(Nothing()));

  EXPECT_CALL(exec, launchTask(_, _))
    .InSequence(sequence)
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies the slave will destroy a container if updating
// the container's resources fails during task launch.
TEST_F(SlaveTest, TaskLaunchContainerizerUpdateFails)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 128, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // The executor may not receive the ExecutorRegisteredMessage if the
  // container is destroyed before that.
  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(AtMost(1));

  // Set up the containerizer so update() will fail.
  EXPECT_CALL(containerizer, update(_, _, _))
    .WillOnce(Return(Failure("update() failed")))
    .WillRepeatedly(Return(Nothing()));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_UPDATE_FAILED, status->reason());

  driver.stop();
  driver.join();
}


// This test ensures that the slave will reregister with the master
// if it does not receive any pings after registering.
TEST_F(SlaveTest, PingTimeoutNoPings)
{
  // Set shorter ping timeout values.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.agent_ping_timeout = Seconds(5);
  masterFlags.max_agent_ping_timeouts = 2u;
  Duration totalTimeout =
    masterFlags.agent_ping_timeout * masterFlags.max_agent_ping_timeouts;

  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Block all pings to the slave.
  DROP_PROTOBUFS(PingSlaveMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_TRUE(slaveRegisteredMessage->has_connection());
  MasterSlaveConnection connection = slaveRegisteredMessage->connection();
  EXPECT_EQ(
      totalTimeout,
      Seconds(static_cast<int64_t>(connection.total_ping_timeout_seconds())));

  // Ensure the slave processes the registration message and schedules
  // the ping timeout, before we advance the clock.
  Clock::pause();
  Clock::settle();

  // Advance to the ping timeout to trigger a re-detection and
  // re-registration.
  Future<Nothing> detected = FUTURE_DISPATCH(_, &Slave::detected);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Clock::advance(totalTimeout);
  AWAIT_READY(detected);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregisteredMessage);
}


// This test ensures that the slave will reregister with the master
// if it stops receiving pings.
TEST_F(SlaveTest, PingTimeoutSomePings)
{
  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  Clock::pause();

  // Ensure a ping reaches the slave.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(ping);

  // Now block further pings from the master and advance
  // the clock to trigger a re-detection and re-registration on
  // the slave.
  DROP_PROTOBUFS(PingSlaveMessage(), _, _);

  Future<Nothing> detected = FUTURE_DISPATCH(_, &Slave::detected);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Clock::advance(slave::DEFAULT_MASTER_PING_TIMEOUT());
  AWAIT_READY(detected);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregisteredMessage);
}


// This test ensures that when a slave removal rate limit is
// specified, the master only removes a slave that fails health checks
// when it is permitted to do so by the rate limiter.
TEST_F(SlaveTest, RateLimitSlaveRemoval)
{
  // Start a master.
  auto slaveRemovalLimiter = std::make_shared<MockRateLimiter>();
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master =
    StartMaster(slaveRemovalLimiter, masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate health check timeout.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(resourceOffers);

  // Return a pending future from the rate limiter.
  Future<Nothing> acquire;
  Promise<Nothing> promise;
  EXPECT_CALL(*slaveRemovalLimiter, acquire())
    .WillOnce(DoAll(FutureSatisfy(&acquire),
                    Return(promise.future())));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(Return()); // Expect a single offer to be rescinded.

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Induce a health check failure of the slave.
  Clock::pause();
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Clock::advance(masterFlags.agent_ping_timeout);

  // The master should attempt to acquire a permit.
  AWAIT_READY(acquire);

  // The slave should not be removed before the permit is satisfied;
  // that means the scheduler shouldn't receive `slaveLost` yet.
  Clock::settle();
  ASSERT_TRUE(slaveLost.isPending());

  // Once the permit is satisfied, the `slaveLost` scheduler callback
  // should be invoked.
  promise.set(Nothing());
  AWAIT_READY(slaveLost);

  driver.stop();
  driver.join();
}


// This test verifies that when a slave responds to pings after the
// slave observer has scheduled it for removal (due to health check
// failure), the slave removal is cancelled.
TEST_F(SlaveTest, CancelSlaveRemoval)
{
  // Start a master.
  auto slaveRemovalLimiter = std::make_shared<MockRateLimiter>();
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master =
    StartMaster(slaveRemovalLimiter, masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate health check timeout.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(sched, slaveLost(&driver, _))
    .Times(0); // The `slaveLost` callback should not be invoked.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(resourceOffers);

  // Return a pending future from the rate limiter.
  Future<Nothing> acquire;
  Promise<Nothing> promise;
  EXPECT_CALL(*slaveRemovalLimiter, acquire())
    .WillOnce(DoAll(FutureSatisfy(&acquire),
                    Return(promise.future())));

  // Induce a health check failure of the slave.
  Clock::pause();
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Clock::advance(masterFlags.agent_ping_timeout);

  // The master should attempt to acquire a permit.
  AWAIT_READY(acquire);

  // Settle to make sure the slave removal does not occur.
  Clock::settle();

  // Reset the filters to allow pongs from the slave.
  filter(nullptr);

  // Advance clock enough to do a ping pong.
  Clock::advance(masterFlags.agent_ping_timeout);
  Clock::settle();

  // The master should have tried to cancel the removal.
  EXPECT_TRUE(promise.future().hasDiscard());

  // Allow the cancellation and settle the clock to ensure the
  // `slaveLost` scheduler callback is not invoked.
  promise.discard();
  Clock::settle();
}


#ifndef __WINDOWS__
// This test checks that the master behaves correctly when a slave
// fails health checks, but concurrently the slave unregisters from
// the master.
TEST_F(SlaveTest, HealthCheckUnregisterRace)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start a slave.
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(offers);

  SlaveID slaveId = offers.get()[0].slave_id();

  // Expect a single offer to be rescinded.
  EXPECT_CALL(sched, offerRescinded(&driver, _));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Cause the slave to shutdown gracefully. This should result in
  // the slave sending `UnregisterSlaveMessage` to the master.
  Future<UnregisterSlaveMessage> unregisterSlaveMessage =
    FUTURE_PROTOBUF(
        UnregisterSlaveMessage(),
        slave.get()->pid,
        master.get()->pid);

  slave.get()->shutdown();
  slave->reset();

  AWAIT_READY(unregisterSlaveMessage);
  AWAIT_READY(slaveLost);

  Clock::pause();
  Clock::settle();

  // We now want to arrange for the agent to fail health checks. We
  // can't do that directly, because the `SlaveObserver` for this
  // agent has already been removed. Instead, we dispatch to the
  // master's `markUnreachable` method directly. We expect the master
  // to ignore this message; in particular, the master should not
  // attempt to update the registry to mark the slave unreachable.
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  SlaveInfo slaveInfo;
  slaveInfo.mutable_id()->CopyFrom(slaveId);
  slaveInfo.set_hostname("hostname");

  process::dispatch(master.get()->pid,
                    &Master::markUnreachable,
                    slaveInfo,
                    false,
                    "dummy test case dispatch");

  Clock::settle();
  Clock::resume();

  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


// This test verifies that when an unreachable agent reregisters after
// master failover, the master consults and updates the registrar for
// re-admitting the agent.
//
// TODO(andschwa): Enable when Windows supports replicated log. See MESOS-5932.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    SlaveTest, UnreachableAgentReregisterAfterFailover)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = CreateSlaveFlags();

  // Drop all the PONGs to simulate slave partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  slave.get()->terminate();
  slave->reset();

  Clock::pause();

  // Settle here to make sure the `SlaveObserver` has already started counting
  // the `slavePingTimeout` before we advance the clock for the first time.
  Clock::settle();

  // Induce agent ping timeouts.
  size_t pings = 0;
  while (true) {
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    Clock::advance(masterFlags.agent_ping_timeout);
    Clock::settle();
  }

  // Now set the expectation when the agent is one ping timeout away
  // from being deemed unreachable.
  Future<Owned<master::RegistryOperation>> markUnreachable;
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&markUnreachable),
                    Invoke(master.get()->registrar.get(),
                           &MockRegistrar::unmocked_apply)));

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(markUnreachable);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::MarkSlaveUnreachable*>(markUnreachable->get()));

  // Make sure the registrar operation completes so the agent will be updated
  // as an unreachable agent in the registry before the master terminates.
  Clock::settle();

  master->reset();

  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start the agent, which will cause it to reregister. Intercept the
  // next registry operation, which we expect to be slave reregistration.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  Future<Owned<master::RegistryOperation>> markReachable;
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&markReachable),
                    Invoke(master.get()->registrar.get(),
                           &MockRegistrar::unmocked_apply)));

  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  // Verify that the reregistration involves registry update.
  AWAIT_READY(markReachable);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::MarkSlaveReachable*>(markReachable->get()));

  AWAIT_READY(slaveReregisteredMessage);
}


// This test verifies that when a registered agent restarts and reregisters
// after master failover, the master does not consult the registrar in
// deciding to re-admit the agent.
//
// TODO(andschwa): Enable when Windows supports replicated log. See MESOS-5932.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    SlaveTest, RegisteredAgentReregisterAfterFailover)
{
  // Pause the clock to avoid registration retries and the agent
  // being deemed unreachable.
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);

  // There should be no registrar operation across both agent termination
  // and reregistration.
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  slave.get()->terminate();
  slave->reset();

  master->reset();

  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  // No registrar operation occurs by the time the agent is fully registered.
  AWAIT_READY(slaveReregisteredMessage);
}


#ifndef __WINDOWS__
// This test checks that the master behaves correctly when a slave
// fails health checks and is in the process of being marked
// unreachable in the registry, but concurrently the slave unregisters
// from the master.
TEST_F(SlaveTest, UnreachableThenUnregisterRace)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  // Intercept the next registry operation. This operation should be
  // attempting to mark the slave unreachable.
  Future<Owned<master::RegistryOperation>> markUnreachable;
  Promise<bool> markUnreachableContinue;
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&markUnreachable),
                    Return(markUnreachableContinue.future())));

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(markUnreachable);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::MarkSlaveUnreachable*>(
          markUnreachable->get()));

  // Cause the slave to shutdown gracefully.  This should result in
  // the slave sending `UnregisterSlaveMessage` to the master.
  // Normally, the master would then remove the slave from the
  // registry, but since the slave is already being marked
  // unreachable, the master should ignore the unregister message.
  Future<UnregisterSlaveMessage> unregisterSlaveMessage =
    FUTURE_PROTOBUF(
        UnregisterSlaveMessage(),
        slave.get()->pid,
        master.get()->pid);

  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  slave.get()->shutdown();
  slave->reset();

  AWAIT_READY(unregisterSlaveMessage);

  // Apply the registry operation to mark the slave unreachable, then
  // pass the result back to the master to allow it to continue.
  Future<bool> applyUnreachable =
    master.get()->registrar->unmocked_apply(markUnreachable.get());

  AWAIT_READY(applyUnreachable);
  markUnreachableContinue.set(applyUnreachable.get());

  AWAIT_READY(slaveLost);

  Clock::resume();

  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


// This test checks that the master behaves correctly when a slave is
// in the process of unregistering from the master when it is marked
// unreachable.
TEST_F(SlaveTest, UnregisterThenUnreachableRace)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(resourceOffers);

  ASSERT_EQ(1u, resourceOffers->size());
  SlaveID slaveId = resourceOffers.get()[0].slave_id();

  Clock::pause();

  // Simulate the slave shutting down gracefully. This might happen
  // normally if the slave receives SIGUSR1. However, we don't use
  // that approach here, because that would also result in an `exited`
  // event at the master; we want to test the case where the slave
  // begins to shutdown but the socket hasn't been closed yet. Hence,
  // we spoof the `UnregisterSlaveMessage`.
  //
  // When the master receives the `UnregisterSlaveMessage`, it should
  // attempt to remove the slave from the registry.
  Future<Owned<master::RegistryOperation>> removeSlave;
  Promise<bool> removeSlaveContinue;
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&removeSlave),
                    Return(removeSlaveContinue.future())));

  process::dispatch(master.get()->pid,
                    &Master::unregisterSlave,
                    slave.get()->pid,
                    slaveId);

  AWAIT_READY(removeSlave);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::RemoveSlave*>(removeSlave->get()));

  // Next, cause the slave to fail health checks; master will attempt
  // to mark it unreachable.
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  // We expect the `SlaveObserver` to dispatch a message to the master
  // to mark the slave unreachable. The master should ignore this
  // request because the slave is already being removed.
  Future<Nothing> unreachableDispatch =
    FUTURE_DISPATCH(master.get()->pid, &Master::markUnreachable);

  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(unreachableDispatch);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Apply the registry operation to remove the slave, then pass the
  // result back to the master to allow it to continue.
  Future<bool> applyRemove =
    master.get()->registrar->unmocked_apply(removeSlave.get());

  AWAIT_READY(applyRemove);
  removeSlaveContinue.set(applyRemove.get());

  AWAIT_READY(slaveLost);

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test ensures that a killTask() can happen between runTask()
// and _run() and then gets "handled properly". This means that
// the task never gets started, but also does not get lost. The end
// result is status TASK_KILLED. Essentially, killing the task is
// realized while preparing to start it. See MESOS-947. This test
// removes the framework and proves that removeFramework() is
// called. See MESOS-1945.
TEST_F(SlaveTest, KillTaskBetweenRunTaskParts)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

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

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(0);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(0);

  EXPECT_CALL(exec, shutdown(_))
    .Times(0);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillRepeatedly(FutureArg<1>(&status));

  EXPECT_CALL(*slave.get()->mock(), runTask(_, _, _, _, _, _, _))
    .WillOnce(Invoke(slave.get()->mock(), &MockSlave::unmocked_runTask));

  // Saved arguments from Slave::_run().
  FrameworkInfo frameworkInfo;
  ExecutorInfo executorInfo;
  Option<TaskGroupInfo> taskGroup;
  Option<TaskInfo> task_;
  vector<ResourceVersionUUID> resourceVersionUuids;
  Option<bool> launchExecutor;

  // Skip what Slave::_run() normally does, save its arguments for
  // later, return a pending future to pause the original continuation,
  // so that we can control when the task is killed.
  Promise<Nothing> promise;
  Future<Nothing> _run;
  EXPECT_CALL(*slave.get()->mock(), _run(_, _, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&_run),
                    SaveArg<0>(&frameworkInfo),
                    SaveArg<1>(&executorInfo),
                    SaveArg<2>(&task_),
                    SaveArg<3>(&taskGroup),
                    SaveArg<4>(&resourceVersionUuids),
                    SaveArg<5>(&launchExecutor),
                    Return(promise.future())));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(_run);

  Future<Nothing> killTask;
  EXPECT_CALL(*slave.get()->mock(), killTask(_, _))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask)));

  Future<Nothing> removeFramework;
  EXPECT_CALL(*slave.get()->mock(), removeFramework(_))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .Times(AtMost(1));

  driver.killTask(task.task_id());

  AWAIT_READY(killTask);

  // The agent will remove the framework when killing this task
  // since there remain no more tasks.
  AWAIT_READY(removeFramework);

  Future<Nothing> unmocked__run = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        frameworkInfo,
        executorInfo,
        task_,
        taskGroup,
        resourceVersionUuids,
        launchExecutor);
  });

  // Resume the original continuation once `unmocked__run` is complete.
  promise.associate(unmocked__run);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  AWAIT(unmocked__run);

  driver.stop();
  driver.join();
}


// This test ensures was added due to MESOS-7863, where the
// agent previously dropped TASK_KILLED in the cases outlined
// in the issue.
TEST_F(SlaveTest, KillMultiplePendingTasks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

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

  // We only pause the clock after receiving the offer since the
  // agent uses a delay to reregister.
  //
  // TODO(bmahler): Remove the initial agent delay within the tests.
  Clock::pause();

  Resources taskResources = Resources::parse("cpus:0.1;mem:32;disk:32").get();

  TaskInfo task1 = createTask(
      offers->at(0).slave_id(), taskResources, "echo hi");

  TaskInfo task2 = createTask(
      offers->at(0).slave_id(), taskResources, "echo hi");

  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .Times(0);

  Future<TaskStatus> status1, status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  EXPECT_CALL(*slave.get()->mock(), runTask(_, _, _, _, _, _, _))
    .WillOnce(Invoke(slave.get()->mock(), &MockSlave::unmocked_runTask))
    .WillOnce(Invoke(slave.get()->mock(), &MockSlave::unmocked_runTask));

  // Skip what Slave::_run() normally does, save its arguments for
  // later, return a pending future to pause the original continuation,
  // so that we can control when the task is killed.
  FrameworkInfo frameworkInfo1, frameworkInfo2;
  ExecutorInfo executorInfo1, executorInfo2;
  Option<TaskGroupInfo> taskGroup1, taskGroup2;
  Option<TaskInfo> task_1, task_2;
  vector<ResourceVersionUUID> resourceVersionUuids1, resourceVersionUuids2;
  Option<bool> launchExecutor1, launchExecutor2;

  Promise<Nothing> promise1, promise2;
  Future<Nothing> _run1, _run2;
  EXPECT_CALL(*slave.get()->mock(), _run(_, _, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&_run1),
                    SaveArg<0>(&frameworkInfo1),
                    SaveArg<1>(&executorInfo1),
                    SaveArg<2>(&task_1),
                    SaveArg<3>(&taskGroup1),
                    SaveArg<4>(&resourceVersionUuids1),
                    SaveArg<5>(&launchExecutor1),
                    Return(promise1.future())))
    .WillOnce(DoAll(FutureSatisfy(&_run2),
                    SaveArg<0>(&frameworkInfo2),
                    SaveArg<1>(&executorInfo2),
                    SaveArg<2>(&task_2),
                    SaveArg<3>(&taskGroup2),
                    SaveArg<4>(&resourceVersionUuids2),
                    SaveArg<5>(&launchExecutor2),
                    Return(promise2.future())));

  driver.launchTasks(offers.get()[0].id(), {task1, task2});

  AWAIT_READY(process::await(_run1, _run2));

  Future<Nothing> killTask1, killTask2;
  EXPECT_CALL(*slave.get()->mock(), killTask(_, _))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask1)))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask2)));

  Future<Nothing> removeFramework;
  EXPECT_CALL(*slave.get()->mock(), removeFramework(_))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  driver.killTask(task1.task_id());
  driver.killTask(task2.task_id());

  AWAIT_READY(process::await(killTask1, killTask2));

  // We expect the tasks to be killed and framework removed.
  AWAIT_READY(status1);
  EXPECT_EQ(TASK_KILLED, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_KILLED, status2->state());

  AWAIT_READY(removeFramework);

  // The `__run` continuations should have no effect.
  Future<Nothing> unmocked__run1 = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        frameworkInfo1,
        executorInfo1,
        task_1,
        taskGroup1,
        resourceVersionUuids1,
        launchExecutor1);
  });

  Future<Nothing> unmocked__run2 = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        frameworkInfo2,
        executorInfo2,
        task_2,
        taskGroup2,
        resourceVersionUuids2,
        launchExecutor2);
  });

  // Resume the original continuation once unmocked__run is complete.
  promise1.associate(unmocked__run1);
  promise2.associate(unmocked__run2);

  Clock::settle();

  driver.stop();
  driver.join();
}


// This test verifies that when the agent gets a `killTask`
// message for a queued task on a registering executor, a
// the agent will generate a TASK_KILLED and will shut down
// the executor.
TEST_F(SlaveTest, KillQueuedTaskDuringExecutorRegistration)
{
  Clock::pause();

  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::settle();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(0);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(0);

  EXPECT_CALL(exec, shutdown(_));

  // Hold on to the executor registration message so that the task stays
  // queued on the agent.
  Future<Message> registerExecutorMessage =
    DROP_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(registerExecutorMessage);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .WillOnce(FutureSatisfy(&executorLost));

  // Kill the task enqueued on the agent.
  driver.killTask(task.task_id());

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_KILLED_DURING_LAUNCH, status->reason());

  // Now let the executor register by spoofing the message.
  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage->body);

  process::post(registerExecutorMessage->from,
                slave.get()->pid,
                registerExecutor);

  Clock::advance(slaveFlags.executor_shutdown_grace_period);

  AWAIT_READY(executorLost);

  driver.stop();
  driver.join();
}


// This test ensures that if a `killTask()` for an HTTP based executor is
// received by the agent before the executor registers, the executor is
// properly cleaned up.
TEST_F(SlaveTest, KillTaskUnregisteredHTTPExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  const ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  Future<v1::executor::Mesos*> executorLib;
  EXPECT_CALL(*executor, connected(_))
    .WillOnce(FutureArg<0>(&executorLib));

  v1::TaskInfo task1 =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskInfo task2 =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offers->offers(0).id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  // Wait for the executor to be launched and then kill the task before
  // the executor subscribes with the agent.
  AWAIT_READY(executorLib);

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

  mesos.send(
      v1::createCallKill(frameworkId, task1.task_id(), offer.agent_id()));

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  ASSERT_EQ(v1::TASK_KILLED, update1->status().state());
  ASSERT_EQ(v1::TASK_KILLED, update2->status().state());

  Future<Nothing> shutdown;
  EXPECT_CALL(*executor, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  // The executor should receive the shutdown event upon subscribing
  // with the agent.
  {
    v1::executor::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(evolve(executorId));

    call.set_type(v1::executor::Call::SUBSCRIBE);

    call.mutable_subscribe();

    executorLib.get()->send(call);
  }

  AWAIT_READY(shutdown);
}


// This test ensures that the agent sends an `ExitedExecutorMessage` when the
// executor is never launched, so that the master's executor bookkeeping entry
// is removed. See MESOS-1720.
TEST_F(SlaveTest, RemoveExecutorUponFailedLaunch)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:512;disk:512;ports:[]";

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags, true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources executorResources = Resources::parse("cpus:0.1;mem:32").get();
  executorResources.allocate("*");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(
      Resources(offers.get()[0].resources()) - executorResources);

  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  task.mutable_executor()->mutable_resources()->CopyFrom(executorResources);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(0);

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(AtMost(1));

  Future<ExitedExecutorMessage> exitedExecutorMessage =
    FUTURE_PROTOBUF(ExitedExecutorMessage(), _, _);

  // Saved arguments from `Slave::_run()`.
  FrameworkInfo frameworkInfo;
  ExecutorInfo executorInfo_;
  Option<TaskGroupInfo> taskGroup_;
  Option<TaskInfo> task_;
  vector<ResourceVersionUUID> resourceVersionUuids;
  Option<bool> launchExecutor;

  // Before launching the executor in `__run`, we pause the continuation
  // by returning a pending future. We then kill the task and re-dispatch
  // `_run`. We use its return result to fulfill the pending future and
  // resume the continuation.
  Promise<Nothing> promise;
  Future<Nothing> _run;
  EXPECT_CALL(*slave.get()->mock(), _run(_, _, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&_run),
                  SaveArg<0>(&frameworkInfo),
                  SaveArg<1>(&executorInfo_),
                  SaveArg<2>(&task_),
                  SaveArg<3>(&taskGroup_),
                  SaveArg<4>(&resourceVersionUuids),
                  SaveArg<5>(&launchExecutor),
                  Return(promise.future())));

  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .WillOnce(FutureSatisfy(&executorLost));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(_run);

  Future<Nothing> killTask;
  EXPECT_CALL(*slave.get()->mock(), killTask(_, _))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask)));

  driver.killTask(task.task_id());

  AWAIT_READY(killTask);

  Future<Nothing> unmocked__run =
    process::dispatch(slave.get()->pid, [=]() -> Future<Nothing> {
      return slave.get()->mock()->unmocked__run(
          frameworkInfo,
          executorInfo_,
          task_,
          taskGroup_,
          resourceVersionUuids,
          launchExecutor);
    });

  promise.associate(unmocked__run);

  AWAIT(unmocked__run);

  // Agent needs to send `ExitedExecutorMessage` to the master because
  // the executor never launched.
  AWAIT_READY(exitedExecutorMessage);

  AWAIT_READY(executorLost);

  // Helper function to post a request to '/api/v1' master endpoint
  // and return the response.
  auto post = [](
      const process::PID<master::Master>& pid,
      const v1::master::Call& call,
      const ContentType& contentType)
  {
    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    return process::http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));
  };

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_EXECUTORS);

  Future<process::http::Response> response =
    post(master.get()->pid, v1Call, ContentType::PROTOBUF);

  response.await();
  ASSERT_EQ(response->status, process::http::OK().status);

  Future<v1::master::Response> v1Response =
    deserialize<v1::master::Response>(ContentType::PROTOBUF, response->body);

  // Master has no executor entry because the executor never launched.
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_EXECUTORS, v1Response->type());
  ASSERT_EQ(0, v1Response->get_executors().executors_size());

  driver.stop();
  driver.join();
}


// This test ensures that agent sends ExitedExecutorMessage when the task group
// fails to launch due to unschedule GC failure and that master's executor
// bookkeeping entry is removed.
TEST_F(SlaveTest, RemoveExecutorUponFailedTaskGroupLaunch)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  MockGarbageCollector mockGarbageCollector(slaveFlags.work_dir);

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &mockGarbageCollector, slaveFlags, true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::DEFAULT_EXECUTOR_INFO;
  executorInfo.clear_command();
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_resources()->CopyFrom(resources);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
    master.get()->pid,
    ContentType::PROTOBUF,
    scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  mesos.send(v1::createCallSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo task1 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskInfo task2 = v1::createTask(agentId, resources, "sleep 1000");

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(executorInfo, taskGroup);

  // The `unschedule()` function is used to prevent premature garbage
  // collection when the executor directory already exists due to a
  // previously-launched task. Simulate this scenario by creating the
  // executor directory manually.
  string path = paths::getExecutorPath(
      slaveFlags.work_dir,
      devolve(agentId),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()));

  Try<Nothing> mkdir = os::mkdir(path, true);
  CHECK_SOME(mkdir);

  // Induce agent unschedule GC failure. This will result in
  // task launch failure before the executor launch.
  EXPECT_CALL(mockGarbageCollector, unschedule(_))
    .WillRepeatedly(Return(Failure("")));

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

  Future<ExitedExecutorMessage> exitedExecutorMessage =
    FUTURE_PROTOBUF(ExitedExecutorMessage(), _, _);

  EXPECT_CALL(*scheduler, failure(_, _))
    .Times(AtMost(1));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(exitedExecutorMessage);

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  ASSERT_EQ(v1::TASK_LOST, update1->status().state());
  ASSERT_EQ(v1::TASK_LOST, update2->status().state());

  // Helper function to post a request to '/api/v1' master endpoint
  // and return the response.
  auto post = [](
      const process::PID<master::Master>& pid,
      const v1::master::Call& call,
      const ContentType& contentType)
  {
    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    return process::http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));
  };

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_EXECUTORS);

  Future<process::http::Response> response =
    post(master.get()->pid, v1Call, ContentType::PROTOBUF);

  response.await();
  ASSERT_EQ(response->status, process::http::OK().status);

  Future<v1::master::Response> v1Response =
    deserialize<v1::master::Response>(ContentType::PROTOBUF, response->body);

  // Master has no executor entry because the executor never launched.
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_EXECUTORS, v1Response->type());
  ASSERT_EQ(0, v1Response->get_executors().executors_size());
}


// This test ensures that tasks using the same executor are successfully
// launched in the order in which the agent receives the RunTask(Group)Message,
// even when we manually reorder the completion of the asynchronous unschedule
// GC step. See MESOS-8624.
TEST_F(SlaveTest, LaunchTasksReorderUnscheduleGC)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slaveFlags = CreateSlaveFlags();
  MockGarbageCollector mockGarbageCollector(slaveFlags.work_dir);

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &mockGarbageCollector, slaveFlags, true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  v1::scheduler::TestMesos mesos(
    master.get()->pid, ContentType::PROTOBUF, scheduler);

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(subscribed);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "default", None(), resources, v1::ExecutorInfo::DEFAULT, frameworkId);

  // Create two separate task groups that use the same executor.
  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup1 = v1::createTaskGroupInfo({taskInfo1});

  v1::TaskInfo taskInfo2 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup2 = v1::createTaskGroupInfo({taskInfo2});

  v1::Offer::Operation launchGroup1 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup1);
  v1::Offer::Operation launchGroup2 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup2);

  // The `unschedule()` function is used to prevent premature garbage
  // collection when the executor directory already exists due to a
  // previously-launched task. Simulate this scenario by creating the
  // executor directory manually.
  string path = paths::getExecutorPath(
      slaveFlags.work_dir,
      devolve(agentId),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()));

  Try<Nothing> mkdir = os::mkdir(path, true);
  CHECK_SOME(mkdir);

  Promise<bool> promise1;

  // Catch the unschedule GC step and reorder the task group launches by
  // pausing the processing of `taskGroup1` while allowing the processing
  // of `taskGroup1` to continue.
  EXPECT_CALL(mockGarbageCollector, unschedule(StrEq(path)))
    .WillOnce(Return(promise1.future()))
    .WillRepeatedly(Return(true));

  Future<v1::scheduler::Event::Update> taskStarting1, taskStarting2;
  Future<v1::scheduler::Event::Update> taskRunning1, taskRunning2;
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo1.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&taskStarting1),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&taskRunning1),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo2.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&taskStarting2),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&taskRunning2),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Launch the two task groups.
  mesos.send(
      v1::createCallAccept(frameworkId, offer, {launchGroup1, launchGroup2}));

  // Settle the clock to finish the processing of `taskGroup2`.
  Clock::settle();

  ASSERT_TRUE(taskStarting2.isPending());

  // Resume the processing of `taskGroup1`.
  promise1.set(true);

  // If taskgroup2 tries to launch the executor first (i.e. if the order is
  // not corrected by the agent), taskgroup2 will be subsequently dropped. The
  // successful launch of both tasks verifies that the agent enforces the task
  // launch order.
  AWAIT_READY(taskStarting1);
  AWAIT_READY(taskStarting2);

  ASSERT_EQ(v1::TASK_STARTING, taskStarting1->status().state());
  ASSERT_EQ(v1::TASK_STARTING, taskStarting2->status().state());

  AWAIT_READY(taskRunning1);
  AWAIT_READY(taskRunning2);

  ASSERT_EQ(v1::TASK_RUNNING, taskRunning1->status().state());
  ASSERT_EQ(v1::TASK_RUNNING, taskRunning2->status().state());
}


// This test ensures that tasks using the same executor are successfully
// launched in the order in which the agent receives the RunTask(Group)Message,
// even when we manually reorder the completion of the asynchronous task
// authorization step. See MESOS-8624.
TEST_F(SlaveTest, LaunchTasksReorderTaskAuthorization)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slaveFlags = CreateSlaveFlags();
  MockAuthorizer mockAuthorizer;

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &mockAuthorizer, CreateSlaveFlags(), true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(subscribed);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "default", None(), resources, v1::ExecutorInfo::DEFAULT, frameworkId);

  // Create two separate task groups that use the same executor.
  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup1 = v1::createTaskGroupInfo({taskInfo1});

  v1::TaskInfo taskInfo2 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup2 = v1::createTaskGroupInfo({taskInfo2});

  v1::Offer::Operation launchGroup1 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup1);
  v1::Offer::Operation launchGroup2 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup2);

  // Catch the task authorization step by returning a pending future.
  Promise<bool> promise1, promise2;
  EXPECT_CALL(
      mockAuthorizer,
      authorized(AuthorizationRequestHasTaskID(devolve(taskInfo1.task_id()))))
    .WillOnce(Return(promise1.future()));
  EXPECT_CALL(
      mockAuthorizer,
      authorized(AuthorizationRequestHasTaskID(devolve(taskInfo2.task_id()))))
    .WillOnce(Return(promise2.future()));

  Future<v1::scheduler::Event::Update> taskStarting1, taskStarting2;
  Future<v1::scheduler::Event::Update> taskRunning1, taskRunning2;
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo1.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&taskStarting1),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&taskRunning1),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo2.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&taskStarting2),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&taskRunning2),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Launch the two task groups.
  mesos.send(
      v1::createCallAccept(frameworkId, offer, {launchGroup1, launchGroup2}));

  // Reorder the task group launches by resuming
  // the processing of `taskGroup2` first.
  promise2.set(true);

  // Settle the clock to finish the processing of `taskGroup2`.
  Clock::settle();

  ASSERT_TRUE(taskStarting2.isPending());

  promise1.set(true);

  // If taskgroup2 tries to launch the executor first (i.e. if the order is
  // not corrected by the agent), taskgroup2 will be subsequently dropped. The
  // successful launch of both tasks verifies that the agent enforces the task
  // launch order.
  AWAIT_READY(taskStarting1);
  AWAIT_READY(taskStarting2);

  ASSERT_EQ(v1::TASK_STARTING, taskStarting1->status().state());
  ASSERT_EQ(v1::TASK_STARTING, taskStarting2->status().state());

  AWAIT_READY(taskRunning1);
  AWAIT_READY(taskRunning2);

  ASSERT_EQ(v1::TASK_RUNNING, taskRunning1->status().state());
  ASSERT_EQ(v1::TASK_RUNNING, taskRunning2->status().state());
}


// This test verifies the agent behavior of launching three task groups using
// the same executor. When all three task groups are launching on the agent
// (before creating any executor), if the first received task group fails to
// launch, subsequent task group launches would also fail.
TEST_F(SlaveTest, LaunchTaskGroupsUsingSameExecutorKillFirstTaskGroup)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), slaveFlags, true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());
  slave.get()->start();

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, failure(_, _))
    .Times(AtMost(1));

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(subscribed);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "default1", None(), resources, v1::ExecutorInfo::DEFAULT, frameworkId);

  // Create three separate task groups that use the same executor.

  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup1 = v1::createTaskGroupInfo({taskInfo1});

  v1::TaskInfo taskInfo2 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup2 = v1::createTaskGroupInfo({taskInfo2});

  v1::TaskInfo taskInfo3 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup3 = v1::createTaskGroupInfo({taskInfo3});

  v1::Offer::Operation launchGroup1 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup1);
  v1::Offer::Operation launchGroup2 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup2);
  v1::Offer::Operation launchGroup3 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup3);

  Future<v1::scheduler::Event::Update> task1Killed;
  Future<v1::scheduler::Event::Update> task2Lost;
  Future<v1::scheduler::Event::Update> task3Lost;
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo1.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&task1Killed),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo2.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&task2Lost),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo3.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&task3Lost),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Saved arguments from `Slave::_run()`.
  FrameworkInfo _frameworkInfo1, _frameworkInfo2, _frameworkInfo3;
  ExecutorInfo _executorInfo1, _executorInfo2, _executorInfo3;
  Option<TaskGroupInfo> _taskGroup1, _taskGroup2,  _taskGroup3;
  Option<TaskInfo> _task1, _task2, _task3;
  vector<ResourceVersionUUID>
    _resourceVersionUuids1, _resourceVersionUuids2, _resourceVersionUuids3;
  Option<bool> _launchExecutor1, _launchExecutor2, _launchExecutor3;

  // Pause all taskgroups at `_run` by returning a pending future.
  Promise<Nothing> promise1, promise2, promise3;
  Future<Nothing> _run1, _run2, _run3;
  EXPECT_CALL(
      *slave.get()->mock(),
      _run(_, _, _,
          OptionTaskGroupHasTaskID(devolve(taskInfo1.task_id())),
          _, _))
    .WillOnce(DoAll(
        FutureSatisfy(&_run1),
        SaveArg<0>(&_frameworkInfo1),
        SaveArg<1>(&_executorInfo1),
        SaveArg<2>(&_task1),
        SaveArg<3>(&_taskGroup1),
        SaveArg<4>(&_resourceVersionUuids1),
        SaveArg<5>(&_launchExecutor1),
        Return(promise1.future())));
  EXPECT_CALL(
      *slave.get()->mock(),
      _run(_, _, _,
          OptionTaskGroupHasTaskID(devolve(taskInfo2.task_id())),
          _, _))
    .WillOnce(DoAll(
        FutureSatisfy(&_run2),
        SaveArg<0>(&_frameworkInfo2),
        SaveArg<1>(&_executorInfo2),
        SaveArg<2>(&_task2),
        SaveArg<3>(&_taskGroup2),
        SaveArg<4>(&_resourceVersionUuids2),
        SaveArg<5>(&_launchExecutor2),
        Return(promise2.future())));
  EXPECT_CALL(
      *slave.get()->mock(),
      _run(_, _, _,
          OptionTaskGroupHasTaskID(devolve(taskInfo3.task_id())),
          _, _))
    .WillOnce(DoAll(
        FutureSatisfy(&_run3),
        SaveArg<0>(&_frameworkInfo3),
        SaveArg<1>(&_executorInfo3),
        SaveArg<2>(&_task3),
        SaveArg<3>(&_taskGroup3),
        SaveArg<4>(&_resourceVersionUuids3),
        SaveArg<5>(&_launchExecutor3),
        Return(promise3.future())));

  // Launch task groups.
  mesos.send(
      v1::createCallAccept(
          frameworkId, offer, {launchGroup1, launchGroup2, launchGroup3}));

  AWAIT_READY(_run1);
  AWAIT_READY(_run2);
  AWAIT_READY(_run3);

  Future<Nothing> killTask1;
  EXPECT_CALL(*slave.get()->mock(), killTask(_, _))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask1)));

  // Kill task1.
  mesos.send(v1::createCallKill(frameworkId, taskInfo1.task_id(), agentId));

  AWAIT_READY(killTask1);

  // Resume the continuation for `taskGroup1`.
  Future<Nothing> unmocked__run1 = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        _frameworkInfo1,
        _executorInfo1,
        _task1,
        _taskGroup1,
        _resourceVersionUuids1,
        _launchExecutor1);
  });

  promise1.associate(unmocked__run1);

  AWAIT(unmocked__run1);
  AWAIT_READY(task1Killed);

  EXPECT_EQ(v1::TASK_KILLED, task1Killed->status().state());

  // Resume the continuation for taskgroup2.
  Future<Nothing> unmocked__run2 = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        _frameworkInfo2,
        _executorInfo2,
        _task2,
        _taskGroup2,
        _resourceVersionUuids2,
        _launchExecutor2);
  });

  promise2.associate(unmocked__run2);

  // Resume the continuation for taskgroup3.
  Future<Nothing> unmocked__run3 = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        _frameworkInfo3,
        _executorInfo3,
        _task3,
        _taskGroup3,
        _resourceVersionUuids3,
        _launchExecutor3);
  });

  promise3.associate(unmocked__run3);

  AWAIT(unmocked__run2);
  AWAIT_READY(task2Lost);

  EXPECT_EQ(v1::TASK_LOST, task2Lost->status().state());

  AWAIT(unmocked__run3);
  AWAIT_READY(task3Lost);

  EXPECT_EQ(v1::TASK_LOST, task3Lost->status().state());
}


// This test verifies the agent behavior of launching two task groups using
// the same executor. When both task groups are launching on the agent
// (before creating any executor), if the second received task group fails to
// launch, the first task group can continue launching successfully.
TEST_F(SlaveTest, LaunchTaskGroupsUsingSameExecutorKillLaterTaskGroup)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), slaveFlags, true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());
  slave.get()->start();

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, failure(_, _))
    .Times(AtMost(1));

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(subscribed);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "default1", None(), resources, v1::ExecutorInfo::DEFAULT, frameworkId);

  // Create two separate task groups that use the same executor.

  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup1 = v1::createTaskGroupInfo({taskInfo1});

  v1::TaskInfo taskInfo2 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup2 = v1::createTaskGroupInfo({taskInfo2});

  v1::Offer::Operation launchGroup1 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup1);
  v1::Offer::Operation launchGroup2 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup2);

  Future<v1::scheduler::Event::Update> task1Starting, task1Running;
  Future<v1::scheduler::Event::Update> task2Killed;
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo1.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&task1Starting),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&task1Running),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo2.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&task2Killed),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Saved arguments from `Slave::_run()`.
  FrameworkInfo _frameworkInfo1, _frameworkInfo2;
  ExecutorInfo _executorInfo1, _executorInfo2;
  Option<TaskGroupInfo> _taskGroup1, _taskGroup2;
  Option<TaskInfo> _task1, _task2;
  vector<ResourceVersionUUID> _resourceVersionUuids1, _resourceVersionUuids2;
  Option<bool> _launchExecutor1, _launchExecutor2;

  // Pause both taskgroups at `_run` by returning a pending future.
  Promise<Nothing> promise1, promise2;
  Future<Nothing> _run1, _run2;
  EXPECT_CALL(
      *slave.get()->mock(),
      _run(_, _, _,
          OptionTaskGroupHasTaskID(devolve(taskInfo1.task_id())),
          _, _))
    .WillOnce(DoAll(
        FutureSatisfy(&_run1),
        SaveArg<0>(&_frameworkInfo1),
        SaveArg<1>(&_executorInfo1),
        SaveArg<2>(&_task1),
        SaveArg<3>(&_taskGroup1),
        SaveArg<4>(&_resourceVersionUuids1),
        SaveArg<5>(&_launchExecutor1),
        Return(promise1.future())));
  EXPECT_CALL(
      *slave.get()->mock(),
      _run(_, _, _,
          OptionTaskGroupHasTaskID(devolve(taskInfo2.task_id())),
          _, _))
    .WillOnce(DoAll(
        FutureSatisfy(&_run2),
        SaveArg<0>(&_frameworkInfo2),
        SaveArg<1>(&_executorInfo2),
        SaveArg<2>(&_task2),
        SaveArg<3>(&_taskGroup2),
        SaveArg<4>(&_resourceVersionUuids2),
        SaveArg<5>(&_launchExecutor2),
        Return(promise2.future())));

  // Launch the two task groups.
  mesos.send(
      v1::createCallAccept(frameworkId, offer, {launchGroup1, launchGroup2}));

  AWAIT_READY(_run1);
  AWAIT_READY(_run2);

  Future<Nothing> killTask2;
  EXPECT_CALL(*slave.get()->mock(), killTask(_, _))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask2)));

  // Kill task2.
  mesos.send(v1::createCallKill(frameworkId, taskInfo2.task_id(), agentId));

  AWAIT_READY(killTask2);

  // Resume the continuation for `taskGroup2`.
  Future<Nothing> unmocked__run2 = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        _frameworkInfo2,
        _executorInfo2,
        _task2,
        _taskGroup2,
        _resourceVersionUuids2,
        _launchExecutor2);
  });

  promise2.associate(unmocked__run2);

  AWAIT(unmocked__run2);
  AWAIT_READY(task2Killed);

  EXPECT_EQ(v1::TASK_KILLED, task2Killed->status().state());

  // Resume the continuation for taskgroup1.
  Future<Nothing> unmocked__run1 = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        _frameworkInfo1,
        _executorInfo1,
        _task1,
        _taskGroup1,
        _resourceVersionUuids1,
        _launchExecutor1);
  });

  promise1.associate(unmocked__run1);

  AWAIT(unmocked__run1);

  AWAIT_READY(task1Starting);
  EXPECT_EQ(v1::TASK_STARTING, task1Starting->status().state());

  AWAIT_READY(task1Running);
  EXPECT_EQ(v1::TASK_RUNNING, task1Running->status().state());
}


// This test verifies that when agent shuts down a running executor, launching
// tasks on the agent that use the same executor will be dropped properly.
TEST_F(SlaveTest, ShutdownExecutorWhileTaskLaunching)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), slaveFlags, true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());
  slave.get()->start();

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, failure(_, _))
    .Times(AtMost(1));

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(subscribed);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "default1", None(), resources, v1::ExecutorInfo::DEFAULT, frameworkId);

  // Create two separate task groups that use the same executor.
  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup1 = v1::createTaskGroupInfo({taskInfo1});

  v1::TaskInfo taskInfo2 = v1::createTask(agentId, resources, "sleep 1000");
  v1::TaskGroupInfo taskGroup2 = v1::createTaskGroupInfo({taskInfo2});

  v1::Offer::Operation launchGroup1 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup1);
  v1::Offer::Operation launchGroup2 =
    v1::LAUNCH_GROUP(executorInfo, taskGroup2);

  Future<v1::scheduler::Event::Update> task1Starting, task1Running;
  Future<v1::scheduler::Event::Update> task2Lost;
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo1.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&task1Starting),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&task1Running),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));
  EXPECT_CALL(
      *scheduler, update(_, TaskStatusUpdateTaskIdEq(taskInfo2.task_id())))
    .WillOnce(DoAll(
        FutureArg<1>(&task2Lost),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  // Saved arguments from `Slave::_run()` for `taskGroup2`.
  FrameworkInfo _frameworkInfo;
  ExecutorInfo _executorInfo;
  Option<TaskGroupInfo> _taskGroup;
  Option<TaskInfo> _task;
  vector<ResourceVersionUUID> _resourceVersionUuids;
  Option<bool> _launchExecutor;

  // Pause the launch of `taskGroup2` at `_run` by returning a pending future.
  Promise<Nothing> promiseTask2;
  Future<Nothing> runTask2;
  EXPECT_CALL(
      *slave.get()->mock(),
      _run(_, _, _,
           OptionTaskGroupHasTaskID(devolve(taskInfo2.task_id())),
           _, _))
    .WillOnce(
        DoAll(FutureSatisfy(&runTask2),
        SaveArg<0>(&_frameworkInfo),
        SaveArg<1>(&_executorInfo),
        SaveArg<2>(&_task),
        SaveArg<3>(&_taskGroup),
        SaveArg<4>(&_resourceVersionUuids),
        SaveArg<5>(&_launchExecutor),
        Return(promiseTask2.future())));

  // Launch the two task groups.
  mesos.send(
      v1::createCallAccept(frameworkId, offer, {launchGroup1, launchGroup2}));

  AWAIT_READY(runTask2);

  // `taskGroup1` launches successfully.
  AWAIT_READY(task1Starting);
  EXPECT_EQ(v1::TASK_STARTING, task1Starting->status().state());

  AWAIT_READY(task1Running);
  EXPECT_EQ(v1::TASK_RUNNING, task1Running->status().state());

  // Shutdown the executor while `taskGroup2` is still launching.
  Future<Nothing> shutdownExecutor;
  EXPECT_CALL(*slave.get()->mock(), shutdownExecutor(_, _, _))
    .WillOnce(DoAll(
        Invoke(slave.get()->mock(), &MockSlave::unmocked_shutdownExecutor),
        FutureSatisfy(&shutdownExecutor)));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::SHUTDOWN);

    Call::Shutdown* shutdown = call.mutable_shutdown();
    shutdown->mutable_executor_id()->CopyFrom(executorInfo.executor_id());
    shutdown->mutable_agent_id()->CopyFrom(agentId);

    mesos.send(call);
  }

  AWAIT_READY(shutdownExecutor);

  // Resume launching `taskGroup2`.
  Future<Nothing> unmocked__run = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        _frameworkInfo,
        _executorInfo,
        _task,
        _taskGroup,
        _resourceVersionUuids,
        _launchExecutor);
  });

  promiseTask2.associate(unmocked__run);

  // `taskGroup2` is dropped because the executor is terminated.
  AWAIT_READY(task2Lost);
  EXPECT_EQ(v1::TASK_LOST, task2Lost->status().state());
}


// This test ensures that agent sends ExitedExecutorMessage when the task
// fails to launch due to task authorization failure and that master's executor
// bookkeeping entry is removed.
TEST_F(SlaveTest, RemoveExecutorUponFailedTaskAuthorization)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  MockAuthorizer mockAuthorizer;

  slave::Flags slaveFlags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      &containerizer,
      &mockAuthorizer,
      slaveFlags,
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources executorResources = Resources::parse("cpus:0.1;mem:32").get();
  executorResources.allocate("*");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(
      Resources(offers.get()[0].resources()) - executorResources);

  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  task.mutable_executor()->mutable_resources()->CopyFrom(executorResources);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(0);

  Future<TaskStatus> statusError;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusError));

  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .Times(AtMost(1));

  Future<ExitedExecutorMessage> exitedExecutorMessage =
    FUTURE_PROTOBUF(ExitedExecutorMessage(), _, _);

  // Induce agent task authorization failure. This will result in
  // task launch failure before the executor launch.
  EXPECT_CALL(mockAuthorizer, authorized(_))
    .WillRepeatedly(Return(false));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(exitedExecutorMessage);

  AWAIT_READY(statusError);
  ASSERT_EQ(TASK_ERROR, statusError->state());

  // Helper function to post a request to '/api/v1' master endpoint
  // and return the response.
  auto post = [](
      const process::PID<master::Master>& pid,
      const v1::master::Call& call,
      const ContentType& contentType)
  {
    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    return process::http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));
  };

  v1::master::Call v1Call;
  v1Call.set_type(v1::master::Call::GET_EXECUTORS);

  Future<process::http::Response> response =
    post(master.get()->pid, v1Call, ContentType::PROTOBUF);

  response.await();
  ASSERT_EQ(response->status, process::http::OK().status);

  Future<v1::master::Response> v1Response =
    deserialize<v1::master::Response>(ContentType::PROTOBUF, response->body);

  // Master has no executor entry because the executor never launched.
  ASSERT_TRUE(v1Response->IsInitialized());
  ASSERT_EQ(v1::master::Response::GET_EXECUTORS, v1Response->type());
  ASSERT_EQ(0, v1Response->get_executors().executors_size());

  driver.stop();
  driver.join();
}


// This test verifies that the executor is shutdown if all of its initial
// tasks could not be delivered, even after the executor has been registered.
// See MESOS-8411.
TEST_F(SlaveTest, KillAllInitialTasksTerminatesExecutor)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, CreateSlaveFlags(), true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources executorResources = Resources::parse("cpus:0.1;mem:32").get();
  executorResources.allocate("*");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers->at(0).slave_id());
  task.mutable_resources()->MergeFrom(
      Resources(offers->at(0).resources()) - executorResources);

  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  task.mutable_executor()->mutable_resources()->CopyFrom(executorResources);

  Future<TaskStatus> killTaskStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&killTaskStatus));

  // Saved arguments from `Slave::___run()`.
  Future<Nothing> future;
  FrameworkID frameworkId;
  ExecutorID executorId;
  ContainerID containerId;
  vector<TaskInfo> tasks;
  vector<TaskGroupInfo> taskGroups;

  // Kill the task after executor registration but before
  // task launch in `___run()`.
  Future<Nothing> ___run;
  EXPECT_CALL(*slave.get()->mock(), ___run(_, _, _, _, _, _))
    .WillOnce(DoAll(
        SaveArg<0>(&future),
        SaveArg<1>(&frameworkId),
        SaveArg<2>(&executorId),
        SaveArg<3>(&containerId),
        SaveArg<4>(&tasks),
        SaveArg<5>(&taskGroups),
        FutureSatisfy(&___run)
        ));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(0);

  // The exeuctor is killed because its initial task is killed
  // and cannot be delivered.
  Future<Nothing> executorShutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&executorShutdown));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(___run);

  driver.killTask(task.task_id());

  // Task is killed before the actual `___run()` is triggered.
  AWAIT_READY(killTaskStatus);
  EXPECT_EQ(TASK_KILLED, killTaskStatus->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_KILLED_DURING_LAUNCH,
      killTaskStatus->reason());

  // We continue dispatching `___run()` to make sure the `CHECK()` in the
  // continuation passes.
  Future<Nothing> unmocked___run = process::dispatch(slave.get()->pid, [=] {
    slave.get()->mock()->unmocked____run(
        future, frameworkId, executorId, containerId, tasks, taskGroups);

    return Nothing();
  });

  AWAIT_READY(unmocked___run);
  AWAIT_READY(executorShutdown);

  driver.stop();
  driver.join();
}


// This test verifies that the executor is shutdown during re-registration if
// all of its initial tasks could not be delivered.
TEST_F(SlaveTest, AgentFailoverTerminatesExecutorWithNoTask)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), slaveFlags, true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers->front(), "sleep 1000");

  // Before sending the the task to the executor, restart the agent.
  Future<Nothing> ___run;
  EXPECT_CALL(*slave.get()->mock(), ___run(_, _, _, _, _, _))
    .WillOnce(FutureSatisfy(&___run));

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(___run);

  slave.get()->terminate();

  slave = StartSlave(detector.get(), slaveFlags, true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  Future<Nothing> shutdownExecutor;
  EXPECT_CALL(*slave.get()->mock(), _shutdownExecutor(_, _))
    .WillOnce(FutureSatisfy(&shutdownExecutor));

  slave.get()->start();

  // The executor is killed during reregistration because its initial task is
  // killed and cannot be delivered.
  AWAIT_READY(shutdownExecutor);

  driver.stop();
  driver.join();
}


// This test verifies that the v1 executor is shutdown if all of its initial
// task group could not be delivered, even after the executor has been
// registered. See MESOS-8411. This test only uses task group.
//
// TODO(mzhu): This test could be simplified if we had a test scheduler that
// provides some basic task launching functionality (see MESOS-8511).
TEST_F(SlaveTest, KillAllInitialTasksTerminatesHTTPExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::DEFAULT_EXECUTOR_INFO;
  executorInfo.set_type(v1::ExecutorInfo::CUSTOM);
  executorInfo.mutable_resources()->CopyFrom(resources);

  const v1::ExecutorID& executorId = executorInfo.executor_id();

  TestContainerizer containerizer(devolve(executorId), executor);

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, CreateSlaveFlags(), true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  mesos.send(
      v1::createCallSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Future<v1::executor::Mesos*> executorLib;
  EXPECT_CALL(*executor, connected(_))
    .WillOnce(FutureArg<0>(&executorLib));

  EXPECT_CALL(*executor, subscribed(_, _));

  // Saved arguments from `Slave::___run()`.
  Future<Nothing> _future;
  FrameworkID _frameworkId;
  ExecutorID _executorId;
  ContainerID _containerId;
  vector<TaskInfo> _tasks;
  vector<TaskGroupInfo> _taskGroups;

  // Kill the task after executor subscription but before
  // task launch in `___run()`.
  Future<Nothing> taskRun;
  EXPECT_CALL(*slave.get()->mock(), ___run(_, _, _, _, _, _))
    .WillOnce(DoAll(
      SaveArg<0>(&_future),
      SaveArg<1>(&_frameworkId),
      SaveArg<2>(&_executorId),
      SaveArg<3>(&_containerId),
      SaveArg<4>(&_tasks),
      SaveArg<5>(&_taskGroups),
      FutureSatisfy(&taskRun)
      ));

  v1::TaskInfo task1 =
    v1::createTask(agentId, resources, "");

  v1::TaskInfo task2 =
    v1::createTask(agentId, resources, "");

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(executorInfo, taskGroup);

  mesos.send(
      v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(executorLib);

  {
    v1::executor::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(executorId);

    call.set_type(v1::executor::Call::SUBSCRIBE);

    call.mutable_subscribe();

    executorLib.get()->send(call);
  }

  // Kill the task right before the task launch. By now, the executor has
  // already subscribed.
  AWAIT_READY(taskRun);

  Future<Nothing> shutdown;
  EXPECT_CALL(*executor, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

  EXPECT_CALL(*scheduler, failure(_, _))
    .Times(AtMost(1));

  // We kill only one of the tasks and expect the entire task group to be
  // killed.
  mesos.send(
      v1::createCallKill(frameworkId, task1.task_id(), offer.agent_id()));

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  ASSERT_EQ(v1::TASK_KILLED, update1->status().state());
  ASSERT_EQ(v1::TASK_KILLED, update2->status().state());

  // We continue dispatching `___run()` to make sure the `CHECK()` in the
  // continuation passes.
  Future<Nothing> unmocked____run = process::dispatch(slave.get()->pid, [=] {
    slave.get()->mock()->unmocked____run(
        _future, _frameworkId, _executorId, _containerId, _tasks, _taskGroups);

    return Nothing();
  });

  // The executor is killed because all of its initial tasks are killed
  // and cannot be delivered.
  AWAIT_READY(shutdown);

  // It is necessary to wait for the `unmocked____run` call to finish before we
  // get out of scope. Otherwise, the objects we captured by reference and
  // passed to `unmocked___run` may be destroyed in the middle of the function.
  AWAIT_READY(unmocked____run);
}


// This test verifies that if an agent fails over after registering
// a v1 executor but before delivering its initial task groups, the
// executor will be shut down since all of its initial task groups
// were dropped. See MESOS-8411.
//
// TODO(mzhu): This test could be simplified if we had a test scheduler that
// provides some basic task launching functionality (see MESOS-8511).
TEST_F(SlaveTest, AgentFailoverTerminatesHTTPExecutorWithNoTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  string processId = process::ID::generate("slave");

  // Start a mock slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), processId, slaveFlags, true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  // Enable checkpointing for the framework.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::DEFAULT_EXECUTOR_INFO;
  executorInfo.clear_command();
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_resources()->CopyFrom(resources);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  mesos.send(
      v1::createCallSubscribe(frameworkInfo));

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Future<Nothing> ___run;
  EXPECT_CALL(*slave.get()->mock(), ___run(_, _, _, _, _, _))
    .WillOnce(FutureSatisfy(&___run));

  v1::TaskInfo task1 =
    v1::createTask(agentId, resources, "sleep 1000");

  v1::TaskInfo task2 =
    v1::createTask(agentId, resources, "sleep 1000");

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(executorInfo, taskGroup);

  mesos.send(
      v1::createCallAccept(frameworkId, offer, {launchGroup}));

  // Before sending the the task to the executor, restart the agent.
  AWAIT_READY(___run);

  slave.get()->terminate();
  slave->reset();

  slave = StartSlave(detector.get(), processId, slaveFlags, true);

  slave.get()->start();

  Future<Nothing> _shutdownExecutor;
  EXPECT_CALL(*slave.get()->mock(), _shutdownExecutor(_, _))
    .WillOnce(FutureSatisfy(&_shutdownExecutor));

  // The executor is killed because all of its initial tasks are killed
  // and cannot be delivered.
  AWAIT_READY(_shutdownExecutor);
}


// This test verifies that when a slave reregisters with the master
// it correctly includes the latest and status update task states.
TEST_F(SlaveTest, ReregisterWithStatusUpdateTaskState)
{
  Clock::pause();

  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Create a StandaloneMasterDetector to enable the slave to trigger
  // re-registration later.
  StandaloneMasterDetector detector(master.get()->pid);

  // Start a slave.
  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &containerizer, agentFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 1024, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Signal when the first update is dropped.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  Future<Nothing> ___statusUpdate = FUTURE_DISPATCH(_, &Slave::___statusUpdate);

  driver.start();

  Clock::advance(masterFlags.allocation_interval);

  // Wait until TASK_RUNNING is sent to the master.
  AWAIT_READY(statusUpdateMessage);

  // Ensure task status update manager handles TASK_RUNNING update.
  AWAIT_READY(___statusUpdate);

  Future<Nothing> ___statusUpdate2 =
    FUTURE_DISPATCH(_, &Slave::___statusUpdate);

  // Now send TASK_FINISHED update.
  TaskStatus finishedStatus;
  finishedStatus = statusUpdateMessage->update().status();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  // Ensure task status update manager handles TASK_FINISHED update.
  AWAIT_READY(___statusUpdate2);

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Drop any updates to the failed over master.
  DROP_PROTOBUFS(StatusUpdateMessage(), _, master.get()->pid);

  // Simulate a new master detected event on the slave,
  // so that the slave will do a re-registration.
  detector.appoint(master.get()->pid);

  // Force evaluation of master detection before we advance clock to trigger
  // agent registration.
  Clock::settle();

  // Capture and inspect the slave reregistration message.
  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(reregisterSlaveMessage);

  ASSERT_EQ(1, reregisterSlaveMessage->tasks_size());

  // The latest state of the task should be TASK_FINISHED.
  ASSERT_EQ(TASK_FINISHED, reregisterSlaveMessage->tasks(0).state());

  // The status update state of the task should be TASK_RUNNING.
  ASSERT_EQ(TASK_RUNNING,
            reregisterSlaveMessage->tasks(0).status_update_state());

  // The status update uuid should match the TASK_RUNNING's uuid.
  ASSERT_EQ(statusUpdateMessage->update().uuid(),
            reregisterSlaveMessage->tasks(0).status_update_uuid());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// The agent's operation status update manager should retry updates for
// operations on agent default resources. Here we drop the first such update and
// verify that the update is sent again after the retry interval elapses.
TEST_F(SlaveTest, UpdateOperationStatusRetry)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.clear_roles();
  frameworkInfo.add_roles("test-role");

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ContentType contentType = ContentType::PROTOBUF;

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  v1::Resources unreserved = offer.resources();
  v1::Resources reserved = unreserved.pushReservation(
      v1::createDynamicReservationInfo(
          frameworkInfo.roles(0),
          frameworkInfo.principal()));

  v1::Offer::Operation reserve = v1::RESERVE(reserved);

  Future<ApplyOperationMessage> applyOperationMessage =
    FUTURE_PROTOBUF(ApplyOperationMessage(), _, slave.get()->pid);

  // Drop the first operation status update.
  Future<UpdateOperationStatusMessage> droppedOperation =
    DROP_PROTOBUF(UpdateOperationStatusMessage(), _, master.get()->pid);

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {reserve}));

  AWAIT_READY(applyOperationMessage);
  UUID operationUuid = applyOperationMessage->operation_uuid();

  AWAIT_READY(droppedOperation);
  ASSERT_EQ(droppedOperation->operation_uuid(), operationUuid);

  // Confirm that the agent retries the update.
  Future<UpdateOperationStatusMessage> updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, master.get()->pid);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(updateOperationStatusMessage);
  ASSERT_EQ(updateOperationStatusMessage->operation_uuid(), operationUuid);
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  Clock::resume();
}


// This test verifies that the slave should properly handle the case
// where the containerizer usage call fails when getting the usage
// information.
TEST_F(SlaveTest, ContainerizerUsageFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));
  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      SLEEP_COMMAND(1000),
      exec.id);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Set up the containerizer so the next usage() will fail.
  EXPECT_CALL(containerizer, usage(_))
    .WillOnce(Return(Failure("Injected failure")));

  // We expect that the slave will still returns ResourceUsage but no
  // statistics will be found.
  Future<ResourceUsage> usage = slave.get()->mock()->usage();

  AWAIT_READY(usage);
  ASSERT_EQ(1, usage->executors_size());
  EXPECT_FALSE(usage->executors(0).has_statistics());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that DiscoveryInfo and Port messages, set in TaskInfo,
// are exposed over the slave state endpoint. The test launches a task with
// the DiscoveryInfo and Port message fields populated. It then makes an HTTP
// request to the state endpoint of the slave and retrieves the JSON data from
// the endpoint. The test passes if the DiscoveryInfo and Port message data in
// JSON matches the corresponding data set in the TaskInfo used to launch the
// task.
TEST_F(SlaveTest, DiscoveryInfoAndPorts)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
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

  TaskInfo task = createTask(
      offers.get()[0],
      SLEEP_COMMAND(100),
      DEFAULT_EXECUTOR_ID);

  Labels labels1;
  labels1.add_labels()->CopyFrom(createLabel("ACTION", "port:7987 DENY"));

  Labels labels2;
  labels2.add_labels()->CopyFrom(createLabel("ACTION", "port:7789 PERMIT"));

  Ports ports;
  Port* port1 = ports.add_ports();
  port1->set_number(80);
  port1->mutable_labels()->CopyFrom(labels1);

  Port* port2 = ports.add_ports();
  port2->set_number(8081);
  port2->mutable_labels()->CopyFrom(labels2);

  DiscoveryInfo discovery;
  discovery.set_name("test_discovery");
  discovery.set_visibility(DiscoveryInfo::CLUSTER);
  discovery.mutable_ports()->CopyFrom(ports);

  task.mutable_discovery()->CopyFrom(discovery);

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(launchTask);

  // Verify label key and value in slave state endpoint.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Object> discoveryResult = parse->find<JSON::Object>(
      "frameworks[0].executors[0].tasks[0].discovery");
  EXPECT_SOME(discoveryResult);

  JSON::Object discoveryObject = discoveryResult.get();
  EXPECT_EQ(JSON::Object(JSON::protobuf(discovery)), discoveryObject);

  // Check the ports are set in the `DiscoveryInfo` object.
  Result<JSON::Object> portResult1 = discoveryObject.find<JSON::Object>(
      "ports.ports[0]");
  Result<JSON::Object> portResult2 = discoveryObject.find<JSON::Object>(
      "ports.ports[1]");

  EXPECT_SOME(portResult1);
  EXPECT_SOME(portResult2);

  // Verify that the ports retrieved from state endpoint are the ones
  // that were set.
  EXPECT_EQ(JSON::Object(JSON::protobuf(*port1)), portResult1.get());
  EXPECT_EQ(JSON::Object(JSON::protobuf(*port2)), portResult2.get());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that executor labels are
// exposed in the slave's state endpoint.
TEST_F(SlaveTest, ExecutorLabels)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
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

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // Add three labels to the executor (two of which share the same key).
  Labels* labels = task.mutable_executor()->mutable_labels();

  labels->add_labels()->CopyFrom(createLabel("key1", "value1"));
  labels->add_labels()->CopyFrom(createLabel("key2", "value2"));
  labels->add_labels()->CopyFrom(createLabel("key1", "value3"));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Verify label key and value in slave state endpoint.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Array> labels_ = parse->find<JSON::Array>(
      "frameworks[0].executors[0].labels");
  ASSERT_SOME(labels_);

  // Verify the contents of labels.
  EXPECT_EQ(3u, labels_->values.size());
  EXPECT_EQ(JSON::Value(JSON::protobuf(createLabel("key1", "value1"))),
            labels_->values[0]);
  EXPECT_EQ(JSON::Value(JSON::protobuf(createLabel("key2", "value2"))),
            labels_->values[1]);
  EXPECT_EQ(JSON::Value(JSON::protobuf(createLabel("key1", "value3"))),
            labels_->values[2]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that label values can be set for tasks and that
// they are exposed over the slave state endpoint.
TEST_F(SlaveTest, TaskLabels)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
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

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // Add three labels to the task (two of which share the same key).
  Labels* labels = task.mutable_labels();

  labels->add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels->add_labels()->CopyFrom(createLabel("bar", "baz"));
  labels->add_labels()->CopyFrom(createLabel("bar", "qux"));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources()), _))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Nothing())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(update);

  // Verify label key and value in slave state endpoint.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Array> find = parse->find<JSON::Array>(
      "frameworks[0].executors[0].tasks[0].labels");
  EXPECT_SOME(find);

  JSON::Array labelsObject = find.get();

  // Verify the contents of 'foo:bar', 'bar:baz', and 'bar:qux' pairs.
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("foo", "bar"))),
      labelsObject.values[0]);
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "baz"))),
      labelsObject.values[1]);
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "qux"))),
      labelsObject.values[2]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that TaskStatus label values are exposed over
// the slave state endpoint.
TEST_F(SlaveTest, TaskStatusLabels)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
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

  TaskInfo task = createTask(
      offers.get()[0],
      SLEEP_COMMAND(100),
      DEFAULT_EXECUTOR_ID);

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  Future<TaskInfo> execTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&execTask));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(execTask);

  // Now send TASK_RUNNING update.
  TaskStatus runningStatus;
  runningStatus.mutable_task_id()->MergeFrom(execTask->task_id());
  runningStatus.set_state(TASK_RUNNING);

  // Add three labels to the task (two of which share the same key).
  Labels* labels = runningStatus.mutable_labels();

  labels->add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels->add_labels()->CopyFrom(createLabel("bar", "baz"));
  labels->add_labels()->CopyFrom(createLabel("bar", "qux"));

  execDriver->sendStatusUpdate(runningStatus);

  AWAIT_READY(status);

  // Verify label key and value in master state endpoint.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Array> find = parse->find<JSON::Array>(
      "frameworks[0].executors[0].tasks[0].statuses[0].labels");
  EXPECT_SOME(find);

  JSON::Array labelsObject = find.get();

  // Verify the contents of 'foo:bar', 'bar:baz', and 'bar:qux' pairs.
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("foo", "bar"))),
      labelsObject.values[0]);
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "baz"))),
      labelsObject.values[1]);
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "qux"))),
      labelsObject.values[2]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that TaskStatus::container_status an is exposed over
// the slave state endpoint.
TEST_F(SlaveTest, TaskStatusContainerStatus)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
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

  TaskInfo task = createTask(
      offers.get()[0],
      SLEEP_COMMAND(100),
      DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);

  const string slaveIPAddress = stringify(slave.get()->pid.address.ip);

  // Validate that the Slave has passed in its IP address in
  // TaskStatus.container_status.network_infos[0].ip_address.
  EXPECT_TRUE(status->has_container_status());
  EXPECT_EQ(1, status->container_status().network_infos().size());
  EXPECT_EQ(1, status->container_status().network_infos(0).ip_addresses().size()); // NOLINT(whitespace/line_length)

  NetworkInfo::IPAddress ipAddress =
    status->container_status().network_infos(0).ip_addresses(0);

  ASSERT_TRUE(ipAddress.has_ip_address());
  EXPECT_EQ(slaveIPAddress, ipAddress.ip_address());

  // Now do the same validation with state endpoint.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  // Validate that the IP address passed in by the Slave is available at the
  // state endpoint.
  ASSERT_SOME_EQ(
      slaveIPAddress,
      parse->find<JSON::String>(
          "frameworks[0].executors[0].tasks[0].statuses[0]"
          ".container_status.network_infos[0]"
          ".ip_addresses[0].ip_address"));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Test that we can set the executors environment variables and it
// won't inherit the slaves.
TEST_F(SlaveTest, ExecutorEnvironmentVariables)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_environment_variables'.
  slave::Flags flags = CreateSlaveFlags();

  const std::string path = os::host_default_path();

  flags.executor_environment_variables = JSON::Object{{"PATH", path}};

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
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

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  // Command executor will run as user running test.
  CommandInfo command;
#ifdef __WINDOWS__
  command.set_shell(false);
  command.set_value("powershell.exe");
  command.add_arguments("powershell.exe");
  command.add_arguments("-NoProfile");
  command.add_arguments("-Command");
  command.add_arguments(
      "if ($env:PATH -eq '" + path + "') { exit 0 } else { exit 1 }");
#else
  command.set_shell(true);
  command.set_value("test $PATH = " + path);
#endif // __WINDOWS__

  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Scheduler should first receive TASK_STARTING, followed by
  // TASK_STARTING and TASK_FINISHED from the executor.
  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that the slave should properly show total slave
// resources.
TEST_F(SlaveTest, TotalSlaveResourcesIncludedInUsage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  TestContainerizer containerizer;
  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:2;gpus:0;mem:1024;disk:1024;ports:[31000-32000]";

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      flags,
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  Clock::pause();

  // Wait for slave to be initialized.
  Clock::settle();

  // We expect that the slave will return ResourceUsage with
  // total resources reported.
  Future<ResourceUsage> usage = slave.get()->mock()->usage();

  AWAIT_READY(usage);

  // Total resources should match the resources from flag.resources.
  EXPECT_EQ(Resources(usage->total()),
            Resources::parse(flags.resources.get()).get());
}


// This test verifies that the slave should properly show total slave
// resources with checkpointed resources applied.
TEST_F(SlaveTest, CheckpointedResourcesIncludedInUsage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  TestContainerizer containerizer;
  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:2;cpus(role1):3;mem:1024;disk:1024;disk(role1):64;"
                    "ports:[31000-32000]";

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      flags,
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  Clock::pause();

  // Wait for slave to be initialized.
  Clock::settle();

  Resource dynamicReservation = createReservedResource(
      "cpus", "1", createDynamicReservationInfo("role1", "principal"));

  Resource persistentVolume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  vector<Resource> checkpointedResources =
    {dynamicReservation, persistentVolume};

  // Add checkpointed resources.
  slave.get()->mock()->checkpointResourceState(checkpointedResources, true);

  // We expect that the slave will return ResourceUsage with
  // total and checkpointed slave resources reported.
  Future<ResourceUsage> usage = slave.get()->mock()->usage();

  AWAIT_READY(usage);

  Resources usageTotalResources(usage->total());

  // Reported total field should contain persistent volumes and dynamic
  // reservations.
  EXPECT_EQ(usageTotalResources.persistentVolumes(), persistentVolume);
  EXPECT_TRUE(usageTotalResources.contains(dynamicReservation));
}


// Ensures that the slave correctly handles a framework without
// a pid, which will be the case for HTTP schedulers. In
// particular, executor messages should be routed through the
// master.
TEST_F(SlaveTest, HTTPScheduler)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 1024, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Capture the run task message to unset the framework pid.
  Future<RunTaskMessage> runTaskMessage =
    DROP_PROTOBUF(RunTaskMessage(), master.get()->pid, slave.get()->pid);

  driver.start();

  AWAIT_READY(runTaskMessage);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendFrameworkMessage("message"));

  // The slave should forward the message through the master.
  Future<ExecutorToFrameworkMessage> executorToFrameworkMessage1 =
    FUTURE_PROTOBUF(
        ExecutorToFrameworkMessage(),
        slave.get()->pid,
        master.get()->pid);

  // The master should then forward the message to the framework.
  Future<ExecutorToFrameworkMessage> executorToFrameworkMessage2 =
    FUTURE_PROTOBUF(ExecutorToFrameworkMessage(), master.get()->pid, _);

  Future<Nothing> frameworkMessage;
  EXPECT_CALL(sched, frameworkMessage(&driver, _, _, "message"))
    .WillOnce(FutureSatisfy(&frameworkMessage));

  // Clear the pid in the run task message so that the slave
  // thinks this is an HTTP scheduler.
  RunTaskMessage spoofed = runTaskMessage.get();
  spoofed.set_pid("");

  process::post(master.get()->pid, slave.get()->pid, spoofed);

  AWAIT_READY(executorToFrameworkMessage1);
  AWAIT_READY(executorToFrameworkMessage2);

  AWAIT_READY(frameworkMessage);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Ensures that the slave correctly handles a framework upgrading
// to HTTP (going from having a pid, to not having a pid). In
// particular, executor messages should be routed through the
// master.
TEST_F(SlaveTest, HTTPSchedulerLiveUpgrade)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 2, 1024, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver.start();

  AWAIT_READY(frameworkId);
  AWAIT_READY(launchTask);

  // Set the `FrameworkID` in `FrameworkInfo`.
  frameworkInfo.mutable_id()->CopyFrom(frameworkId.get());

  // Now spoof a live upgrade of the framework by updating
  // the framework information to have an empty pid.
  UpdateFrameworkMessage updateFrameworkMessage;
  updateFrameworkMessage.mutable_framework_id()->CopyFrom(frameworkId.get());
  updateFrameworkMessage.set_pid("");
  updateFrameworkMessage.mutable_framework_info()->CopyFrom(frameworkInfo);

  process::post(master.get()->pid, slave.get()->pid, updateFrameworkMessage);

  // Send a message from the executor; the slave should forward
  // the message through the master.
  Future<ExecutorToFrameworkMessage> executorToFrameworkMessage1 =
    FUTURE_PROTOBUF(
        ExecutorToFrameworkMessage(),
        slave.get()->pid,
        master.get()->pid);

  Future<ExecutorToFrameworkMessage> executorToFrameworkMessage2 =
    FUTURE_PROTOBUF(ExecutorToFrameworkMessage(), master.get()->pid, _);

  Future<Nothing> frameworkMessage;
  EXPECT_CALL(sched, frameworkMessage(&driver, _, _, "message"))
    .WillOnce(FutureSatisfy(&frameworkMessage));

  execDriver->sendFrameworkMessage("message");

  AWAIT_READY(executorToFrameworkMessage1);
  AWAIT_READY(executorToFrameworkMessage2);

  AWAIT_READY(frameworkMessage);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Ensures that the slave can restart when there is an empty
// framework pid. Executor messages should go through the
// master (instead of directly to the scheduler!).
TEST_F(SlaveTest, HTTPSchedulerSlaveRestart)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    this->StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());      // Ignore subsequent offers.

  driver.start();

  // Capture the executor information.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  SlaveID slaveId = offers.get()[0].slave_id();

  // Capture the run task so that we can unset the framework pid.
  Future<RunTaskMessage> runTaskMessage =
    DROP_PROTOBUF(RunTaskMessage(), master.get()->pid, slave.get()->pid);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(1000));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(runTaskMessage);

  // Clear the pid in the run task message so that the slave
  // thinks this is an HTTP scheduler.
  RunTaskMessage spoofedRunTaskMessage = runTaskMessage.get();
  spoofedRunTaskMessage.set_pid("");

  process::post(master.get()->pid, slave.get()->pid, spoofedRunTaskMessage);

  AWAIT_READY(registerExecutorMessage);

  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage->body);
  ExecutorID executorId = registerExecutor.executor_id();
  UPID executorPid = registerExecutorMessage->from;

  AWAIT_READY(status);
  EXPECT_EQ(TASK_STARTING, status->state());

  // Restart the slave.
  slave.get()->terminate();

  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
     FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Capture this so that we can unset the framework pid.
  Future<UpdateFrameworkMessage> updateFrameworkMessage =
     DROP_PROTOBUF(UpdateFrameworkMessage(), _, _);

  // Ensure that there will be no reregistration retries from the
  // slave resulting in another UpdateFrameworkMessage from master.
  Clock::pause();

  slave = StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  // Let the executor reregister.
  AWAIT_READY(reregisterExecutorMessage);

  // Ensure the slave considers itself recovered and reregisters.
  Clock::settle();
  Clock::advance(flags.executor_reregistration_timeout);

  Clock::settle();
  Clock::advance(flags.registration_backoff_factor);

  AWAIT_READY(slaveReregisteredMessage);
  AWAIT_READY(updateFrameworkMessage);

  // Make sure the slave sees an empty framework pid after recovery.
  UpdateFrameworkMessage spoofedUpdateFrameworkMessage =
    updateFrameworkMessage.get();
  spoofedUpdateFrameworkMessage.set_pid("");

  process::post(
      master.get()->pid,
      slave.get()->pid,
      spoofedUpdateFrameworkMessage);

  // Spoof a message from the executor, to ensure the slave
  // sends it through the master (instead of directly to the
  // scheduler driver!).
  Future<ExecutorToFrameworkMessage> executorToFrameworkMessage1 =
    FUTURE_PROTOBUF(
        ExecutorToFrameworkMessage(),
        slave.get()->pid,
        master.get()->pid);

  Future<ExecutorToFrameworkMessage> executorToFrameworkMessage2 =
    FUTURE_PROTOBUF(ExecutorToFrameworkMessage(), master.get()->pid, _);

  Future<Nothing> frameworkMessage;
  EXPECT_CALL(sched, frameworkMessage(&driver, _, _, "message"))
    .WillOnce(FutureSatisfy(&frameworkMessage));

  ExecutorToFrameworkMessage executorToFrameworkMessage;
  executorToFrameworkMessage.mutable_slave_id()->CopyFrom(slaveId);
  executorToFrameworkMessage.mutable_framework_id()->CopyFrom(frameworkId);
  executorToFrameworkMessage.mutable_executor_id()->CopyFrom(executorId);
  executorToFrameworkMessage.set_data("message");

  process::post(executorPid, slave.get()->pid, executorToFrameworkMessage);

  AWAIT_READY(executorToFrameworkMessage1);
  AWAIT_READY(executorToFrameworkMessage2);
  AWAIT_READY(frameworkMessage);

  driver.stop();
  driver.join();

  // We must resume the clock to ensure the agent can reap the
  // executor after we destroy it.
  Clock::resume();
}


// Ensures that if `ExecutorInfo.shutdown_grace_period` is set, it
// overrides the default value from the agent flag, is observed by
// executor, and is enforced by the agent.
TEST_F(SlaveTest, ExecutorShutdownGracePeriod)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, agentFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  // We need framework's ID to shutdown the executor later on.
  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  // Customize executor shutdown grace period to be larger than the
  // default agent flag value, so that we can check it is respected.
  Duration customGracePeriod = agentFlags.executor_shutdown_grace_period * 2;

  ExecutorInfo executorInfo(DEFAULT_EXECUTOR_INFO);
  executorInfo.mutable_shutdown_grace_period()->set_nanoseconds(
      customGracePeriod.ns());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("2");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(executorInfo);

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<TaskInfo> receivedTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&receivedTask)));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  EXPECT_EQ(customGracePeriod.ns(),
            receivedTask->executor().shutdown_grace_period().nanoseconds());

  // If executor is asked to shutdown but fails to do so within the grace
  // shutdown period, the shutdown is enforced by the agent. The agent
  // adjusts its timeout according to `ExecutorInfo.shutdown_grace_period`.
  //
  // NOTE: Executors relying on the executor driver have a built-in suicide
  // mechanism (`ShutdownProcess`), that kills the OS process where the
  // executor is running after the grace period ends. This mechanism is
  // disabled in tests, hence we do not observe crashes induced by this test.
  // The test containerizer only accepts "local" executors and it considers
  // them "terminated" only once destroy is called.

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1))
    .WillOnce(Return());

  // Once the grace period ends, the agent forcibly shuts down the executor.
  Future<Nothing> executorShutdownTimeout =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::shutdownExecutorTimeout);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ExecutorID> lostExecutorId;
  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _))
    .WillOnce(FutureArg<1>(&lostExecutorId));

  // Ask executor to shutdown. There is no support in the scheduler
  // driver for shutting down executors, hence we have to spoof it.
  AWAIT_READY(frameworkId);
  ShutdownExecutorMessage shutdownMessage;
  shutdownMessage.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  shutdownMessage.mutable_framework_id()->CopyFrom(frameworkId.get());
  post(master.get()->pid, slave.get()->pid, shutdownMessage);

  // Ensure the `ShutdownExecutorMessage` message is
  // received by the agent before we start the timer.
  Clock::pause();
  Clock::settle();
  Clock::advance(agentFlags.executor_shutdown_grace_period);
  Clock::settle();

  // The executor shutdown timeout should not have fired, since the
  // `ExecutorInfo` contains a grace period larger than the agent flag.
  EXPECT_TRUE(executorShutdownTimeout.isPending());

  // Trigger the shutdown grace period from the `ExecutorInfo`
  // (note that is is 2x the agent flag).
  Clock::advance(agentFlags.executor_shutdown_grace_period);

  AWAIT_READY(executorShutdownTimeout);

  AWAIT_READY(statusFailed);
  EXPECT_EQ(TASK_FAILED, statusFailed->state());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_TERMINATED,
            statusFailed->reason());

  AWAIT_EXPECT_EQ(DEFAULT_EXECUTOR_ID, lostExecutorId);

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test verifies that the agent can forward a task group to an
// executor atomically via the `LAUNCH_GROUP` event.
TEST_F(SlaveTest, RunTaskGroup)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
  executorInfo.set_type(ExecutorInfo::CUSTOM);

  executorInfo.mutable_resources()->CopyFrom(resources);

  const ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _));

  EXPECT_CALL(*executor, launch(_, _))
    .Times(0);

  Future<v1::executor::Event::LaunchGroup> launchGroupEvent;
  EXPECT_CALL(*executor, launchGroup(_, _))
    .WillOnce(FutureArg<1>(&launchGroupEvent));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  v1::TaskInfo taskInfo1 =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskInfo taskInfo2 =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(launchGroupEvent);

  ASSERT_EQ(2, launchGroupEvent->task_group().tasks().size());

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  const hashset<v1::TaskID> launchedTasks{
      launchGroupEvent->task_group().tasks(0).task_id(),
      launchGroupEvent->task_group().tasks(1).task_id()};

  EXPECT_EQ(tasks, launchedTasks);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));
}


// This test verifies that TASK_FAILED updates are sent correctly for all the
// tasks in a task group when secret generation fails.
TEST_F(SlaveTest, RunTaskGroupFailedSecretGeneration)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::DEFAULT_EXECUTOR_INFO;
  executorInfo.set_type(v1::ExecutorInfo::CUSTOM);

  executorInfo.mutable_resources()->CopyFrom(resources);

  const v1::ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(devolve(executorId), executor);

  StandaloneMasterDetector detector(master.get()->pid);

  Owned<MockSecretGenerator> secretGenerator(new MockSecretGenerator());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      secretGenerator.get(),
      None(),
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "");

  v1::TaskInfo taskInfo2 = v1::createTask(agentId, resources, "");

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  // The tasks will fail to launch because the executor secret generation fails.
  const string failureMessage = "Mock secret generator failed";
  EXPECT_CALL(*secretGenerator, generate(_))
    .WillOnce(Return(Failure(failureMessage)));

  EXPECT_CALL(*executor, connected(_))
    .Times(0);

  EXPECT_CALL(*executor, subscribed(_, _))
    .Times(0);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(0);

  EXPECT_CALL(*executor, launchGroup(_, _))
    .Times(0);

  EXPECT_CALL(*executor, launch(_, _))
    .Times(0);

  EXPECT_CALL(*slave.get()->mock(), executorTerminated(_, _, _))
    .WillOnce(Invoke(slave.get()->mock(),
                     &MockSlave::unmocked_executorTerminated));

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

  Future<Nothing> failure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureSatisfy(&failure));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  AWAIT_READY(failure);

  const hashset<v1::TaskID> failedTasks{
      update1->status().task_id(), update2->status().task_id()};

  ASSERT_EQ(v1::TASK_FAILED, update1->status().state());
  ASSERT_EQ(v1::TASK_FAILED, update2->status().state());

  EXPECT_TRUE(strings::contains(update1->status().message(), failureMessage));
  EXPECT_TRUE(strings::contains(update2->status().message(), failureMessage));

  ASSERT_EQ(tasks, failedTasks);

  // Since this is the only task group for this framework, the
  // framework should be removed after secret generation fails.
  Future<Nothing> removeFramework;
  EXPECT_CALL(*slave.get()->mock(), removeFramework(_))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  // Acknowledge the status updates so that the agent will remove the framework.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update1->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update1->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update2->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update2->status().uuid());

    mesos.send(call);
  }

  AWAIT_READY(removeFramework);
}


// This test verifies that TASK_FAILED updates are sent correctly for all the
// tasks in a task group when the secret generator returns an invalid secret.
TEST_F(SlaveTest, RunTaskGroupInvalidExecutorSecret)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::DEFAULT_EXECUTOR_INFO;
  executorInfo.set_type(v1::ExecutorInfo::CUSTOM);

  executorInfo.mutable_resources()->CopyFrom(resources);

  const v1::ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(devolve(executorId), executor);

  StandaloneMasterDetector detector(master.get()->pid);

  Owned<MockSecretGenerator> secretGenerator(new MockSecretGenerator());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      secretGenerator.get(),
      None(),
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "");

  v1::TaskInfo taskInfo2 = v1::createTask(agentId, resources, "");

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  // The tasks will fail to launch because the executor secret is invalid
  // (VALUE type secrets must not have the `reference` member set).
  Secret authenticationToken;
  authenticationToken.set_type(Secret::VALUE);
  authenticationToken.mutable_reference()->set_name("secret_name");
  authenticationToken.mutable_reference()->set_key("secret_key");

  EXPECT_CALL(*secretGenerator, generate(_))
    .WillOnce(Return(authenticationToken));

  EXPECT_CALL(*executor, connected(_))
    .Times(0);

  EXPECT_CALL(*executor, subscribed(_, _))
    .Times(0);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(0);

  EXPECT_CALL(*executor, launchGroup(_, _))
    .Times(0);

  EXPECT_CALL(*executor, launch(_, _))
    .Times(0);

  EXPECT_CALL(*slave.get()->mock(), executorTerminated(_, _, _))
    .WillOnce(Invoke(slave.get()->mock(),
                     &MockSlave::unmocked_executorTerminated));

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

  Future<Nothing> failure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureSatisfy(&failure));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  AWAIT_READY(failure);

  const hashset<v1::TaskID> failedTasks{
      update1->status().task_id(), update2->status().task_id()};

  ASSERT_EQ(v1::TASK_FAILED, update1->status().state());
  ASSERT_EQ(v1::TASK_FAILED, update2->status().state());

  const string failureMessage =
    "Secret of type VALUE must have the 'value' field set";

  EXPECT_TRUE(strings::contains(update1->status().message(), failureMessage));
  EXPECT_TRUE(strings::contains(update2->status().message(), failureMessage));

  ASSERT_EQ(tasks, failedTasks);

  // Since this is the only task group for this framework, the
  // framework should be removed after secret generation fails.
  Future<Nothing> removeFramework;
  EXPECT_CALL(*slave.get()->mock(), removeFramework(_))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  // Acknowledge the status updates so that the agent will remove the framework.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update1->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update1->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update2->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update2->status().uuid());

    mesos.send(call);
  }

  AWAIT_READY(removeFramework);
}


// This test verifies that TASK_FAILED updates are sent correctly for all the
// tasks in a task group when the secret generator returns a REFERENCE type
// secret. Only VALUE type secrets are supported at this time.
TEST_F(SlaveTest, RunTaskGroupReferenceTypeSecret)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::DEFAULT_EXECUTOR_INFO;
  executorInfo.set_type(v1::ExecutorInfo::CUSTOM);

  executorInfo.mutable_resources()->CopyFrom(resources);

  const v1::ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(devolve(executorId), executor);

  StandaloneMasterDetector detector(master.get()->pid);

  Owned<MockSecretGenerator> secretGenerator(new MockSecretGenerator());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      secretGenerator.get(),
      None(),
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "");

  v1::TaskInfo taskInfo2 = v1::createTask(agentId, resources, "");

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  // The tasks will fail to launch because the executor secret is invalid
  // (only VALUE type secrets are supported at this time).
  Secret authenticationToken;
  authenticationToken.set_type(Secret::REFERENCE);
  authenticationToken.mutable_reference()->set_name("secret_name");
  authenticationToken.mutable_reference()->set_key("secret_key");

  EXPECT_CALL(*secretGenerator, generate(_))
    .WillOnce(Return(authenticationToken));

  EXPECT_CALL(*executor, connected(_))
    .Times(0);

  EXPECT_CALL(*executor, subscribed(_, _))
    .Times(0);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(0);

  EXPECT_CALL(*executor, launchGroup(_, _))
    .Times(0);

  EXPECT_CALL(*executor, launch(_, _))
    .Times(0);

  EXPECT_CALL(*slave.get()->mock(), executorTerminated(_, _, _))
    .WillOnce(Invoke(slave.get()->mock(),
                     &MockSlave::unmocked_executorTerminated));

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

  Future<Nothing> failure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureSatisfy(&failure));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  AWAIT_READY(failure);

  const hashset<v1::TaskID> failedTasks{
      update1->status().task_id(), update2->status().task_id()};

  ASSERT_EQ(v1::TASK_FAILED, update1->status().state());
  ASSERT_EQ(v1::TASK_FAILED, update2->status().state());

  const string failureMessage =
    "Expecting generated secret to be of VALUE type instead of REFERENCE type";

  EXPECT_TRUE(strings::contains(update1->status().message(), failureMessage));
  EXPECT_TRUE(strings::contains(update2->status().message(), failureMessage));

  ASSERT_EQ(tasks, failedTasks);

  // Since this is the only task group for this framework, the
  // framework should be removed after secret generation fails.
  Future<Nothing> removeFramework;
  EXPECT_CALL(*slave.get()->mock(), removeFramework(_))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  // Acknowledge the status updates so that the agent will remove the framework.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update1->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update1->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update2->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update2->status().uuid());

    mesos.send(call);
  }

  AWAIT_READY(removeFramework);
}


// This test verifies that TASK_FAILED updates and an executor FAILURE message
// are sent correctly when the secret generator returns the executor secret
// after the scheduler has shutdown the executor.
TEST_F(SlaveTest, RunTaskGroupGenerateSecretAfterShutdown)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::DEFAULT_EXECUTOR_INFO;
  executorInfo.set_type(v1::ExecutorInfo::CUSTOM);

  executorInfo.mutable_resources()->CopyFrom(resources);

  const v1::ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(devolve(executorId), executor);

  StandaloneMasterDetector detector(master.get()->pid);

  Owned<MockSecretGenerator> secretGenerator(new MockSecretGenerator());

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      secretGenerator.get(),
      None(),
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo taskInfo1 = v1::createTask(agentId, resources, "");

  v1::TaskInfo taskInfo2 = v1::createTask(agentId, resources, "");

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  // We return this promise's future so that we can delay its fulfillment
  // until after the scheduler has shutdown the executor.
  Promise<Secret> secret;
  Future<Nothing> generate;
  EXPECT_CALL(*secretGenerator, generate(_))
    .WillOnce(DoAll(FutureSatisfy(&generate),
                    Return(secret.future())));

  EXPECT_CALL(*executor, connected(_))
    .Times(0);

  EXPECT_CALL(*executor, subscribed(_, _))
    .Times(0);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(0);

  EXPECT_CALL(*executor, launchGroup(_, _))
    .Times(0);

  EXPECT_CALL(*executor, launch(_, _))
    .Times(0);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(generate);

  Future<Nothing> shutdownExecutor;
  EXPECT_CALL(*slave.get()->mock(), shutdownExecutor(_, _, _))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_shutdownExecutor),
                    FutureSatisfy(&shutdownExecutor)));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::SHUTDOWN);

    Call::Shutdown* shutdown = call.mutable_shutdown();
    shutdown->mutable_executor_id()->CopyFrom(executorId);
    shutdown->mutable_agent_id()->CopyFrom(offer.agent_id());

    mesos.send(call);
  }

  AWAIT_READY(shutdownExecutor);

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

  Future<Nothing> failure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureSatisfy(&failure));

  EXPECT_CALL(*slave.get()->mock(), executorTerminated(_, _, _))
    .WillOnce(Invoke(slave.get()->mock(),
                     &MockSlave::unmocked_executorTerminated));

  // The tasks will fail to launch because the executor has been shutdown.
  Secret authenticationToken;
  authenticationToken.set_type(Secret::VALUE);
  authenticationToken.mutable_value()->set_data("secret_data");
  secret.set(authenticationToken);

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  AWAIT_READY(failure);

  const hashset<v1::TaskID> failedTasks{
      update1->status().task_id(), update2->status().task_id()};

  ASSERT_EQ(v1::TASK_FAILED, update1->status().state());
  ASSERT_EQ(v1::TASK_FAILED, update2->status().state());

  const string failureMessage = "Executor terminating";

  EXPECT_TRUE(strings::contains(update1->status().message(), failureMessage));
  EXPECT_TRUE(strings::contains(update2->status().message(), failureMessage));

  ASSERT_EQ(tasks, failedTasks);

  // Since this is the only task group for this framework, the
  // framework should be removed after secret generation fails.
  Future<Nothing> removeFramework;
  EXPECT_CALL(*slave.get()->mock(), removeFramework(_))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  // Acknowledge the status updates so that the agent will remove the framework.

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update1->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update1->status().uuid());

    mesos.send(call);
  }

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update2->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update2->status().uuid());

    mesos.send(call);
  }

  AWAIT_READY(removeFramework);
}


#ifdef USE_SSL_SOCKET
// This test verifies that a default executor which is launched when secret
// generation is enabled and HTTP executor authentication is not required will
// be able to re-subscribe successfully when the agent is restarted with
// required HTTP executor authentication.
//
TEST_F(SlaveTest, RestartSlaveRequireExecutorAuthentication)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_executors = false;
  flags.authenticate_http_readwrite = false;

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start the agent with a static process ID. This allows the executor to
  // reconnect with the agent upon a process restart.
  const string id("agent");

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), id, flags);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Future<v1::scheduler::Event::Update> updateStarting;
  Future<v1::scheduler::Event::Update> updateRunning;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(
      DoAll(
          FutureArg<1>(&updateStarting),
          v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(
      DoAll(
          FutureArg<1>(&updateRunning),
          v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  // Create a task which should run indefinitely.
  const string command =
#ifdef __WINDOWS__
    "more";
#else
    "cat";
#endif // __WINDOWS__
  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, command);

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  v1::ExecutorInfo executorInfo = v1::DEFAULT_EXECUTOR_INFO;
  executorInfo.clear_command();
  executorInfo.mutable_framework_id()->CopyFrom(subscribed->framework_id());
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_resources()->CopyFrom(resources);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(updateStarting);

  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);

  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  // Restart the agent.
  slave.get()->terminate();

  // Enable authentication.
  flags.authenticate_http_executors = true;
  flags.authenticate_http_readwrite = true;

  // Confirm that the executor does not fail.
  EXPECT_CALL(*scheduler, failure(_, _))
    .Times(0);

  Future<Nothing> __recover =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::__recover);

  slave = StartSlave(detector.get(), id, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  Future<Response> response = process::http::get(
      slave.get()->pid,
      "containers",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Value> value = JSON::parse(response->body);
  ASSERT_SOME(value);

  Try<JSON::Value> expected = JSON::parse(
      "[{"
          "\"executor_id\":\"" + stringify(executorInfo.executor_id()) + "\""
      "}]");

  ASSERT_SOME(expected);
  EXPECT_TRUE(value->contains(expected.get()));

  // Settle the clock to ensure that an executor failure would be detected.
  Clock::pause();
  Clock::settle();
  Clock::resume();
}
#endif // USE_SSL_SOCKET


// This test ensures that a `killTask()` can happen between `runTask()`
// and `_run()` and then gets "handled properly" for a task group.
// This should result in TASK_KILLED updates for all the tasks in the
// task group.
TEST_F(SlaveTest, KillTaskGroupBetweenRunTaskParts)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
  executorInfo.set_type(ExecutorInfo::CUSTOM);

  executorInfo.mutable_resources()->CopyFrom(resources);

  const ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(executorId, executor);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &containerizer,
      None(),
      true);

  ASSERT_SOME(slave);
  ASSERT_NE(nullptr, slave.get()->mock());

  slave.get()->start();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  EXPECT_CALL(*scheduler, failure(_, _))
    .Times(AtMost(1));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  EXPECT_CALL(*executor, connected(_))
    .Times(0);

  EXPECT_CALL(*executor, subscribed(_, _))
    .Times(0);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(0);

  EXPECT_CALL(*executor, launchGroup(_, _))
    .Times(0);

  EXPECT_CALL(*executor, launch(_, _))
    .Times(0);

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2))
    .WillRepeatedly(Return());

  EXPECT_CALL(*slave.get()->mock(), runTaskGroup(_, _, _, _, _, _))
    .WillOnce(Invoke(slave.get()->mock(),
                     &MockSlave::unmocked_runTaskGroup));

  // Saved arguments from `Slave::_run()`.
  FrameworkInfo frameworkInfo;
  ExecutorInfo executorInfo_;
  Option<TaskGroupInfo> taskGroup_;
  Option<TaskInfo> task_;
  vector<ResourceVersionUUID> resourceVersionUuids;
  Option<bool> launchExecutor;

  // Skip what `Slave::_run()` normally does, save its arguments for
  // later, return a pending future to pause the original continuation,
  // till reaching the critical moment when to kill the task in the future.
  Promise<Nothing> promise;
  Future<Nothing> _run;
  EXPECT_CALL(*slave.get()->mock(), _run(_, _, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&_run),
                    SaveArg<0>(&frameworkInfo),
                    SaveArg<1>(&executorInfo_),
                    SaveArg<2>(&task_),
                    SaveArg<3>(&taskGroup_),
                    SaveArg<4>(&resourceVersionUuids),
                    SaveArg<5>(&launchExecutor),
                    Return(promise.future())));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  v1::TaskInfo taskInfo1 =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskInfo taskInfo2 =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo1);
  taskGroup.add_tasks()->CopyFrom(taskInfo2);

  const hashset<v1::TaskID> tasks{taskInfo1.task_id(), taskInfo2.task_id()};

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(_run);

  Future<Nothing> killTask;
  EXPECT_CALL(*slave.get()->mock(), killTask(_, _))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask)));

  // Since this is the only task group for this framework, the
  // framework should get removed when the task is killed.
  Future<Nothing> removeFramework;
  EXPECT_CALL(*slave.get()->mock(), removeFramework(_))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  mesos.send(
      v1::createCallKill(frameworkId, taskInfo1.task_id(), offer.agent_id()));

  AWAIT_READY(killTask);

  AWAIT_READY(removeFramework);

  Future<Nothing> unmocked__run = process::dispatch(slave.get()->pid, [=] {
    return slave.get()->mock()->unmocked__run(
        frameworkInfo,
        executorInfo_,
        task_,
        taskGroup_,
        resourceVersionUuids,
        launchExecutor);
  });

  // Resume the original continuation once `unmocked__run` is complete.
  promise.associate(unmocked__run);

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  AWAIT(unmocked__run);

  const hashset<v1::TaskID> killedTasks{
    update1->status().task_id(), update2->status().task_id()};

  EXPECT_EQ(v1::TASK_KILLED, update1->status().state());
  EXPECT_EQ(v1::TASK_KILLED, update2->status().state());
  EXPECT_EQ(tasks, killedTasks);
}


// This test verifies that the agent correctly populates the
// command info for default executor.
//
// TODO(andschwa): Enable when user impersonation works on Windows.
TEST_F_TEMP_DISABLED_ON_WINDOWS(SlaveTest, DefaultExecutorCommandInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  const ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(evolve(frameworkInfo));

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<1>(&containerConfig),
                    Return(Future<Containerizer::LaunchResult>())));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  v1::TaskInfo taskInfo =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(containerConfig);

  // TODO(anand): Add a `strings::contains()` check to ensure
  // `MESOS_DEFAULT_EXECUTOR` is present in the command when
  // we add the executable for default executor.
  ASSERT_TRUE(containerConfig->has_executor_info());
  ASSERT_TRUE(containerConfig->executor_info().has_command());
  EXPECT_EQ(
      frameworkInfo.user(),
      containerConfig->executor_info().command().user());
}


// This test verifies that the agent correctly populates the resources
// for default executor. This is a regression test for MESOS-9925.
TEST_F(SlaveTest, DefaultExecutorResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  ExecutorInfo executorInfo;
  executorInfo.set_type(ExecutorInfo::DEFAULT);

  executorInfo.mutable_executor_id()->CopyFrom(DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_resources()->CopyFrom(resources);

  const ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(evolve(frameworkInfo));

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<1>(&containerConfig),
                    Return(Future<Containerizer::LaunchResult>())));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  v1::TaskInfo taskInfo =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(containerConfig);

  Resources containerResources = Resources(containerConfig->resources());
  containerResources.unallocate();

  // The resources used to launch executor container should include
  // both executorInfo's resources and all task's resources.
  EXPECT_EQ(containerResources, resources + resources);
}


// This test ensures that we do not send a queued task group to
// the executor if any of its tasks are killed before the executor
// subscribes with the agent.
TEST_F(SlaveTest, KillQueuedTaskGroup)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  Resources resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
  executorInfo.set_type(ExecutorInfo::CUSTOM);

  executorInfo.mutable_resources()->CopyFrom(resources);

  const ExecutorID& executorId = executorInfo.executor_id();
  TestContainerizer containerizer(executorId, executor);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(devolve(frameworkId));

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  Future<v1::executor::Mesos*> executorLibrary;
  EXPECT_CALL(*executor, connected(_))
    .WillOnce(FutureArg<0>(&executorLibrary));

  const v1::Offer& offer = offers->offers(0);
  const SlaveID slaveId = devolve(offer.agent_id());

  // Launch a task and task group.
  v1::TaskInfo taskInfo1 =
    evolve(createTask(slaveId, resources, "", executorId));

  taskInfo1.mutable_executor()->CopyFrom(evolve(executorInfo));

  v1::TaskInfo taskInfo2 =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskInfo taskInfo3 =
    evolve(createTask(slaveId, resources, ""));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo2);
  taskGroup.add_tasks()->CopyFrom(taskInfo3);

  const hashset<v1::TaskID> tasks{taskInfo2.task_id(), taskInfo3.task_id()};

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation1 = accept->add_operations();
    operation1->set_type(v1::Offer::Operation::LAUNCH);
    operation1->mutable_launch()->add_task_infos()->CopyFrom(taskInfo1);

    v1::Offer::Operation* operation2 = accept->add_operations();
    operation2->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation2->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(evolve(executorInfo));
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(executorLibrary);

  Future<v1::scheduler::Event::Update> update1;
  Future<v1::scheduler::Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2))
    .WillRepeatedly(Return());

  // Kill a task in the task group before the executor
  // subscribes with the agent.
  mesos.send(
      v1::createCallKill(frameworkId, taskInfo2.task_id(), offer.agent_id()));

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  const hashset<v1::TaskID> killedTasks{
    update1->status().task_id(), update2->status().task_id()};

  EXPECT_EQ(v1::TASK_KILLED, update1->status().state());
  EXPECT_EQ(v1::TASK_KILLED, update2->status().state());
  EXPECT_EQ(tasks, killedTasks);

  EXPECT_CALL(*executor, subscribed(_, _));

  // The executor should only receive the queued task upon subscribing
  // with the agent since the task group has been killed in the meantime.
  Future<Nothing> launch;
  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(FutureSatisfy(&launch));

  EXPECT_CALL(*executor, launchGroup(_, _))
    .Times(0);

  {
    v1::executor::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(evolve(executorId));

    call.set_type(v1::executor::Call::SUBSCRIBE);

    call.mutable_subscribe();

    executorLibrary.get()->send(call);
  }

  AWAIT_READY(launch);

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));
}


// Test the max_completed_executors_per_framework flag.
TEST_F(SlaveTest, MaxCompletedExecutorsPerFrameworkFlag)
{
  Clock::pause();

  // We verify that the proper amount of history is maintained
  // by launching a single framework with exactly 2 executors. We
  // do this when setting `max_completed_executors_per_framework`
  // to 0, 1, and 2. This covers the cases of maintaining no
  // history, some history less than the total number of executors
  // launched, and history equal to the total number of executors
  // launched.
  const size_t totalExecutorsPerFramework = 2;
  const size_t maxExecutorsPerFrameworkArray[] = {0, 1, 2};

  foreach (const size_t maxExecutorsPerFramework,
           maxExecutorsPerFrameworkArray) {
    master::Flags masterFlags = MesosTest::CreateMasterFlags();
    Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
    ASSERT_SOME(master);

    hashmap<ExecutorID, Executor*> executorMap;
    vector<Owned<MockExecutor>> executors;

    vector<ExecutorInfo> executorInfos;

    for (size_t i = 0; i < totalExecutorsPerFramework; i++) {
      ExecutorInfo executorInfo = createExecutorInfo(stringify(i), "exit 1");

      executorInfos.push_back(executorInfo);

      Owned<MockExecutor> executor =
        Owned<MockExecutor>(new MockExecutor(executorInfo.executor_id()));

      executorMap.put(executorInfo.executor_id(), executor.get());
      executors.push_back(executor);
    }

    TestContainerizer containerizer(executorMap);

    slave::Flags agentFlags = CreateSlaveFlags();
    agentFlags.max_completed_executors_per_framework = maxExecutorsPerFramework;

    Owned<MasterDetector> detector = master.get()->createDetector();
    Try<Owned<cluster::Slave>> agent =
      StartSlave(detector.get(), &containerizer, agentFlags);

    ASSERT_SOME(agent);

    MockScheduler sched;
    MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

    Future<Nothing> schedRegistered;
    EXPECT_CALL(sched, registered(_, _, _))
      .WillOnce(FutureSatisfy(&schedRegistered));

    process::Queue<Offer> offers;
    EXPECT_CALL(sched, resourceOffers(_, _))
      .WillRepeatedly(EnqueueOffers(&offers));

    driver.start();

    AWAIT_READY(schedRegistered);

    for (size_t i = 0; i < totalExecutorsPerFramework; i++) {
      // Advance the clock to trigger both agent registration and a
      // batch allocation.
      Clock::advance(agentFlags.registration_backoff_factor);
      Clock::advance(masterFlags.allocation_interval);

      Future<Offer> offer = offers.get();
      AWAIT_READY(offer);

      TaskInfo task;
      task.set_name("");
      task.mutable_task_id()->set_value(stringify(i));
      task.mutable_slave_id()->MergeFrom(offer->slave_id());
      task.mutable_resources()->MergeFrom(offer->resources());
      task.mutable_executor()->MergeFrom(executorInfos[i]);

      EXPECT_CALL(*executors[i], registered(_, _, _, _));

      // Make sure the task passes through its `TASK_FINISHED`
      // state properly. We force this state change through
      // the launchTask() callback on our MockExecutor.
      Future<TaskStatus> statusFinished;

      EXPECT_CALL(*executors[i], launchTask(_, _))
        .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

      EXPECT_CALL(sched, statusUpdate(_, _))
        .WillOnce(FutureArg<1>(&statusFinished));

      driver.launchTasks(offer->id(), {task});

      AWAIT_READY(statusFinished);
      EXPECT_EQ(TASK_FINISHED, statusFinished->state());

      EXPECT_CALL(*executors[i], shutdown(_))
        .Times(AtMost(1));
    }

    // Destroy all of the containers to complete the executors.
    Future<hashset<ContainerID>> containerIds = containerizer.containers();
    AWAIT_READY(containerIds);

    foreach (const ContainerID& containerId, containerIds.get()) {
      Future<Nothing> executorLost;
      EXPECT_CALL(sched, executorLost(_, _, _, _))
        .WillOnce(FutureSatisfy(&executorLost));

      AWAIT_READY(containerizer.destroy(containerId));
      AWAIT_READY(executorLost);
    }

    // Ensure the agent processes the executor terminations.
    Clock::settle();

    // At this point the agent would have considered the framework
    // completed since it no longer has active executors.

    Future<Response> response = process::http::get(
      agent.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    Result<JSON::Array> completedFrameworks =
      state.values["completed_frameworks"].as<JSON::Array>();

    // There should be only 1 framework.
    ASSERT_EQ(1u, completedFrameworks->values.size());

    JSON::Object completedFramework =
      completedFrameworks->values[0].as<JSON::Object>();

    Result<JSON::Array> completedExecutorsPerFramework =
        completedFramework.values["completed_executors"].as<JSON::Array>();

    // The number of completed executors in the completed framework
    // should match the limit.
    EXPECT_EQ(maxExecutorsPerFramework,
              completedExecutorsPerFramework->values.size());

    driver.stop();
    driver.join();
  }
}


// This ensures that if the executor reconnect retry is disabled,
// PID-based V0 executors are disallowed from reregistering in
// the steady state.
//
// TODO(bmahler): It should be simpler to write a test that
// follows a standard recipe (e.g. bring up a mock executor).
TEST_F(SlaveTest, ShutdownV0ExecutorIfItReregistersWithoutReconnect)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags agentFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, agentFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Capture the agent and executor PIDs.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task;
  task.set_name("test-task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers->at(0).slave_id());
  task.mutable_resources()->MergeFrom(offers->at(0).resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(registerExecutorMessage);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Now spoof an executor re-registration, the executor
  // should be shut down.
  Future<Nothing> executorShutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&executorShutdown));

  UPID executorPid = registerExecutorMessage->from;
  UPID agentPid = registerExecutorMessage->to;

  ReregisterExecutorMessage reregisterExecutorMessage;
  reregisterExecutorMessage.mutable_executor_id()->CopyFrom(
      task.executor().executor_id());
  reregisterExecutorMessage.mutable_framework_id()->CopyFrom(
      frameworkId);

  process::post(executorPid, agentPid, reregisterExecutorMessage);

  AWAIT_READY(executorShutdown);

  driver.stop();
  driver.join();
}


// This ensures that if the executor reconnect retry is enabled,
// re-registrations from PID-based V0 executors are ignored when
// already (re-)registered.
//
// TODO(bmahler): It should be simpler to write a test that
// follows a standard recipe (e.g. bring up a mock executor).
TEST_F(SlaveTest, IgnoreV0ExecutorIfItReregistersWithoutReconnect)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.executor_reregistration_timeout = Seconds(2);
  agentFlags.executor_reregistration_retry_interval = Seconds(1);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, agentFlags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true); // Enable checkpointing.

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Capture the agent and executor PIDs.
  Future<Message> registerExecutorMessage =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task;
  task.set_name("test-task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers->at(0).slave_id());
  task.mutable_resources()->MergeFrom(offers->at(0).resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(registerExecutorMessage);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Now spoof an executor re-registration, it should be ignored
  // and the agent should not respond.
  EXPECT_NO_FUTURE_PROTOBUFS(ExecutorReregisteredMessage(), _, _);

  Future<Nothing> executorShutdown;
  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1))
    .WillOnce(FutureSatisfy(&executorShutdown));

  UPID executorPid = registerExecutorMessage->from;
  UPID agentPid = registerExecutorMessage->to;

  ReregisterExecutorMessage reregisterExecutorMessage;
  reregisterExecutorMessage.mutable_executor_id()->CopyFrom(
      task.executor().executor_id());
  reregisterExecutorMessage.mutable_framework_id()->CopyFrom(
      frameworkId);

  process::post(executorPid, agentPid, reregisterExecutorMessage);

  Clock::settle();
  EXPECT_TRUE(executorShutdown.isPending());

  driver.stop();
  driver.join();
}


// This test verifies that an executor's latest run directory can
// be browsed via the `/files` endpoint both while the executor is
// still running and after the executor terminates.
//
// Note that we only test the recommended virtual path format:
//   `/framework/FID/executor/EID/latest`.
TEST_F(SlaveTest, BrowseExecutorSandboxByVirtualPath)
{
  master::Flags masterFlags = this->CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = this->CreateSlaveFlags();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, agentFlags);
  ASSERT_SOME(slave);

  // Ensure slave has finished recovery.
  AWAIT_READY(__recover);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Advance the clock to trigger both agent registration and a batch
  // allocation.
  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_FALSE(offers->empty());

  Resources executorResources = Resources::parse("cpus:0.1;mem:32").get();
  executorResources.allocate("*");

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(
      Resources(offers.get()[0].resources()) - executorResources);

  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  task.mutable_executor()->mutable_resources()->CopyFrom(executorResources);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Manually inject a file into the sandbox.
  FrameworkID frameworkId = offers->front().framework_id();
  SlaveID slaveId = offers->front().slave_id();

  const string latestRunPath = paths::getExecutorLatestRunPath(
    agentFlags.work_dir, slaveId, frameworkId, DEFAULT_EXECUTOR_ID);
  EXPECT_TRUE(os::exists(latestRunPath));
  ASSERT_SOME(os::write(path::join(latestRunPath, "foo.bar"), "testing"));

  const string virtualPath =
    paths::getExecutorVirtualPath(frameworkId, DEFAULT_EXECUTOR_ID);

  const process::UPID files("files", slave.get()->pid.address);

  {
    const string query = string("path=") + virtualPath;
    Future<Response> response = process::http::get(
        files,
        "browse",
        query,
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_ASSERT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_ASSERT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Array> parse = JSON::parse<JSON::Array>(response->body);
    ASSERT_SOME(parse);
    EXPECT_NE(0u, parse->values.size());
  }

  {
    const string query =
      string("path=") + path::join(virtualPath, "foo.bar") + "&offset=0";
    process::UPID files("files", slave.get()->pid.address);
    Future<Response> response = process::http::get(
        files,
        "read",
        query,
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_ASSERT_RESPONSE_STATUS_EQ(OK().status, response);

    JSON::Object expected;
    expected.values["offset"] = 0;
    expected.values["data"] = "testing";

    AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);
  }

  // Now destroy the executor and make sure that the sandbox is
  // still available. We're sure that the GC won't prune the
  // sandbox since the clock is paused.
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  EXPECT_CALL(sched, executorLost(_, _, _, _))
    .Times(AtMost(1));

  AWAIT_READY(containerizer.destroy(frameworkId, DEFAULT_EXECUTOR_ID));

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_FAILED, status2->state());

  {
    const string query = string("path=") + virtualPath;
    Future<Response> response = process::http::get(
        files,
        "browse",
        query,
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_ASSERT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_ASSERT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Array> parse = JSON::parse<JSON::Array>(response->body);
    ASSERT_SOME(parse);
    EXPECT_NE(0u, parse->values.size());
  }

  {
    const string query =
      string("path=") + path::join(virtualPath, "foo.bar") + "&offset=0";
    process::UPID files("files", slave.get()->pid.address);
    Future<Response> response = process::http::get(
        files,
        "read",
        query,
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_ASSERT_RESPONSE_STATUS_EQ(OK().status, response);

    JSON::Object expected;
    expected.values["offset"] = 0;
    expected.values["data"] = "testing";

    AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that a disconnected PID-based executor will drop
// RunTaskMessages.
TEST_F(SlaveTest, DisconnectedExecutorDropsMessages)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);

  // Enable checkpointing for the framework so that the executor continues
  // running after agent termination.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, false, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  FrameworkID frameworkId = offers->front().framework_id();

  TaskInfo runningTask =
    createTask(offers->front(), "sleep 1000", DEFAULT_EXECUTOR_ID);

  // Capture the executor registration message to get the executor's pid.
  Future<Message> registerExecutor =
    FUTURE_MESSAGE(Eq(RegisterExecutorMessage().GetTypeName()), _, _);

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Capture the `RunTaskMessage` so that we can use the framework pid to spoof
  // another `RunTaskMessage` later.
  Future<RunTaskMessage> capturedRunTaskMessage =
    FUTURE_PROTOBUF(RunTaskMessage(), master.get()->pid, slave.get()->pid);

  // In addition to returning the expected task status here, this expectation
  // will also ensure that the spoofed `RunTaskMessage` we send later does not
  // trigger a call to `launchTask`.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusUpdate));

  driver.launchTasks(offers->front().id(), {runningTask});

  AWAIT_READY(registerExecutor);
  UPID executorPid = registerExecutor->from;

  AWAIT_READY(capturedRunTaskMessage);

  AWAIT_READY(statusUpdate);
  ASSERT_EQ(TASK_RUNNING, statusUpdate->state());

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.acknowledgeStatusUpdate(statusUpdate.get());

  AWAIT_READY(_statusUpdateAcknowledgement);

  // Ensure that the executor continues running after agent termination.
  EXPECT_CALL(exec, shutdown(_))
    .Times(0);

  // Terminate the agent so that the executor becomes disconnected.
  slave.get()->terminate();

  Clock::settle();

  TaskInfo droppedTask =
    createTask(offers->front(), "sleep 1000", DEFAULT_EXECUTOR_ID);

  RunTaskMessage runTaskMessage;
  runTaskMessage.mutable_framework_id()->CopyFrom(frameworkId);
  runTaskMessage.mutable_framework()->CopyFrom(frameworkInfo);
  runTaskMessage.mutable_task()->CopyFrom(droppedTask);
  runTaskMessage.set_pid(capturedRunTaskMessage->pid());

  // Send the executor a `RunTaskMessage` while it's disconnected.
  // This message should be dropped.
  process::post(executorPid, runTaskMessage);

  // Settle the clock to ensure that the `RunTaskMessage` is processed. If it is
  // not ignored, the test would fail due to a violation of the expectation we
  // previously registered on `Executor::launchTask`.
  Clock::settle();

  // Executor may call shutdown during test teardown.
  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Clock::resume();
}


// This test verifies that the 'executor_reregistration_timeout' agent flag
// successfully extends the timeout within which an executor can reregister.
TEST_F(SlaveTest, ExecutorReregistrationTimeoutFlag)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set the executor reregister timeout to a value greater than the default.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.executor_reregistration_timeout = process::TEST_AWAIT_TIMEOUT;

  Fetcher fetcher(slaveFlags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(slaveFlags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  Owned<slave::Containerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, false, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers->front(), SLEEP_COMMAND(1000));

  Future<TaskStatus> statusUpdate0;
  Future<TaskStatus> statusUpdate1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusUpdate0))
    .WillOnce(FutureArg<1>(&statusUpdate1));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusUpdate0);
  ASSERT_EQ(TASK_STARTING, statusUpdate0->state());

  driver.acknowledgeStatusUpdate(statusUpdate0.get());

  AWAIT_READY(statusUpdate1);
  ASSERT_EQ(TASK_RUNNING, statusUpdate1->state());

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.acknowledgeStatusUpdate(statusUpdate1.get());

  AWAIT_READY(_statusUpdateAcknowledgement);

  slave.get()->terminate();

  Future<ReregisterExecutorMessage> reregisterExecutor =
    DROP_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave (use same flags) with a new containerizer.
  _containerizer = MesosContainerizer::create(slaveFlags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  slave = StartSlave(detector.get(), containerizer.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Ensure that the executor attempts to reregister, so that we can capture
  // its re-registration message.
  AWAIT_READY(reregisterExecutor);

  // Make sure that we're advancing the clock more than the default timeout.
  ASSERT_TRUE(
      slaveFlags.executor_reregistration_timeout * 0.9 >
      slave::EXECUTOR_REREGISTRATION_TIMEOUT);
  Clock::advance(slaveFlags.executor_reregistration_timeout * 0.9);

  // Send the executor's delayed re-registration message.
  process::post(slave.get()->pid, reregisterExecutor.get());

  // Advance the clock to prompt the agent to reregister, and ensure that the
  // executor's task would have been marked unreachable if the executor had not
  // reregistered successfully.
  Clock::advance(slaveFlags.executor_reregistration_timeout * 0.2);

  Clock::resume();

  AWAIT_READY(slaveReregistered);

  // Perform reconciliation to verify that the task has not been transitioned to
  // TASK_LOST, as would occur if the agent had been deemed unreachable.
  vector<TaskStatus> statuses;

  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.set_state(TASK_STAGING); // Dummy value.

  statuses.push_back(status);

  Future<TaskStatus> statusUpdate2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusUpdate2));

  driver.reconcileTasks(statuses);

  AWAIT_READY(statusUpdate2);
  EXPECT_EQ(TASK_RUNNING, statusUpdate2->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, statusUpdate2->source());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, statusUpdate2->reason());

  driver.stop();
  driver.join();
}


class DefaultContainerDNSFlagTest
  : public MesosTest,
    public WithParamInterface<string> {};


INSTANTIATE_TEST_CASE_P(
    ContainerizerType,
    DefaultContainerDNSFlagTest,
    ::testing::Values("mesos", "docker"));


// This test verifies the validation for the
// agent flag `--default_container_dns`.
TEST_P(DefaultContainerDNSFlagTest, ValidateFlag)
{
  const int argc = 4;
  const char* argv[argc] = {
    "/path/to/program",
    "--master=127.0.0.1:5050",
    "--work_dir=/tmp"
  };

  string containerizer = GetParam();

  // Verifies the unknown network mode is not supported.
  //
  // TODO(qianzhang): Change the value of the `network_mode`
  // to an non-existent enum value once MESOS-7828 is resolved.
  string defaultContainerDNSInfo =
    "--default_container_dns={"
    "  \"" + containerizer + "\": [\n"
    "    {\n"
    "      \"network_mode\": \"UNKNOWN\",\n"
    "      \"dns\": {\n"
    "        \"nameservers\": [ \"8.8.8.8\" ]\n"
    "      }\n"
    "    }\n"
    "  ]\n"
    "}";

  argv[3] = defaultContainerDNSInfo.c_str();

  {
    slave::Flags flags;
    Try<flags::Warnings> load = flags.load(None(), argc, argv);
    EXPECT_ERROR(load);
  }

  // Verifies the host network mode is not supported.
  defaultContainerDNSInfo =
    "--default_container_dns={"
    "  \"" + containerizer + "\": [\n"
    "    {\n"
    "      \"network_mode\": \"HOST\",\n"
    "      \"dns\": {\n"
    "        \"nameservers\": [ \"8.8.8.8\" ]\n"
    "      }\n"
    "    }\n"
    "  ]\n"
    "}";

  argv[3] = defaultContainerDNSInfo.c_str();

  {
    slave::Flags flags;
    Try<flags::Warnings> load = flags.load(None(), argc, argv);
    EXPECT_ERROR(load);
  }

  string network_mode = (containerizer == "mesos" ? "CNI"  : "USER");

  // Verifies multiple DNS configuration without network name for
  // user-defined CNM network or CNI network is not supported.
  defaultContainerDNSInfo =
    "--default_container_dns={"
    "  \"" + containerizer + "\": [\n"
    "    {\n"
    "      \"network_mode\": \"" + network_mode + "\",\n"
    "      \"dns\": {\n"
    "        \"nameservers\": [ \"8.8.8.8\" ]\n"
    "      }\n"
    "    },\n"
    "    {\n"
    "      \"network_mode\": \"" + network_mode + "\",\n"
    "      \"dns\": {\n"
    "        \"nameservers\": [ \"8.8.8.8\" ]\n"
    "      }\n"
    "    }\n"
    "  ]\n"
    "}";

  argv[3] = defaultContainerDNSInfo.c_str();

  {
    slave::Flags flags;
    Try<flags::Warnings> load = flags.load(None(), argc, argv);
    EXPECT_ERROR(load);
  }

  // Verifies multiple DNS configuration with the same network name for CNI
  // network or user-defined CNM network or CNI network is not supported.
  defaultContainerDNSInfo =
    "--default_container_dns={"
    "  \"" + containerizer + "\": [\n"
    "    {\n"
    "      \"network_mode\": \"" + network_mode + "\",\n"
    "      \"network_name\": \"net1\",\n"
    "      \"dns\": {\n"
    "        \"nameservers\": [ \"8.8.8.8\" ]\n"
    "      }\n"
    "    },\n"
    "    {\n"
    "      \"network_mode\": \"" + network_mode + "\",\n"
    "      \"network_name\": \"net1\",\n"
    "      \"dns\": {\n"
    "        \"nameservers\": [ \"8.8.8.8\" ]\n"
    "      }\n"
    "    }\n"
    "  ]\n"
    "}";

  argv[3] = defaultContainerDNSInfo.c_str();

  {
    slave::Flags flags;
    Try<flags::Warnings> load = flags.load(None(), argc, argv);
    EXPECT_ERROR(load);
  }

  // Verifies multiple DNS configuration for Docker
  // default bridge network is not supported.
  if (containerizer == "docker") {
    // Verifies the host network mode is not supported.
    defaultContainerDNSInfo =
      "--default_container_dns={"
      "  \"" + containerizer + "\": [\n"
      "    {\n"
      "      \"network_mode\": \"BRIDGE\",\n"
      "      \"dns\": {\n"
      "        \"nameservers\": [ \"8.8.8.8\" ]\n"
      "      }\n"
      "    },\n"
      "    {\n"
      "      \"network_mode\": \"BRIDGE\",\n"
      "      \"dns\": {\n"
      "        \"nameservers\": [ \"8.8.8.8\" ]\n"
      "      }\n"
      "    }\n"
      "  ]\n"
      "}";

    argv[3] = defaultContainerDNSInfo.c_str();

    {
      slave::Flags flags;
      Try<flags::Warnings> load = flags.load(None(), argc, argv);
      EXPECT_ERROR(load);
    }
  }
}


// This test checks that when a resource provider subscribes with the
// agent's resource provider manager, the agent send an
// `UpdateSlaveMessage` reflecting the updated capacity.
//
// TODO(bbannier): We should also add tests for the agent behavior
// with resource providers where the agent ultimately resends the
// previous total when the master fails over, or for the interaction
// with the usual oversubscription protocol (oversubscribed resources
// vs. updates of total).
TEST_F(SlaveTest, ResourceProviderSubscribe)
{
  Clock::pause();

  // Start an agent and a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Specify the agent resources so we can check the reported total later.
  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:2;mem:512;disk:512;ports:[]";

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage);

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.resource_provider.test");
  resourceProviderInfo.set_name("test");

  // Register a local resource provider with the agent.
  v1::TestResourceProvider resourceProvider(resourceProviderInfo);

  Future<Nothing> connected;
  EXPECT_CALL(*resourceProvider.process, connected())
    .WillOnce(FutureSatisfy(&connected));

  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(slave.get()->pid));

  resourceProvider.start(std::move(endpointDetector), ContentType::PROTOBUF);

  AWAIT_READY(connected);

  Future<mesos::v1::resource_provider::Event::Subscribed> subscribed;
  EXPECT_CALL(*resourceProvider.process, subscribed(_))
    .WillOnce(FutureArg<0>(&subscribed));

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  {
    mesos::v1::resource_provider::Call call;
    call.set_type(mesos::v1::resource_provider::Call::SUBSCRIBE);

    call.mutable_subscribe()->mutable_resource_provider_info()->CopyFrom(
        resourceProviderInfo);

    resourceProvider.send(call);
  }

  // The subscription event contains the assigned resource provider id.
  AWAIT_READY(subscribed);

  const mesos::v1::ResourceProviderID& resourceProviderId =
    subscribed->provider_id();

  v1::Resource resourceProviderResources =
    v1::Resources::parse("disk", "8096", "*").get();

  resourceProviderResources.mutable_provider_id()->CopyFrom(resourceProviderId);

  const string resourceVersionUuid = id::UUID::random().toBytes();

  {
    mesos::v1::resource_provider::Call call;
    call.set_type(mesos::v1::resource_provider::Call::UPDATE_STATE);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId);

    mesos::v1::resource_provider::Call::UpdateState* updateState =
      call.mutable_update_state();

    updateState->mutable_resources()->CopyFrom(
        v1::Resources(resourceProviderResources));

    updateState->mutable_resource_version_uuid()->set_value(
        resourceVersionUuid);

    resourceProvider.send(call);
  }

  AWAIT_READY(updateSlaveMessage);

  ASSERT_TRUE(updateSlaveMessage->has_resource_providers());
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());

  const UpdateSlaveMessage::ResourceProvider& receivedResourceProvider =
    updateSlaveMessage->resource_providers().providers(0);

  EXPECT_EQ(
      Resources(devolve(resourceProviderResources)),
      Resources(receivedResourceProvider.total_resources()));

  EXPECT_EQ(
      resourceVersionUuid,
      receivedResourceProvider.resource_version_uuid().value());
}


// This test checks that before a workload (executor or task) is
// launched, all resources from resoruce providers nended to run the
// current set of workloads are properly published.
TEST_F(SlaveTest, ResourceProviderPublishAll)
{
  // Start an agent and a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);

  // Register a mock local resource provider with the agent.
  v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.local.mock");
  resourceProviderInfo.set_name("test");

  vector<v1::Resource> resources = {
      v1::Resources::parse("disk", "4096", "role1").get(),
      v1::Resources::parse("disk", "4096", "role2").get()
  };

  v1::TestResourceProvider resourceProvider(resourceProviderInfo, resources);

  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider.start(std::move(endpointDetector), ContentType::PROTOBUF);

  AWAIT_READY(updateSlaveMessage);

  // We want to register two frameworks to launch two concurrent tasks
  // that use the provider resources, and verify that when the second
  // task is launched, all provider resources are published.
  // NOTE: The mock schedulers and drivers are stored outside the loop
  // to avoid implicit destruction before the test ends.
  vector<Owned<MockScheduler>> scheds;
  vector<Owned<MesosSchedulerDriver>> drivers;

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  for (size_t i = 0; i < resources.size(); i++) {
    FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
    framework.set_roles(0, resources.at(i).reservations(0).role());

    Owned<MockScheduler> sched(new MockScheduler());
    Owned<MesosSchedulerDriver> driver(new MesosSchedulerDriver(
        sched.get(), framework, master.get()->pid, DEFAULT_CREDENTIAL));

    EXPECT_CALL(*sched, registered(driver.get(), _, _));

    Future<vector<Offer>> offers;

    // Decline unmatched offers.
    // NOTE: This ensures that this framework do not hold the agent's
    // default resources. Otherwise, the other one will get no offer.
    EXPECT_CALL(*sched, resourceOffers(driver.get(), _))
      .WillRepeatedly(DeclineOffers());

    EXPECT_CALL(*sched, resourceOffers(driver.get(), OffersHaveAnyResource(
        std::bind(&Resources::isReserved, lambda::_1, framework.roles(0)))))
      .WillOnce(FutureArg<1>(&offers));

    driver->start();

    AWAIT_READY(offers);
    ASSERT_FALSE(offers->empty());

    Future<mesos::v1::resource_provider::Event::PublishResources> publish;

    // Two PUBLISH_RESOURCES events will be received: one for launching the
    // executor, and the other for launching the task.
    auto resourceProviderProcess = resourceProvider.process->self();
    EXPECT_CALL(*resourceProvider.process, publishResources(_))
      .WillOnce(
          Invoke([resourceProviderProcess](
                     const typename Event::PublishResources& publish) {
            dispatch(
                resourceProviderProcess,
                &v1::TestResourceProviderProcess::publishDefault,
                publish);
          }))
      .WillOnce(DoAll(
          Invoke([resourceProviderProcess](
                     const typename Event::PublishResources& publish) {
            dispatch(
                resourceProviderProcess,
                &v1::TestResourceProviderProcess::publishDefault,
                publish);
          }),
          FutureArg<0>(&publish)));

    Future<TaskStatus> taskStarting;
    Future<TaskStatus> taskRunning;

    EXPECT_CALL(*sched, statusUpdate(driver.get(), _))
      .WillOnce(FutureArg<1>(&taskStarting))
      .WillOnce(FutureArg<1>(&taskRunning));

    // Launch a task using a provider resource.
    driver->acceptOffers(
        {offers->at(0).id()},
        {LAUNCH({createTask(
            offers->at(0).slave_id(),
            Resources(offers->at(0).resources()).reserved(framework.roles(0)),
            createCommandInfo(SLEEP_COMMAND(1000)))})},
        filters);

    AWAIT_READY(publish);

    // Test if the resources of all running executors are published.
    // This is checked through counting how many reservatinos there are
    // in the published resources: one (role1) when launching the first
    // task, two (role1, role2) when the second task is launched.
    EXPECT_EQ(i + 1, v1::Resources(publish->resources()).reservations().size());

    AWAIT_READY(taskStarting);
    EXPECT_EQ(TASK_STARTING, taskStarting->state());

    AWAIT_READY(taskRunning);
    EXPECT_EQ(TASK_RUNNING, taskRunning->state());

    // Store the mock scheduler and driver to prevent destruction.
    scheds.emplace_back(std::move(sched));
    drivers.emplace_back(std::move(driver));
  }
}


// This test checks that a resource provider gets properly disconnected when
// being marked gone, and is not able to reconnect. We expect pending operations
// to be transition to the correct terminal status.
TEST_F(SlaveTest, RemoveResourceProvider)
{
  Clock::pause();

  // Start an agent and a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(updateSlaveMessage);

  v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.rp.test");
  resourceProviderInfo.set_name("test");

  // Register a local resource provider with the agent.
  v1::Resource disk = v1::createDiskResource(
      "200",
      "storage",
      None(),
      None(),
      v1::createDiskSourceRaw(None(), "profile"));

  v1::TestResourceProvider resourceProvider(resourceProviderInfo, disk);

  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider.start(std::move(endpointDetector), ContentType::PROTOBUF);

  AWAIT_READY(updateSlaveMessage);

  // Register a framework and perform a pending operations on the
  // resource provider resources.
  v1::FrameworkInfo framework = v1::DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, disk.reservations(0).role());

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(framework));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  // Since the resources from the resource provider have already reached the
  // master at this point, the framework will be offered resource provider
  // resources.
  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());
  const v1::Offer& offer = offers->offers(0);

  Option<v1::Resource> rawDisk;
  foreach (const v1::Resource& resource, offer.resources()) {
    if (resource.has_provider_id() && resource.has_disk() &&
        resource.disk().has_source() &&
        resource.disk().source().type() ==
          v1::Resource::DiskInfo::Source::RAW) {
      rawDisk = resource;
      break;
    }
  }

  ASSERT_SOME(rawDisk);

  // Create a pending operation.
  Future<v1::resource_provider::Event::ApplyOperation> applyOperation;
  EXPECT_CALL(*resourceProvider.process, applyOperation(_))
    .WillOnce(FutureArg<0>(&applyOperation));

  v1::OperationID operationId;
  operationId.set_value("operation");

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::CREATE_DISK(
          rawDisk.get(),
          v1::Resource::DiskInfo::Source::MOUNT,
          None(),
          operationId)}));

  // Wait for the operation to reach the resource provider.
  AWAIT_READY(applyOperation);

  // A resource provider cannot be removed while it still has resources.
  Future<v1::ResourceProviderID> resourceProviderId =
    resourceProvider.process->id();

  AWAIT_READY(resourceProviderId);

  v1::agent::Call v1Call;
  v1Call.set_type(v1::agent::Call::MARK_RESOURCE_PROVIDER_GONE);
  v1Call.mutable_mark_resource_provider_gone()
    ->mutable_resource_provider_id()
    ->CopyFrom(resourceProviderId.get());

  constexpr ContentType contentType = ContentType::PROTOBUF;
  process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  headers["Accept"] = stringify(contentType);

  Future<process::http::Response> response = process::http::post(
    slave.get()->pid,
    "api/v1",
    headers,
    serialize(contentType, v1Call),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(InternalServerError().status, response);

  // Remove all resources on the resource provider.
  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  {
    using mesos::v1::resource_provider::Call;
    using mesos::v1::resource_provider::Event;

    Call call;
    call.set_type(Call::UPDATE_STATE);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    Call::UpdateState* update = call.mutable_update_state();
    update->mutable_resources()->Clear();
    update->mutable_resource_version_uuid()->set_value(
        id::UUID::random().toBytes());

    // Still report the operation. This allows us to send an operation
    // status update later on.
    mesos::v1::Operation* operation = update->add_operations();

    ASSERT_TRUE(applyOperation->has_framework_id());
    operation->mutable_framework_id()->CopyFrom(applyOperation->framework_id());

    operation->mutable_info()->CopyFrom(applyOperation->info());
    operation->mutable_uuid()->CopyFrom(applyOperation->operation_uuid());
    operation->mutable_latest_status()->CopyFrom(
        evolve(protobuf::createOperationStatus(OPERATION_PENDING)));

    operation->add_statuses()->CopyFrom(operation->latest_status());

    AWAIT_READY(resourceProvider.send(call));
  }

  // Once the master has seen that there is no resource managed
  // by the resource provider it can be removed successfully.
  AWAIT_READY(updateSlaveMessage);

  Future<v1::scheduler::Event::UpdateOperationStatus> updateOperationStatus;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&updateOperationStatus));

  // The agent will eventually update the master on its resources
  // after the resource provider disconnected.
  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // The resource provider will receive a TEARDOWN event on being marked gone.
  Future<Nothing> teardown;
  EXPECT_CALL(*resourceProvider.process, teardown())
    .WillOnce(FutureSatisfy(&teardown));

  // We expect at least two disconnection events, one initially when the
  // connected resource provider gets removed, and when the automatic attempt
  // to resubscribe fails and leads the remote to close the connection.
  Future<Nothing> disconnected;
  EXPECT_CALL(*resourceProvider.process, disconnected())
    .WillOnce(DoDefault())
    .WillOnce(FutureSatisfy(&disconnected))
    .WillRepeatedly(Return()); // Ignore additional ddisconnection events.

  // The resource provider will automatically attempt to reconnect.
  Future<Nothing> connected;
  EXPECT_CALL(*resourceProvider.process, connected())
    .WillOnce(DoDefault())
    .WillRepeatedly(Return());

  // The resource provider should never successfully resubscribe.
  EXPECT_CALL(*resourceProvider.process, subscribed(_))
    .Times(Exactly(0));

  response = process::http::post(
      slave.get()->pid,
      "api/v1",
      headers,
      serialize(contentType, v1Call),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_READY(teardown);

  // On resource provider removal the framework should have received
  // an operation status update.
  AWAIT_READY(updateOperationStatus);

  EXPECT_EQ(
    v1::OperationState::OPERATION_GONE_BY_OPERATOR,
    updateOperationStatus->status().state());

  // The status update should be generated by the agent and have no
  // associated resource provider ID.
  EXPECT_TRUE(updateOperationStatus->status().has_agent_id());
  EXPECT_FALSE(updateOperationStatus->status().has_resource_provider_id());

  // The associated metrics should have also been increased.
  EXPECT_TRUE(metricEquals("master/operations/gone_by_operator", 1));

  // The agent should also report a change to its resources.
  AWAIT_READY(updateSlaveMessage);

  // Once we have seen the second disconnection event we know that the
  // attempt to resubscribe was unsuccessful.
  AWAIT_READY(disconnected);

  // Settle the clock to ensure no more `subscribed` calls to the
  // resource provider are enqueued.
  Clock::settle();
}


// This test checks that the agent correctly updates and sends
// resource version values when it registers or reregisters.
TEST_F(SlaveTest, ResourceVersions)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Check that the agent sends its resource version uuid with
  // `RegisterSlaveMessage`.
  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::settle();
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(registerSlaveMessage);

  // Since no resource providers registered, the agent only sends its
  // own resource version uuid. The agent has no resource provider id.
  ASSERT_TRUE(registerSlaveMessage->has_resource_version_uuid());

  // Check that the agent sends its resource version uuid in
  // `ReregisterSlaveMessage`.
  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Simulate a new master detected event on the slave,
  // so that the slave will attempt to reregister.
  detector.appoint(master.get()->pid);

  Clock::settle();
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(reregisterSlaveMessage);

  // No resource changes occurred on the agent and we expect the
  // resource version uuid to be unchanged to the one sent in the
  // original registration.
  ASSERT_TRUE(reregisterSlaveMessage->has_resource_version_uuid());

  EXPECT_EQ(
      registerSlaveMessage->resource_version_uuid(),
      reregisterSlaveMessage->resource_version_uuid());
}


// Test that it is possible to add additional resources, attributes,
// and a domain when the reconfiguration policy is set to
// `additive`.
TEST_F(SlaveTest, ReconfigurationPolicy)
{
  DomainInfo domain = flags::parse<DomainInfo>(
      "{"
      "    \"fault_domain\": {"
      "        \"region\": {\"name\": \"europe\"},"
      "        \"zone\": {\"name\": \"europe-b2\"}"
      "    }"
      "}").get();

  master::Flags masterFlags = CreateMasterFlags();
  // Need to set a master domain, otherwise it will reject a slave with
  // a configured domain.
  masterFlags.domain = domain;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.attributes = "distro:debian";
  slaveFlags.resources = "cpus:4;mem:32;disk:512";

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);

  ASSERT_SOME(slave);

  // Wait until the slave registers to ensure that it has successfully
  // checkpointed its state.
  AWAIT_READY(slaveRegisteredMessage);

  slave.get()->terminate();
  slave->reset();

  // Do a valid reconfiguration.
  slaveFlags.reconfiguration_policy = "additive";
  slaveFlags.resources = "cpus:8;mem:128;disk:512";
  slaveFlags.attributes = "distro:debian;version:8";
  slaveFlags.domain = domain;

  // Restart slave.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  slave = StartSlave(detector.get(), &containerizer, slaveFlags);

  ASSERT_SOME(slave);

  // If we get here without the slave exiting, things are working as expected.
  AWAIT_READY(slaveReregisteredMessage);

  // Start scheduler and check that it gets offered the updated resources
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
  EXPECT_EQ(1u, offers->size());

  // Verify that the offer contains the new domain, attributes and resources.
  EXPECT_TRUE(offers.get()[0].has_domain());
  EXPECT_EQ(
      Attributes(offers.get()[0].attributes()),
      Attributes::parse(slaveFlags.attributes.get()));

  // The resources are slightly transformed by both master and slave
  // before they end up in an offer (in particular, ports are implicitly
  // added and they're assigned to role '*'), so we cannot simply compare
  // for equality.
  Resources offeredResources = Resources(offers.get()[0].resources());
  Resources reconfiguredResources = allocatedResources(
      Resources::parse(slaveFlags.resources.get()).get(), "*");

  EXPECT_TRUE(offeredResources.contains(reconfiguredResources));
}


// This test checks that a resource provider triggers an
// `UpdateSlaveMessage` to be sent to the master if an non-speculated
// operation fails in the resource provider.
TEST_F(SlaveTest, ResourceProviderReconciliation)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::settle();
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.resource_provider.test");
  resourceProviderInfo.set_name("test");

  // Register a resource provider with the agent.
  v1::Resources resourceProviderResources = v1::createDiskResource(
      "200",
      "*",
      None(),
      None(),
      v1::createDiskSourceRaw());

  v1::TestResourceProvider resourceProvider(
      resourceProviderInfo,
      resourceProviderResources);

  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider.start(std::move(endpointDetector), ContentType::PROTOBUF);

  AWAIT_READY(updateSlaveMessage);

  // Register a framework to excercise operations.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  Future<v1::scheduler::Event::Offers> offers1;

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers;

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "foo");

  // Subscribe the framework.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

    EXPECT_CALL(*scheduler, subscribed(_, _))
      .WillOnce(FutureArg<1>(&subscribed));

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId = subscribed->framework_id();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->offers().empty());

  // We now perform a `RESERVE` operation on the offered resources,
  // but let the operation fail in the resource provider.
  Future<v1::resource_provider::Event::ApplyOperation> operation;
  EXPECT_CALL(*resourceProvider.process, applyOperation(_))
    .WillOnce(FutureArg<0>(&operation));

  {
    const v1::Offer& offer = offers1->offers(0);

    v1::Resources reserved = offer.resources();
    reserved = reserved.filter(
        [](const v1::Resource& r) { return r.has_provider_id(); });

    ASSERT_FALSE(reserved.empty());

    reserved = reserved.pushReservation(v1::createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

    Call call =
      v1::createCallAccept(frameworkId, offer, {v1::RESERVE(reserved)});
    call.mutable_accept()->mutable_filters()->set_refuse_seconds(0);

    mesos.send(call);
  }

  AWAIT_READY(operation);

  // We expect the agent to send an `UpdateSlaveMessage` since below
  // the resource provider responds with an `UPDATE_STATE` call.
  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Future<v1::scheduler::Event::Offers> offers2;

  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers;

  // Fail the operation in the resource provider. This should trigger
  // an `UpdateSlaveMessage` to the master.
  {
    Future<v1::ResourceProviderID> resourceProviderId =
      resourceProvider.process->id();

    AWAIT_READY(resourceProviderId);

    v1::Resources resourceProviderResources_;
    foreach (v1::Resource resource, resourceProviderResources) {
      resource.mutable_provider_id()->CopyFrom(resourceProviderId.get());

      resourceProviderResources_ += resource;
    }

    // Update the resource version of the resource provider.
    id::UUID resourceVersionUuid = id::UUID::random();

    v1::resource_provider::Call call;

    call.set_type(v1::resource_provider::Call::UPDATE_STATE);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    v1::resource_provider::Call::UpdateState* updateState =
      call.mutable_update_state();

    updateState->mutable_resource_version_uuid()->CopyFrom(
        evolve(protobuf::createUUID(resourceVersionUuid)));
    updateState->mutable_resources()->CopyFrom(resourceProviderResources_);

    mesos::v1::Operation* _operation = updateState->add_operations();
    _operation->mutable_framework_id()->CopyFrom(operation->framework_id());
    _operation->mutable_info()->CopyFrom(operation->info());
    _operation->mutable_uuid()->CopyFrom(operation->operation_uuid());

    mesos::v1::OperationStatus* lastStatus =
      _operation->mutable_latest_status();
    lastStatus->set_state(::mesos::v1::OPERATION_FAILED);

    _operation->add_statuses()->CopyFrom(*lastStatus);

    AWAIT_READY(resourceProvider.send(call));
  }

  AWAIT_READY(updateSlaveMessage);

  // The reserve operation will be reported as failed, but the `statuses` field
  // will remain empty since no operation status update has been received from
  // the resource provider.
  ASSERT_TRUE(updateSlaveMessage->has_resource_providers());
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());
  auto provider = updateSlaveMessage->resource_providers().providers(0);
  ASSERT_TRUE(provider.has_operations());
  ASSERT_EQ(1, provider.operations().operations_size());

  const Operation& reserve = provider.operations().operations(0);

  EXPECT_EQ(Offer::Operation::RESERVE, reserve.info().type());
  ASSERT_TRUE(reserve.has_latest_status());
  EXPECT_EQ(OPERATION_FAILED, reserve.latest_status().state());
  EXPECT_EQ(0, reserve.statuses_size());

  // The resources are returned to the available pool and the framework will get
  // offered the same resources as in the previous offer cycle.
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers2);
  ASSERT_EQ(1, offers2->offers_size());

  const v1::Offer& offer1 = offers1->offers(0);
  const v1::Offer& offer2 = offers2->offers(0);

  EXPECT_EQ(
      v1::Resources(offer1.resources()), v1::Resources(offer2.resources()));
}


// This test verifies that the agent checks resource versions received when
// launching tasks against its own state of the used resource providers and
// rejects tasks assuming incompatible state.
TEST_F(SlaveTest, RunTaskResourceVersions)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::settle();
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  // Register a resource provider with the agent.
  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.resource_provider.test");
  resourceProviderInfo.set_name("test");

  v1::Resources resourceProviderResources = v1::createDiskResource(
      "200",
      "*",
      None(),
      None(),
      v1::createDiskSourceRaw());

  v1::TestResourceProvider resourceProvider(
      resourceProviderInfo,
      resourceProviderResources);

  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider.start(std::move(endpointDetector), ContentType::PROTOBUF);

  AWAIT_READY(updateSlaveMessage);

  // Start a framework to launch a task.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      false,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // Below we update the agent's resource version of the registered
  // resource provider. We prevent this update from propagating to the
  // master to simulate a race between the agent updating its state
  // and the master launching a task.
  updateSlaveMessage = DROP_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Update resource version of the resource provider.
  {
    Future<v1::ResourceProviderID> resourceProviderId =
      resourceProvider.process->id();

    AWAIT_READY(resourceProviderId);

    v1::Resources resourceProviderResources_;
    foreach (v1::Resource resource, resourceProviderResources) {
      resource.mutable_provider_id()->CopyFrom(resourceProviderId.get());

      resourceProviderResources_ += resource;
    }

    v1::resource_provider::Call call;
    call.set_type(v1::resource_provider::Call::UPDATE_STATE);
    call.mutable_resource_provider_id()->CopyFrom(resourceProviderId.get());

    v1::resource_provider::Call::UpdateState* updateState =
      call.mutable_update_state();

    updateState->mutable_resource_version_uuid()->CopyFrom(
        evolve(protobuf::createUUID()));
    updateState->mutable_resources()->CopyFrom(resourceProviderResources_);

    AWAIT_READY(resourceProvider.send(call));
  }

  AWAIT_READY(updateSlaveMessage);

  // Launch a task on the offered resources. Since the agent will only check
  // resource versions from resource providers used in the task launch, we
  // explicitly confirm that the offer included resource provider resources.
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  const Resources& offeredResources = offers->front().resources();
  ASSERT_TRUE(std::any_of(
      offeredResources.begin(), offeredResources.end(), [](const Resource& r) {
        return r.has_provider_id();
      }));

  TaskInfo task = createTask(offers->front(), "sleep 1000");

  Future<TaskStatus> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusUpdate));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY(statusUpdate);
  EXPECT_EQ(TASK_LOST, statusUpdate->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, statusUpdate->source());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, statusUpdate->reason());
}


// This test verifies that on agent restarts, unacknowledged operation status
// updates are resent to the master. This verifies that the agent's
// checkpointing/recovery logic works.
//
// To accomplish this:
//   1. Sends a `RESERVE` operation.
//   2. Verifies that the framework receives an operation status update. The
//      status update is not acknowledged.
//   3. Restarts the agent.
//   4. Verifies that the agent resends the operation status update.
TEST_F(SlaveTest, RetryOperationStatusUpdateAfterRecovery)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Register a framework to exercise an operation.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;

  // Set an expectation for the first offer.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // Reserve resources.
  v1::OperationID operationId;
  operationId.set_value("operation");

  ASSERT_FALSE(offer.resources().empty());

  v1::Resource reservedResources(*(offer.resources().begin()));
  reservedResources.add_reservations()->CopyFrom(
      v1::createDynamicReservationInfo(
          frameworkInfo.roles(0), DEFAULT_CREDENTIAL.principal()));

  Future<v1::scheduler::Event::UpdateOperationStatus> update;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::RESERVE(reservedResources, operationId)}));

  AWAIT_READY(update);

  EXPECT_EQ(operationId, update->status().operation_id());
  EXPECT_EQ(v1::OPERATION_FINISHED, update->status().state());
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  // Restart the agent.
  slave.get()->terminate();

  // Once the agent is restarted, the agent should resend the unacknowledged
  // operation status update.
  Future<v1::scheduler::Event::UpdateOperationStatus> retriedUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&retriedUpdate));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(retriedUpdate);

  EXPECT_EQ(operationId, retriedUpdate->status().operation_id());
  EXPECT_EQ(v1::OPERATION_FINISHED, retriedUpdate->status().state());

  // The scheduler will acknowledge the operation status update, so the agent
  // should receive an acknowledgement.
  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatusMessage =
    FUTURE_PROTOBUF(
      AcknowledgeOperationStatusMessage(), master.get()->pid, slave.get()->pid);

  mesos.send(v1::createCallAcknowledgeOperationStatus(
      frameworkId, offer.agent_id(), None(), retriedUpdate.get()));

  AWAIT_READY(acknowledgeOperationStatusMessage);

  // The master has acknowledged the operation status update, so the SLRP
  // shouldn't send further operation status updates.
  EXPECT_NO_FUTURE_PROTOBUFS(UpdateOperationStatusMessage(), _, _);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test verifies that on agent failover HTTP-based executors using resource
// provider resources can resubscribe without crashing the agent or killing the
// executor. This is a regression test for MESOS-9667 and MESOS-9711.
TEST_F(
    SlaveTest, DISABLED_AgentFailoverHTTPExecutorUsingResourceProviderResources)
{
  // This test is run with paused clock to avoid
  // dealing with retried task status updates.
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  // Use the same process ID so the executor can resubscribe.
  string processId = process::ID::generate("slave");

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, processId, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  mesos::v1::ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("org.apache.mesos.resource_provider.test");
  resourceProviderInfo.set_name("test");

  // Register a resource provider with the agent.
  v1::Resources resourceProviderResources = v1::createDiskResource(
      "200",
      "*",
      None(),
      None(),
      v1::createDiskSourceRaw());

  Owned<v1::TestResourceProvider> resourceProvider(new v1::TestResourceProvider(
      resourceProviderInfo, resourceProviderResources));

  Owned<EndpointDetector> endpointDetector(
      resource_provider::createEndpointDetector(slave.get()->pid));

  updateSlaveMessage = FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  resourceProvider->start(std::move(endpointDetector), ContentType::PROTOBUF);

  AWAIT_READY(updateSlaveMessage);

  // Register a framework to exercise operations.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  Future<v1::scheduler::Event::Offers> offers;

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(v1::scheduler::DeclineOffers());

  EXPECT_CALL(
      *scheduler,
      offers(
          _,
          v1::scheduler::OffersHaveAnyResource(
              &v1::Resources::hasResourceProvider)))
    .WillOnce(FutureArg<1>(&offers));

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "foo");
  frameworkInfo.set_checkpoint(true);
  frameworkInfo.set_failover_timeout(Days(365).secs());

  // Subscribe the framework.
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

    EXPECT_CALL(*scheduler, subscribed(_, _))
      .WillOnce(FutureArg<1>(&subscribed));

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);
  ASSERT_EQ(1, offers->offers_size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  const v1::Resources resources = offer.resources();
  ASSERT_FALSE(resources.filter(&v1::Resources::hasResourceProvider).empty())
    << "Offer does not contain resource provider resources: " << resources;

  v1::TaskID taskId;
  taskId.set_value(id::UUID::random().toString());

  Future<v1::scheduler::Event::Update> taskStarting;
  Future<v1::scheduler::Event::Update> taskRunning;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        v1::scheduler::SendAcknowledge(frameworkId, agentId),
        FutureArg<1>(&taskStarting)))
    .WillOnce(DoAll(
        v1::scheduler::SendAcknowledge(frameworkId, agentId),
        FutureArg<1>(&taskRunning)));

  // The following futures will ensure that the task status update manager has
  // checkpointed the status update acknowledgements so there will be no retry.
  //
  // NOTE: The order of the two `FUTURE_DISPATCH`s is reversed because Google
  // Mock will search the expectations in reverse order.
  Future<Nothing> _taskRunningAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);
  Future<Nothing> _taskStartingAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  {
    v1::Resources executorResources =
      *v1::Resources::parse("cpus:0.1;mem:32;disk:32");
    executorResources.allocate(frameworkInfo.roles(0));

    v1::ExecutorInfo executorInfo;
    executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
    executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
    executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
    executorInfo.mutable_resources()->CopyFrom(executorResources);

    ASSERT_TRUE(v1::Resources(offer.resources()).contains(executorResources));
    v1::Resources taskResources = offer.resources() - executorResources;

    v1::TaskInfo taskInfo =
      v1::createTask(agentId, taskResources, SLEEP_COMMAND(1000));

    Call call = v1::createCallAccept(
        frameworkId,
        offer,
        {v1::LAUNCH_GROUP(executorInfo, v1::createTaskGroupInfo({taskInfo}))});

    mesos.send(call);
  }

  AWAIT_READY(taskStarting);
  ASSERT_EQ(v1::TaskState::TASK_STARTING, taskStarting->status().state());
  ASSERT_EQ(v1::TaskStatus::SOURCE_EXECUTOR, taskStarting->status().source());
  AWAIT_READY(_taskStartingAcknowledgement);

  AWAIT_READY(taskRunning);
  ASSERT_EQ(v1::TaskState::TASK_RUNNING, taskRunning->status().state());
  ASSERT_EQ(v1::TaskStatus::SOURCE_EXECUTOR, taskRunning->status().source());
  AWAIT_READY(_taskRunningAcknowledgement);

  // Fail over the agent. We expect the executor to resubscribe successfully
  // even if the resource provider does not resubscribe.
  EXPECT_CALL(*resourceProvider->process, disconnected())
    .Times(AtMost(1));

  EXPECT_NO_FUTURE_DISPATCHES(_, &Slave::executorTerminated);

  slave.get()->terminate();

  // Terminate the mock resource provider so it won't resubscribe.
  resourceProvider.reset();

  // The following future will be satisfied when an HTTP executor subscribes.
  Future<Nothing> executorSubscribed = FUTURE_DISPATCH(_, &Slave::___run);

  slave = StartSlave(&detector, processId, slaveFlags);
  ASSERT_SOME(slave);

  // Resume the clock so when the regression happens, we'll see the executor
  // termination and the test will likely (but not 100% reliable) fail.
  Clock::resume();

  AWAIT_READY(executorSubscribed);
}


// When an agent receives a `DrainSlaveMessage`, it should kill running tasks.
// Agent API outputs related to draining are also verified.
TEST_F(SlaveTest, DrainAgentKillsRunningTask)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags, true);
  ASSERT_SOME(slave);

  slave.get()->start();

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::Offer::Operation launch = v1::LAUNCH({taskInfo});

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {launch}));

  AWAIT_READY(startingUpdate);
  EXPECT_EQ(v1::TASK_STARTING, startingUpdate->status().state());

  AWAIT_READY(runningUpdate);
  EXPECT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());

  Future<v1::scheduler::Event::Update> killedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&killedUpdate));

  // Simulate the master sending a `DrainSlaveMessage` to the agent.

  // Immediately kill the task forcefully.
  DurationInfo maxGracePeriod;
  maxGracePeriod.set_nanoseconds(0);

  DrainConfig drainConfig;
  drainConfig.mutable_max_grace_period()->CopyFrom(maxGracePeriod);

  DrainSlaveMessage drainSlaveMessage;
  drainSlaveMessage.mutable_config()->CopyFrom(drainConfig);

  process::post(master.get()->pid, slave.get()->pid, drainSlaveMessage);

  AWAIT_READY(killedUpdate);

  EXPECT_EQ(v1::TASK_KILLED, killedUpdate->status().state());
  EXPECT_EQ(
      v1::TaskStatus::REASON_AGENT_DRAINING, killedUpdate->status().reason());

  // Since the scheduler has not acknowledged the terminal task status update,
  // the agent should still be in the draining state. Confirm that its drain
  // info appears in API outputs.
  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::GET_AGENT);

    const ContentType contentType = ContentType::PROTOBUF;

    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<process::http::Response> httpResponse =
      process::http::post(
          slave.get()->pid,
          "api/v1",
          headers,
          serialize(contentType, call),
          stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, httpResponse);

    Future<v1::agent::Response> responseMessage =
      deserialize<v1::agent::Response>(contentType, httpResponse->body);

    AWAIT_READY(responseMessage);
    ASSERT_TRUE(responseMessage->get_agent().has_drain_config());
    EXPECT_EQ(
        drainConfig,
        devolve(responseMessage->get_agent().drain_config()));
  }

  {
    Future<Response> response = process::http::get(
        slave.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> state = JSON::parse<JSON::Object>(response->body);

    ASSERT_SOME(state);

    EXPECT_EQ(JSON::protobuf(drainConfig), state->values["drain_config"]);
  }

  // Now acknowledge the terminal update and confirm that the agent's drain info
  // is gone.

  Future<StatusUpdateAcknowledgementMessage> terminalAcknowledgement =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  // The agent won't complete draining until the framework has been removed.
  // Set up an expectation to await on this event.
  Future<Nothing> removeFramework;
  EXPECT_CALL(*slave.get()->mock(), removeFramework(_))
    .WillOnce(DoAll(Invoke(slave.get()->mock(),
                           &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(killedUpdate->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(killedUpdate->status().uuid());

    mesos.send(call);
  }

  // Resume the clock so that the timer used by `delay()` in `os::reap()` can
  // elapse and allow the executor process to be reaped.
  Clock::resume();

  AWAIT_READY(terminalAcknowledgement);
  AWAIT_READY(removeFramework);

  {
    v1::agent::Call call;
    call.set_type(v1::agent::Call::GET_AGENT);

    const ContentType contentType = ContentType::PROTOBUF;

    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<process::http::Response> httpResponse =
      process::http::post(
          slave.get()->pid,
          "api/v1",
          headers,
          serialize(contentType, call),
          stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, httpResponse);

    Future<v1::agent::Response> responseMessage =
      deserialize<v1::agent::Response>(contentType, httpResponse->body);

    AWAIT_READY(responseMessage);
    ASSERT_FALSE(responseMessage->get_agent().has_drain_config());
  }
}


// When the agent receives a `DrainSlaveMessage`, it should kill queued tasks.
TEST_F(SlaveTest, DrainAgentKillsQueuedTask)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  MockContainerizer mockContainerizer;
  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags slaveFlags = CreateSlaveFlags();

  EXPECT_CALL(mockContainerizer, recover(_))
    .WillOnce(Return(Nothing()));

  EXPECT_CALL(mockContainerizer, containers())
    .WillOnce(Return(hashset<ContainerID>()));

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &mockContainerizer,
      slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  // Set the partition-aware capability to ensure that the terminal update state
  // is TASK_GONE_BY_OPERATOR, since we will set `mark_gone = true`.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      v1::FrameworkInfo::Capability::PARTITION_AWARE);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::Offer::Operation launch = v1::LAUNCH({taskInfo});

  // Return a pending future from the containerizer when launching the executor
  // container so that the task remains pending.
  Promise<slave::Containerizer::LaunchResult> launchResult;
  Future<Nothing> launched;
  EXPECT_CALL(mockContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(
        FutureSatisfy(&launched),
        Return(launchResult.future())));

  EXPECT_CALL(mockContainerizer, update(_, _, _))
    .WillOnce(Return(Nothing()));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {launch}));

  AWAIT_READY(launched);

  Future<v1::scheduler::Event::Update> killedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&killedUpdate));

  // Simulate the master sending a `DrainSlaveMessage` to the agent.

  // Immediately kill the task forcefully.
  DurationInfo maxGracePeriod;
  maxGracePeriod.set_nanoseconds(0);

  DrainConfig drainConfig;
  drainConfig.set_mark_gone(true);
  drainConfig.mutable_max_grace_period()->CopyFrom(maxGracePeriod);

  DrainSlaveMessage drainSlaveMessage;
  drainSlaveMessage.mutable_config()->CopyFrom(drainConfig);

  EXPECT_CALL(mockContainerizer, destroy(_))
    .WillOnce(Return(None()));

  process::post(master.get()->pid, slave.get()->pid, drainSlaveMessage);

  AWAIT_READY(killedUpdate);

  EXPECT_EQ(v1::TASK_GONE_BY_OPERATOR, killedUpdate->status().state());
  EXPECT_EQ(
      v1::TaskStatus::REASON_AGENT_DRAINING, killedUpdate->status().reason());
}


// When the agent receives a `DrainSlaveMessage`, it should kill pending tasks.
TEST_F(SlaveTest, DrainAgentKillsPendingTask)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags slaveFlags = CreateSlaveFlags();
  MockAuthorizer mockAuthorizer;

  Try<Owned<cluster::Slave>> slave = StartSlave(
      &detector,
      &mockAuthorizer,
      slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::TaskInfo taskInfo =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000));

  v1::Offer::Operation launch = v1::LAUNCH({taskInfo});

  // Intercept authorization so that the task remains pending.
  Future<Nothing> authorized;
  Promise<bool> promise; // Never satisfied.
  EXPECT_CALL(mockAuthorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorized),
                    Return(promise.future())));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {launch}));

  AWAIT_READY(authorized);

  Future<v1::scheduler::Event::Update> killedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&killedUpdate));

  // Simulate the master sending a `DrainSlaveMessage` to the agent.

  // Immediately kill the task forcefully.
  DurationInfo maxGracePeriod;
  maxGracePeriod.set_nanoseconds(0);

  DrainConfig drainConfig;
  drainConfig.set_mark_gone(true);
  drainConfig.mutable_max_grace_period()->CopyFrom(maxGracePeriod);

  DrainSlaveMessage drainSlaveMessage;
  drainSlaveMessage.mutable_config()->CopyFrom(drainConfig);

  process::post(master.get()->pid, slave.get()->pid, drainSlaveMessage);

  AWAIT_READY(killedUpdate);

  // The terminal update state in this case should be TASK_KILLED because the
  // scheduler is not partition-aware.
  EXPECT_EQ(v1::TASK_KILLED, killedUpdate->status().state());
  EXPECT_EQ(
      v1::TaskStatus::REASON_AGENT_DRAINING, killedUpdate->status().reason());
}


// This test validates that a draining agent fails further task launch
// attempts to protect its internal draining invariants, and that the
// agent leaves the draining state on its own once all tasks have
// terminated and their status updates have been acknowledged.
TEST_F(SlaveTest, DrainingAgentRejectLaunch)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  // Register a scheduler to launch tasks.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers1;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->offers().empty());

  v1::Offer offer = offers1->offers(0);
  v1::AgentID agentId = offer.agent_id();

  // Launch a task. When the agent is put into draining state this task will be
  // killed, but we will leave the draining state open even after the task is
  // killed by not acknowledging the terminal task status update.
  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::TaskInfo taskInfo1 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000), None());

  // We do not acknowledge the KILLED update to control
  // when the agent finishes draining.
  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> killedUpdate1;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(v1::scheduler::SendAcknowledge(frameworkId, agentId)) // Starting.
    .WillOnce(DoAll(
        v1::scheduler::SendAcknowledge(frameworkId, agentId),
        FutureArg<1>(&runningUpdate1)))
    .WillOnce(FutureArg<1>(&killedUpdate1));

  mesos.send(
      v1::createCallAccept(frameworkId, offer, {v1::LAUNCH({taskInfo1})}));

  AWAIT_READY(runningUpdate1);

  // Simulate the master sending a `DrainSlaveMessage` to the agent.
  DurationInfo maxGracePeriod;
  maxGracePeriod.set_nanoseconds(0);

  DrainConfig drainConfig;
  drainConfig.set_mark_gone(false);
  drainConfig.mutable_max_grace_period()->CopyFrom(maxGracePeriod);

  DrainSlaveMessage drainSlaveMessage;
  drainSlaveMessage.mutable_config()->CopyFrom(drainConfig);

  // Explicitly wait for the executor to be terminated.
  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  process::post(master.get()->pid, slave.get()->pid, drainSlaveMessage);

  // Wait until we have received the terminal task status update
  // (which we did not acknowledge) before continuing. The agent will
  // subsequentially be left in a draining state.
  AWAIT_READY(killedUpdate1);
  ASSERT_EQ(v1::TASK_KILLED, killedUpdate1->status().state());

  Future<v1::scheduler::Event::Offers> offers2;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Resume the clock so the containerizer can detect the terminated executor.
  Clock::resume();
  AWAIT_READY(executorTerminated);
  Clock::pause();
  Clock::settle();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->offers().empty());

  offer = offers2->offers(0);
  agentId = offer.agent_id();

  // Launch another task. Since the agent is in draining
  // state the task will be rejected by the agent.
  Future<v1::scheduler::Event::Update> lostUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        v1::scheduler::SendAcknowledge(frameworkId, agentId),
        FutureArg<1>(&lostUpdate)));

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, SLEEP_COMMAND(1000), None());

  mesos.send(
      v1::createCallAccept(frameworkId, offer, {v1::LAUNCH({taskInfo2})}));

  AWAIT_READY(lostUpdate);
  ASSERT_EQ(taskInfo2.task_id(), lostUpdate->status().task_id());
  ASSERT_EQ(v1::TASK_LOST, lostUpdate->status().state());
  ASSERT_EQ(
      v1::TaskStatus::REASON_AGENT_DRAINING, lostUpdate->status().reason());

  // Acknowledge the pending task status update. Once the acknowledgement has
  // been processed the agent will leave its draining state and accept task
  // launches again.
  Future<Nothing> statusUpdateAcknowledgement =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  {
    v1::scheduler::Call call;
    call.set_type(v1::scheduler::Call::ACKNOWLEDGE);
    call.mutable_framework_id()->CopyFrom(frameworkId);

    v1::scheduler::Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(killedUpdate1->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(agentId);
    acknowledge->set_uuid(killedUpdate1->status().uuid());

    mesos.send(call);
  }

  AWAIT_READY(statusUpdateAcknowledgement);

  Future<v1::scheduler::Event::Offers> offers3;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers3))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Trigger another allocation.
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers3);
  ASSERT_FALSE(offers3->offers().empty());

  offer = offers3->offers(0);
  agentId = offer.agent_id();

  // The agent should have left its running state and now accept task launches.
  Future<v1::scheduler::Event::Update> runningUpdate2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(v1::scheduler::SendAcknowledge(frameworkId, agentId)) // Starting.
    .WillOnce(FutureArg<1>(&runningUpdate2));

  mesos.send(
      v1::createCallAccept(frameworkId, offer, {v1::LAUNCH({taskInfo2})}));

  AWAIT_READY(runningUpdate2);
  EXPECT_EQ(taskInfo2.task_id(), runningUpdate2->status().task_id());
  EXPECT_EQ(v1::TASK_RUNNING, runningUpdate2->status().state());
}


// This test verifies that if the agent recovers that it is in
// draining state any tasks after the restart are killed.
TEST_F(SlaveTest, CheckpointedDrainInfo)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);

  slave::Flags slaveFlags = CreateSlaveFlags();

  // Make the executor reregistration timeout less than the agent's
  // registration backoff factor to avoid resent status updates.
  slaveFlags.executor_reregistration_timeout = Milliseconds(2);

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  MockExecutor exec(executorId);
  TestContainerizer containerizer(&exec);

  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger the agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlaveMessage);

  // Start a framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureSatisfy(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  SlaveID slaveId = offers.get()[0].slave_id();
  TaskInfo task = createTask(
      slaveId,
      Resources::parse("cpus:1;mem:32").get(),
      SLEEP_COMMAND(1000),
      executorId);

  EXPECT_CALL(exec, registered(_, _, _, _));

  driver.launchTasks(offers.get()[0].id(), {task});

  constexpr int GRACE_PERIOD_NANOS = 1000000;
  DurationInfo maxGracePeriod;
  maxGracePeriod.set_nanoseconds(GRACE_PERIOD_NANOS);

  // We do not mark the agent as gone in contrast to some other tests here to
  // validate that we observe `TASK_KILLED` instead of `TASK_GONE_BY_OPERATOR`.
  DrainConfig drainConfig;
  drainConfig.set_mark_gone(false);
  drainConfig.mutable_max_grace_period()->CopyFrom(maxGracePeriod);

  DrainSlaveMessage drainSlaveMessage;
  drainSlaveMessage.mutable_config()->CopyFrom(drainConfig);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TaskState::TASK_RUNNING, statusRunning->state());

  // We expect a request to kill the task when the drain request is initially
  // received. The executor ignores the request and reregisters after agent
  // restart.
  Future<Nothing> killTask1;
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(FutureSatisfy(&killTask1));

  process::post(master.get()->pid, slave.get()->pid, drainSlaveMessage);

  AWAIT_READY(killTask1);

  Future<Nothing> reregistered;
  EXPECT_CALL(exec, reregistered(_, _))
    .WillOnce(DoAll(
        Invoke(&exec, &MockExecutor::reregistered),
        FutureSatisfy(&reregistered)))
    .WillRepeatedly(DoDefault());

  // Once the agent has finished recovering executors it should send
  // another task kill request to the executor.
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  // Restart the agent.
  slave.get()->terminate();

  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled))
    .WillRepeatedly(Return()); // Ignore resent updates.

  slave = StartSlave(&detector, &containerizer, slaveFlags);

  AWAIT_READY(reregistered);

  // Advance the clock to finish the executor and agent reregistration phases.
  Clock::advance(slaveFlags.executor_reregistration_timeout);
  Clock::settle();

  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_DRAINING, statusKilled->reason());
}

TEST_F(SlaveTest, DuplicateTaskIdCommandExecutor)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers1;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  Offer offer1 = offers1.get()[0];

  // Start the first task, and wait for it to complete.
  TaskInfo task1 = createTask(
      offer1.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      echoAuthorCommand(),
      None(),
      "task-1",
      "duplicate-task-id");

  Future<TaskStatus> statusStarting1;
  Future<TaskStatus> statusRunning1;
  Future<TaskStatus> statusFinished1;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&statusStarting1))
    .WillOnce(FutureArg<1>(&statusRunning1))
    .WillOnce(FutureArg<1>(&statusFinished1));

  driver.launchTasks(offer1.id(), {task1});

  AWAIT_READY(statusStarting1);
  EXPECT_EQ(TASK_STARTING, statusStarting1->state());

  AWAIT_READY(statusRunning1);
  EXPECT_EQ(TASK_RUNNING, statusRunning1->state());

  AWAIT_READY(statusFinished1);
  EXPECT_EQ(TASK_FINISHED, statusFinished1->state());

  // Start second task with same TaskId.
  Future<vector<Offer>> offers2;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
  Offer offer2 = offers2.get()[0];

  TaskInfo task2 = createTask(
      offer2.slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      echoAuthorCommand(),
      None(),
      "task-2",
      "duplicate-task-id");

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offer2.id(), {task2});

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_LOST, status2->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status2->reason());
  EXPECT_EQ(status2->message(),
      "Cannot reuse an already existing executor for a command task");

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
