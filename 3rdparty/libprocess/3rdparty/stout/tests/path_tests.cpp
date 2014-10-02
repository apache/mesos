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
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <stout/path.hpp>

using std::string;
using std::vector;


TEST(PathTest, Join)
{
  EXPECT_EQ("a/b/c", path::join("a", "b", "c"));
  EXPECT_EQ("a/b/c", path::join(string("a"), string("b"), string("c")));
  EXPECT_EQ("a/b/c", path::join(string("a"), "b", string("c")));
  EXPECT_EQ("a/b/c", path::join(vector<string>({"a", "b", "c"})));
  EXPECT_EQ("", path::join(vector<string>()));
  EXPECT_EQ("", path::join(vector<string>({"", "", ""})));
  EXPECT_EQ("/asdf", path::join("/", "asdf"));
  EXPECT_EQ("/", path::join("", "/", ""));
  EXPECT_EQ("a/b/c", path::join("a/", "b/", "c/"));
  EXPECT_EQ("/a/b/c", path::join("/a", "/b", "/c"));
  EXPECT_EQ("/a/b/c", path::join("/a/", "/b/", "/c/"));
  EXPECT_EQ("a/b/c", path::join("a/", "/b/", "/c/"));
}
