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

#include <memory>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/master/allocator.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "common/build.hpp"
#include "common/protobuf_utils.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"

#include "slave/constants.hpp"
#include "slave/gc.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/limiter.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::Master;

using mesos::internal::master::allocator::MesosAllocatorProcess;

using mesos::internal::protobuf::createLabel;

using mesos::internal::slave::GarbageCollectorProcess;
using mesos::internal::slave::Slave;
using mesos::internal::slave::Containerizer;
using mesos::internal::slave::MesosContainerizerProcess;

using process::Clock;
using process::Future;
using process::PID;
using process::Promise;

using std::shared_ptr;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Not;
using testing::Return;
using testing::SaveArg;

namespace mesos {
namespace internal {
namespace tests {

// Those of the overall Mesos master/slave/scheduler/driver tests
// that seem vaguely more master than slave-related are in this file.
// The others are in "slave_tests.cpp".

class MasterTest : public MesosTest {};


TEST_F(MasterTest, TaskRunning)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Ensure the hostname and url are set correctly.
  EXPECT_EQ(slave.get().address.hostname().get(), offers.get()[0].hostname());

  mesos::URL url;
  url.set_scheme("http");
  url.mutable_address()->set_ip(stringify(slave.get().address.ip));
  url.mutable_address()->set_hostname(slave.get().address.hostname().get());
  url.mutable_address()->set_port(slave.get().address.port);
  url.set_path("/" + slave.get().id);

  EXPECT_EQ(url, offers.get()[0].url());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

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
  EXPECT_EQ(TASK_RUNNING, status.get().state());
  EXPECT_TRUE(status.get().has_executor_id());
  EXPECT_EQ(exec.id, status.get().executor_id());

  AWAIT_READY(update);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test ensures that stopping a scheduler driver triggers
// executor's shutdown callback and all still running tasks are
// marked as killed.
TEST_F(MasterTest, ShutdownFrameworkWhileTaskRunning)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();

  Try<PID<Slave>> slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

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

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offer.resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Nothing())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(update);

  // Set expectation that Master receives teardown call, which
  // triggers marking running tasks as killed.
  Future<mesos::scheduler::Call> teardownCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::TEARDOWN, _, _);

  // Set expectation that Executor's shutdown callback is invoked.
  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  // Stop the driver while the task is running.
  driver.stop();
  driver.join();

  // Wait for teardown call to be dispatched and executor's shutdown
  // callback to be called.
  AWAIT_READY(teardownCall);
  AWAIT_READY(shutdown);

  // We have to be sure the teardown call is processed completely and
  // running tasks enter a terminal state before we request the master
  // state.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Request master state.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state");
  AWAIT_READY(response);

  // These checks are not essential for the test, but may help
  // understand what went wrong.
  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  // Make sure the task landed in completed and marked as killed.
  Result<JSON::String> state = parse.get().find<JSON::String>(
      "completed_frameworks[0].completed_tasks[0].state");

  ASSERT_SOME_EQ(JSON::String("TASK_KILLED"), state);

  Shutdown();  // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, KillTask)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that a killTask for an unknown task results in a
// TASK_LOST when there are no slaves in transitionary states.
TEST_F(MasterTest, KillUnknownTask)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  TaskID unknownTaskId;
  unknownTaskId.set_value("2");

  driver.killTask(unknownTaskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, KillUnknownTaskSlaveInTransition)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<PID<Slave>> slave = StartSlave(&exec, slaveFlags);
  ASSERT_SOME(slave);

  // Wait for slave registration.
  AWAIT_READY(slaveRegisteredMessage);
  const SlaveID slaveId = slaveRegisteredMessage.get().slave_id();

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Start a task.
  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Future<Nothing> _reregisterSlave =
    DROP_DISPATCH(_, &Master::_reregisterSlave);

  // Stop master and slave.
  Stop(master.get());
  Stop(slave.get());

  frameworkId = Future<FrameworkID>();
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Restart master.
  master = StartMaster();
  ASSERT_SOME(master);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  detector.appoint(master.get());

  AWAIT_READY(frameworkId);

  // Restart slave.
  slave = StartSlave(&exec, slaveFlags);

  // Wait for the slave to start reregistration.
  AWAIT_READY(_reregisterSlave);

  // As Master::killTask isn't doing anything, we shouldn't get a status update.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  Future<mesos::scheduler::Call> killCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::KILL, _, _);

  // Attempt to kill unknown task while slave is transitioning.
  TaskID unknownTaskId;
  unknownTaskId.set_value("2");

  ASSERT_FALSE(unknownTaskId == task.task_id());

  Clock::pause();

  driver.killTask(unknownTaskId);

  AWAIT_READY(killCall);

  // Wait for all messages to be dispatched and processed completely to satisfy
  // the expectation that we didn't receive a status update.
  Clock::settle();

  Clock::resume();

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, StatusUpdateAck)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateAcknowledgementMessage> acknowledgement =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, Eq(slave.get()));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Ensure the slave gets a status update ACK.
  AWAIT_READY(acknowledgement);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, RecoverResources)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(
      "cpus:2;mem:1024;disk:1024;ports:[1-10, 20-30]");

  Try<PID<Slave>> slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  ExecutorInfo executorInfo;
  executorInfo.MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resources executorResources =
    Resources::parse("cpus:0.3;mem:200;ports:[5-8, 23-25]").get();
  executorInfo.mutable_resources()->MergeFrom(executorResources);

  TaskID taskId;
  taskId.set_value("1");

  Resources taskResources = offers.get()[0].resources() - executorResources;

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(taskResources);
  task.mutable_executor()->MergeFrom(executorInfo);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Scheduler should get an offer for killed task's resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  driver.reviveOffers(); // Don't wait till the next allocation.

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  Offer offer = offers.get()[0];
  EXPECT_EQ(taskResources, offer.resources());

  driver.declineOffer(offer.id());

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Now kill the executor, scheduler should get an offer it's resources.
  containerizer.destroy(offer.framework_id(), executorInfo.executor_id());

  // TODO(benh): We can't do driver.reviveOffers() because we need to
  // wait for the killed executors resources to get aggregated! We
  // should wait for the allocator to recover the resources first. See
  // the allocator tests for inspiration.

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Resources slaveResources = Resources::parse(flags.resources.get()).get();
  EXPECT_EQ(slaveResources, offers.get()[0].resources());

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, FrameworkMessage)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&schedDriver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  schedDriver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<ExecutorDriver*> execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&status));

  schedDriver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  Future<string> execData;
  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(FutureArg<1>(&execData));

  schedDriver.sendFrameworkMessage(
      DEFAULT_EXECUTOR_ID, offers.get()[0].slave_id(), "hello");

  AWAIT_READY(execData);
  EXPECT_EQ("hello", execData.get());

  Future<string> schedData;
  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _, _, _))
    .WillOnce(FutureArg<3>(&schedData));

  execDriver.get()->sendFrameworkMessage("world");

  AWAIT_READY(schedData);
  EXPECT_EQ("world", schedData.get());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  schedDriver.stop();
  schedDriver.join();

  Shutdown();
}


