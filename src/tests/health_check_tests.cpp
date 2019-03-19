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
#include <mesos/http.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/v1/master/master.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include "checks/health_checker.hpp"

#include "common/validation.hpp"

#include "docker/docker.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/http_server_test_helper.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_docker.hpp"
#include "tests/resources_utils.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/docker_common.hpp"

#ifdef __linux__
#include "tests/containerizer/docker_archive.hpp"
#endif // __linux__

namespace http = process::http;

using mesos::internal::common::validation::validateHealthCheck;

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

using process::Failure;
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
#ifdef __WINDOWS__
#define HEALTH_CHECK_COMMAND(path) \
  "powershell -NoProfile -Command " \
  "$ri_err = Remove-Item -ErrorAction SilentlyContinue \"" + \
  path + "\"; if (-not $?) { Set-Content -Path (\"" + path + \
  "\") -Value ($null); exit 1 }"
#else
#define HEALTH_CHECK_COMMAND(path) \
  "rm " + path + " || (touch " + path + " && exit 1)"
#endif // __WINDOWS__


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

    Option<Error> validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.set_type(HealthCheck::UNKNOWN);
    validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);
  }

  // The associated with the type health description must be present.
  {
    HealthCheck healthCheckProto;

    healthCheckProto.set_type(HealthCheck::COMMAND);
    Option<Error> validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.set_type(HealthCheck::HTTP);
    validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.set_type(HealthCheck::TCP);
    validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);
  }

  // Duration parameters must be non-negative.
  {
    HealthCheck healthCheckProto;

    healthCheckProto.set_type(HealthCheck::HTTP);
    healthCheckProto.mutable_http()->set_port(8080);

    healthCheckProto.set_delay_seconds(-1.0);
    Option<Error> validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.set_delay_seconds(0.0);
    healthCheckProto.set_interval_seconds(-1.0);
    validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.set_interval_seconds(0.0);
    healthCheckProto.set_timeout_seconds(-1.0);
    validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.set_timeout_seconds(0.0);
    validate = validateHealthCheck(healthCheckProto);
    EXPECT_NONE(validate);
  }

  // Command health check must specify an actual command in `command.value`.
  {
    HealthCheck healthCheckProto;

    healthCheckProto.set_type(HealthCheck::COMMAND);
    healthCheckProto.mutable_command()->CopyFrom(CommandInfo());
    Option<Error> validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);
  }

  // Command health check must specify a command with a valid environment.
  // Environment variable's `value` field must be set in this case.
  {
    HealthCheck healthCheckProto;
    healthCheckProto.set_type(HealthCheck::COMMAND);
    healthCheckProto.mutable_command()->CopyFrom(createCommandInfo("exit 0"));

    Option<Error> validate = validateHealthCheck(healthCheckProto);
    EXPECT_NONE(validate);

    Environment::Variable* variable =
      healthCheckProto.mutable_command()->mutable_environment()
          ->mutable_variables()->Add();
    variable->set_name("ENV_VAR_KEY");

    validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);
  }

  // HTTP health check may specify a known scheme and a path starting with '/'.
  {
    HealthCheck healthCheckProto;

    healthCheckProto.set_type(HealthCheck::HTTP);
    healthCheckProto.mutable_http()->set_port(8080);

    Option<Error> validate = validateHealthCheck(healthCheckProto);
    EXPECT_NONE(validate);

    healthCheckProto.mutable_http()->set_scheme("ftp");
    validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);

    healthCheckProto.mutable_http()->set_scheme("https");
    healthCheckProto.mutable_http()->set_path("healthz");
    validate = validateHealthCheck(healthCheckProto);
    EXPECT_SOME(validate);
  }
}


