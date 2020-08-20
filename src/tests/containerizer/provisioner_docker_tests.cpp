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

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>

#include <mesos/docker/spec.hpp>

#ifdef __linux__
#include "linux/fs.hpp"
#endif // __linux__

#include "slave/containerizer/mesos/provisioner/constants.hpp"
#include "slave/containerizer/mesos/provisioner/paths.hpp"

#include "slave/containerizer/mesos/provisioner/docker/message.hpp"
#include "slave/containerizer/mesos/provisioner/docker/metadata_manager.hpp"
#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/store.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

#ifdef __linux__
#include "tests/containerizer/docker_archive.hpp"
#endif // __linux__

namespace master = mesos::internal::master;
namespace paths = mesos::internal::slave::docker::paths;
namespace slave = mesos::internal::slave;
namespace spec = ::docker::spec;

using std::string;
using std::vector;

using process::Clock;
using process::Future;
using process::Owned;
using process::PID;
using process::Promise;

using master::Master;

using mesos::internal::slave::AUFS_BACKEND;
using mesos::internal::slave::Containerizer;
using mesos::internal::slave::COPY_BACKEND;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::OVERLAY_BACKEND;
using mesos::internal::slave::Provisioner;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerTermination;

using slave::ImageInfo;
using slave::Slave;

using slave::docker::Puller;
using slave::docker::RegistryPuller;
using slave::docker::Store;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

class ProvisionerDockerLocalStoreTest : public TemporaryDirectoryTest
{
public:
  void verifyLocalDockerImage(
      const slave::Flags& flags,
      const vector<string>& layers)
  {
    // Verify contents of the image in store directory.
    const string layerPath1 = paths::getImageLayerRootfsPath(
        flags.docker_store_dir,
        "123",
        COPY_BACKEND);

    const string layerPath2 = paths::getImageLayerRootfsPath(
        flags.docker_store_dir,
        "456",
        COPY_BACKEND);

    EXPECT_TRUE(os::exists(layerPath1));
    EXPECT_TRUE(os::exists(layerPath2));

    EXPECT_SOME_EQ(
        "foo 123",
        os::read(path::join(layerPath1, "temp")));

    EXPECT_SOME_EQ(
        "bar 456",
        os::read(path::join(layerPath2, "temp")));

    // Verify the Docker Image provided.
    vector<string> expectedLayers;
    expectedLayers.push_back(layerPath1);
    expectedLayers.push_back(layerPath2);
    EXPECT_EQ(expectedLayers, layers);
  }

protected:
  void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    const string archivesDir = path::join(os::getcwd(), "images");
    const string image = path::join(archivesDir, "abc");
    ASSERT_SOME(os::mkdir(archivesDir));
    ASSERT_SOME(os::mkdir(image));

    JSON::Value repositories = JSON::parse(
        "{"
        "  \"abc\": {"
        "    \"latest\": \"456\""
        "  }"
        "}").get();
    ASSERT_SOME(
        os::write(path::join(image, "repositories"), stringify(repositories)));

    ASSERT_SOME(os::mkdir(path::join(image, "123")));
    JSON::Value manifest123 = JSON::parse(
        "{"
        "  \"parent\": \"\""
        "}").get();
    ASSERT_SOME(os::write(
        path::join(image, "123", "json"), stringify(manifest123)));
    ASSERT_SOME(os::mkdir(path::join(image, "123", "layer")));
    ASSERT_SOME(
        os::write(path::join(image, "123", "layer", "temp"), "foo 123"));

    // Must change directory to avoid carrying over /path/to/archive during tar.
    const string cwd = os::getcwd();
    ASSERT_SOME(os::chdir(path::join(image, "123", "layer")));
    ASSERT_SOME(os::tar(".", "../layer.tar"));
    ASSERT_SOME(os::chdir(cwd));
    ASSERT_SOME(os::rmdir(path::join(image, "123", "layer")));

    ASSERT_SOME(os::mkdir(path::join(image, "456")));
    JSON::Value manifest456 = JSON::parse(
        "{"
        "  \"parent\": \"123\""
        "}").get();
    ASSERT_SOME(
        os::write(path::join(image, "456", "json"), stringify(manifest456)));
    ASSERT_SOME(os::mkdir(path::join(image, "456", "layer")));
    ASSERT_SOME(
        os::write(path::join(image, "456", "layer", "temp"), "bar 456"));

