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
#include <process/owned.hpp>
#include <process/pid.hpp>

#include "docker/docker.hpp"

#include "slave/slave.hpp"

#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "tests/containerizer.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;

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
      const Option<ContainerInfo>& containerInfo = None())
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
        containerInfo);
  }

  vector<TaskInfo> populateTasks(
      const string& cmd,
      CommandInfo healthCommand,
      const Offer& offer,
      int gracePeriodSeconds = 0,
      const Option<int>& consecutiveFailures = None(),
      const Option<map<string, string>>& env = None(),
      const Option<ContainerInfo>& containerInfo = None())
  {
    TaskInfo task;
    task.set_name("");
    task.mutable_task_id()->set_value("1");
    task.mutable_slave_id()->CopyFrom(offer.slave_id());
    task.mutable_resources()->CopyFrom(offer.resources());

    CommandInfo command;
    command.set_value(cmd);

    Environment::Variable* variable =
      command.mutable_environment()->add_variables();

    // We need to set the correct directory to launch health check process
    // instead of the default for tests.
    variable->set_name("MESOS_LAUNCHER_DIR");
    variable->set_value(path::join(tests::flags.build_dir, "src"));

    task.mutable_command()->CopyFrom(command);

    if (containerInfo.isSome()) {
      task.mutable_container()->CopyFrom(containerInfo.get());
    }

    HealthCheck healthCheck;

    if (env.isSome()) {
      foreachpair (const string& name, const string value, env.get()) {
        Environment::Variable* variable =
          healthCommand.mutable_environment()->mutable_variables()->Add();
        variable->set_name(name);
        variable->set_value(value);
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
TEST_F(HealthCheckTest, HealthyTask)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
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

  vector<TaskInfo> tasks =
    populateTasks("sleep 120", "exit 0", offers.get()[0]);

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
  EXPECT_TRUE(statusHealth.get().has_healthy());
  EXPECT_TRUE(statusHealth.get().healthy());

  Future<TaskStatus> explicitReconciliation;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&explicitReconciliation));

  vector<TaskStatus> statuses;
  TaskStatus status;

  // Send a task status to trigger explicit reconciliation.
  const TaskID taskId = statusHealth.get().task_id();
  const SlaveID slaveId = statusHealth.get().slave_id();
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

  driver.stop();
  driver.join();

  Shutdown();
}


// Testing a healthy task reporting one healthy status to scheduler for
// docker executor.
TEST_F(HealthCheckTest, ROOT_DOCKER_DockerHealthyTask)
{
  Owned<Docker> docker(Docker::create(tests::flags.docker, false).get());
  Try<Nothing> validateResult = docker->validateVersion(Version(1, 3, 0));
  ASSERT_SOME(validateResult)
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because of 'docker exec' command \n"
    << "require docker version greater than '1.3.0'. You won't be \n"
    << "able to use the docker exec method, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<DockerContainerizer*> containerizer =
    DockerContainerizer::create(flags, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get());
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

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  vector<TaskInfo> tasks = populateTasks(
    "sleep 120", "exit 0", offers.get()[0], 0, None(), None(), containerInfo);

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
  EXPECT_TRUE(statusHealth.get().has_healthy());
  EXPECT_TRUE(statusHealth.get().healthy());

  driver.stop();
  driver.join();

  Shutdown();

  Future<std::list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// Same as above, but use the non-shell version of the health command.
TEST_F(HealthCheckTest, HealthyTaskNonShell)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
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

  CommandInfo command;
  command.set_shell(false);
  command.set_value("true");
  command.add_arguments("true");

  vector<TaskInfo> tasks =
    populateTasks("sleep 120", command, offers.get()[0]);

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


// Testing health status change reporting to scheduler.
TEST_F(HealthCheckTest, HealthStatusChange)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave> > slave = StartSlave(containerizer.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Create a temporary file.
  Try<string> temporaryPath = os::mktemp();
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
  string alt = "rm " + tmpPath + " || (touch " + tmpPath + " && exit 1)";

  vector<TaskInfo> tasks = populateTasks(
      "sleep 120", alt, offers.get()[0], 0, 3);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealth1;
  Future<TaskStatus> statusHealth2;
  Future<TaskStatus> statusHealth3;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealth1))
    .WillOnce(FutureArg<1>(&statusHealth2))
    .WillOnce(FutureArg<1>(&statusHealth3));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealth1);
  EXPECT_EQ(TASK_RUNNING, statusHealth1.get().state());
  EXPECT_TRUE(statusHealth1.get().healthy());

  AWAIT_READY(statusHealth2);
  EXPECT_EQ(TASK_RUNNING, statusHealth2.get().state());
  EXPECT_FALSE(statusHealth2.get().healthy());

  AWAIT_READY(statusHealth3);
  EXPECT_EQ(TASK_RUNNING, statusHealth3.get().state());
  EXPECT_TRUE(statusHealth3.get().healthy());

  os::rm(tmpPath); // Clean up the temporary file.

  driver.stop();
  driver.join();

  Shutdown();
}


