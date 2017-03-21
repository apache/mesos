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

#include <utility>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/gtest.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>

#include "linux/fs.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/linux_launcher.hpp"

#include "slave/containerizer/mesos/isolators/docker/runtime.hpp"

#include "slave/containerizer/mesos/isolators/docker/volume/driver.hpp"
#include "slave/containerizer/mesos/isolators/docker/volume/isolator.hpp"

#include "slave/containerizer/mesos/isolators/filesystem/linux.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_docker.hpp"

namespace slave = mesos::internal::slave;

using process::Future;
using process::Owned;

using std::string;
using std::vector;

using mesos::internal::master::Master;

using mesos::internal::slave::DockerRuntimeIsolatorProcess;
using mesos::internal::slave::DockerVolumeIsolatorProcess;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Launcher;
using mesos::internal::slave::LinuxFilesystemIsolatorProcess;
using mesos::internal::slave::LinuxLauncher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::Provisioner;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::slave::Isolator;

using slave::docker::volume::DriverClient;

using testing::DoAll;

namespace mesos {
namespace internal {
namespace tests {

class MockDockerVolumeDriverClient : public DriverClient
{
public:
  MockDockerVolumeDriverClient() {}

  virtual ~MockDockerVolumeDriverClient() {}

  MOCK_METHOD3(
      mount,
      Future<string>(
          const string& driver,
          const string& name,
          const hashmap<string, string>& options));

  MOCK_METHOD2(
      unmount,
      Future<Nothing>(
          const string& driver,
          const string& name));
};


class DockerVolumeIsolatorTest : public MesosTest
{
protected:
  virtual void TearDown()
  {
    // Try to remove any mounts under sandbox.
    if (::geteuid() == 0) {
      Try<Nothing> unmountAll = fs::unmountAll(sandbox->c_str(), MNT_DETACH);
      if (unmountAll.isError()) {
        LOG(ERROR) << "Failed to unmount '" << sandbox->c_str()
                   << "': " << unmountAll.error();
        return;
      }
    }

    MesosTest::TearDown();
  }

  Volume createDockerVolume(
      const string& driver,
      const string& name,
      const string& containerPath,
      const Option<hashmap<string, string>>& options = None())
  {
    Volume volume;
    volume.set_mode(Volume::RW);
    volume.set_container_path(containerPath);

    Volume::Source* source = volume.mutable_source();
    source->set_type(Volume::Source::DOCKER_VOLUME);

    Volume::Source::DockerVolume* docker = source->mutable_docker_volume();
    docker->set_driver(driver);
    docker->set_name(name);

    Parameters parameters;

    if (options.isSome()) {
      foreachpair (const string& key, const string& value, options.get()) {
        Parameter* parameter = parameters.add_parameter();
        parameter->set_key(key);
        parameter->set_value(value);
      }

      docker->mutable_driver_options()->CopyFrom(parameters);
    }

    return volume;
  }

  // This helper creates a MesosContainerizer instance that uses the
  // LinuxFilesystemIsolator and DockerVolumeIsolator.
  Try<Owned<MesosContainerizer>> createContainerizer(
      const slave::Flags& flags,
      const Owned<DriverClient>& mockClient)
  {
    Try<Isolator*> linuxIsolator_ =
      LinuxFilesystemIsolatorProcess::create(flags);

    if (linuxIsolator_.isError()) {
      return Error(
          "Failed to create LinuxFilesystemIsolator: " +
          linuxIsolator_.error());
    }

    Owned<Isolator> linuxIsolator(linuxIsolator_.get());

    Try<Isolator*> runtimeIsolator_ =
      DockerRuntimeIsolatorProcess::create(flags);

    if (runtimeIsolator_.isError()) {
      return Error(
          "Failed to create DockerRuntimeIsolator: " +
          runtimeIsolator_.error());
    }

    Owned<Isolator> runtimeIsolator(runtimeIsolator_.get());

    Try<Isolator*> volumeIsolator_ =
      DockerVolumeIsolatorProcess::_create(flags, mockClient);

    if (volumeIsolator_.isError()) {
      return Error(
          "Failed to create DockerVolumeIsolator: " +
          volumeIsolator_.error());
    }

    Owned<Isolator> volumeIsolator(volumeIsolator_.get());

    Try<Launcher*> launcher_ = LinuxLauncher::create(flags);
    if (launcher_.isError()) {
      return Error("Failed to create LinuxLauncher: " + launcher_.error());
    }

    Owned<Launcher> launcher(launcher_.get());

    Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
    if (provisioner.isError()) {
      return Error("Failed to create provisioner: " + provisioner.error());
    }

    Try<MesosContainerizer*> containerizer = MesosContainerizer::create(
        flags,
        true,
        &fetcher,
        std::move(launcher),
        provisioner->share(),
        {std::move(linuxIsolator),
         std::move(runtimeIsolator),
         std::move(volumeIsolator)});

    if (containerizer.isError()) {
      return Error("Failed to create containerizer: " + containerizer.error());
    }

    return Owned<MesosContainerizer>(containerizer.get());
  }

private:
  Fetcher fetcher;
};


// This test verifies that multiple docker volumes with both absolute
// path and relative path are properly mounted to a container without
// rootfs, and launches a command task that reads files from the
// mounted docker volumes.
TEST_F(DockerVolumeIsolatorTest, ROOT_CommandTaskNoRootfsWithVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDockerVolumeDriverClient* mockClient = new MockDockerVolumeDriverClient;

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, Owned<DriverClient>(mockClient));

