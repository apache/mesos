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

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <mesos/slave/container_logger.hpp>

#include <mesos/v1/mesos.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>
#include <stout/uuid.hpp>

#ifdef __linux__
#include "linux/cgroups.hpp"
#include "linux/fs.hpp"
#endif // __linux__

#include "messages/messages.hpp"

#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"

#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_docker.hpp"

#include "tests/containerizer/docker_common.hpp"

using namespace mesos::internal::slave::paths;
using namespace mesos::internal::slave::state;

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::DockerContainerizerProcess;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLogger;
using mesos::slave::ContainerTermination;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::Invoke;
using testing::Return;

namespace process {

// We need to reinitialize libprocess in order to test against
// different configurations, such as when libprocess is initialized
// with SSL or IPv6 enabled.
void reinitialize(
    const Option<string>& delegate,
    const Option<string>& readonlyAuthenticationRealm,
    const Option<string>& readwriteAuthenticationRealm);

} // namespace process {


namespace mesos {
namespace internal {
namespace tests {

#ifdef __WINDOWS__
static constexpr char DOCKER_INKY_IMAGE[] = "akagup/inky";
#else
static constexpr char DOCKER_INKY_IMAGE[] = "mesosphere/inky";
#endif // __WINDOWS__


static
ContainerInfo createDockerInfo(const string& imageName)
{
  ContainerInfo containerInfo;

  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image(imageName);

  return containerInfo;
}


class DockerContainerizerTest : public MesosTest
{
public:
  static string containerName(const ContainerID& containerId)
  {
    return slave::DOCKER_NAME_PREFIX + containerId.value();
  }

  enum ContainerState
  {
    EXISTS,
    RUNNING
  };

  static bool exists(
      const process::Shared<Docker>& docker,
      const ContainerID& containerId,
      ContainerState state = ContainerState::EXISTS,
      bool retry = true)
  {
    Duration waited = Duration::zero();
    string expectedName = containerName(containerId);

#ifdef __WINDOWS__
    constexpr Duration waitInspect = Seconds(10);
    constexpr Duration waitInterval = Milliseconds(500);
    constexpr Duration waitMax = Seconds(15);
#else
    constexpr Duration waitInspect = Seconds(3);
    constexpr Duration waitInterval = Milliseconds(200);
    constexpr Duration waitMax = Seconds(5);
#endif // __WINDOWS__

    do {
      Future<Docker::Container> inspect = docker->inspect(expectedName);

      if (!inspect.await(waitInspect)) {
        return false;
      }

      if (inspect.isReady()) {
        switch (state) {
          case ContainerState::RUNNING:
            if (inspect->pid.isSome()) {
              return true;
            }
            // Retry looking for running pid until timeout.
            break;
          case ContainerState::EXISTS:
            return true;
        }
      }

      os::sleep(waitInterval);
      waited += waitInterval;
    } while (retry && waited < waitMax);

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

  void SetUp() override
  {
    MesosTest::SetUp();

    Future<std::tuple<Nothing, Nothing>> pulls = process::collect(
        pullDockerImage(DOCKER_TEST_IMAGE),
        pullDockerImage(DOCKER_INKY_IMAGE));

    // The pull should only need to happen once since we don't delete the
    // image. So, we only log the warning once.
    LOG_FIRST_N(WARNING, 1) << "Pulling " << string(DOCKER_TEST_IMAGE)
                            << " and " << string(DOCKER_INKY_IMAGE) << ". "
                            << "This might take a while...";

    // The Windows images are ~200 MB, while the Linux images are ~2MB, so
    // hopefully this is enough time for the Windows images. There should
    // be some parallelism too, since we're pulling them simultaneously and
    // they share the same base Windows layer.
    AWAIT_READY_FOR(pulls, Minutes(10));
  }

  void TearDown() override
  {
    Try<Owned<Docker>> docker = Docker::create(
        tests::flags.docker,
        tests::flags.docker_socket,
        false);

    ASSERT_SOME(docker);

    Future<vector<Docker::Container>> containers =
      docker.get()->ps(true, slave::DOCKER_NAME_PREFIX);

    AWAIT_READY(containers);

    // Cleanup all mesos launched containers.
    foreach (const Docker::Container& container, containers.get()) {
      AWAIT_READY_FOR(docker.get()->rm(container.id, true), Seconds(30));
    }

    MesosTest::TearDown();
  }
};


// Only enable executor launch on linux as other platforms
// requires running linux VM and need special port forwarding
// to get host networking to work.
#ifdef __linux__
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch_Executor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  ExecutorID executorId;
  executorId.set_value("e1");

  CommandInfo command;
  command.set_value("/bin/test-executor");

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command,
      executorId);

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_executor()->mutable_container()->CopyFrom(
      createDockerInfo("tnachen/test-executor"));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  ASSERT_TRUE(exists(docker, containerId.get()));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  ASSERT_FALSE(
    exists(docker, containerId.get(), ContainerState::RUNNING, false));
}


// This test verifies that a custom executor can be launched and
// registered with the slave with docker bridge network enabled.
// We're assuming that the custom executor is registering its public
// ip instead of 0.0.0.0 or equivalent to the slave as that's the
// default behavior for libprocess.
//
// Currently this test fails on ubuntu and centos since the slave is
// binding and advertising 127.0.x.x address and unreachable by executor
// in bridge network.
// TODO(tnachen): Re-enable this test when we are able to fix MESOS-3123.
TEST_F(DockerContainerizerTest, DISABLED_ROOT_DOCKER_Launch_Executor_Bridged)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  ExecutorID executorId;
  executorId.set_value("e1");

  CommandInfo command;
  command.set_value("/bin/test-executor");

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command,
      executorId);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo = createDockerInfo("alpine");

  containerInfo.mutable_docker()->set_network(
      ContainerInfo::DockerInfo::BRIDGE);

  task.mutable_executor()->mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  ASSERT_TRUE(exists(docker, containerId.get()));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  ASSERT_FALSE(
    exists(docker, containerId.get(), ContainerState::RUNNING, false));
}
#endif // __linux__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  ASSERT_TRUE(statusRunning->has_data());

  Try<JSON::Array> array = JSON::parse<JSON::Array>(statusRunning->data());
  ASSERT_SOME(array);

  // Check if container information is exposed through master's state endpoint.
  Future<http::Response> response = http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Value> find = parse->find<JSON::Value>(
      "frameworks[0].tasks[0].container.docker.privileged");

  EXPECT_SOME_FALSE(find);

  // Check if container information is exposed through slave's state endpoint.
  response = http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  find = parse->find<JSON::Value>(
      "frameworks[0].executors[0].tasks[0].container.docker.privileged");

  EXPECT_SOME_FALSE(find);

  // Now verify the ContainerStatus fields in the TaskStatus.
  ASSERT_TRUE(statusRunning->has_container_status());
  EXPECT_TRUE(statusRunning->container_status().has_container_id());
  ASSERT_EQ(1, statusRunning->container_status().network_infos().size());
  EXPECT_EQ(1, statusRunning->container_status().network_infos(0).ip_addresses().size()); // NOLINT(whitespace/line_length)

  ASSERT_TRUE(exists(docker, containerId.get()));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  ASSERT_FALSE(
    exists(docker, containerId.get(), ContainerState::RUNNING, false));
}


// This test verifies that docker executor will terminate a task after it
// reaches `max_completion_time`.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_MaxCompletionTime)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  TaskInfo task =
    createTask(offer.slave_id(), offer.resources(), SLEEP_COMMAND(1000));

  // Set a `max_completion_time` for 10 seconds on Windows and 2 seconds on
  // other platforms. Hopefully this should not block test too long and still
  // keep it reliable.
#ifdef __WINDOWS__
  task.mutable_max_completion_time()->set_nanoseconds(Seconds(10).ns());
#else
  task.mutable_max_completion_time()->set_nanoseconds(Seconds(2).ns());
#endif // __WINDOWS__

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFailed;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<Nothing> executorTerminated =
    FUTURE_DISPATCH(_, &Slave::executorTerminated);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY(statusFailed);
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  EXPECT_EQ(
      TaskStatus::REASON_MAX_COMPLETION_TIME_REACHED, statusFailed->reason());

  AWAIT_READY(executorTerminated);

  driver.stop();
  driver.join();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Kill)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  ASSERT_TRUE(
    exists(docker, containerId.get(), ContainerState::RUNNING));

  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  // Even though the task is killed, the executor should exit gracefully.
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_EQ(0, termination.get()->status());

  ASSERT_FALSE(
    exists(docker, containerId.get(), ContainerState::RUNNING, false));

  driver.stop();
  driver.join();
}


