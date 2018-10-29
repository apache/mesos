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

#include <paths.h>

#include <gmock/gmock.h>

// This header include must be enclosed in an `extern "C"` block to
// workaround a bug in glibc <= 2.12 (see MESOS-7378).
//
// TODO(neilc): Remove this when we no longer support glibc <= 2.12.
extern "C" {
#include <sys/sysmacros.h>
}

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/hashset.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include <stout/tests/utils.hpp>

#include "linux/fs.hpp"

#include "tests/environment.hpp"

using std::set;
using std::string;

using mesos::internal::fs::MountTable;
using mesos::internal::fs::MountInfoTable;

namespace mesos {
namespace internal {
namespace tests {

class FsTest : public TemporaryDirectoryTest {};


TEST_F(FsTest, SupportedFS)
{
  EXPECT_SOME_TRUE(fs::supported("proc"));
  EXPECT_SOME_TRUE(fs::supported("sysfs"));

  EXPECT_SOME_FALSE(fs::supported("nonexistingfs"));
}


TEST_F(FsTest, Type)
{
  Try<string> fsType = os::shell("stat -fc%%t " + os::getcwd());
  EXPECT_SOME(fsType);

  Try<uint32_t> fsTypeId = numify<uint32_t>("0x" + strings::trim(fsType.get()));
  ASSERT_SOME(fsTypeId);

  EXPECT_SOME_EQ(fsTypeId.get(), fs::type(os::getcwd()));
}


TEST_F(FsTest, MountTableRead)
{
  Try<MountTable> table = MountTable::read(_PATH_MOUNTED);

  ASSERT_SOME(table);

  Option<MountTable::Entry> root = None();
  Option<MountTable::Entry> proc = None();
  foreach (const MountTable::Entry& entry, table->entries) {
    if (entry.dir == "/") {
      root = entry;
    } else if (entry.dir == "/proc") {
      proc = entry;
    }
  }

  EXPECT_SOME(root);
  ASSERT_SOME(proc);
  EXPECT_EQ("proc", proc->type);
}


TEST_F(FsTest, MountTableHasOption)
{
  Try<MountTable> table = MountTable::read(_PATH_MOUNTED);

  ASSERT_SOME(table);

  Option<MountTable::Entry> proc = None();
  foreach (const MountTable::Entry& entry, table->entries) {
    if (entry.dir == "/proc") {
      proc = entry;
    }
  }

  ASSERT_SOME(proc);
  EXPECT_TRUE(proc->hasOption(MNTOPT_RW));
}


TEST_F(FsTest, MountInfoTableParse)
{
  // Parse a private mount (no optional fields).
  const string privateMount =
    "19 1 8:1 / / rw,relatime - ext4 /dev/sda1 rw,seclabel,data=ordered";
  Try<MountInfoTable::Entry> entry = MountInfoTable::Entry::parse(privateMount);

  ASSERT_SOME(entry);
  EXPECT_EQ(19, entry->id);
  EXPECT_EQ(1, entry->parent);
  EXPECT_EQ(makedev(8, 1), entry->devno);
  EXPECT_EQ("/", entry->root);
  EXPECT_EQ("/", entry->target);
  EXPECT_EQ("rw,relatime", entry->vfsOptions);
  EXPECT_EQ("rw,seclabel,data=ordered", entry->fsOptions);
  EXPECT_EQ("", entry->optionalFields);
  EXPECT_EQ("ext4", entry->type);
  EXPECT_EQ("/dev/sda1", entry->source);

  // Parse a shared mount (includes one optional field).
  const string sharedMount =
    "19 1 8:1 / / rw,relatime shared:2 - ext4 /dev/sda1 rw,seclabel";
  entry = MountInfoTable::Entry::parse(sharedMount);

  ASSERT_SOME(entry);
  EXPECT_EQ(19, entry->id);
  EXPECT_EQ(1, entry->parent);
  EXPECT_EQ(makedev(8, 1), entry->devno);
  EXPECT_EQ("/", entry->root);
  EXPECT_EQ("/", entry->target);
  EXPECT_EQ("rw,relatime", entry->vfsOptions);
  EXPECT_EQ("rw,seclabel", entry->fsOptions);
  EXPECT_EQ("shared:2", entry->optionalFields);
  EXPECT_EQ("ext4", entry->type);
  EXPECT_EQ("/dev/sda1", entry->source);
}


// TODO(alexr): Enable after MESOS-8709 is resolved.
TEST_F(FsTest, DISABLED_MountInfoTableRead)
{
  // Examine the calling process's mountinfo table.
  Try<MountInfoTable> table = MountInfoTable::read();
  ASSERT_SOME(table);

  // Every system should have at least a rootfs mounted.
  Option<MountInfoTable::Entry> root = None();
  foreach (const MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == "/") {
      root = entry;
    }
  }