  ASSERT_SOME(containerizer);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer->get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

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
  ASSERT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  const string key = "iops";
  const string value = "150";

  hashmap<string, string> options = {{key, value}};

  // Create a volume with relative path.
  const string driver1 = "driver1";
  const string name1 = "name1";
  const string containerPath1 = "tmp/foo1";

  Volume volume1 = createDockerVolume(driver1, name1, containerPath1, options);

  // Create a volume with absolute path.
  const string driver2 = "driver2";
  const string name2 = "name2";

  // Make sure the absolute path exist.
  const string containerPath2 = path::join(os::getcwd(), "foo2");
  ASSERT_SOME(os::mkdir(containerPath2));

  Volume volume2 = createDockerVolume(driver2, name2, containerPath2);

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      "test -f " + containerPath1 + "/file1 && "
      "test -f " + containerPath2 + "/file2;");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.add_volumes()->CopyFrom(volume1);
  containerInfo.add_volumes()->CopyFrom(volume2);

  task.mutable_container()->CopyFrom(containerInfo);

  // Create mount point for volume1.
  const string mountPoint1 = path::join(os::getcwd(), "volume1");
  ASSERT_SOME(os::mkdir(mountPoint1));
  ASSERT_SOME(os::touch(path::join(mountPoint1, "file1")));

  // Create mount point for volume2.
  const string mountPoint2 = path::join(os::getcwd(), "volume2");
  ASSERT_SOME(os::mkdir(mountPoint2));
  ASSERT_SOME(os::touch(path::join(mountPoint2, "file2")));

  Future<string> mount1Name;
  Future<string> mount2Name;
  Future<hashmap<string, string>> mount1Options;

  EXPECT_CALL(*mockClient, mount(driver1, _, _))
    .WillOnce(DoAll(FutureArg<1>(&mount1Name),
                    FutureArg<2>(&mount1Options),
                    Return(mountPoint1)));

  EXPECT_CALL(*mockClient, mount(driver2, _, _))
    .WillOnce(DoAll(FutureArg<1>(&mount2Name),
                    Return(mountPoint2)));

  Future<string> unmount1Name;
  Future<string> unmount2Name;

  EXPECT_CALL(*mockClient, unmount(driver1, _))
    .WillOnce(DoAll(FutureArg<1>(&unmount1Name),
                    Return(Nothing())));

  EXPECT_CALL(*mockClient, unmount(driver2, _))
    .WillOnce(DoAll(FutureArg<1>(&unmount2Name),
                    Return(Nothing())));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Make sure the docker volume mount parameters are same with the
  // parameters in `containerInfo`.
  AWAIT_EXPECT_EQ(name1, mount1Name);
  AWAIT_EXPECT_EQ(name2, mount2Name);

  AWAIT_READY(mount1Options);
  EXPECT_SOME_EQ(value, mount1Options->get(key));

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // Make sure the docker volume unmount parameters are same with
  // the parameters in `containerInfo`.
  AWAIT_EXPECT_EQ(name1, unmount1Name);
  AWAIT_EXPECT_EQ(name2, unmount2Name);