TEST_F(MasterTest, MultipleExecutors)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  ExecutorInfo executor1 = CREATE_EXECUTOR_INFO("executor-1", "exit 1");
  ExecutorInfo executor2 = CREATE_EXECUTOR_INFO("executor-2", "exit 1");

  MockExecutor exec1(executor1.executor_id());
  MockExecutor exec2(executor2.executor_id());

  hashmap<ExecutorID, Executor*> execs;
  execs[executor1.executor_id()] = &exec1;
  execs[executor2.executor_id()] = &exec2;

  TestContainerizer containerizer(execs);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task1.mutable_executor()->MergeFrom(executor1);

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task2.mutable_executor()->MergeFrom(executor2);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  EXPECT_CALL(exec1, registered(_, _, _, _))
    .Times(1);

  Future<TaskInfo> exec1Task;
  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&exec1Task)));

  EXPECT_CALL(exec2, registered(_, _, _, _))
    .Times(1);

  Future<TaskInfo> exec2Task;
  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&exec2Task)));

  Future<TaskStatus> status1, status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(exec1Task);
  EXPECT_EQ(task1.task_id(), exec1Task.get().task_id());

  AWAIT_READY(exec2Task);
  EXPECT_EQ(task2.task_id(), exec2Task.get().task_id());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2.get().state());

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, MasterInfo)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get().address.port, masterInfo.get().port());
  EXPECT_EQ(master.get().address.ip, net::IP(ntohl(masterInfo.get().ip())));

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(MasterTest, MasterInfoOnReElection)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  Try<PID<Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers));

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message);
  AWAIT_READY(resourceOffers);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureArg<1>(&masterInfo));

  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Simulate a spurious event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  detector.appoint(master.get());

  AWAIT_READY(disconnected);

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get().address.port, masterInfo.get().port());
  EXPECT_EQ(master.get().address.ip, net::IP(ntohl(masterInfo.get().ip())));
  EXPECT_EQ(MESOS_VERSION, masterInfo.get().version());

  // The re-registered framework should get offers.
  AWAIT_READY(resourceOffers2);

  driver.stop();
  driver.join();

  Shutdown();
}


class WhitelistTest : public MasterTest
{
protected:
  WhitelistTest()
    : path("whitelist.txt")
  {}

  virtual ~WhitelistTest()
  {
    os::rm(path);
  }
  const string path;
};


TEST_F(WhitelistTest, WhitelistSlave)
{
  // Add some hosts to the white list.
  Try<string> hostname = net::hostname();
  ASSERT_SOME(hostname);

  string hosts = hostname.get() + "\n" + "dummy-slave";
  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  master::Flags flags = CreateMasterFlags();
  flags.whitelist = path;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.hostname = hostname.get();
  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers); // Implies the slave has registered.

  driver.stop();
  driver.join();

  Shutdown();
}


class HostnameTest : public MasterTest {};


TEST_F(HostnameTest, LookupEnabled)
{
  master::Flags flags = CreateMasterFlags();
  EXPECT_TRUE(flags.hostname_lookup);

  Try<PID<Master>> pid = StartMaster(flags);
  ASSERT_SOME(pid);

  Option<Master*> master = cluster.find(pid.get());
  ASSERT_SOME(master);

  EXPECT_EQ(
      pid.get().address.hostname().get(),
      master.get()->info().hostname());

  Shutdown();
}


TEST_F(HostnameTest, LookupDisabled)
{
  master::Flags flags = CreateMasterFlags();
  EXPECT_TRUE(flags.hostname_lookup);
  EXPECT_NONE(flags.hostname);

  flags.hostname_lookup = false;

  Try<PID<Master>> pid = StartMaster(flags);
  ASSERT_SOME(pid);

  Option<Master*> master = cluster.find(pid.get());
  ASSERT_SOME(master);

  EXPECT_EQ(
      stringify(pid.get().address.ip),
      master.get()->info().hostname());

  Shutdown();
}


TEST_F(MasterTest, MasterLost)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get());

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious event at the scheduler.
  detector.appoint(None());

  AWAIT_READY(disconnected);

  driver.stop();
  driver.join();

  Shutdown();
}


