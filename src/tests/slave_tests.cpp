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
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/authentication/http/basic_authenticator_factory.hpp>


#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
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
#include "common/http.hpp"
#include "common/protobuf_utils.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"

#include "master/detector/standalone.hpp"

#include "slave/constants.hpp"
#include "slave/gc.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/active_user_test_helper.hpp"
#include "tests/containerizer.hpp"
#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/limiter.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_slave.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal::slave;

#ifdef USE_SSL_SOCKET
using mesos::authentication::executor::JWTSecretGenerator;
#endif // USE_SSL_SOCKET

using mesos::internal::master::Master;

using mesos::internal::protobuf::createLabel;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::slave::ContainerTermination;

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

using std::map;
using std::shared_ptr;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Invoke;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {

// Those of the overall Mesos master/slave/scheduler/driver tests
// that seem vaguely more slave than master-related are in this file.
// The others are in "master_tests.cpp".

class SlaveTest : public MesosTest {};


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
  EXPECT_NE(0u, offers->size());

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

  // At this point the status update manager has enqueued
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


TEST_F_TEMP_DISABLED_ON_WINDOWS(SlaveTest, ShutdownUnregisteredExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  // Set the isolation flag so we know a MesosContainerizer will
  // be created.
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

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

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(process::MAX_REAP_INTERVAL());
    Clock::settle();
  }

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_REGISTRATION_TIMEOUT,
            status->reason());

  Clock::resume();

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

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

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

  // Now advance time until the reaper reaps the executor.
  while (status.isPending()) {
    Clock::advance(process::MAX_REAP_INTERVAL());
    Clock::settle();
  }

  AWAIT_READY(executorLost);

  AWAIT_READY(status);
  ASSERT_EQ(TASK_FAILED, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_SLAVE, status->source());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_LAUNCH_FAILED, status->reason());

  Clock::resume();

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
  EXPECT_NE(0u, offers->size());

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
TEST_F_TEMP_DISABLED_ON_WINDOWS(SlaveTest, CommandTaskWithArguments)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  // Command executor will run as user running test.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/echo");
  command.add_arguments("/bin/echo");
  command.add_arguments("--author");

  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
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
  EXPECT_NE(0u, offers->size());
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

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offer.id(), {task});

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
TEST_F_TEMP_DISABLED_ON_WINDOWS(SlaveTest, GetExecutorInfo)
{
  TestContainerizer containerizer;
  StandaloneMasterDetector detector;

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);

  FrameworkID frameworkId;
  frameworkId.set_value("20141010-221431-251662764-60288-32120-0000");

  FrameworkInfo frameworkInfo;
  frameworkInfo.mutable_id()->CopyFrom(frameworkId);

  // Launch a task with the command executor.
  Resources taskResources = Resources::parse("cpus:0.1;mem:32").get();
  taskResources.allocate(frameworkInfo.role());

  TaskInfo task;
  task.set_name("task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value(
      "20141010-221431-251662764-60288-32120-0001");
  task.mutable_resources()->MergeFrom(taskResources);

  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/echo");
  command.add_arguments("/bin/echo");
  command.add_arguments("--author");

  task.mutable_command()->MergeFrom(command);

  DiscoveryInfo* info = task.mutable_discovery();
  info->set_visibility(DiscoveryInfo::EXTERNAL);
  info->set_name("mytask");
  info->set_environment("mytest");
  info->set_location("mylocation");
  info->set_version("v0.1.1");

  Labels* labels = task.mutable_labels();
  labels->add_labels()->CopyFrom(createLabel("label1", "key1"));
  labels->add_labels()->CopyFrom(createLabel("label2", "key2"));

  const ExecutorInfo& executor = slave.getExecutorInfo(frameworkInfo, task);

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
TEST_F_TEMP_DISABLED_ON_WINDOWS(SlaveTest, GetExecutorInfoForTaskWithContainer)
{
  TestContainerizer containerizer;
  StandaloneMasterDetector detector;

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);

  FrameworkInfo frameworkInfo;
  frameworkInfo.mutable_id()->set_value(
      "20141010-221431-251662764-60288-12345-0000");

  // Launch a task with the command executor and ContainerInfo with
  // NetworkInfo.
  Resources taskResources = Resources::parse("cpus:0.1;mem:32").get();
  taskResources.allocate(frameworkInfo.role());

  TaskInfo task;
  task.set_name("task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value(
      "20141010-221431-251662764-60288-12345-0001");
  task.mutable_resources()->MergeFrom(taskResources);

  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/echo");
  command.add_arguments("/bin/echo");
  command.add_arguments("--author");

  task.mutable_command()->MergeFrom(command);

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  NetworkInfo* network = container->add_network_infos();
  network->add_ip_addresses()->set_ip_address("4.3.2.1");
  network->add_groups("public");

  const ExecutorInfo& executor = slave.getExecutorInfo(frameworkInfo, task);

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


