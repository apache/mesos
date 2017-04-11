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

#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/hashset.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include <stout/os/find.hpp>
#include <stout/os/getcwd.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/read.hpp>
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
  const string testdir = path::join(os::getcwd(), UUID::random().toString());
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
  const string testfile  = path::join(os::getcwd(), UUID::random().toString());
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
  const string testfile  = path::join(os::getcwd(), UUID::random().toString());

  ASSERT_SOME(os::touch(testfile));
  ASSERT_TRUE(os::exists(testfile));
}


TEST_F(FsTest, SYMLINK_Symlink)
{
  const string temp_path = os::getcwd();
  const string link = path::join(temp_path, "sym.link");
  const string file = path::join(temp_path, UUID::random().toString());

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
  const string testdir = path::join(os::getcwd(), UUID::random().toString());
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
  EXPECT_EQ(3u, allFiles.get().size());

  // Search .jpg files in folder
  Try<list<string>> jpgFiles = fs::list(path::join(testdir, "*.jpg"));
  ASSERT_SOME(jpgFiles);
  EXPECT_EQ(1u, jpgFiles.get().size());

  // Search test*.txt files in folder
  Try<list<string>> testTxtFiles = fs::list(path::join(testdir, "*.txt"));
  ASSERT_SOME(testTxtFiles);
  EXPECT_EQ(2u, testTxtFiles.get().size());

  // Verify that we return empty list when we provide an invalid path.
  Try<list<string>> noFiles = fs::list("this_path_does_not_exist");
  ASSERT_SOME(noFiles);
  EXPECT_EQ(0u, noFiles.get().size());
}


TEST_F(FsTest, Rename)
{
  const string testdir = path::join(os::getcwd(), UUID::random().toString());
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
  EXPECT_EQ(2u, allFiles.get().size());

  // Rename a `file1` to `file3`, which does not exist yet. Verify `file3`
  // contains the text that was in `file1`, and make sure the count of files in
  // the directory has stayed the same.
  EXPECT_SOME(os::rename(file1, file3));

  Try<string> file3Contents = os::read(file3);
  ASSERT_SOME(file3Contents);
  EXPECT_EQ(message, file3Contents.get());

  allFiles = fs::list(path::join(testdir, "*"));
  ASSERT_SOME(allFiles);
  EXPECT_EQ(2u, allFiles.get().size());

  // Rename `file3` -> `file2`. `file2` exists, so this will replace it. Verify
  // text in the file, and that the count of files in the directory have gone
  // down.
  EXPECT_SOME(os::rename(file3, file2));
  Try<string> file2Contents = os::read(file2);
  ASSERT_SOME(file2Contents);
  EXPECT_EQ(message, file2Contents.get());

  allFiles = fs::list(path::join(testdir, "*"));
  ASSERT_SOME(allFiles);
  EXPECT_EQ(1u, allFiles.get().size());

  // Rename a fake file, verify failure.
  const string fakeFile = testdir + "does_not_exist";
  EXPECT_ERROR(os::rename(fakeFile, file1));

  EXPECT_FALSE(os::exists(file1));

  allFiles = fs::list(path::join(testdir, "*"));
  ASSERT_SOME(allFiles);
  EXPECT_EQ(1u, allFiles.get().size());
}


TEST_F(FsTest, Close)
{
#ifdef __WINDOWS__
  // On Windows, CRT functions like `_close` will cause an assert dialog box
  // to pop up if you pass them a bad file descriptor. For this test, we prefer
  // to just have the functions error out.
  const int previous_report_mode = _CrtSetReportMode(_CRT_ASSERT, 0);
#endif // __WINDOWS__

  const string testfile = path::join(os::getcwd(), UUID::random().toString());

  ASSERT_SOME(os::touch(testfile));
  ASSERT_TRUE(os::exists(testfile));

  const string test_message1 = "test1";
  const string error_message = "should not be written";

  // Open a file, and verify that writing to that file descriptor succeeds
  // before we close it, and fails after.
  const Try<int_fd> open_valid_fd = os::open(testfile, O_RDWR);
  ASSERT_SOME(open_valid_fd);
  ASSERT_SOME(os::write(open_valid_fd.get(), test_message1));

  EXPECT_SOME(os::close(open_valid_fd.get()));

  EXPECT_ERROR(os::write(open_valid_fd.get(), error_message));

  const Result<string> read_valid_fd = os::read(testfile);
  EXPECT_SOME(read_valid_fd);
  ASSERT_EQ(test_message1, read_valid_fd.get());

#ifdef __WINDOWS__
  // Open a file with the traditional Windows `HANDLE` API, then verify that
  // writing to that `HANDLE` succeeds before we close it, and fails after.
  const HANDLE open_valid_handle = CreateFile(
      testfile.c_str(),
      FILE_APPEND_DATA,
      0,                     // No sharing mode.
      nullptr,               // Default security.
      OPEN_EXISTING,         // Open only if it exists.
      FILE_ATTRIBUTE_NORMAL, // Open a normal file.
      nullptr);              // No attribute tempate file.
  ASSERT_NE(INVALID_HANDLE_VALUE, open_valid_handle);

  DWORD bytes_written;
  BOOL written = WriteFile(
      open_valid_handle,
      test_message1.c_str(),                     // Data to write.
      static_cast<DWORD>(test_message1.size()),  // Bytes to write.
      &bytes_written,                            // Bytes written.
      nullptr);                                  // No overlapped I/O.
  ASSERT_TRUE(written == TRUE);
  ASSERT_EQ(test_message1.size(), bytes_written);

  EXPECT_SOME(os::close(open_valid_handle));

  written = WriteFile(
      open_valid_handle,
      error_message.c_str(),                     // Data to write.
      static_cast<DWORD>(error_message.size()),  // Bytes to write.
      &bytes_written,                            // Bytes written.
      nullptr);                                  // No overlapped I/O.
  ASSERT_TRUE(written == FALSE);
  ASSERT_EQ(0, bytes_written);

  const Result<string> read_valid_handle = os::read(testfile);
  EXPECT_SOME(read_valid_handle);
  ASSERT_EQ(test_message1 + test_message1, read_valid_handle.get());
#endif // __WINDOWS__

  // Try `close` with invalid file descriptor.
  EXPECT_ERROR(os::close(static_cast<int>(-1)));

#ifdef __WINDOWS__
  // Try `close` with invalid `SOCKET` and `HANDLE`.
  EXPECT_ERROR(os::close(static_cast<SOCKET>(INVALID_SOCKET)));
  EXPECT_ERROR(os::close(INVALID_SOCKET));
  EXPECT_ERROR(os::close(static_cast<HANDLE>(open_valid_handle)));
#endif // __WINDOWS__

#ifdef __WINDOWS__
  // Reset the CRT assert dialog settings.
  _CrtSetReportMode(_CRT_ASSERT, previous_report_mode);
#endif // __WINDOWS__
}


#if defined(__linux__) || defined(__APPLE__)
TEST_F(FsTest, Xattr)
{
  const string file = path::join(os::getcwd(), UUID::random().toString());

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
