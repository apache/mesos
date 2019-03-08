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

#include <vector>

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <mesos/v1/scheduler.hpp>

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "tests/mesos.hpp"

using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Event;

using process::Clock;
using process::Future;
using process::Owned;

using std::string;
using std::vector;

using testing::AtMost;
using testing::DoAll;

namespace process {

void reinitialize(
    const Option<string>& delegate,
    const Option<string>& readonlyAuthenticationRealm,
    const Option<string>& readwriteAuthenticationRealm);

} // namespace process {


namespace mesos {
namespace internal {
namespace tests {

class VolumeGidManagerTest : public ContainerizerTest<MesosContainerizer> {};


// This test verifies that agent will fail to start if the flag
// `--volume_gid_range` is specified with an invalid value.
TEST_F(VolumeGidManagerTest, ROOT_Flag)
{
  slave::Flags flags;

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Volume gids must be a range.
  flags = CreateSlaveFlags();
  flags.volume_gid_range = "foo";
  ASSERT_ERROR(StartSlave(detector.get(), flags));

  // Volume gids must be a range.
  flags = CreateSlaveFlags();
  flags.volume_gid_range = "10000";
  ASSERT_ERROR(StartSlave(detector.get(), flags));

  // Volume gids are 32 bit.
  flags = CreateSlaveFlags();
  flags.volume_gid_range = "[10000-1099511627776]";
  ASSERT_ERROR(StartSlave(detector.get(), flags));

  // Volume gid range cannot be empty.
  flags = CreateSlaveFlags();
  flags.volume_gid_range = "[10000-5000]";
  ASSERT_ERROR(StartSlave(detector.get(), flags));
}


// This test verifies that two command tasks launched with a non-root
// user try to write to the same shared persistent volume and the
// volume will only be allocated with gid once rather than twice.
TEST_F(VolumeGidManagerTest, ROOT_UNPRIVILEGED_USER_GidReused)
{
  // Reinitialize libprocess to ensure volume gid manager's metrics
  // can be added in each iteration of this test (i.e., run this test
  // repeatedly with the `--gtest_repeat` option).
  process::reinitialize(None(), None(), None());

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:4;mem:256;disk(role1):128";
  flags.volume_gid_range = "[10000-20000]";

  if (flags.launcher == "linux") {
    flags.isolation = "filesystem/linux";
  } else {
    flags.isolation = "filesystem/posix";

    // Agent's work directory is created with the mode 0700, here
    // we change their modes to 0711 to ensure the non-root user
    // used to launch the command task can enter it.
    ASSERT_SOME(os::chmod(flags.work_dir, 0711));
  }

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");
  frameworkInfo.set_checkpoint(true);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  // Create a shared persistent volume which shall be used
  // by the task to write to that volume.
  Resource volume = createPersistentVolume(
      Megabytes(4),
      "role1",
      "id1",
      "volume_path",
      None(),
      None(),
      frameworkInfo.principal(),
      true); // Shared volume.

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  CommandInfo command = createCommandInfo(
        "echo hello > volume_path/file && sleep 1000");

  command.set_user(user.get());

  Resources taskResources =
    Resources::parse("cpus:1;mem:64;disk(role1):1").get() + volume;

  // Launched the first task.
  TaskInfo task = createTask(
      offers1.get()[0].slave_id(),
      taskResources,
      command);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  driver.acceptOffers(
      {offers1.get()[0].id()},
      {CREATE(volume),
       LAUNCH({task})});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // One gid should have been allocated to the volume.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_total")
        ->as<int>() - 1,
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_free")
        ->as<int>());

  string volumePath =
    slave::paths::getPersistentVolumePath(flags.work_dir, volume);

  // The owner group of the volume should be changed to the gid allocated
  // to it, i.e., the first gid in the agent flag `--volume_gid_range`.
  struct stat s;
  EXPECT_EQ(0, ::stat(volumePath.c_str(), &s));
  EXPECT_EQ(10000u, s.st_gid);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  // Launch the second task.
  task = createTask(
      offers2.get()[0].slave_id(),
      taskResources,
      command);

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  driver.launchTasks(offers2.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Still one gid is allocated.
  metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_total")
        ->as<int>() - 1,
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_free")
        ->as<int>());

  // The owner group of the volume should still be the
  // one allocated when the first task was launched.
  EXPECT_EQ(0, ::stat(volumePath.c_str(), &s));
  EXPECT_EQ(10000u, s.st_gid);