// This tests ensures that MesosContainerizer will launch a command
// executor even if it contains a ContainerInfo in the TaskInfo.
// Prior to 0.26.0, this was only used to launch Docker containers, so
// MesosContainerizer would fail the launch.
//
// TODO(jieyu): Move this test to the mesos containerizer tests.
TEST_F(SlaveTest, ROOT_LaunchTaskInfoWithContainerInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  StandaloneMasterDetector detector;
  MockSlave slave(flags, &detector, containerizer.get());

  FrameworkInfo frameworkInfo;
  frameworkInfo.mutable_id()->set_value(
      "20141010-221431-251662764-60288-12345-0000");

  Resources taskResources = Resources::parse("cpus:0.1;mem:32").get();
  taskResources.allocate(frameworkInfo.role());

  // Launch a task with the command executor and ContainerInfo with
  // NetworkInfo.
  TaskInfo task;
  task.set_name("task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->set_value(
      "20141010-221431-251662764-60288-12345-0001");
  task.mutable_resources()->MergeFrom(taskResources);

  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/echo");
  command.add_arguments("/bin/echo");
  command.add_arguments("--author");

  task.mutable_command()->MergeFrom(command);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  NetworkInfo* network = container->add_network_infos();
  network->add_ip_addresses()->set_ip_address("4.3.2.1");
  network->add_groups("public");

  const ExecutorInfo& executor = slave.getExecutorInfo(frameworkInfo, task);

  Try<string> sandbox = environment->mkdtemp();
  ASSERT_SOME(sandbox);

  SlaveID slaveID;
  slaveID.set_value(UUID::random().toString());
  Future<bool> launch = containerizer->launch(
      containerId,
      task,
      executor,
      sandbox.get(),
      "nobody",
      slaveID,
      map<string, string>(),
      false);
  AWAIT_READY(launch);

  // TODO(spikecurtis): With agent capabilities (MESOS-3362), the
  // Containerizer should fail this request since none of the listed
  // isolators can handle NetworkInfo, which implies
  // IP-per-container.
  EXPECT_TRUE(launch.get());

  // Wait for the container to terminate before shutting down.
  AWAIT_READY(containerizer->wait(containerId));
}


