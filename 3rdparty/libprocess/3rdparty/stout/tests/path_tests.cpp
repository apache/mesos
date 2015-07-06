/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <stout/path.hpp>

#include <stout/tests/utils.hpp>

using std::string;
using std::vector;


// Test many corner cases of Path::basename.
TEST(PathTest, Basename)
{
  // Empty path check.
  EXPECT_EQ(".", Path("").basename());

  // Check common path patterns.
  EXPECT_EQ("/", Path("/").basename());
  EXPECT_EQ(".", Path(".").basename());
  EXPECT_EQ("..", Path("..").basename());

  EXPECT_EQ("a", Path("a").basename());
  EXPECT_EQ("b", Path("a/b").basename());
  EXPECT_EQ("c", Path("a/b/c").basename());

  // Check leading slashes get cleaned up properly.
  EXPECT_EQ("a", Path("/a").basename());
  EXPECT_EQ("a", Path("//a").basename());
  EXPECT_EQ("a", Path("/a/").basename());
  EXPECT_EQ("c", Path("/a/b/c").basename());
  EXPECT_EQ("b", Path("/a/b").basename());
  EXPECT_EQ("b", Path("//a//b").basename());

  // Check trailing slashes get cleaned up properly.
  EXPECT_EQ("a", Path("a/").basename());
  EXPECT_EQ("c", Path("/a/b/c//").basename());
  EXPECT_EQ("c", Path("/a/b/c///").basename());
  EXPECT_EQ("/", Path("//").basename());
  EXPECT_EQ("/", Path("///").basename());
}


// Test many corner cases of Path::dirname.
TEST(PathTest, Dirname)
{
  // Empty path check.
  EXPECT_EQ(".", Path("").dirname());

  // Check common path patterns.
  EXPECT_EQ("/", Path("/").dirname());
  EXPECT_EQ(".", Path(".").dirname());
  EXPECT_EQ(".", Path("..").dirname());

  EXPECT_EQ(".", Path("a").dirname());
  EXPECT_EQ("a", Path("a/b").dirname());
  EXPECT_EQ("a/b", Path("a/b/c/").dirname());

  // Check leading slashes get cleaned up properly.
  EXPECT_EQ("/", Path("/a").dirname());
  EXPECT_EQ("/", Path("//a").dirname());
  EXPECT_EQ("/", Path("/a/").dirname());
  EXPECT_EQ("/a", Path("/a/b").dirname());
  EXPECT_EQ("//a", Path("//a//b").dirname());
  EXPECT_EQ("/a/b", Path("/a/b/c").dirname());

  // Check intermittent slashes get handled just like ::dirname does.
  EXPECT_EQ("/a//b", Path("/a//b//c//").dirname());
  EXPECT_EQ("//a/b", Path("//a/b//c").dirname());

  // Check trailing slashes get cleaned up properly.
  EXPECT_EQ(".", Path("a/").dirname());
  EXPECT_EQ("a/b", Path("a/b/c").dirname());
  EXPECT_EQ("/a/b", Path("/a/b/c/").dirname());
  EXPECT_EQ("/a/b", Path("/a/b/c//").dirname());
  EXPECT_EQ("/a/b", Path("/a/b/c///").dirname());
  EXPECT_EQ("/", Path("//").dirname());
  EXPECT_EQ("/", Path("///").dirname());
}


TEST(PathTest, Join)
{
  EXPECT_EQ("a/b/c", path::join("a", "b", "c"));
  EXPECT_EQ("/a/b/c", path::join("/a", "b", "c"));

  EXPECT_EQ("", path::join(vector<string>()));
  EXPECT_EQ("a/b/c", path::join(vector<string>({"a", "b", "c"})));

  //TODO(cmaloney): This should join to ""
  EXPECT_EQ("/", path::join(vector<string>({"", "", ""})));

  // Interesting corner cases around being the first, middle, last.
  EXPECT_EQ("/asdf", path::join("/", "asdf"));
  EXPECT_EQ("/", path::join("", "/", ""));
  EXPECT_EQ("ab/", path::join("ab/", "", "/"));
  EXPECT_EQ("/ab", path::join("/", "/", "ab"));
  EXPECT_EQ("ab/", path::join("ab", "/", "/"));
  EXPECT_EQ("/ab", path::join("/", "", "/ab"));

  // Check trailing and leading slashes get cleaned up.
  EXPECT_EQ("a/b/c/", path::join("a/", "b/", "c/"));
  EXPECT_EQ("/a/b/c", path::join("/a", "/b", "/c"));
  EXPECT_EQ("/a/b/c/", path::join("/a/", "/b/", "/c/"));
  EXPECT_EQ("a/b/c/", path::join("a/", "/b/", "/c/"));
}


class PathFileTest : public TemporaryDirectoryTest {};


TEST_F(PathFileTest, ImplicitConversion)
{
  // Should be implicitly converted to string for the various os::_ calls.
  const Path testfile(path::join(os::getcwd(), "file.txt"));

  // Create the test file.
  ASSERT_SOME(os::touch(testfile));
  ASSERT_TRUE(os::exists(testfile));

  // Open and close the file.
  Try<int> fd = os::open(
      testfile,
      O_RDONLY,
      S_IRUSR | S_IRGRP | S_IROTH);
  ASSERT_SOME(fd);
  close(fd.get());

  // Delete the file.
  EXPECT_SOME(os::rm(testfile));
}