// This test creates a healthy task and verifies that the healthy
// status is reflected in the status updates sent as reconciliation
// answers, and in the state endpoint of both the master and the
// agent.
TEST_F(HealthCheckTest, HealthyTask)
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
  ASSERT_FALSE(offers->empty());

  vector<TaskInfo> tasks = populateTasks(
      SLEEP_COMMAND(120),
      "exit 0",
      offers.get()[0],
      1,
      1,
      None(),
      None(),
      1);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  Future<TaskStatus> explicitReconciliation;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&explicitReconciliation));

  vector<TaskStatus> statuses;
  TaskStatus status;

  // Send a task status to trigger explicit reconciliation.
  const TaskID taskId = statusHealthy->task_id();
  status.mutable_task_id()->CopyFrom(taskId);

  // State is not checked by reconciliation, but is required to be
  // a valid task status.
  status.set_state(TASK_RUNNING);
  statuses.push_back(status);
  driver.reconcileTasks(statuses);

  AWAIT_READY(explicitReconciliation);
  EXPECT_EQ(TASK_RUNNING, explicitReconciliation->state());
  EXPECT_TRUE(explicitReconciliation->has_healthy());
  EXPECT_TRUE(explicitReconciliation->healthy());

  Future<TaskStatus> implicitReconciliation;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&implicitReconciliation));

  // Send an empty vector of task statuses to trigger implicit
  // reconciliation.
  statuses.clear();
  driver.reconcileTasks(statuses);

  AWAIT_READY(implicitReconciliation);
  EXPECT_EQ(TASK_RUNNING, implicitReconciliation->state());
  EXPECT_TRUE(implicitReconciliation->has_healthy());
  EXPECT_TRUE(implicitReconciliation->healthy());

  // Verify that task's health check definition and current health status
  // are exposed in the master's state endpoint.
  {
    Future<http::Response> response = http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::Value> find = parse->find<JSON::Value>(
        "frameworks[0].tasks[0].statuses[1].healthy");
    EXPECT_SOME_TRUE(find);

    find = parse->find<JSON::Value>(
        "frameworks[0].tasks[0].health_check.type");
    EXPECT_SOME_EQ("COMMAND", find);

    find = parse->find<JSON::Value>(
        "frameworks[0].tasks[0].health_check.command.value");
    EXPECT_SOME_EQ("exit 0", find);

    find = parse->find<JSON::Value>(
        "frameworks[0].tasks[0].health_check.delay_seconds");
    EXPECT_SOME_EQ(0, find);

    find = parse->find<JSON::Value>(
        "frameworks[0].tasks[0].health_check.interval_seconds");
    EXPECT_SOME_EQ(0, find);

    find = parse->find<JSON::Value>(
        "frameworks[0].tasks[0].health_check.timeout_seconds");
    EXPECT_SOME_EQ(1, find);

    find = parse->find<JSON::Value>(
        "frameworks[0].tasks[0].health_check.consecutive_failures");
    EXPECT_SOME_EQ(1u, find);

    find = parse->find<JSON::Value>(
        "frameworks[0].tasks[0].health_check.grace_period_seconds");
    EXPECT_SOME_EQ(1, find);
  }

  // Verify that the task's health definition and current health status
  // are exposed in the agent's state endpoint.
  {
    Future<http::Response> response = http::get(
        agent.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::Value> find = parse->find<JSON::Value>(
        "frameworks[0].executors[0].tasks[0].statuses[1].healthy");
    EXPECT_SOME_TRUE(find);

    find = parse->find<JSON::Value>(
        "frameworks[0].executors[0].tasks[0].health_check.type");
    EXPECT_SOME_EQ("COMMAND", find);

    find = parse->find<JSON::Value>(
        "frameworks[0].executors[0].tasks[0].health_check.command.value");
    EXPECT_SOME_EQ("exit 0", find);
  }

  driver.stop();
  driver.join();
}


#ifdef __linux__
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
  ASSERT_FALSE(offers->empty());

  // Make use of 'populateTasks()' to avoid duplicate code.
  vector<TaskInfo> tasks =
    populateTasks(SLEEP_COMMAND(120), "exit 0", offers.get()[0]);

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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  // Verify that task health is exposed in the master's state endpoint.
  {
    Future<http::Response> response = http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::Value> find = parse->find<JSON::Value>(
        "frameworks[0].tasks[0].statuses[1].healthy");
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

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::Value> find = parse->find<JSON::Value>(
        "frameworks[0].executors[0].tasks[0].statuses[1].healthy");
    EXPECT_SOME_TRUE(find);
  }

  driver.stop();
  driver.join();
}
#endif // __linux__


// Same as above, but use the non-shell version of the health command.
TEST_F(HealthCheckTest, HealthyTaskNonShell)
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
  ASSERT_FALSE(offers->empty());

  CommandInfo command;
  command.set_shell(false);
#ifdef __WINDOWS__
  command.set_value(os::Shell::name);
  command.add_arguments(os::Shell::arg0);
  command.add_arguments(os::Shell::arg1);
  command.add_arguments("exit 0");
#else
  command.set_value("true");
  command.add_arguments("true");
