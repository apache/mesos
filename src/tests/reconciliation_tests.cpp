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

#include <stdint.h>
#include <unistd.h>

#include <gmock/gmock.h>

#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Future;
using process::PID;

using std::vector;

using testing::_;
using testing::AtMost;
using testing::Return;


class ReconciliationTest : public MesosTest {};


// Test sends different state than current and expects an update with
// the current state of task.
//
// TODO(nnielsen): Stubs have been left for future test, where test sends
// expected state of non-existing task and an update with TASK_LOST should
// be received. Also (not currently covered) if statuses are up to date,
// nothing should happen.
TEST_F(ReconciliationTest, TaskStateMismatch)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  TestContainerizer containerizer(&exec);

  Try<PID<Slave> > slave = StartSlave(&containerizer);
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

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  EXPECT_EQ(true, status.get().has_slave_id());

  const TaskID taskId = status.get().task_id();
  const SlaveID slaveId = status.get().slave_id();

  // If framework has different state, current state should be reported.
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  vector<TaskStatus> statuses;

  TaskStatus differentStatus;
  differentStatus.mutable_task_id()->CopyFrom(taskId);
  differentStatus.mutable_slave_id()->CopyFrom(slaveId);
  differentStatus.set_state(TASK_KILLED);

  statuses.push_back(differentStatus);

  driver.reconcileTasks(statuses);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2.get().state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}