  driver.stop();
  driver.join();
}


// This test verifies that multiple docker volumes with the same
// driver and name cannot be mounted to the same container. If that
// happens, the task will fail.
TEST_F(DockerVolumeIsolatorTest, ROOT_CommandTaskNoRootfsFailedWithSameVolumes)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDockerVolumeDriverClient* mockClient = new MockDockerVolumeDriverClient;

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, Owned<DriverClient>(mockClient));

  ASSERT_SOME(containerizer);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer->get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

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
  ASSERT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  const string key = "iops";
  const string value = "150";

  hashmap<string, string> options = {{key, value}};

  // Create a volume with relative path.
  const string driver1 = "driver1";
  const string name1 = "name1";
  const string containerPath1 = "tmp/foo1";

  Volume volume1 = createDockerVolume(driver1, name1, containerPath1, options);

  // Create a volume with absolute path and make sure the absolute
  // path exist. Please note that volume1 and volume2 will be created
  // with same volume driver and name, this will cause task failed
  // when mounting same mount point to one container.
  const string containerPath2 = path::join(os::getcwd(), "foo2");
  ASSERT_SOME(os::mkdir(containerPath2));

  Volume volume2 = createDockerVolume(driver1, name1, containerPath2);

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      "test -f " + containerPath1 + "/file1 && "
      "test -f " + containerPath2 + "/file2;");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.add_volumes()->CopyFrom(volume1);
  containerInfo.add_volumes()->CopyFrom(volume2);

  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusFailed);
  EXPECT_EQ(task.task_id(), statusFailed->task_id());
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  driver.stop();
  driver.join();
}


// This test verifies that the docker volumes are properly recovered
// during slave recovery.
TEST_F(DockerVolumeIsolatorTest, ROOT_CommandTaskNoRootfsSlaveRecovery)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDockerVolumeDriverClient* mockClient = new MockDockerVolumeDriverClient;

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, Owned<DriverClient>(mockClient));

  ASSERT_SOME(containerizer);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer->get(),
      flags);

  ASSERT_SOME(slave);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

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
  ASSERT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  const string key = "iops";
  const string value = "150";

  hashmap<string, string> options = {{key, value}};

  // Create a volume with relative path.
  const string driver1 = "driver1";
  const string name1 = "name1";
  const string containerPath1 = "tmp/foo1";

  Volume volume1 = createDockerVolume(driver1, name1, containerPath1, options);

  // Create a volume with absolute path.
  const string driver2 = "driver2";
  const string name2 = "name2";

  // Make sure the absolute path exist.
  const string containerPath2 = path::join(os::getcwd(), "foo2");
  ASSERT_SOME(os::mkdir(containerPath2));

  Volume volume2 = createDockerVolume(driver2, name2, containerPath2);

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      "while true; do test -f " + containerPath1 + "/file1 && "
      "test -f " + containerPath2 + "/file2; done");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.add_volumes()->CopyFrom(volume1);
  containerInfo.add_volumes()->CopyFrom(volume2);

  task.mutable_container()->CopyFrom(containerInfo);

  // Create mount point for volume1.
  const string mountPoint1 = path::join(os::getcwd(), "volume1");
  ASSERT_SOME(os::mkdir(mountPoint1));
  ASSERT_SOME(os::touch(path::join(mountPoint1, "file1")));

  // Create mount point for volume2.
  const string mountPoint2 = path::join(os::getcwd(), "volume2");
  ASSERT_SOME(os::mkdir(mountPoint2));
  ASSERT_SOME(os::touch(path::join(mountPoint2, "file2")));

  Future<string> mount1Name;
  Future<string> mount2Name;
  Future<hashmap<string, string>> mount1Options;

  EXPECT_CALL(*mockClient, mount(driver1, _, _))
    .WillOnce(DoAll(FutureArg<1>(&mount1Name),
                    FutureArg<2>(&mount1Options),
                    Return(mountPoint1)));

  EXPECT_CALL(*mockClient, mount(driver2, _, _))
    .WillOnce(DoAll(FutureArg<1>(&mount2Name),
                    Return(mountPoint2)));

  Future<TaskStatus> statusRunning;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  // Stop the slave after TASK_RUNNING is received.
  slave.get()->terminate();

  // Set up so we can wait until the new slave updates the container's
  // resources (this occurs after the executor has re-registered).
  Future<Nothing> update =
    FUTURE_DISPATCH(_, &MesosContainerizerProcess::update);

  mockClient = new MockDockerVolumeDriverClient;

  containerizer = createContainerizer(
      flags, Owned<DriverClient>(mockClient));

  ASSERT_SOME(containerizer);

  Future<SlaveReregisteredMessage> reregistered =
      FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  Future<string> unmount1Name;
  Future<string> unmount2Name;

  EXPECT_CALL(*mockClient, unmount(driver1, _))
    .WillOnce(DoAll(FutureArg<1>(&unmount1Name),
                    Return(Nothing())));

  EXPECT_CALL(*mockClient, unmount(driver2, _))
    .WillOnce(DoAll(FutureArg<1>(&unmount2Name),
                    Return(Nothing())));

  // Use the same flags.
  slave = StartSlave(detector.get(), containerizer->get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregistered);

  // Wait until the containerizer is updated.
  AWAIT_READY(update);

  Future<hashset<ContainerID>> containers = containerizer.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  // Kill the task.
  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  // Make sure the docker volume unmount parameters are same with
  // the parameters in `containerInfo`.
  AWAIT_EXPECT_EQ(name1, unmount1Name);
  AWAIT_EXPECT_EQ(name2, unmount2Name);

  driver.stop();
  driver.join();
}