    ASSERT_SOME(os::chdir(path::join(image, "456", "layer")));
    ASSERT_SOME(os::tar(".", "../layer.tar"));
    ASSERT_SOME(os::chdir(cwd));
    ASSERT_SOME(os::rmdir(path::join(image, "456", "layer")));

    ASSERT_SOME(os::chdir(image));
    ASSERT_SOME(os::tar(".", "../abc.tar"));
    ASSERT_SOME(os::chdir(cwd));
    ASSERT_SOME(os::rmdir(image));
  }
};


// This test verifies that a locally stored Docker image in the form of a
// tar archive created from a 'docker save' command can be unpacked and
// stored in the proper locations accessible to the Docker provisioner.
TEST_F(ProvisionerDockerLocalStoreTest, LocalStoreTestWithTar)
{
  slave::Flags flags;
  flags.docker_registry = path::join(os::getcwd(), "images");
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.image_provisioner_backend = COPY_BACKEND;

  Try<Owned<slave::Store>> store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);

  Image mesosImage;
  mesosImage.set_type(Image::DOCKER);
  mesosImage.mutable_docker()->set_name("abc");

  Future<slave::ImageInfo> imageInfo =
    store.get()->get(mesosImage, COPY_BACKEND);

  AWAIT_READY(imageInfo);

  verifyLocalDockerImage(flags, imageInfo->layers);
}


// This tests the ability of the metadata manger to recover the images it has
// already stored on disk when it is initialized.
TEST_F(ProvisionerDockerLocalStoreTest, MetadataManagerInitialization)
{
  slave::Flags flags;
  flags.docker_registry = path::join(os::getcwd(), "images");
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.image_provisioner_backend = COPY_BACKEND;

  Try<Owned<slave::Store>> store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("abc");

  Future<slave::ImageInfo> imageInfo = store.get()->get(image, COPY_BACKEND);
  AWAIT_READY(imageInfo);

  // Store is deleted and recreated. Metadata Manager is initialized upon
  // creation of the store.
  store->reset();
  store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);
  Future<Nothing> recover = store.get()->recover();
  AWAIT_READY(recover);

  imageInfo = store.get()->get(image, COPY_BACKEND);
  AWAIT_READY(imageInfo);
  verifyLocalDockerImage(flags, imageInfo->layers);
}

// This is a regression test for MESOS-8871.
// This test the ability of the metadata manger to ignore the empty images
// file when it recover images.
TEST_F(ProvisionerDockerLocalStoreTest,
       MetadataManagerRecoveryWithEmptyImagesFile)
{
  slave::Flags flags;
  flags.docker_registry = path::join(os::getcwd(), "images");
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.image_provisioner_backend = COPY_BACKEND;
  const string emptyImages = paths::getStoredImagesPath(flags.docker_store_dir);

  ASSERT_SOME(os::mkdir(flags.docker_store_dir));
  ASSERT_SOME(os::touch(emptyImages));

  Try<Owned<slave::Store>> store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);

  Future<Nothing> recover = store.get()->recover();
  AWAIT_READY(recover);
}

// This test verifies that the layer that is missing from the store
// will be pulled.
TEST_F(ProvisionerDockerLocalStoreTest, MissingLayer)
{
  slave::Flags flags;
  flags.docker_registry = path::join(os::getcwd(), "images");
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.image_provisioner_backend = COPY_BACKEND;

  Try<Owned<slave::Store>> store = Store::create(flags);
  ASSERT_SOME(store);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("abc");

  Future<slave::ImageInfo> imageInfo = store.get()->get(
      image, flags.image_provisioner_backend.get());

  AWAIT_READY(imageInfo);
  verifyLocalDockerImage(flags, imageInfo->layers);

  // Remove one of the layers from the store.
  ASSERT_SOME(os::rmdir(paths::getImageLayerRootfsPath(
      flags.docker_store_dir, "456", flags.image_provisioner_backend.get())));

  // Pull the image again to get the missing layer.
  imageInfo = store.get()->get(image, flags.image_provisioner_backend.get());
  AWAIT_READY(imageInfo);
  verifyLocalDockerImage(flags, imageInfo->layers);
}


class MockPuller : public Puller
{
public:
  MockPuller()
  {
    EXPECT_CALL(*this, pull(_, _, _, _))
      .WillRepeatedly(Invoke(this, &MockPuller::unmocked_pull));
  }

