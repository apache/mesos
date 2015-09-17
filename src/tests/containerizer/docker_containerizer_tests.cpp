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
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>

#include "linux/cgroups.hpp"

#include "messages/messages.hpp"

#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using namespace mesos::internal::slave::paths;
using namespace mesos::internal::slave::state;

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::DockerContainerizerProcess;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Slave;

using process::Future;
using process::Message;
using process::PID;
using process::UPID;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::Invoke;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


class DockerContainerizerTest : public MesosTest
{
public:
  static string containerName(
      const SlaveID& slaveId,
      const ContainerID& containerId)
  {
    return slave::DOCKER_NAME_PREFIX + slaveId.value() +
      slave::DOCKER_NAME_SEPERATOR + containerId.value();
  }

  enum ContainerState
  {
    EXISTS,
    RUNNING
  };

  static bool exists(
      const process::Shared<Docker>& docker,
      const SlaveID& slaveId,
      const ContainerID& containerId,
      ContainerState state = ContainerState::EXISTS)
  {
    Duration waited = Duration::zero();
    string expectedName = containerName(slaveId, containerId);

    do {
      Future<Docker::Container> inspect = docker->inspect(expectedName);

      if (!inspect.await(Seconds(3))) {
        return false;
      }

      if (inspect.isReady()) {
        switch (state) {
          case ContainerState::RUNNING:
            if (inspect.get().pid.isSome()) {
              return true;
            }
            // Retry looking for running pid until timeout.
            break;
          case ContainerState::EXISTS:
            return true;
        }
      }

      os::sleep(Milliseconds(200));
      waited += Milliseconds(200);
    } while (waited < Seconds(5));

    return false;
  }

  static bool containsLine(
    const vector<string>& lines,
    const string& expectedLine)
  {
    foreach (const string& line, lines) {
      if (line == expectedLine) {
        return true;
      }
    }

    return false;
  }

  virtual void TearDown()
  {
    Try<Docker*> docker =
      Docker::create(tests::flags.docker, tests::flags.docker_socket,
      false);

    ASSERT_SOME(docker);
    Future<list<Docker::Container>> containers =
      docker.get()->ps(true, slave::DOCKER_NAME_PREFIX);

    AWAIT_READY(containers);

    // Cleanup all mesos launched containers.
    foreach (const Docker::Container& container, containers.get()) {
      AWAIT_READY_FOR(docker.get()->rm(container.id, true), Seconds(30));
    }

    delete docker.get();
  }
};


// Only enable executor launch on linux as other platforms
// requires running linux VM and need special port forwarding
// to get host networking to work.
#ifdef __linux__
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch_Executor)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("/bin/test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("tnachen/test-executor");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launchExecutor)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  Shutdown();
}


