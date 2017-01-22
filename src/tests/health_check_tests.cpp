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

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include "checks/health_checker.hpp"

#include "docker/docker.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/health_check_test_helper.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_docker.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/docker_archive.hpp"

namespace http = process::http;

using mesos::internal::master::Master;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerLogger;
using mesos::slave::ContainerTermination;

using process::Future;
using process::Owned;
using process::PID;
using process::Shared;

using testing::_;
using testing::AtMost;
using testing::Eq;
using testing::Return;

using std::vector;
using std::queue;
using std::string;
using std::map;

namespace mesos {
namespace internal {
namespace tests {


class HealthCheckTest : public MesosTest
{
public:
  vector<TaskInfo> populateTasks(
      const string& cmd,
      const string& healthCmd,
      const Offer& offer,
      int gracePeriodSeconds = 0,
      const Option<int>& consecutiveFailures = None(),
      const Option<map<string, string>>& env = None(),
      const Option<ContainerInfo>& containerInfo = None(),
      const Option<int>& timeoutSeconds = None())
  {
    CommandInfo healthCommand;
    healthCommand.set_value(healthCmd);

    return populateTasks(
        cmd,
        healthCommand,
        offer,
        gracePeriodSeconds,
        consecutiveFailures,
        env,
        containerInfo,
        timeoutSeconds);
  }

  vector<TaskInfo> populateTasks(
      const string& cmd,
      CommandInfo healthCommand,
      const Offer& offer,
      int gracePeriodSeconds = 0,
      const Option<int>& consecutiveFailures = None(),
      const Option<map<string, string>>& env = None(),
      const Option<ContainerInfo>& containerInfo = None(),
      const Option<int>& timeoutSeconds = None())
  {
    TaskInfo task;
    task.set_name("");
    task.mutable_task_id()->set_value("1");
    task.mutable_slave_id()->CopyFrom(offer.slave_id());
    task.mutable_resources()->CopyFrom(offer.resources());

    CommandInfo command;
    command.set_value(cmd);

    task.mutable_command()->CopyFrom(command);

    if (containerInfo.isSome()) {
      task.mutable_container()->CopyFrom(containerInfo.get());
    }

    HealthCheck healthCheck;

    if (env.isSome()) {
      foreachpair (const string& name, const string& value, env.get()) {
        Environment::Variable* variable =
          healthCommand.mutable_environment()->mutable_variables()->Add();
        variable->set_name(name);
        variable->set_value(value);
      }
    }

    healthCheck.set_type(HealthCheck::COMMAND);
    healthCheck.mutable_command()->CopyFrom(healthCommand);
    healthCheck.set_delay_seconds(0);
    healthCheck.set_interval_seconds(0);
    healthCheck.set_grace_period_seconds(gracePeriodSeconds);

    if (timeoutSeconds.isSome()) {
      healthCheck.set_timeout_seconds(timeoutSeconds.get());
    }

    if (consecutiveFailures.isSome()) {
      healthCheck.set_consecutive_failures(consecutiveFailures.get());
    }

    task.mutable_health_check()->CopyFrom(healthCheck);

    vector<TaskInfo> tasks;
    tasks.push_back(task);

    return tasks;
  }
};


// This tests ensures `HealthCheck` protobuf is validated correctly.
TEST_F(HealthCheckTest, HealthCheckProtobufValidation)
{
  using namespace mesos::internal::checks;

  // Health check type must be set to a known value.
  {
    HealthCheck healthCheckProto;

    Option<Error> validate = validation::healthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.set_type(HealthCheck::UNKNOWN);
    validate = validation::healthCheck(healthCheckProto);
    EXPECT_SOME(validate);
  }

  // The associated with the type health description must be present.
  {
    HealthCheck healthCheckProto;

    healthCheckProto.set_type(HealthCheck::COMMAND);
    Option<Error> validate = validation::healthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.set_type(HealthCheck::HTTP);
    validate = validation::healthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.set_type(HealthCheck::TCP);
    validate = validation::healthCheck(healthCheckProto);
    EXPECT_SOME(validate);
  }

  // Command health check must specify an actual command in `command.value`.
  {
    HealthCheck healthCheckProto;

    healthCheckProto.set_type(HealthCheck::COMMAND);
    healthCheckProto.mutable_command()->CopyFrom(CommandInfo());
    Option<Error> validate = validation::healthCheck(healthCheckProto);
    EXPECT_SOME(validate);
  }

  // HTTP health check may specify a known scheme and a path starting with '/'.
  {
    HealthCheck healthCheckProto;

    healthCheckProto.set_type(HealthCheck::HTTP);
    healthCheckProto.mutable_http()->set_port(8080);

    Option<Error> validate = validation::healthCheck(healthCheckProto);
    EXPECT_NONE(validate);

    healthCheckProto.mutable_http()->set_scheme("ftp");
    validate = validation::healthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.mutable_http()->set_scheme("https");
    healthCheckProto.mutable_http()->set_path("healthz");
    validate = validation::healthCheck(healthCheckProto);
    EXPECT_SOME(validate);
  }
}


// This test creates a healthy task and verifies that the healthy
// status is reflected in the status updates sent as reconciliation
// answers, and in the state endpoint of both the master and the
// agent.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, HealthyTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  vector<TaskInfo> tasks =
    populateTasks("sleep 120", "exit 0", offers.get()[0]);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  Future<TaskStatus> explicitReconciliation;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&explicitReconciliation));

