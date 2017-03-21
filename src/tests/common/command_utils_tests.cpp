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

#include <gtest/gtest.h>

#include <process/io.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>

#include "common/command_utils.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using std::string;

using process::Future;

namespace mesos {
namespace internal {
namespace tests {

class TarTest : public TemporaryDirectoryTest
{
protected:
  Try<Nothing> createTestFile(
      const Path& file,
      const Option<Path>& directory = None())
  {
    const Path testFile(
        directory.isSome() ?
        path::join(directory.get(), file.string()): file);

    const string& testFileDir = testFile.dirname();
    if (!os::exists(testFileDir)) {
      Try<Nothing> mkdir = os::mkdir(testFileDir, true);
      if (mkdir.isError()) {
        return Error("Failed to create test directory: " + mkdir.error());
      }
    }

    Try<Nothing> write = os::write(testFile, "test");
    if (write.isError()) {
      return Error("Failed to create to test file: " + write.error());
    }

    return Nothing();
  }
};


// Tests the archive APIs for a simple file.
TEST_F_TEMP_DISABLED_ON_WINDOWS(TarTest, File)
{
  // Create a test file.
  const Path testFile("testfile");
  ASSERT_SOME(createTestFile(testFile));

  // Archive the test file.
  const Path outputTarFile("test.tar");
  AWAIT_ASSERT_READY(command::tar(testFile, outputTarFile, None()));
  ASSERT_TRUE(os::exists(outputTarFile));

  // Remove the test file to make sure untar process creates new test file.
  ASSERT_SOME(os::rm(testFile));
  ASSERT_FALSE(os::exists(testFile));

  // Untar the tarball and verify that the original file is created.
  AWAIT_ASSERT_READY(command::untar(outputTarFile));
  ASSERT_TRUE(os::exists(testFile));

  // Verify that the content is same as original file.
  EXPECT_SOME_EQ("test", os::read(testFile));
}


// Tests the archive APIs for a simple directory.
TEST_F_TEMP_DISABLED_ON_WINDOWS(TarTest, Directory)
{
  const Path testDir("test_dir");
  const Path testFile(path::join(testDir, "testfile"));

  // Create a test file in the test directory.
  ASSERT_SOME(createTestFile(testFile));

  // Archive the test directory.
  const Path outputTarFile("test_dir.tar");
  AWAIT_ASSERT_READY(command::tar(testDir, outputTarFile, None()));
  ASSERT_TRUE(os::exists(outputTarFile));

  // Remove the test directory to make sure untar process creates new test file.
  ASSERT_SOME(os::rmdir(testDir));
  ASSERT_FALSE(os::exists(testDir));

  // Untar the tarball and verify that the original directory is created.
  AWAIT_ASSERT_READY(command::untar(outputTarFile));
  ASSERT_TRUE(os::exists(testDir));

  // Verify that the content is same as original file.
  EXPECT_SOME_EQ("test", os::read(testFile));
}


// Tests the archive APIs for archiving after changing directory.
TEST_F_TEMP_DISABLED_ON_WINDOWS(TarTest, ChangeDirectory)
{
  // Create a top directory where the directory to be archived will be placed.
  const Path topDir("top_dir");
  const Path testDir("test_dir");

  const Path testFile(path::join(testDir, "testfile"));

  // Create a test file in the test directory.
  ASSERT_SOME(createTestFile(testFile, topDir));

  // Archive the test directory.
  const Path outputTarFile("test_dir.tar");
  AWAIT_ASSERT_READY(command::tar(testDir, outputTarFile, topDir));
  ASSERT_TRUE(os::exists(outputTarFile));

  // Remove the top directory to make sure untar process creates new directory.
  ASSERT_SOME(os::rmdir(topDir));
  ASSERT_FALSE(os::exists(topDir));

  // Untar the tarball and verify that the original file is created.
  AWAIT_ASSERT_READY(command::untar(outputTarFile));
  ASSERT_TRUE(os::exists(testDir));

  // Verify that the top directory was not created.
  ASSERT_FALSE(os::exists(topDir));

  // Verify that the content is same as original file.
  EXPECT_SOME_EQ("test", os::read(testFile));
}


// Tests the archive APIs for archiving/unarchiving a simple file for BZIP2
// compression.
TEST_F_TEMP_DISABLED_ON_WINDOWS(TarTest, BZIP2CompressFile)
{
  // Create a test file.
  const Path testFile("testfile");
  ASSERT_SOME(createTestFile(testFile));

  // Archive the test file.
  const Path outputTarFile("test.tar");
  AWAIT_ASSERT_READY(command::tar(
      testFile,
      outputTarFile,
      None(),
      command::Compression::BZIP2));

  ASSERT_TRUE(os::exists(outputTarFile));

  // Remove the test file to make sure untar process creates new test file.
  ASSERT_SOME(os::rm(testFile));
  ASSERT_FALSE(os::exists(testFile));

  // Untar the tarball and verify that the original file is created.
  AWAIT_ASSERT_READY(command::untar(outputTarFile));
  ASSERT_TRUE(os::exists(testFile));

  // Verify that the content is same as original file.
  EXPECT_SOME_EQ("test", os::read(testFile));
}


// Tests the archive APIs for archiving/unarchiving a directory for GZIP
// compression. This test specifically tests for the use case of changing
// directory before archiving.
TEST_F_TEMP_DISABLED_ON_WINDOWS(TarTest, GZIPChangeDirectory)
{
  // Create a top directory where the directory to be archived will be placed.
  const Path topDir("top_dir");
  const Path testDir("test_dir");

  const Path testFile(path::join(testDir, "testfile"));

  // Create a test file in the test directory.
  ASSERT_SOME(createTestFile(testFile, topDir));

  // Archive the test directory.
  const Path outputTarFile("test_dir.tar");
  AWAIT_ASSERT_READY(command::tar(
      testDir,
      outputTarFile,
      topDir,
      command::Compression::GZIP));

  ASSERT_TRUE(os::exists(outputTarFile));

  // Remove the top directory to make sure untar process creates new directory.
  ASSERT_SOME(os::rmdir(topDir));
  ASSERT_FALSE(os::exists(topDir));

  // Untar the tarball and verify that the original file is created.
  AWAIT_ASSERT_READY(command::untar(outputTarFile));
  ASSERT_TRUE(os::exists(testDir));

  // Verify that the top directory was not created.
  ASSERT_FALSE(os::exists(topDir));

  // Verify that the content is same as original file.
  EXPECT_SOME_EQ("test", os::read(testFile));
}


class ShasumTest : public TemporaryDirectoryTest {};


TEST_F_TEMP_DISABLED_ON_WINDOWS(ShasumTest, SHA512SimpleFile)
{
  const Path testFile(path::join(os::getcwd(), "test"));

  Try<Nothing> write = os::write(testFile, "hello world");
  ASSERT_SOME(write);

  Future<string> sha512 = command::sha512(testFile);
  AWAIT_ASSERT_READY(sha512);

  ASSERT_EQ(
      sha512.get(),
      "309ecc489c12d6eb4cc40f50c902f2b4d0ed77ee511a7c7a9bcd3ca86d4cd86f989dd35bc5ff499670da34255b45b0cfd830e81f605dcf7dc5542e93ae9cd76f"); // NOLINT(whitespace/line_length)
}


class CompressionTest : public TemporaryDirectoryTest {};


// Tests command::decompress API for a tar + gzip file.
TEST_F_TEMP_DISABLED_ON_WINDOWS(CompressionTest, GZIPDecompressTarFile)
{
  // Create a test file.
  const Path testFile("testfile");

  Try<Nothing> write = os::write(testFile, "test");
  ASSERT_SOME(write);

  // Archive the test file.
  const Path outputTarFile("test.tar");

  AWAIT_ASSERT_READY(command::tar(
      testFile,
      outputTarFile,
      None(),
      command::Compression::GZIP));

  ASSERT_TRUE(os::exists(outputTarFile));

  // Remove the test file.
  ASSERT_SOME(os::rm(testFile));
  ASSERT_FALSE(os::exists(testFile));

  // Append ".gz" as gzip expects it.
  const Path gzipFile(outputTarFile.string() + ".gz");
  ASSERT_SOME(os::rename(outputTarFile, gzipFile));

  AWAIT_ASSERT_READY(command::decompress(gzipFile));

  // Tar file should exist now.
  ASSERT_TRUE(os::exists(outputTarFile));

  // Untar the tarball and verify that the original file is created.
  AWAIT_ASSERT_READY(command::untar(outputTarFile));
  ASSERT_TRUE(os::exists(testFile));

  // Verify that the content is same as original file.
  EXPECT_SOME_EQ("test", os::read(testFile));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