  driver.stop();
  driver.join();
}


#ifdef __linux__
// This test verifies that a nested container which is launched with a
// non-root user and has checkpoint enabled can write to a PARENT type
// SANDBOX_PATH volume and the owner group of the volume will be changed
// to the first gid in the agent flag `--volume_gid_range`. After the
// agent is restarted and the task is killed, the owner group of the
// volume will be changed back to the original one.
TEST_F(VolumeGidManagerTest, ROOT_UNPRIVILEGED_USER_SlaveRecovery)
{
  // Reinitialize libprocess to ensure volume gid manager's metrics
  // can be added in each iteration of this test (i.e., run this test
  // repeatedly with the `--gtest_repeat` option).
  process::reinitialize(None(), None(), None());

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;
  flags.isolation = "filesystem/linux,volume/sandbox_path";
  flags.volume_gid_range = "[10000-20000]";

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start the slave with a static process ID. This allows the executor to
  // reconnect with the slave upon a process restart.
  const string id("agent");

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), id, flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  v1::CommandInfo command =
    v1::createCommandInfo("echo 'hello' > task_volume_path/file && sleep 1000");

  command.set_user(user.get());

  // Launch a task with a non-root user and a PARENT type SANDBOX_PATH volume.
  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      resources,
      command);

  mesos::v1::ContainerInfo* containerInfo = taskInfo.mutable_container();
  containerInfo->set_type(mesos::v1::ContainerInfo::MESOS);

  mesos::v1::Volume* taskVolume = containerInfo->add_volumes();
  taskVolume->set_mode(mesos::v1::Volume::RW);
  taskVolume->set_container_path("task_volume_path");

  mesos::v1::Volume::Source* source = taskVolume->mutable_source();
  source->set_type(mesos::v1::Volume::Source::SANDBOX_PATH);

  mesos::v1::Volume::Source::SandboxPath* sandboxPath =
    source->mutable_sandbox_path();

  sandboxPath->set_type(mesos::v1::Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("executor_volume_path");

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  Future<Event::Update> updateKilled;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        agentId)))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        agentId)))
    .WillOnce(FutureArg<1>(&updateKilled));

  Future<Nothing> ackStarting =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> ackRunning =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(ackStarting);

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  AWAIT_READY(ackRunning);

  // One gid should have been allocated to the volume.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_total")
        ->as<int>() - 1,
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_free")
        ->as<int>());

  string executorSandbox = slave::paths::getExecutorLatestRunPath(
      flags.work_dir,
      devolve(agentId),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()));

  string volumePath = path::join(executorSandbox, "executor_volume_path");

  // The owner group of the volume should be changed to the gid allocated
  // to it, i.e., the first gid in the agent flag `--volume_gid_range`.
  struct stat s;
  EXPECT_EQ(0, ::stat(volumePath.c_str(), &s));
  EXPECT_EQ(10000u, s.st_gid);

  // Stop the slave.
  slave.get()->terminate();
  slave->reset();

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  // Restart the slave.
  slave = StartSlave(detector.get(), id, flags);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  Future<v1::scheduler::Event::Failure> executorFailure;
  EXPECT_CALL(*scheduler, failure(_, _))
    .WillOnce(FutureArg<1>(&executorFailure));

  // Kill the task.
  mesos.send(v1::createCallKill(frameworkId, taskInfo.task_id()));

  AWAIT_READY(updateKilled);
  ASSERT_EQ(v1::TASK_KILLED, updateKilled->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateKilled->status().task_id());

  // The executor should commit suicide after the task have been killed.
  AWAIT_READY(executorFailure);

  // Even though the task was killed, the executor should exit gracefully.
  ASSERT_TRUE(executorFailure->has_status());
  ASSERT_EQ(0, executorFailure->status());

  // The owner group of the volume should be changed back to
  // the original one, i.e., root.
  EXPECT_EQ(0, ::stat(volumePath.c_str(), &s));
  EXPECT_EQ(0u, s.st_gid);
}