// Test ensures two offers from same slave can be used for single task.
// This is done by first launching single task which utilize half of the
// available resources. A subsequent offer for the rest of the available
// resources will be sent by master. The first task is killed and an offer
// for the remaining resources will be sent. Which means two offers covering
// all slave resources and a single task should be able to run on these.
TEST_F(MasterTest, LaunchCombinedOfferTest)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // The CPU granularity is 1.0 which means that we need slaves with at least
  // 2 cpus for a combined offer.
  Resources halfSlave = Resources::parse("cpus:1;mem:512").get();
  Resources fullSlave = halfSlave + halfSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave>> slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Get 1st offer and use half of the slave resources to get subsequent offer.
  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(halfSlave);
  task1.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1));

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  // We want to be notified immediately with new offer.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.launchTasks(offers1.get()[0].id(), {task1}, filters);

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  // Await 2nd offer.
  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  Resources resources2(offers2.get()[0].resources());
  EXPECT_EQ(1, resources2.cpus().get());
  EXPECT_EQ(Megabytes(512), resources2.mem().get());

  Future<TaskStatus> status2;
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  Future<vector<Offer>> offers3;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers3))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Kill 1st task.
  TaskID taskId1 = task1.task_id();
  driver.killTask(taskId1);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_KILLED, status2.get().state());

  // Await 3rd offer - 2nd and 3rd offer to same slave are now ready.
  AWAIT_READY(offers3);
  EXPECT_NE(0u, offers3.get().size());
  Resources resources3(offers3.get()[0].resources());
  EXPECT_EQ(1, resources3.cpus().get());
  EXPECT_EQ(Megabytes(512), resources3.mem().get());

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers2.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(fullSlave);
  task2.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status3;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status3));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers2.get()[0].id());
  combinedOffers.push_back(offers3.get()[0].id());

  driver.launchTasks(combinedOffers, {task2});

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_RUNNING, status3.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test ensures offers for launchTasks cannot span multiple slaves.
TEST_F(MasterTest, LaunchAcrossSlavesTest)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();
  Resources twoSlaves = fullSlave + fullSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave>> slave1 = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  // Test that offers cannot span multiple slaves.
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Create new Flags as we require another work_dir for checkpoints.
  slave::Flags flags2 = CreateSlaveFlags();
  flags2.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave>> slave2 = StartSlave(&containerizer, flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());
  Resources resources2(offers1.get()[0].resources());
  EXPECT_EQ(2, resources2.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources2.mem().get());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(twoSlaves);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers1.get()[0].id());
  combinedOffers.push_back(offers2.get()[0].id());

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  driver.launchTasks(combinedOffers, {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, status.get().reason());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(1u, stats.values.count("master/tasks_lost"));
  EXPECT_EQ(1u, stats.values["master/tasks_lost"]);
  EXPECT_EQ(
      1u,
      stats.values.count(
          "master/task_lost/source_master/reason_invalid_offers"));
  EXPECT_EQ(
      1u,
      stats.values["master/task_lost/source_master/reason_invalid_offers"]);

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test ensures that an offer cannot appear more than once in offers
// for launchTasks.
TEST_F(MasterTest, LaunchDuplicateOfferTest)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave>> slave = StartSlave(&containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Test that same offers cannot be used more than once.
  // Kill 2nd task and get offer for full slave.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Resources resources(offers.get()[0].resources());
  EXPECT_EQ(2, resources.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources.mem().get());

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers.get()[0].id());
  combinedOffers.push_back(offers.get()[0].id());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(fullSlave);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  driver.launchTasks(combinedOffers, {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status.get().state());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, status.get().reason());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(1u, stats.values.count("master/tasks_lost"));
  EXPECT_EQ(1u, stats.values["master/tasks_lost"]);
  EXPECT_EQ(
      1u,
      stats.values.count(
          "master/task_lost/source_master/reason_invalid_offers"));
  EXPECT_EQ(
      1u,
      stats.values["master/task_lost/source_master/reason_invalid_offers"]);

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// TODO(vinod): These tests only verify that the master metrics exist
// but we need tests that verify that these metrics are updated.
TEST_F(MasterTest, MetricsInMetricsEndpoint)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  JSON::Object snapshot = Metrics();

  EXPECT_EQ(1u, snapshot.values.count("master/uptime_secs"));

  EXPECT_EQ(1u, snapshot.values.count("master/elected"));
  EXPECT_EQ(1, snapshot.values["master/elected"]);

  EXPECT_EQ(1u, snapshot.values.count("master/slaves_connected"));
  EXPECT_EQ(1u, snapshot.values.count("master/slaves_disconnected"));
  EXPECT_EQ(1u, snapshot.values.count("master/slaves_active"));
  EXPECT_EQ(1u, snapshot.values.count("master/slaves_inactive"));

  EXPECT_EQ(1u, snapshot.values.count("master/frameworks_connected"));
  EXPECT_EQ(1u, snapshot.values.count("master/frameworks_disconnected"));
  EXPECT_EQ(1u, snapshot.values.count("master/frameworks_active"));
  EXPECT_EQ(1u, snapshot.values.count("master/frameworks_inactive"));

  EXPECT_EQ(1u, snapshot.values.count("master/outstanding_offers"));

  EXPECT_EQ(1u, snapshot.values.count("master/tasks_staging"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_starting"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_running"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_finished"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_failed"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_killed"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_lost"));

  EXPECT_EQ(1u, snapshot.values.count("master/dropped_messages"));

  // Messages from schedulers.
  EXPECT_EQ(1u, snapshot.values.count("master/messages_register_framework"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_reregister_framework"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_unregister_framework"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_deactivate_framework"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_kill_task"));
  EXPECT_EQ(1u, snapshot.values.count(
      "master/messages_status_update_acknowledgement"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_resource_request"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_launch_tasks"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_decline_offers"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_revive_offers"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_suppress_offers"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_reconcile_tasks"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_framework_to_executor"));

  // Messages from executors.
  EXPECT_EQ(1u, snapshot.values.count("master/messages_executor_to_framework"));

  // Messages from slaves.
  EXPECT_EQ(1u, snapshot.values.count("master/messages_register_slave"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_reregister_slave"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_unregister_slave"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_status_update"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_exited_executor"));

  // Messages from both schedulers and slaves.
  EXPECT_EQ(1u, snapshot.values.count("master/messages_authenticate"));

  EXPECT_EQ(1u, snapshot.values.count(
      "master/valid_framework_to_executor_messages"));
  EXPECT_EQ(1u, snapshot.values.count(
      "master/invalid_framework_to_executor_messages"));

  EXPECT_EQ(1u, snapshot.values.count("master/valid_status_updates"));
  EXPECT_EQ(1u, snapshot.values.count("master/invalid_status_updates"));

  EXPECT_EQ(1u, snapshot.values.count(
      "master/valid_status_update_acknowledgements"));
  EXPECT_EQ(1u, snapshot.values.count(
      "master/invalid_status_update_acknowledgements"));

  EXPECT_EQ(1u, snapshot.values.count("master/recovery_slave_removals"));

  EXPECT_EQ(1u, snapshot.values.count("master/event_queue_messages"));
  EXPECT_EQ(1u, snapshot.values.count("master/event_queue_dispatches"));
  EXPECT_EQ(1u, snapshot.values.count("master/event_queue_http_requests"));

  EXPECT_EQ(1u, snapshot.values.count("master/cpus_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/cpus_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/cpus_percent"));

  EXPECT_EQ(1u, snapshot.values.count("master/mem_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/mem_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/mem_percent"));

  EXPECT_EQ(1u, snapshot.values.count("master/disk_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/disk_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/disk_percent"));

  // Registrar Metrics.
  EXPECT_EQ(1u, snapshot.values.count("registrar/queued_operations"));
  EXPECT_EQ(1u, snapshot.values.count("registrar/registry_size_bytes"));

  EXPECT_EQ(1u, snapshot.values.count("registrar/state_fetch_ms"));
  EXPECT_EQ(1u, snapshot.values.count("registrar/state_store_ms"));

  // Allocator Metrics.
  EXPECT_EQ(1u, snapshot.values.count("allocator/event_queue_dispatches"));

  Shutdown();
}


// Ensures that an empty response arrives if information about
// registered slaves is requested from a master where no slaves
// have been registered.
TEST_F(MasterTest, SlavesEndpointWithoutSlaves)
{
  // Start up.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Query the master.
  Future<process::http::Response> response =
    process::http::get(master.get(), "slaves");

  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  const Try<JSON::Value> parse =
    JSON::parse(response.get().body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"slaves\" : []"
      "}");

  ASSERT_SOME(expected);
  EXPECT_SOME_EQ(expected.get(), parse);

  Shutdown();
}


// Ensures that the number of registered slaves resported by
// /master/slaves coincides with the actual number of registered
// slaves.
TEST_F(MasterTest, SlavesEndpointTwoSlaves)
{
  // Start up the master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start a couple of slaves. Their only use is for them to register
  // to the master.
  Future<SlaveRegisteredMessage> slave1RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);
  Try<PID<Slave>> slave1 = StartSlave();

  Future<SlaveRegisteredMessage> slave2RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), Not(slave1.get()));
  StartSlave();

  // Wait for the slaves to be registered.
  AWAIT_READY(slave1RegisteredMessage);
  AWAIT_READY(slave2RegisteredMessage);

  // Query the master.
  Future<process::http::Response> response =
    process::http::get(master.get(), "slaves");

  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  const Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(parse);

  // Check that there are at least two elements in the array.
  Result<JSON::Array> array = parse.get().find<JSON::Array>("slaves");
  ASSERT_SOME(array);
  EXPECT_EQ(2u, array.get().values.size());

  Shutdown();
}


// This test ensures that when a slave is recovered from the registry
// but does not re-register with the master, it is removed from the
// registry and the framework is informed that the slave is lost, and
// the slave is refused re-registration.
TEST_F(MasterTest, RecoveredSlaveDoesNotReregister)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  this->Stop(master.get());

  this->Stop(slave.get());

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  // Step 6: Advance the clock until the re-registration timeout
  // elapses, and expect the slave / task to be lost!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);

  AWAIT_READY(slaveLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);

  Clock::resume();

  // Step 7: Ensure the slave cannot re-register!
  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), master.get(), _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(shutdownMessage);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that a non-strict registry is write-only by
// inducing a slave removal during recovery. After which, we expect
// that the framework is *not* informed, and we expect that the
// slave can re-register successfully.
TEST_F(MasterTest, NonStrictRegistryWriteOnly)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_strict = false;

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  this->Stop(master.get());

  this->Stop(slave.get());

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  AWAIT_READY(registered);

  // Step 6: Advance the clock and make sure the slave is not
  // removed!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);
  Clock::settle();

  ASSERT_TRUE(slaveLost.isPending());

  Clock::resume();

  // Step 7: Now expect the slave to be able to re-register,
  // according to the non-strict semantics.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that slave removals during master recovery
