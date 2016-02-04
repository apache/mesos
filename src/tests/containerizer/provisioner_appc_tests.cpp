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

#include <mesos/slave/isolator.hpp>

#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include <stout/tests/utils.hpp>

#include <mesos/appc/spec.hpp>

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/provisioner/paths.hpp"
#include "slave/containerizer/mesos/provisioner/provisioner.hpp"
#include "slave/containerizer/mesos/provisioner/appc/store.hpp"

using std::list;
using std::string;
using std::vector;

using namespace process;

using namespace mesos::internal::slave::appc;

namespace spec = appc::spec;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Provisioner;

namespace mesos {
namespace internal {
namespace tests {

class AppcSpecTest : public TemporaryDirectoryTest {};


TEST_F(AppcSpecTest, ValidateImageManifest)
{
  JSON::Value manifest = JSON::parse(
      "{"
      "  \"acKind\": \"ImageManifest\","
      "  \"acVersion\": \"0.6.1\","
      "  \"name\": \"foo.com/bar\","
      "  \"labels\": ["
      "    {"
      "      \"name\": \"version\","
      "      \"value\": \"1.0.0\""
      "    },"
      "    {"
      "      \"name\": \"arch\","
      "      \"value\": \"amd64\""
      "    },"
      "    {"
      "      \"name\": \"os\","
      "      \"value\": \"linux\""
      "    }"
      "  ],"
      "  \"annotations\": ["
      "    {"
      "      \"name\": \"created\","
      "      \"value\": \"1438983392\""
      "    }"
      "  ]"
      "}").get();

  EXPECT_SOME(spec::parse(stringify(manifest)));

  // Incorrect acKind for image manifest.
  manifest = JSON::parse(
      "{"
      "  \"acKind\": \"PodManifest\","
      "  \"acVersion\": \"0.6.1\","
      "  \"name\": \"foo.com/bar\""
      "}").get();

  EXPECT_ERROR(spec::parse(stringify(manifest)));
}


TEST_F(AppcSpecTest, ValidateLayout)
{
  string image = os::getcwd();

  JSON::Value manifest = JSON::parse(
      "{"
      "  \"acKind\": \"ImageManifest\","
      "  \"acVersion\": \"0.6.1\","
      "  \"name\": \"foo.com/bar\""
      "}").get();

  ASSERT_SOME(os::write(path::join(image, "manifest"), stringify(manifest)));

  // Missing rootfs.
  EXPECT_SOME(spec::validateLayout(image));

  ASSERT_SOME(os::mkdir(path::join(image, "rootfs", "tmp")));
  ASSERT_SOME(os::write(path::join(image, "rootfs", "tmp", "test"), "test"));

  EXPECT_NONE(spec::validateLayout(image));
}


class AppcStoreTest : public TemporaryDirectoryTest {};


TEST_F(AppcStoreTest, Recover)
{
  // Create store.
  slave::Flags flags;
  flags.appc_store_dir = path::join(os::getcwd(), "store");
  Try<Owned<slave::Store>> store = Store::create(flags);
  ASSERT_SOME(store);

  // Create a simple image in the store:
  // <store>
  // |--images
  //    |--<id>
  //       |--manifest
  //       |--rootfs/tmp/test
  JSON::Value manifest = JSON::parse(
      "{"
      "  \"acKind\": \"ImageManifest\","
      "  \"acVersion\": \"0.6.1\","
      "  \"name\": \"foo.com/bar\","
      "  \"labels\": ["
      "    {"
      "      \"name\": \"version\","
      "      \"value\": \"1.0.0\""
      "    },"
      "    {"
      "      \"name\": \"arch\","
      "      \"value\": \"amd64\""
      "    },"
      "    {"
      "      \"name\": \"os\","
      "      \"value\": \"linux\""
      "    }"
      "  ],"
      "  \"annotations\": ["
      "    {"
      "      \"name\": \"created\","
      "      \"value\": \"1438983392\""
      "    }"
      "  ]"
      "}").get();

  // The 'imageId' below has the correct format but it's not computed
  // by hashing the tarball of the image. It's OK here as we assume
  // the images under 'images' have passed such check when they are
  // downloaded and validated.
  string imageId =
    "sha512-e77d96aa0240eedf134b8c90baeaf76dca8e78691836301d7498c84020446042e"
    "797b296d6ab296e0954c2626bfb264322ebeb8f447dac4fac6511ea06bc61f0";

  string imagePath = path::join(flags.appc_store_dir, "images", imageId);

  ASSERT_SOME(os::mkdir(path::join(imagePath, "rootfs", "tmp")));
  ASSERT_SOME(
      os::write(path::join(imagePath, "rootfs", "tmp", "test"), "test"));
  ASSERT_SOME(
      os::write(path::join(imagePath, "manifest"), stringify(manifest)));

  // Recover the image from disk.
  AWAIT_READY(store.get()->recover());

  Image image;
  image.mutable_appc()->set_name("foo.com/bar");
  Future<slave::ImageInfo> ImageInfo = store.get()->get(image);
  AWAIT_READY(ImageInfo);

  EXPECT_EQ(1u, ImageInfo.get().layers.size());
  ASSERT_SOME(os::realpath(imagePath));
  EXPECT_EQ(
      os::realpath(path::join(imagePath, "rootfs")).get(),
      ImageInfo.get().layers.front());
}


class ProvisionerAppcTest : public TemporaryDirectoryTest {};


#ifdef __linux__
// This test verifies that the provisioner can provision an rootfs
// from an image that is already put into the store directory.
TEST_F(ProvisionerAppcTest, ROOT_Provision)
{
  // Create provisioner.
  slave::Flags flags;
  flags.image_providers = "APPC";
  flags.appc_store_dir = path::join(os::getcwd(), "store");
  flags.image_provisioner_backend = "bind";
  flags.work_dir = "work_dir";

  Fetcher fetcher;

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  // Create a simple image in the store:
  // <store>
  // |--images
  //    |--<id>
  //       |--manifest
  //       |--rootfs/tmp/test
  JSON::Value manifest = JSON::parse(
      "{"
      "  \"acKind\": \"ImageManifest\","
      "  \"acVersion\": \"0.6.1\","
      "  \"name\": \"foo.com/bar\","
      "  \"labels\": ["
      "    {"
      "      \"name\": \"version\","
      "      \"value\": \"1.0.0\""
      "    },"
      "    {"
      "      \"name\": \"arch\","
      "      \"value\": \"amd64\""
      "    },"
      "    {"
      "      \"name\": \"os\","
      "      \"value\": \"linux\""
      "    }"
      "  ],"
      "  \"annotations\": ["
      "    {"
      "      \"name\": \"created\","
      "      \"value\": \"1438983392\""
      "    }"
      "  ]"
      "}").get();

  // The 'imageId' below has the correct format but it's not computed
  // by hashing the tarball of the image. It's OK here as we assume
  // the images under 'images' have passed such check when they are
  // downloaded and validated.
  string imageId =
    "sha512-e77d96aa0240eedf134b8c90baeaf76dca8e78691836301d7498c84020446042e"
    "797b296d6ab296e0954c2626bfb264322ebeb8f447dac4fac6511ea06bc61f0";

  string imagePath = path::join(flags.appc_store_dir, "images", imageId);

  ASSERT_SOME(os::mkdir(path::join(imagePath, "rootfs", "tmp")));
  ASSERT_SOME(
      os::write(path::join(imagePath, "rootfs", "tmp", "test"), "test"));
  ASSERT_SOME(
      os::write(path::join(imagePath, "manifest"), stringify(manifest)));

  // Recover. This is when the image in the store is loaded.
  AWAIT_READY(provisioner.get()->recover({}, {}));

  // Simulate a task that requires an image.
  Image image;
  image.mutable_appc()->set_name("foo.com/bar");

  ContainerID containerId;
  containerId.set_value("12345");

  Future<slave::ProvisionInfo> provisionInfo =
    provisioner.get()->provision(containerId, image);
  AWAIT_READY(provisionInfo);

  string provisionerDir = slave::paths::getProvisionerDir(flags.work_dir);

  string containerDir =
    slave::provisioner::paths::getContainerDir(
        provisionerDir,
        containerId);

  Try<hashmap<string, hashset<string>>> rootfses =
    slave::provisioner::paths::listContainerRootfses(
        provisionerDir,
        containerId);

  ASSERT_SOME(rootfses);

  // Verify that the rootfs is successfully provisioned.
  ASSERT_TRUE(rootfses->contains(flags.image_provisioner_backend));
  ASSERT_EQ(1u, rootfses->get(flags.image_provisioner_backend)->size());
  EXPECT_EQ(*rootfses->get(flags.image_provisioner_backend)->begin(),
            Path(provisionInfo.get().rootfs).basename());

  Future<bool> destroy = provisioner.get()->destroy(containerId);
  AWAIT_READY(destroy);

  // One rootfs is destroyed.
  EXPECT_TRUE(destroy.get());

  // The container directory is successfully cleaned up.
  EXPECT_FALSE(os::exists(containerDir));
}
#endif // __linux__


// This test verifies that a provisioner can recover the rootfs
// provisioned by a previous provisioner and then destroy it. Note
// that we use the copy backend in this test so Linux is not required.
TEST_F(ProvisionerAppcTest, Recover)
{
  // Create provisioner.
  slave::Flags flags;
  flags.image_providers = "APPC";
  flags.appc_store_dir = path::join(os::getcwd(), "store");
  flags.image_provisioner_backend = "copy";
  flags.work_dir = "work_dir";

  Fetcher fetcher;
  Try<Owned<Provisioner>> provisioner1 = Provisioner::create(flags);
  ASSERT_SOME(provisioner1);

  // Create a simple image in the store:
  // <store>
  // |--images
  //    |--<id>
  //       |--manifest
  //       |--rootfs/tmp/test
  JSON::Value manifest = JSON::parse(
      "{"
      "  \"acKind\": \"ImageManifest\","
      "  \"acVersion\": \"0.6.1\","
      "  \"name\": \"foo.com/bar\""
      "}").get();

  // The 'imageId' below has the correct format but it's not computed
  // by hashing the tarball of the image. It's OK here as we assume
  // the images under 'images' have passed such check when they are
  // downloaded and validated.
  string imageId =
    "sha512-e77d96aa0240eedf134b8c90baeaf76dca8e78691836301d7498c84020446042e"
    "797b296d6ab296e0954c2626bfb264322ebeb8f447dac4fac6511ea06bc61f0";

  string imagePath = path::join(flags.appc_store_dir, "images", imageId);

  ASSERT_SOME(os::mkdir(path::join(imagePath, "rootfs", "tmp")));
  ASSERT_SOME(
      os::write(path::join(imagePath, "rootfs", "tmp", "test"), "test"));
  ASSERT_SOME(
      os::write(path::join(imagePath, "manifest"), stringify(manifest)));

  // Recover. This is when the image in the store is loaded.
  AWAIT_READY(provisioner1.get()->recover({}, {}));

  Image image;
  image.mutable_appc()->set_name("foo.com/bar");

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  Future<slave::ProvisionInfo> provisionInfo =
    provisioner1.get()->provision(containerId, image);
  AWAIT_READY(provisionInfo);

  // Create a new provisioner to recover the state from the container.
  Try<Owned<Provisioner>> provisioner2 = Provisioner::create(flags);
  ASSERT_SOME(provisioner2);

  mesos::slave::ContainerState state;

  // Here we are using an ExecutorInfo in the ContainerState without a
  // ContainerInfo. This is the situation where the Image is specified
  // via --default_container_info so it's not part of the recovered
  // ExecutorInfo.
  state.mutable_container_id()->CopyFrom(containerId);

  AWAIT_READY(provisioner2.get()->recover({state}, {}));

  // It's possible for the user to provision two different rootfses
  // from the same image.
  AWAIT_READY(provisioner2.get()->provision(containerId, image));

  string provisionerDir = slave::paths::getProvisionerDir(flags.work_dir);

  string containerDir =
    slave::provisioner::paths::getContainerDir(
        provisionerDir,
        containerId);

  Try<hashmap<string, hashset<string>>> rootfses =
    slave::provisioner::paths::listContainerRootfses(
        provisionerDir,
        containerId);

  ASSERT_SOME(rootfses);

  // Verify that the rootfs is successfully provisioned.
  ASSERT_TRUE(rootfses->contains(flags.image_provisioner_backend));
  EXPECT_EQ(2u, rootfses->get(flags.image_provisioner_backend)->size());

  Future<bool> destroy = provisioner2.get()->destroy(containerId);
  AWAIT_READY(destroy);
  EXPECT_TRUE(destroy.get());

  // The container directory is successfully cleaned up.
  EXPECT_FALSE(os::exists(containerDir));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