// Testing health status change reporting to scheduler for docker executor.
TEST_F(HealthCheckTest, ROOT_DOCKER_DockerHealthStatusChange)
{
  Owned<Docker> docker(Docker::create(tests::flags.docker, false).get());
  Try<Nothing> validateResult = docker->validateVersion(Version(1, 3, 0));
  ASSERT_SOME(validateResult)
    << "-------------------------------------------------------------\n"
    << "We cannot run this test because of 'docker exec' command \n"
    << "require docker version greater than '1.3.0'. You won't be \n"
    << "able to use the docker exec method, but feel free to disable\n"
    << "this test.\n"
    << "-------------------------------------------------------------";

  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;

  Try<DockerContainerizer*> containerizer =
    DockerContainerizer::create(flags, &fetcher);
  CHECK_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get());
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

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  // TODO(tnachen): Use local image to test if possible.
  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  // Create a temporary file in host and then we could this file to make sure
  // the health check command is run in docker container.
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
  string alt = "rm " + tmpPath + " || (mkdir -p " + os::getcwd() +
               " && echo foo >" + tmpPath + " && exit 1)";

  vector<TaskInfo> tasks = populateTasks(
      "sleep 120", alt, offers.get()[0], 0, 3, None(), containerInfo);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusHealth1;
  Future<TaskStatus> statusHealth2;
  Future<TaskStatus> statusHealth3;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusHealth1))
    .WillOnce(FutureArg<1>(&statusHealth2))
    .WillOnce(FutureArg<1>(&statusHealth3));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusHealth1);
  EXPECT_EQ(TASK_RUNNING, statusHealth1.get().state());
  EXPECT_FALSE(statusHealth1.get().healthy());

  AWAIT_READY(statusHealth2);
  EXPECT_EQ(TASK_RUNNING, statusHealth2.get().state());
  EXPECT_TRUE(statusHealth2.get().healthy());

  AWAIT_READY(statusHealth3);
  EXPECT_EQ(TASK_RUNNING, statusHealth3.get().state());
  EXPECT_FALSE(statusHealth3.get().healthy());

  // Check the temporary file created in host still exists and the content
  // don't change.
  ASSERT_SOME(os::read(tmpPath));
  EXPECT_EQ("bar", os::read(tmpPath).get());

  driver.stop();
  driver.join();

  Shutdown();

  Future<std::list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  // Cleanup all mesos launched containers.
  foreach (const Docker::Container& container, containers.get()) {
    AWAIT_READY_FOR(docker->rm(container.id, true), Seconds(30));
  }
}


// Testing killing task after number of consecutive failures.
TEST_F(HealthCheckTest, ConsecutiveFailures)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
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

  Shutdown();
}


// Testing command using environment variable.
TEST_F(HealthCheckTest, EnvironmentSetup)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
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
    "sleep 120", "exit $STATUS", offers.get()[0], 0, None(), env);

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
TEST_F(HealthCheckTest, DISABLED_GracePeriod)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem";

  Fetcher fetcher;

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher);
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
    "sleep 120", "exit 1", offers.get()[0], 6);

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