  ~MockPuller() override {}

  MOCK_METHOD4(
      pull,
      Future<slave::docker::Image>(
          const spec::ImageReference&,
          const string&,
          const string&,
          const Option<Secret>&));

  Future<slave::docker::Image> unmocked_pull(
      const spec::ImageReference& reference,
      const string& directory,
      const string& backend,
      const Option<Secret>& config)
  {
    // TODO(gilbert): Allow return Image to be overridden.
    return slave::docker::Image();
  }
};


// This tests the store to pull the same image simultaneously.
// This test verifies that the store only calls the puller once
// when multiple requests for the same image is in flight.
// In addition, this test verifies that if some of the futures
// returned by `Store::get()` that are pending image pull are discarded,
// the pull still completes.
TEST_F(ProvisionerDockerLocalStoreTest, PullingSameImageSimultaneously)
{
  slave::Flags flags;
  flags.docker_registry = path::join(os::getcwd(), "images");
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  MockPuller* puller = new MockPuller();
  Future<Nothing> pull;
  Future<string> directory;
  Promise<slave::docker::Image> promise;

  EXPECT_CALL(*puller, pull(_, _, _, _))
    .WillOnce(testing::DoAll(FutureSatisfy(&pull),
                             FutureArg<1>(&directory),
                             Return(promise.future())));

  Try<Owned<slave::Store>> store =
      slave::docker::Store::create(flags, Owned<Puller>(puller));
  ASSERT_SOME(store);

  Image mesosImage;
  mesosImage.set_type(Image::DOCKER);
  mesosImage.mutable_docker()->set_name("abc");

  Future<slave::ImageInfo> imageInfo1 =
    store.get()->get(mesosImage, COPY_BACKEND);

  AWAIT_READY(pull);
  AWAIT_READY(directory);

  // TODO(gilbert): Need a helper method to create test layers
  // which will allow us to set manifest so that we can add
  // checks here.
  const string layerPath = path::join(directory.get(), "456");

  Try<Nothing> mkdir = os::mkdir(layerPath);
  ASSERT_SOME(mkdir);

  JSON::Value manifest = JSON::parse(
        "{"
        "  \"parent\": \"\""
        "}").get();

  ASSERT_SOME(
      os::write(path::join(layerPath, "json"), stringify(manifest)));

  ASSERT_TRUE(imageInfo1.isPending());
  Future<slave::ImageInfo> imageInfo2 =
    store.get()->get(mesosImage, COPY_BACKEND);

  Future<slave::ImageInfo> imageInfo3 =
    store.get()->get(mesosImage, COPY_BACKEND);

  Try<spec::ImageReference> reference =
    spec::parseImageReference(mesosImage.docker().name());

  ASSERT_SOME(reference);

  slave::docker::Image result;
  result.mutable_reference()->CopyFrom(reference.get());
  result.add_layer_ids("456");

  ASSERT_TRUE(imageInfo2.isPending());
  ASSERT_TRUE(imageInfo3.isPending());

  // Pull should still complete, despite of the discard of a single future.
  imageInfo3.discard();

  promise.set(result);

  AWAIT_READY(imageInfo1);
  AWAIT_READY(imageInfo2);

  EXPECT_EQ(imageInfo1->layers, imageInfo2->layers);
}


// This tests that pulling the image will be cancelled if all the pending
// futures returned by `Store::get()` for this pull are discarded.
TEST_F(ProvisionerDockerLocalStoreTest, PullDiscarded)
{
  slave::Flags flags;
  flags.docker_registry = path::join(os::getcwd(), "images");
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  MockPuller* puller = new MockPuller();
  Future<Nothing> pullCalled;
  Promise<slave::docker::Image> promise;

  EXPECT_CALL(*puller, pull(_, _, _, _))
    .WillOnce(testing::DoAll(FutureSatisfy(&pullCalled),
                             Return(promise.future())));

  Try<Owned<slave::Store>> store =
      slave::docker::Store::create(flags, Owned<Puller>(puller));

  ASSERT_SOME(store);

  Image mesosImage;
  mesosImage.set_type(Image::DOCKER);
  mesosImage.mutable_docker()->set_name("abc");

  Future<slave::ImageInfo> imageInfo1 =
    store.get()->get(mesosImage, COPY_BACKEND);

  Future<slave::ImageInfo> imageInfo2 =
    store.get()->get(mesosImage, COPY_BACKEND);

  AWAIT_READY(pullCalled);

  imageInfo1.discard();
  imageInfo2.discard();

  Clock::pause();
  Clock::settle();

  ASSERT_TRUE(promise.future().hasDiscard());
}