  vector<TaskStatus> statuses;
  TaskStatus status;

  // Send a task status to trigger explicit reconciliation.
  const TaskID taskId = statusHealthy.get().task_id();
  status.mutable_task_id()->CopyFrom(taskId);

  // State is not checked by reconciliation, but is required to be
  // a valid task status.
  status.set_state(TASK_RUNNING);
  statuses.push_back(status);
  driver.reconcileTasks(statuses);

  AWAIT_READY(explicitReconciliation);
  EXPECT_EQ(TASK_RUNNING, explicitReconciliation.get().state());
  EXPECT_TRUE(explicitReconciliation.get().has_healthy());
  EXPECT_TRUE(explicitReconciliation.get().healthy());

  Future<TaskStatus> implicitReconciliation;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&implicitReconciliation));

  // Send an empty vector of task statuses to trigger implicit
  // reconciliation.
  statuses.clear();
  driver.reconcileTasks(statuses);

  AWAIT_READY(implicitReconciliation);
  EXPECT_EQ(TASK_RUNNING, implicitReconciliation.get().state());
  EXPECT_TRUE(implicitReconciliation.get().has_healthy());
  EXPECT_TRUE(implicitReconciliation.get().healthy());

  // Verify that task health is exposed in the master's state endpoint.
  {
    Future<http::Response> response = http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    Result<JSON::Value> find = parse.get().find<JSON::Value>(
        "frameworks[0].tasks[0].statuses[0].healthy");
    EXPECT_SOME_TRUE(find);
  }

  // Verify that task health is exposed in the agent's state endpoint.
  {
    Future<http::Response> response = http::get(
        agent.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    Result<JSON::Value> find = parse.get().find<JSON::Value>(
        "frameworks[0].executors[0].tasks[0].statuses[0].healthy");
    EXPECT_SOME_TRUE(find);
  }

  driver.stop();
  driver.join();
}


// This test creates a healthy task with a container image using mesos
// containerizer and verifies that the healthy status is reported to the
// scheduler and is reflected in the state endpoints of both the master
// and the agent.
TEST_F(HealthCheckTest, ROOT_HealthyTaskWithContainerImage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage = DockerArchive::create(directory, "alpine");
  AWAIT_READY(testImage);

  ASSERT_TRUE(os::exists(path::join(directory, "alpine.tar")));

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.isolation = "docker/runtime,filesystem/linux";
  agentFlags.image_providers = "docker";
  agentFlags.docker_registry = directory;
  agentFlags.docker_store_dir = path::join(os::getcwd(), "store");

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  // Make use of 'populateTasks()' to avoid duplicate code.
  vector<TaskInfo> tasks =
    populateTasks("sleep 120", "exit 0", offers.get()[0]);

  TaskInfo task = tasks[0];

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  HealthCheck* health = task.mutable_health_check();
  health->set_type(HealthCheck::COMMAND);
  health->mutable_command()->set_value("exit 0");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  // Verify that task health is exposed in the master's state endpoint.
  {
    Future<http::Response> response = http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    Result<JSON::Value> find = parse.get().find<JSON::Value>(
        "frameworks[0].tasks[0].statuses[0].healthy");
    EXPECT_SOME_TRUE(find);
  }

  // Verify that task health is exposed in the agent's state endpoint.
  {
    Future<http::Response> response = http::get(
        agent.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
    ASSERT_SOME(parse);

    Result<JSON::Value> find = parse.get().find<JSON::Value>(
        "frameworks[0].executors[0].tasks[0].statuses[0].healthy");
    EXPECT_SOME_TRUE(find);
  }

  driver.stop();
  driver.join();
}


// This test creates a healthy task using the Docker executor and
// verifies that the healthy status is reported to the scheduler.
TEST_F(HealthCheckTest, ROOT_DOCKER_DockerHealthyTask)
{
  Shared<Docker> docker(new MockDocker(
      tests::flags.docker, tests::flags.docker_socket));

  Try<Nothing> validateResult = docker->validateVersion(Version(1, 3, 0));
  ASSERT_SOME(validateResult)
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because of 'docker exec' command \n"
    << "require docker version greater than '1.3.0'. You won't be \n"
    << "able to use the docker exec method, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers.get().size());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  vector<TaskInfo> tasks = populateTasks(
    "sleep 120", "exit 0", offers.get()[0], 0, None(), None(), containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(containerId);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  agent.get()->terminate();
  agent->reset();

  Future<std::list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// Same as above, but use the non-shell version of the health command.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, HealthyTaskNonShell)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  CommandInfo command;
  command.set_shell(false);
  command.set_value("true");
  command.add_arguments("true");

  vector<TaskInfo> tasks =
    populateTasks("sleep 120", command, offers.get()[0]);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().healthy());

  driver.stop();
  driver.join();
}