#endif // __WINDOWS__

  vector<TaskInfo> tasks =
    populateTasks(SLEEP_COMMAND(120), command, offers.get()[0]);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->healthy());

  driver.stop();
  driver.join();
}


// This test creates a task whose health flaps, and verifies that the
// health status updates are sent to the framework scheduler.
TEST_F(HealthCheckTest, HealthStatusChange)
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
  ASSERT_FALSE(offers->empty());

  // Create a temporary file.
  Try<string> temporaryPath = os::mktemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(temporaryPath);
  string tmpPath = temporaryPath.get();

  vector<TaskInfo> tasks = populateTasks(
      SLEEP_COMMAND(120),
      HEALTH_CHECK_COMMAND(tmpPath),
      offers.get()[0],
      0,
      3);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;
  Future<TaskStatus> statusUnhealthy;
  Future<TaskStatus> statusHealthyAgain;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillOnce(FutureArg<1>(&statusUnhealthy))
    .WillOnce(FutureArg<1>(&statusHealthyAgain))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->healthy());

  AWAIT_READY(statusUnhealthy);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusUnhealthy->reason());
  EXPECT_FALSE(statusUnhealthy->healthy());

  AWAIT_READY(statusHealthyAgain);
  EXPECT_EQ(TASK_RUNNING, statusHealthyAgain->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthyAgain->reason());
  EXPECT_TRUE(statusHealthyAgain->healthy());

  driver.stop();
  driver.join();
}


// This test ensures that a task is killed if the number of maximum
// health check failures is reached.
TEST_F(HealthCheckTest, ConsecutiveFailures)
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
  ASSERT_FALSE(offers->empty());

  vector<TaskInfo> tasks = populateTasks(
    SLEEP_COMMAND(120), "exit 1", offers.get()[0], 0, 4);

  // Expecting four unhealthy updates and one final kill update.
  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  Future<TaskStatus> status3;
  Future<TaskStatus> status4;
  Future<TaskStatus> statusKilled;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2))
    .WillOnce(FutureArg<1>(&status3))
    .WillOnce(FutureArg<1>(&status4))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      status1->reason());
  EXPECT_FALSE(status1->healthy());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      status2->reason());
  EXPECT_FALSE(status2->healthy());

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_RUNNING, status3->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      status3->reason());
  EXPECT_FALSE(status3->healthy());

  AWAIT_READY(status4);
  EXPECT_EQ(TASK_RUNNING, status4->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      status4->reason());
  EXPECT_FALSE(status4->healthy());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());
  EXPECT_TRUE(statusKilled->has_healthy());
  EXPECT_FALSE(statusKilled->healthy());

  driver.stop();
  driver.join();
}


// Tests that the task's env variables are copied to the env used to
// execute COMMAND health checks.
TEST_F(HealthCheckTest, EnvironmentSetup)
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
  ASSERT_FALSE(offers->empty());

  map<string, string> env;
  env["STATUS"] = "0";

  vector<TaskInfo> tasks = populateTasks(
    SLEEP_COMMAND(120), "exit $STATUS", offers.get()[0], 0, None(), env);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
  .WillOnce(FutureArg<1>(&statusStarting))
  .WillOnce(FutureArg<1>(&statusRunning))
  .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->healthy());

  driver.stop();
  driver.join();
}


// Tests that health check failures are ignored during the grace period.
TEST_F(HealthCheckTest, GracePeriod)
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
  ASSERT_FALSE(offers->empty());

#ifndef __WINDOWS__
  const string falseCommand = "false";
#else
  const string falseCommand = "cmd /C exit 1";
#endif // __WINDOWS__

  // The health check for this task will always fail, but the grace period of
  // 9999 seconds should mask the failures.
  vector<TaskInfo> tasks = populateTasks(
    SLEEP_COMMAND(2), falseCommand, offers.get()[0], 9999);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  EXPECT_FALSE(statusRunning->has_healthy());

  // No task unhealthy update should be called in grace period.
  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());
  EXPECT_FALSE(statusFinished->has_healthy());

  driver.stop();
  driver.join();
}


