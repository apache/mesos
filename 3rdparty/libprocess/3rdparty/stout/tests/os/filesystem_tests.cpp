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
#include <stout/os/ls.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/read.hpp>
#include <stout/os/rename.hpp>
#include <stout/os/touch.hpp>
#include <stout/os/write.hpp>

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


TEST_F(FsTest, Touch)
{
  const string testfile  = path::join(os::getcwd(), UUID::random().toString());

  ASSERT_SOME(os::touch(testfile));
  ASSERT_TRUE(os::exists(testfile));
}

TEST_F(FsTest, Symlink)
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
