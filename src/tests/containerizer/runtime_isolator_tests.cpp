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

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <mesos/v1/scheduler.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include <process/owned.hpp>

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

#include "tests/containerizer/docker_archive.hpp"

namespace master = mesos::internal::master;
namespace slave = mesos::internal::slave;

using std::string;
using std::vector;

using process::Future;
using process::Owned;
using process::PID;

using master::Master;

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Event;

using slave::Slave;

using testing::_;
using testing::DoAll;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

class DockerArchiveTest : public TemporaryDirectoryTest {};


// This test verifies that a testing docker image is created as
// a local archive under a given directory. This is a ROOT test
// because permission is need to create linux root filesystem.
TEST_F(DockerArchiveTest, ROOT_CreateDockerLocalTar)
{
  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage = DockerArchive::create(directory, "alpine");
  AWAIT_READY(testImage);

  EXPECT_TRUE(os::exists(path::join(directory, "alpine.tar")));
  EXPECT_FALSE(os::exists(path::join(directory, "alpine")));
}


class DockerRuntimeIsolatorTest : public MesosTest {};


// This test verifies that docker image default cmd is executed correctly.
// This corresponds to the case in runtime isolator logic table: sh=0,
// value=0, argv=1, entrypoint=0, cmd=1.
TEST_F(DockerRuntimeIsolatorTest, ROOT_DockerDefaultCmdImageTarPuller)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage =
    DockerArchive::create(directory, "alpine", "null", "[\"sh\"]");

  AWAIT_READY(testImage);

  ASSERT_TRUE(os::exists(path::join(directory, "alpine.tar")));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_registry = directory;

  // Make docker store directory as a temparary directory. Because the
  // manifest of the test image is changeable, the image cached on
  // previous tests should never be used.
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

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
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("test-task");
  task.mutable_task_id()->set_value(id::UUID::random().toString());
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(Resources::parse("cpus:1;mem:128").get());
  task.mutable_command()->set_shell(false);
  task.mutable_command()->add_arguments("-c");
  task.mutable_command()->add_arguments("echo 'hello world'");

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that docker image default entrypoint is executed
// correctly using image tar puller. This corresponds to the case in
// runtime isolator logic table: sh=0, value=0, argv=1, entrypoint=1, cmd=0.
TEST_F(DockerRuntimeIsolatorTest, ROOT_DockerDefaultEntryptImageTarPuller)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage =
    DockerArchive::create(directory, "alpine", "[\"echo\"]", "null");

  AWAIT_READY(testImage);

  ASSERT_TRUE(os::exists(path::join(directory, "alpine.tar")));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_registry = directory;
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

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
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("test-task");
  task.mutable_task_id()->set_value(id::UUID::random().toString());
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(Resources::parse("cpus:1;mem:128").get());
  task.mutable_command()->set_shell(false);
  task.mutable_command()->add_arguments("hello world");

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that docker image default entrypoint is executed
// correctly using registry puller. This corresponds to the case in runtime
// isolator logic table: sh=0, value=0, argv=1, entrypoint=1, cmd=0.
TEST_F(DockerRuntimeIsolatorTest,
       ROOT_INTERNET_CURL_DockerDefaultEntryptRegistryPuller)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

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
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("test-task");
  task.mutable_task_id()->set_value(id::UUID::random().toString());
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(Resources::parse("cpus:1;mem:128").get());
  task.mutable_command()->set_shell(false);
  task.mutable_command()->add_arguments("hello world");

  Image image;
  image.set_type(Image::DOCKER);

  // 'mesosphere/inky' image is used in docker containerizer test, which
  // contains entrypoint as 'echo' and cmd as null.
  image.mutable_docker()->set_name("mesosphere/inky");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies a simple task with container image can be