// This test creates a task with a health check command that will time
// out, and verifies that the health check is retried after the timeout.
TEST_F(HealthCheckTest, CheckCommandTimeout)
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
  ASSERT_FALSE(offers->empty());

  vector<TaskInfo> tasks = populateTasks(
      SLEEP_COMMAND(120),
      SLEEP_COMMAND(120),
      offers.get()[0],
      0,
      1,
      None(),
      None(),
      1);

  // Expecting one unhealthy update and one final kill update.
  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusUnhealthy;
  Future<TaskStatus> statusKilled;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusUnhealthy))
    .WillOnce(FutureArg<1>(&statusKilled));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusUnhealthy);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusUnhealthy->reason());
  EXPECT_FALSE(statusUnhealthy->healthy());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());
  EXPECT_TRUE(statusKilled->has_healthy());
  EXPECT_FALSE(statusKilled->healthy());

  driver.stop();
  driver.join();
}


// Tests the transition from healthy to unhealthy within the grace period, to
// make sure that failures within the grace period aren't ignored if they come
// after a success.
TEST_F(HealthCheckTest, HealthyToUnhealthyTransitionWithinGracePeriod)
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
  ASSERT_FALSE(offers->empty());

  // Create a temporary file.
  const string tmpPath = path::join(os::getcwd(), "healthyToUnhealthy");

  vector<TaskInfo> tasks = populateTasks(
      SLEEP_COMMAND(120),
      HEALTH_CHECK_COMMAND(tmpPath),
      offers.get()[0],
      9999,
      0);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;
  Future<TaskStatus> statusUnhealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillOnce(FutureArg<1>(&statusUnhealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  AWAIT_READY(statusUnhealthy);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusUnhealthy->reason());
  EXPECT_TRUE(statusUnhealthy->has_healthy());
  EXPECT_FALSE(statusUnhealthy->healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy non-contained task via HTTP.
TEST_F(HealthCheckTest, HealthyTaskViaHTTP)
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
  ASSERT_FALSE(offers->empty());

  const uint16_t testPort = getFreePort().get();

  // Use `test-helper` to launch a simple HTTP
  // server to respond to HTTP health checks.
  const string command = strings::format(
      "%s %s --ip=127.0.0.1 --port=%u",
      getTestHelperPath("test-helper"),
      HttpServerTestHelper::NAME,
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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

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
TEST_F(HealthCheckTest, HealthyTaskViaHTTPWithoutType)
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
  ASSERT_FALSE(offers->empty());

  const uint16_t testPort = getFreePort().get();

  // Use `test-helper` to launch a simple HTTP
  // server to respond to HTTP health checks.
  const string command = strings::format(
      "%s %s --ip=127.0.0.1 --port=%u",
      getTestHelperPath("test-helper"),
      HttpServerTestHelper::NAME,
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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy non-contained task via TCP.
//
// NOTE: This test is almost identical to HealthyTaskViaHTTP
// with the difference being TCP health check.
TEST_F(HealthCheckTest, HealthyTaskViaTCP)
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
  ASSERT_FALSE(offers->empty());

  const uint16_t testPort = getFreePort().get();

  // Use `test-helper` to launch a simple HTTP
  // server to respond to TCP health checks.
  const string command = strings::format(
      "%s %s --ip=127.0.0.1 --port=%u",
      getTestHelperPath("test-helper"),
      HttpServerTestHelper::NAME,
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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy task via HTTP with a container image using mesos
// containerizer. To emulate a task responsive to HTTP health checks,
// starts Netcat in the docker "alpine" image.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HealthCheckTest, ROOT_INTERNET_CURL_HealthyTaskViaHTTPWithContainerImage)
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
  ASSERT_FALSE(offers->empty());

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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  driver.stop();
  driver.join();
}


// Tests a healthy task via HTTPS with a container image using mesos
// containerizer. To emulate a task responsive to HTTPS health checks,
// starts an HTTPS server in the docker "haosdent/https-server" image.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HealthCheckTest, ROOT_INTERNET_CURL_HealthyTaskViaHTTPSWithContainerImage)
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
  ASSERT_FALSE(offers->empty());

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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  // Increase time here to wait for pulling image finish.
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

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
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HealthCheckTest, ROOT_INTERNET_CURL_HealthyTaskViaTCPWithContainerImage)
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
  ASSERT_FALSE(offers->empty());

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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  driver.stop();
  driver.join();
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HealthCheckTest, DefaultExecutorCommandHealthCheck)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

#ifndef USE_SSL_SOCKET
  // Set permissive ACLs in the agent so that the local authorizer will be
  // loaded and implicit executor authorization will be tested.
  ACLs acls;
  acls.set_permissive(true);

  flags.acls = acls;
#endif // USE_SSL_SOCKET

  Fetcher fetcher(flags);

  // We have to explicitly create a `Containerizer` in non-local mode,
  // because `LaunchNestedContainerSession` (used by command health
  // checks) tries to start a IO switchboard, which doesn't work in
  // local mode yet.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(agent);

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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  TaskInfo task = createTask(offers->front(), "sleep 120");

  HealthCheck healthCheck;

  healthCheck.set_type(HealthCheck::COMMAND);
  healthCheck.mutable_command()->set_value("exit $STATUS");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(0);

  Environment::Variable* variable = healthCheck.mutable_command()->
    mutable_environment()->mutable_variables()->Add();
  variable->set_name("STATUS");
  variable->set_value("0");

  task.mutable_health_check()->CopyFrom(healthCheck);

  Resources executorResources =
    allocatedResources(Resources::parse("cpus:0.1;mem:32;disk:32").get(), "*");

  task.mutable_resources()->CopyFrom(task.resources() - executorResources);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task);

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_framework_id()->CopyFrom(frameworkId.get());
  executor.mutable_resources()->CopyFrom(executorResources);
  executor.mutable_shutdown_grace_period()->set_nanoseconds(Seconds(10).ns());

  driver.acceptOffers(
      {offers->front().id()}, {LAUNCH_GROUP(executor, taskGroup)});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  Future<hashset<ContainerID>> containerIds = containerizer->containers();

  AWAIT_READY(containerIds);

  driver.stop();
  driver.join();

  // Cleanup all mesos launched containers.
  foreach (const ContainerID& containerId, containerIds.get()) {
    AWAIT_READY(containerizer->wait(containerId));
  }
}