// are rate limited.
TEST_F(MasterTest, RateLimitRecoveredSlaveRemoval)
{
  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Stop the slave while the master is down.
  this->Stop(master.get());
  this->Stop(slave.get());

  shared_ptr<MockRateLimiter> slaveRemovalLimiter(new MockRateLimiter());

  // Return a pending future from the rate limiter.
  Future<Nothing> acquire;
  Promise<Nothing> promise;
  EXPECT_CALL(*slaveRemovalLimiter, acquire())
    .WillOnce(DoAll(FutureSatisfy(&acquire),
                    Return(promise.future())));

  // Restart the master.
  master = StartMaster(slaveRemovalLimiter, masterFlags);
  ASSERT_SOME(master);

  // Start a scheduler to ensure the master would notify
  // a framework about slave removal.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  driver.start();

  AWAIT_READY(registered);

  // Trigger the slave re-registration timeout.
  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);

  // The master should attempt to acquire a permit.
  AWAIT_READY(acquire);

  // The removal should not occur before the permit is satisfied.
  Clock::settle();
  ASSERT_TRUE(slaveLost.isPending());

  // Once the permit is satisfied, the slave should be removed.
  promise.set(Nothing());
  AWAIT_READY(slaveLost);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that slave removals that get scheduled during
// master recovery can be canceled if the slave re-registers.
TEST_F(MasterTest, CancelRecoveredSlaveRemoval)
{
  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Stop the slave while the master is down.
  this->Stop(master.get());
  this->Stop(slave.get());

  shared_ptr<MockRateLimiter> slaveRemovalLimiter(new MockRateLimiter());

  // Return a pending future from the rate limiter.
  Future<Nothing> acquire;
  Promise<Nothing> promise;
  EXPECT_CALL(*slaveRemovalLimiter, acquire())
    .WillOnce(DoAll(FutureSatisfy(&acquire),
                    Return(promise.future())));

  // Restart the master.
  master = StartMaster(slaveRemovalLimiter, masterFlags);
  ASSERT_SOME(master);

  // Start a scheduler to ensure the master would notify
  // a framework about slave removal.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  driver.start();

  AWAIT_READY(registered);

  // Trigger the slave re-registration timeout.
  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);

  // The master should attempt to acquire a permit.
  AWAIT_READY(acquire);

  // The removal should not occur before the permit is satisfied.
  Clock::settle();
  ASSERT_TRUE(slaveLost.isPending());

  // Ignore resource offers from the re-registered slave.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  // Restart the slave.
  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  // Satisfy the rate limit permit. Ensure a removal does not occur!
  promise.set(Nothing());
  Clock::settle();
  ASSERT_TRUE(slaveLost.isPending());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that when a slave is recovered from the registry
// and re-registers with the master, it is *not* removed after the
// re-registration timeout elapses.
TEST_F(MasterTest, RecoveredSlaveReregisters)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  this->Stop(master.get());

  this->Stop(slave.get());

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler to ensure the master would notify
  // a framework, were a slave to be lost.
  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Ignore all offer related calls. The scheduler might receive
  // offerRescinded calls because the slave might re-register due to
  // ping timeout.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(registered);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  // Step 6: Advance the clock and make sure the slave is not
  // removed!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.slave_reregister_timeout);
  Clock::settle();

  ASSERT_TRUE(slaveLost.isPending());

  driver.stop();
  driver.join();

  Shutdown();
}


#ifdef MESOS_HAS_JAVA

class MasterZooKeeperTest : public MesosZooKeeperTest {};

// This test verifies that when the ZooKeeper cluster is lost,
// master, slave & scheduler all get informed.
TEST_F(MasterZooKeeperTest, LostZooKeeperCluster)
{
  ASSERT_SOME(StartMaster());

  Future<process::Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  ASSERT_SOME(StartSlave());

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, stringify(url.get()), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  // Wait for the "registered" messages so that we know the master is
  // detected by everyone.
  AWAIT_READY(frameworkRegisteredMessage);
  AWAIT_READY(slaveRegisteredMessage);

  Future<Nothing> schedulerDisconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&schedulerDisconnected));

  // Need to drop these two dispatches because otherwise the master
  // will EXIT.
  Future<Nothing> masterDetected = DROP_DISPATCH(_, &Master::detected);
  Future<Nothing> lostCandidacy = DROP_DISPATCH(_, &Master::lostCandidacy);

  Future<Nothing> slaveDetected = FUTURE_DISPATCH(_, &Slave::detected);

  server->shutdownNetwork();

  Clock::pause();

  while (schedulerDisconnected.isPending() ||
         masterDetected.isPending() ||
         slaveDetected.isPending() ||
         lostCandidacy.isPending()) {
    Clock::advance(MASTER_CONTENDER_ZK_SESSION_TIMEOUT);
    Clock::settle();
  }

  Clock::resume();

  // Master, slave and scheduler all lose the leading master.
  AWAIT_READY(schedulerDisconnected);
  AWAIT_READY(masterDetected);
  AWAIT_READY(lostCandidacy);
  AWAIT_READY(slaveDetected);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the Address inside MasterInfo
// is populated correctly, during master initialization.
TEST_F(MasterZooKeeperTest, MasterInfoAddress)
{
  Try<PID<Master>> master_ = StartMaster();
  ASSERT_SOME(master_);

  auto master = master_.get();

  ASSERT_SOME(StartSlave());

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master, DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();
  AWAIT_READY(masterInfo);

  const Address& address = masterInfo.get().address();
  EXPECT_EQ(stringify(master.address.ip), address.ip());
  EXPECT_EQ(master.address.port, address.port());

  // Protect from failures on those hosts where
  // hostname cannot be resolved.
  if (master.address.hostname().isSome()) {
    ASSERT_EQ(master.address.hostname().get(), address.hostname());
  }

  driver.stop();
  driver.join();
  Shutdown();
}

#endif // MESOS_HAS_JAVA