// This test verifies that a nested container which is launched with a
// non-root user and has checkpoint enabled can write to a PARENT type
// SANDBOX_PATH volume and the owner group of the volume will be changed
// to the first gid in the agent flag `--volume_gid_range`. After the
// agent is rebooted, the owner group of the volume will be changed back
// to the original one.
TEST_F(VolumeGidManagerTest, ROOT_UNPRIVILEGED_USER_SlaveReboot)
{
  // Reinitialize libprocess to ensure volume gid manager's metrics
  // can be added in each iteration of this test (i.e., run this test
  // repeatedly with the `--gtest_repeat` option).
  process::reinitialize(None(), None(), None());

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;
  flags.isolation = "filesystem/linux,volume/sandbox_path";
  flags.volume_gid_range = "[10000-20000]";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  v1::CommandInfo command =
    v1::createCommandInfo("echo 'hello' > task_volume_path/file && sleep 1000");

  command.set_user(user.get());

  // Launch a task with a non-root user and a PARENT type SANDBOX_PATH volume.
  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      resources,
      command);

  mesos::v1::ContainerInfo* containerInfo = taskInfo.mutable_container();
  containerInfo->set_type(mesos::v1::ContainerInfo::MESOS);

  mesos::v1::Volume* taskVolume = containerInfo->add_volumes();
  taskVolume->set_mode(mesos::v1::Volume::RW);
  taskVolume->set_container_path("task_volume_path");

  mesos::v1::Volume::Source* source = taskVolume->mutable_source();
  source->set_type(mesos::v1::Volume::Source::SANDBOX_PATH);

  mesos::v1::Volume::Source::SandboxPath* sandboxPath =
    source->mutable_sandbox_path();

  sandboxPath->set_type(mesos::v1::Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("executor_volume_path");

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        agentId)))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        agentId)))
    .WillRepeatedly(Return());

  Future<Nothing> ackStarting =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> ackRunning =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(ackStarting);

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  AWAIT_READY(ackRunning);

  // One gid should have been allocated to the volume.
  JSON::Object metrics = Metrics();
  EXPECT_EQ(
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_total")
        ->as<int>() - 1,
      metrics.at<JSON::Number>("volume_gid_manager/volume_gids_free")
        ->as<int>());

  string executorSandbox = slave::paths::getExecutorLatestRunPath(
      flags.work_dir,
      devolve(agentId),
      devolve(frameworkId),
      devolve(executorInfo.executor_id()));

  string volumePath = path::join(executorSandbox, "executor_volume_path");

  // The owner group of the volume should be changed to the gid allocated
  // to it, i.e., the first gid in the agent flag `--volume_gid_range`.
  struct stat s;
  EXPECT_EQ(0, ::stat(volumePath.c_str(), &s));
  EXPECT_EQ(10000u, s.st_gid);

  // Stop the slave.
  slave.get()->terminate();

  // Modify the boot ID to simulate a reboot.
  ASSERT_SOME(os::write(
      slave::paths::getBootIdPath(slave::paths::getMetaRootDir(flags.work_dir)),
      "rebooted!"));

  // Remove the runtime directory to simulate a reboot.
  ASSERT_SOME(os::rmdir(flags.runtime_dir));

  Future<Nothing> __recover = FUTURE_DISPATCH(_, &Slave::__recover);

  // Restart the slave.
  slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(__recover);

  // The owner group of the volume should be changed back to
  // the original one, i.e., root.
  EXPECT_EQ(0, ::stat(volumePath.c_str(), &s));
  EXPECT_EQ(0u, s.st_gid);
}


// This test verifies that agent is started with only one free volume gid
// and a task which is launched with a non-root user and tries to use two
// PARENT type SANDBOX_PATH volumes will fail due to volume gid exhausted.
TEST_F(VolumeGidManagerTest, ROOT_UNPRIVILEGED_USER_GidRangeExhausted)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.authenticate_http_readwrite = false;
  flags.isolation = "filesystem/linux,volume/sandbox_path";

  // Specify a volume gid range which has only one gid.
  flags.volume_gid_range = "[10000-10000]";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  v1::CommandInfo command = v1::createCommandInfo("sleep 1000");
  command.set_user(user.get());

  // Launch a task with a non-root user and with
  // two PARENT type SANDBOX_PATH volumes.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      resources,
      command);

  mesos::v1::ContainerInfo* containerInfo = taskInfo.mutable_container();
  containerInfo->set_type(mesos::v1::ContainerInfo::MESOS);

  mesos::v1::Volume* taskVolume = containerInfo->add_volumes();
  taskVolume->set_mode(mesos::v1::Volume::RW);
  taskVolume->set_container_path("task_volume_path1");

  mesos::v1::Volume::Source* source = taskVolume->mutable_source();
  source->set_type(mesos::v1::Volume::Source::SANDBOX_PATH);

  mesos::v1::Volume::Source::SandboxPath* sandboxPath =
    source->mutable_sandbox_path();

  sandboxPath->set_type(mesos::v1::Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("executor_volume_path1");

  taskVolume = containerInfo->add_volumes();
  taskVolume->set_mode(mesos::v1::Volume::RW);
  taskVolume->set_container_path("task_volume_path2");

  source = taskVolume->mutable_source();
  source->set_type(mesos::v1::Volume::Source::SANDBOX_PATH);

  sandboxPath = source->mutable_sandbox_path();
  sandboxPath->set_type(mesos::v1::Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("executor_volume_path2");

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateFailed;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(FutureArg<1>(&updateFailed));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  // The task should fail because no free gid can be allocated to
  // its second volume.
  AWAIT_READY(updateFailed);
  ASSERT_EQ(v1::TASK_FAILED, updateFailed->status().state());
  ASSERT_EQ(taskInfo.task_id(), updateFailed->status().task_id());
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