#ifdef __linux__
// Tests a healthy docker task via CMD health checks using the
// DefaultExecutor.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HealthCheckTest, ROOT_DefaultExecutorWithDockerImageCommandHealthCheck)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

#ifndef USE_SSL_SOCKET
  // Set permissive ACLs in the agent so that the local authorizer will be
  // loaded and implicit executor authorization will be tested.
  ACLs acls;
  acls.set_permissive(true);

  flags.acls = acls;
#endif // USE_SSL_SOCKET

  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage = DockerArchive::create(directory, "alpine");
  AWAIT_READY(testImage);
  ASSERT_TRUE(os::exists(path::join(directory, "alpine.tar")));

  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_registry = directory;
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Fetcher fetcher(flags);

  // We have to explicitly create a `Containerizer` in non-local mode,
  // because `LaunchNestedContainerSession` (used by command health
  // checks) tries to start a IO switchboard, which doesn't work in
  // local mode yet.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(agent);

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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  TaskInfo task = createTask(offers->front(), "sleep 120");

  // TODO(tnachen): Use local image to test if possible.
  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_mesos()->mutable_image()->CopyFrom(image);

  task.mutable_container()->CopyFrom(containerInfo);

  HealthCheck healthCheck;

  healthCheck.set_type(HealthCheck::COMMAND);
  healthCheck.mutable_command()->set_value("exit $STATUS");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(0);

  Environment::Variable* variable = healthCheck.mutable_command()->
    mutable_environment()->mutable_variables()->Add();
  variable->set_name("STATUS");
  variable->set_value("0");

  task.mutable_health_check()->CopyFrom(healthCheck);

  Resources executorResources =
    allocatedResources(Resources::parse("cpus:0.1;mem:32;disk:32").get(), "*");

  task.mutable_resources()->CopyFrom(task.resources() - executorResources);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task);

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_framework_id()->CopyFrom(frameworkId.get());
  executor.mutable_resources()->CopyFrom(executorResources);
  executor.mutable_shutdown_grace_period()->set_nanoseconds(Seconds(10).ns());

  driver.acceptOffers(
      {offers->front().id()}, {LAUNCH_GROUP(executor, taskGroup)});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  Future<hashset<ContainerID>> containerIds = containerizer->containers();

  AWAIT_READY(containerIds);

  driver.stop();
  driver.join();

  // Cleanup all mesos launched containers.
  foreach (const ContainerID& containerId, containerIds.get()) {
    AWAIT_READY(containerizer->wait(containerId));
  }
}
#endif  // __linux__


// This test verifies that the debug container launched by the command health
// check will be run as the user of its parent container rather than the user
// of the default executor. This is a regression test for MESOS-9332.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
    HealthCheckTest, ROOT_UNPRIVILEGED_USER_DefaultExecutorCommandHealthCheck)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

