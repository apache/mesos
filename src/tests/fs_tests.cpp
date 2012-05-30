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

#include "common/option.hpp"
#include "common/try.hpp"

#include "common/foreach.hpp"
#include "linux/fs.hpp"

using namespace mesos;
using namespace mesos::internal;

using fs::MountTable;
using fs::FileSystemTable;


TEST(FsTest, MountTableRead)
{
  Try<MountTable> table = MountTable::read(_PATH_MOUNTED);

  ASSERT_TRUE(table.isSome());

  Option<MountTable::Entry> root = Option<MountTable::Entry>::none();
  Option<MountTable::Entry> proc = Option<MountTable::Entry>::none();
  foreach (const MountTable::Entry& entry, table.get().entries) {
    if (entry.dir == "/") {
      root = entry;
    } else if (entry.dir == "/proc") {
      proc = entry;
    }
  }

  EXPECT_TRUE(root.isSome());
  ASSERT_TRUE(proc.isSome());
  EXPECT_EQ(proc.get().type, "proc");
}


TEST(FsTest, MountTableHasOption)
{
  Try<MountTable> table = MountTable::read(_PATH_MOUNTED);

  ASSERT_TRUE(table.isSome());

  Option<MountTable::Entry> proc = Option<MountTable::Entry>::none();
  foreach (const MountTable::Entry& entry, table.get().entries) {
    if (entry.dir == "/proc") {
      proc = entry;
    }
  }

  ASSERT_TRUE(proc.isSome());
  EXPECT_TRUE(proc.get().hasOption("rw"));
  EXPECT_TRUE(proc.get().hasOption("noexec"));
  EXPECT_TRUE(proc.get().hasOption("nodev"));
}


TEST(FsTest, FileSystemTableRead)
{
  Try<FileSystemTable> table = FileSystemTable::read();

  ASSERT_TRUE(table.isSome());

  Option<FileSystemTable::Entry> root = Option<FileSystemTable::Entry>::none();
  Option<FileSystemTable::Entry> proc = Option<FileSystemTable::Entry>::none();
  foreach (const FileSystemTable::Entry& entry, table.get().entries) {
    if (entry.file == "/") {
      root = entry;
    } else if (entry.file == "/proc") {
      proc = entry;
    }
  }

  EXPECT_TRUE(root.isSome());
  ASSERT_TRUE(proc.isSome());
  EXPECT_EQ(proc.get().vfstype, "proc");
}
