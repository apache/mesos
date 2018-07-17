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

#include <stout/os/realpath.hpp>

#include <stout/tests/utils.hpp>

#include <mesos/appc/spec.hpp>

#include "common/command_utils.hpp"

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/provisioner/constants.hpp"
#include "slave/containerizer/mesos/provisioner/paths.hpp"
#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

#include "slave/containerizer/mesos/provisioner/appc/fetcher.hpp"
#include "slave/containerizer/mesos/provisioner/appc/paths.hpp"
#include "slave/containerizer/mesos/provisioner/appc/store.hpp"

#include "tests/mesos.hpp"

#ifdef __linux__
#include "tests/containerizer/rootfs.hpp"
#endif

namespace http = process::http;
namespace paths = mesos::internal::slave::appc::paths;

using std::list;
using std::string;
using std::vector;

using process::Future;
using process::Owned;
using process::Process;

using testing::_;
using testing::Return;

using mesos::internal::slave::BIND_BACKEND;
using mesos::internal::slave::COPY_BACKEND;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Provisioner;
using mesos::internal::slave::appc::Store;

using mesos::master::detector::MasterDetector;

namespace mesos {
namespace internal {
namespace tests {

/**
 * Helper function that returns a Appc protobuf object for given
 * parameters.
 *
 * @param name Appc image name.
 * @param arch Machine architecture(e.g, x86, amd64).
 * @param os Operating system(e.g, linux).
 * @return Appc protobuf message object.
 */
static Image::Appc getAppcImage(
    const string& name,
    const string& arch = "amd64",
    const string& os = "linux")
{
  Image::Appc appc;
  appc.set_name(name);

  Label archLabel;
  archLabel.set_key("arch");
  archLabel.set_value(arch);

  Label osLabel;
  osLabel.set_key("os");
  osLabel.set_value(os);

  Labels labels;
  labels.add_labels()->CopyFrom(archLabel);
  labels.add_labels()->CopyFrom(osLabel);

  appc.mutable_labels()->CopyFrom(labels);

  return appc;
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

  Future<slave::ImageInfo> ImageInfo = store.get()->get(image, COPY_BACKEND);
  AWAIT_READY(ImageInfo);

  EXPECT_EQ(1u, ImageInfo->layers.size());
  ASSERT_SOME(os::realpath(imagePath));
  EXPECT_EQ(
      os::realpath(path::join(imagePath, "rootfs")).get(),
      ImageInfo->layers.front());
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
  flags.image_provisioner_backend = BIND_BACKEND;
  flags.work_dir = path::join(sandbox.get(), "work_dir");

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<string> createImage = createTestImage(
      flags.appc_store_dir,
      getManifest());

  ASSERT_SOME(createImage);

  // Recover. This is when the image in the store is loaded.
  AWAIT_READY(provisioner.get()->recover({}));

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
  ASSERT_TRUE(rootfses->contains(flags.image_provisioner_backend.get()));
  ASSERT_EQ(1u, rootfses->get(flags.image_provisioner_backend.get())->size());
  EXPECT_EQ(*rootfses->get(flags.image_provisioner_backend.get())->begin(),
            Path(provisionInfo->rootfs).basename());

  Future<bool> destroy = provisioner.get()->destroy(containerId);
  AWAIT_READY(destroy);

  // One rootfs is destroyed.
  EXPECT_TRUE(destroy.get());

  // The container directory is successfully cleaned up.
  EXPECT_FALSE(os::exists(containerDir));
}


// This test verifies that the provisioner can provision an rootfs
// from an image for a child container.
TEST_F(ProvisionerAppcTest, ROOT_ProvisionNestedContainer)
{
  slave::Flags flags;
  flags.image_providers = "APPC";
  flags.appc_store_dir = path::join(os::getcwd(), "store");
  flags.image_provisioner_backend = BIND_BACKEND;
  flags.work_dir = path::join(sandbox.get(), "work_dir");

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<string> createImage = createTestImage(
      flags.appc_store_dir,
      getManifest());

  ASSERT_SOME(createImage);

  // Recover. This is when the image in the store is loaded.
  AWAIT_READY(provisioner.get()->recover({}));

  Image image;
  image.mutable_appc()->CopyFrom(getTestImage());

  ContainerID parent;
  ContainerID child;

  parent.set_value(id::UUID::random().toString());
  child.set_value(id::UUID::random().toString());
  child.mutable_parent()->CopyFrom(parent);

  Future<slave::ProvisionInfo> provisionInfo =
    provisioner.get()->provision(child, image);

  AWAIT_READY(provisionInfo);

  const string provisionerDir = slave::paths::getProvisionerDir(flags.work_dir);

  const string containerDir =
    slave::provisioner::paths::getContainerDir(
        provisionerDir,
        child);

  Try<hashmap<string, hashset<string>>> rootfses =
    slave::provisioner::paths::listContainerRootfses(
        provisionerDir,
        child);

  ASSERT_SOME(rootfses);

  // Verify that the rootfs is successfully provisioned.
  ASSERT_TRUE(rootfses->contains(flags.image_provisioner_backend.get()));
  ASSERT_EQ(1u, rootfses->get(flags.image_provisioner_backend.get())->size());
  EXPECT_EQ(*rootfses->get(flags.image_provisioner_backend.get())->begin(),
            Path(provisionInfo->rootfs).basename());

  // TODO(jieyu): Verify that 'containerDir' is nested under its
  // parent container's 'containerDir'.

  Future<bool> destroy = provisioner.get()->destroy(child);
  AWAIT_READY(destroy);
  EXPECT_TRUE(destroy.get());
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
  flags.image_provisioner_backend = COPY_BACKEND;
  flags.work_dir = path::join(sandbox.get(), "work_dir");

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<string> createImage = createTestImage(
      flags.appc_store_dir,
      getManifest());

  ASSERT_SOME(createImage);

  // Recover. This is when the image in the store is loaded.
  AWAIT_READY(provisioner.get()->recover({}));

  Image image;
  image.mutable_appc()->CopyFrom(getTestImage());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Future<slave::ProvisionInfo> provisionInfo =
    provisioner.get()->provision(containerId, image);
  AWAIT_READY(provisionInfo);

  provisioner->reset();

  // Create a new provisioner to recover the state from the container.
  provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  AWAIT_READY(provisioner.get()->recover({containerId}));

  // It's possible for the user to provision two different rootfses
  // from the same image.
  AWAIT_READY(provisioner.get()->provision(containerId, image));

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
  ASSERT_TRUE(rootfses->contains(flags.image_provisioner_backend.get()));
  EXPECT_EQ(2u, rootfses->get(flags.image_provisioner_backend.get())->size());

  Future<bool> destroy = provisioner.get()->destroy(containerId);
  AWAIT_READY(destroy);
  EXPECT_TRUE(destroy.get());

  // The container directory is successfully cleaned up.
  EXPECT_FALSE(os::exists(containerDir));
}


// This test verifies that the provisioner can recover the rootfses
// for both parent and child containers.
TEST_F(ProvisionerAppcTest, RecoverNestedContainer)
{
  slave::Flags flags;
  flags.image_providers = "APPC";
  flags.appc_store_dir = path::join(os::getcwd(), "store");
  flags.image_provisioner_backend = COPY_BACKEND;
  flags.work_dir = path::join(sandbox.get(), "work_dir");

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<string> createImage = createTestImage(
      flags.appc_store_dir,
      getManifest());

  ASSERT_SOME(createImage);

  // Recover. This is when the image in the store is loaded.
  AWAIT_READY(provisioner.get()->recover({}));

  Image image;
  image.mutable_appc()->CopyFrom(getTestImage());

  ContainerID parent;
  ContainerID child;

  parent.set_value(id::UUID::random().toString());
  child.set_value(id::UUID::random().toString());
  child.mutable_parent()->CopyFrom(parent);

  AWAIT_READY(provisioner.get()->provision(parent, image));
  AWAIT_READY(provisioner.get()->provision(child, image));

  provisioner->reset();

  // Create a new provisioner to recover the state from the container.
  provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  AWAIT_READY(provisioner.get()->recover({parent, child}));
  AWAIT_READY(provisioner.get()->provision(child, image));

  const string provisionerDir = slave::paths::getProvisionerDir(flags.work_dir);

  string containerDir =
    slave::provisioner::paths::getContainerDir(
        provisionerDir,
        child);

  Future<bool> destroy = provisioner.get()->destroy(child);
  AWAIT_READY(destroy);
  EXPECT_TRUE(destroy.get());
  EXPECT_FALSE(os::exists(containerDir));

  containerDir =
    slave::provisioner::paths::getContainerDir(
        provisionerDir,
        parent);

  destroy = provisioner.get()->destroy(parent);
  AWAIT_READY(destroy);
  EXPECT_TRUE(destroy.get());
  EXPECT_FALSE(os::exists(containerDir));
}


// This test verifies that the provisioner can recover the rootfses
// for the child containers if there is no image specified for its
// parent container.
TEST_F(ProvisionerAppcTest, RecoverNestedContainerNoParentImage)
{
  slave::Flags flags;
  flags.image_providers = "APPC";
  flags.appc_store_dir = path::join(os::getcwd(), "store");
  flags.image_provisioner_backend = COPY_BACKEND;
  flags.work_dir = path::join(sandbox.get(), "work_dir");

  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  Try<string> createImage = createTestImage(
      flags.appc_store_dir,
      getManifest());

  ASSERT_SOME(createImage);

  // Recover. This is when the image in the store is loaded.
  AWAIT_READY(provisioner.get()->recover({}));

  Image image;
  image.mutable_appc()->CopyFrom(getTestImage());

  ContainerID parent;
  ContainerID child;

  parent.set_value(id::UUID::random().toString());
  child.set_value(id::UUID::random().toString());
  child.mutable_parent()->CopyFrom(parent);

  AWAIT_READY(provisioner.get()->provision(child, image));

  provisioner->reset();

  // Create a new provisioner to recover the state from the container.
  provisioner = Provisioner::create(flags);
  ASSERT_SOME(provisioner);

  AWAIT_READY(provisioner.get()->recover({parent, child}));
  AWAIT_READY(provisioner.get()->provision(child, image));

  const string provisionerDir = slave::paths::getProvisionerDir(flags.work_dir);

  string containerDir =
    slave::provisioner::paths::getContainerDir(
        provisionerDir,
        child);

  Future<bool> destroy = provisioner.get()->destroy(child);
  AWAIT_READY(destroy);
  EXPECT_TRUE(destroy.get());
  EXPECT_FALSE(os::exists(containerDir));

  containerDir =
    slave::provisioner::paths::getContainerDir(
        provisionerDir,
        parent);

  destroy = provisioner.get()->destroy(parent);
  AWAIT_READY(destroy);
  EXPECT_TRUE(destroy.get());
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
  // TODO(jojy): Currently hard-codes the images directory name.
  // Consider parameterizing the directory name. This could be done
  // by removing the 'const' ness of the variable and adding mutator.
  const string imagesDirName;
};


// Test fixture that uses the mock HTTP image server. This fixture provides the
// abstraction for creating and composing complex test cases for Appc image
// fetcher and store.
class AppcImageFetcherTest : public AppcStoreTest
{
protected:
  // Custom implementation that overrides the host and port of the image name.
  JSON::Value getManifest() const override
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