// This test ensures that when a master fails over, those tasks that
// belong to some currently unregistered frameworks will appear in the
// "orphan_tasks" field in the state endpoint. And those unregistered frameworks
// will appear in the "unregistered_frameworks" field.
TEST_F(MasterTest, OrphanTasks)
{
  // Start a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  StandaloneMasterDetector detector (master.get());

  // Start a slave.
  Try<PID<Slave>> slave = StartSlave(&exec, &detector);
  ASSERT_SOME(slave);

  // Create a task on the slave.
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId))
    .WillRepeatedly(Return()); // Ignore subsequent events.

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Get the master's state.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  JSON::Object state = parse.get();
  // Record the original framework and task info.
  JSON::Array frameworks =
    state.values["frameworks"].as<JSON::Array>();
  JSON::Object activeFramework =
    frameworks.values.front().as<JSON::Object>();
  JSON::String activeFrameworkId =
    activeFramework.values["id"].as<JSON::String>();
  JSON::Array activeTasks =
    activeFramework.values["tasks"].as<JSON::Array>();
  JSON::Array orphanTasks =
    state.values["orphan_tasks"].as<JSON::Array>();
  JSON::Array unknownFrameworksArray =
    state.values["unregistered_frameworks"].as<JSON::Array>();

  EXPECT_EQ(1u, frameworks.values.size());
  EXPECT_EQ(1u, activeTasks.values.size());
  EXPECT_EQ(0u, orphanTasks.values.size());
  EXPECT_EQ(0u, unknownFrameworksArray.values.size());
  EXPECT_EQ(frameworkId.value(), activeFrameworkId.value);

  EXPECT_CALL(sched, disconnected(&driver))
    .Times(1);

  // Stop the master.
  Stop(master.get());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  // Drop the subscribe call to delay the framework from
  // re-registration.
  // Grab the stuff we need to replay the subscribe call.
  Future<mesos::scheduler::Call> subscribeCall = DROP_CALL(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::SUBSCRIBE,
      _,
      _);

  Clock::pause();

  // The master failover.
  master = StartMaster();
  ASSERT_SOME(master);

  // Settle the clock to ensure the master finishes
  // executing _recover().
  Clock::settle();

  // Simulate a new master detected event to the slave and the framework.
  detector.appoint(master.get());

  AWAIT_READY(slaveReregisteredMessage);
  AWAIT_READY(subscribeCall);

  // Get the master's state.
  response = process::http::get(master.get(), "state");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  // Verify that we have some orphan tasks and unregistered
  // frameworks.
  state = parse.get();
  orphanTasks = state.values["orphan_tasks"].as<JSON::Array>();
  EXPECT_EQ(activeTasks, orphanTasks);

  unknownFrameworksArray =
    state.values["unregistered_frameworks"].as<JSON::Array>();
  EXPECT_EQ(1u, unknownFrameworksArray.values.size());

  JSON::String unknownFrameworkId =
    unknownFrameworksArray.values.front().as<JSON::String>();
  EXPECT_EQ(activeFrameworkId, unknownFrameworkId);

  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    FUTURE_PROTOBUF(FrameworkRegisteredMessage(), _, _);

  // Advance the clock to let the framework re-register with the master.
  Clock::advance(Seconds(1));
  Clock::settle();
  Clock::resume();

  AWAIT_READY(frameworkRegisteredMessage);

  // Get the master's state.
  response = process::http::get(master.get(), "state");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  // Verify the orphan tasks and unregistered frameworks are removed.
  state = parse.get();
  unknownFrameworksArray =
    state.values["unregistered_frameworks"].as<JSON::Array>();
  EXPECT_EQ(0u, unknownFrameworksArray.values.size());

  orphanTasks = state.values["orphan_tasks"].as<JSON::Array>();
  EXPECT_EQ(0u, orphanTasks.values.size());

  // Cleanup.
  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the master will strip ephemeral ports
// resource from offers so that frameworks cannot see it.
TEST_F(MasterTest, IgnoreEphemeralPortsResource)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  string resourcesWithoutEphemeralPorts =
    "cpus:2;mem:1024;disk:1024;ports:[31000-32000]";

  string resourcesWithEphemeralPorts =
    resourcesWithoutEphemeralPorts + ";ephemeral_ports:[30001-30999]";

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = resourcesWithEphemeralPorts;

  Try<PID<Slave>> slave = StartSlave(flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers.get().size());

  EXPECT_EQ(
      Resources(offers.get()[0].resources()),
      Resources::parse(resourcesWithoutEphemeralPorts).get());

  driver.stop();
  driver.join();

  Shutdown();
}


#ifdef WITH_NETWORK_ISOLATOR
TEST_F(MasterTest, MaxExecutorsPerSlave)
{
  master::Flags flags = CreateMasterFlags();
  flags.max_executors_per_slave = 0;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .Times(0);

  driver.start();

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get().address.port, masterInfo.get().port());
  EXPECT_EQ(master.get().address.ip, net::IP(ntohl(masterInfo.get().ip())));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}
#endif  // WITH_NETWORK_ISOLATOR


// This test verifies that when the Framework has not responded to
// an offer within the default timeout, the offer is rescinded.
TEST_F(MasterTest, OfferTimeout)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  masterFlags.offer_timeout = Seconds(30);
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2));

  // Expect offer rescinded.
  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureSatisfy(&offerRescinded));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  driver.start();

  AWAIT_READY(registered);
  AWAIT_READY(offers1);
  ASSERT_EQ(1u, offers1.get().size());

  // Now advance the clock, we need to resume it afterwards to
  // allow the allocator to make a new allocation decision.
  Clock::pause();
  Clock::advance(masterFlags.offer_timeout.get());
  Clock::resume();

  AWAIT_READY(offerRescinded);

  AWAIT_READY(recoverResources);

  // Expect that the resources are re-offered to the framework after
  // the rescind.
  AWAIT_READY(offers2);
  ASSERT_EQ(1u, offers2.get().size());

  EXPECT_EQ(offers1.get()[0].resources(), offers2.get()[0].resources());

  driver.stop();
  driver.join();

  Shutdown();
}


// Offer should not be rescinded if it's accepted.
TEST_F(MasterTest, OfferNotRescindedOnceUsed)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  masterFlags.offer_timeout = Seconds(30);
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

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

  // We don't expect any rescinds if the offer has been accepted.
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(0);

  driver.start();
  AWAIT_READY(registered);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Now advance to the offer timeout, we need to settle the clock to
  // ensure that the offer rescind timeout would be processed
  // if triggered.
  Clock::pause();
  Clock::advance(masterFlags.offer_timeout.get());
  Clock::settle();

  driver.stop();
  driver.join();

  Shutdown();
}


// Offer should not be rescinded if it has been declined.
TEST_F(MasterTest, OfferNotRescindedOnceDeclined)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  masterFlags.offer_timeout = Seconds(30);
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers()); // Decline all offers.

  Future<mesos::scheduler::Call> acceptCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::ACCEPT, _, _);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(0);

  driver.start();
  AWAIT_READY(registered);

  // Wait for the framework to decline the offers.
  AWAIT_READY(acceptCall);

  // Now advance to the offer timeout, we need to settle the clock to
  // ensure that the offer rescind timeout would be processed
  // if triggered.
  Clock::pause();
  Clock::advance(masterFlags.offer_timeout.get());
  Clock::settle();

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that the master releases resources for tasks
// when they terminate, even if no acknowledgements occur.
TEST_F(MasterTest, UnacknowledgedTerminalTask)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:64";
  Try<PID<Slave>> slave = StartSlave(&containerizer, slaveFlags);
  ASSERT_SOME(slave);

  // Launch a framework and get a task into a terminal state.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(FutureArg<1>(&offers1),
                    LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*")))
    .WillOnce(FutureArg<1>(&offers2)); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  // Capture the status update message from the slave to the master.
  Future<StatusUpdateMessage> update =
    FUTURE_PROTOBUF(StatusUpdateMessage(), _, master.get());

  // Drop the status updates forwarded to the framework to ensure
  // that the task remains terminal and unacknowledged in the master.
  DROP_PROTOBUFS(StatusUpdateMessage(), master.get(), _);

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);
  AWAIT_READY(offers1);

  // Once the update is sent, the master should re-offer the
  // resources consumed by the task.
  AWAIT_READY(update);

  // Don't wait around for the allocation interval.
  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  AWAIT_READY(offers2);

  EXPECT_FALSE(offers1.get().empty());
  EXPECT_FALSE(offers2.get().empty());

  // Ensure we get all of the resources back.
  EXPECT_EQ(offers1.get()[0].resources(), offers2.get()[0].resources());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test ensures that the master releases resources for a
