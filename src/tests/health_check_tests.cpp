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

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>

#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::Containerizer;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;

using process::Clock;
using process::Future;
using process::PID;

using testing::_;
using testing::AtMost;
using testing::Eq;
using testing::Return;

using std::vector;
using std::queue;
using std::string;
using std::map;


// Temporarily disabled due to MESOS-1533.
class DISABLED_HealthCheckTest : public MesosTest
{
public:
  vector<TaskInfo> populateTasks(
    const string& cmd,
    const string& healthCmd,
    const Offer& offer,
    const int gracePeriodSeconds = 0,
    const Option<int>& consecutiveFailures = None(),
    const Option<map<string, string> >& env = None())
  {
    TaskInfo task;
    task.set_name("");
    task.mutable_task_id()->set_value("1");
    task.mutable_slave_id()->CopyFrom(offer.slave_id());
    task.mutable_resources()->CopyFrom(offer.resources());

    CommandInfo command;
    command.set_value(cmd);

    task.mutable_command()->CopyFrom(command);

    HealthCheck healthCheck;

    CommandInfo healthCommand;
    healthCommand.set_value(healthCmd);

    if (env.isSome()) {
      foreachpair (const string& name, const string variable, env.get()) {
        Environment_Variable* var =
          healthCommand.mutable_environment()->mutable_variables()->Add();
        var->set_name(name);
        var->set_value(variable);
      }
    }

    healthCheck.mutable_command()->CopyFrom(healthCommand);
    healthCheck.set_delay_seconds(0);
    healthCheck.set_interval_seconds(0);
    healthCheck.set_grace_period_seconds(gracePeriodSeconds);

    if (consecutiveFailures.isSome()) {
      healthCheck.set_consecutive_failures(consecutiveFailures.get());
    }

    task.mutable_health_check()->CopyFrom(healthCheck);

    vector<TaskInfo> tasks;
    tasks.push_back(task);

    return tasks;
  }
};


// Testing a healthy task reporting one healthy status to scheduler.
TEST_F(DISABLED_HealthCheckTest, HealthyTask)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false);
  CHECK_SOME(containerizer);

  Try<PID<Slave> > slave = StartSlave(containerizer.get());
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

  vector<TaskInfo> tasks = populateTasks("sleep 20", "exit 0", offers.get()[0]);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealth;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealth));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealth);
  EXPECT_EQ(TASK_RUNNING, statusHealth.get().state());
  EXPECT_TRUE(statusHealth.get().healthy());

  driver.stop();
  driver.join();

  Shutdown();
}


// Testing killing task after number of consecutive failures.
TEST_F(DISABLED_HealthCheckTest, ConsecutiveFailures)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Try<MesosContainerizer*> containerizer =
  MesosContainerizer::create(flags, false);
  CHECK_SOME(containerizer);

  Try<PID<Slave> > slave = StartSlave(containerizer.get());
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

  vector<TaskInfo> tasks = populateTasks(
    "sleep 20", "exit 1", offers.get()[0], 0, 4);

  // Expecting four unhealthy updates and one final kill update.
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  Future<TaskStatus> status3;
  Future<TaskStatus> status4;
  Future<TaskStatus> statusKilled;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1.get().state());
  EXPECT_FALSE(status1.get().healthy());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2.get().state());
  EXPECT_FALSE(status2.get().healthy());

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_RUNNING, status3.get().state());
  EXPECT_FALSE(status3.get().healthy());

  AWAIT_READY(status4);
  EXPECT_EQ(TASK_RUNNING, status4.get().state());
  EXPECT_FALSE(status4.get().healthy());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled.get().state());
  EXPECT_FALSE(statusKilled.get().healthy());

  driver.stop();
  driver.join();

  Shutdown();
}


// Testing command using environment variable.
TEST_F(DISABLED_HealthCheckTest, EnvironmentSetup)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false);

  CHECK_SOME(containerizer);

  Try<PID<Slave> > slave = StartSlave(containerizer.get());
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

  map<string, string> env;
  env["STATUS"] = "0";

  vector<TaskInfo> tasks = populateTasks(
    "sleep 20", "exit $STATUS", offers.get()[0], 0, None(), env);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealth;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
  .WillOnce(FutureArg<1>(&statusRunning))
  .WillOnce(FutureArg<1>(&statusHealth));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealth);
  EXPECT_EQ(TASK_RUNNING, statusHealth.get().state());
  EXPECT_TRUE(statusHealth.get().healthy());

  driver.stop();
  driver.join();

  Shutdown();
}


// Testing grace period that ignores all failed task failures.
TEST_F(DISABLED_HealthCheckTest, GracePeriod)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Try<MesosContainerizer*> containerizer =
  MesosContainerizer::create(flags, false);
  CHECK_SOME(containerizer);

  Try<PID<Slave> > slave = StartSlave(containerizer.get());
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

  vector<TaskInfo> tasks = populateTasks(
    "sleep 20", "exit 1", offers.get()[0], 6);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealth;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealth))
    .WillRepeatedly(Return());

  driver.launchTasks(offers.get()[0].id(), tasks);

  Clock::pause();
  EXPECT_TRUE(statusHealth.isPending());

  // No task unhealthy update should be called in grace period.
  Clock::advance(Seconds(5));
  EXPECT_TRUE(statusHealth.isPending());

  Clock::advance(Seconds(1));
  Clock::settle();
  Clock::resume();

  AWAIT_READY(statusHealth);
  EXPECT_EQ(TASK_RUNNING, statusHealth.get().state());
  EXPECT_FALSE(statusHealth.get().healthy());

  driver.stop();
  driver.join();

  Shutdown();
}
