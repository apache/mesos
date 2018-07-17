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

#include <process/gtest.hpp>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/os/permissions.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/tests/utils.hpp>

#ifdef __linux__
#include "linux/fs.hpp"
#endif // __linux__

#include "slave/containerizer/mesos/provisioner/backends/bind.hpp"
#include "slave/containerizer/mesos/provisioner/backends/copy.hpp"
#include "slave/containerizer/mesos/provisioner/backends/overlay.hpp"

#include "slave/containerizer/mesos/provisioner/constants.hpp"

#include "tests/flags.hpp"

using namespace process;

using namespace mesos::internal::slave;

using mesos::internal::slave::AUFS_BACKEND;
using mesos::internal::slave::BIND_BACKEND;
using mesos::internal::slave::COPY_BACKEND;
using mesos::internal::slave::OVERLAY_BACKEND;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

#ifdef __linux__
class MountBackendTest : public TemporaryDirectoryTest
{
protected:
  void TearDown() override
  {
    Try<fs::MountInfoTable> mountTable = fs::MountInfoTable::read();
    ASSERT_SOME(mountTable);

    // TODO(xujyan): Make sandbox a plain string instead of an option.
    ASSERT_SOME(sandbox);
    foreach (const fs::MountInfoTable::Entry& entry, mountTable->entries) {
      if (strings::startsWith(entry.target, sandbox.get())) {
        fs::unmount(entry.target, MNT_DETACH);
      }
    }

    TemporaryDirectoryTest::TearDown();
  }
};


class OverlayBackendTest : public MountBackendTest {};


// Provision a rootfs using multiple layers with the overlay backend.
TEST_F(OverlayBackendTest, ROOT_OVERLAYFS_OverlayFSBackend)
{
  string layer1 = path::join(sandbox.get(), "source1");
  ASSERT_SOME(os::mkdir(layer1));
  ASSERT_SOME(os::mkdir(path::join(layer1, "dir1")));
  ASSERT_SOME(os::write(path::join(layer1, "dir1", "1"), "1"));
  ASSERT_SOME(os::write(path::join(layer1, "file"), "test1"));

  string layer2 = path::join(sandbox.get(), "source2");
  ASSERT_SOME(os::mkdir(layer2));
  ASSERT_SOME(os::mkdir(path::join(layer2, "dir2")));
  ASSERT_SOME(os::write(path::join(layer2, "dir2", "2"), "2"));
  ASSERT_SOME(os::write(path::join(layer2, "file"), "test2"));

  string rootfs = path::join(sandbox.get(), "rootfs");

  hashmap<string, Owned<Backend>> backends = Backend::create(slave::Flags());
  ASSERT_TRUE(backends.contains(OVERLAY_BACKEND));

  AWAIT_READY(backends[OVERLAY_BACKEND]->provision(
      {layer1, layer2},
      rootfs,
      sandbox.get()));

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir1", "1")));
  EXPECT_SOME_EQ("1", os::read(path::join(rootfs, "dir1", "1")));

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir2", "2")));
  EXPECT_SOME_EQ("2", os::read(path::join(rootfs, "dir2", "2")));

  // Last layer should overwrite existing file of earlier layers.
  EXPECT_TRUE(os::exists(path::join(rootfs, "file")));
  EXPECT_SOME_EQ("test2", os::read(path::join(rootfs, "file")));

  // Rootfs should be writable.
  ASSERT_SOME(os::write(path::join(rootfs, "file"), "test3"));

  // Files created in rootfs should shadow the files of lower dirs.
  EXPECT_SOME_EQ("test3", os::read(path::join(rootfs, "file")));
  EXPECT_SOME_EQ("test2", os::read(path::join(layer2, "file")));

  AWAIT_READY(backends[OVERLAY_BACKEND]->destroy(rootfs, sandbox.get()));

  EXPECT_FALSE(os::exists(rootfs));
}