  void TearDown() override
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
      stringify(server.self().address),
      imageName).get();

  Image::Appc appc = getAppcImage(discoverableImageName);

  // Create image fetcher.
  Try<Owned<uri::Fetcher>> uriFetcher = uri::fetcher::create();
  ASSERT_SOME(uriFetcher);

  slave::Flags flags;

  Try<Owned<slave::appc::Fetcher>> fetcher =
    slave::appc::Fetcher::create(flags, uriFetcher->share());

  ASSERT_SOME(fetcher);

  // Prepare fetch directory.
  const Path imageFetchDir(path::join(os::getcwd(), "fetched-images"));

  Try<Nothing> mkdir = os::mkdir(imageFetchDir);
  ASSERT_SOME(mkdir);

  // Now fetch the image.
  AWAIT_READY(fetcher.get()->fetch(appc, imageFetchDir));

  // Verify that there is an image directory.
  Try<list<string>> imageDirs = os::ls(imageFetchDir);
  ASSERT_SOME(imageDirs);

  // Verify that there is only ONE image directory.
  ASSERT_EQ(1u, imageDirs->size());

  // Verify that there is a roofs.
  const Path imageRootfs(path::join(
      imageFetchDir,
      imageDirs->front(),
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

  Image::Appc appc = getAppcImage("image");

  // Create image fetcher.
  Try<Owned<uri::Fetcher>> uriFetcher = uri::fetcher::create();
  ASSERT_SOME(uriFetcher);

  slave::Flags flags;

  // Set file path prefix for simple image discovery.
  flags.appc_simple_discovery_uri_prefix = imageDirMountPath + "/";

  Try<Owned<slave::appc::Fetcher>> fetcher =
    slave::appc::Fetcher::create(flags, uriFetcher->share());

  ASSERT_SOME(fetcher);

  // Prepare fetch directory.
  const Path imageFetchDir(path::join(os::getcwd(), "fetched-images"));

  Try<Nothing> mkdir = os::mkdir(imageFetchDir);
  ASSERT_SOME(mkdir);

  // Now fetch the image.
  AWAIT_READY(fetcher.get()->fetch(appc, imageFetchDir));

  // Verify that there is an image directory.
  Try<list<string>> imageDirs = os::ls(imageFetchDir);
  ASSERT_SOME(imageDirs);

  // Verify that there is only ONE image directory.
  ASSERT_EQ(1u, imageDirs->size());

  // Verify that there is a roofs.
  const Path imageRootfs(path::join(
      imageFetchDir,
      imageDirs->front(),
      "rootfs"));

  ASSERT_TRUE(os::exists(imageRootfs));

  // Verify that the image fetched is the same as on the server.
  ASSERT_SOME_EQ("test", os::read(path::join(imageRootfs, "tmp", "test")));
}


#ifdef __linux__
// Test fixture for Appc image provisioner integration tests. It also provides a
// helper for creating a base linux image bundle.
class AppcProvisionerIntegrationTest : public MesosTest
{
protected:
  // Prepares a base 'linux' Appc image bundle from host's rootfs.
  void prepareImageBundle(const string& imageName, const string& mntDir)
  {
    // TODO(jojy): Consider parameterizing the labels for image name decoration.
    const string imageBundleName = imageName + "-latest-linux-amd64.aci";

    const string imagesPath = path::join(mntDir, "images");
    const string imagePath = path::join(imagesPath, imageBundleName);

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

    const string rootfsPath = path::join(imagePath, "rootfs");

    // Setup image on the server.
    Try<Owned<Rootfs>> rootfs = LinuxRootfs::create(rootfsPath);
    ASSERT_SOME(rootfs);

    Try<Nothing> manifestWrite = os::write(
        path::join(imagePath, "manifest"),
        manifest);

    ASSERT_SOME(manifestWrite);

    const string imageBundlePath = path::join(mntDir, imageBundleName);

    Future<Nothing> future = command::tar(
        Path("."),
        Path(imageBundlePath),
        imagePath,
        command::Compression::GZIP);

    AWAIT_READY_FOR(future, Seconds(120));
  }
};


// Tests the Appc image provisioner for a single layered Linux image. The
// image's rootfs is built from local host's file system.
TEST_F(AppcProvisionerIntegrationTest, ROOT_SimpleLinuxImageTest)
{
  const string imageName = "linux";

  // Represents the directory where image bundles would be mounted.
  const string mntDir = path::join(os::getcwd(), "mnt");

  // Prepare the image bundle at the mount point.
  prepareImageBundle(imageName, mntDir);

  // Start master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start agent.
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";
  flags.image_providers = "APPC";
  flags.appc_simple_discovery_uri_prefix = path::join(mntDir, "");

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

  // Setup image for task.
  Image::Appc appc = getAppcImage(imageName);

  Image image;
  image.set_type(Image::APPC);
  image.mutable_appc()->CopyFrom(appc);

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

  AWAIT_READY_FOR(statusStarting, Seconds(120));
  EXPECT_EQ(task.task_id(), statusStarting->task_id());
  EXPECT_EQ(TASK_STARTING, statusStarting->state());

  AWAIT_READY_FOR(statusRunning, Seconds(120));
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}
#endif

// TODO(jojy): Add integration test for image with dependencies.

} // namespace tests {
} // namespace internal {
} // namespace mesos {
