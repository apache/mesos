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

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <string>

#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/hashset.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include <stout/os/access.hpp>
#include <stout/os/dup.hpp>
#include <stout/os/find.hpp>
#include <stout/os/getcwd.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/lseek.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/pipe.hpp>
#include <stout/os/read.hpp>
#include <stout/os/realpath.hpp>
#include <stout/os/rename.hpp>
#include <stout/os/rm.hpp>
#include <stout/os/touch.hpp>
#include <stout/os/write.hpp>
#include <stout/os/xattr.hpp>

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


class FsTest : public TemporaryDirectoryTest {};


TEST_F(FsTest, Find)
{
  const string testdir =
    path::join(os::getcwd(), id::UUID::random().toString());
  const string subdir = path::join(testdir, "test1");
  ASSERT_SOME(os::mkdir(subdir)); // Create the directories.

  // Now write some files.
  const string file1 = path::join(testdir, "file1.txt");
  const string file2 = path::join(subdir, "file2.txt");
  const string file3 = path::join(subdir, "file3.jpg");

  ASSERT_SOME(os::touch(file1));
  ASSERT_SOME(os::touch(file2));
  ASSERT_SOME(os::touch(file3));

  // Find "*.txt" files.
  Try<list<string>> result = os::find(testdir, ".txt");
  ASSERT_SOME(result);

  hashset<string> files;
  foreach (const string& file, result.get()) {
    files.insert(file);
  }

  ASSERT_EQ(2u, files.size());
  ASSERT_TRUE(files.contains(file1));
  ASSERT_TRUE(files.contains(file2));
}


TEST_F(FsTest, ReadWriteString)
{
  const string testfile =
    path::join(os::getcwd(), id::UUID::random().toString());
  const string teststr = "line1\nline2";

  ASSERT_SOME(os::write(testfile, teststr));

  Try<string> readstr = os::read(testfile);

  EXPECT_SOME_EQ(teststr, readstr);
}


TEST_F(FsTest, Mkdir)
{
  const hashset<string> EMPTY;
  const string tmpdir = os::getcwd();

  hashset<string> expectedListing = EMPTY;
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  ASSERT_SOME(os::mkdir(path::join(tmpdir, "a", "b", "c")));
  ASSERT_SOME(os::mkdir(path::join(tmpdir, "a", "b", "d")));
  ASSERT_SOME(os::mkdir(path::join(tmpdir, "e", "f")));

  expectedListing = EMPTY;
  expectedListing.insert("a");
  expectedListing.insert("e");
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  expectedListing = EMPTY;
  expectedListing.insert("b");
  EXPECT_EQ(expectedListing, listfiles(path::join(tmpdir, "a")));

  expectedListing = EMPTY;
  expectedListing.insert("c");
  expectedListing.insert("d");
  EXPECT_EQ(expectedListing, listfiles(path::join(tmpdir, "a", "b")));

  expectedListing = EMPTY;
  EXPECT_EQ(expectedListing, listfiles(path::join(tmpdir, "a", "b", "c")));
  EXPECT_EQ(expectedListing, listfiles(path::join(tmpdir, "a", "b", "d")));

  expectedListing.insert("f");
  EXPECT_EQ(expectedListing, listfiles(path::join(tmpdir, "e")));

  expectedListing = EMPTY;
  EXPECT_EQ(expectedListing, listfiles(path::join(tmpdir, "e", "f")));
}


TEST_F(FsTest, Exists)
{
  const hashset<string> EMPTY;
  const string tmpdir = os::getcwd();

  hashset<string> expectedListing = EMPTY;
  ASSERT_EQ(expectedListing, listfiles(tmpdir));

  // Create simple directory structure.
  ASSERT_SOME(os::mkdir(path::join(tmpdir, "a", "b", "c")));

  // Expect all the directories exist.
  EXPECT_TRUE(os::exists(tmpdir));
  EXPECT_TRUE(os::exists(path::join(tmpdir, "a")));
  EXPECT_TRUE(os::exists(path::join(tmpdir, "a", "b")));
  EXPECT_TRUE(os::exists(path::join(tmpdir, "a", "b", "c")));

  // Return false if a component of the path does not exist.
  EXPECT_FALSE(os::exists(path::join(tmpdir, "a", "fakeDir")));
  EXPECT_FALSE(os::exists(path::join(tmpdir, "a", "fakeDir", "c")));

  // Add file to directory tree.
  ASSERT_SOME(os::touch(path::join(tmpdir, "a", "b", "c", "yourFile")));

  // Assert it exists.
  EXPECT_TRUE(os::exists(path::join(tmpdir, "a", "b", "c", "yourFile")));

  // Return false if file is wrong.
  EXPECT_FALSE(os::exists(path::join(tmpdir, "a", "b", "c", "yourFakeFile")));
}


