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

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>

#include <mesos/docker/spec.hpp>

#include "slave/containerizer/mesos/provisioner/docker/metadata_manager.hpp"
#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/store.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

namespace master = mesos::internal::master;
namespace paths = mesos::internal::slave::docker::paths;
namespace slave = mesos::internal::slave;
namespace spec = ::docker::spec;

using std::string;
using std::vector;

using process::Future;
using process::Owned;
using process::PID;
using process::Promise;

using master::Master;

using slave::ImageInfo;
using slave::Slave;

using slave::docker::Puller;
using slave::docker::RegistryPuller;
using slave::docker::Store;

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
        "123");

    const string layerPath2 = paths::getImageLayerRootfsPath(
        flags.docker_store_dir,
        "456");

    EXPECT_TRUE(os::exists(layerPath1));
    EXPECT_TRUE(os::exists(layerPath2));

    EXPECT_SOME_EQ(
        "foo 123",
        os::read(path::join(layerPath1 , "temp")));

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
  virtual void SetUp()
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
// tar achive created from a 'docker save' command can be unpacked and
// stored in the proper locations accessible to the Docker provisioner.
TEST_F(ProvisionerDockerLocalStoreTest, LocalStoreTestWithTar)
{
  const string archivesDir = path::join(os::getcwd(), "images");
  const string image = path::join(archivesDir, "abc");
  ASSERT_SOME(os::mkdir(archivesDir));
  ASSERT_SOME(os::mkdir(image));

  slave::Flags flags;
  flags.docker_registry = archivesDir;
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Try<Owned<slave::Store>> store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);

  Image mesosImage;
  mesosImage.set_type(Image::DOCKER);
  mesosImage.mutable_docker()->set_name("abc");

  Future<slave::ImageInfo> imageInfo = store.get()->get(mesosImage);
  AWAIT_READY(imageInfo);

  verifyLocalDockerImage(flags, imageInfo.get().layers);
}


// This tests the ability of the metadata manger to recover the images it has
// already stored on disk when it is initialized.
TEST_F(ProvisionerDockerLocalStoreTest, MetadataManagerInitialization)
{
  slave::Flags flags;
  flags.docker_registry = path::join(os::getcwd(), "images");
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  Try<Owned<slave::Store>> store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("abc");

  Future<slave::ImageInfo> imageInfo = store.get()->get(image);
  AWAIT_READY(imageInfo);

  // Store is deleted and recreated. Metadata Manager is initialized upon
  // creation of the store.
  store.get().reset();
  store = slave::docker::Store::create(flags);
  ASSERT_SOME(store);
  Future<Nothing> recover = store.get()->recover();
  AWAIT_READY(recover);

  imageInfo = store.get()->get(image);
  AWAIT_READY(imageInfo);
  verifyLocalDockerImage(flags, imageInfo.get().layers);
}


class MockPuller : public Puller
{
public:
  MockPuller()
  {
    EXPECT_CALL(*this, pull(_, _))
      .WillRepeatedly(Invoke(this, &MockPuller::unmocked_pull));
  }

  virtual ~MockPuller() {}

  MOCK_METHOD2(
      pull,
      Future<vector<string>>(
          const spec::ImageReference&,
          const string&));

  Future<vector<string>> unmocked_pull(
      const spec::ImageReference& reference,
      const string& directory)
  {
    // TODO(gilbert): Allow return list to be overridden.
    return vector<string>();
  }
};


// This tests the store to pull the same image simutanuously.
// This test verifies that the store only calls the puller once
// when multiple requests for the same image is in flight.
TEST_F(ProvisionerDockerLocalStoreTest, PullingSameImageSimutanuously)
{
  const string archivesDir = path::join(os::getcwd(), "images");
  const string image = path::join(archivesDir, "abc:latest");
  ASSERT_SOME(os::mkdir(archivesDir));
  ASSERT_SOME(os::mkdir(image));

  slave::Flags flags;
  flags.docker_registry = "file://" + archivesDir;
  flags.docker_store_dir = path::join(os::getcwd(), "store");

  MockPuller* puller = new MockPuller();
  Future<Nothing> pull;
  Future<string> directory;
  Promise<vector<string>> promise;

  EXPECT_CALL(*puller, pull(_, _))
    .WillOnce(testing::DoAll(FutureSatisfy(&pull),
                             FutureArg<1>(&directory),
                             Return(promise.future())));

  Try<Owned<slave::Store>> store =
      slave::docker::Store::create(flags, Owned<Puller>(puller));
  ASSERT_SOME(store);

  Image mesosImage;
  mesosImage.set_type(Image::DOCKER);
  mesosImage.mutable_docker()->set_name("abc");

  Future<slave::ImageInfo> imageInfo1 = store.get()->get(mesosImage);
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
  Future<slave::ImageInfo> imageInfo2 = store.get()->get(mesosImage);

  const vector<string> result = {"456"};

  ASSERT_TRUE(imageInfo2.isPending());
  promise.set(result);

  AWAIT_READY(imageInfo1);
  AWAIT_READY(imageInfo2);

  EXPECT_EQ(imageInfo1.get().layers, imageInfo2.get().layers);
}


#ifdef __linux__
class ProvisionerDockerRegistryPullerTest : public MesosTest {};


// TODO(jieyu): This is a ROOT test because of MESOS-4757. Remove the
// ROOT restriction after MESOS-4757 is resolved.
TEST_F(ProvisionerDockerRegistryPullerTest, ROOT_INTERNET_CURL_ShellCommand)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_registry = "https://registry-1.docker.io";

  Try<PID<Slave>> slave = StartSlave(flags);
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
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      "ls -al /");

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("library/alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();

  Shutdown();
}
#endif

} // namespace tests {
} // namespace internal {
} // namespace mesos {