#ifndef USE_SSL_SOCKET
  // Set permissive ACLs in the agent so that the local authorizer will be
  // loaded and implicit executor authorization will be tested.
  ACLs acls;
  acls.set_permissive(true);

  flags.acls = acls;
#endif // USE_SSL_SOCKET

  Fetcher fetcher(flags);

  // We have to explicitly create a `Containerizer` in non-local mode,
  // because `LaunchNestedContainerSession` (used by command health
  // checks) tries to start a IO switchboard, which doesn't work in
  // local mode yet.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::Containerizer> containerizer(_containerizer.get());
  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> agent =
    StartSlave(detector.get(), containerizer.get(), flags);

  ASSERT_SOME(agent);

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  // Set the framework user to a non-root user so the default executor will
  // also be run as this non-root user since the default executor always runs
  // as the same user of the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_user(user.get());

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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  // Launch a task as root user to create a file.
  TaskInfo task = createTask(offers->front(), "touch file && sleep 120");
  task.mutable_command()->set_user("root");

  // Run the command health check without specifying user in its `CommandInfo`,
  // and the health check will try to write to the file created by the task.
  // Since the debug container launched by the health check will run as the same
  // user (i.e., root user) of its parent container (i.e., the task), it will
  // have the permission to write to the file.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::COMMAND);
  healthCheck.mutable_command()->set_value("echo abc > file");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(0);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Resources executorResources =
    allocatedResources(Resources::parse("cpus:0.1;mem:32;disk:32").get(), "*");

  task.mutable_resources()->CopyFrom(task.resources() - executorResources);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task);

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_framework_id()->CopyFrom(frameworkId.get());
  executor.mutable_resources()->CopyFrom(executorResources);
  executor.mutable_shutdown_grace_period()->set_nanoseconds(Seconds(10).ns());

  driver.acceptOffers(
      {offers->front().id()}, {LAUNCH_GROUP(executor, taskGroup)});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  Future<hashset<ContainerID>> containerIds = containerizer->containers();

  AWAIT_READY(containerIds);

  driver.stop();
  driver.join();

  // Cleanup all mesos launched containers.
  foreach (const ContainerID& containerId, containerIds.get()) {
    AWAIT_READY(containerizer->wait(containerId));
  }
}


// Fixture for testing TCP/HTTP(S) health check support
// for Docker containers on Docker user network.
class DockerContainerizerHealthCheckTest
  : public MesosTest,
    public ::testing::WithParamInterface<NetworkInfo::Protocol>
{
protected:
  void SetUp() override
  {
    MesosTest::SetUp();

    Future<std::tuple<Nothing, Nothing, Nothing>> pulls = process::collect(
        pullDockerImage(DOCKER_TEST_IMAGE),
        pullDockerImage(DOCKER_HTTP_IMAGE),
        pullDockerImage(DOCKER_HTTPS_IMAGE));

    // The pull should only need to happen once since we don't delete the
    // image. So, we only log the warning once.
    LOG_FIRST_N(WARNING, 1) << "Pulling " << string(DOCKER_TEST_IMAGE) << ", "
                            << string(DOCKER_HTTP_IMAGE) << " and "
                            << string(DOCKER_HTTPS_IMAGE) << ". "
                            << "This might take a while...";

    // The Windows images are ~200 MB, while the Linux images are ~2MB, so
    // hopefully this is enough time for the Windows images. There should
    // be some parallelism too, since we're pulling them simultaneously and
    // they share the same base Windows layer.
    AWAIT_READY_FOR(pulls, Minutes(10));

    createDockerIPv6UserNetwork();
  }

  void TearDown() override
  {
    Try<Owned<Docker>> docker = Docker::create(
        tests::flags.docker,
        tests::flags.docker_socket,
        false);

    ASSERT_SOME(docker);

    Future<std::vector<Docker::Container>> containers =
      docker.get()->ps(true, slave::DOCKER_NAME_PREFIX);

    AWAIT_READY(containers);

    // Cleanup all mesos launched containers.
    foreach (const Docker::Container& container, containers.get()) {
      AWAIT_READY_FOR(docker.get()->rm(container.id, true), Seconds(30));
    }

    removeDockerIPv6UserNetwork();

    MesosTest::TearDown();
  }
};


// The tests are parameterized by the network protocol.
// On Windows, Docker IPv6 doesn't work, so just do IPv4.
#ifdef __WINDOWS__
INSTANTIATE_TEST_CASE_P(
    NetworkProtocol,
    DockerContainerizerHealthCheckTest,
    ::testing::Values(NetworkInfo::IPv4));