// Test overlayfs backend for rootfs provisioning when an image has
// many layers. This test is used to verify the fix for MESOS-6000.
TEST_F(OverlayBackendTest, ROOT_OVERLAYFS_OverlayFSBackendWithManyLayers)
{
  // Create 64 image layers with more than 64 char length path to make
  // sure total length of mount option exceeds the 4096 support limit.
  const int imageCount = 64;
  vector<string> layers;

  for (int i = 0; i < imageCount; ++i) {
    const string layer = path::join(
        sandbox.get(),
        strings::format("lower_%.59d", i).get());

    const string dir = strings::format("dir%d", i).get();

    ASSERT_SOME(os::mkdir(layer));
    ASSERT_SOME(os::mkdir(path::join(layer, dir)));

    layers.push_back(layer);
  }

  string rootfs = path::join(sandbox.get(), "rootfs");

  hashmap<string, Owned<Backend>> backends = Backend::create(slave::Flags());
  ASSERT_TRUE(backends.contains(OVERLAY_BACKEND));

  AWAIT_READY(backends[OVERLAY_BACKEND]->provision(
      layers,
      rootfs,
      sandbox.get()));

  // Verify that all layers are available.
  for (int i = 0; i < imageCount; ++i) {
    EXPECT_TRUE(os::exists(path::join(
        rootfs,
        strings::format("dir%d", i).get())));
  }

  AWAIT_READY(backends[OVERLAY_BACKEND]->destroy(rootfs, sandbox.get()));
}


class BindBackendTest : public MountBackendTest {};


// Provision a rootfs using a BindBackend to another directory and
// verify if it is read-only within the mount.
TEST_F(BindBackendTest, ROOT_BindBackend)
{
  string rootfs = path::join(sandbox.get(), "source");

  // Create a writable directory under the dummy rootfs.
  Try<Nothing> mkdir = os::mkdir(path::join(rootfs, "tmp"));
  ASSERT_SOME(mkdir);

  hashmap<string, Owned<Backend>> backends = Backend::create(slave::Flags());
  ASSERT_TRUE(backends.contains(BIND_BACKEND));

  string target = path::join(sandbox.get(), "target");

  AWAIT_READY(backends[BIND_BACKEND]->provision(
      {rootfs},
      target,
      sandbox.get()));

  EXPECT_TRUE(os::stat::isdir(path::join(target, "tmp")));

  // 'target' _appears_ to be writable but is really not due to read-only mount.
  Try<mode_t> mode = os::stat::mode(path::join(target, "tmp"));
  ASSERT_SOME(mode);
  EXPECT_TRUE(os::Permissions(mode.get()).owner.w);
  EXPECT_ERROR(os::write(path::join(target, "tmp", "test"), "data"));

  AWAIT_READY(backends[BIND_BACKEND]->destroy(target, sandbox.get()));

  EXPECT_FALSE(os::exists(target));
}


class AufsBackendTest : public MountBackendTest {};


// Provision a rootfs using multiple layers with the aufs backend.
TEST_F(AufsBackendTest, ROOT_AUFS_AufsBackend)
{
  string layer1 = path::join(sandbox.get(), "source1");
  ASSERT_SOME(os::mkdir(layer1));
  ASSERT_SOME(os::mkdir(path::join(layer1, "dir1")));
  ASSERT_SOME(os::write(path::join(layer1, "dir1", "1"), "1"));
  ASSERT_SOME(os::write(path::join(layer1, "file"), "test1"));

  string layer2 = path::join(sandbox.get(), "source2");
  ASSERT_SOME(os::mkdir(layer2));
  ASSERT_SOME(os::mkdir(path::join(layer2, "dir2")));
  ASSERT_SOME(os::write(path::join(layer2, "dir2", "2"), "2"));
  ASSERT_SOME(os::write(path::join(layer2, "file"), "test2"));

  string rootfs = path::join(sandbox.get(), "rootfs");

  hashmap<string, Owned<Backend>> backends = Backend::create(slave::Flags());
  ASSERT_TRUE(backends.contains(AUFS_BACKEND));

  AWAIT_READY(backends[AUFS_BACKEND]->provision(
      {layer1, layer2},
      rootfs,
      sandbox.get()));

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir1", "1")));
  EXPECT_SOME_EQ("1", os::read(path::join(rootfs, "dir1", "1")));

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir2", "2")));
  EXPECT_SOME_EQ("2", os::read(path::join(rootfs, "dir2", "2")));

  // Last layer should overwrite existing file of earlier layers.
  EXPECT_TRUE(os::exists(path::join(rootfs, "file")));
  EXPECT_SOME_EQ("test2", os::read(path::join(rootfs, "file")));

  // Rootfs should be writable.
  ASSERT_SOME(os::write(path::join(rootfs, "file"), "test3"));

  // Files created in rootfs should shadow the files of lower dirs.
  EXPECT_SOME_EQ("test3", os::read(path::join(rootfs, "file")));
  EXPECT_SOME_EQ("test2", os::read(path::join(layer2, "file")));

  AWAIT_READY(backends[AUFS_BACKEND]->destroy(rootfs, sandbox.get()));

  EXPECT_FALSE(os::exists(rootfs));
}