TEST_F(FsTest, Touch)
{
  const string testfile =
    path::join(os::getcwd(), id::UUID::random().toString());

  ASSERT_SOME(os::touch(testfile));
  ASSERT_TRUE(os::exists(testfile));
}


#ifdef __WINDOWS__
// This tests the expected behavior of the `longpath` helper.
TEST_F(FsTest, WindowsInternalLongPath)
{
  using ::internal::windows::longpath;

  // Not absolute.
  EXPECT_EQ(longpath("path"), wide_stringify("path"));

  // Absolute, but short.
  EXPECT_EQ(longpath("C:\\path"), wide_stringify("C:\\path"));

  // Edge case exactly one under `max_path_length`.
  const size_t max_path_length = 248;
  const string root = "C:\\";
  string path = root + string(max_path_length - root.length() - 1, 'c');
  EXPECT_EQ(path.length(), max_path_length - 1);
  EXPECT_EQ(longpath(path), wide_stringify(path));

  // Edge case exactly at `max_path_length`.
  path += "c";
  EXPECT_EQ(path.length(), max_path_length);
  EXPECT_EQ(longpath(path), wide_stringify(os::LONGPATH_PREFIX + path));

  // Edge case exactly one over `max_path_length`.
  path += "c";
  EXPECT_EQ(path.length(), max_path_length + 1);
  EXPECT_EQ(longpath(path), wide_stringify(os::LONGPATH_PREFIX + path));

  // Idempotency.
  EXPECT_EQ(longpath(os::LONGPATH_PREFIX + path),
            wide_stringify(os::LONGPATH_PREFIX + path));
}
#endif // __WINDOWS__


// This test attempts to perform some basic file operations on a file
// with an absolute path at exactly the internal `MAX_PATH` of 248.
//
// NOTE: This tests an edge case on Windows, but is a cross-platform test.
TEST_F(FsTest, CreateDirectoryAtMaxPath)
{
  const size_t max_path_length = 248;
  const string testdir = path::join(
    sandbox.get(),
    string(max_path_length - sandbox->length() - 1 /* separator */, 'c'));

  EXPECT_EQ(testdir.length(), max_path_length);
  ASSERT_SOME(os::mkdir(testdir));

  const string testfile = path::join(testdir, "file.txt");

  EXPECT_SOME(os::touch(testfile));
  EXPECT_TRUE(os::exists(testfile));
  EXPECT_SOME_TRUE(os::access(testfile, R_OK | W_OK));
  EXPECT_SOME_EQ(testfile, os::realpath(testfile));
}


// This test attempts to perform some basic file operations on a file
// with an absolute path longer than the `MAX_PATH`.
//
// NOTE: This tests an edge case on Windows, but is a cross-platform test.
TEST_F(FsTest, CreateDirectoryLongerThanMaxPath)
{
  string testdir = sandbox.get();
  const size_t max_path_length = 260;
  while (testdir.length() <= max_path_length) {
    testdir = path::join(testdir, id::UUID::random().toString());
  }

  EXPECT_TRUE(testdir.length() > max_path_length);
  ASSERT_SOME(os::mkdir(testdir));

  const string testfile = path::join(testdir, "file.txt");

  EXPECT_SOME(os::touch(testfile));
  EXPECT_TRUE(os::exists(testfile));
  EXPECT_SOME_TRUE(os::access(testfile, R_OK | W_OK));
  EXPECT_SOME_EQ(testfile, os::realpath(testfile));
}


// This test ensures that `os::realpath` will work on open files.
//
// NOTE: This tests an edge case on Windows, but is a cross-platform test.
TEST_F(FsTest, RealpathValidationOnOpenFile)
{
  // Open a file to write, with "SHARE" read/write permissions,
  // then call `os::realpath` on that file.
  const string file = path::join(sandbox.get(), id::UUID::random().toString());

  const Try<int_fd> fd = os::open(file, O_CREAT | O_RDWR);
  ASSERT_SOME(fd);
  EXPECT_SOME(os::write(fd.get(), "data"));

  // Verify that `os::realpath` (which calls `CreateFileW` on Windows) is
  // successful even though the file is open elsewhere.
  EXPECT_SOME_EQ(file, os::realpath(file));
  EXPECT_SOME(os::close(fd.get()));
}