// Ensures that the framework will receive a TASK_KILLING update
// before TASK_KILLED, if the capability is supported.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_TaskKillingCapability)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  // Start the framework with the task killing capability.
  FrameworkInfo::Capability capability;
  capability.set_type(FrameworkInfo::Capability::TASK_KILLING_STATE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->CopyFrom(capability);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  ASSERT_TRUE(
    exists(docker, containerId.get(), ContainerState::RUNNING));

  Future<TaskStatus> statusKilling, statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilling))
    .WillOnce(FutureArg<1>(&statusKilled));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilling);
  EXPECT_EQ(TASK_KILLING, statusKilling->state());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  ASSERT_FALSE(
    exists(docker, containerId.get(), ContainerState::RUNNING, false));

  driver.stop();
  driver.join();
}


// This test tests DockerContainerizer::usage().
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Usage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>("cpus:2;mem:1024");

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  CommandInfo command;
  // Run a CPU intensive command, so we can measure utime and stime later.
#ifdef __WINDOWS__
  command.set_value("for /L %n in (1, 0, 2) do rem");
#else
  command.set_value("dd if=/dev/zero of=/dev/null");
#endif // __WINDOWS__

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command);

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  // We ignore all update calls to prevent resizing cgroup limits.
  EXPECT_CALL(dockerContainerizer, update(_, _))
    .WillRepeatedly(Return(Nothing()));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

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
#ifndef __WINDOWS__
  // These aren't provided by the Windows Container APIs, so skip them.
  EXPECT_LT(0, statistics.cpus_user_time_secs());
  EXPECT_LT(0, statistics.cpus_system_time_secs());
  EXPECT_GT(statistics.mem_rss_bytes(), 0u);
#endif // __WINDOWS__

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  dockerContainerizer.destroy(containerId.get());

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  // Usage() should fail again since the container is destroyed.
  Future<ResourceStatistics> usage =
    dockerContainerizer.usage(containerId.get());

  AWAIT_FAILED(usage);

  driver.stop();
  driver.join();
}


#ifdef __linux__
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Update)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo("alpine"));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  ASSERT_TRUE(
    exists(docker, containerId.get(), ContainerState::RUNNING));

  string name = containerName(containerId.get());

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

  Option<pid_t> pid = inspect->pid;
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
  EXPECT_EQ(128u, mem->bytes() / Bytes::MEGABYTES);

  newResources = Resources::parse("cpus:1;mem:144");

  // Issue second update that uses the cached cgroups instead of inspect.
  update = dockerContainerizer.update(containerId.get(), newResources.get());

  AWAIT_READY(update);

  cpu = cgroups::cpu::shares(cpuHierarchy.get(), cpuCgroup.get());

  ASSERT_SOME(cpu);

  mem = cgroups::memory::soft_limit_in_bytes(
      memoryHierarchy.get(),
      memoryCgroup.get());

  ASSERT_SOME(mem);

  EXPECT_EQ(1024u, cpu.get());
  EXPECT_EQ(144u, mem->bytes() / Bytes::MEGABYTES);

  driver.stop();
  driver.join();
}
#endif // __linux__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Recover)
{
  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  SlaveID slaveId;
  slaveId.set_value("s1");
  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());
  ContainerID reapedContainerId;
  reapedContainerId.set_value(id::UUID::random().toString());

  string container1 = containerName(containerId);
  string container2 = containerName(reapedContainerId);

  // Clean up artifacts if containers still exists.
  ASSERT_TRUE(docker->rm(container1, true).await(Seconds(30)));
  ASSERT_TRUE(docker->rm(container2, true).await(Seconds(30)));

  Resources resources = Resources::parse("cpus:1;mem:512").get();

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo = createDockerInfo(DOCKER_TEST_IMAGE);

  CommandInfo commandInfo;
  commandInfo.set_value(SLEEP_COMMAND(1000));

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      container1,
      flags.work_dir,
      flags.sandbox_directory,
      resources);

  ASSERT_SOME(runOptions);

  docker->run(runOptions.get());

  Try<Docker::RunOptions> orphanOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      container2,
      flags.work_dir,
      flags.sandbox_directory,
      resources);

  ASSERT_SOME(orphanOptions);

  Future<Option<int>> orphanRun = docker->run(orphanOptions.get());

  ASSERT_TRUE(
    exists(docker, containerId, ContainerState::RUNNING));
  ASSERT_TRUE(
    exists(docker, reapedContainerId, ContainerState::RUNNING));

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
  runState.forkedPid = wait->pid();

  execState.runs.put(containerId, runState);
  frameworkState.executors.put(execId, execState);

  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);

  AWAIT_READY(recover);

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId);

  ASSERT_FALSE(termination.isFailed());

  // The reaped container should be cleaned up and unknown at this point.
  Future<Option<ContainerTermination>> termination2 =
    dockerContainerizer.wait(reapedContainerId);

  AWAIT_READY(termination2);
  EXPECT_NONE(termination2.get());

  // Expect the orphan to be stopped!
  assertDockerKillStatus(orphanRun);
}


// This test ensures that orphaned docker containers unknown to the current
// agent instance are properly cleaned up.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_KillOrphanContainers)
{
  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  SlaveID slaveId;
  slaveId.set_value("s1");

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ContainerID orphanContainerId;
  orphanContainerId.set_value(id::UUID::random().toString());

  string container1 = containerName(containerId);

  // Start the orphan container with the old slave id.
  string container2 = containerName(orphanContainerId);

  // Clean up artifacts if containers still exists.
  ASSERT_TRUE(docker->rm(container1, true).await(Seconds(30)));
  ASSERT_TRUE(docker->rm(container2, true).await(Seconds(30)));

  Resources resources = Resources::parse("cpus:1;mem:512").get();

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo = createDockerInfo(DOCKER_TEST_IMAGE);

  CommandInfo commandInfo;
  commandInfo.set_value(SLEEP_COMMAND(1000));

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      container1,
      flags.work_dir,
      flags.sandbox_directory,
      resources);

  ASSERT_SOME(runOptions);

  docker->run(runOptions.get());

  Try<Docker::RunOptions> orphanOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      container2,
      flags.work_dir,
      flags.sandbox_directory,
      resources);

  ASSERT_SOME(orphanOptions);

  Future<Option<int>> orphanRun = docker->run(orphanOptions.get());

  ASSERT_TRUE(
    exists(docker, containerId, ContainerState::RUNNING));

  ASSERT_TRUE(
    exists(docker, orphanContainerId, ContainerState::RUNNING));

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
  runState.forkedPid = wait->pid();

  execState.runs.put(containerId, runState);
  frameworkState.executors.put(execId, execState);

  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);

  AWAIT_READY(recover);

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId);

  ASSERT_FALSE(termination.isFailed());

  // The orphaned container should be correctly cleaned up.
  Future<Option<ContainerTermination>> termination2 =
    dockerContainerizer.wait(orphanContainerId);

  AWAIT_READY(termination2);
  EXPECT_NONE(termination2.get());
  ASSERT_FALSE(
      exists(docker, orphanContainerId, ContainerState::EXISTS, false));

  assertDockerKillStatus(orphanRun);
}


// This test checks the docker containerizer doesn't recover executors
// that were started by another containerizer (e.g: mesos).
TEST_F(DockerContainerizerTest, ROOT_DOCKER_SkipRecoverNonDocker)
{
  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorID executorId;
  executorId.set_value(id::UUID::random().toString());

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
  frameworkId.set_value(id::UUID::random().toString());
  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);
  AWAIT_READY(recover);

  Future<hashset<ContainerID>> containers = dockerContainerizer.containers();
  AWAIT_READY(containers);

  // A MesosContainerizer task shouldn't be recovered by
  // DockerContainerizer.
  EXPECT_TRUE(containers->empty());
}


