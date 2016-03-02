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

#include <mesos/mesos.hpp>

#include <mesos/slave/container_logger.hpp>

#include <process/owned.hpp>
#include <process/gtest.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#include "slave/paths.hpp"

#ifdef __linux__
#include "slave/containerizer/mesos/linux_launcher.hpp"

#include "slave/containerizer/mesos/isolators/filesystem/linux.hpp"
#endif

#include "slave/containerizer/mesos/containerizer.hpp"

#include "slave/containerizer/mesos/provisioner/backend.hpp"
#include "slave/containerizer/mesos/provisioner/paths.hpp"

#include "slave/containerizer/mesos/provisioner/backends/copy.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

#include "tests/containerizer/rootfs.hpp"
#include "tests/containerizer/store.hpp"

using namespace process;

using std::string;
using std::vector;

using mesos::internal::master::Master;

using mesos::internal::slave::Backend;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Launcher;
#ifdef __linux__
using mesos::internal::slave::LinuxFilesystemIsolatorProcess;
using mesos::internal::slave::LinuxLauncher;
#endif
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::Provisioner;
using mesos::internal::slave::ProvisionerProcess;
using mesos::internal::slave::Slave;
using mesos::internal::slave::Store;

using mesos::slave::ContainerLogger;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace tests {

#ifdef __linux__
class LinuxFilesystemIsolatorTest : public MesosTest
{
protected:
  virtual void TearDown()
  {
    // Try to remove any mounts under sandbox.
    if (::geteuid() == 0) {
      Try<string> umount = os::shell(
          "grep '%s' /proc/mounts | "
          "cut -d' ' -f2 | "
          "xargs --no-run-if-empty umount -l",
          sandbox.get().c_str());

      if (umount.isError()) {
        LOG(ERROR) << "Failed to umount for sandbox '" << sandbox.get()
                   << "': " << umount.error();
      }
    }

    MesosTest::TearDown();
  }

  // This helper creates a MesosContainerizer instance that uses the
  // LinuxFilesystemIsolator. The filesystem isolator takes a
  // TestAppcProvisioner which provisions APPC images by copying files
  // from the host filesystem.
  // 'images' is a map of imageName -> rootfsPath.
  // TODO(xujyan): The current assumption of one rootfs per image name
  // is inconsistent with the real provisioner and we should fix it.
  Try<Owned<MesosContainerizer>> createContainerizer(
      const slave::Flags& flags,
      const hashmap<string, string>& images)
  {
    // Create the root filesystems.
    hashmap<string, Shared<Rootfs>> rootfses;
    foreachpair (const string& imageName, const string& rootfsPath, images) {
      Try<Owned<Rootfs>> rootfs = LinuxRootfs::create(rootfsPath);

      if (rootfs.isError()) {
        return Error("Failed to create LinuxRootfs: " + rootfs.error());
      }

      rootfses.put(imageName, rootfs.get().share());
    }

    Owned<Store> store(new TestStore(rootfses));
    hashmap<Image::Type, Owned<Store>> stores;
    stores[Image::APPC] = store;

    hashmap<string, Owned<Backend>> backends = Backend::create(flags);

    const string rootDir = slave::paths::getProvisionerDir(flags.work_dir);

    if (!os::exists(rootDir)) {
      Try<Nothing> mkdir = os::mkdir(rootDir);
      if (mkdir.isError()) {
        return Error("Failed to create root dir: " + mkdir.error());
      }
    }

    Owned<ProvisionerProcess> provisionerProcess(new ProvisionerProcess(
        flags,
        rootDir,
        stores,
        backends));

    Owned<Provisioner> provisioner(new Provisioner(provisionerProcess));

    Try<Isolator*> isolator = LinuxFilesystemIsolatorProcess::create(flags);

    if (isolator.isError()) {
      return Error(
          "Failed to create LinuxFilesystemIsolatorProcess: " +
          isolator.error());
    }

    Try<Launcher*> launcher = LinuxLauncher::create(flags);

    if (launcher.isError()) {
      return Error("Failed to create LinuxLauncher: " + launcher.error());
    }

    // Create and initialize a new container logger.
    Try<ContainerLogger*> logger =
      ContainerLogger::create(flags.container_logger);

    if (logger.isError()) {
      return Error("Failed to create container logger: " + logger.error());
    }

    return Owned<MesosContainerizer>(
        new MesosContainerizer(
            flags,
            true,
            &fetcher,
            Owned<ContainerLogger>(logger.get()),
            Owned<Launcher>(launcher.get()),
            provisioner,
            {Owned<Isolator>(isolator.get())}));
  }

  ContainerInfo createContainerInfo(
      const Option<string>& imageName = None(),
      const vector<Volume>& volumes = vector<Volume>())
  {
    ContainerInfo info;
    info.set_type(ContainerInfo::MESOS);

    if (imageName.isSome()) {
      Image* image = info.mutable_mesos()->mutable_image();
      image->set_type(Image::APPC);

      Image::Appc* appc = image->mutable_appc();
      appc->set_name(imageName.get());
    }

    foreach (const Volume& volume, volumes) {
      info.add_volumes()->CopyFrom(volume);
    }

    return info;
  }

  Volume createVolumeFromHostPath(
      const string& containerPath,
      const string& hostPath,
      const Volume::Mode& mode)
  {
    return CREATE_VOLUME(containerPath, hostPath, mode);
  }

  Volume createVolumeFromImage(
      const string& containerPath,
      const string& imageName,
      const Volume::Mode& mode)
  {
    Volume volume;
    volume.set_container_path(containerPath);
    volume.set_mode(mode);

    Image* image = volume.mutable_image();
    image->set_type(Image::APPC);

    Image::Appc* appc = image->mutable_appc();
    appc->set_name(imageName);

    return volume;
  }

private:
  Fetcher fetcher;
};


// This test verifies that the root filesystem of the container is
// properly changed to the one that's provisioned by the provisioner.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_ChangeRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "[ ! -d '" + os::getcwd() + "' ]");

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copying.
  AWAIT_READY_FOR(launch, Seconds(60));

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies that the root filesystem of the container is
// properly changed to the one that's provisioned by the provisioner.
// Also runs the command executor with the new root filesystem.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_ChangeRootFilesystemCommandExecutor)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.image_provisioner_backend = "copy";

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get().get(), flags);
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

  SlaveID slaveId = offer.slave_id();

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      "test -d " + flags.sandbox_directory);

  ContainerInfo containerInfo;
  Image* image = containerInfo.mutable_mesos()->mutable_image();
  image->set_type(Image::APPC);
  image->mutable_appc()->set_name("test_image");
  containerInfo.set_type(ContainerInfo::MESOS);
  task.mutable_container()->CopyFrom(containerInfo);

  driver.launchTasks(offers.get()[0].id(), {task});

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  // Need to wait for Rootfs copying.
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that the root filesystem of the container is
// properly changed to the one that's provisioned by the provisioner.
// Also runs the command executor with the new root filesystem.
TEST_F(LinuxFilesystemIsolatorTest,
       ROOT_ChangeRootFilesystemCommandExecutorWithVolumes)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.image_provisioner_backend = "copy";

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get().get(), flags);
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

  SlaveID slaveId = offer.slave_id();

  // Preparing two volumes:
  // - host_path: dir1, container_path: /tmp
  // - host_path: dir2, container_path: relative_dir
  const string dir1 = path::join(os::getcwd(), "dir1");
  ASSERT_SOME(os::mkdir(dir1));

  const string testFile = path::join(dir1, "testfile");
  ASSERT_SOME(os::touch(testFile));

  const string dir2 = path::join(os::getcwd(), "dir2");
  ASSERT_SOME(os::mkdir(dir2));

  TaskInfo task = createTask(
      offer.slave_id(),
      offer.resources(),
      "test -f /tmp/testfile && test -d " +
      path::join(flags.sandbox_directory, "relative_dir"));

  ContainerInfo containerInfo;
  Image* image = containerInfo.mutable_mesos()->mutable_image();
  image->set_type(Image::APPC);
  image->mutable_appc()->set_name("test_image");
  containerInfo.set_type(ContainerInfo::MESOS);

  // We are assuming the image created by the tests have /tmp to be
  // able to mount the directory.
  containerInfo.add_volumes()->CopyFrom(
      createVolumeFromHostPath("/tmp", dir1, Volume::RW));
  containerInfo.add_volumes()->CopyFrom(
      createVolumeFromHostPath("relative_dir", dir2, Volume::RW));
  task.mutable_container()->CopyFrom(containerInfo);

  driver.launchTasks(offers.get()[0].id(), {task});

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that a command task with new root filesystem
// with persistent volumes works correctly.
TEST_F(LinuxFilesystemIsolatorTest,
       ROOT_ChangeRootFilesystemCommandExecutorPersistentVolume)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.image_provisioner_backend = "copy";
  flags.resources = "cpus:2;mem:1024;disk(role1):1024";

  // Need this otherwise the persistent volumes are not created
  // within the slave work_dir and thus not retrievable.
  flags.work_dir = os::getcwd();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get().get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  MesosSchedulerDriver driver(
    &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

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

  Offer offer = offers.get()[0];

  const string dir1 = path::join(os::getcwd(), "dir1");
  ASSERT_SOME(os::mkdir(dir1));

  Resource persistentVolume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:512").get() + persistentVolume,
      "echo abc > path1/file");
  ContainerInfo containerInfo;
  Image* image = containerInfo.mutable_mesos()->mutable_image();
  image->set_type(Image::APPC);
  image->mutable_appc()->set_name("test_image");
  containerInfo.set_type(ContainerInfo::MESOS);

  // We are assuming the image created by the tests have /tmp to be
  // able to mount the directory.
  containerInfo.add_volumes()->CopyFrom(
      createVolumeFromHostPath("/tmp", dir1, Volume::RW));
  task.mutable_container()->CopyFrom(containerInfo);

  // Create the persistent volumes and launch task via `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume), LAUNCH({task})},
      filters);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // NOTE: The command executor's id is the same as the task id.
  ExecutorID executorId;
  executorId.set_value(task.task_id().value());

  const string& directory = slave::paths::getExecutorLatestRunPath(
      flags.work_dir,
      offer.slave_id(),
      frameworkId.get(),
      executorId);

  EXPECT_FALSE(os::exists(path::join(directory, "path1")));

  const string& volumePath = slave::paths::getPersistentVolumePath(
      flags.work_dir,
      "role1",
      "id1");

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volumePath, "file")));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that persistent volumes are unmounted properly
// after a checkpointed framework disappears and the slave restarts.
//
// TODO(jieyu): Even though the command task specifies a new
// filesystem root, the executor (command executor) itself does not
// change filesystem root (uses the host filesystem). We need to add a
// test to test the scenario that the executor itself changes rootfs.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_RecoverOrphanedPersistentVolume)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.image_provisioner_backend = "copy";
  flags.resources = "cpus:2;mem:1024;disk(role1):1024";
  flags.isolation = "posix/disk,filesystem/linux";

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  Try<PID<Slave>> slave = StartSlave(containerizer.get().get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
    &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

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
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  const string dir1 = path::join(os::getcwd(), "dir1");
  ASSERT_SOME(os::mkdir(dir1));

  Resource persistentVolume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1");

  // Create a task that does nothing for a long time.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:512").get() + persistentVolume,
      "sleep 1000");

  ContainerInfo containerInfo;
  Image* image = containerInfo.mutable_mesos()->mutable_image();
  image->set_type(Image::APPC);
  image->mutable_appc()->set_name("test_image");
  containerInfo.set_type(ContainerInfo::MESOS);

  // We are assuming the image created by the tests have /tmp to be
  // able to mount the directory.
  containerInfo.add_volumes()->CopyFrom(
      createVolumeFromHostPath("/tmp", dir1, Volume::RW));
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(DoDefault());

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  // Create the persistent volumes and launch task via `acceptOffers`.
  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume), LAUNCH({task})});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  // Restart the slave.
  Stop(slave.get());

  // Wipe the slave meta directory so that the slave will treat the
  // above running task as an orphan.
  ASSERT_SOME(os::rmdir(slave::paths::getMetaRootDir(flags.work_dir)));

  // Recreate the containerizer using the same helper as above.
  containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  slave = StartSlave(containerizer.get().get(), flags);
  ASSERT_SOME(slave);

  // Wait until slave recovery is complete.
  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);
  AWAIT_READY(_recover);

  // Wait until the containerizer's recovery is done too.
  // This is called once orphans are cleaned up.  But this future is not
  // directly tied to the `Slave::_recover` future above.
  _recover = FUTURE_DISPATCH(_, &MesosContainerizerProcess::___recover);
  AWAIT_READY(_recover);

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  ASSERT_SOME(table);

  // All mount targets should be under this directory.
  const string directory = slave::paths::getSandboxRootDir(flags.work_dir);

  // Verify that the orphaned container's persistent volume and
  // the rootfs are unmounted.
  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    EXPECT_FALSE(strings::contains(entry.target, directory))
      << "Target was not unmounted: " << entry.target;
  }

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(LinuxFilesystemIsolatorTest, ROOT_Metrics)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  // Use a long running task so we can reliably capture the moment it's alive.
  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "sleep 1000");

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copying.
  AWAIT_READY_FOR(launch, Seconds(60));

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(1u, stats.values.count(
      "containerizer/mesos/filesystem/containers_new_rootfs"));
  EXPECT_EQ(
      1, stats.values["containerizer/mesos/filesystem/containers_new_rootfs"]);

  containerizer.get()->destroy(containerId);

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Executor was killed.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(9, wait.get().status());
}


