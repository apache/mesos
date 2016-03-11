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

#include <gmock/gmock.h>

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

#include "common/command_utils.hpp"

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/provisioner/paths.hpp"
#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

#include "slave/containerizer/mesos/provisioner/appc/fetcher.hpp"
#include "slave/containerizer/mesos/provisioner/appc/paths.hpp"
#include "slave/containerizer/mesos/provisioner/appc/store.hpp"

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::Return;

using namespace process;

using namespace mesos::internal::slave::appc;

namespace spec = appc::spec;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Provisioner;

namespace mesos {
namespace internal {
namespace tests {

// TODO(jojy): Move AppcSpecTest to its own test file.
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


class AppcStoreTest : public TemporaryDirectoryTest
{
protected:
  // Reusable method to create a simple test image directory inside the store
  // directory. The directory structure reflects the Appc image spec.
  //
  // @param storeDir Path to the store directory where all images are stored.
  // @param manifest Manifest JSON to be used for the test image.
  // @return  Path to the test image directory.
  Try<string> createTestImage(
      const string& storeDir,
      const JSON::Value& manifest)
  {
    Try<Nothing> mkdir = os::mkdir(storeDir, true);
    if (mkdir.isError()) {
      return mkdir.error();
    }

    // Create a simple image in the store:
    // <store>
    // |--images
    //    |--<id>
    //       |--manifest
    //       |--rootfs/tmp/test

    // The 'imageId' below has the correct format but it's not computed
    // by hashing the tarball of the image. It's OK here as we assume
    // the images under 'images' have passed such check when they are
    // downloaded and validated.
    const string imageId =
      "sha512-e77d96aa0240eedf134b8c90baeaf76dca8e78691836301d7498c84020446042e"
      "797b296d6ab296e0954c2626bfb264322ebeb8f447dac4fac6511ea06bc61f0";

    // Create rootfs inside the store directory.
    const string rootfsPath = paths::getImageRootfsPath(storeDir, imageId);

    Try<Nothing> mkRootfsDir = os::mkdir(rootfsPath);
    if (mkRootfsDir.isError()) {
      return mkRootfsDir.error();
    }

    // Create tmp directory inside rootfs.
    const string tmpDirPath = path::join(rootfsPath, "tmp");

    Try<Nothing> mkTmpDir = os::mkdir(tmpDirPath);
    if (mkTmpDir.isError()) {
      return mkTmpDir.error();
    }

    // Create test file in the tmp directory.
    Try<Nothing> testFileWrite =
      os::write(path::join(tmpDirPath, "test"), "test");

    if (testFileWrite.isError()) {
      return testFileWrite.error();
    }

    // Create manifest in the image directory.
    Try<Nothing> manifestWrite = os::write(
        paths::getImageManifestPath(storeDir, imageId),
        stringify(manifest));

    if (manifestWrite.isError()) {
      return manifestWrite.error();
    }

    return paths::getImagePath(storeDir, imageId);
  }

  Image::Appc getTestImage() const
  {
    Image::Appc appc;
    appc.set_name("foo.com/bar");

    Label version;
    version.set_key("version");
    version.set_value("1.0.0");

    Label arch;
    arch.set_key("arch");
    arch.set_value("amd64");

    Label os;
    os.set_key("os");
    os.set_value("linux");

    Labels labels;
    labels.add_labels()->CopyFrom(version);
    labels.add_labels()->CopyFrom(arch);
    labels.add_labels()->CopyFrom(os);

    appc.mutable_labels()->CopyFrom(labels);

    return appc;
  }

  // Abstracts the manifest accessor for the test fixture. This provides the
  // ability for customizing manifests for fixtures.
  virtual JSON::Value getManifest() const
  {
    return JSON::parse(
        R"~(
        {
          "acKind": "ImageManifest",
          "acVersion": "0.6.1",
          "name": "foo.com/bar",
          "labels": [
            {
              "name": "version",
              "value": "1.0.0"
            },
            {
              "name": "arch",
              "value": "amd64"
            },
            {
              "name": "os",
              "value": "linux"
            }
          ],
          "annotations": [
            {
              "name": "created",
              "value": "1438983392"
            }
          ]
        })~").get();
  }
};


TEST_F(AppcStoreTest, Recover)
{
  // Create store.
  slave::Flags flags;
  flags.appc_store_dir = path::join(os::getcwd(), "store");
  Try<Owned<slave::Store>> store = Store::create(flags);
  ASSERT_SOME(store);

  Try<string> createImage = createTestImage(
      flags.appc_store_dir,
      getManifest());

  ASSERT_SOME(createImage);

  const string imagePath = createImage.get();

  // Recover the image from disk.
  AWAIT_READY(store.get()->recover());

  Image image;
  image.mutable_appc()->CopyFrom(getTestImage());

  Future<slave::ImageInfo> ImageInfo = store.get()->get(image);
  AWAIT_READY(ImageInfo);

  EXPECT_EQ(1u, ImageInfo.get().layers.size());
  ASSERT_SOME(os::realpath(imagePath));
  EXPECT_EQ(
      os::realpath(path::join(imagePath, "rootfs")).get(),
      ImageInfo.get().layers.front());
}


class ProvisionerAppcTest : public AppcStoreTest {};


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

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<string> createImage = createTestImage(
      flags.appc_store_dir,
      getManifest());