// This test checks the docker containerizer doesn't recover containers
// with malformed uuid.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_SkipRecoverMalformedUUID)
{
  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.docker_kill_orphans = true;

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  SlaveID slaveId;
  slaveId.set_value("s1");
  ContainerID containerId;
  containerId.set_value("malformedUUID");

  string container = containerName(containerId);

  // Clean up container if it still exists.
  ASSERT_TRUE(docker->rm(container, true).await(Seconds(30)));

  Resources resources = Resources::parse("cpus:1;mem:512").get();

  // TODO(tnachen): Use local image to test if possible.

  CommandInfo commandInfo;
  commandInfo.set_value(SLEEP_COMMAND(1000));

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      createDockerInfo(DOCKER_TEST_IMAGE),
      commandInfo,
      container,
      flags.work_dir,
      flags.sandbox_directory,
      resources);

  Future<Option<int>> run = docker->run(runOptions.get());

  ASSERT_TRUE(
    exists(docker, containerId, ContainerState::RUNNING));

  SlaveState slaveState;
  slaveState.id = slaveId;
  FrameworkState frameworkState;

  ExecutorID execId;
  execId.set_value("e1");

  ExecutorState execState;
  ExecutorInfo execInfo;
  execState.info = execInfo;

  FrameworkID frameworkId;
  frameworkState.executors.put(execId, execState);
  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);
  AWAIT_READY(recover);

  // The container should still exist and should not get killed
  // by containerizer recovery.
  ASSERT_TRUE(exists(docker, containerId));
}


// TOOD(akagup): Persistent volumes aren't implemented on Windows, but these
// tests should be enabled once we implement them. See MESOS-5461.
#ifdef __linux__
// This test verifies that we can launch a docker container with
// persistent volume.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_LaunchWithPersistentVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpu:2;mem:2048;disk(role1):2048";

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  Resource volume = createPersistentVolume(
    Megabytes(64),
    "role1",
    "id1",
    "path1",
    None(),
    None(),
    frameworkInfo.principal());

  CommandInfo command;
  command.set_value("echo abc > " +
                    path::join(flags.sandbox_directory, "path1", "file"));

  TaskInfo task = createTask(
      offers->front().slave_id(),
      Resources::parse("cpus:1;mem:64").get() + volume,
      command);

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo("alpine"));

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Future<ContainerID> containerId;
  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<1>(&containerConfig),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.acceptOffers(
      {offers->front().id()},
      {CREATE(volume), LAUNCH({task})},
      filters);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(containerConfig);

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  ASSERT_FALSE(
    exists(docker, containerId.get(), ContainerState::RUNNING, false));

  const string& volumePath = getPersistentVolumePath(
      flags.work_dir,
      volume);

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volumePath, "file")));

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  EXPECT_SOME(table);

  // Verify that the persistent volume is unmounted.
  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    EXPECT_FALSE(strings::contains(
        entry.target,
        path::join(containerConfig->directory(), "path1")));
  }
}


// This test checks the docker containerizer is able to recover containers
// with persistent volumes and destroy it properly.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_RecoverPersistentVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpu:2;mem:2048;disk(role1):2048";

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Owned<MockDockerContainerizer> dockerContainerizer(
      new MockDockerContainerizer(
          flags,
          &fetcher,
          Owned<ContainerLogger>(logger.get()),
          docker));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Filters filters;
  filters.set_refuse_seconds(0);

  // NOTE: We set filter explicitly here so that the resources will
  // not be filtered for 5 seconds (the default).
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers(filters));      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resource volume = createPersistentVolume(
    Megabytes(64),
    "role1",
    "id1",
    "path1",
    None(),
    None(),
    frameworkInfo.principal());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      Resources::parse("cpus:1;mem:64").get() + volume,
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo("alpine"));

  Future<ContainerID> containerId;
  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(*dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<1>(&containerConfig),
                    Invoke(dockerContainerizer.get(),
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.acceptOffers(
      {offers->front().id()},
      {CREATE(volume), LAUNCH({task})},
      filters);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(containerConfig);
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Recreate containerizer and start slave again.
  slave.get()->terminate();
  slave->reset();

  logger = ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  dockerContainerizer.reset(new MockDockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker));

  slave = StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Wait until containerizer recover is complete.
  AWAIT_READY(_recover);

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer->destroy(containerId.get());

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  EXPECT_SOME(table);

  // Verify that the recovered container's persistent volume is
  // unmounted.
  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    EXPECT_FALSE(strings::contains(
        entry.target,
        path::join(containerConfig->directory(), "path1")));
  }

  driver.stop();
  driver.join();
}


// This test checks the docker containerizer is able to clean up
// orphaned containers with persistent volumes.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_RecoverOrphanedPersistentVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpu:2;mem:2048;disk(role1):2048";

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Owned<MockDockerContainerizer> dockerContainerizer(
      new MockDockerContainerizer(
          flags,
          &fetcher,
          Owned<ContainerLogger>(logger.get()),
          docker));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Filters filters;
  filters.set_refuse_seconds(0);

  // NOTE: We set filter explicitly here so that the resources will
  // not be filtered for 5 seconds (the default).
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers(filters)); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resource volume = createPersistentVolume(
    Megabytes(64),
    "role1",
    "id1",
    "path1",
    None(),
    None(),
    frameworkInfo.principal());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      Resources::parse("cpus:1;mem:64").get() + volume,
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo("alpine"));

  Future<ContainerID> containerId;
  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(*dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<1>(&containerConfig),
                    Invoke(dockerContainerizer.get(),
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.acceptOffers(
      {offers->front().id()},
      {CREATE(volume), LAUNCH({task})},
      filters);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(containerConfig);
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Recreate containerizer and start slave again.
  slave.get()->terminate();
  slave->reset();

  // Wipe the framework directory so that the slave will treat the
  // above running task as an orphan. We don't want to wipe the whole
  // meta directory since Docker Containerizer will skip recover if
  // state is not found.
  ASSERT_SOME(
      os::rmdir(getFrameworkPath(
          getMetaRootDir(flags.work_dir),
          offers->front().slave_id(),
          frameworkId.get())));

  logger = ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  dockerContainerizer.reset(new MockDockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker));

  slave = StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  // Wait until containerizer recover is complete.
  AWAIT_READY(_recover);

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  EXPECT_SOME(table);

  // Verify that the orphaned container's persistent volume is
  // unmounted.
  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    EXPECT_FALSE(strings::contains(
        entry.target,
        path::join(containerConfig->directory(), "path1")));
  }

  driver.stop();
  driver.join();

  slave->reset();

  EXPECT_FALSE(
      exists(docker, containerId.get(), ContainerState::EXISTS, false));
}
#endif // __linux__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Logs)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  string uuid = id::UUID::random().toString();

  CommandInfo command;
#ifdef __WINDOWS__
  // We avoid spaces in `echo` since `echo` in `cmd.exe` treats spaces
  // in the argument as literal spaces so `echo X<SPACE>` outputs X<SPACE>.
  // We don't use powershell here since `Write-Error` is verbose and causes
  // the script to return a failure.
  command.set_value(
      "echo out" + uuid + "&"
      "(echo err" + uuid + ")1>&2");
#else
  // NOTE: We prefix `echo` with `unbuffer` so that we can immediately
  // flush the output of `echo`.  This mitigates a race in Docker where
  // it mangles reads from stdout/stderr and commits suicide.
  // See MESOS-4676 for more information.
  command.set_value(
      "unbuffer echo out" + uuid + " ; "
      "unbuffer echo err" + uuid + " 1>&2");
#endif // __WINDOWS__

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command);

  // TODO(tnachen): Use local image to test if possible.
#ifdef __WINDOWS__
  const ContainerInfo containerInfo =
    createDockerInfo(DOCKER_TEST_IMAGE);
#else
  // NOTE: This is an image that is exactly
  // `docker run -t -i alpine /bin/sh -c "apk add --update expect"`.
  const ContainerInfo containerInfo =
    createDockerInfo("mesosphere/alpine-expect");