// This test verifies that a volume with a relative host path is
// properly created in the container's sandbox and is properly mounted
// in the container's mount namespace.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromSandbox)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "echo abc > /tmp/file");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp", "tmp", Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copying.
  AWAIT_READY_FOR(launch, Seconds(60));

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(directory, "tmp", "file")));
}


// This test verifies that a volume with an absolute host path as
// well as an absolute container path is properly mounted in the
// container's mount namespace.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromHost)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "test -d /tmp/sandbox");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("/tmp", os::getcwd(), Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copying.
  AWAIT_READY_FOR(launch, Seconds(60));

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies that a volume with an absolute host path and a
// relative container path is properly mounted in the container's
// mount namespace. The mount point will be created in the sandbox.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeFromHostSandboxMountPoint)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "test -d mountpoint/sandbox");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image",
      {createVolumeFromHostPath("mountpoint", os::getcwd(), Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copying.
  AWAIT_READY_FOR(launch, Seconds(60));

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies that persistent volumes are properly mounted in
// the container's root filesystem.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_PersistentVolumeWithRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  // Need this otherwise the persistent volumes are not created
  // within the slave work_dir and thus not retrievable.
  flags.work_dir = os::getcwd();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "echo abc > volume/file");

  executor.add_resources()->CopyFrom(createPersistentVolume(
      Megabytes(32),
      "test_role",
      "persistent_volume_id",
      "volume"));

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  // Create a persistent volume.
  string volume = slave::paths::getPersistentVolumePath(
      os::getcwd(),
      "test_role",
      "persistent_volume_id");

  ASSERT_SOME(os::mkdir(volume));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copying.
  AWAIT_READY_FOR(launch, Seconds(60));

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volume, "file")));
}


