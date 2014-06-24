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
#include <gtest/gtest.h>

#include <process/future.hpp>

#include "tests/mesos.hpp"

#include "slave/slave.hpp"
#include "slave/containerizer/docker.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Future;
using process::PID;

using std::vector;
using std::list;

using testing::_;
using testing::Eq;
using testing::Return;

class DockerContainerizerTest : public MesosTest {};

TEST_F(DockerContainerizerTest, DOCKER_Launch) {
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation.clear();
  flags.containerizers = "docker";

  Try<PID<Slave> > slave = StartSlave(flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;

  CommandInfo::ContainerInfo* containerInfo =
    task.mutable_command()->mutable_container();

  containerInfo->set_image("docker://busybox");

  command.set_value("sleep 30");

  task.mutable_command()->CopyFrom(command);

  Future<TaskStatus> statusRunning;

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  ASSERT_EQ(TASK_RUNNING, statusRunning.get().state());

  Docker docker("docker");
  Future<list<Docker::Container> > containers = docker.ps();

  AWAIT_READY(containers);

  ASSERT_TRUE(containers.get().size() > 0);

  driver.stop();
  driver.join();

  Shutdown();
}