#endif // __WINDOWS__

  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<1>(&containerConfig),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(containerConfig);
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // Now check that the proper output is in stderr and stdout (which
  // might also contain other things, hence the use of a UUID).
  Try<string> read =
    os::read(path::join(containerConfig->directory(), "stderr"));

  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  EXPECT_TRUE(containsLine(lines, "err" + uuid));
  EXPECT_FALSE(containsLine(lines, "out" + uuid));

  read = os::read(path::join(containerConfig->directory(), "stdout"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_TRUE(containsLine(lines, "out" + uuid));
  EXPECT_FALSE(containsLine(lines, "err" + uuid));

  driver.stop();
  driver.join();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  CommandInfo command;
  command.set_shell(false);

  // NOTE: By not setting CommandInfo::value we're testing that we
  // will still be able to run the container because it has a default
  // entrypoint!

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command);

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_INKY_IMAGE));

  Future<ContainerID> containerId;
  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<1>(&containerConfig),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(containerConfig);
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  Try<string> read =
    os::read(path::join(containerConfig->directory(), "stdout"));

  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  // Since we're not passing any command value, we're expecting the
  // default entry point to be run which is 'echo' with the default
  // command from the image which is 'inky'.
  EXPECT_TRUE(containsLine(lines, "inky"));

  read = os::read(path::join(containerConfig->directory(), "stderr"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_FALSE(containsLine(lines, "inky"));

  driver.stop();
  driver.join();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD_Override)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping  a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  string uuid = id::UUID::random().toString();

  CommandInfo command;
  command.set_shell(false);

  // We can set the value to just the 'uuid' since it should get
  // passed as an argument to the entrypoint, i.e., 'echo uuid'.
  command.set_value(uuid);

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command);

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_INKY_IMAGE));

  Future<ContainerID> containerId;
  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<1>(&containerConfig),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(containerConfig);
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // Now check that the proper output is in stderr and stdout.
  Try<string> read =
    os::read(path::join(containerConfig->directory(), "stdout"));

  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  // We expect the passed in command value to override the image's
  // default command, thus we should see the value of 'uuid' in the
  // output instead of the default command which is 'inky'.
  EXPECT_TRUE(containsLine(lines, uuid));
  EXPECT_FALSE(containsLine(lines, "inky"));

  read = os::read(path::join(containerConfig->directory(), "stderr"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_FALSE(containsLine(lines, "inky"));
  EXPECT_FALSE(containsLine(lines, uuid));

  driver.stop();
  driver.join();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD_Args)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  string uuid = id::UUID::random().toString();

  CommandInfo command;
  command.set_shell(false);

  // We should also be able to skip setting the command value and just
  // set the arguments and those should also get passed through to the
  // entrypoint!
  command.add_arguments(uuid);

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command);

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_INKY_IMAGE));

  Future<ContainerID> containerId;
  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<1>(&containerConfig),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(containerConfig);
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // Now check that the proper output is in stderr and stdout.
  Try<string> read =
    os::read(path::join(containerConfig->directory(), "stdout"));

  ASSERT_SOME(read);

  vector<string> lines = strings::split(read.get(), "\n");

  // We expect the passed in command arguments to override the image's
  // default command, thus we should see the value of 'uuid' in the
  // output instead of the default command which is 'inky'.
  EXPECT_TRUE(containsLine(lines, uuid));
  EXPECT_FALSE(containsLine(lines, "inky"));

  read = os::read(path::join(containerConfig->directory(), "stderr"));
  ASSERT_SOME(read);

  lines = strings::split(read.get(), "\n");

  EXPECT_FALSE(containsLine(lines, "inky"));
  EXPECT_FALSE(containsLine(lines, uuid));

  driver.stop();
  driver.join();
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up we make sure the executor
// reregisters and the slave properly sends the update.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_SlaveRecoveryTaskContainer)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  // This is owned by the containerizer, so we'll need one per containerizer.
  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Owned<MockDockerContainerizer> dockerContainerizer(
      new MockDockerContainerizer(
          flags,
          &fetcher,
          Owned<ContainerLogger>(logger.get()),
          docker));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<ContainerID> containerId;
  EXPECT_CALL(*dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(dockerContainerizer.get(),
                           &MockDockerContainerizer::_launch)));

  // Drop the status updates from the executor. We actually wait until we can
  // drop the `TASK_RUNNING` update here because the window between the two is
  // small enough that we could still successfully receive `TASK_RUNNING` after
  // we have dropped `TASK_STARTING`.
  Future<StatusUpdateMessage> runningUpdate =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  Future<StatusUpdateMessage> startingUpdate =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  // Stop the slave before the status updates are received.
  AWAIT_READY(startingUpdate);
  AWAIT_READY(runningUpdate);

  slave.get()->terminate();

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  // This is owned by the containerizer, so we'll need one per containerizer.
  logger = ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  dockerContainerizer.reset(new MockDockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker));

  slave = StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  // Ensure the executor reregisters.
  AWAIT_READY(reregisterExecutorMessage);

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage->body);

  // Executor should inform about the unacknowledged updates.
  ASSERT_EQ(2, reregister.updates_size());

  const StatusUpdate& updateStarting = reregister.updates(0);
  ASSERT_EQ(task.task_id(), updateStarting.status().task_id());
  ASSERT_EQ(TASK_STARTING, updateStarting.status().state());

  const StatusUpdate& updateRunning = reregister.updates(1);
  ASSERT_EQ(task.task_id(), updateRunning.status().task_id());
  ASSERT_EQ(TASK_RUNNING, updateRunning.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_STARTING, status->state());

  ASSERT_TRUE(exists(docker, containerId.get()));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer->wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}

#ifndef __WINDOWS__
// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up we make sure the executor
// reregisters and the slave properly sends the update.
//
// The test is removed on Windows, because the `mesosphere/test-executor`
// image doesn't work on Windows and probably won't ever be ported.
//
// TODO(benh): This test is currently disabled because the executor
// inside the image mesosphere/test-executor does not properly set the
// executor PID that is uses during registration, so when the new
// slave recovers it can't reconnect and instead destroys that
// container. In particular, it uses '0' for its IP which we properly
// parse and can even properly use for sending other messages, but the
// current implementation of 'UPID::operator bool()' fails if the IP
// component of a PID is '0'.
//
// TODO(alexr): Enable after MESOS-8708 is resolved.
TEST_F(DockerContainerizerTest,
       DISABLED_ROOT_DOCKER_SlaveRecoveryExecutorContainer)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  // This is owned by the containerizer, so we'll need one per containerizer.
  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Owned<MockDockerContainerizer> dockerContainerizer(
      new MockDockerContainerizer(
          flags,
          &fetcher,
          Owned<ContainerLogger>(logger.get()),
          docker));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  ExecutorID executorId;
  executorId.set_value("e1");

  CommandInfo command;
  command.set_value("test-executor");

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command,
      executorId);

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_executor()->mutable_container()->CopyFrom(
      createDockerInfo("mesosphere/test-executor"));

  Future<ContainerID> containerId;
  EXPECT_CALL(*dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(dockerContainerizer.get(),
                           &MockDockerContainerizer::_launch)));

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

  AWAIT_READY(executorLaunched);
  AWAIT_READY(statusUpdateMessage1);
  AWAIT_READY(statusUpdateMessage2);

  slave.get()->terminate();

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  // This is owned by the containerizer, so we'll need one per containerizer.
  logger = ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  dockerContainerizer.reset(new MockDockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker));

  slave = StartSlave(detector.get(), dockerContainerizer.get(), flags);
  ASSERT_SOME(slave);

  // Ensure the executor reregisters.
  AWAIT_READY(reregisterExecutorMessage);

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage->body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_STARTING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_STARTING, status->state());

  ASSERT_TRUE(exists(docker, containerId.get()));

  driver.stop();
  driver.join();
}
#endif // __WINDOWS__


// This test verifies that port mapping with bridge network is
// exposing the host port to the container port, by sending data
// to the host port and receiving it in the container by listening
// to the mapped container port.
//
// TODO(akagup): This test requres netcat on the Windows host before
// it can be ported. We could provide a build of netcat or just replace
// it with powershell for this test.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    DockerContainerizerTest, ROOT_DOCKER_NC_PortMapping)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:1;mem:1024;ports:[10000-10000]";

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  CommandInfo command;
  command.set_shell(false);
  command.set_value("nc");
  command.add_arguments("-l");
  command.add_arguments("-p");
  command.add_arguments("1000");

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo = createDockerInfo("alpine");

  containerInfo.mutable_docker()->set_network(
      ContainerInfo::DockerInfo::BRIDGE);

  ContainerInfo::DockerInfo::PortMapping portMapping;
  portMapping.set_host_port(10000);
  portMapping.set_container_port(1000);

  containerInfo.mutable_docker()->add_port_mappings()->CopyFrom(portMapping);

  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<1>(&containerConfig),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(containerConfig);
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  ASSERT_TRUE(
    exists(docker,
           containerId.get(),
           ContainerState::RUNNING));

  string uuid = id::UUID::random().toString();

  // Write uuid to docker mapped host port.
  Try<process::Subprocess> s = process::subprocess(
      "echo " + uuid + " | nc localhost 10000");

  ASSERT_SOME(s);
  AWAIT_READY_FOR(s->status(), Seconds(60));

  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // Now check that the proper output is in stdout.
  Try<string> read =
    os::read(path::join(containerConfig->directory(), "stdout"));

  ASSERT_SOME(read);

  const vector<string> lines = strings::split(read.get(), "\n");

  // We expect the uuid that is sent to host port to be written
  // to stdout by the docker container running nc -l.
  EXPECT_TRUE(containsLine(lines, uuid));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}