// This test verifies that a custom executor can be launched and
// registered with the slave with docker bridge network enabled.
// We're assuming that the custom executor is registering it's public
// ip instead of 0.0.0.0 or equivelent to the slave as that's the
// default behavior for libprocess.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch_Executor_Bridged)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("/bin/test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("tnachen/test-executor");
  dockerInfo.set_network(ContainerInfo::DockerInfo::BRIDGE);

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launchExecutor)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  Shutdown();
}
#endif // __linux__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  ASSERT_TRUE(statusRunning.get().has_data());

  Try<JSON::Array> parse = JSON::parse<JSON::Array>(statusRunning.get().data());
  ASSERT_SOME(parse);

  // Now verify that the Docker.NetworkSettings.IPAddress label is
  // present.
  // TODO(karya): Deprecated -- Remove after 0.25.0 has shipped.
  ASSERT_TRUE(statusRunning.get().has_labels());
  EXPECT_EQ(1, statusRunning.get().labels().labels().size());
  EXPECT_EQ("Docker.NetworkSettings.IPAddress",
            statusRunning.get().labels().labels(0).key());

  // Now verify that the TaskStatus contains the container IP address.
  ASSERT_TRUE(statusRunning.get().has_container_status());
  EXPECT_EQ(1, statusRunning.get().container_status().network_infos().size());
  EXPECT_TRUE(
      statusRunning.get().container_status().network_infos(0).has_ip_address());

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  Shutdown();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Kill)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  ASSERT_TRUE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled.get().state());

  AWAIT_READY(termination);

  ASSERT_FALSE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test tests DockerContainerizer::usage().
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Usage)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>("cpus:2;mem:1024");

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  // Run a CPU intensive command, so we can measure utime and stime later.
  command.set_value("dd if=/dev/zero of=/dev/null");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  // We ignore all update calls to prevent resizing cgroup limits.
  EXPECT_CALL(dockerContainerizer, update(_, _))
    .WillRepeatedly(Return(Nothing()));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  // Verify the usage.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage =
      dockerContainerizer.usage(containerId.get());
    // TODO(tnachen): Replace await with AWAIT_COMPLETED once
    // implemented.
    ASSERT_TRUE(usage.await(Seconds(3)));

    if (usage.isReady()) {
      statistics = usage.get();

      if (statistics.cpus_user_time_secs() > 0 &&
          statistics.cpus_system_time_secs() > 0) {
        break;
      }
    }

    os::sleep(Milliseconds(200));
    waited += Milliseconds(200);
  } while (waited < Seconds(3));

  // Usage includes the executor resources.
  EXPECT_EQ(2.0 + slave::DEFAULT_EXECUTOR_CPUS, statistics.cpus_limit());
  EXPECT_EQ((Gigabytes(1) + slave::DEFAULT_EXECUTOR_MEM).bytes(),
            statistics.mem_limit_bytes());
  EXPECT_LT(0, statistics.cpus_user_time_secs());
  EXPECT_LT(0, statistics.cpus_system_time_secs());
  EXPECT_GT(statistics.mem_rss_bytes(), 0u);

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  dockerContainerizer.destroy(containerId.get());

  AWAIT_READY(termination);

  // Usage() should fail again since the container is destroyed.
  Future<ResourceStatistics> usage =
    dockerContainerizer.usage(containerId.get());

  AWAIT_FAILED(usage);

  driver.stop();
  driver.join();

  Shutdown();
}