#else
INSTANTIATE_TEST_CASE_P(
    NetworkProtocol,
    DockerContainerizerHealthCheckTest,
    ::testing::Values(NetworkInfo::IPv4, NetworkInfo::IPv6));
#endif // __WINDOWS__


// Tests a healthy Docker task via HTTP. To emulate a task responsive
// to HTTP health checks, starts Netcat in the Docker "alpine" image.
TEST_P(
    DockerContainerizerHealthCheckTest,
    ROOT_DOCKER_USERNETWORK_NETNAMESPACE_HealthyTaskViaHTTP)
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

  TaskInfo task = createTask(offers.get()[0], DOCKER_HTTP_COMMAND);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image(DOCKER_HTTP_IMAGE);
#ifndef __WINDOWS__
  containerInfo.mutable_docker()->set_network(ContainerInfo::DockerInfo::USER);

  // Setup the Docker IPv6 network.
  NetworkInfo networkInfo;
  networkInfo.set_name(DOCKER_IPv6_NETWORK);
  containerInfo.add_network_infos()->CopyFrom(networkInfo);
#endif // __WINDOWS__

  task.mutable_container()->CopyFrom(containerInfo);

  // Set `grace_period_seconds` here because it takes some time to
  // launch Netcat to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::HTTP);
  healthCheck.mutable_http()->set_port(80);
  healthCheck.mutable_http()->set_protocol(GetParam());
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());

  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}


// Tests a healthy Docker task via HTTPS. To emulate a task
// responsive to HTTPS health checks, starts an HTTPS server
// in the Docker "zhq527725/https-server" image. On Windows,
// it runs an HTTPS IIS server.
TEST_P(
    DockerContainerizerHealthCheckTest,
    ROOT_DOCKER_USERNETWORK_NETNAMESPACE_HealthyTaskViaHTTPS)
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

  TaskInfo task = createTask(offers.get()[0], DOCKER_HTTPS_COMMAND);

  // TODO(qianzhang): Use local image to test if possible.
  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image(DOCKER_HTTPS_IMAGE);
#ifndef __WINDOWS__
  containerInfo.mutable_docker()->set_network(ContainerInfo::DockerInfo::USER);

  // Setup the Docker IPv6 network.
  NetworkInfo networkInfo;
  networkInfo.set_name(DOCKER_IPv6_NETWORK);
  containerInfo.add_network_infos()->CopyFrom(networkInfo);
#endif // __WINDOWS__

  task.mutable_container()->CopyFrom(containerInfo);

  // Set `grace_period_seconds` here because it takes some time to
  // launch the HTTPS server to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::HTTP);
  healthCheck.mutable_http()->set_port(443);
  healthCheck.mutable_http()->set_scheme("https");
  healthCheck.mutable_http()->set_protocol(GetParam());
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  // Increase time here to wait for pulling image finish.
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}


// Tests a healthy Docker task via TCP. To emulate a task responsive
// to TCP health checks, starts Netcat in the Docker "alpine" image.
//
// NOTE:
// This test is almost identical to ROOT_DOCKER_USERNETWORK_HealthyTaskViaHTTP
// with the difference being TCP health check.
TEST_P(
    DockerContainerizerHealthCheckTest,
    ROOT_DOCKER_USERNETWORK_NETNAMESPACE_HealthyTaskViaTCP)
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

  // Since HTTP runs over TCP, we can just run the HTTP server.
  TaskInfo task = createTask(offers.get()[0], DOCKER_HTTP_COMMAND);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image(DOCKER_HTTP_IMAGE);
#ifndef __WINDOWS__
  containerInfo.mutable_docker()->set_network(ContainerInfo::DockerInfo::USER);

  // Setup the Docker IPv6 network.
  NetworkInfo networkInfo;
  networkInfo.set_name(DOCKER_IPv6_NETWORK);
  containerInfo.add_network_infos()->CopyFrom(networkInfo);
#endif // __WINDOWS__

  task.mutable_container()->CopyFrom(containerInfo);

  // Set `grace_period_seconds` here because it takes some time to
  // launch Netcat to serve requests.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::TCP);
  healthCheck.mutable_tcp()->set_port(80);
  healthCheck.mutable_tcp()->set_protocol(GetParam());
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&containerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());
}