TEST_F(FsTest, SYMLINK_Symlink)
{
  const string temp_path = os::getcwd();
  const string link = path::join(temp_path, "sym.link");
  const string file = path::join(temp_path, id::UUID::random().toString());

  // Create file
  ASSERT_SOME(os::touch(file))
      << "Failed to create file '" << file << "'";
  ASSERT_TRUE(os::exists(file));

  // Create symlink
  ASSERT_SOME(fs::symlink(file, link));

  // Test symlink
  EXPECT_TRUE(os::stat::islink(link));
}


TEST_F(FsTest, SYMLINK_Rm)
{
  const string tmpdir = os::getcwd();

  hashset<string> expectedListing;
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  const string file1 = path::join(tmpdir, "file1.txt");
  const string directory1 = path::join(tmpdir, "directory1");

  const string fileSymlink1 = path::join(tmpdir, "fileSymlink1");
  const string fileToSymlink = path::join(tmpdir, "fileToSymlink.txt");
  const string directorySymlink1 = path::join(tmpdir, "directorySymlink1");
  const string directoryToSymlink = path::join(tmpdir, "directoryToSymlink");

  // Create a file, a directory, and a symlink to a file and a directory.
  ASSERT_SOME(os::touch(file1));
  expectedListing.insert("file1.txt");

  ASSERT_SOME(os::mkdir(directory1));
  expectedListing.insert("directory1");

  ASSERT_SOME(os::touch(fileToSymlink));
  ASSERT_SOME(fs::symlink(fileToSymlink, fileSymlink1));
  expectedListing.insert("fileToSymlink.txt");
  expectedListing.insert("fileSymlink1");

  ASSERT_SOME(os::mkdir(directoryToSymlink));
  ASSERT_SOME(fs::symlink(directoryToSymlink, directorySymlink1));
  expectedListing.insert("directoryToSymlink");
  expectedListing.insert("directorySymlink1");

  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  // Verify `rm` of non-empty directory fails.
  EXPECT_ERROR(os::rm(tmpdir));
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  // Remove all, and verify.
  EXPECT_SOME(os::rm(file1));
  expectedListing.erase("file1.txt");

  EXPECT_SOME(os::rm(directory1));
  expectedListing.erase("directory1");

  EXPECT_SOME(os::rm(fileSymlink1));
  expectedListing.erase("fileSymlink1");

  EXPECT_SOME(os::rm(directorySymlink1));
  expectedListing.erase("directorySymlink1");

  // `os::rm` doesn't act on the target, therefore we must verify they each
  // still exist.
  EXPECT_EQ(expectedListing, listfiles(tmpdir));

  // Verify that we error out for paths that don't exist.
  EXPECT_ERROR(os::rm("fakeFile"));
}


TEST_F(FsTest, List)
{
  const string testdir =
    path::join(os::getcwd(), id::UUID::random().toString());
  ASSERT_SOME(os::mkdir(testdir)); // Create the directories.

  // Now write some files.
  const string file1 = path::join(testdir, "file1.txt");
  const string file2 = path::join(testdir, "file2.txt");
  const string file3 = path::join(testdir, "file3.jpg");

  ASSERT_SOME(os::touch(file1));
  ASSERT_SOME(os::touch(file2));
  ASSERT_SOME(os::touch(file3));

  // Search all files in folder
  Try<list<string>> allFiles = fs::list(path::join(testdir, "*"));
  ASSERT_SOME(allFiles);
  EXPECT_EQ(3u, allFiles->size());

  // Search .jpg files in folder
  Try<list<string>> jpgFiles = fs::list(path::join(testdir, "*.jpg"));
  ASSERT_SOME(jpgFiles);
  EXPECT_EQ(1u, jpgFiles->size());

  // Search test*.txt files in folder
  Try<list<string>> testTxtFiles = fs::list(path::join(testdir, "*.txt"));
  ASSERT_SOME(testTxtFiles);
  EXPECT_EQ(2u, testTxtFiles->size());

  // Verify that we return empty list when we provide an invalid path.
  Try<list<string>> noFiles = fs::list("this_path_does_not_exist");
  ASSERT_SOME(noFiles);
  EXPECT_TRUE(noFiles->empty());
}