#ifdef __linux__
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Update)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  ASSERT_TRUE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  string name = containerName(slaveId, containerId.get());

  Future<Docker::Container> inspect = docker->inspect(name);

  AWAIT_READY(inspect);

  Try<Resources> newResources = Resources::parse("cpus:1;mem:128");

  ASSERT_SOME(newResources);

  Future<Nothing> update =
    dockerContainerizer.update(containerId.get(), newResources.get());

  AWAIT_READY(update);

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  Result<string> memoryHierarchy = cgroups::hierarchy("memory");

  ASSERT_SOME(cpuHierarchy);
  ASSERT_SOME(memoryHierarchy);

  Option<pid_t> pid = inspect.get().pid;
  ASSERT_SOME(pid);

  Result<string> cpuCgroup = cgroups::cpu::cgroup(pid.get());
  ASSERT_SOME(cpuCgroup);

  Result<string> memoryCgroup = cgroups::memory::cgroup(pid.get());
  ASSERT_SOME(memoryCgroup);

  Try<uint64_t> cpu = cgroups::cpu::shares(
      cpuHierarchy.get(),
      cpuCgroup.get());

  ASSERT_SOME(cpu);

  Try<Bytes> mem = cgroups::memory::soft_limit_in_bytes(
      memoryHierarchy.get(),
      memoryCgroup.get());

  ASSERT_SOME(mem);

  EXPECT_EQ(1024u, cpu.get());
  EXPECT_EQ(128u, mem.get().megabytes());

  newResources = Resources::parse("cpus:1;mem:144");

  // Issue second update that uses the cached pid instead of inspect.
  update = dockerContainerizer.update(containerId.get(), newResources.get());

  AWAIT_READY(update);

  cpu = cgroups::cpu::shares(cpuHierarchy.get(), cpuCgroup.get());

  ASSERT_SOME(cpu);

  mem = cgroups::memory::soft_limit_in_bytes(
      memoryHierarchy.get(),
      memoryCgroup.get());

  ASSERT_SOME(mem);

  EXPECT_EQ(1024u, cpu.get());
  EXPECT_EQ(144u, mem.get().megabytes());

  driver.stop();
  driver.join();

  Shutdown();
}
#endif //__linux__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Recover)
{
  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Future<string> stoppedContainer;
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillOnce(DoAll(FutureArg<0>(&stoppedContainer),
                    Return(Nothing())));

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  SlaveID slaveId;
  slaveId.set_value("s1");
  ContainerID containerId;
  containerId.set_value("c1");
  ContainerID reapedContainerId;
  reapedContainerId.set_value("c2");

  string container1 = containerName(slaveId, containerId);
  string container2 = containerName(slaveId, reapedContainerId);

  // Clean up artifacts if containers still exists.
  ASSERT_TRUE(docker->rm(container1, true).await(Seconds(30)));
  ASSERT_TRUE(docker->rm(container2, true).await(Seconds(30)));

  Resources resources = Resources::parse("cpus:1;mem:512").get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_value("sleep 1000");

  Future<Nothing> d1 =
    docker->run(
        containerInfo,
        commandInfo,
        container1,
        flags.work_dir,
        flags.sandbox_directory,
        resources);

  Future<Nothing> d2 =
    docker->run(
        containerInfo,
        commandInfo,
        container2,
        flags.work_dir,
        flags.sandbox_directory,
        resources);

  ASSERT_TRUE(
    exists(docker, slaveId, containerId, ContainerState::RUNNING));
  ASSERT_TRUE(
    exists(docker, slaveId, reapedContainerId, ContainerState::RUNNING));

  Future<Docker::Container> inspect = docker->inspect(container2);
  AWAIT_READY(inspect);

  SlaveState slaveState;
  slaveState.id = slaveId;
  FrameworkState frameworkState;

  ExecutorID execId;
  execId.set_value("e1");

  ExecutorState execState;
  ExecutorInfo execInfo;
  execState.info = execInfo;
  execState.latest = containerId;

  Try<process::Subprocess> wait =
    process::subprocess(tests::flags.docker + " wait " + container1);

  ASSERT_SOME(wait);

  FrameworkID frameworkId;

  RunState runState;
  runState.id = containerId;
  runState.forkedPid = wait.get().pid();
  execState.runs.put(containerId, runState);
  frameworkState.executors.put(execId, execState);

  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);

  AWAIT_READY(recover);

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId);

  ASSERT_FALSE(termination.isFailed());

  AWAIT_FAILED(dockerContainerizer.wait(reapedContainerId));

  AWAIT_EQ(inspect.get().id, stoppedContainer);

  Shutdown();
}