// terminated task even when it receives a non-terminal update (with
// latest state set).
TEST_F(MasterTest, ReleaseResourcesForTerminalTaskWithPendingUpdates)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:64";
  Try<PID<Slave>> slave = StartSlave(&containerizer, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop all the updates from master to scheduler.
  DROP_PROTOBUFS(StatusUpdateMessage(), master.get(), _);

  Future<StatusUpdateMessage> statusUpdateMessage =
    FUTURE_PROTOBUF(StatusUpdateMessage(), _, master.get());

  Future<Nothing> __statusUpdate = FUTURE_DISPATCH(_, &Slave::__statusUpdate);

  driver.start();

  // Wait until TASK_RUNNING is sent to the master.
  AWAIT_READY(statusUpdateMessage);

  // Ensure status update manager handles TASK_RUNNING update.
  AWAIT_READY(__statusUpdate);

  Future<Nothing> __statusUpdate2 = FUTURE_DISPATCH(_, &Slave::__statusUpdate);

  // Now send TASK_FINISHED update.
  TaskStatus finishedStatus;
  finishedStatus = statusUpdateMessage.get().update().status();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  // Ensure status update manager handles TASK_FINISHED update.
  AWAIT_READY(__statusUpdate2);

  Future<Nothing> recoverResources = FUTURE_DISPATCH(
      _, &MesosAllocatorProcess::recoverResources);

  // Advance the clock so that the status update manager resends
  // TASK_RUNNING update with 'latest_state' as TASK_FINISHED.
  Clock::pause();
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::resume();

  // Ensure the resources are recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


TEST_F(MasterTest, StateEndpoint)
{
  master::Flags flags = CreateMasterFlags();

  flags.hostname = "localhost";
  flags.cluster = "test-cluster";

  // Capture the start time deterministically.
  Clock::pause();

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Future<process::http::Response> response =
    process::http::get(master.get(), "state");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
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

  ASSERT_TRUE(state.values["start_time"].is<JSON::Number>());
  EXPECT_EQ(
      static_cast<int>(Clock::now().secs()),
      state.values["start_time"].as<JSON::Number>().as<int>());

  ASSERT_TRUE(state.values["id"].is<JSON::String>());
  EXPECT_NE("", state.values["id"].as<JSON::String>().value);

  EXPECT_EQ(stringify(master.get()), state.values["pid"]);
  EXPECT_EQ(flags.hostname.get(), state.values["hostname"]);

  EXPECT_EQ(0, state.values["activated_slaves"]);
  EXPECT_EQ(0, state.values["deactivated_slaves"]);

  EXPECT_EQ(flags.cluster.get(), state.values["cluster"]);

  // TODO(bmahler): Test "leader", "log_dir", "external_log_file".

  // TODO(bmahler): Ensure this contains all the flags.
  ASSERT_TRUE(state.values["flags"].is<JSON::Object>());
  EXPECT_FALSE(state.values["flags"].as<JSON::Object>().values.empty());

  ASSERT_TRUE(state.values["slaves"].is<JSON::Array>());
  EXPECT_TRUE(state.values["slaves"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(state.values["orphan_tasks"].is<JSON::Array>());
  EXPECT_TRUE(state.values["orphan_tasks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());
  EXPECT_TRUE(state.values["frameworks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(
      state.values["completed_frameworks"].is<JSON::Array>());
  EXPECT_TRUE(
      state.values["completed_frameworks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(
      state.values["unregistered_frameworks"].is<JSON::Array>());
  EXPECT_TRUE(
      state.values["unregistered_frameworks"].as<JSON::Array>().values.empty());
}


TEST_F(MasterTest, StateSummaryEndpoint)
{
  master::Flags flags = CreateMasterFlags();

  flags.hostname = "localhost";
  flags.cluster = "test-cluster";

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Future<process::http::Response> response =
    process::http::get(master.get(), "state-summary");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  JSON::Object state = parse.get();

  EXPECT_EQ(flags.hostname.get(), state.values["hostname"]);

  EXPECT_EQ(flags.cluster.get(), state.values["cluster"]);

  ASSERT_TRUE(state.values["slaves"].is<JSON::Array>());
  ASSERT_EQ(1u, state.values["slaves"].as<JSON::Array>().values.size());
  ASSERT_SOME_EQ(0u, state.find<JSON::Number>("slaves[0].TASK_RUNNING"));
  ASSERT_SOME_EQ(1u, state.find<JSON::Number>("slaves[0].TASK_KILLED"));

  ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());
  ASSERT_EQ(1u, state.values["frameworks"].as<JSON::Array>().values.size());
  ASSERT_SOME_EQ(0u, state.find<JSON::Number>("frameworks[0].TASK_RUNNING"));
  ASSERT_SOME_EQ(1u, state.find<JSON::Number>("frameworks[0].TASK_KILLED"));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test ensures that the web UI and capabilities of a framework
// are included in the master's state endpoint, if provided by the
// framework.
TEST_F(MasterTest, FrameworkWebUIUrlandCapabilities)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_webui_url("http://localhost:8080/");
  auto capabilityType = FrameworkInfo::Capability::REVOCABLE_RESOURCES;
  framework.add_capabilities()->set_type(capabilityType);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  Future<process::http::Response> masterState =
    process::http::get(master.get(), "state");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, masterState);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(masterState.get().body);
  ASSERT_SOME(parse);

  // We need a mutable copy of parse to use [].
  JSON::Object masterStateObject = parse.get();

  EXPECT_EQ(1u, masterStateObject.values.count("frameworks"));
  JSON::Array frameworks =
    masterStateObject.values["frameworks"].as<JSON::Array>();

  EXPECT_EQ(1u, frameworks.values.size());
  JSON::Object framework_ = frameworks.values.front().as<JSON::Object>();

  EXPECT_EQ(1u, framework_.values.count("webui_url"));
  JSON::String webui_url =
    framework_.values["webui_url"].as<JSON::String>();

  EXPECT_EQ("http://localhost:8080/", webui_url.value);

  EXPECT_EQ(1u, framework_.values.count("capabilities"));
  JSON::Array capabilities =
    framework_.values["capabilities"].as<JSON::Array>();
  JSON::String capability = capabilities.values.front().as<JSON::String>();

  EXPECT_EQ(FrameworkInfo::Capability::Type_Name(capabilityType),
            capability.value);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that label values are exposed over the master's
// state endpoint.
TEST_F(MasterTest, TaskLabels)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

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

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

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
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(update);

  // Verify label key and value in the master's state endpoint.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::Array> find = parse.get().find<JSON::Array>(
      "frameworks[0].tasks[0].labels");
  EXPECT_SOME(find);

  JSON::Array labelsObject = find.get();

  // Verify the contents of 'foo:bar', 'bar:baz', and 'bar:qux' pairs.
  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("foo", "bar"))),
      labelsObject.values[0]);
  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("bar", "baz"))),
      labelsObject.values[1]);
  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("bar", "qux"))),
      labelsObject.values[2]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that TaskStatus label values are exposed over
// the master's state endpoint.
TEST_F(MasterTest, TaskStatusLabels)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 100", DEFAULT_EXECUTOR_ID);

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
  runningStatus.mutable_task_id()->MergeFrom(execTask.get().task_id());
  runningStatus.set_state(TASK_RUNNING);

  // Add three labels to the task (two of which share the same key).
  Labels* labels = runningStatus.mutable_labels();

  labels->add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels->add_labels()->CopyFrom(createLabel("bar", "baz"));
  labels->add_labels()->CopyFrom(createLabel("bar", "qux"));

  execDriver->sendStatusUpdate(runningStatus);

  AWAIT_READY(status);

  // Verify label key and value in master state.json.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::Array> find = parse.get().find<JSON::Array>(
      "frameworks[0].tasks[0].statuses[0].labels");
  EXPECT_SOME(find);

  JSON::Array labelsObject = find.get();

  // Verify the content of 'foo:bar' pair.
  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("foo", "bar"))),
      labelsObject.values[0]);
  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("bar", "baz"))),
      labelsObject.values[1]);
  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("bar", "qux"))),
      labelsObject.values[2]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test verifies that TaskStatus::container_status is exposed over the