TEST_F(FsTest, Rename)
{
  const string testdir =
    path::join(os::getcwd(), id::UUID::random().toString());
  ASSERT_SOME(os::mkdir(testdir)); // Create the directories.

  // Now write some files.
  const string file1 = path::join(testdir, "file1.txt");
  const string file2 = path::join(testdir, "file2.txt");
  const string file3 = path::join(testdir, "file3.jpg");

  ASSERT_SOME(os::touch(file1));
  ASSERT_SOME(os::touch(file2));

  // Write something to `file1`.
  const string message = "hello world!";
  ASSERT_SOME(os::write(file1, message));

  // Search all files in folder
  Try<list<string>> allFiles = fs::list(path::join(testdir, "*"));
  ASSERT_SOME(allFiles);
  EXPECT_EQ(2u, allFiles->size());

  // Rename a `file1` to `file3`, which does not exist yet. Verify `file3`
  // contains the text that was in `file1`, and make sure the count of files in
  // the directory has stayed the same.
  EXPECT_SOME(os::rename(file1, file3));

  Try<string> file3Contents = os::read(file3);
  ASSERT_SOME(file3Contents);
  EXPECT_EQ(message, file3Contents.get());

  allFiles = fs::list(path::join(testdir, "*"));
  ASSERT_SOME(allFiles);
  EXPECT_EQ(2u, allFiles->size());

  // Rename `file3` -> `file2`. `file2` exists, so this will replace it. Verify
  // text in the file, and that the count of files in the directory have gone
  // down.
  EXPECT_SOME(os::rename(file3, file2));
  Try<string> file2Contents = os::read(file2);
  ASSERT_SOME(file2Contents);
  EXPECT_EQ(message, file2Contents.get());

  allFiles = fs::list(path::join(testdir, "*"));
  ASSERT_SOME(allFiles);
  EXPECT_EQ(1u, allFiles->size());

  // Rename a fake file, verify failure.
  const string fakeFile = testdir + "does_not_exist";
  EXPECT_ERROR(os::rename(fakeFile, file1));

  EXPECT_FALSE(os::exists(file1));

  allFiles = fs::list(path::join(testdir, "*"));
  ASSERT_SOME(allFiles);
  EXPECT_EQ(1u, allFiles->size());
}


#ifdef __WINDOWS__
TEST_F(FsTest, IntFD)
{
  const int_fd fd(INVALID_HANDLE_VALUE);
  EXPECT_EQ(int_fd::Type::HANDLE, fd.type());
  EXPECT_FALSE(fd.is_valid());
  EXPECT_EQ(fd, int_fd(-1));
  EXPECT_EQ(-1, fd);
  EXPECT_LT(fd, 0);
  EXPECT_GT(0, fd);
}
#endif // __WINDOWS__