// This test checks the docker containerizer doesn't recover executors
// that were started by another containerizer (e.g: mesos).
TEST_F(DockerContainerizerTest, ROOT_DOCKER_SkipRecoverNonDocker)
{
  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  ContainerID containerId;
  containerId.set_value("c1");
  ContainerID reapedContainerId;
  reapedContainerId.set_value("c2");

  ExecutorID executorId;
  executorId.set_value(UUID::random().toString());

  ExecutorInfo executorInfo;
  executorInfo.mutable_container()->set_type(ContainerInfo::MESOS);

  ExecutorState executorState;
  executorState.info = executorInfo;
  executorState.latest = containerId;

  RunState runState;
  runState.id = containerId;
  executorState.runs.put(containerId, runState);

  FrameworkState frameworkState;
  frameworkState.executors.put(executorId, executorState);

  SlaveState slaveState;
  FrameworkID frameworkId;
  frameworkId.set_value(UUID::random().toString());
  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);
  AWAIT_READY(recover);

  Future<hashset<ContainerID>> containers = dockerContainerizer.containers();
  AWAIT_READY(containers);

  // A MesosContainerizer task shouldn't be recovered by
  // DockerContainerizer.
  EXPECT_EQ(0u, containers.get().size());
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Logs)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  string uuid = UUID::random().toString();

  CommandInfo command;
  command.set_value("echo out" + uuid + " ; echo err" + uuid + " 1>&2");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // Now check that the proper output is in stderr and stdout (which
  // might also contain other things, hence the use of a UUID).
  Try<string> read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  EXPECT_TRUE(containsLine(lines, "err" + uuid));
  EXPECT_FALSE(containsLine(lines, "out" + uuid));

  read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_TRUE(containsLine(lines, "out" + uuid));
  EXPECT_FALSE(containsLine(lines, "err" + uuid));

  driver.stop();
  driver.join();

  Shutdown();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_shell(false);

  // NOTE: By not setting CommandInfo::value we're testing that we
  // will still be able to run the container because it has a default
  // entrypoint!

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/inky");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  Try<string> read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  // Since we're not passing any command value, we're expecting the
  // default entry point to be run which is 'echo' with the default
  // command from the image which is 'inky'.
  EXPECT_TRUE(containsLine(lines, "inky"));

  read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_FALSE(containsLine(lines, "inky"));

  driver.stop();
  driver.join();

  Shutdown();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD_Override)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  // We skip stopping the docker container because stopping  a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  string uuid = UUID::random().toString();

  CommandInfo command;
  command.set_shell(false);

  // We can set the value to just the 'uuid' since it should get
  // passed as an argument to the entrypoint, i.e., 'echo uuid'.
  command.set_value(uuid);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/inky");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // Now check that the proper output is in stderr and stdout.
  Try<string> read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  // We expect the passed in command value to override the image's
  // default command, thus we should see the value of 'uuid' in the
  // output instead of the default command which is 'inky'.
  EXPECT_TRUE(containsLine(lines, uuid));
  EXPECT_FALSE(containsLine(lines, "inky"));

  read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_FALSE(containsLine(lines, "inky"));
  EXPECT_FALSE(containsLine(lines, uuid));

  driver.stop();
  driver.join();

  Shutdown();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD_Args)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  string uuid = UUID::random().toString();

  CommandInfo command;
  command.set_shell(false);

  // We should also be able to skip setting the comamnd value and just
  // set the arguments and those should also get passed through to the
  // entrypoint!
  command.add_arguments(uuid);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/inky");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // Now check that the proper output is in stderr and stdout.
  Try<string> read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  // We expect the passed in command arguments to override the image's
  // default command, thus we should see the value of 'uuid' in the
  // output instead of the default command which is 'inky'.
  EXPECT_TRUE(containsLine(lines, uuid));
  EXPECT_FALSE(containsLine(lines, "inky"));

  read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_FALSE(containsLine(lines, "inky"));
  EXPECT_FALSE(containsLine(lines, uuid));

  driver.stop();
  driver.join();

  Shutdown();
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up we make sure the executor
// re-registers and the slave properly sends the update.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_SlaveRecoveryTaskContainer)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  // We put the containerizer on the heap so we can more easily
  // control it's lifetime, i.e., when we invoke the destructor.
  MockDockerContainerizer* dockerContainerizer1 =
    new MockDockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave1 = StartSlave(dockerContainerizer1, flags);
  ASSERT_SOME(slave1);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(*dockerContainerizer1, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(dockerContainerizer1,
                           &MockDockerContainerizer::_launch)));

  // Drop the first update from the executor.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  // Stop the slave before the status update is received.
  AWAIT_READY(statusUpdateMessage);

  Stop(slave1.get());

  delete dockerContainerizer1;

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  MockDockerContainerizer* dockerContainerizer2 =
    new MockDockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave2 = StartSlave(dockerContainerizer2, flags);
  ASSERT_SOME(slave2);

  // Ensure the executor re-registers.
  AWAIT_READY(reregisterExecutorMessage);
  UPID executorPid = reregisterExecutorMessage.get().from;

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage.get().body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_RUNNING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer2->wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  Shutdown();

  delete dockerContainerizer2;
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up we make sure the executor
// re-registers and the slave properly sends the update.
//
// TODO(benh): This test is currently disabled because the executor
// inside the image mesosphere/test-executor does not properly set the
// executor PID that is uses during registration, so when the new
// slave recovers it can't reconnect and instead destroys that
// container. In particular, it uses '0' for it's IP which we properly
// parse and can even properly use for sending other messages, but the
// current implementation of 'UPID::operator bool()' fails if the IP
// component of a PID is '0'.
TEST_F(DockerContainerizerTest,
       DISABLED_ROOT_DOCKER_SlaveRecoveryExecutorContainer)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  MockDockerContainerizer* dockerContainerizer1 =
    new MockDockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave1 = StartSlave(dockerContainerizer1, flags);
  ASSERT_SOME(slave1);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/test-executor");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  Future<ContainerID> containerId;
  Future<SlaveID> slaveId;
  EXPECT_CALL(*dockerContainerizer1, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<4>(&slaveId),
                    Invoke(dockerContainerizer1,
                           &MockDockerContainerizer::_launchExecutor)));

  // We need to wait until the container's pid has been been
  // checkpointed so that when the next slave recovers it won't treat
  // the executor as having gone lost! We know this has completed
  // after Containerizer::launch returns and the
  // Slave::executorLaunched gets dispatched.
  Future<Nothing> executorLaunched =
    FUTURE_DISPATCH(_, &Slave::executorLaunched);

  // The test-executor in the image immediately sends a TASK_RUNNING
  // followed by TASK_FINISHED (no sleep/delay in between) so we need
  // to drop the first TWO updates that come from the executor rather
  // than only the first update like above where we can control how
  // the length of the task.
  Future<StatusUpdateMessage> statusUpdateMessage1 =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  // Drop the first update from the executor.
  Future<StatusUpdateMessage> statusUpdateMessage2 =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);
  AWAIT_READY(slaveId);

  AWAIT_READY(executorLaunched);
  AWAIT_READY(statusUpdateMessage1);
  AWAIT_READY(statusUpdateMessage2);

  Stop(slave1.get());

  delete dockerContainerizer1;

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  MockDockerContainerizer* dockerContainerizer2 =
    new MockDockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave2 = StartSlave(dockerContainerizer2, flags);
  ASSERT_SOME(slave2);

  // Ensure the executor re-registers.
  AWAIT_READY(reregisterExecutorMessage);
  UPID executorPid = reregisterExecutorMessage.get().from;

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage.get().body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_RUNNING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  ASSERT_TRUE(exists(docker, slaveId.get(), containerId.get()));

  driver.stop();
  driver.join();

  delete dockerContainerizer2;
}