// This test creates a healthy task using the Docker executor and
// verifies that the healthy status is reported to the scheduler.
TEST_F(DockerContainerizerHealthCheckTest, ROOT_DOCKER_DockerHealthyTask)
{
  Shared<Docker> docker(
      new MockDocker(tests::flags.docker, tests::flags.docker_socket));

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(120));

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image(DOCKER_TEST_IMAGE);

  task.mutable_container()->CopyFrom(containerInfo);

  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::COMMAND);
  healthCheck.mutable_command()->set_value("exit 0");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(0);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(
        FutureArg<0>(&containerId),
        Invoke(&containerizer, &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  agent.get()->terminate();
  agent->reset();

  Future<std::vector<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}

// This test creates a task that uses the Docker executor and whose
// health flaps. It then verifies that the health status updates are
// sent to the framework scheduler.
TEST_F(DockerContainerizerHealthCheckTest, ROOT_DOCKER_DockerHealthStatusChange)
{
  Shared<Docker> docker(
      new MockDocker(tests::flags.docker, tests::flags.docker_socket));

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(120));

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image(DOCKER_TEST_IMAGE);

  task.mutable_container()->CopyFrom(containerInfo);

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
  //
  // NOTE: On Windows, we delete a temporary directory instead since `del`
  // doesn't return an error if it tries to delete a nonexistent file, but
  // `rmdir` does. Also, we hard code a path starting with `C:\` instead
  // of using `tmpPath` since the path might not be possible to make inside
  // the container (for example, if the `tmpPath` is in the `D:\` drive).
#ifdef __WINDOWS__
  const string dockerPath = path::join("C:", id::UUID::random().toString());
  const string healthCheckCmd =
    "rmdir /s /q " + dockerPath + " || (mkdir " + dockerPath + " && exit 1)";
#else
  const string healthCheckCmd =
    "rm " + tmpPath + " || "
    "(mkdir -p " + os::getcwd() + " && echo foo >" + tmpPath + " && exit 1)";
#endif // __WINDOWS__

  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::COMMAND);
  healthCheck.mutable_command()->set_value(healthCheckCmd);
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(0);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(
        FutureArg<0>(&containerId),
        Invoke(&containerizer, &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusUnhealthy;
  Future<TaskStatus> statusHealthy;
  Future<TaskStatus> statusUnhealthyAgain;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusUnhealthy))
    .WillOnce(FutureArg<1>(&statusHealthy))
    .WillOnce(FutureArg<1>(&statusUnhealthyAgain))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusUnhealthy);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusUnhealthy->reason());
  EXPECT_FALSE(statusUnhealthy->healthy());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->healthy());

  AWAIT_READY(statusUnhealthyAgain);
  EXPECT_EQ(TASK_RUNNING, statusUnhealthyAgain->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusUnhealthyAgain->reason());
  EXPECT_FALSE(statusUnhealthyAgain->healthy());

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

  Future<std::vector<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// This test verifies that tasks with quotes in the `COMMAND` health check
// work for the Docker executor.
TEST_F(
    DockerContainerizerHealthCheckTest,
    ROOT_DOCKER_DockerHealthyTaskWithQuotedCommand)
{
  Shared<Docker> docker(
      new MockDocker(tests::flags.docker, tests::flags.docker_socket));

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

  TaskInfo task = createTask(offers.get()[0], SLEEP_COMMAND(120));

  // TODO(akagup): Use local image to test if possible.
  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);
  containerInfo.mutable_docker()->set_image(DOCKER_TEST_IMAGE);

  task.mutable_container()->CopyFrom(containerInfo);

  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::COMMAND);
  healthCheck.mutable_command()->set_value("echo '\"'");
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(0);

  task.mutable_health_check()->CopyFrom(healthCheck);

  Future<ContainerID> containerId;
  EXPECT_CALL(containerizer, launch(_, _, _, _))
    .WillOnce(DoAll(
        FutureArg<0>(&containerId),
        Invoke(&containerizer, &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealthy;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealthy));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(containerId);

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusHealthy);
  EXPECT_EQ(TASK_RUNNING, statusHealthy->state());
  EXPECT_EQ(
      TaskStatus::REASON_TASK_HEALTH_CHECK_STATUS_UPDATED,
      statusHealthy->reason());
  EXPECT_TRUE(statusHealthy->has_healthy());
  EXPECT_TRUE(statusHealthy->healthy());

  Future<Option<ContainerTermination>> termination =
    containerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);
  EXPECT_SOME(termination.get());

  agent.get()->terminate();
  agent->reset();

  Future<std::vector<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