// NOTE: These tests may not make a lot of sense on Linux, as `open`
// is expected to be implemented correctly by the system. However, on
// Windows we map the POSIX semantics of `open` to `CreateFile`, which
// this checks. These tests passing on Linux assert that the tests
// themselves are correct.
TEST_F(FsTest, Open)
{
  const string testfile =
    path::join(os::getcwd(), id::UUID::random().toString());
  const string data = "data";

  // Without `O_CREAT`, opening a non-existing file should fail.
  EXPECT_FALSE(os::exists(testfile));
  EXPECT_ERROR(os::open(testfile, O_RDONLY));
#ifdef __WINDOWS__
  // `O_EXCL` without `O_CREAT` is undefined, but on Windows, we error.
  EXPECT_ERROR(os::open(testfile, O_RDONLY | O_EXCL));
  EXPECT_ERROR(os::open(testfile, O_RDONLY | O_EXCL | O_TRUNC));
#endif // __WINDOWS__
  EXPECT_ERROR(os::open(testfile, O_RDONLY | O_TRUNC));

  // With `O_CREAT | O_EXCL`, open a non-existing file should succeed.
  Try<int_fd> fd = os::open(testfile, O_CREAT | O_EXCL | O_RDONLY, S_IRWXU);
  ASSERT_SOME(fd);
  EXPECT_TRUE(os::exists(testfile));
  EXPECT_SOME(os::close(fd.get()));

  // File already exists, so `O_EXCL` should fail.
  EXPECT_ERROR(os::open(testfile, O_CREAT | O_EXCL | O_RDONLY));
  EXPECT_ERROR(os::open(testfile, O_CREAT | O_EXCL | O_TRUNC | O_RDONLY));

  // With `O_CREAT` but no `O_EXCL`, it should still open.
  fd = os::open(testfile, O_CREAT | O_RDONLY);
  ASSERT_SOME(fd);
  EXPECT_SOME(os::close(fd.get()));

  // `O_RDWR` should be able to write data, and read it back.
  fd = os::open(testfile, O_RDWR);
  ASSERT_SOME(fd);
  EXPECT_SOME(os::write(fd.get(), data));
  // Seek back to beginning to read the written data.
  EXPECT_SOME_EQ(0, os::lseek(fd.get(), 0, SEEK_SET));
  EXPECT_SOME_EQ(data, os::read(fd.get(), data.size()));
  EXPECT_SOME(os::close(fd.get()));

  // `O_RDONLY` should be able to read the previously written data,
  // but fail writing more.
  fd = os::open(testfile, O_RDONLY);
  ASSERT_SOME(fd);
  EXPECT_SOME_EQ(data, os::read(fd.get(), data.size()));
  EXPECT_ERROR(os::write(fd.get(), data));
  EXPECT_SOME(os::close(fd.get()));

  // `O_WRONLY` should be able to overwrite the data, but fail reading.
  fd = os::open(testfile, O_WRONLY);
  ASSERT_SOME(fd);
  EXPECT_SOME(os::write(fd.get(), data));
  EXPECT_ERROR(os::read(fd.get(), data.size()));
  EXPECT_SOME(os::close(fd.get()));

  // `O_APPEND` should write to an existing file.
  fd = os::open(testfile, O_APPEND | O_RDWR);
  ASSERT_SOME(fd);
  EXPECT_SOME_EQ(data, os::read(fd.get(), data.size()));
  EXPECT_SOME(os::write(fd.get(), data));
  const string datadata = "datadata";
  // Seek back to beginning to read the written data.
  EXPECT_SOME_EQ(0, os::lseek(fd.get(), 0, SEEK_SET));
  EXPECT_SOME_EQ(datadata, os::read(fd.get(), datadata.size()));
  EXPECT_SOME(os::close(fd.get()));

  // `O_TRUNC` should truncate an existing file.
  fd = os::open(testfile, O_TRUNC | O_RDWR);
  ASSERT_SOME(fd);
  EXPECT_NONE(os::read(fd.get(), 1));
  EXPECT_SOME(os::write(fd.get(), data));
  // Seek back to beginning to read the written data.
  EXPECT_SOME_EQ(0, os::lseek(fd.get(), 0, SEEK_SET));
  EXPECT_SOME_EQ(data, os::read(fd.get(), data.size()));
  EXPECT_SOME(os::close(fd.get()));

  // `O_CREAT | O_TRUNC` should create an empty file.
  const string testtruncfile =
    path::join(os::getcwd(), id::UUID::random().toString());
  fd = os::open(testtruncfile, O_CREAT | O_TRUNC | O_RDWR, S_IRWXU);
  ASSERT_SOME(fd);
  EXPECT_NONE(os::read(fd.get(), 1));
  EXPECT_SOME(os::write(fd.get(), data));
  // Seek back to beginning to read the written data.
  EXPECT_SOME_EQ(0, os::lseek(fd.get(), 0, SEEK_SET));
  EXPECT_SOME_EQ(data, os::read(fd.get(), data.size()));
  EXPECT_SOME(os::close(fd.get()));
}