#ifndef __WINDOWS__
// This test verifies that sandbox with ':' in the path can still
// run successfully. This a limitation of the Docker CLI where
// the volume map parameter treats colons (:) as separators,
// and incorrectly separates the sandbox directory.
//
// On Windows, colons aren't a legal path character, so this test is skipped.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_LaunchSandboxWithColon)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  // Create a sleep task whose name is "test:colon".
  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000),
      None(),
      "test:colon");

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo("alpine"));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  ASSERT_TRUE(exists(docker, containerId.get()));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}
#endif // __WINDOWS__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_DestroyWhileFetching)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Promise<Nothing> promise;
  Future<Nothing> fetch;

  // We want to pause the fetch call to simulate a long fetch time.
  EXPECT_CALL(*process, fetch(_))
    .WillOnce(DoAll(FutureSatisfy(&fetch),
                    Return(promise.future())));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
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

  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  driver.stop();
  driver.join();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_DestroyWhilePulling)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Future<Nothing> fetch;
  EXPECT_CALL(*process, fetch(_))
    .WillOnce(DoAll(FutureSatisfy(&fetch),
                    Return(Nothing())));

  Promise<Nothing> promise;

  // We want to pause the fetch call to simulate a long fetch time.
  EXPECT_CALL(*process, pull(_))
    .WillOnce(Return(promise.future()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
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

  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  driver.stop();
  driver.join();
}


// Ensures the containerizer responds correctly (false Future) to
// a request to destroy an unknown container.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_DestroyUnknownContainer)
{
  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<DockerContainerizer*> create =
    DockerContainerizer::create(flags, &fetcher);

  ASSERT_SOME(create);

  DockerContainerizer* containerizer = create.get();

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Future<Option<ContainerTermination>> destroyed =
    containerizer->destroy(containerId);

  AWAIT_READY(destroyed);
  EXPECT_NONE(destroyed.get());
}


// This test checks that when a docker containerizer update failed
// and the container failed before the executor started, the executor
// is properly killed and cleaned up.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_ExecutorCleanupWhenLaunchFailed)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      "exit 0");

  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<TaskStatus> statusGone;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusGone));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  // Fail the update so we don't proceed to send run task to the executor.
  EXPECT_CALL(dockerContainerizer, update(_, _))
    .WillRepeatedly(Return(Failure("Fail resource update")));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusGone);
  EXPECT_EQ(TASK_GONE, statusGone->state());
  EXPECT_EQ(TaskStatus::REASON_CONTAINER_UPDATE_FAILED,
            statusGone->reason());

  driver.stop();
  driver.join();
}


// When the fetch fails we should send the scheduler a status
// update with message the shows the actual error.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_FetchFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      "exit 0");

  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  EXPECT_CALL(*process, fetch(_))
    .WillOnce(Return(Failure("some error from fetch")));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed->state());
  EXPECT_EQ("Failed to launch container: some error from fetch",
             statusFailed->message());

  // TODO(jaybuff): When MESOS-2035 is addressed we should validate
  // that statusFailed->reason() is correctly set here.

  driver.stop();
  driver.join();
}


// When the docker pull fails we should send the scheduler a status
// update with message the shows the actual error.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_DockerPullFailure)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      "exit 0");

  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  EXPECT_CALL(*mockDocker, pull(_, _, _))
    .WillOnce(Return(Failure("some error from docker pull")));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed->state());
  EXPECT_EQ("Failed to launch container: some error from docker pull",
             statusFailed->message());

  // TODO(jaybuff): When MESOS-2035 is addressed we should validate
  // that statusFailed->reason() is correctly set here.

  driver.stop();
  driver.join();
}


// When the docker executor container fails to launch, docker inspect
// future that is in a retry loop should be discarded.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_DockerInspectDiscard)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Future<Docker::Container> inspect;
  EXPECT_CALL(*mockDocker, inspect(_, _))
    .WillOnce(FutureResult(&inspect,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_inspect)));

  EXPECT_CALL(*mockDocker, run(_, _, _))
    .WillOnce(Return(Failure("Run failed")));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  ExecutorID executorId;
  executorId.set_value("e1");

  CommandInfo command;
#ifdef __WINDOWS__
  command.set_value(SLEEP_COMMAND(1000));
#else
  command.set_value("/bin/test-executor");
#endif // __WINDOWS__

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command,
      executorId);

  // TODO(tnachen): Use local image to test if possible.
#ifdef __WINDOWS__
  const ContainerInfo containerInfo = createDockerInfo(DOCKER_TEST_IMAGE);
#else
  const ContainerInfo containerInfo = createDockerInfo("tnachen/test-executor");
#endif // __WINDOWS__

  task.mutable_executor()->mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<Nothing> executorLost;
  EXPECT_CALL(sched, executorLost(&driver, executorId, _, _))
    .WillOnce(FutureSatisfy(&executorLost));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(statusFailed);
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  AWAIT_READY(executorLost);

  AWAIT_DISCARDED(inspect);

  driver.stop();
  driver.join();
}


// Ensures the containerizer responds correctly (returns None)
// to a request to wait on an unknown container.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_WaitUnknownContainer)
{
  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<DockerContainerizer*> create =
    DockerContainerizer::create(flags, &fetcher);

  ASSERT_SOME(create);

  DockerContainerizer* containerizer = create.get();

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  EXPECT_NONE(wait.get());
}