#ifdef __linux__
class ProvisionerDockerTest
  : public MesosTest,
    public WithParamInterface<string> {};


// This test verifies that local docker image can be pulled and
// provisioned correctly, and shell command should be executed.
TEST_F(ProvisionerDockerTest, ROOT_ImageTarPullerSimpleCommand)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage = DockerArchive::create(directory, "alpine");
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

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      "ls -al /");

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


class ProvisionerDockerHdfsTest
  : public MesosTest,
    public WithParamInterface<string> {};


// The host of HDFS can be a remote host or local directory.
INSTANTIATE_TEST_CASE_P(
    HdfsHost,
    ProvisionerDockerHdfsTest,
    ::testing::ValuesIn(vector<string>({
        "hdfs://localhost:8020",
        "hdfs://"})));


// This test verifies that the image tar puller could pull image
// with the hdfs uri fetcher plugin.
TEST_P(ProvisionerDockerHdfsTest, ROOT_ImageTarPullerHdfsFetcherSimpleCommand)
{
  string hadoopPath = os::getcwd();
  ASSERT_TRUE(os::exists(hadoopPath));

  string hadoopBinPath = path::join(hadoopPath, "bin");
  ASSERT_SOME(os::mkdir(hadoopBinPath));
  ASSERT_SOME(os::chmod(hadoopBinPath, S_IRWXU | S_IRWXG | S_IRWXO));

  const string& proof = path::join(hadoopPath, "proof");

  // This acts exactly as "hadoop" for testing purposes. On some platforms, the
  // "hadoop" wrapper command will emit a warning that Hadoop installation has
  // no native code support. We always emit that here to make sure it is parsed
  // correctly.
  string mockHadoopScript =
    "#!/usr/bin/env bash\n"
    "\n"
    "touch " + proof + "\n"
    "\n"
    "now=$(date '+%y/%m/%d %I:%M:%S')\n"
    "echo \"$now WARN util.NativeCodeLoader: "
      "Unable to load native-hadoop library for your platform...\" 1>&2\n"
    "\n"
    "if [[ 'version' == $1 ]]; then\n"
    "  echo $0 'for Mesos testing'\n"
    "fi\n"
    "\n"
    "# hadoop fs -copyToLocal $3 $4\n"
    "if [[ 'fs' == $1 && '-copyToLocal' == $2 ]]; then\n"
    "  if [[ $3 == 'hdfs://'* ]]; then\n"
    "    # Remove 'hdfs://<host>/' and use just the (absolute) path.\n"
    "    withoutProtocol=${3/'hdfs:'\\/\\//}\n"
    "    withoutHost=${withoutProtocol#*\\/}\n"
    "    absolutePath='/'$withoutHost\n"
    "   cp $absolutePath $4\n"
    "  else\n"
    "    cp $3 $4\n"
    "  fi\n"
    "fi\n";

  string hadoopCommand = path::join(hadoopBinPath, "hadoop");
  ASSERT_SOME(os::write(hadoopCommand, mockHadoopScript));
  ASSERT_SOME(os::chmod(hadoopCommand,
                        S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage = DockerArchive::create(directory, "alpine");
  AWAIT_READY(testImage);

  ASSERT_TRUE(os::exists(path::join(directory, "alpine.tar")));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_registry = GetParam() + directory;
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.hadoop_home = hadoopPath;

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

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      "ls -al /");

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


// For official Docker images, users can omit the 'library/' prefix
// when specifying the repository name (e.g., 'busybox'). The registry
// puller normalize docker official images if necessary.
INSTANTIATE_TEST_CASE_P(
    ContainerImage,
    ProvisionerDockerTest,
    ::testing::ValuesIn(vector<string>({
        "alpine", // Verifies the normalization of the Docker repository name.
        "library/alpine",
        "gcr.io/google-containers/busybox:1.24", // manifest.v1+prettyjws
        "gcr.io/google-containers/busybox:1.27", // manifest.v2+json
        // TODO(alexr): The registry below is unreliable and hence disabled.
        // Consider re-enabling shall it become more stable.
        // "registry.cn-hangzhou.aliyuncs.com/acs-sample/alpine",
        "quay.io/coreos/alpine-sh" // manifest.v1+prettyjws
      })));


// TODO(jieyu): This is a ROOT test because of MESOS-4757. Remove the
// ROOT restriction after MESOS-4757 is resolved.
TEST_P(ProvisionerDockerTest, ROOT_INTERNET_CURL_SimpleCommand)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";

  // Image pulling time may be long, depending on the location of
  // the registry server.
  flags.executor_registration_timeout = Minutes(10);

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

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/ls");
  command.add_arguments("ls");
  command.add_arguments("-al");
  command.add_arguments("/");

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name(GetParam());

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

  AWAIT_READY_FOR(statusStarting, Minutes(10));
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies the functionality of image `cached` option
// for image force pulling.
TEST_F(ProvisionerDockerTest, ROOT_INTERNET_CURL_ImageForcePulling)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";

  // Image pulling time may be long, depending on the location of
  // the registry server.
  flags.executor_registration_timeout = Minutes(10);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1, offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_EQ(1u, offers1->size());

  const Offer& offer1 = offers1.get()[0];

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/ls");
  command.add_arguments("ls");
  command.add_arguments("-al");
  command.add_arguments("/");

  TaskInfo task = createTask(
      offer1.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<TaskStatus> statusStarting1;
  Future<TaskStatus> statusRunning1;
  Future<TaskStatus> statusFinished1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting1))
    .WillOnce(FutureArg<1>(&statusRunning1))
    .WillOnce(FutureArg<1>(&statusFinished1));

  driver.launchTasks(offer1.id(), {task});

  AWAIT_READY_FOR(statusStarting1, Minutes(10));
  EXPECT_EQ(task.task_id(), statusStarting1->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting1->state());

  AWAIT_READY(statusRunning1);
  EXPECT_EQ(task.task_id(), statusRunning1->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning1->state());

  AWAIT_READY(statusFinished1);
  EXPECT_EQ(task.task_id(), statusFinished1->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished1->state());

  AWAIT_READY(offers2);
  ASSERT_EQ(1u, offers2->size());

  const Offer& offer2 = offers2.get()[0];

  task = createTask(
      offer2.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  // Image force pulling.
  image.set_cached(false);

  container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<TaskStatus> statusStarting2;
  Future<TaskStatus> statusRunning2;
  Future<TaskStatus> statusFinished2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting2))
    .WillOnce(FutureArg<1>(&statusRunning2))
    .WillOnce(FutureArg<1>(&statusFinished2));

  driver.launchTasks(offer2.id(), {task});

  AWAIT_READY_FOR(statusStarting2, Minutes(10));
  EXPECT_EQ(task.task_id(), statusStarting2->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting2->state());

  AWAIT_READY(statusRunning2);
  EXPECT_EQ(task.task_id(), statusRunning2->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning2->state());

  AWAIT_READY(statusFinished2);
  EXPECT_EQ(task.task_id(), statusFinished2->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished2->state());

  driver.stop();
  driver.join();
}