// This test runs a command without the command user field set. The
// command will verify the assumption that the command is run as the
// slave user (in this case, root).
TEST_F(SlaveTest, ROOT_RunTaskWithCommandInfoWithoutUser)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

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

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
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
// specified user. We use (and assume the presence) of the
// unprivileged 'nobody' user which should be available on both Linux
// and Mac OS X.
TEST_F(SlaveTest, DISABLED_ROOT_RunTaskWithCommandInfoWithUser)
{
  // TODO(nnielsen): Introduce STOUT abstraction for user verification
  // instead of flat getpwnam call.
  const string testUser = "nobody";
  if (::getpwnam(testUser.c_str()) == nullptr) {
    LOG(WARNING) << "Cannot run ROOT_RunTaskWithCommandInfoWithUser test:"
                 << " user '" << testUser << "' is not present";
    return;
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_registration_timeout'.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

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

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  const string helper = getTestHelperPath("test-helper");

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  // HACK: Launch a prepare task as root to prepare the binaries.
  // This task creates the lt-mesos-executor binary in the build dir.
  // Because the real task is run as a test user (nobody), it does not
  // have permission to create files in the build directory.
  TaskInfo prepareTask;
  prepareTask.set_name("prepare task");
  prepareTask.mutable_task_id()->set_value("1");
  prepareTask.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  prepareTask.mutable_resources()->CopyFrom(
      offers.get()[0].resources());

  Result<string> user = os::user();
  ASSERT_SOME(user) << "Failed to get current user name"
                    << (user.isError() ? ": " + user.error() : "");
  // Current user should be root.
  EXPECT_EQ("root", user.get());

  // This prepare command executor will run as the current user
  // running the tests (root). After this command executor finishes,
  // we know that the lt-mesos-executor binary file exists.
  CommandInfo prepareCommand;
  prepareCommand.set_shell(false);
  prepareCommand.set_value(helper);
  prepareCommand.add_arguments(helper);
  prepareCommand.add_arguments(ActiveUserTestHelper::NAME);
  prepareCommand.add_arguments("--user=" + user.get());
  prepareTask.mutable_command()->CopyFrom(prepareCommand);

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {prepareTask});

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusRunning->source());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusFinished->source());

  // Start to launch a task with different user.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("2");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());

  CommandInfo command;
  command.set_user(testUser);
  command.set_shell(false);
  command.set_value(helper);
  command.add_arguments(helper);
  command.add_arguments(ActiveUserTestHelper::NAME);
  command.add_arguments("--user=" + testUser);

  task.mutable_command()->CopyFrom(command);

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusRunning->source());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());
  EXPECT_EQ(TaskStatus::SOURCE_EXECUTOR, statusFinished->source());

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
  EXPECT_NE(0u, offers->size());

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

  JSON::Object snapshot = Metrics();

  EXPECT_EQ(1u, snapshot.values.count("slave/uptime_secs"));
  EXPECT_EQ(1u, snapshot.values.count("slave/registered"));

  EXPECT_EQ(1u, snapshot.values.count("slave/recovery_errors"));

  EXPECT_EQ(1u, snapshot.values.count("slave/frameworks_active"));

  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_staging"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_starting"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_running"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_finished"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_failed"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_killed"));
  EXPECT_EQ(1u, snapshot.values.count("slave/tasks_lost"));

  EXPECT_EQ(1u, snapshot.values.count("slave/executors_registering"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_running"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_terminating"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_terminated"));
  EXPECT_EQ(1u, snapshot.values.count("slave/executors_preempted"));

  EXPECT_EQ(1u, snapshot.values.count("slave/valid_status_updates"));
  EXPECT_EQ(1u, snapshot.values.count("slave/invalid_status_updates"));

  EXPECT_EQ(1u, snapshot.values.count("slave/valid_framework_messages"));
  EXPECT_EQ(1u, snapshot.values.count("slave/invalid_framework_messages"));

  EXPECT_EQ(
      1u,
      snapshot.values.count("slave/executor_directory_max_allowed_age_secs"));

  EXPECT_EQ(1u, snapshot.values.count("slave/container_launch_errors"));

  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/cpus_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/gpus_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/gpus_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/gpus_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/mem_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/mem_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/mem_percent"));

  EXPECT_EQ(1u, snapshot.values.count("slave/disk_total"));
  EXPECT_EQ(1u, snapshot.values.count("slave/disk_used"));
  EXPECT_EQ(1u, snapshot.values.count("slave/disk_percent"));
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
  EXPECT_NE(0u, offers->size());
  const Offer offer = offers.get()[0];

  // Verify that we start with no launch failures.
  JSON::Object snapshot = Metrics();
  EXPECT_EQ(0, snapshot.values["slave/container_launch_errors"]);

  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _, _))
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

  // Agents should always have MULTI_ROLE capability in current implementation.
  Try<JSON::Value> expectedCapabilities = JSON::parse("[\"MULTI_ROLE\"]");
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
  EXPECT_NE(0u, offers->size());

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
  JSON::Object framework = frameworks.values[0].as<JSON::Object>();

  EXPECT_EQ("*", framework.values["role"]);
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


