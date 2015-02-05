/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <paths.h>

#include <gmock/gmock.h>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "linux/fs.hpp"


using namespace mesos;

using fs::MountTable;
using fs::FileSystemTable;


TEST(FsTest, MountTableRead)
{
  Try<MountTable> table = MountTable::read(_PATH_MOUNTED);

  ASSERT_SOME(table);

  Option<MountTable::Entry> root = None();
  Option<MountTable::Entry> proc = None();
  foreach (const MountTable::Entry& entry, table.get().entries) {
    if (entry.dir == "/") {
      root = entry;
    } else if (entry.dir == "/proc") {
      proc = entry;
    }
  }

  EXPECT_SOME(root);
  ASSERT_SOME(proc);
  EXPECT_EQ(proc.get().type, "proc");
}


TEST(FsTest, MountTableHasOption)
{
  Try<MountTable> table = MountTable::read(_PATH_MOUNTED);

  ASSERT_SOME(table);

  Option<MountTable::Entry> proc = None();
  foreach (const MountTable::Entry& entry, table.get().entries) {
    if (entry.dir == "/proc") {
      proc = entry;
    }
  }

  ASSERT_SOME(proc);
  EXPECT_TRUE(proc.get().hasOption(MNTOPT_RW));
}


TEST(FsTest, FileSystemTableRead)
{
  Try<FileSystemTable> table = FileSystemTable::read();

  ASSERT_SOME(table);

  // NOTE: We do not check for /proc because, it is not always present in
  // /etc/fstab.
  Option<FileSystemTable::Entry> root = None();
  foreach (const FileSystemTable::Entry& entry, table.get().entries) {
    if (entry.file == "/") {
      root = entry;
    }
  }

  EXPECT_SOME(root);
}