// This test verifies that the scratch based docker image (that
// only contain a single binary and its dependencies) can be
// launched correctly.
TEST_F(ProvisionerDockerTest, ROOT_INTERNET_CURL_ScratchImage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";

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

  CommandInfo command;
  command.set_shell(false);

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  Image image;
  image.set_type(Image::DOCKER);

  // 'hello-world' is a scratch image. It contains only one
  // binary 'hello' in its rootfs.
  image.mutable_docker()->set_name("hello-world");

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


class ProvisionerDockerBackendTest
  : public MesosTest,
    public WithParamInterface<string>
{
public:
  // Returns the supported backends.
  static vector<string> parameters()
  {
    vector<string> backends = {COPY_BACKEND};

    Try<bool> aufsSupported = fs::supported("aufs");
    if (aufsSupported.isSome() && aufsSupported.get()) {
      backends.push_back(AUFS_BACKEND);
    }

    Try<bool> overlayfsSupported = fs::supported("overlayfs");
    if (overlayfsSupported.isSome() && overlayfsSupported.get()) {
      backends.push_back(OVERLAY_BACKEND);
    }

    return backends;
  }
};


INSTANTIATE_TEST_CASE_P(
    BackendFlag,
    ProvisionerDockerBackendTest,
    ::testing::ValuesIn(ProvisionerDockerBackendTest::parameters()));


// This test verifies that a docker image containing whiteout files
// will be processed correctly by copy, aufs and overlay backends.
TEST_P(ProvisionerDockerBackendTest, ROOT_INTERNET_CURL_DTYPE_Whiteout)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.image_provisioner_backend = GetParam();

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

  // We are using the docker image 'zhq527725/whiteout' to verify that the
  // files '/dir1/file1' and '/dir1/dir2/file2' do not exist in container's
  // rootfs because of the following two whiteout files in the image:
  //   '/dir1/.wh.file1'
  //   '/dir1/dir2/.wh..wh..opq'
  // And we also verify that the file '/dir1/dir2/file3' exists in container's
  // rootfs which will NOT be applied by '/dir1/dir2/.wh..wh..opq' since they
  // are in the same layer.
  // See more details about this docker image in the link below:
  //   https://hub.docker.com/r/zhq527725/whiteout/
  CommandInfo command = createCommandInfo(
      "test ! -f /dir1/file1 && "
      "test ! -f /dir1/dir2/file2 && "
      "test -f /dir1/dir2/file3");

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  Image image = createDockerImage("zhq527725/whiteout");

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

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that the provisioner correctly overwrites a
// directory in underlying layers with a with a regular file or symbolic
// link of the same name in an upper layer, and vice versa.
TEST_P(ProvisionerDockerBackendTest, ROOT_INTERNET_CURL_DTYPE_Overwrite)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.image_provisioner_backend = GetParam();

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

  // We are using the docker image 'chhsiao/overwrite' to verify that:
  //   1. The '/merged' directory is merged.
  //   2. All '/replaced*' files/directories are correctly overwritten.
  //   3. The '/foo', '/bar' and '/baz' files are correctly overwritten.
  // See more details in the following link:
  //   https://hub.docker.com/r/chhsiao/overwrite/
  CommandInfo command = createCommandInfo(
      "test -f /replaced1 &&"
      "test -L /replaced2 &&"
      "test -f /replaced2/m1 &&"
      "test -f /replaced2/m2 &&"
      "! test -e /replaced2/r2 &&"
      "test -d /replaced3 &&"
      "test -d /replaced4 &&"
      "! test -e /replaced4/m1 &&"
      "test -f /foo &&"
      "! test -L /bar &&"
      "test -L /baz");

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  Image image = createDockerImage("chhsiao/overwrite");

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

  // Create a non-empty file 'abc' in the agent's work directory on the
  // host filesystem. This file is symbolically linked by
  //   '/xyz -> ../../../../../../../abc'
  // in the 2nd layer of the testing image during provisioning.
  // For more details about the provisioner directory please see:
  //   https://github.com/apache/mesos/blob/master/src/slave/containerizer/mesos/provisioner/paths.hpp#L34-L48 // NOLINT
  const string hostFile = path::join(flags.work_dir, "abc");
  ASSERT_SOME(os::write(hostFile, "abc"));
  ASSERT_SOME(os::shell("test -s " + hostFile));

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

  // The non-empty file should not be overwritten by the empty file
  // '/xyz' in the 3rd layer of the testing image during provisioning.
  EXPECT_SOME(os::shell("test -s " + hostFile));

  driver.stop();
  driver.join();
}