// This test checks that when a slave is in RECOVERING state it responds
// to HTTP requests for "/state" endpoint with ServiceUnavailable.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    SlaveTest,
    StateEndpointUnavailableDuringRecovery)
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

  // This pointer is passed to the agent, which will perform the cleanup.
  MockSecretGenerator* mockSecretGenerator = new MockSecretGenerator();

  MockSlave slave(
      CreateSlaveFlags(),
      detector.get(),
      containerizer.get(),
      None(),
      None(),
      mockSecretGenerator);
  process::PID<Slave> slavePid = spawn(slave);

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

  {
    v1::scheduler::Call call;
    call.set_type(v1::scheduler::Call::SUBSCRIBE);
    v1::scheduler::Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0, offers->offers().size());

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
    containerId.set_value(UUID::random().toString());
    containerId.mutable_parent()->CopyFrom(parentContainerId);

    v1::agent::Call call;
    call.set_type(v1::agent::Call::LAUNCH_NESTED_CONTAINER);

    call.mutable_launch_nested_container()->mutable_container_id()
      ->CopyFrom(containerId);

    process::http::Headers headers;
    headers["Authorization"] =
      "Bearer " + authenticationToken.get().value().data();

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

  terminate(slave);
  wait(slave);
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

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);
  spawn(slave);

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
  EXPECT_NE(0u, offers->size());

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
      slave.self(),
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

  terminate(slave);
  wait(slave);
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

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);

  EXPECT_CALL(slave, usage())
    .WillOnce(Return(Failure("Resource Collection Failure")));

  spawn(slave);

  Future<Response> response = process::http::get(
      slave.self(),
      "monitor/statistics",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_READY(response);
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(InternalServerError().status, response);

  terminate(slave);
  wait(slave);
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
  EXPECT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  // Launch a task and wait until it is in RUNNING status.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:32").get(),
      SLEEP_COMMAND(1000));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_RUNNING, status->state());

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
              "\"cpus_limit\":%g,"
              "\"mem_limit_bytes\":%lu"
          "}"
      "}]",
      1 + slave::DEFAULT_EXECUTOR_CPUS,
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

  const string statisticsEndpoints[] =
    {"monitor/statistics", "monitor/statistics.json"};

  foreach (const string& statisticsEndpoint, statisticsEndpoints) {
    // Unauthenticated requests are rejected.
    {
      Future<Response> response = process::http::get(
          agent.get()->pid,
          statisticsEndpoint);

      AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response)
          << response->body;
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

      AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response)
          << response->body;
    }

    // Correctly authenticated requests succeed.
    {
      Future<Response> response = process::http::get(
          agent.get()->pid,
          statisticsEndpoint,
          None(),
          createBasicAuthHeaders(DEFAULT_CREDENTIAL));

      AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
          << response->body;
    }
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
// correct container status and resource statistics based on the
// currently running executors, and the values returned by the
// '/containers' endpoint are as expected.
TEST_F(SlaveTest, ContainersEndpoint)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);
  spawn(slave);

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
  EXPECT_NE(0u, offers->size());

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

  ResourceStatistics statistics;
  statistics.set_mem_limit_bytes(2048);

  EXPECT_CALL(containerizer, usage(_))
    .WillOnce(Return(statistics));

  ContainerStatus containerStatus;

  ContainerID parent;
  ContainerID child;
  parent.set_value("parent");
  child.set_value("child");
  child.mutable_parent()->CopyFrom(parent);
  containerStatus.mutable_container_id()->CopyFrom(child);

  CgroupInfo* cgroupInfo = containerStatus.mutable_cgroup_info();
  CgroupInfo::NetCls* netCls = cgroupInfo->mutable_net_cls();
  netCls->set_classid(42);

  NetworkInfo* networkInfo = containerStatus.add_network_infos();
  NetworkInfo::IPAddress* ipAddr = networkInfo->add_ip_addresses();
  ipAddr->set_ip_address("192.168.1.20");

  EXPECT_CALL(containerizer, status(_))
    .WillOnce(Return(containerStatus));

  Future<Response> response = process::http::get(
      slave.self(),
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
          "\"executor_id\":\"default\","
          "\"executor_name\":\"\","
          "\"source\":\"\","
          "\"statistics\":{"
              "\"mem_limit_bytes\":2048"
          "},"
          "\"status\":{"
              "\"container_id\":{"
                "\"parent\":{\"value\":\"parent\"},"
                "\"value\":\"child\""
              "},"
              "\"cgroup_info\":{\"net_cls\":{\"classid\":42}},"
              "\"network_infos\":[{"
                  "\"ip_addresses\":[{\"ip_address\":\"192.168.1.20\"}]"
              "}]"
          "}"
      "}]");

  ASSERT_SOME(expected);
  EXPECT_TRUE(value->contains(expected.get()));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  terminate(slave);
  wait(slave);
}


