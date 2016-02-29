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

#include "tests/flags.hpp"

using namespace process;

using namespace mesos::internal::slave;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

#ifdef __linux__
class MountBackendTest : public TemporaryDirectoryTest
{
protected:
  virtual void TearDown()
  {
    Try<fs::MountInfoTable> mountTable = fs::MountInfoTable::read();
    ASSERT_SOME(mountTable);

    // TODO(xujyan): Make sandbox a plain string instead of an option.
    ASSERT_SOME(sandbox);
    foreach (const fs::MountInfoTable::Entry& entry, mountTable.get().entries) {
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
  string layer1 = path::join(os::getcwd(), "source1");
  ASSERT_SOME(os::mkdir(layer1));
  ASSERT_SOME(os::mkdir(path::join(layer1, "dir1")));
  ASSERT_SOME(os::write(path::join(layer1, "dir1", "1"), "1"));
  ASSERT_SOME(os::write(path::join(layer1, "file"), "test1"));

  string layer2 = path::join(os::getcwd(), "source2");
  ASSERT_SOME(os::mkdir(layer2));
  ASSERT_SOME(os::mkdir(path::join(layer2, "dir2")));
  ASSERT_SOME(os::write(path::join(layer2, "dir2", "2"), "2"));
  ASSERT_SOME(os::write(path::join(layer2, "file"), "test2"));

  string rootfs = path::join(os::getcwd(), "rootfs");

  hashmap<string, Owned<Backend>> backends = Backend::create(slave::Flags());
  ASSERT_TRUE(backends.contains("overlay"));

  AWAIT_READY(backends["overlay"]->provision({layer1, layer2}, rootfs));

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir1", "1")));
  Try<string> read = os::read(path::join(rootfs, "dir1", "1"));
  EXPECT_SOME_EQ("1", read);

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir2", "2")));
  read = os::read(path::join(rootfs, "dir2", "2"));
  EXPECT_SOME_EQ("2", read);

  EXPECT_TRUE(os::exists(path::join(rootfs, "file")));
  read = os::read(path::join(rootfs, "file"));
  // Last layer should overwrite existing file of earlier layers.
  EXPECT_SOME_EQ("test2", read);

  AWAIT_READY(backends["overlay"]->destroy(rootfs));

  EXPECT_FALSE(os::exists(rootfs));
}


class BindBackendTest : public MountBackendTest {};


// Provision a rootfs using a BindBackend to another directory and
// verify if it is read-only within the mount.
TEST_F(BindBackendTest, ROOT_BindBackend)
{
  string rootfs = path::join(os::getcwd(), "source");

  // Create a writable directory under the dummy rootfs.
  Try<Nothing> mkdir = os::mkdir(path::join(rootfs, "tmp"));
  ASSERT_SOME(mkdir);

  hashmap<string, Owned<Backend>> backends = Backend::create(slave::Flags());
  ASSERT_TRUE(backends.contains("bind"));

  string target = path::join(os::getcwd(), "target");

  AWAIT_READY(backends["bind"]->provision({rootfs}, target));

  EXPECT_TRUE(os::stat::isdir(path::join(target, "tmp")));

  // 'target' _appears_ to be writable but is really not due to read-only mount.
  Try<mode_t> mode = os::stat::mode(path::join(target, "tmp"));
  ASSERT_SOME(mode);
  EXPECT_TRUE(os::Permissions(mode.get()).owner.w);
  EXPECT_ERROR(os::write(path::join(target, "tmp", "test"), "data"));

  AWAIT_READY(backends["bind"]->destroy(target));

  EXPECT_FALSE(os::exists(target));
}
#endif // __linux__


class CopyBackendTest : public TemporaryDirectoryTest {};


// Provision a rootfs using multiple layers with the copy backend.
TEST_F(CopyBackendTest, ROOT_CopyBackend)
{
  string layer1 = path::join(os::getcwd(), "source1");
  ASSERT_SOME(os::mkdir(layer1));
  ASSERT_SOME(os::mkdir(path::join(layer1, "dir1")));
  ASSERT_SOME(os::write(path::join(layer1, "dir1", "1"), "1"));
  ASSERT_SOME(os::write(path::join(layer1, "file"), "test1"));

  string layer2 = path::join(os::getcwd(), "source2");
  ASSERT_SOME(os::mkdir(layer2));
  ASSERT_SOME(os::mkdir(path::join(layer2, "dir2")));
  ASSERT_SOME(os::write(path::join(layer2, "dir2", "2"), "2"));
  ASSERT_SOME(os::write(path::join(layer2, "file"), "test2"));

  string rootfs = path::join(os::getcwd(), "rootfs");

  hashmap<string, Owned<Backend>> backends = Backend::create(slave::Flags());
  ASSERT_TRUE(backends.contains("copy"));

  AWAIT_READY(backends["copy"]->provision({layer1, layer2}, rootfs));

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir1", "1")));
  Try<string> read = os::read(path::join(rootfs, "dir1", "1"));
  ASSERT_SOME(read);
  EXPECT_EQ("1", read.get());

  EXPECT_TRUE(os::exists(path::join(rootfs, "dir2", "2")));
  read = os::read(path::join(rootfs, "dir2", "2"));
  ASSERT_SOME(read);
  EXPECT_EQ("2", read.get());

  EXPECT_TRUE(os::exists(path::join(rootfs, "file")));
  read = os::read(path::join(rootfs, "file"));
  ASSERT_SOME(read);

  // Last layer should overwrite existing file.
  EXPECT_EQ("test2", read.get());

  AWAIT_READY(backends["copy"]->destroy(rootfs));

  EXPECT_FALSE(os::exists(rootfs));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