TEST_F(LinuxFilesystemIsolatorTest, ROOT_PersistentVolumeWithoutRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  // Need this otherwise the persistent volumes are not created
  // within the slave work_dir and thus not retrievable.
  flags.work_dir = os::getcwd();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "echo abc > volume/file");

  executor.add_resources()->CopyFrom(createPersistentVolume(
      Megabytes(32),
      "test_role",
      "persistent_volume_id",
      "volume"));

  executor.mutable_container()->CopyFrom(createContainerInfo());

  // Create a persistent volume.
  string volume = slave::paths::getPersistentVolumePath(
      os::getcwd(),
      "test_role",
      "persistent_volume_id");

  ASSERT_SOME(os::mkdir(volume));

  // To make sure the sandbox directory has the container ID in its
  // path so it doesn't get unmounted by the launcher.
  string directory = slave::paths::createExecutorDirectory(
      flags.work_dir,
      SlaveID(),
      FrameworkID(),
      executor.executor_id(),
      containerId);

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copying.
  AWAIT_READY_FOR(launch, Seconds(60));

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());

  EXPECT_SOME_EQ("abc\n", os::read(path::join(volume, "file")));
}


// This test verifies that the image specified in the volume will be
// properly provisioned and mounted into the container if container
// root filesystem is not specified.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_ImageInVolumeWithoutRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "test -d rootfs/bin");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      None(),
      {createVolumeFromImage("rootfs", "test_image", Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copying.
  AWAIT_READY_FOR(launch, Seconds(60));

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies that the image specified in the volume will be
// properly provisioned and mounted into the container if container
// root filesystem is specified.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_ImageInVolumeWithRootFilesystem)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(
        flags,
        {{"test_image_rootfs", path::join(os::getcwd(), "test_image_rootfs")},
         {"test_image_volume", path::join(os::getcwd(), "test_image_volume")}});

  ASSERT_SOME(containerizer);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      "[ ! -d '" + os::getcwd() + "' ] && [ -d rootfs/bin ]");

  executor.mutable_container()->CopyFrom(createContainerInfo(
      "test_image_rootfs",
      {createVolumeFromImage("rootfs", "test_image_volume", Volume::RW)}));

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copy.
  AWAIT_READY_FOR(launch, Seconds(240));

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  // Because destroy rootfs spents a lot of time, we use 30s as timeout here.
  AWAIT_READY_FOR(wait, Seconds(30));

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies that multiple containers with images can be
// launched simultaneously with no interference.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_MultipleContainers)
{
  slave::Flags flags = CreateSlaveFlags();

  // Need this otherwise the persistent volumes are not created
  // within the slave work_dir and thus not retrievable.
  flags.work_dir = os::getcwd();

  ContainerID containerId1;
  containerId1.set_value(UUID::random().toString());

  ContainerID containerId2;
  containerId2.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer =
    createContainerizer(
        flags,
        {{"test_image1", path::join(os::getcwd(), "test_image1")},
         {"test_image2", path::join(os::getcwd(), "test_image2")}});

  ASSERT_SOME(containerizer);

  SlaveID slaveId;
  slaveId.set_value("test_slave");

  // First launch container 1 which has a long running task which
  // guarantees that its work directory mount is in the host mount
  // table when container 2 is launched.
  ExecutorInfo executor1 = CREATE_EXECUTOR_INFO(
      "test_executor1",
      "sleep 1000"); // Long running task.

  executor1.mutable_container()->CopyFrom(createContainerInfo("test_image1"));

  // Create a persistent volume for container 1. We do this because
  // we want to test container 2 cleaning up multiple mounts.
  executor1.add_resources()->CopyFrom(createPersistentVolume(
      Megabytes(32),
      "test_role",
      "persistent_volume_id",
      "volume"));

  string volume = slave::paths::getPersistentVolumePath(
      os::getcwd(),
      "test_role",
      "persistent_volume_id");

  ASSERT_SOME(os::mkdir(volume));

  string directory1 = slave::paths::createExecutorDirectory(
      flags.work_dir,
      slaveId,
      DEFAULT_FRAMEWORK_INFO.id(),
      executor1.executor_id(),
      containerId1);

  Future<bool> launch1 = containerizer.get()->launch(
      containerId1,
      executor1,
      directory1,
      None(),
      slaveId,
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copy.
  AWAIT_READY_FOR(launch1, Seconds(120));

  // Now launch container 2 which will copy the host mount table with
  // container 1's work directory mount in it.
  ExecutorInfo executor2 = CREATE_EXECUTOR_INFO(
      "test_executor2",
      "[ ! -d '" + os::getcwd() + "' ]");

  executor2.mutable_container()->CopyFrom(createContainerInfo("test_image2"));

  string directory2 = slave::paths::createExecutorDirectory(
      flags.work_dir,
      slaveId,
      DEFAULT_FRAMEWORK_INFO.id(),
      executor2.executor_id(),
      containerId2);

  Future<bool> launch2 = containerizer.get()->launch(
      containerId2,
      executor2,
      directory2,
      None(),
      slaveId,
      PID<Slave>(),
      false);

  // Need to wait for Rootfs copy.
  AWAIT_READY_FOR(launch1, Seconds(60));

  containerizer.get()->destroy(containerId1);

  // Wait on the containers.
  Future<containerizer::Termination> wait1 =
    containerizer.get()->wait(containerId1);
  Future<containerizer::Termination> wait2 =
    containerizer.get()->wait(containerId2);

  AWAIT_READY(wait1);
  AWAIT_READY(wait2);

  // Executor 1 was forcefully killed.
  EXPECT_TRUE(wait1.get().has_status());
  EXPECT_EQ(9, wait1.get().status());

  // Executor 2 exited normally.
  EXPECT_TRUE(wait2.get().has_status());
  EXPECT_EQ(0, wait2.get().status());
}


// This test verifies that the environment variables for sandbox
// (i.e., MESOS_DIRECTORY and MESOS_SANDBOX) are set properly.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_SandboxEnvironmentVariable)
{
  slave::Flags flags = CreateSlaveFlags();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Try<Owned<MesosContainerizer>> containerizer = createContainerizer(
      flags,
      {{"test_image", path::join(os::getcwd(), "test_image")}});

  ASSERT_SOME(containerizer);

  string directory = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  Try<string> script = strings::format(
      "if [ \"$MESOS_DIRECTORY\" != \"%s\" ]; then exit 1; fi &&"
      "if [ \"$MESOS_SANDBOX\" != \"%s\" ]; then exit 1; fi",
      directory,
      flags.sandbox_directory);

  ASSERT_SOME(script);

  ExecutorInfo executor = CREATE_EXECUTOR_INFO(
      "test_executor",
      script.get());

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  Future<bool> launch = containerizer.get()->launch(
      containerId,
      executor,
      directory,
      None(),
      SlaveID(),
      PID<Slave>(),
      false);

  // Wait for the launch to complete.
  // Need to wait for Rootfs copy.
  AWAIT_READY_FOR(launch, Seconds(60));

  // Wait on the container.
  Future<containerizer::Termination> wait =
    containerizer.get()->wait(containerId);

  AWAIT_READY(wait);

  // Check the executor exited correctly.
  EXPECT_TRUE(wait.get().has_status());
  EXPECT_EQ(0, wait.get().status());
}


// This test verifies the slave's work directory mount preparation if
// the mount does not exist initially.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_WorkDirMount)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<Isolator*> isolator = LinuxFilesystemIsolatorProcess::create(flags);

  ASSERT_SOME(isolator);

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  ASSERT_SOME(table);

  bool mountFound = false;
  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    if (entry.target == flags.work_dir) {
      EXPECT_SOME(entry.shared());
      mountFound = true;
    }
  }

  EXPECT_TRUE(mountFound);

  delete isolator.get();
}


