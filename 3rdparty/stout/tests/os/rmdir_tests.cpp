// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <list>
#include <map>
#include <set>
#include <string>

#include <stout/fs.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/exec.hpp>
#include <stout/os/getcwd.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/stat.hpp>
#include <stout/os/touch.hpp>

#include <stout/tests/utils.hpp>

using std::list;
using std::set;
using std::string;


static hashset<string> listfiles(const string& directory)
{
  hashset<string> fileset;
  Try<list<string>> entries = os::ls(directory);
  if (entries.isSome()) {
    foreach (const string& entry, entries.get()) {
      fileset.insert(entry);
    }
  }
  return fileset;
}


class RmdirTest : public TemporaryDirectoryTest {};


// TODO(hausdorff): This test is almost copy-pasted from
// `TrivialRemoveEmptyDirectoryRelativePath`; we should parameterize them to
// reduce redundancy.
TEST_F(RmdirTest, TrivialRemoveEmptyDirectoryAbsolutePath)
{
  const string tmpdir = os::getcwd();

  // Directory is initially empty.
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(tmpdir));

  // Successfully make directory using absolute path.
  const string newDirectoryName = "newDirectory";
  const string newDirectoryAbsolutePath = path::join(tmpdir, newDirectoryName);
  const hashset<string> expectedListing = { newDirectoryName };

  EXPECT_SOME(os::mkdir(newDirectoryAbsolutePath));
  EXPECT_EQ(expectedListing, listfiles(tmpdir));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(newDirectoryAbsolutePath));

  // Successfully remove.
  EXPECT_SOME(os::rmdir(newDirectoryAbsolutePath));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(tmpdir));
}


TEST_F(RmdirTest, TrivialRemoveEmptyDirectoryRelativePath)
{
  const string tmpdir = os::getcwd();

  // Directory is initially empty.
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(tmpdir));

  // Successfully make directory using relative path.
  const string newDirectoryName = "newDirectory";
  const hashset<string> expectedListing = { newDirectoryName };

  EXPECT_SOME(os::mkdir(newDirectoryName));
  EXPECT_EQ(expectedListing, listfiles(tmpdir));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(newDirectoryName));

  // Successfully remove.
  EXPECT_SOME(os::rmdir(newDirectoryName));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(tmpdir));
}


// Tests behavior of `rmdir` when path points at a file instead of a directory.
TEST_F(RmdirTest, RemoveFile)
{
  const string tmpdir = os::getcwd();

  // Directory is initially empty.
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(tmpdir));

  // Successfully make directory using absolute path, and then `touch` a file
  // in that folder.
  const string newDirectoryName = "newDirectory";
  const string newDirectoryAbsolutePath = path::join(tmpdir, newDirectoryName);
  const string newFileName = "newFile";
  const string newFileAbsolutePath = path::join(
      newDirectoryAbsolutePath,
      newFileName);

  const hashset<string> expectedRootListing = { newDirectoryName };
  const hashset<string> expectedSubListing = { newFileName };

  EXPECT_SOME(os::mkdir(newDirectoryAbsolutePath));
  EXPECT_SOME(os::touch(newFileAbsolutePath));
  EXPECT_EQ(expectedRootListing, listfiles(tmpdir));
  EXPECT_EQ(expectedSubListing, listfiles(newDirectoryAbsolutePath));

  // Successful recursive remove with `removeRoot` set to `true` (using the
  // semantics of `rm -r`).
  EXPECT_SOME(os::rmdir(newFileAbsolutePath));
  EXPECT_TRUE(os::exists(newDirectoryAbsolutePath));
  ASSERT_EQ(hashset<string>::EMPTY, listfiles(newDirectoryAbsolutePath));

  // Add file to directory again.
  EXPECT_SOME(os::touch(newFileAbsolutePath));

  // Successful recursive remove with `removeRoot` set to `false` (using the
  // semantics of `rm -r`).
  EXPECT_SOME(os::rmdir(newFileAbsolutePath, true, false));
  EXPECT_TRUE(os::exists(newDirectoryAbsolutePath));
  ASSERT_EQ(hashset<string>::EMPTY, listfiles(newDirectoryAbsolutePath));

  // Add file to directory again.
  EXPECT_SOME(os::touch(newFileAbsolutePath));

  // Error on non-recursive remove with `removeRoot` set to `true` (using the
  // semantics of `rmdir`).
  EXPECT_ERROR(os::rmdir(newFileAbsolutePath, false, true));
  EXPECT_TRUE(os::exists(newDirectoryAbsolutePath));
  EXPECT_TRUE(os::exists(newFileAbsolutePath));

  // Error on non-recursive remove with `removeRoot` set to `false` (using the
  // semantics of `rmdir`).
  EXPECT_ERROR(os::rmdir(newFileAbsolutePath, false, false));
  EXPECT_TRUE(os::exists(newDirectoryAbsolutePath));
  EXPECT_TRUE(os::exists(newFileAbsolutePath));
}