// This test creates a task whose health flaps, and verifies that the
// health status updates are sent to the framework scheduler.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, HealthStatusChange)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  // Create a temporary file.
  Try<string> temporaryPath = os::mktemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(temporaryPath);
  string tmpPath = temporaryPath.get();

  // This command fails every other invocation.
  // For all runs i in Nat0, the following case i % 2 applies:
  //
  // Case 0:
  //   - Remove the temporary file.
  //
  // Case 1:
  //   - Attempt to remove the nonexistent temporary file.
  //   - Create the temporary file.
  //   - Exit with a non-zero status.
  const string healthCheckCmd =
    "rm " + tmpPath + " || (touch " + tmpPath + " && exit 1)";

  vector<TaskInfo> tasks = populateTasks(
      "sleep 120", healthCheckCmd, offers.get()[0], 0, 3);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;
  Future<TaskStatus> statusUnhealthy;
  Future<TaskStatus> statusHealthyAgain;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillOnce(FutureArg<1>(&statusUnhealthy))
    .WillOnce(FutureArg<1>(&statusHealthyAgain))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().healthy());

  AWAIT_READY(statusUnhealthy);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthy.get().state());
  EXPECT_FALSE(statusUnhealthy.get().healthy());

  AWAIT_READY(statusHealthyAgain);
  EXPECT_EQ(TASK_RUNNING, statusHealthyAgain.get().state());
  EXPECT_TRUE(statusHealthyAgain.get().healthy());

  driver.stop();
  driver.join();
}


// This test creates a task that uses the Docker executor and whose
// health flaps. It then verifies that the health status updates are
// sent to the framework scheduler.
TEST_F(HealthCheckTest, ROOT_DOCKER_DockerHealthStatusChange)
{
  Shared<Docker> docker(new MockDocker(
      tests::flags.docker, tests::flags.docker_socket));

  Try<Nothing> validateResult = docker->validateVersion(Version(1, 3, 0));
  ASSERT_SOME(validateResult)
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because of 'docker exec' command \n"
    << "require docker version greater than '1.3.0'. You won't be \n"
    << "able to use the docker exec method, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers.get().size());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  // Create a temporary file in host and then we could this file to
  // make sure the health check command is run in docker container.
  string tmpPath = path::join(os::getcwd(), "foobar");
  ASSERT_SOME(os::write(tmpPath, "bar"));

  // This command fails every other invocation.
  // For all runs i in Nat0, the following case i % 2 applies:
  //
  // Case 0:
  //   - Attempt to remove the nonexistent temporary file.
  //   - Create the temporary file.
  //   - Exit with a non-zero status.
  //
  // Case 1:
  //   - Remove the temporary file.
  const string healthCheckCmd =
    "rm " + tmpPath + " || "
    "(mkdir -p " + os::getcwd() + " && echo foo >" + tmpPath + " && exit 1)";

  vector<TaskInfo> tasks = populateTasks(
      "sleep 60", healthCheckCmd, offers.get()[0], 0, 3, None(), containerInfo);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusUnhealthy;
  Future<TaskStatus> statusHealthy;
  Future<TaskStatus> statusUnhealthyAgain;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusUnhealthy))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillOnce(FutureArg<1>(&statusUnhealthyAgain))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusUnhealthy);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthy.get().state());
  EXPECT_FALSE(statusUnhealthy.get().healthy());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().healthy());

  AWAIT_READY(statusUnhealthyAgain);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthyAgain.get().state());
  EXPECT_FALSE(statusUnhealthyAgain.get().healthy());

  // Check the temporary file created in host still
  // exists and the content has not changed.
  ASSERT_SOME(os::read(tmpPath));
  EXPECT_EQ("bar", os::read(tmpPath).get());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  agent.get()->terminate();
  agent->reset();

  Future<std::list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// This test ensures that a task is killed if the number of maximum