  ASSERT_SOME(createImage);

  // Recover. This is when the image in the store is loaded.
  AWAIT_READY(provisioner.get()->recover({}, {}));

  // Simulate a task that requires an image.
  Image image;
  image.mutable_appc()->CopyFrom(getTestImage());

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

  Try<Owned<Provisioner>> provisioner1 = Provisioner::create(flags);
  ASSERT_SOME(provisioner1);

  Try<string> createImage = createTestImage(
      flags.appc_store_dir,
      getManifest());

  ASSERT_SOME(createImage);

  // Recover. This is when the image in the store is loaded.
  AWAIT_READY(provisioner1.get()->recover({}, {}));

  Image image;
  image.mutable_appc()->CopyFrom(getTestImage());

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


// Mock HTTP image server.
class TestAppcImageServer : public Process<TestAppcImageServer>
{
public:
  TestAppcImageServer()
    : ProcessBase("TestAppcImageServer"),
      imagesDirName("server") {}

  void load()
  {
    const string imagesDir = path::join(os::getcwd(), imagesDirName);

    Try<list<string>> imageBundles = os::ls(imagesDir);
    ASSERT_SOME(imageBundles);

    foreach (const string& imageName, imageBundles.get()) {
      route("/" + imageName, None(), &TestAppcImageServer::serveRequest);
    }
  }

  Future<http::Response> serveRequest(const http::Request& request)
  {
    const string imageBundleName = request.url.path;

    const string imageBundlePath = path::join(
        os::getcwd(),
        imagesDirName,
        Path(imageBundleName).basename());

    http::OK response;

    response.type = response.PATH;
    response.path = imageBundlePath;
    response.headers["Content-Type"] = "application/octet-stream";
    response.headers["Content-Disposition"] = strings::format(
        "attachment; filename=%s",
        imageBundlePath).get();

    return response;
  }

private:
  // TODO(jojy): Currently hard-codes the images dierctory name.
  // Consider parameterizing the directory name. This could be done
  // by removing the 'const' ness of teh variable and adding mutator.
  const string imagesDirName;
};


// Test fixture that uses the mock HTTP image server. This fixture provides the
// abstraction for creating and composing complex test cases for Appc image
// fetcher and store.
class AppcImageFetcherTest : public AppcStoreTest
{
protected:
  // Custom implementation that overrides the host and port of the image name.
  JSON::Value getManifest() const
  {
    string imageName = strings::format(
        "%s:%d/TestAppcImageServer/image",
        stringify(server.self().address.ip),
        server.self().address.port).get();

    const string manifest = strings::format(
        R"~(
        {
          "acKind": "ImageManifest",
            "acVersion": "0.6.1",
            "name": "%s",
            "labels": [
            {
              "name": "version",
              "value": "latest"
            },
            {
              "name": "arch",
              "value": "amd64"
            },
            {
              "name": "os",
              "value": "linux"
            }
          ],
            "annotations": [
            {
              "name": "created",
              "value": "1438983392"
            }
          ]
        })~",
        imageName).get();

    return JSON::parse(manifest).get();
  }

  void prepareImage(
      const string& directory,
      const string& imageBundlePath,
      const JSON::Value& manifest)
  {
    // Place the image in dir '@imageTopDir/images'.
    Try<string> createImage = createTestImage(directory, getManifest());
    ASSERT_SOME(createImage);

    // Test image is created in a directory with this name.
    const string imageId =
      "sha512-e77d96aa0240eedf134b8c90baeaf76dca8e78691836301d7498c8402044604"
      "2e797b296d6ab296e0954c2626bfb264322ebeb8f447dac4fac6511ea06bc61f0";

    const Path imageDir(path::join(directory, "images", imageId));

    Future<Nothing> future = command::tar(
        Path("."),
        Path(imageBundlePath),
        imageDir,
        command::Compression::GZIP);

    AWAIT_READY(future);
  }

  void startServer()
  {
    spawn(server);
  }

  virtual void TearDown()
  {
    terminate(server);
    wait(server);

    TemporaryDirectoryTest::TearDown();
  }