// This test verifies the slave's work directory mount preparation if
// the mount already exists (e.g., to simulate the case when the slave
// crashes while preparing the work directory mount).
TEST_F(LinuxFilesystemIsolatorTest, ROOT_WorkDirMountPreExists)
{
  slave::Flags flags = CreateSlaveFlags();

  // Simulate the situation in which the slave crashes while preparing
  // the work directory mount.
  ASSERT_SOME(os::shell(
      "mount --bind %s %s",
      flags.work_dir.c_str(),
      flags.work_dir.c_str()));

  Try<Isolator*> isolator = LinuxFilesystemIsolatorProcess::create(flags);

  ASSERT_SOME(isolator);

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  ASSERT_SOME(table);

  bool mountFound = false;
  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    if (entry.target == flags.work_dir) {
      EXPECT_SOME(entry.shared());
      mountFound = true;
    }
  }

  EXPECT_TRUE(mountFound);

  delete isolator.get();
}


// This test verifies that the volume usage accounting for sandboxes with
// bind-mounted volumes works correctly by creating a file within the volume
// the size of which exceeds the sandbox quota.
TEST_F(LinuxFilesystemIsolatorTest, ROOT_VolumeUsageExceedsSandboxQuota)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("role1");

  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/disk,filesystem/linux";

  // NOTE: We can't pause the clock because we need the reaper to reap
  // the 'du' subprocess.
  flags.container_disk_watch_interval = Milliseconds(1);
  flags.enforce_container_disk_quota = true;
  flags.resources = "cpus:2;mem:128;disk(role1):128";

  Try<PID<Slave>> slave = StartSlave(flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

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

  // We request a sandbox (1MB) that is smaller than the persistent
  // volume (4MB) and attempt to create a file in that volume that is
  // twice the size of the sanbox (2MB).
  Resources volume = createPersistentVolume(
      Megabytes(4),
      "role1",
      "id1",
      "volume_path");

  Resources taskResources =
      Resources::parse("cpus:1;mem:64;disk(role1):1").get() + volume;

  // We sleep to give quota enforcement (du) a chance to kick in.
  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      taskResources,
      "dd if=/dev/zero of=volume_path/file bs=1048576 count=2 && sleep 1");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.acceptOffers(
      {offers.get()[0].id()},
      {CREATE(volume),
      LAUNCH({task})});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning.get().task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished.get().task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}

#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