// health check failures is reached.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, ConsecutiveFailures)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  vector<TaskInfo> tasks = populateTasks(
    "sleep 120", "exit 1", offers.get()[0], 0, 4);

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
  EXPECT_TRUE(statusKilled.get().has_healthy());
  EXPECT_FALSE(statusKilled.get().healthy());

  driver.stop();
  driver.join();
}


// Tests that the task's env variables are copied to the env used to
// execute COMMAND health checks.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, EnvironmentSetup)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  map<string, string> env;
  env["STATUS"] = "0";

  vector<TaskInfo> tasks = populateTasks(
    "sleep 120", "exit $STATUS", offers.get()[0], 0, None(), env);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
  .WillOnce(FutureArg<1>(&statusRunning))
  .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().healthy());

  driver.stop();
  driver.join();
}


// Tests that health check failures are ignored during the grace period.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, GracePeriod)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  // The health check for this task will always fail, but the grace period of
  // 9999 seconds should mask the failures.
  vector<TaskInfo> tasks = populateTasks(
    "sleep 2", "false", offers.get()[0], 9999);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  EXPECT_FALSE(statusRunning.get().has_healthy());

  // No task unhealthy update should be called in grace period.
  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());
  EXPECT_FALSE(statusFinished.get().has_healthy());

  driver.stop();
  driver.join();
}


// This test creates a task with a health check command that will time
// out, and verifies that the health check is retried after the timeout.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, CheckCommandTimeout)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  vector<TaskInfo> tasks = populateTasks(
    "sleep 120", "sleep 120", offers.get()[0], 0, 1, None(), None(), 1);

  // Expecting one unhealthy update and one final kill update.
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusUnhealthy;
  Future<TaskStatus> statusKilled;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusUnhealthy))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusUnhealthy);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthy.get().state());
  EXPECT_FALSE(statusUnhealthy.get().healthy());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled.get().state());
  EXPECT_TRUE(statusKilled.get().has_healthy());
  EXPECT_FALSE(statusKilled.get().healthy());

  driver.stop();
  driver.join();
}