// This test verifies that the container rootfs can be unmounted correctly
// during cleanup. This is a regression test for container cleanpu EBUSY
// issue. Please see MESOS-9196 for details.
TEST_P(ProvisionerDockerBackendTest, ROOT_INTERNET_CURL_DTYPE_RootfsCleanup)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.image_provisioner_backend = GetParam();

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      flags);

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

  CommandInfo command = createCommandInfo("sleep 1000");
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  Image image = createDockerImage("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(Return());

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containerIds = containerizer->containers();
  AWAIT_READY(containerIds);
  ASSERT_EQ(1u, containerIds->size());

  const ContainerID& containerId = *containerIds->begin();
  Try<hashmap<string, hashset<string>>> rootfses =
    slave::provisioner::paths::listContainerRootfses(
        slave::paths::getProvisionerDir(flags.work_dir),
        containerId);

  ASSERT_SOME(rootfses);
  ASSERT_EQ(1u, rootfses->size());
  ASSERT_EQ(1u, rootfses->values().begin()->size());

  const string rootfsDir = slave::provisioner::paths::getContainerRootfsDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId,
      *rootfses->keys().begin(),
      *rootfses->values().begin()->begin());

  // This keeps a reference to the persistent volume mount.
  Try<int_fd> fd = os::open(
      path::join(rootfsDir, "etc/hostname"),
      O_WRONLY | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(fd);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());

  // Verifies that mount point has been removed.
  EXPECT_FALSE(os::exists(path::join(rootfsDir, "etc/hostname")));

  os::close(fd.get());

  driver.stop();
  driver.join();
}