// This test ensures that a task will transition straight from `TASK_KILLING` to
// `TASK_KILLED`, even if the health check begins to fail during the kill policy
// grace period.
//
// TODO(gkleiman): this test takes about 7 seconds to run, consider using mock
// tasks and health checkers to speed it up.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_NoTransitionFromKillingToRunning)
{
  Shared<Docker> docker(new MockDocker(
      tests::flags.docker, tests::flags.docker_socket));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();

  Fetcher fetcher(agentFlags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(agentFlags.container_logger);
  ASSERT_SOME(logger);

  MockDockerContainerizer containerizer(
      agentFlags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent =
    StartSlave(detector.get(), &containerizer, agentFlags);
  ASSERT_SOME(agent);

  // Start the framework with the task killing capability.
  FrameworkInfo::Capability capability;
  capability.set_type(FrameworkInfo::Capability::TASK_KILLING_STATE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->CopyFrom(capability);

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
  EXPECT_EQ(1u, offers->size());

  const uint16_t testPort = getFreePort().get();

#ifdef __WINDOWS__
  // On Windows, we will do a command health check instead of a TCP one,
  // so that this test will work on Windows 10 (Hyper-V isolation) containers.
  const string command = SLEEP_COMMAND(1000);
#else
  // Launch a HTTP server until SIGTERM is received, then sleep for
  // 15 seconds to let the health check fail.
  const string command = strings::format(
      "trap \"sleep 15\" SIGTERM && nc -lk -p %u -e echo",
      testPort).get();
#endif // __WINDOWS__

  TaskInfo task = createTask(offers->front(), command);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo = createDockerInfo(DOCKER_TEST_IMAGE);

  // On Linux, the docker container runs in host network mode.
#ifndef __WINDOWS__
  containerInfo.mutable_docker()->set_network(
      ContainerInfo::DockerInfo::HOST);
#endif // __WINDOWS__

  task.mutable_container()->CopyFrom(containerInfo);

  HealthCheck healthCheck;
#ifdef __WINDOWS__
  healthCheck.set_type(HealthCheck::COMMAND);

  // The first `mkdir` will succeed, but the later ones will fail, so we get
  // the same behavior as the Linux test.
  healthCheck.mutable_command()->set_value("mkdir C:\\healthcheck-test");
#else
  healthCheck.set_type(HealthCheck::TCP);
  healthCheck.mutable_tcp()->set_port(testPort);
#endif // __WINDOWS__

  // Set `grace_period_seconds` here because it takes some time to launch
  // Netcat to serve requests.
  healthCheck.set_delay_seconds(0);
  healthCheck.set_grace_period_seconds(15);
  healthCheck.set_interval_seconds(0);

  task.mutable_health_check()->CopyFrom(healthCheck);

  // Set the kill policy grace period to 5 seconds.
  KillPolicy killPolicy;
  killPolicy.mutable_grace_period()->set_nanoseconds(Seconds(5).ns());

  task.mutable_kill_policy()->CopyFrom(killPolicy);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;
  Future<TaskStatus> statusKilling;
  Future<TaskStatus> statusKilled;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillOnce(FutureArg<1>(&statusKilling))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilling);
  EXPECT_EQ(TASK_KILLING, statusKilling->state());
  EXPECT_FALSE(statusKilling->has_healthy());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());
  EXPECT_FALSE(statusKilled->has_healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}


#ifndef __WINDOWS__
// This test ensures that a task will transition from `TASK_KILLING`
// to `TASK_KILLED` rather than `TASK_FINISHED` when it is killed,
// even if it returns an "EXIT_STATUS" of 0 on receiving a SIGTERM.
//
// This test is ignored on Windows, since Windows containers seem to
// always return `STATUS_CONTROL_C_EXIT` and `STATUS_UNSUCCESSFUL` for
// graceful and forceful shutdown.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_NoTransitionFromKillingToFinished)
{
  Shared<Docker> docker(new MockDocker(
      tests::flags.docker, tests::flags.docker_socket));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();

  Fetcher fetcher(agentFlags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(agentFlags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer containerizer(
      agentFlags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent =
    StartSlave(detector.get(), &containerizer, agentFlags);

  ASSERT_SOME(agent);

  // Start the framework with the task killing capability.
  FrameworkInfo::Capability capability;
  capability.set_type(FrameworkInfo::Capability::TASK_KILLING_STATE);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->CopyFrom(capability);

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
  EXPECT_EQ(1u, offers->size());

  CommandInfo command;
  command.set_shell(false);

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command);

  // The "nginx:alpine" container returns an "EXIT_STATUS" of 0 on
  // receiving a SIGTERM.
  //
  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo("nginx:alpine"));

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusKilling;
  Future<TaskStatus> statusKilled;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusKilling))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.launchTasks(offers->front().id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Docker executor will call "docker stop ..." to send SIGTERM
  // to kill the task.
  driver.killTask(task.task_id());

  AWAIT_READY(statusKilling);
  EXPECT_EQ(TASK_KILLING, statusKilling->state());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}
#endif // __WINDOWS__


// This test ensures that when `cgroups_enable_cfs` is set on agent,
// the docker container launched through docker containerizer has
// `cpuQuotas` limit.
// Cgroups cpu quota is only available on Linux.
#ifdef __linux__
TEST_F(DockerContainerizerTest, ROOT_DOCKER_CGROUPS_CFS_CgroupsEnableCFS)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();
  flags.cgroups_enable_cfs = true;
  flags.resources = "cpus:1;mem:128";

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo("alpine"));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  ASSERT_TRUE(statusRunning->has_data());

  // Find cgroups cpu hierarchy of the container and verifies
  // quota is set.
  string name = containerName(containerId.get());
  Future<Docker::Container> inspect = docker->inspect(name);
  AWAIT_READY(inspect);

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  ASSERT_SOME(cpuHierarchy);

  Option<pid_t> pid = inspect->pid;
  ASSERT_SOME(pid);

  Result<string> cpuCgroup = cgroups::cpu::cgroup(pid.get());
  ASSERT_SOME(cpuCgroup);

  Try<Duration> cfsQuota =
    cgroups::cpu::cfs_quota_us(cpuHierarchy.get(), cpuCgroup.get());

  ASSERT_SOME(cfsQuota);

  const Duration expectedCpuQuota = mesos::internal::slave::CPU_CFS_PERIOD * 1;
  EXPECT_EQ(expectedCpuQuota, cfsQuota.get());

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}
#endif // __linux__


#ifndef __WINDOWS__
// Run a task as non root while inheriting this ownership from the
// framework supplied default user. Tests if the sandbox "stdout"
// is correctly owned and writeable by the tasks user.
// This test isn't run on Windows, because the `switch_user` flag
// isn't supported.
TEST_F(DockerContainerizerTest,
       ROOT_DOCKER_UNPRIVILEGED_USER_NonRootSandbox)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Shared<Docker> docker(
      new MockDocker(tests::flags.docker, tests::flags.docker_socket));

  slave::Flags flags = CreateSlaveFlags();
  flags.switch_user = true;

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);
  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  FrameworkInfo framework;
  framework.set_name("default");
  framework.set_user(user.get());
  framework.set_principal(DEFAULT_CREDENTIAL.principal());
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  // Start the task as a user without supplying an explicit command
  // user. This should inherit the framework user for the task
  // ownership.
  CommandInfo command;
  command.set_value("echo \"foo\" && sleep 1000");

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      command);

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo("alpine"));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  ASSERT_TRUE(exists(docker, containerId.get()));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  ASSERT_FALSE(
    exists(docker, containerId.get(), ContainerState::RUNNING, false));

  // Check that the sandbox was written to.
  const string sandboxDirectory = slave::paths::getExecutorRunPath(
      flags.work_dir,
      slaveId,
      frameworkId.get(),
      statusRunning->executor_id(),
      containerId.get());

  ASSERT_TRUE(os::exists(sandboxDirectory));

  // Check the sandbox "stdout" was written to.
  const string stdoutPath = path::join(sandboxDirectory, "stdout");
  ASSERT_TRUE(os::exists(stdoutPath));

  // Check the sandbox "stdout" is owned by the framework default user.
  struct stat stdoutStat;
  ASSERT_GE(::stat(stdoutPath.c_str(), &stdoutStat), 0);

  Result<uid_t> uid = os::getuid(framework.user());
  ASSERT_SOME(uid);

  ASSERT_EQ(stdoutStat.st_uid, uid.get());

  // Validate that our task was able to log into the sandbox.
  Result<string> stdout = os::read(stdoutPath);
  ASSERT_SOME(stdout);

  EXPECT_TRUE(strings::contains(stdout.get(), "foo"));
}
#endif // __WINDOWS__


// This test verifies the DNS configuration of the Docker container
// can be successfully set with the agent flag `--default_container_dns`.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_DefaultDNS)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

#ifdef __WINDOWS__
  // --dns-option and --dns-search are not supported on Windows.
  // See https://docs.microsoft.com/en-us/virtualization/windowscontainers/manage-containers/container-networking // NOLINT(whitespace/line_length)
  Try<ContainerDNSInfo> parse = flags::parse<ContainerDNSInfo>(
      R"~(
      {
        "docker": [
          {
            "network_mode": "BRIDGE",
            "dns": {
              "nameservers": [ "8.8.8.8", "8.8.4.4" ]
            }
          }
        ]
      })~");
#else
  Try<ContainerDNSInfo> parse = flags::parse<ContainerDNSInfo>(
      R"~(
      {
        "docker": [
          {
            "network_mode": "BRIDGE",
            "dns": {
              "nameservers": [ "8.8.8.8", "8.8.4.4" ],
              "search": [ "example1.com", "example2.com" ],
              "options": [ "timeout:3", "attempts:2" ]
            }
          }
        ]
      })~");
#endif // __WINDOWS__

  ASSERT_SOME(parse);

  flags.default_container_dns = parse.get();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo = createDockerInfo(DOCKER_TEST_IMAGE);

  containerInfo.mutable_docker()->set_network(
      ContainerInfo::DockerInfo::BRIDGE);

  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  ASSERT_TRUE(statusRunning->has_data());

  // Find the DNS configuration of the container and verify
  // if it is consistent with `flags.default_container_dns`.
  string name = containerName(containerId.get());
  Future<Docker::Container> inspect = docker->inspect(name);
  AWAIT_READY(inspect);

  vector<string> defaultDNS;
  std::copy(
      flags.default_container_dns->docker(0).dns().nameservers().begin(),
      flags.default_container_dns->docker(0).dns().nameservers().end(),
      std::back_inserter(defaultDNS));

  EXPECT_EQ(inspect->dns, defaultDNS);