// Tests the transition from healthy to unhealthy within the grace period, to
// make sure that failures within the grace period aren't ignored if they come
// after a success.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HealthCheckTest,
    HealthyToUnhealthyTransitionWithinGracePeriod)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  // Create a temporary file.
  const string tmpPath = path::join(os::getcwd(), "healthyToUnhealthy");

  // This command fails every other invocation.
  // For all runs i in Nat0, the following case i % 2 applies:
  //
  // Case 0:
  //   - Remove the temporary file.
  //
  // Case 1:
  //   - Attempt to remove the nonexistent temporary file.
  //   - Create the temporary file.
  //   - Exit with a non-zero status.
  const string healthCheckCmd =
    "rm " + tmpPath + " || (touch " + tmpPath + " && exit 1)";

  // Set the grace period to 9999 seconds, so that the healthy -> unhealthy
  // transition happens during the grace period.
  vector<TaskInfo> tasks = populateTasks(
      "sleep 120", healthCheckCmd, offers.get()[0], 9999, 0);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;
  Future<TaskStatus> statusUnhealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillOnce(FutureArg<1>(&statusUnhealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  AWAIT_READY(statusUnhealthy);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthy.get().state());
  EXPECT_TRUE(statusUnhealthy.get().has_healthy());
  EXPECT_FALSE(statusUnhealthy.get().healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy non-contained task via HTTP.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, HealthyTaskViaHTTP)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  const uint16_t testPort = getFreePort().get();

  // Use `test-helper` to launch a simple HTTP
  // server to respond to HTTP health checks.
  const string command = strings::format(
      "%s %s --ip=127.0.0.1 --port=%u",
      getTestHelperPath("test-helper"),
      HealthCheckTestHelper::NAME,
      testPort).get();

  TaskInfo task = createTask(offers.get()[0], command);

  // Set `grace_period_seconds` here because it takes some time to
  // launch the HTTP server to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::HTTP);
  healthCheck.mutable_http()->set_port(testPort);
  healthCheck.mutable_http()->set_path("/help");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy task via HTTP without specifying `type`. HTTP health
// checks without `type` are allowed for backwards compatibility with the
// v0 and v1 API.
//
// NOTE: This test is almost identical to HealthyTaskViaHTTP
// with the difference being the health check type is not set.
//
// TODO(haosdent): Remove this after the deprecation cycle which starts in 2.0.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, HealthyTaskViaHTTPWithoutType)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  const uint16_t testPort = getFreePort().get();

  // Use `test-helper` to launch a simple HTTP
  // server to respond to HTTP health checks.
  const string command = strings::format(
      "%s %s --ip=127.0.0.1 --port=%u",
      getTestHelperPath("test-helper"),
      HealthCheckTestHelper::NAME,
      testPort).get();

  TaskInfo task = createTask(offers.get()[0], command);

  // Set `grace_period_seconds` here because it takes some time to
  // launch the HTTP server to serve requests.
  HealthCheck healthCheck;
  healthCheck.mutable_http()->set_port(testPort);
  healthCheck.mutable_http()->set_path("/help");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy non-contained task via TCP.
//
// NOTE: This test is almost identical to HealthyTaskViaHTTP
// with the difference being TCP health check.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HealthCheckTest, HealthyTaskViaTCP)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  const uint16_t testPort = getFreePort().get();

  // Use `test-helper` to launch a simple HTTP
  // server to respond to TCP health checks.
  const string command = strings::format(
      "%s %s --ip=127.0.0.1 --port=%u",
      getTestHelperPath("test-helper"),
      HealthCheckTestHelper::NAME,
      testPort).get();

  TaskInfo task = createTask(offers.get()[0], command);

  // Set `grace_period_seconds` here because it takes some time to
  // launch the HTTP server to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::TCP);
  healthCheck.mutable_tcp()->set_port(testPort);
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy task via HTTP with a container image using mesos
// containerizer. To emulate a task responsive to HTTP health checks,
// starts Netcat in the docker "alpine" image.
TEST_F(HealthCheckTest, ROOT_INTERNET_CURL_HealthyTaskViaHTTPWithContainerImage)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.isolation = "docker/runtime,filesystem/linux";
  agentFlags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  const uint16_t testPort = getFreePort().get();

  // Use Netcat to launch a HTTP server.
  const string command = strings::format(
      "nc -lk -p %u -e echo -e \"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\"",
      testPort).get();

  TaskInfo task = createTask(offers.get()[0], command);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  // Set `grace_period_seconds` here because it takes some time to
  // launch Netcat to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::HTTP);
  healthCheck.mutable_http()->set_port(testPort);
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy task via HTTPS with a container image using mesos
// containerizer. To emulate a task responsive to HTTPS health checks,
// starts an HTTPS server in the docker "haosdent/https-server" image.
TEST_F(HealthCheckTest,
       ROOT_INTERNET_CURL_HealthyTaskViaHTTPSWithContainerImage)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.isolation = "docker/runtime,filesystem/linux";
  agentFlags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  const uint16_t testPort = getFreePort().get();

  const string command = strings::format(
      "python https_server.py %u",
      testPort).get();

  TaskInfo task = createTask(offers.get()[0], command);

  // Refer to https://github.com/haosdent/https-server/ for how the
  // docker image `haosdent/https-server` works.
  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("haosdent/https-server");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  // Set `grace_period_seconds` here because it takes some time to
  // launch the HTTPS server to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::HTTP);
  healthCheck.mutable_http()->set_port(testPort);
  healthCheck.mutable_http()->set_scheme("https");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  // Increase time here to wait for pulling image finish.
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy task via TCP with a container image using mesos
// containerizer. To emulate a task responsive to TCP health checks,
// starts Netcat in the docker "alpine" image.
//
// NOTE: This test is almost identical to
// ROOT_INTERNET_CURL_HealthyTaskViaHTTPWithContainerImage
// with the difference being TCP health check.
TEST_F(HealthCheckTest, ROOT_INTERNET_CURL_HealthyTaskViaTCPWithContainerImage)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.isolation = "docker/runtime,filesystem/linux";
  agentFlags.image_providers = "docker";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

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
  EXPECT_NE(0u, offers.get().size());

  const uint16_t testPort = getFreePort().get();

  // Use Netcat to launch a HTTP server.
  const string command = strings::format(
      "nc -lk -p %u -e echo -e \"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\"",
      testPort).get();

  TaskInfo task = createTask(offers.get()[0], command);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  // Set `grace_period_seconds` here because it takes some time to
  // launch Netcat to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::TCP);
  healthCheck.mutable_tcp()->set_port(testPort);
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy docker task via HTTP. To emulate a task responsive
// to HTTP health checks, starts Netcat in the docker "alpine" image.
TEST_F(HealthCheckTest, ROOT_DOCKER_DockerHealthyTaskViaHTTP)
{
  Shared<Docker> docker(new MockDocker(
      tests::flags.docker, tests::flags.docker_socket));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers.get().size());

  const uint16_t testPort = getFreePort().get();

  // Use Netcat to launch a HTTP server.
  const string command = strings::format(
      "nc -lk -p %u -e echo -e \"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\"",
      testPort).get();

  TaskInfo task = createTask(offers.get()[0], command);

  // The docker container runs in bridge network mode.
  //
  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image("alpine");
  containerInfo.mutable_docker()->set_network(
      ContainerInfo::DockerInfo::BRIDGE);

  task.mutable_container()->CopyFrom(containerInfo);

  // Set `grace_period_seconds` here because it takes some time to
  // launch Netcat to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::HTTP);
  healthCheck.mutable_http()->set_port(testPort);
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  agent.get()->terminate();
  agent->reset();

  Future<std::list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Clean up all mesos launched docker containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// Tests a healthy docker task via HTTPS. To emulate a task responsive
// to HTTPS health checks, starts an HTTPS server in the docker
// "haosdent/https-server" image.
TEST_F(HealthCheckTest, ROOT_DOCKER_DockerHealthyTaskViaHTTPS)
{
  Shared<Docker> docker(new MockDocker(
      tests::flags.docker, tests::flags.docker_socket));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers.get().size());

  const uint16_t testPort = getFreePort().get();

  const string command = strings::format(
      "python https_server.py %u",
      testPort).get();

  TaskInfo task = createTask(offers.get()[0], command);

  // The docker container runs in bridge network mode.
  // Refer to https://github.com/haosdent/https-server/ for how the
  // docker image `haosdent/https-server` works.
  //
  // TODO(haosdent): Use local image to test if possible.
  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image("haosdent/https-server");
  containerInfo.mutable_docker()->set_network(
      ContainerInfo::DockerInfo::BRIDGE);

  task.mutable_container()->CopyFrom(containerInfo);

  // Set `grace_period_seconds` here because it takes some time to
  // launch the HTTPS server to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::HTTP);
  healthCheck.mutable_http()->set_port(testPort);
  healthCheck.mutable_http()->set_scheme("https");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  // Increase time here to wait for pulling image finish.
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  agent.get()->terminate();
  agent->reset();

  Future<std::list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Clean up all mesos launched docker containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// Tests a healthy docker task via TCP. To emulate a task responsive
// to TCP health checks, starts Netcat in the docker "alpine" image.
//
// NOTE: This test is almost identical to ROOT_DOCKER_DockerHealthyTaskViaHTTP
// with the difference being TCP health check.
TEST_F(HealthCheckTest, ROOT_DOCKER_DockerHealthyTaskViaTCP)
{
  Shared<Docker> docker(new MockDocker(
      tests::flags.docker, tests::flags.docker_socket));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();

  Fetcher fetcher;

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
  EXPECT_NE(0u, offers.get().size());

  const uint16_t testPort = getFreePort().get();

  // Use Netcat to launch a HTTP server.
  const string command = strings::format(
      "nc -lk -p %u -e echo -e \"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\"",
      testPort).get();

  TaskInfo task = createTask(offers.get()[0], command);

  // The docker container runs in bridge network mode.
  //
  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image("alpine");
  containerInfo.mutable_docker()->set_network(
      ContainerInfo::DockerInfo::BRIDGE);

  task.mutable_container()->CopyFrom(containerInfo);

  // Set `grace_period_seconds` here because it takes some time to
  // launch Netcat to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::TCP);
  healthCheck.mutable_tcp()->set_port(testPort);
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy.get().state());
  EXPECT_TRUE(statusHealthy.get().has_healthy());
  EXPECT_TRUE(statusHealthy.get().healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  agent.get()->terminate();
  agent->reset();

  Future<std::list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Clean up all mesos launched docker containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