// This test ensures that when a slave is shutting down, it will not
// try to re-register with the master.
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
  EXPECT_NE(0u, offers->size());
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
  EXPECT_CALL(containerizer, update(_, _))
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
  EXPECT_NE(0u, offers->size());
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
  EXPECT_CALL(containerizer, update(_, _))
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

  EXPECT_CALL(containerizer, update(_, _))
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
  EXPECT_CALL(containerizer, update(_, _))
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


// This test ensures that the slave will re-register with the master
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
  EXPECT_EQ(totalTimeout, Seconds(connection.total_ping_timeout_seconds()));

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


// This test ensures that the slave will re-register with the master
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
  EXPECT_CALL(*master.get()->registrar.get(), apply(_))
    .Times(0);

  process::dispatch(master.get()->pid,
                    &Master::markUnreachable,
                    slaveId,
                    "dummy test case dispatch");

  Clock::settle();
  Clock::resume();

  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


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
  Future<Owned<master::Operation>> markUnreachable;
  Promise<bool> markUnreachableContinue;
  EXPECT_CALL(*master.get()->registrar.get(), apply(_))
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

  EXPECT_CALL(*master.get()->registrar.get(), apply(_))
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
  Future<Owned<master::Operation>> removeSlave;
  Promise<bool> removeSlaveContinue;
  EXPECT_CALL(*master.get()->registrar.get(), apply(_))
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

  EXPECT_CALL(*master.get()->registrar.get(), apply(_))
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

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);
  spawn(slave);

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
  EXPECT_NE(0u, offers->size());

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

  EXPECT_CALL(slave, runTask(_, _, _, _, _))
    .WillOnce(Invoke(&slave, &MockSlave::unmocked_runTask));

  // Saved arguments from Slave::_run().
  Future<bool> future;
  FrameworkInfo frameworkInfo;
  ExecutorInfo executorInfo;
  Option<TaskGroupInfo> taskGroup;
  Option<TaskInfo> task_;
  // Skip what Slave::_run() normally does, save its arguments for
  // later, tie reaching the critical moment when to kill the task to
  // a future.
  Future<Nothing> _run;
  EXPECT_CALL(slave, _run(_, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&_run),
                    SaveArg<0>(&future),
                    SaveArg<1>(&frameworkInfo),
                    SaveArg<2>(&executorInfo),
                    SaveArg<3>(&task_),
                    SaveArg<4>(&taskGroup)));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(_run);

  Future<Nothing> killTask;
  EXPECT_CALL(slave, killTask(_, _))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask)));

  driver.killTask(task.task_id());

  AWAIT_READY(killTask);

  // Since this is the only task ever for this framework, the
  // framework should get removed in Slave::_run().
  // Thus we can observe that this happens before Shutdown().
  Future<Nothing> removeFramework;
  EXPECT_CALL(slave, removeFramework(_))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  slave.unmocked__run(
      future, frameworkInfo, executorInfo, task_, taskGroup);

  AWAIT_READY(removeFramework);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  driver.stop();
  driver.join();

  terminate(slave);
  wait(slave);
}


// This test ensures that if a `killTask()` for an executor is received by the
// agent before the executor registers, the executor is properly cleaned up.
TEST_F(SlaveTest, KillTaskUnregisteredExecutor)
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

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());

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
  EXPECT_EQ(TaskStatus::REASON_EXECUTOR_UNREGISTERED, status->reason());

  // Now let the executor register by spoofing the message.
  RegisterExecutorMessage registerExecutor;
  registerExecutor.ParseFromString(registerExecutorMessage->body);

  process::post(registerExecutorMessage->from,
                slave.get()->pid,
                registerExecutor);

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
  EXPECT_NE(0, offers->offers().size());

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

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::KILL);

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(task1.task_id());
    kill->mutable_agent_id()->CopyFrom(offer.agent_id());

    mesos.send(call);
  }

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