// This test verifies that a single docker volumes can be used by
// multiple containers, and the docker volume isolator will mount
// the single volume to multiple containers when running tasks and
// the docker volume isolator will call unmount only once when cleanup
// the last container that is using the volume.
TEST_F(DockerVolumeIsolatorTest,
       ROOT_CommandTaskNoRootfsSingleVolumeMultipleContainers)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDockerVolumeDriverClient* mockClient = new MockDockerVolumeDriverClient;

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, Owned<DriverClient>(mockClient));

  ASSERT_SOME(containerizer);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer->get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

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
  ASSERT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  // Create a volume with relative path and share the volume with
  // two different containers.
  const string driver1 = "driver1";
  const string name1 = "name1";
  const string containerPath1 = "tmp/foo";

  Volume volume1 = createDockerVolume(driver1, name1, containerPath1);

  TaskInfo task1 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:64").get(),
      "while true; do test -f " + containerPath1 + "/file; done");

  ContainerInfo containerInfo1;
  containerInfo1.set_type(ContainerInfo::MESOS);
  containerInfo1.add_volumes()->CopyFrom(volume1);

  task1.mutable_container()->CopyFrom(containerInfo1);

  // Create mount point for volume.
  const string mountPoint1 = path::join(os::getcwd(), "volume");
  ASSERT_SOME(os::mkdir(mountPoint1));
  ASSERT_SOME(os::touch(path::join(mountPoint1, "file")));

  TaskInfo task2 = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:64").get(),
      "while true; do test -f " + containerPath1 + "/file; done");

  ContainerInfo containerInfo2;
  containerInfo2.set_type(ContainerInfo::MESOS);
  containerInfo2.add_volumes()->CopyFrom(volume1);

  task2.mutable_container()->CopyFrom(containerInfo2);

  // The mount operation will be called multiple times as there are
  // two containers using the same volume.
  EXPECT_CALL(*mockClient, mount(driver1, _, _))
    .WillRepeatedly(Return(mountPoint1));

  // Expect the unmount was called only once because two containers
  // sharing one volume.
  EXPECT_CALL(*mockClient, unmount(driver1, _))
    .WillOnce(Return(Nothing()));

  Future<TaskStatus> statusRunning1;
  Future<TaskStatus> statusKilled1;
  Future<TaskStatus> statusRunning2;
  Future<TaskStatus> statusKilled2;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning1))
    .WillOnce(FutureArg<1>(&statusRunning2))
    .WillOnce(FutureArg<1>(&statusKilled1))
    .WillOnce(FutureArg<1>(&statusKilled2));

  driver.launchTasks(offer.id(), {task1, task2});

  AWAIT_READY(statusRunning1);
  EXPECT_EQ(TASK_RUNNING, statusRunning1->state());

  AWAIT_READY(statusRunning2);
  EXPECT_EQ(TASK_RUNNING, statusRunning2->state());

  // Kill both of the tasks.
  driver.killTask(task1.task_id());
  driver.killTask(task2.task_id());

  AWAIT_READY(statusKilled1);
  EXPECT_EQ(TASK_KILLED, statusKilled1->state());

  AWAIT_READY(statusKilled2);
  EXPECT_EQ(TASK_KILLED, statusKilled2->state());

  driver.stop();
  driver.join();
}