  TestAppcImageServer server;
};


// Tests simple http fetch functionality of the appc::Fetcher component.
// The test fetches a test Appc image from the http server and
// verifies its content. The image is served in 'tar + gzip' format.
TEST_F(AppcImageFetcherTest, CURL_SimpleHttpFetch)
{
  const string imageName = "image";

  const string imageBundleName = imageName + "-latest-linux-amd64.aci";

  // Use the default server directory of the image server.
  const Path serverDir(path::join(os::getcwd(), "server"));
  const string imageBundlePath = path::join(serverDir, imageBundleName);

  // Setup the image.
  prepareImage(serverDir, imageBundlePath, getManifest());

  // Setup http server to serve the image prepared above.
  server.load();

  startServer();

  // Appc Image to be fetched.
  const string discoverableImageName = strings::format(
      "%s/TestAppcImageServer/%s",
      stringify(server.self().address) ,
      imageName).get();

  Image::Appc imageInfo;
  imageInfo.set_name(discoverableImageName);

  Label archLabel;
  archLabel.set_key("arch");
  archLabel.set_value("amd64");

  Label osLabel;
  osLabel.set_key("os");
  osLabel.set_value("linux");

  Labels labels;
  labels.add_labels()->CopyFrom(archLabel);
  labels.add_labels()->CopyFrom(osLabel);

  imageInfo.mutable_labels()->CopyFrom(labels);

  // Create image fetcher.
  Try<Owned<uri::Fetcher>> uriFetcher = uri::fetcher::create();
  ASSERT_SOME(uriFetcher);

  slave::Flags flags;

  Try<Owned<slave::appc::Fetcher>> fetcher =
    slave::appc::Fetcher::create(flags, uriFetcher.get().share());

  ASSERT_SOME(fetcher);

  // Prepare fetch directory.
  const Path imageFetchDir(path::join(os::getcwd(), "fetched-images"));

  Try<Nothing> mkdir = os::mkdir(imageFetchDir);
  ASSERT_SOME(mkdir);

  // Now fetch the image.
  AWAIT_READY(fetcher.get()->fetch(imageInfo, imageFetchDir));

  // Verify that there is an image directory.
  Try<list<string>> imageDirs = os::ls(imageFetchDir);
  ASSERT_SOME(imageDirs);

  // Verify that there is only ONE image directory.
  ASSERT_EQ(1u, imageDirs.get().size());

  // Verify that there is a roofs.
  const Path imageRootfs(path::join(
      imageFetchDir,
      imageDirs.get().front(),
      "rootfs"));

  ASSERT_TRUE(os::exists(imageRootfs));

  // Verify that the image fetched is the same as on the server.
  ASSERT_SOME_EQ("test", os::read(path::join(imageRootfs, "tmp", "test")));
}


// Tests simple file fetch functionality of the appc::Fetcher component.
TEST_F(AppcImageFetcherTest, SimpleFileFetch)
{
  const string imageName = "image";

  const string imageBundleName = imageName + "-latest-linux-amd64.aci";

  // This represents the directory where images volume could be mounted.
  const string imageDirMountPath(path::join(os::getcwd(), "mnt"));

  const string imageBundlePath = path::join(imageDirMountPath, imageBundleName);

  // Setup the image bundle.
  prepareImage(imageDirMountPath, imageBundlePath, getManifest());

  Image::Appc imageInfo;
  imageInfo.set_name("image");

  Label archLabel;
  archLabel.set_key("arch");
  archLabel.set_value("amd64");

  Label osLabel;
  osLabel.set_key("os");
  osLabel.set_value("linux");

  Labels labels;
  labels.add_labels()->CopyFrom(archLabel);
  labels.add_labels()->CopyFrom(osLabel);

  imageInfo.mutable_labels()->CopyFrom(labels);

  // Create image fetcher.
  Try<Owned<uri::Fetcher>> uriFetcher = uri::fetcher::create();
  ASSERT_SOME(uriFetcher);

  slave::Flags flags;

  // Set file path prefix for simple image discovery.
  flags.appc_simple_discovery_uri_prefix = imageDirMountPath + "/";

  Try<Owned<slave::appc::Fetcher>> fetcher =
    slave::appc::Fetcher::create(flags, uriFetcher.get().share());

  ASSERT_SOME(fetcher);

  // Prepare fetch directory.
  const Path imageFetchDir(path::join(os::getcwd(), "fetched-images"));

  Try<Nothing> mkdir = os::mkdir(imageFetchDir);
  ASSERT_SOME(mkdir);

  // Now fetch the image.
  AWAIT_READY(fetcher.get()->fetch(imageInfo, imageFetchDir));

  // Verify that there is an image directory.
  Try<list<string>> imageDirs = os::ls(imageFetchDir);
  ASSERT_SOME(imageDirs);

  // Verify that there is only ONE image directory.
  ASSERT_EQ(1u, imageDirs.get().size());

  // Verify that there is a roofs.
  const Path imageRootfs(path::join(
      imageFetchDir,
      imageDirs.get().front(),
      "rootfs"));

  ASSERT_TRUE(os::exists(imageRootfs));

  // Verify that the image fetched is the same as on the server.
  ASSERT_SOME_EQ("test", os::read(path::join(imageRootfs, "tmp", "test")));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