// This test verifies that when a slave re-registers with the master
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

  // Ensure status update manager handles TASK_RUNNING update.
  AWAIT_READY(___statusUpdate);

  Future<Nothing> ___statusUpdate2 =
    FUTURE_DISPATCH(_, &Slave::___statusUpdate);

  // Now send TASK_FINISHED update.
  TaskStatus finishedStatus;
  finishedStatus = statusUpdateMessage->update().status();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  // Ensure status update manager handles TASK_FINISHED update.
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

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);
  spawn(slave);

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
  EXPECT_NE(0u, offers->size());

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
  Future<ResourceUsage> usage = slave.usage();

  AWAIT_READY(usage);
  ASSERT_EQ(1, usage->executors_size());
  EXPECT_FALSE(usage->executors(0).has_statistics());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  terminate(slave);
  wait(slave);
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
  EXPECT_NE(0u, offers->size());

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
  EXPECT_NE(0u, offers->size());

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
  EXPECT_NE(0u, offers->size());

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
              update(_, Resources(offers.get()[0].resources())))
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
  EXPECT_NE(0u, offers->size());

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
  EXPECT_NE(0u, offers->size());

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
// won't inhert the slaves.
TEST_F_TEMP_DISABLED_ON_WINDOWS(SlaveTest, ExecutorEnvironmentVariables)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Need flags for 'executor_environment_variables'.
  slave::Flags flags = CreateSlaveFlags();

  Try<JSON::Object> parse = JSON::parse<JSON::Object>("{\"PATH\": \"/bin\"}");

  ASSERT_SOME(parse);

  flags.executor_environment_variables = parse.get();

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
  EXPECT_NE(0u, offers->size());

  // Launch a task with the command executor.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());

  // Command executor will run as user running test.
  CommandInfo command;
  command.set_shell(true);
  command.set_value("test $PATH = /bin");

  task.mutable_command()->MergeFrom(command);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  // Scheduler should first receive TASK_RUNNING followed by the
  // TASK_FINISHED from the executor.
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

  MockSlave slave(flags, &detector, &containerizer);
  spawn(slave);

  Clock::pause();

  // Wait for slave to be initialized.
  Clock::settle();

  // We expect that the slave will return ResourceUsage with
  // total resources reported.
  Future<ResourceUsage> usage = slave.usage();

  AWAIT_READY(usage);

  // Total resources should match the resources from flag.resources.
  EXPECT_EQ(Resources(usage->total()),
            Resources::parse(flags.resources.get()).get());

  terminate(slave);
  wait(slave);
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

  MockSlave slave(flags, &detector, &containerizer);
  spawn(slave);

  Clock::pause();

  // Wait for slave to be initialized.
  Clock::settle();

  Resource dynamicReservation = Resources::parse("cpus", "1", "role1").get();
  dynamicReservation.mutable_reservation()->CopyFrom(
      createReservationInfo("principal"));

  Resource persistentVolume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  vector<Resource> checkpointedResources =
    {dynamicReservation, persistentVolume};

  // Add checkpointed resources.
  slave.checkpointResources(checkpointedResources);

  // We expect that the slave will return ResourceUsage with
  // total and checkpointed slave resources reported.
  Future<ResourceUsage> usage = slave.usage();

  AWAIT_READY(usage);

  Resources usageTotalResources(usage->total());

  // Reported total field should contain persistent volumes and dynamic
  // reservations.
  EXPECT_EQ(usageTotalResources.persistentVolumes(), persistentVolume);
  EXPECT_TRUE(usageTotalResources.contains(dynamicReservation));

  terminate(slave);
  wait(slave);
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
TEST_F_TEMP_DISABLED_ON_WINDOWS(SlaveTest, HTTPSchedulerSlaveRestart)
{
  Try<Owned<cluster::Master>> master = this->StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = this->CreateSlaveFlags();

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers->size());

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
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Restart the slave.
  slave.get()->terminate();

  _containerizer = MesosContainerizer::create(flags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

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

  Clock::settle();

  // Ensure the slave considers itself recovered.
  Clock::advance(flags.executor_reregistration_timeout);

  Clock::resume();

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
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers->size());
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
  EXPECT_NE(0, offers->offers().size());

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

  // This pointer is passed to the agent, which will perform the cleanup.
  MockSecretGenerator* secretGenerator = new MockSecretGenerator();

  MockSlave slave(
      CreateSlaveFlags(),
      &detector,
      &containerizer,
      None(),
      None(),
      secretGenerator);
  spawn(slave);

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
  ASSERT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

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

  EXPECT_CALL(slave, executorTerminated(_, _, _))
    .WillOnce(Invoke(&slave, &MockSlave::unmocked_executorTerminated));

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

  ASSERT_EQ(TASK_FAILED, update1->status().state());
  ASSERT_EQ(TASK_FAILED, update2->status().state());

  EXPECT_TRUE(strings::contains(update1->status().message(), failureMessage));
  EXPECT_TRUE(strings::contains(update2->status().message(), failureMessage));

  ASSERT_EQ(tasks, failedTasks);

  // Since this is the only task group for this framework, the
  // framework should be removed after secret generation fails.
  Future<Nothing> removeFramework;
  EXPECT_CALL(slave, removeFramework(_))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_removeFramework),
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

  terminate(slave);
  wait(slave);
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

  // This pointer is passed to the agent, which will perform the cleanup.
  MockSecretGenerator* secretGenerator = new MockSecretGenerator();

  MockSlave slave(
      CreateSlaveFlags(),
      &detector,
      &containerizer,
      None(),
      None(),
      secretGenerator);
  spawn(slave);

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
  ASSERT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

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

  EXPECT_CALL(slave, executorTerminated(_, _, _))
    .WillOnce(Invoke(&slave, &MockSlave::unmocked_executorTerminated));

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

  ASSERT_EQ(TASK_FAILED, update1->status().state());
  ASSERT_EQ(TASK_FAILED, update2->status().state());

  const string failureMessage =
    "Secret of type VALUE must have the 'value' field set";

  EXPECT_TRUE(strings::contains(update1->status().message(), failureMessage));
  EXPECT_TRUE(strings::contains(update2->status().message(), failureMessage));

  ASSERT_EQ(tasks, failedTasks);

  // Since this is the only task group for this framework, the
  // framework should be removed after secret generation fails.
  Future<Nothing> removeFramework;
  EXPECT_CALL(slave, removeFramework(_))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_removeFramework),
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

  terminate(slave);
  wait(slave);
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

  // This pointer is passed to the agent, which will perform the cleanup.
  MockSecretGenerator* secretGenerator = new MockSecretGenerator();

  MockSlave slave(
      CreateSlaveFlags(),
      &detector,
      &containerizer,
      None(),
      None(),
      secretGenerator);
  spawn(slave);

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
  ASSERT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

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

  EXPECT_CALL(slave, executorTerminated(_, _, _))
    .WillOnce(Invoke(&slave, &MockSlave::unmocked_executorTerminated));

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

  ASSERT_EQ(TASK_FAILED, update1->status().state());
  ASSERT_EQ(TASK_FAILED, update2->status().state());

  const string failureMessage =
    "Expecting generated secret to be of VALUE type instead of REFERENCE type";

  EXPECT_TRUE(strings::contains(update1->status().message(), failureMessage));
  EXPECT_TRUE(strings::contains(update2->status().message(), failureMessage));

  ASSERT_EQ(tasks, failedTasks);

  // Since this is the only task group for this framework, the
  // framework should be removed after secret generation fails.
  Future<Nothing> removeFramework;
  EXPECT_CALL(slave, removeFramework(_))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_removeFramework),
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

  terminate(slave);
  wait(slave);
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

  // This pointer is passed to the agent, which will perform the cleanup.
  MockSecretGenerator* secretGenerator = new MockSecretGenerator();

  MockSlave slave(
      CreateSlaveFlags(),
      &detector,
      &containerizer,
      None(),
      None(),
      secretGenerator);
  spawn(slave);

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
  ASSERT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

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
  EXPECT_CALL(slave, shutdownExecutor(_, _, _))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_shutdownExecutor),
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

  EXPECT_CALL(slave, executorTerminated(_, _, _))
    .WillOnce(Invoke(&slave, &MockSlave::unmocked_executorTerminated));

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

  ASSERT_EQ(TASK_FAILED, update1->status().state());
  ASSERT_EQ(TASK_FAILED, update2->status().state());

  const string failureMessage = "Executor terminating";

  EXPECT_TRUE(strings::contains(update1->status().message(), failureMessage));
  EXPECT_TRUE(strings::contains(update2->status().message(), failureMessage));

  ASSERT_EQ(tasks, failedTasks);

  // Since this is the only task group for this framework, the
  // framework should be removed after secret generation fails.
  Future<Nothing> removeFramework;
  EXPECT_CALL(slave, removeFramework(_))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_removeFramework),
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

  terminate(slave);
  wait(slave);
}