  EXPECT_SOME(root);

  // Repeat for pid 1.
  table = MountInfoTable::read(1);
  ASSERT_SOME(table);

  // Every system should have at least a rootfs mounted.
  root = None();
  foreach (const MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == "/") {
      root = entry;
    }
  }

  EXPECT_SOME(root);
}


TEST_F(FsTest, MountInfoTableReadSorted)
{
  // Examine the calling process's mountinfo table.
  Try<MountInfoTable> table = MountInfoTable::read();
  ASSERT_SOME(table);

  hashset<int> ids;

  // Verify that all parent entries appear *before* their children.
  foreach (const MountInfoTable::Entry& entry, table->entries) {
    if (entry.target != "/") {
      ASSERT_TRUE(ids.contains(entry.parent));
    }

    ASSERT_FALSE(ids.contains(entry.id));

    ids.insert(entry.id);
  }
}


TEST_F(FsTest, MountInfoTableReadSortedParentOfSelf)
{
  // Construct a mount info table with a few entries out of order as
  // well as a few having themselves as parents.
  string lines =
    "1 1 0:00 / / rw shared:6 - sysfs sysfs rw\n"
    "6 5 0:00 / /6 rw shared:6 - sysfs sysfs rw\n"
    "7 6 0:00 / /7 rw shared:6 - sysfs sysfs rw\n"
    "8 8 0:00 / /8 rw shared:6 - sysfs sysfs rw\n"
    "9 8 0:00 / /9 rw shared:6 - sysfs sysfs rw\n"
    "2 1 0:00 / /2 rw shared:6 - sysfs sysfs rw\n"
    "3 2 0:00 / /3 rw shared:6 - sysfs sysfs rw\n"
    "4 3 0:00 / /4 rw shared:6 - sysfs sysfs rw\n"
    "5 4 0:00 / /5 rw shared:6 - sysfs sysfs rw\n";

  // Examine the calling process's mountinfo table.
  Try<MountInfoTable> table = MountInfoTable::read(lines);
  ASSERT_SOME(table);

  hashset<int> ids;

  // Verify that all parent entries appear *before* their children.
  foreach (const MountInfoTable::Entry& entry, table->entries) {
    if (entry.target != "/") {
      ASSERT_TRUE(ids.contains(entry.parent));
    }

    ASSERT_FALSE(ids.contains(entry.id));

    ids.insert(entry.id);
  }
}


TEST_F(FsTest, ROOT_ReadOnlyMount)
{
  string directory = os::getcwd();

  string ro = path::join(directory, "ro");
  string rw = path::join(directory, "rw");

  ASSERT_SOME(os::mkdir(ro));
  ASSERT_SOME(os::mkdir(rw));

  ASSERT_SOME(fs::mount(rw, ro, None(), MS_BIND | MS_RDONLY, None()));

  EXPECT_ERROR(os::touch(path::join(ro, "touched")));
  EXPECT_SOME(os::touch(path::join(rw, "touched")));

  EXPECT_SOME(fs::unmount(ro));

  EXPECT_SOME(os::touch(path::join(ro, "touched")));
}


TEST_F(FsTest, ROOT_SharedMount)
{
  string directory = os::getcwd();

  // Do a self bind mount of the temporary directory.
  ASSERT_SOME(fs::mount(directory, directory, None(), MS_BIND, None()));

  // Mark the mount as a shared mount.
  ASSERT_SOME(fs::mount(None(), directory, None(), MS_SHARED, None()));

  // Find the above mount in the mount table.
  Try<MountInfoTable> table = MountInfoTable::read();
  ASSERT_SOME(table);

  Option<MountInfoTable::Entry> entry;
  foreach (const MountInfoTable::Entry& _entry, table->entries) {
    if (_entry.target == directory) {
      entry = _entry;
    }
  }

  ASSERT_SOME(entry);
  EXPECT_SOME(entry->shared());

  // Clean up the mount.
  EXPECT_SOME(fs::unmount(directory));
}


TEST_F(FsTest, ROOT_SlaveMount)
{
  string directory = os::getcwd();

  // Do a self bind mount of the temporary directory.
  ASSERT_SOME(fs::mount(directory, directory, None(), MS_BIND, None()));

  // Mark the mount as a shared mount of its own peer group.
  ASSERT_SOME(fs::mount(None(), directory, None(), MS_PRIVATE, None()));
  ASSERT_SOME(fs::mount(None(), directory, None(), MS_SHARED, None()));

  // Create a sub-mount under 'directory'.
  string source = path::join(directory, "source");
  string target = path::join(directory, "target");

  ASSERT_SOME(os::mkdir(source));
  ASSERT_SOME(os::mkdir(target));

  ASSERT_SOME(fs::mount(source, target, None(), MS_BIND, None()));

  // Mark the sub-mount as a slave mount.
  ASSERT_SOME(fs::mount(None(), target, None(), MS_SLAVE, None()));

  // Find the above sub-mount in the mount table, and check if it is a
  // slave mount as expected.
  Try<MountInfoTable> table = MountInfoTable::read();
  ASSERT_SOME(table);

  Option<MountInfoTable::Entry> parent;
  Option<MountInfoTable::Entry> child;
  foreach (const MountInfoTable::Entry& entry, table->entries) {
    if (entry.target == directory) {
      ASSERT_NONE(parent);
      parent = entry;
    } else if (entry.target == target) {
      ASSERT_NONE(child);
      child = entry;
    }
  }

  ASSERT_SOME(parent);
  ASSERT_SOME(child);

  EXPECT_SOME(parent->shared());
  EXPECT_SOME(child->master());
  EXPECT_EQ(child->master(), parent->shared());

  // Clean up the mount.
  EXPECT_SOME(fs::unmount(target));
  EXPECT_SOME(fs::unmount(directory));
}


TEST_F(FsTest, ROOT_FindTargetInMountInfoTable)
{
  Try<string> base = environment->mkdtemp();
  ASSERT_SOME(base);

  const string sourceDir = path::join(base.get(), "sourceDir");
  const string targetDir = path::join(base.get(), "targetDir");
  const string sourceFile = path::join(base.get(), "sourceFile");
  const string targetFile = path::join(base.get(), "targetFile");

  // `targetDirUnrelated` is a prefix of `targetDir`, but under the
  // same base directory.
  const string sourceDirUnrelated = path::join(base.get(), "source");
  const string targetDirUnrelated = path::join(base.get(), "target");

  const string file = path::join(targetDir, "file");

  ASSERT_SOME(os::mkdir(sourceDir));
  ASSERT_SOME(os::mkdir(targetDir));
  ASSERT_SOME(os::touch(sourceFile));
  ASSERT_SOME(os::touch(targetFile));
  ASSERT_SOME(os::mkdir(sourceDirUnrelated));
  ASSERT_SOME(os::mkdir(targetDirUnrelated));

  ASSERT_SOME(fs::mount(sourceDir, targetDir, None(), MS_BIND, None()));
  ASSERT_SOME(fs::mount(sourceFile, targetFile, None(), MS_BIND, None()));

  ASSERT_SOME(fs::mount(
      sourceDirUnrelated,
      targetDirUnrelated,
      None(),
      MS_BIND,
      None()));

  ASSERT_SOME(os::touch(file));

  Try<MountInfoTable> table = MountInfoTable::read();
  ASSERT_SOME(table);

  Try<MountInfoTable::Entry> entry = table->findByTarget(targetDir);
  ASSERT_SOME(entry);
  EXPECT_EQ(entry->target, targetDir);

  entry = table->findByTarget(file);
  ASSERT_SOME(entry);
  EXPECT_EQ(entry->target, targetDir);

  entry = table->findByTarget(targetFile);
  ASSERT_SOME(entry);
  EXPECT_EQ(entry->target, targetFile);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