#ifndef __WINDOWS__
  vector<string> defaultDNSSearch;
  std::copy(
      flags.default_container_dns->docker(0).dns().search().begin(),
      flags.default_container_dns->docker(0).dns().search().end(),
      std::back_inserter(defaultDNSSearch));

  EXPECT_EQ(inspect->dnsSearch, defaultDNSSearch);

  vector<string> defaultDNSOption;
  std::copy(
      flags.default_container_dns->docker(0).dns().options().begin(),
      flags.default_container_dns->docker(0).dns().options().end(),
      std::back_inserter(defaultDNSOption));

  EXPECT_EQ(inspect->dnsOptions, defaultDNSOption);
#endif // __WINDOWS__

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}


// This test verifies that the `MESOS_ALLOCATION_ROLE`
// environment variable is set properly.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_AllocationRoleEnvironmentVariable)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  // Start the framework with a role specified.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.clear_roles();
  frameworkInfo.add_roles("role1");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(
      offers->front().slave_id(),
      offers->front().resources(),
#ifdef __WINDOWS__
      "if %MESOS_ALLOCATION_ROLE% == \"role1\" (exit 1)");
#else
      "if [ \"$MESOS_ALLOCATION_ROLE\" != \"role1\" ]; then exit 1; fi");
#endif // __WINDOWS__

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<ContainerID> containerId;
  Future<ContainerConfig> containerConfig;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<1>(&containerConfig),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(containerConfig);
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// Fixture for testing IPv6 support for docker containers on host network.
//
// TODO(akagup): Windows containers do not support IPv6, but they should
// in the future, so enable these when IPv6 is supported. See MESOS-8566.
//
// TODO(asridharan): Currently in the `Setup` and `TearDown` methods
// of this class we re-initialize libprocess to take an IPv6 address.
// Ideally, we should be moving this into a more general test fixture
// in mesos.hpp to be used by any other tests for IPv6. This might
// need changes to `MesosTest` in to order to allow for multiple
// inheritance.
class DockerContainerizerIPv6Test : public DockerContainerizerTest
{
protected:
  void SetUp() override
  {
    os::setenv("LIBPROCESS_IP6", "::1234");
    process::reinitialize(
        None(),
        READWRITE_HTTP_AUTHENTICATION_REALM,
        READONLY_HTTP_AUTHENTICATION_REALM);

    DockerContainerizerTest::SetUp();
  }

  void TearDown() override
  {
    DockerContainerizerTest::TearDown();

    os::unsetenv("LIBPROCESS_IP6");
    process::reinitialize(
        None(),
        READWRITE_HTTP_AUTHENTICATION_REALM,
        READONLY_HTTP_AUTHENTICATION_REALM);
  }
};


// Launches a docker container on the host network. The host network
// is assumed to have an IPv4 address and an IPv6 address. The test
// passes if the Mesos state correctly exposes both the IPv4 and IPv6
// address.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    DockerContainerizerIPv6Test,
    ROOT_DOCKER_LaunchIPv6HostNetwork)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);

  ASSERT_SOME(slave);

  // Check if the slave has the IPv6 address stored in its PID.
  EXPECT_SOME(slave.get()->pid.addresses.v6);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());  // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      SLEEP_COMMAND(10000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo("alpine"));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  ASSERT_TRUE(statusRunning->has_data());

  Try<JSON::Array> array = JSON::parse<JSON::Array>(statusRunning->data());
  ASSERT_SOME(array);

  // Check if container information is exposed through master's state endpoint.
  Future<http::Response> response = http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Value> find = parse->find<JSON::Value>(
      "frameworks[0].tasks[0].container.docker.privileged");

  EXPECT_SOME_FALSE(find);

  // Check if container information is exposed through slave's state endpoint.
  response = http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  find = parse->find<JSON::Value>(
      "frameworks[0].executors[0].tasks[0].container.docker.privileged");

  EXPECT_SOME_FALSE(find);

  // Now verify the ContainerStatus fields in the TaskStatus.
  ASSERT_TRUE(statusRunning->has_container_status());
  EXPECT_TRUE(statusRunning->container_status().has_container_id());
  ASSERT_EQ(1, statusRunning->container_status().network_infos().size());
  EXPECT_EQ(2, statusRunning->container_status().network_infos(0).ip_addresses().size()); // NOLINT(whitespace/line_length)

  Option<string> containerIPv4 = None();
  Option<string> containerIPv6 = None();

  foreach(const NetworkInfo::IPAddress& ipAddress,
          statusRunning->container_status().network_infos(0).ip_addresses()) {
    if (ipAddress.protocol() == NetworkInfo::IPv4) {
      containerIPv4 = ipAddress.ip_address();
    }

    if (ipAddress.protocol() == NetworkInfo::IPv6) {
      containerIPv6 = ipAddress.ip_address();
    }
  }

  EXPECT_SOME(containerIPv4);
  ASSERT_SOME(containerIPv6);
  EXPECT_EQ(containerIPv6.get(), "::1234");

  ASSERT_TRUE(exists(docker, containerId.get()));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  ASSERT_FALSE(
    exists(docker, containerId.get(), ContainerState::RUNNING, false));
}


// Fixture for testing IPv6 support for docker containers on docker
// user network.
class DockerContainerizerIPv6UserNetworkTest : public DockerContainerizerTest
{
protected:
  void SetUp() override
  {
    createDockerIPv6UserNetwork();
    DockerContainerizerTest::SetUp();
  }

  void TearDown() override
  {
    DockerContainerizerTest::TearDown();
    removeDockerIPv6UserNetwork();
  }
};


// Launches a docker container on the docker user network. The docker network
// is assumed to have an IPv4 address and an IPv6 address. The test passes if
// the Mesos state correctly exposes both the IPv4 and IPv6 address.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    DockerContainerizerIPv6UserNetworkTest,
    ROOT_DOCKER_USERNETWORK_LaunchIPv6Container)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);

  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      SLEEP_COMMAND(10000));

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo = createDockerInfo("alpine");

  containerInfo.mutable_docker()->set_network(
      ContainerInfo::DockerInfo::USER);

  // Setup the docker IPv6 network.
  NetworkInfo networkInfo;
  networkInfo.set_name(DOCKER_IPv6_NETWORK);
  containerInfo.add_network_infos()->CopyFrom(networkInfo);

  task.mutable_container()->CopyFrom(containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  ASSERT_TRUE(statusRunning->has_data());

  // Now verify the ContainerStatus fields in the TaskStatus.
  ASSERT_TRUE(statusRunning->has_container_status());
  EXPECT_TRUE(statusRunning->container_status().has_container_id());
  ASSERT_EQ(1, statusRunning->container_status().network_infos().size());
  ASSERT_EQ(2, statusRunning->container_status().network_infos(0).ip_addresses().size()); // NOLINT(whitespace/line_length)

  Option<string> containerIPv4 = None();
  Option<string> containerIPv6 = None();

  foreach(const NetworkInfo::IPAddress& ipAddress,
          statusRunning->container_status().network_infos(0).ip_addresses()) {
    if (ipAddress.protocol() == NetworkInfo::IPv4) {
      containerIPv4 = ipAddress.ip_address();
    }

    if (ipAddress.protocol() == NetworkInfo::IPv6) {
      containerIPv6 = ipAddress.ip_address();
    }
  }

  ASSERT_SOME(containerIPv4);
  ASSERT_SOME(containerIPv6);

  // Check if container information is exposed through slave's state endpoint.
  Future<http::Response> response = http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  // Verify that the slave state information has the same container
  // status as received in the status update message.
  for (int i = 0; i < 2; i++) {
    Result<JSON::String> protocol = parse->find<JSON::String>(
        "frameworks[0].executors[0].tasks[0].statuses[1]"
        ".container_status.network_infos[0].ip_addresses[" +
        stringify(i) + "].protocol");

    ASSERT_SOME(protocol);

    Result<JSON::String> ip = parse->find<JSON::String>(
        "frameworks[0].executors[0].tasks[0].statuses[1]"
        ".container_status.network_infos[0].ip_addresses[" +
        stringify(i) + "].ip_address");

    ASSERT_SOME(ip);

    if (protocol->value == "IPv4") {
      EXPECT_EQ(ip->value, containerIPv4.get());
    } else {
      EXPECT_EQ(ip->value, containerIPv6.get());
    }

    LOG(INFO) << "IP: " << ip->value;
  }

  ASSERT_TRUE(exists(docker, containerId.get()));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  ASSERT_FALSE(
    exists(docker, containerId.get(), ContainerState::RUNNING, false));
}