// Test aufs backend for rootfs provisioning when an image has
// many layers. This test is used to verify the fix for MESOS-6001.
TEST_F(AufsBackendTest, ROOT_AUFS_AufsBackendWithManyLayers)
{
  // Create 64 image layers with more than 64 char length path to make
  // sure total length of mount option exceeds the 4096 support limit.
  const int imageCount = 64;
  vector<string> layers;

  for (int i = 0; i < imageCount; ++i) {
    const string layer = path::join(
        sandbox.get(),
        strings::format("lower_%.59d", i).get());

    const string dir = strings::format("dir%d", i).get();

    ASSERT_SOME(os::mkdir(layer));
    ASSERT_SOME(os::mkdir(path::join(layer, dir)));

    layers.push_back(layer);
  }

  string rootfs = path::join(sandbox.get(), "rootfs");

  hashmap<string, Owned<Backend>> backends = Backend::create(slave::Flags());
  ASSERT_TRUE(backends.contains(AUFS_BACKEND));

  AWAIT_READY(backends[AUFS_BACKEND]->provision(
      layers,
      rootfs,
      sandbox.get()));

  // Verify that all layers are available.
  for (int i = 0; i < imageCount; ++i) {
    EXPECT_TRUE(os::exists(path::join(
        rootfs,
        strings::format("dir%d", i).get())));
  }

  AWAIT_READY(backends[AUFS_BACKEND]->destroy(rootfs, sandbox.get()));
}
#endif // __linux__


class CopyBackendTest : public TemporaryDirectoryTest {};


// Provision a rootfs using multiple layers with the copy backend.
TEST_F(CopyBackendTest, ROOT_CopyBackend)
{
  string layer1 = path::join(sandbox.get(), "source1");
  ASSERT_SOME(os::mkdir(layer1));
  ASSERT_SOME(os::mkdir(path::join(layer1, "dir1")));
  ASSERT_SOME(os::write(path::join(layer1, "dir1", "1"), "1"));
  ASSERT_SOME(os::write(path::join(layer1, "file"), "test1"));

  string layer2 = path::join(sandbox.get(), "source2");
  ASSERT_SOME(os::mkdir(layer2));
  ASSERT_SOME(os::mkdir(path::join(layer2, "dir2")));
  ASSERT_SOME(os::write(path::join(layer2, "dir2", "2"), "2"));
  ASSERT_SOME(os::write(path::join(layer2, "file"), "test2"));

  string rootfs = path::join(sandbox.get(), "rootfs");

  hashmap<string, Owned<Backend>> backends = Backend::create(slave::Flags());
  ASSERT_TRUE(backends.contains(COPY_BACKEND));

  AWAIT_READY(backends[COPY_BACKEND]->provision(
      {layer1, layer2},
      rootfs,
      sandbox.get()));

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir1", "1")));
  EXPECT_SOME_EQ("1", os::read(path::join(rootfs, "dir1", "1")));

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir2", "2")));
  EXPECT_SOME_EQ("2", os::read(path::join(rootfs, "dir2", "2")));

  // Last layer should overwrite existing file.
  EXPECT_TRUE(os::exists(path::join(rootfs, "file")));
  EXPECT_SOME_EQ("test2", os::read(path::join(rootfs, "file")));

  AWAIT_READY(backends[COPY_BACKEND]->destroy(rootfs, sandbox.get()));

  EXPECT_FALSE(os::exists(rootfs));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