TEST_F(FsTest, Close)
{
  const string testfile =
    path::join(os::getcwd(), id::UUID::random().toString());

  ASSERT_SOME(os::touch(testfile));
  ASSERT_TRUE(os::exists(testfile));

  const string test_message1 = "test1";
  const string error_message = "should not be written";

  // Open a file, and verify that writing to that file descriptor succeeds
  // before we close it, and fails after.
  const Try<int_fd> fd = os::open(testfile, O_CREAT | O_RDWR);
  ASSERT_SOME(fd);
#ifdef __WINDOWS__
  ASSERT_EQ(fd->type(), os::WindowsFD::Type::HANDLE);
  ASSERT_TRUE(fd->is_valid());
#endif // __WINDOWS__

  ASSERT_SOME(os::write(fd.get(), test_message1));

  EXPECT_SOME(os::close(fd.get()));

  EXPECT_ERROR(os::write(fd.get(), error_message));

  const Result<string> read = os::read(testfile);
  EXPECT_SOME(read);
  ASSERT_EQ(test_message1, read.get());

  // Try `close` with invalid file descriptor.
  // NOTE: This should work on both Windows and POSIX because the implicit
  // conversion to `int_fd` maps `-1` to `INVALID_HANDLE_VALUE` on Windows.
  EXPECT_ERROR(os::close(static_cast<int>(-1)));

#ifdef __WINDOWS__
  // Try `close` with invalid `HANDLE` and `SOCKET`.
  EXPECT_ERROR(os::close(int_fd(INVALID_HANDLE_VALUE)));
  EXPECT_ERROR(os::close(int_fd(INVALID_SOCKET)));
#endif // __WINDOWS__
}


#if defined(__linux__) || defined(__APPLE__)
// NOTE: This test is otherwise disabled since it uses `os::setxattr`
// and `os::getxattr` which are not available elsewhere.
TEST_F(FsTest, Xattr)
{
  const string file = path::join(os::getcwd(), id::UUID::random().toString());

  // Create file.
  ASSERT_SOME(os::touch(file));
  ASSERT_TRUE(os::exists(file));

  // Set an extended attribute.
  Try<Nothing> setxattr = os::setxattr(
      file,
      "user.mesos.test",
      "y",
      0);

  // Only run this test if extended attribute is supported.
  if (setxattr.isError() && setxattr.error() == os::strerror(ENOTSUP)) {
    return;
  }

  ASSERT_SOME(setxattr);

  // Get the extended attribute.
  Try<string> value = os::getxattr(file, "user.mesos.test");
  ASSERT_SOME(value);
  EXPECT_EQ(value.get(), "y");

  // Remove the extended attribute.
  ASSERT_SOME(os::removexattr(file, "user.mesos.test"));

  // Get the extended attribute again which should not exist.
  ASSERT_ERROR(os::getxattr(file, "user.mesos.test"));
}
#endif // __linux__ || __APPLE__


#ifdef __WINDOWS__
// Check if the overlapped field is set properly on Windows.
TEST_F(FsTest, Overlapped)
{
  const string testfile =
    path::join(sandbox.get(), id::UUID::random().toString());

  // Case 1: `os::open` should return non-overlapped handles.
  const Try<int_fd> fd1 = os::open(testfile, O_CREAT | O_TRUNC | O_RDWR);
  ASSERT_SOME(fd1);
  ASSERT_FALSE(fd1->is_overlapped());

  const Try<int_fd> fd2 = os::dup(fd1.get());
  ASSERT_SOME(fd2);
  ASSERT_FALSE(fd2->is_overlapped());

  EXPECT_SOME(os::close(fd1.get()));
  EXPECT_SOME(os::close(fd2.get()));

  // Case 2: `net::socket` should return overlapped handles.
  const Try<int_fd> socket1 = net::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_SOME(socket1);
  ASSERT_TRUE(socket1->is_overlapped());

  const Try<int_fd> socket2 = os::dup(socket1.get());
  ASSERT_SOME(socket2);
  ASSERT_TRUE(socket2->is_overlapped());

  EXPECT_SOME(os::close(socket1.get()));
  EXPECT_SOME(os::close(socket2.get()));

  // Case 3: `os::pipe` should return overlapped values depending on the
  // parameters given.
  const Try<std::array<int_fd, 2>> pipes = os::pipe(true, false);
  ASSERT_SOME(pipes);

  const int_fd pipe1 = pipes.get()[0];
  const int_fd pipe2 = pipes.get()[1];
  ASSERT_TRUE(pipe1.is_overlapped());
  ASSERT_FALSE(pipe2.is_overlapped());

  Try<int_fd> pipe3 = os::dup(pipe1);
  ASSERT_SOME(pipe3);
  ASSERT_TRUE(pipe3->is_overlapped());

  Try<int_fd> pipe4 = os::dup(pipe2);
  ASSERT_SOME(pipe4);
  ASSERT_FALSE(pipe4->is_overlapped());

  EXPECT_SOME(os::close(pipe1));
  EXPECT_SOME(os::close(pipe2));
  EXPECT_SOME(os::close(pipe3.get()));
  EXPECT_SOME(os::close(pipe4.get()));
}