TEST_F(RmdirTest, RemoveRecursiveByDefault)
{
  const string tmpdir = os::getcwd();

  // Directory is initially empty.
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(tmpdir));

  // Successfully make directory using absolute path, and then `touch` a file
  // in that folder.
  const string newDirectoryName = "newDirectory";
  const string newDirectoryAbsolutePath = path::join(tmpdir, newDirectoryName);
  const string newFileName = "newFile";
  const string newFileAbsolutePath = path::join(
      newDirectoryAbsolutePath,
      newFileName);

  const hashset<string> expectedRootListing = { newDirectoryName };
  const hashset<string> expectedSubListing = { newFileName };

  EXPECT_SOME(os::mkdir(newDirectoryAbsolutePath));
  EXPECT_SOME(os::touch(newFileAbsolutePath));
  EXPECT_EQ(expectedRootListing, listfiles(tmpdir));
  EXPECT_EQ(expectedSubListing, listfiles(newDirectoryAbsolutePath));

  // Successfully remove.
  EXPECT_SOME(os::rmdir(newDirectoryAbsolutePath));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(tmpdir));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(newDirectoryAbsolutePath));
}


TEST_F(RmdirTest, TrivialFailToRemoveInvalidPath)
{
  // Directory is initially empty.
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(os::getcwd()));

  // Removing fake relative paths should error out.
  EXPECT_ERROR(os::rmdir("fakeRelativePath", false));
  EXPECT_ERROR(os::rmdir("fakeRelativePath", true));

  // Directory still empty.
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(os::getcwd()));
}


TEST_F(RmdirTest, FailToRemoveNestedInvalidPath)
{
  const string tmpdir = os::getcwd();

  // Directory is initially empty.
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(tmpdir));

  // Successfully make directory using absolute path.
  const string newDirectoryName = "newDirectory";
  const string newDirectoryAbsolutePath = path::join(tmpdir, newDirectoryName);

  const hashset<string> expectedRootListing = { newDirectoryName };

  EXPECT_SOME(os::mkdir(newDirectoryAbsolutePath));
  EXPECT_EQ(expectedRootListing, listfiles(tmpdir));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(newDirectoryAbsolutePath));

  // Fail to remove a path to an invalid folder inside the
  // `newDirectoryAbsolutePath`.
  const string fakeAbsolutePath = path::join(newDirectoryAbsolutePath, "fake");
  EXPECT_ERROR(os::rmdir(fakeAbsolutePath, false));
  EXPECT_EQ(expectedRootListing, listfiles(tmpdir));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(newDirectoryAbsolutePath));

  // Test the same thing, but using the `recursive` flag.
  EXPECT_ERROR(os::rmdir(fakeAbsolutePath, true));
  EXPECT_EQ(expectedRootListing, listfiles(tmpdir));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(newDirectoryAbsolutePath));
}