class HungDockerTest : public DockerContainerizerTest
{
public:
  const string testDockerBinary = "docker";
#ifdef __WINDOWS__
  const string testDockerScript = "test-docker.bat";
  const string testDockerEnvFile = "test-docker-env.bat";
#else
  const string testDockerScript = "test-docker.sh";
  const string testDockerEnvFile = "test-docker.env";
#endif // __WINDOWS__

  string commandsEnv;
  string delayEnv;

  slave::Flags CreateSlaveFlags() override
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();

    flags.docker = path::join(os::getcwd(), testDockerScript);

    return flags;
  }

  void writeEnv()
  {
    // TODO(greggomann): This write operation is not atomic, which means an
    // ill-timed write may cause the shell script to be invoked when this
    // file is in an unintended state. We should make this atomic.

    Try<Nothing> write =
#ifdef __WINDOWS__
      os::write(testDockerEnvFile, commandsEnv + "\r\n" + delayEnv);
#else
      os::write(testDockerEnvFile, commandsEnv + "\n" + delayEnv);
#endif // __WINDOWS__

    ASSERT_SOME(write);
  }

  void setDelayedCommands(const std::vector<string>& commands)
  {
#ifdef __WINDOWS__
    commandsEnv = "set ";
#else
    commandsEnv = "";
#endif // __WINDOWS__

    commandsEnv += "DELAYED_COMMANDS=( ";
    foreach (const string& command, commands) {
      commandsEnv += (command + " ");
    }
    commandsEnv += ")";

    writeEnv();
  }

  void setDelay(const int seconds)
  {
#ifdef __WINDOWS__
    delayEnv = "set ";
#else
    delayEnv = "";
#endif // __WINDOWS__

    delayEnv += "DELAY_SECONDS=" + stringify(seconds);

    writeEnv();
  }

  void SetUp() override
  {
    DockerContainerizerTest::SetUp();

    // Write a wrapper script which allows us to delay Docker commands.
#ifdef __WINDOWS__
    const string dockerScriptText =
      "@echo off\r\n"
      "setlocal enabledelayedexpansion\r\n"
      "call \"" + path::join(os::getcwd(), testDockerEnvFile) + "\"\r\n"
      "set ACTIVE_COMMAND=%3\r\n"
      "if not defined DELAYED_COMMANDS set DELAYED_COMMANDS=()\r\n"
      "for %%G in %DELAYED_COMMANDS% do (\r\n"
      "  if %ACTIVE_COMMAND% == %%G (\r\n"
      "    ping -n %DELAY_SECONDS% 127.0.0.1 > NUL\r\n"
      "  )\r\n"
      ")\r\n" +
      testDockerBinary + " %*\r\n";
#else
    const string dockerScriptText =
      "#!/usr/bin/env bash\n"
      "source " + stringify(path::join(os::getcwd(), testDockerEnvFile)) + "\n"
      "ACTIVE_COMMAND=$3\n"
      "for DELAYED_COMMAND in \"${DELAYED_COMMANDS[@]}\"; do\n"
      "  if [ \"$ACTIVE_COMMAND\" == \"$DELAYED_COMMAND\" ]; then\n"
      "    sleep $DELAY_SECONDS\n"
      "  fi\n"
      "done\n" +
      testDockerBinary + " \"$@\"\n";
#endif // __WINDOWS__

    Try<Nothing> write = os::write(testDockerScript, dockerScriptText);
    ASSERT_SOME(write);

#ifndef __WINDOWS__
    Try<Nothing> chmod = os::chmod(
        testDockerScript, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    ASSERT_SOME(chmod);
#endif // __WINDOWS__

    // Set a very long delay by default to simulate an indefinitely
    // hung Docker daemon.
    setDelay(999999);
  }
};


TEST_F(HungDockerTest, ROOT_DOCKER_InspectHungDuringPull)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // When the 'executor_registration_timeout' elapses, the agent will destroy
  // the container whose 'docker pull' command is stuck. This should cause the
  // launch to fail and the terminal task status update to be sent.
  flags.executor_registration_timeout = Milliseconds(100);

  MockDocker* mockDocker =
    new MockDocker(flags.docker, tests::flags.docker_socket);
  Shared<Docker> docker(mockDocker);

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

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
  ASSERT_FALSE(offers->empty());

  // Causing the 'docker inspect' call preceding the container pull to hang
  // should result in a TASK_FAILED update.
  setDelayedCommands({"inspect"});

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      SLEEP_COMMAND(1000));

  // TODO(tnachen): Use local image to test if possible.
  task.mutable_container()->CopyFrom(createDockerInfo(DOCKER_TEST_IMAGE));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusFailed);
  EXPECT_EQ(TASK_FAILED, statusFailed->state());
  EXPECT_EQ(
      TaskStatus::REASON_CONTAINER_LAUNCH_FAILED,
      statusFailed->reason());

  driver.stop();
  driver.join();
}


// This test is disabled on windows due to the bash-specific
// command used in the task below.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    DockerContainerizerTest, ROOT_DOCKER_OverrideKillPolicy)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker =
    new MockDocker(tests::flags.docker, tests::flags.docker_socket);

  Shared<Docker> docker(mockDocker);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher(flags);

  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  ASSERT_SOME(logger);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockDockerContainerizer dockerContainerizer(
      flags,
      &fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &dockerContainerizer, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

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

  Try<v1::Resources> parsed =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32");

  ASSERT_SOME(parsed);

  v1::Resources resources = parsed.get();

  // Create a task which ignores SIGTERM so that we can detect
  // when the task receives SIGKILL.
  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      resources,
      "trap \"echo 'SIGTERM received'\" SIGTERM; sleep 999999");

  // TODO(tnachen): Use local image to test if possible.
  taskInfo.mutable_container()->CopyFrom(
      evolve(createDockerInfo(DOCKER_TEST_IMAGE)));

  {
    // Set a long grace period on the task's kill policy so that we
    // can detect if the override is effective.
    mesos::v1::DurationInfo gracePeriod;
    gracePeriod.set_nanoseconds(Minutes(10).ns());

    mesos::v1::KillPolicy killPolicy;
    killPolicy.mutable_grace_period()->CopyFrom(gracePeriod);

    taskInfo.mutable_kill_policy()->CopyFrom(killPolicy);
  }

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH({taskInfo})}));

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(startingUpdate, Seconds(60));
  EXPECT_EQ(v1::TASK_STARTING, startingUpdate->status().state());
  AWAIT_READY_FOR(runningUpdate, Seconds(60));
  EXPECT_EQ(v1::TASK_RUNNING, runningUpdate->status().state());

  ASSERT_TRUE(
    exists(docker, containerId.get(), ContainerState::RUNNING));

  Future<v1::scheduler::Event::Update> killedUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&killedUpdate));

  Future<Option<ContainerTermination>> termination =
    dockerContainerizer.wait(containerId.get());

  {
    // Set a short grace period on the kill call so that we
    // can detect if the override is effective.
    mesos::v1::DurationInfo gracePeriod;
    gracePeriod.set_nanoseconds(100);

    mesos::v1::KillPolicy killPolicy;
    killPolicy.mutable_grace_period()->CopyFrom(gracePeriod);

    mesos.send(
        v1::createCallKill(
            frameworkId,
            taskInfo.task_id(),
            agentId,
            killPolicy));
  }

  AWAIT_READY(killedUpdate);
  EXPECT_EQ(v1::TASK_KILLED, killedUpdate->status().state());

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  // Even though the task is killed, the executor should exit gracefully.
  ASSERT_TRUE(termination.get()->has_status());
  EXPECT_EQ(0, termination.get()->status());

  ASSERT_FALSE(
      exists(docker, containerId.get(), ContainerState::RUNNING, false));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
