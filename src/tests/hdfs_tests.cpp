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
#include <process/owned.hpp>

#include <stout/check.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/touch.hpp>
#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

#include "hdfs/hdfs.hpp"

using std::string;

using process::Future;
using process::Owned;

namespace mesos {
namespace internal {
namespace tests {

class HdfsTest : public TemporaryDirectoryTest
{
public:
  void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    // Create a fake hadoop command line tool. The tests serialize
    // bash scripts into this file which emulates the hadoop client's
    // logic while operating on the local filesystem.
    hadoop = path::join(os::getcwd(), "hadoop");

    ASSERT_SOME(os::touch(hadoop));

    // Make sure the script has execution permission.
    // TODO(coffler): Work out how to handle permissions on Windows
#ifndef __WINDOWS__
    ASSERT_SOME(os::chmod(
        hadoop,
        S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH));
#endif // __WINDOWS__
  }

protected:
  string hadoop;
};


// This test verifies the 'HDFS::exists(path)' method. We emulate the
// hadoop client by testing the existence of a local file.
//
// TODO(coffler): This test is currently disabled due to 'sh' dependency.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HdfsTest, Exists)
{
  // The script emulating 'hadoop fs -test -e <path>'.
  // NOTE: We emulate a version call here which is exercised when
  // creating the HDFS client.
  ASSERT_SOME(os::write(
      hadoop,
      "#!/bin/sh\n"
      "if [ \"$1\" = \"version\" ]; then\n"
      "  exit 0\n"
      "fi\n"
      "test -e $4\n"));

  Try<Owned<HDFS>> hdfs = HDFS::create(hadoop);
  ASSERT_SOME(hdfs);

  Future<bool> exists = hdfs.get()->exists(hadoop);
  AWAIT_READY(exists);
  EXPECT_TRUE(exists.get());

  exists = hdfs.get()->exists(path::join(os::getcwd(), "NotExists"));
  AWAIT_READY(exists);
  EXPECT_FALSE(exists.get());
}


// This test verifies the 'HDFS::du(path)' method. We emulate the
// hadoop client by doing a 'du' on the local filesystem.
//
// TODO(coffler): This test is currently disabled due to 'sh' dependency.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HdfsTest, Du)
{
  // The script emulating 'hadoop fs -du <path>'.
  // NOTE: We emulate a version call here which is exercised when
  // creating the HDFS client.
  ASSERT_SOME(os::write(
      hadoop,
      "#!/bin/sh\n"
      "if [ \"$1\" = \"version\" ]; then\n"
      "  exit 0\n"
      "fi\n"
      "du $3\n"));

  Try<Owned<HDFS>> hdfs = HDFS::create(hadoop);
  ASSERT_SOME(hdfs);

  Future<Bytes> bytes = hdfs.get()->du(hadoop);
  AWAIT_READY(bytes);

  bytes = hdfs.get()->du(path::join(os::getcwd(), "Invalid"));
  AWAIT_FAILED(bytes);
}


// This is the same test as HdfsTest::Du except it emulates a HDFS
// version that returns 3 fields for the du subcommand.
//
// TODO(coffler): This test is currently disabled due to 'sh' dependency.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HdfsTest, ThreeFieldDu)
{
  // The script emulating 'hadoop fs -du <path>'.
  // NOTE: We emulate a version call here which is exercised when
  // creating the HDFS client.
  ASSERT_SOME(os::write(
      hadoop,
      "#!/bin/sh\n"
      "if [ \"$1\" = \"version\" ]; then\n"
      "  exit 0\n"
      "fi\n"
      "du $3 | awk '{printf \"%s  %s  %s\\n\", $1, $1, $2}' \n"));

  Try<Owned<HDFS>> hdfs = HDFS::create(hadoop);
  ASSERT_SOME(hdfs);

  Future<Bytes> bytes = hdfs.get()->du(hadoop);
  AWAIT_READY(bytes);

  bytes = hdfs.get()->du(path::join(os::getcwd(), "Invalid"));
  AWAIT_FAILED(bytes);
}


// This test verifies the 'HDFS::rm(path)' method. We emulate the
// hadoop client by removing a file on the local filesystem.
//
// TODO(coffler): This test is currently disabled due to 'sh' dependency.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HdfsTest, Rm)
{
  // The script emulating 'hadoop fs -rm <path>'.
  // NOTE: We emulate a version call here which is exercised when
  // creating the HDFS client.
  ASSERT_SOME(os::write(
      hadoop,
      "#!/bin/sh\n"
      "if [ \"$1\" = \"version\" ]; then\n"
      "  exit 0\n"
      "fi\n"
      "rm $3\n"));

  Try<Owned<HDFS>> hdfs = HDFS::create(hadoop);
  ASSERT_SOME(hdfs);

  string file = path::join(os::getcwd(), "file");

  ASSERT_SOME(os::touch(file));

  Future<Nothing> rm = hdfs.get()->rm(file);
  AWAIT_READY(rm);

  rm = hdfs.get()->rm(path::join(os::getcwd(), "Invalid"));
  AWAIT_FAILED(rm);
}


// This test verifies the 'HDFS::copyFromLocal(from, to)' method. We
// emulate the hadoop client by doing a 'cp' on the local filesystem.
//
// TODO(coffler): This test is currently disabled due to 'sh' dependency.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HdfsTest, CopyFromLocal)
{
  // The script emulating 'hadoop fs -copyFromLocal <from> <to>'.
  // NOTE: We emulate a version call here which is exercised when
  // creating the HDFS client.
  ASSERT_SOME(os::write(
      hadoop,
      "#!/bin/sh\n"
      "if [ \"$1\" = \"version\" ]; then\n"
      "  exit 0\n"
      "fi\n"
      "cp $3 $4"));

  Try<Owned<HDFS>> hdfs = HDFS::create(hadoop);
  ASSERT_SOME(hdfs);

  string file1 = path::join(os::getcwd(), "file1");
  string file2 = path::join(os::getcwd(), "file2");

  ASSERT_SOME(os::write(file1, "abc"));

  Future<Nothing> copy = hdfs.get()->copyFromLocal(file1, file2);
  AWAIT_READY(copy);

  EXPECT_SOME_EQ("abc", os::read(file2));
}


// This test verifies the 'HDFS::copyToLocal(from, to)' method. We
// emulate the hadoop client by doing a 'cp' on the local filesystem.
//
// TODO(coffler): This test is currently disabled due to 'sh' dependency.
TEST_F_TEMP_DISABLED_ON_WINDOWS(HdfsTest, CopyToLocal)
{
  // The script emulating 'hadoop fs -copyToLocal <from> <to>'.
  // NOTE: We emulate a version call here which is exercised when
  // creating the HDFS client.
  ASSERT_SOME(os::write(
      hadoop,
      "#!/bin/sh\n"
      "if [ \"$1\" = \"version\" ]; then\n"
      "  exit 0\n"
      "fi\n"
      "cp $3 $4"));

  Try<Owned<HDFS>> hdfs = HDFS::create(hadoop);
  ASSERT_SOME(hdfs);

  string file1 = path::join(os::getcwd(), "file1");
  string file2 = path::join(os::getcwd(), "file2");

  ASSERT_SOME(os::write(file1, "abc"));

  Future<Nothing> copy = hdfs.get()->copyToLocal(file1, file2);
  AWAIT_READY(copy);

  EXPECT_SOME_EQ("abc", os::read(file2));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