#ifndef __WINDOWS__
// This test verifies that `rmdir` can remove a directory with a
// device file.
//
// NOTE: Enable this test if `os::rdev` and `os::mknod` are
// implemented on Windows. 'os::rdev` calls `::lstat` and `::stat`.
TEST_F(RmdirTest, RemoveDirectoryWithDeviceFile)
{
#ifdef __FreeBSD__
  // If we're in a jail on FreeBSD, we can't use mknod.
  if (isJailed()) {
      return;
  }
#endif

  // mknod requires root permission.
  Result<string> user = os::user();
  ASSERT_SOME(user);

  if (user.get() != "root") {
    return;
  }

  // Create a 'char' device file with major number same as that of
  // `/dev/null`.
  const string deviceDirectory = path::join(os::getcwd(),
      "deviceDirectory");
  ASSERT_SOME(os::mkdir(deviceDirectory));

  const string device = "null";

  const string existing = path::join("/dev", device);
  ASSERT_TRUE(os::exists(existing));

  Try<mode_t> mode = os::stat::mode(existing);
  ASSERT_SOME(mode);

  Try<dev_t> rdev = os::stat::rdev(existing);
  ASSERT_SOME(rdev);

  const string another = path::join(deviceDirectory, device);
  ASSERT_FALSE(os::exists(another));

  EXPECT_SOME(os::mknod(another, mode.get(), rdev.get()));

  EXPECT_SOME(os::rmdir(deviceDirectory));
}
#endif // __WINDOWS__


// This test verifies that `rmdir` can remove a directory with a
// symlink that has no target.
TEST_F(RmdirTest, SYMLINK_RmDirNoTargetSymbolicLink)
{
  const string newDirectory = path::join(os::getcwd(), "newDirectory");
  ASSERT_SOME(os::mkdir(newDirectory));

  const string link = path::join(newDirectory, "link");

  // Create a symlink to non-existent file 'tmp'.
  ASSERT_SOME(fs::symlink("tmp", link));

  EXPECT_SOME(os::rmdir(newDirectory));
}


// This test verifies that `rmdir` can remove a directory with a
// "hanging" symlink whose target has been deleted.
TEST_F(RmdirTest, SYMLINK_RemoveDirectoryHangingSymlink)
{
  const string newDirectory = path::join(os::getcwd(), "newDirectory");
  ASSERT_SOME(os::mkdir(newDirectory));

  const string link = path::join(newDirectory, "link");

  // Create a hanging symlink to a directory.
  ASSERT_SOME(os::mkdir("tmp"));
  ASSERT_SOME(fs::symlink("tmp", link));
  ASSERT_SOME(os::rmdir("tmp"));

  // Remove the parent directory to exercise the recursive deletion path of
  // `os::rmdir`.
  EXPECT_SOME(os::rmdir(newDirectory));
}


// This test verifies that `rmdir` will only remove the symbolic link and not
// the target directory.
TEST_F(RmdirTest, SYMLINK_RemoveDirectoryWithSymbolicLinkTargetDirectory)
{
  const string newDirectory = path::join(os::getcwd(), "newDirectory");
  ASSERT_SOME(os::mkdir(newDirectory));

  const string link = path::join(newDirectory, "link");

  const string targetDirectory = path::join(os::getcwd(), "targetDirectory");

  ASSERT_SOME(os::mkdir(targetDirectory));

  // Create a symlink that targets a directory outside the 'newDirectory'.
  ASSERT_SOME(fs::symlink(targetDirectory, link));

  EXPECT_SOME(os::rmdir(newDirectory));

  // Verify that the target directory is not removed.
  ASSERT_TRUE(os::exists(targetDirectory));
}