// master state endpoint.
TEST_F(MasterTest, TaskStatusContainerStatus)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task = createTask(offers.get()[0], "sleep 100", DEFAULT_EXECUTOR_ID);

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);

  const string slaveIPAddress = stringify(slave.get().address.ip);

  // Validate that the Slave has passed in its IP address in
  // TaskStatus.container_status.network_infos[0].ip_address.
  EXPECT_TRUE(status.get().has_container_status());
  EXPECT_EQ(1, status.get().container_status().network_infos().size());
  EXPECT_TRUE(
      status.get().container_status().network_infos(0).has_ip_address());
  EXPECT_EQ(
      slaveIPAddress,
      status.get().container_status().network_infos(0).ip_address());

  // Now do the same validation with state endpoint.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state.json");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  // Validate that the IP address passed in by the Slave is available at the
  // state endpoint.
  ASSERT_SOME_EQ(
      slaveIPAddress,
      parse.get().find<JSON::String>(
          "frameworks[0].tasks[0].statuses[0]"
          ".container_status.network_infos[0].ip_address"));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This tests the 'active' field in slave entries from the master's
// state endpoint. We first verify an active slave, deactivate it
// and verify that the 'active' field is false.
TEST_F(MasterTest, SlaveActiveEndpoint)
{
  // Start a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<process::Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Verify slave is active.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state");
  AWAIT_READY(response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::Boolean> status = parse.get().find<JSON::Boolean>(
      "slaves[0].active");

  ASSERT_SOME_EQ(JSON::Boolean(true), status);

  Future<Nothing> deactivateSlave =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::deactivateSlave);

  // Inject a slave exited event at the master causing the master
  // to mark the slave as disconnected.
  process::inject::exited(slaveRegisteredMessage.get().to, master.get());

  // Wait until master deactivates the slave.
  AWAIT_READY(deactivateSlave);

  // Verify slave is inactive.
  response = process::http::get(master.get(), "state");
  AWAIT_READY(response);

  parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  status = parse.get().find<JSON::Boolean>("slaves[0].active");

  ASSERT_SOME_EQ(JSON::Boolean(false), status);

  Shutdown();
}


// This test verifies that service info for tasks is exposed over the
// master's state endpoint.
TEST_F(MasterTest, TaskDiscoveryInfo)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave>> slave = StartSlave(&containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("testtask");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_executor()->CopyFrom(DEFAULT_EXECUTOR_INFO);

  // An expanded service discovery info to the task.
  DiscoveryInfo* info = task.mutable_discovery();
  info->set_visibility(DiscoveryInfo::EXTERNAL);
  info->set_name("mytask");
  info->set_environment("mytest");
  info->set_location("mylocation");
  info->set_version("v0.1.1");

  // Add two named ports to the discovery info.
  Ports* ports = info->mutable_ports();
  Port* port1 = ports->add_ports();
  port1->set_number(8888);
  port1->set_name("myport1");
  port1->set_protocol("tcp");
  Port* port2 = ports->add_ports();
  port2->set_number(9999);
  port2->set_name("myport2");
  port2->set_protocol("udp");

  // Add two labels to the discovery info.
  Labels* labels = info->mutable_labels();
  labels->add_labels()->CopyFrom(createLabel("clearance", "high"));
  labels->add_labels()->CopyFrom(createLabel("RPC", "yes"));

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
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  AWAIT_READY(update);

  // Verify label key and value in the master's state endpoint.
  Future<process::http::Response> response =
    process::http::get(master.get(), "state");
  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::String> taskName = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].name");
  EXPECT_SOME(taskName);
  ASSERT_EQ("testtask", taskName.get());

  // Verify basic content for discovery info.
  Result<JSON::String> visibility = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.visibility");
  EXPECT_SOME(visibility);
  DiscoveryInfo::Visibility visibility_value;
  DiscoveryInfo::Visibility_Parse(visibility.get().value, &visibility_value);
  ASSERT_EQ(DiscoveryInfo::EXTERNAL, visibility_value);

  Result<JSON::String> discoveryName = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.name");
  EXPECT_SOME(discoveryName);
  ASSERT_EQ("mytask", discoveryName.get());

  Result<JSON::String> environment = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.environment");
  EXPECT_SOME(environment);
  ASSERT_EQ("mytest", environment.get());

  Result<JSON::String> location = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.location");
  EXPECT_SOME(location);
  ASSERT_EQ("mylocation", location.get());

  Result<JSON::String> version = parse.get().find<JSON::String>(
      "frameworks[0].tasks[0].discovery.version");
  EXPECT_SOME(version);
  ASSERT_EQ("v0.1.1", version.get());

  // Verify content of two named ports.
  Result<JSON::Array> find1 = parse.get().find<JSON::Array>(
      "frameworks[0].tasks[0].discovery.ports.ports");
  EXPECT_SOME(find1);

  JSON::Array portsArray = find1.get();
  EXPECT_EQ(2u, portsArray.values.size());

  // Verify the content of '8888:myport1:tcp' port.
  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"number\":8888,"
      "  \"name\":\"myport1\","
      "  \"protocol\":\"tcp\""
      "}");
  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), portsArray.values[0]);

  // Verify the content of '9999:myport2:udp' port.
  expected = JSON::parse(
      "{"
      "  \"number\":9999,"
      "  \"name\":\"myport2\","
      "  \"protocol\":\"udp\""
      "}");
  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), portsArray.values[1]);

  // Verify content of two labels.
  Result<JSON::Array> find2 = parse.get().find<JSON::Array>(
      "frameworks[0].tasks[0].discovery.labels.labels");
  EXPECT_SOME(find2);

  JSON::Array labelsArray = find2.get();
  EXPECT_EQ(2u, labelsArray.values.size());

  // Verify the content of 'clearance:high' pair.
  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("clearance", "high"))),
      labelsArray.values[0]);

  // Verify the content of 'RPC:yes' pair.
  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("RPC", "yes"))),
      labelsArray.values[1]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test verifies that a long lived executor works after master
// fail-over. The test launches a task, restarts the master and
// launches another task using the same executor.
TEST_F(MasterTest, MasterFailoverLongLivedExecutor)
{
  // Start master and create detector to inform scheduler and slave
  // about newly elected master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);
  StandaloneMasterDetector detector (master.get());

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  // Compute half of total available resources in order to launch two
  // tasks on the same executor (and thus slave).
  Resources halfSlave = Resources::parse("cpus:1;mem:512").get();
  Resources fullSlave = halfSlave + halfSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Try<PID<Slave>> slave = StartSlave(&containerizer, &detector, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(2);

  EXPECT_CALL(sched, disconnected(&driver));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(halfSlave);
  task1.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Expect two tasks to eventually be running on the executor.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusEq(task1)))
    .WillOnce(FutureArg<1>(&status1))
    .WillRepeatedly(Return());

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  // Fail over master.
  Stop(master.get());

  master = StartMaster();
  ASSERT_SOME(master);

  // Subsequent offers have been ignored until now, set an expectation
  // to get offers from the failed over master.
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  detector.appoint(master.get());

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  // The second task is a just a copy of the first task (using the
  // same executor and resources). We have to set a new task id.
  TaskInfo task2 = task1;
  task2.mutable_task_id()->set_value("2");

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusEq(task2)))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return());

  // Start the second task with the new master on the running executor.
  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// This test ensures that a slave gets a unique SlaveID even after