// This test verifies that Docker image can be pulled from the
// repository by digest.
TEST_F(ProvisionerDockerTest, ROOT_INTERNET_CURL_ImageDigest)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";

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

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/bin' is not in the PATH by default.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/ls");
  command.add_arguments("ls");
  command.add_arguments("-al");
  command.add_arguments("/");

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  // NOTE: We use the digest of the 'alpine:2.7' image because it has a
  // Schema 1 manifest (the only manifest schema that we currently support).
  const string digest =
    "sha256:9f08005dff552038f0ad2f46b8e65ff3d25641747d3912e3ea8da6785046561a";

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("library/alpine@" + digest);

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


// This test verifies that Docker manifest configuration is not carried over
// into the container if the `--docker_ignore_runtime` flag is set. Due to the
// complexity of the `CommandInfo` mapping, simply check to see if the
// environment was merged by the isolator into the task container.
TEST_F(ProvisionerDockerTest, ROOT_DockerIgnoreRuntime)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  const string directory = path::join(os::getcwd(), "registry");

  const std::vector<std::string>& environment = {
    {"DOCKER_RUNTIME_ENV=true"},
  };

  // Setting the entrypoint and command that will be reflected in the
  // manifest.
  Future<Nothing> testImage = DockerArchive::create(
      directory, "alpine", "null", "null", environment);
  AWAIT_READY(testImage);

  ASSERT_TRUE(os::exists(path::join(directory, "alpine.tar")));

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_registry = directory;
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.docker_ignore_runtime = true;

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

  // This task will fail if Docker manifest metadata is propagated
  // to the container i.e. the `DOCKER_RUNTIME_ENV` variable should
  // not be present in the container since we are setting the
  // `--docker_ignore_runtime` flag.
  TaskInfo task = createTask(
    offer.slave_id(),
    offer.resources(),
    "test -z $DOCKER_RUNTIME_ENV");

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

  AWAIT_READY(statusStarting);
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that if a container image is specified, the
// command runs as the specified user "$SUDO_USER" and the sandbox of
// the command task is writeable by the specified user. It also
// verifies that stdout/stderr are owned by the specified user.
TEST_F(ProvisionerDockerTest,
       ROOT_INTERNET_CURL_UNPRIVILEGED_USER_CommandTaskUser)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";

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

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  Result<uid_t> uid = os::getuid(user.get());
  ASSERT_SOME(uid);

  CommandInfo command;
  command.set_user(user.get());
  command.set_value(strings::format(
      "#!/bin/sh\n"
      "touch $MESOS_SANDBOX/file\n"
      "FILE_UID=`stat -c %%u $MESOS_SANDBOX/file`\n"
      "test $FILE_UID = %d\n"
      "STDOUT_UID=`stat -c %%u $MESOS_SANDBOX/stdout`\n"
      "test $STDOUT_UID = %d\n"
      "STDERR_UID=`stat -c %%u $MESOS_SANDBOX/stderr`\n"
      "test $STDERR_UID = %d\n",
      uid.get(), uid.get(), uid.get()).get());

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

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


// This test simulate the case that after the agent reboots the
// container runtime directory is gone while the provisioner
// directory still survives. The recursive `provisioner::destroy()`
// can make sure that a child container is always cleaned up
// before its parent container.
TEST_F(ProvisionerDockerTest, ROOT_RecoverNestedOnReboot)
{
  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage = DockerArchive::create(directory, "alpine");
  AWAIT_READY(testImage);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_registry = directory;
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  AWAIT_READY(provisioner.get()->provision(nestedContainerId, image));

  // Passing an empty hashset to `provisioner::recover()` to
  // simulate the agent reboot scenario.
  AWAIT_READY(provisioner.get()->recover({}));

  const string containerDir = slave::provisioner::paths::getContainerDir(
      slave::paths::getProvisionerDir(flags.work_dir),
      containerId);

  EXPECT_FALSE(os::exists(containerDir));
}

#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