// This test verifies that `rmdir` will only remove the symbolic link and not
// the target file.
TEST_F(RmdirTest, SYMLINK_RemoveDirectoryWithSymbolicLinkTargetFile)
{
  const string newDirectory = path::join(os::getcwd(), "newDirectory");
  ASSERT_SOME(os::mkdir(newDirectory));

  const string link = path::join(newDirectory, "link");

  const string targetFile = path::join(os::getcwd(), "targetFile");

  ASSERT_SOME(os::touch(targetFile));

  // Create a symlink that targets a file outside the 'newDirectory'.
  ASSERT_SOME(fs::symlink(targetFile, link));

  EXPECT_SOME(os::rmdir(newDirectory));

  // Verify that the target file is not removed.
  ASSERT_TRUE(os::exists(targetFile));
}


// This tests that when appropriately instructed, `rmdir` can remove
// the files and subdirectories that appear in a directory but
// preserve the directory itself.
TEST_F(RmdirTest, RemoveDirectoryButPreserveRoot)
{
  const string newDirectory = path::join(os::getcwd(), "newDirectory");
  ASSERT_SOME(os::mkdir(newDirectory));

  const string subDirectory = path::join(newDirectory, "subDirectory");
  ASSERT_SOME(os::mkdir(subDirectory));

  const string file1 = path::join(newDirectory, "file1");
  ASSERT_SOME(os::touch(file1));

  const string file2 = path::join(subDirectory, "file2");
  ASSERT_SOME(os::touch(file2));

  EXPECT_SOME(os::rmdir(newDirectory, true, false));
  EXPECT_TRUE(os::exists(newDirectory));
  EXPECT_EQ(hashset<string>::EMPTY, listfiles(newDirectory));
}


// This test fixture verifies that `rmdir` behaves correctly
// with option `continueOnError` and makes sure the undeletable
// files from tests are cleaned up during teardown.
class RmdirContinueOnErrorTest : public RmdirTest
{
public:
  void TearDown() override
  {
    if (mountPoint.isSome()) {
      if (os::system("umount -f -l " + mountPoint.get()) != 0) {
        LOG(ERROR) << "Failed to unmount '" << mountPoint.get();
      }
    }

    RmdirTest::TearDown();
  }

protected:
  Option<string> mountPoint;
};


// This test creates a busy mount point which is not directly deletable
// by rmdir and verifies that rmdir deletes all files that
// it's able to delete if `continueOnError = true`.
TEST_F(RmdirContinueOnErrorTest, RemoveWithContinueOnError)
{
  // Mounting a filesystem requires root permission.
  Result<string> user = os::user();
  ASSERT_SOME(user);

  if (user.get() != "root") {
    return;
  }

  const string tmpdir = os::getcwd();

  // Successfully make directory and then `touch` a file
  // in that folder.
  const string directory = "directory";

  // The busy mount point goes before the regular file in `rmdir`'s
  // directory traversal due to their names. This makes sure that
  // an error occurs before all deletable files are deleted.
  const string mountPoint_ = path::join(
      directory,
      "mount.point");

  const string regularFile = path::join(
      directory,
      "regular.file");

  ASSERT_SOME(os::mkdir(directory));
  ASSERT_SOME(os::mkdir(mountPoint_));
  ASSERT_SOME(os::touch(regularFile));

  ASSERT_SOME_EQ(0, os::system(
      "mount --bind " + mountPoint_ + " " + mountPoint_));

  // Register the mount point for cleanup.
  mountPoint = Option<string>(mountPoint_);

  EXPECT_TRUE(os::exists(directory));
  EXPECT_TRUE(os::exists(mountPoint_));
  EXPECT_TRUE(os::exists(regularFile));

  // Run rmdir with `continueOnError = true`.
  ASSERT_ERROR(os::rmdir(directory, true, true, true));

  EXPECT_TRUE(os::exists(directory));
  EXPECT_TRUE(os::exists(mountPoint_));
  EXPECT_FALSE(os::exists(regularFile));
}