// This test verifies that port mapping with bridge network is
// exposing the host port to the container port, by sending data
// to the host port and receiving it in the container by listening
// to the mapped container port.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_NC_PortMapping)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  flags.resources = "cpus:1;mem:1024;ports:[10000-10000]";

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_shell(false);
  command.set_value("nc");
  command.add_arguments("-l");
  command.add_arguments("-p");
  command.add_arguments("1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  dockerInfo.set_network(ContainerInfo::DockerInfo::BRIDGE);

  ContainerInfo::DockerInfo::PortMapping portMapping;
  portMapping.set_host_port(10000);
  portMapping.set_container_port(1000);

  dockerInfo.add_port_mappings()->CopyFrom(portMapping);
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  ASSERT_TRUE(
    exists(docker, slaveId, containerId.get(), ContainerState::RUNNING));

  string uuid = UUID::random().toString();

  // Write uuid to docker mapped host port.
  Try<process::Subprocess> s = process::subprocess(
      "echo " + uuid + " | nc localhost 10000");

  ASSERT_SOME(s);
  AWAIT_READY_FOR(s.get().status(), Seconds(60));

  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // Now check that the proper output is in stdout.
  Try<string> read = os::read(path::join(directory.get(), "stdout"));
  ASSERT_SOME(read);

  const vector<string> lines = strings::split(read.get(), "\n");

  // We expect the uuid that is sent to host port to be written
  // to stdout by the docker container running nc -l.
  EXPECT_TRUE(containsLine(lines, uuid));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  Shutdown();
}