// launched in a nested container using v1 TASK_GROUP API.
TEST_F(DockerRuntimeIsolatorTest, ROOT_INTERNET_CURL_NestedSimpleCommand)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.image_providers = "docker";
  flags.authenticate_http_readwrite = false;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "test_default_executor",
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT);

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      v1::createCommandInfo("/bin/ls", {"ls", "-al", "/"}));

  taskInfo.mutable_container()->CopyFrom(
      v1::createContainerInfo("library/alpine"));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())));

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateRunning->status().task_id());
  EXPECT_TRUE(updateRunning->status().has_timestamp());

  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateFinished));

  AWAIT_READY(updateFinished);
  ASSERT_EQ(v1::TASK_FINISHED, updateFinished->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateFinished->status().task_id());
  EXPECT_TRUE(updateFinished->status().has_timestamp());
}


// This test verifies that docker image default cmd is executed
// and user defined argv is appended correctly for nested container.
// This is a regression test for MESOS-6852. It corresponds to the
// following case in runtime isolator logic table for nested
// container: sh=0, value=0, argv=1, entrypoint=0, cmd=1.
TEST_F(DockerRuntimeIsolatorTest, ROOT_NestedDockerDefaultCmdImageTarPuller)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage =
    DockerArchive::create(directory, "alpine", "null", "[\"sh\"]");

  AWAIT_READY(testImage);

  ASSERT_TRUE(os::exists(path::join(directory, "alpine.tar")));

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.image_providers = "docker";
  flags.docker_registry = directory;
  flags.authenticate_http_readwrite = false;

  // Make docker store directory as a temparary directory. Because the
  // manifest of the test image is changeable, the image cached on
  // previous tests should never be used.
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "test_default_executor",
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT);

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      v1::createCommandInfo(None(), {"-c", "echo 'hello world'"}));

  taskInfo.mutable_container()->CopyFrom(
      v1::createContainerInfo("alpine"));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())));

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateRunning->status().task_id());
  EXPECT_TRUE(updateRunning->status().has_timestamp());

  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateFinished));

  AWAIT_READY(updateFinished);
  ASSERT_EQ(v1::TASK_FINISHED, updateFinished->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateFinished->status().task_id());
  EXPECT_TRUE(updateFinished->status().has_timestamp());
}


// This test verifies that docker image default entrypoint is
// executed and user defined argv is appended correctly for
// nested container. This is a regression test for MESOS-6852.
// It corresponds to the following case in runtime isolator
// logic table for nested container: sh=0, value=0, argv=1,
// entrypoint=1, cmd=0.
TEST_F(DockerRuntimeIsolatorTest, ROOT_NestedDockerDefaultEntryptImageTarPuller)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage =
    DockerArchive::create(directory, "alpine", "[\"echo\"]", "null");

  AWAIT_READY(testImage);

  ASSERT_TRUE(os::exists(path::join(directory, "alpine.tar")));

  // Disable AuthN on the agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,docker/runtime";
  flags.image_providers = "docker";
  flags.docker_registry = directory;
  flags.authenticate_http_readwrite = false;

  // Make docker store directory as a temparary directory. Because the
  // manifest of the test image is changeable, the image cached on
  // previous tests should never be used.
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "test_default_executor",
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT);

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  v1::TaskInfo taskInfo = v1::createTask(
      offer.agent_id(),
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      v1::createCommandInfo(None(), {"hello world"}));

  taskInfo.mutable_container()->CopyFrom(
      v1::createContainerInfo("alpine"));

  Future<Event::Update> updateStarting;
  Future<Event::Update> updateRunning;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateStarting),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())));

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(updateStarting);
  ASSERT_EQ(v1::TASK_STARTING, updateStarting->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateStarting->status().task_id());

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateRunning->status().task_id());
  EXPECT_TRUE(updateRunning->status().has_timestamp());

  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&updateFinished));

  AWAIT_READY(updateFinished);
  ASSERT_EQ(v1::TASK_FINISHED, updateFinished->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateFinished->status().task_id());
  EXPECT_TRUE(updateFinished->status().has_timestamp());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