// master fails over. Please refer to MESOS-3351 for further details.
TEST_F(MasterTest, DuplicatedSlaveIdWhenSlaveReregister)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage1 =
      FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  StandaloneMasterDetector slaveDetector1 (master.get());
  Try<PID<Slave>> slave1 = StartSlave(&slaveDetector1);
  ASSERT_SOME(slave1);

  AWAIT_READY(slaveRegisteredMessage1);

  Stop(master.get());
  master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage2 =
      FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a new slave and make sure it registers before the old slave.
  slave::Flags slaveFlags2 = CreateSlaveFlags();
  Try<PID<Slave>> slave2 = StartSlave(slaveFlags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(slaveRegisteredMessage2);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage1 =
      FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get(), _);

  // Now let the first slave re-register.
  slaveDetector1.appoint(master.get());

  // If both the slaves get the same SlaveID, the re-registration would
  // fail here.
  AWAIT_READY(slaveReregisteredMessage1);

  Shutdown();
}


// This test ensures that if a framework scheduler provides any
// labels in its FrameworkInfo message, those labels are included
// in the master's state endpoint.
TEST_F(MasterTest, FrameworkInfoLabels)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;

  // Add three labels to the FrameworkInfo. Two labels share the same key.
  framework.mutable_labels()->add_labels()->CopyFrom(createLabel("foo", "bar"));
  framework.mutable_labels()->add_labels()->CopyFrom(createLabel("bar", "baz"));
  framework.mutable_labels()->add_labels()->CopyFrom(createLabel("bar", "qux"));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  Future<process::http::Response> response =
    process::http::get(master.get(), "state.json");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(parse);

  Result<JSON::Array> labelsObject = parse.get().find<JSON::Array>(
      "frameworks[0].labels");
  EXPECT_SOME(labelsObject);

  JSON::Array labelsObject_ = labelsObject.get();

  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("foo", "bar"))),
      labelsObject_.values[0]);

  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("bar", "baz"))),
      labelsObject_.values[1]);

  EXPECT_EQ(
      JSON::Value(JSON::Protobuf(createLabel("bar", "qux"))),
      labelsObject_.values[2]);

  driver.stop();
  driver.join();

  Shutdown();
}


// Test the max_completed_frameworks flag for master.
TEST_F(MasterTest, MaxCompletedFrameworksFlag)
{
  // In order to verify that the proper amount of history
  // is maintained, we launch exactly 2 frameworks when
  // 'max_completed_frameworks' is set to 0, 1, and 2. This
  // covers the cases of maintaining no history, some history
  // less than the total number of frameworks launched, and
  // history equal to the total number of frameworks launched.
  const size_t totalFrameworks = 2;
  const size_t maxFrameworksArray[] = {0, 1, 2};

  foreach (const size_t maxFrameworks, maxFrameworksArray) {
    master::Flags masterFlags = CreateMasterFlags();
    masterFlags.max_completed_frameworks = maxFrameworks;

    Try<PID<Master>> master = StartMaster(masterFlags);
    ASSERT_SOME(master);

    Try<PID<Slave>> slave = StartSlave();
    ASSERT_SOME(slave);

    for (size_t i = 0; i < totalFrameworks; i++) {
      MockScheduler sched;
      MesosSchedulerDriver schedDriver(
          &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

      // Ignore any incoming resource offers to the scheduler.
      EXPECT_CALL(sched, resourceOffers(_, _))
        .WillRepeatedly(Return());

      Future<Nothing> schedRegistered;
      EXPECT_CALL(sched, registered(_, _, _))
        .WillOnce(FutureSatisfy(&schedRegistered));

      schedDriver.start();

      AWAIT_READY(schedRegistered);

      schedDriver.stop();
      schedDriver.join();
    }

    Future<process::http::Response> response =
      process::http::get(master.get(), "state");
    AWAIT_READY(response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    // The number of completed frameworks should match the limit.
    Result<JSON::Array> completedFrameworks =
      state.values["completed_frameworks"].as<JSON::Array>();

    EXPECT_EQ(maxFrameworks, completedFrameworks->values.size());

    Stop(slave.get());
    Stop(master.get());
  }
}


// Test the max_completed_tasks_per_framework flag for master.
TEST_F(MasterTest, MaxCompletedTasksPerFrameworkFlag)
{
  // We verify that the proper amount of history is maintained
  // by launching a single framework with exactly 2 tasks. We
  // do this when setting `max_completed_tasks_per_framework`
  // to 0, 1, and 2. This covers the cases of maintaining no
  // history, some history less than the total number of tasks
  // launched, and history equal to the total number of tasks
  // launched.
  const size_t totalTasksPerFramework = 2;
  const size_t maxTasksPerFrameworkArray[] = {0, 1, 2};

  foreach (const size_t maxTasksPerFramework, maxTasksPerFrameworkArray) {
    master::Flags masterFlags = CreateMasterFlags();
    masterFlags.max_completed_tasks_per_framework = maxTasksPerFramework;

    Try<PID<Master>> master = StartMaster(masterFlags);
    ASSERT_SOME(master);

    MockExecutor exec(DEFAULT_EXECUTOR_ID);
    EXPECT_CALL(exec, registered(_, _, _, _));

    Try<PID<Slave>> slave = StartSlave(&exec);
    ASSERT_SOME(slave);

    MockScheduler sched;
    MesosSchedulerDriver schedDriver(
        &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

    Future<Nothing> schedRegistered;
    EXPECT_CALL(sched, registered(_, _, _))
      .WillOnce(FutureSatisfy(&schedRegistered));

    schedDriver.start();

    AWAIT_READY(schedRegistered);

    for (size_t i = 0; i < totalTasksPerFramework; i++) {
      Future<vector<Offer>> offers;
      EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
        .WillOnce(FutureArg<1>(&offers))
        .WillRepeatedly(Return());

      AWAIT_READY(offers);
      EXPECT_NE(0u, offers->size());
      Offer offer = offers.get()[0];

      TaskInfo task;
      task.set_name("");
      task.mutable_task_id()->set_value(stringify(i));
      task.mutable_slave_id()->MergeFrom(offer.slave_id());
      task.mutable_resources()->MergeFrom(offer.resources());
      task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

      // Make sure the task passes through its TASK_FINISHED
      // state properly. We force this state change through
      // the launchTask() callback on our MockExecutor.
      Future<TaskStatus> statusFinished;
      EXPECT_CALL(exec, launchTask(_, _))
        .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));
      EXPECT_CALL(sched, statusUpdate(_, _))
        .WillOnce(FutureArg<1>(&statusFinished));

      schedDriver.launchTasks(offer.id(), {task});

      AWAIT_READY(statusFinished);
      EXPECT_EQ(TASK_FINISHED, statusFinished->state());
    }

    EXPECT_CALL(exec, shutdown(_))
      .Times(AtMost(1));

    schedDriver.stop();
    schedDriver.join();

    Future<process::http::Response> response =
      process::http::get(master.get(), "state");
    AWAIT_READY(response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    // There should be only 1 completed framework.
    Result<JSON::Array> completedFrameworks =
      state.values["completed_frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, completedFrameworks->values.size());

    // The number of completed tasks in the completed framework
    // should match the limit.
    JSON::Object completedFramework =
      completedFrameworks->values[0].as<JSON::Object>();
    Result<JSON::Array> completedTasksPerFramework =
      completedFramework.values["completed_tasks"].as<JSON::Array>();

    EXPECT_EQ(maxTasksPerFramework, completedTasksPerFramework->values.size());

    Stop(slave.get());
    Stop(master.get());
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