#ifdef USE_SSL_SOCKET
// This test verifies that a default executor which is launched when secret
// generation is enabled and HTTP executor authentication is not required will
// be able to re-subscribe successfully when the agent is restarted with
// required HTTP executor authentication.
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
  ASSERT_NE(0, offers->offers().size());

  Future<v1::scheduler::Event::Update> update;

  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  const v1::Offer offer = offers->offers(0);
  const v1::AgentID agentId = offer.agent_id();

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  // Create a task which should run indefinitely.
  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, "cat");

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

  AWAIT_READY(update);

  ASSERT_EQ(TASK_RUNNING, update->status().state());
  ASSERT_EQ(taskInfo.task_id(), update->status().task_id());

  Future<Nothing> _statusUpdateAcknowledgement =
    FUTURE_DISPATCH(slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update->status().uuid());

    mesos.send(call);
  }

  AWAIT_READY(_statusUpdateAcknowledgement);

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

  MockSlave slave(CreateSlaveFlags(), &detector, &containerizer);
  spawn(slave);

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
  EXPECT_NE(0, offers->offers().size());

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

  EXPECT_CALL(slave, runTaskGroup(_, _, _, _))
    .WillOnce(Invoke(&slave, &MockSlave::unmocked_runTaskGroup));

  // Saved arguments from `Slave::_run()`.
  Future<bool> future;
  FrameworkInfo frameworkInfo;
  ExecutorInfo executorInfo_;
  Option<TaskGroupInfo> taskGroup_;
  Option<TaskInfo> task_;

  // Skip what `Slave::_run()` normally does, save its arguments for
  // later, till reaching the critical moment when to kill the task
  // in the future.
  Future<Nothing> _run;
  EXPECT_CALL(slave, _run(_, _, _, _, _))
    .WillOnce(DoAll(FutureSatisfy(&_run),
                    SaveArg<0>(&future),
                    SaveArg<1>(&frameworkInfo),
                    SaveArg<2>(&executorInfo_),
                    SaveArg<3>(&task_),
                    SaveArg<4>(&taskGroup_)));

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
  EXPECT_CALL(slave, killTask(_, _))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_killTask),
                    FutureSatisfy(&killTask)));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::KILL);

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(taskInfo1.task_id());
    kill->mutable_agent_id()->CopyFrom(offer.agent_id());

    mesos.send(call);
  }

  AWAIT_READY(killTask);

  // Since this is the only task group for this framework, the
  // framework should get removed in `Slave::_run()`.
  Future<Nothing> removeFramework;
  EXPECT_CALL(slave, removeFramework(_))
    .WillOnce(DoAll(Invoke(&slave, &MockSlave::unmocked_removeFramework),
                    FutureSatisfy(&removeFramework)));

  slave.unmocked__run(
      future, frameworkInfo, executorInfo_, task_, taskGroup_);

  AWAIT_READY(removeFramework);

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  const hashset<v1::TaskID> killedTasks{
    update1->status().task_id(), update2->status().task_id()};

  EXPECT_EQ(TASK_KILLED, update1->status().state());
  EXPECT_EQ(TASK_KILLED, update2->status().state());
  EXPECT_EQ(tasks, killedTasks);

  terminate(slave);
  wait(slave);
}


// This test verifies that the agent correctly populates the
// command info for default executor.
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
  EXPECT_NE(0, offers->offers().size());

  Future<ExecutorInfo> executorInfo_;
  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<2>(&executorInfo_),
                    Return(Future<bool>())));

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

  AWAIT_READY(executorInfo_);

  // TODO(anand): Add a `strings::contains()` check to ensure
  // `MESOS_DEFAULT_EXECUTOR` is present in the command when
  // we add the executable for default executor.
  ASSERT_TRUE(executorInfo_->has_command());
  EXPECT_EQ(frameworkInfo.user(), executorInfo_->command().user());
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
  EXPECT_NE(0, offers->offers().size());

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
  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::KILL);

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(taskInfo2.task_id());
    kill->mutable_agent_id()->CopyFrom(offer.agent_id());

    mesos.send(call);
  }

  AWAIT_READY(update1);
  AWAIT_READY(update2);

  const hashset<v1::TaskID> killedTasks{
    update1->status().task_id(), update2->status().task_id()};

  EXPECT_EQ(TASK_KILLED, update1->status().state());
  EXPECT_EQ(TASK_KILLED, update2->status().state());
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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