TEST_F(FsTest, ReadWriteAsync)
{
  const Try<std::array<int_fd, 2>> pipes = os::pipe(true, true);
  ASSERT_SOME(pipes);

  OVERLAPPED read_overlapped = {};
  OVERLAPPED write_overlapped = {};
  std::vector<char> write_buffer(64, 'A');
  std::vector<char> read_buffer(write_buffer.size());

  // Do an async read. This should return that the IO is pending.
  const Result<size_t> result_read = os::read_async(
      pipes.get()[0],
      read_buffer.data(),
      read_buffer.size(),
      &read_overlapped);

  ASSERT_NONE(result_read);

  // Do an async write. This will return immediately.
  const Result<size_t> result_write = os::write_async(
      pipes.get()[1],
      write_buffer.data(),
      write_buffer.size(),
      &write_overlapped);

  ASSERT_SOME(result_write);

  // Wait for read to finish.
  DWORD bytes;
  ASSERT_EQ(
      TRUE,
      ::GetOverlappedResult(pipes.get()[0], &read_overlapped, &bytes, TRUE));

  ASSERT_GT(bytes, static_cast<DWORD>(0));
  ASSERT_EQ(result_write.get(), bytes);
  ASSERT_EQ(
      string(write_buffer.data(), result_write.get()),
      string(read_buffer.data(), bytes));

  EXPECT_SOME(os::close(pipes.get()[0]));
  EXPECT_SOME(os::close(pipes.get()[1]));
}


TEST_F(FsTest, ReadWriteAsyncLargeBuffer)
{
  const Try<std::array<int_fd, 2>> pipes = os::pipe(true, true);
  ASSERT_SOME(pipes);

  OVERLAPPED read_overlapped = {};
  OVERLAPPED write_overlapped = {};
  std::vector<char> write_buffer(1024 * 1024, 'A');
  std::vector<char> read_buffer(write_buffer.size());

  // This should return IO pending because it's larger than the internal pipe
  // buffer.
  const Result<size_t>  result_write = os::write_async(
      pipes.get()[1],
      write_buffer.data(),
      write_buffer.size(),
      &write_overlapped);

  ASSERT_NONE(result_write);

  // This should return immediately.
  const Result<size_t> result_read = os::read_async(
      pipes.get()[0],
      read_buffer.data(),
      read_buffer.size(),
      &read_overlapped);

  ASSERT_SOME(result_read);

  // Wait for write to finish.
  DWORD bytes;
  ASSERT_EQ(
      TRUE,
      ::GetOverlappedResult(pipes.get()[1], &write_overlapped, &bytes, TRUE));

  ASSERT_GT(bytes, static_cast<DWORD>(0));
  ASSERT_EQ(result_read.get(), bytes);
  ASSERT_EQ(
      string(write_buffer.data(), result_read.get()),
      string(read_buffer.data(), bytes));

  EXPECT_SOME(os::close(pipes.get()[0]));
  EXPECT_SOME(os::close(pipes.get()[1]));
}
#endif // __WINDOWS__


#ifndef __WINDOWS__
TEST_F(FsTest, Used)
{
  Try<Bytes> used = fs::used(".");
  ASSERT_SOME(used);

  Try<Bytes> size = fs::size(".");
  ASSERT_SOME(size);

  // We unfortunately can't easily verify the used value since
  // the disk usage can change at any point.

  EXPECT_GT(used.get(), 0u);
  EXPECT_LT(used.get(), size.get());
}


// This test verifies that the file descriptors returned by `os::lsof()`
// are all open file descriptors and contains stdin, stdout and stderr.
TEST_F(FsTest, Lsof)
{
  Try<std::vector<int_fd>> fds = os::lsof();
  ASSERT_SOME(fds);

  // Verify each `fd` is an open file descriptor.
  foreach (int_fd fd, fds.get()) {
    EXPECT_NE(-1, ::fcntl(fd, F_GETFD));
  }

  EXPECT_NE(std::find(fds->begin(), fds->end(), 0), fds->end());
  EXPECT_NE(std::find(fds->begin(), fds->end(), 1), fds->end());
  EXPECT_NE(std::find(fds->begin(), fds->end(), 2), fds->end());
}
#endif // __WINDOWS__