// This test verifies that a docker volume with absolute path can
// be properly mounted to a container with rootfs, and launches a
// command task that reads files from the mounted docker volume.
TEST_F(DockerVolumeIsolatorTest,
       ROOT_INTERNET_CURL_CommandTaskRootfsWithAbsolutePathVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/volume,docker/runtime,filesystem/linux";
  flags.image_providers = "docker";

  MockDockerVolumeDriverClient* mockClient = new MockDockerVolumeDriverClient;

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, Owned<DriverClient>(mockClient));

  ASSERT_SOME(containerizer);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer->get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

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
  ASSERT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  // Create a volume with absolute path.
  const string volumeDriver = "driver";
  const string name = "name";

  const string containerPath = path::join(os::getcwd(), "foo");

  Volume volume = createDockerVolume(volumeDriver, name, containerPath);

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/usr/bin/test");
  command.add_arguments("test");
  command.add_arguments("-f");
  command.add_arguments(containerPath + "/file");

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      command);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.add_volumes()->CopyFrom(volume);

  containerInfo.mutable_mesos()->mutable_image()->CopyFrom(image);

  task.mutable_container()->CopyFrom(containerInfo);

  // Create mount point for volume.
  const string mountPoint = path::join(os::getcwd(), "volume");
  ASSERT_SOME(os::mkdir(mountPoint));
  ASSERT_SOME(os::touch(path::join(mountPoint, "file")));

  Future<string> mountName;

  EXPECT_CALL(*mockClient, mount(volumeDriver, _, _))
    .WillOnce(DoAll(FutureArg<1>(&mountName),
                    Return(mountPoint)));

  Future<string> unmountName;

  EXPECT_CALL(*mockClient, unmount(volumeDriver, _))
    .WillOnce(DoAll(FutureArg<1>(&unmountName),
                    Return(Nothing())));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Make sure the docker volume mount parameters are same with the
  // parameters in `containerInfo`.
  AWAIT_EXPECT_EQ(name, mountName);

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // Make sure the docker volume unmount parameters are same with
  // the parameters in `containerInfo`.
  AWAIT_EXPECT_EQ(name, unmountName);

  driver.stop();
  driver.join();
}


// This test verifies that a docker volume with relative path can
// be properly mounted to a container with rootfs, and launches a
// command task that reads files from the mounted docker volume.
TEST_F(DockerVolumeIsolatorTest,
       ROOT_INTERNET_CURL_CommandTaskRootfsWithRelativeVolume)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/volume,docker/runtime,filesystem/linux";
  flags.image_providers = "docker";

  MockDockerVolumeDriverClient* mockClient = new MockDockerVolumeDriverClient;

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(flags, Owned<DriverClient>(mockClient));

  ASSERT_SOME(containerizer);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer->get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

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
  ASSERT_NE(0u, offers->size());

  const Offer& offer = offers.get()[0];

  const string key = "iops";
  const string value = "150";

  hashmap<string, string> options = {{key, value}};

  // Create a volume with relative path.
  const string volumeDriver = "driver";
  const string name = "name";
  const string containerPath = "tmp/foo";

  Volume volume = createDockerVolume(
      volumeDriver, name, containerPath, options);

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/usr/bin/test");
  command.add_arguments("test");
  command.add_arguments("-f");
  command.add_arguments(containerPath + "/file");

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      command);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.add_volumes()->CopyFrom(volume);

  containerInfo.mutable_mesos()->mutable_image()->CopyFrom(image);

  task.mutable_container()->CopyFrom(containerInfo);

  // Create mount point for volume.
  const string mountPoint = path::join(os::getcwd(), "volume");
  ASSERT_SOME(os::mkdir(mountPoint));
  ASSERT_SOME(os::touch(path::join(mountPoint, "file")));

  Future<string> mountName;
  Future<hashmap<string, string>> mountOptions;

  EXPECT_CALL(*mockClient, mount(volumeDriver, _, _))
    .WillOnce(DoAll(FutureArg<1>(&mountName),
                    FutureArg<2>(&mountOptions),
                    Return(mountPoint)));

  Future<string> unmountName;

  EXPECT_CALL(*mockClient, unmount(volumeDriver, _))
    .WillOnce(DoAll(FutureArg<1>(&unmountName),
                    Return(Nothing())));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Make sure the docker volume mount parameters are same with the
  // parameters in `containerInfo`.
  AWAIT_EXPECT_EQ(name, mountName);

  AWAIT_READY(mountOptions);
  EXPECT_SOME_EQ(value, mountOptions->get(key));

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  // Make sure the docker volume unmount parameters are same with
  // the parameters in `containerInfo`.
  AWAIT_EXPECT_EQ(name, unmountName);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