// This test verifies that sandbox with ':' in the path can still
// run successfully. This a limitation of the Docker CLI where
// the volume map parameter treats colons (:) as seperators,
// and incorrectly seperates the sandbox directory.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_LaunchSandboxWithColon)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  MockDockerContainerizer dockerContainerizer(flags, &fetcher, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  SlaveID slaveId = offer.slave_id();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("test:colon");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  ASSERT_TRUE(exists(docker, slaveId, containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  Shutdown();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_DestroyWhileFetching)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(flags, &fetcher, docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Promise<Nothing> promise;
  Future<Nothing> fetch;

  // We want to pause the fetch call to simulate a long fetch time.
  EXPECT_CALL(*process, fetch(_, _))
    .WillOnce(DoAll(FutureSatisfy(&fetch),
                    Return(promise.future())));

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(fetch);

  dockerContainerizer.destroy(containerId.get());

  // Resume docker launch.
  promise.set(Nothing());

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_DestroyWhilePulling)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(flags, &fetcher, docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Future<Nothing> fetch;
  EXPECT_CALL(*process, fetch(_, _))
    .WillOnce(DoAll(FutureSatisfy(&fetch),
                    Return(Nothing())));

  Promise<Nothing> promise;

  // We want to pause the fetch call to simulate a long fetch time.
  EXPECT_CALL(*process, pull(_))
    .WillOnce(Return(promise.future()));

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer);
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

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  // Wait until fetch is finished.
  AWAIT_READY(fetch);

  dockerContainerizer.destroy(containerId.get());

  // Resume docker launch.
  promise.set(Nothing());

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test checks that when a docker containerizer update failed
// and the container failed before the executor started, the executor
// is properly killed and cleaned up.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_ExecutorCleanupWhenLaunchFailed)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(flags, &fetcher, docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Try<PID<Slave>> slave = StartSlave(&dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("ls");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  // Fail the update so we don't proceed to send run task to the executor.
  EXPECT_CALL(dockerContainerizer, update(_, _))
    .WillRepeatedly(Return(Failure("Fail resource update")));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());

  driver.stop();
  driver.join();

  // We expect the executor to have exited, and if not in Shutdown
  // the test will fail because of the executor process still running.
  Shutdown();
}


// When the fetch fails we should send the scheduler a status
// update with message the shows the actual error.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_FetchFailure)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(flags, &fetcher, docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Try<PID<Slave>> slave = StartSlave(&dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("ls");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  EXPECT_CALL(*process, fetch(_, _))
    .WillOnce(Return(Failure("some error from fetch")));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());
  EXPECT_EQ("Failed to launch container: some error from fetch",
             statusFailed.get().message());

  // TODO(jaybuff): When MESOS-2035 is addressed we should validate
  // that statusFailed.get().reason() is correctly set here.

  driver.stop();
  driver.join();

  // We expect the executor to have exited, and if not in Shutdown
  // the test will fail because of the executor process still running.
  Shutdown();
}


// When the docker pull fails we should send the scheduler a status
// update with message the shows the actual error.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_DockerPullFailure)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher;

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(flags, &fetcher, docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Try<PID<Slave>> slave = StartSlave(&dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("ls");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  EXPECT_CALL(*mockDocker, pull(_, _, _))
    .WillOnce(Return(Failure("some error from docker pull")));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());
  EXPECT_EQ("Failed to launch container: some error from docker pull",
             statusFailed.get().message());

  // TODO(jaybuff): When MESOS-2035 is addressed we should validate
  // that statusFailed.get().reason() is correctly set here.

  driver.stop();
  driver.join();

  // We expect the executor to have exited, and if not in Shutdown
  // the test will fail because of the executor process still running.
  Shutdown();
}


// When the docker executor container fails to launch, docker inspect
// future that is in a retry loop should be discarded.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_DockerInspectDiscard)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Future<Docker::Container> inspect;
  EXPECT_CALL(*mockDocker, inspect(_, _))
    .WillOnce(FutureResult(&inspect,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_inspect)));

  EXPECT_CALL(*mockDocker, run(_, _, _, _, _, _, _, _, _))
    .WillOnce(Return(Failure("Run failed")));

  Fetcher fetcher;

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(flags, &fetcher, docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Try<PID<Slave>> slave = StartSlave(&dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("/bin/test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("tnachen/test-executor");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launchExecutor)));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);
  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());

  AWAIT_DISCARDED(inspect);

  driver.stop();
  driver.join();

  // We expect the inspect to have exited, and if not in Shutdown
  // the test will fail because of the inspect process still running.
  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
