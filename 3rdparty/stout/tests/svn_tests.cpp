// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdlib> // For rand().
#include <string>

#include <gtest/gtest.h>

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>
#include <stout/svn.hpp>

using std::string;


TEST(SVNTest, DiffPatch)
{
  string source;

  while (Bytes(source.size()) < Megabytes(1)) {
    source += (char) rand() % 256;
  }

  // Make the target string have 512 different bytes in the middle.
  string target = source;

  for (size_t index = 0; index < 512; index++) {
    target[1024 + index] = (char) rand() % 256;
  }

  ASSERT_NE(source, target);

  Try<svn::Diff> diff = svn::diff(source, target);

  ASSERT_SOME(diff);

  Try<string> result = svn::patch(source, diff.get());

  ASSERT_SOME_EQ(target, result);
  ASSERT_SOME_NE(source, result);
}


TEST(SVNTest, EmptyDiffPatch)
{
  string source;

  while (Bytes(source.size()) < Megabytes(1)) {
    source += (char) rand() % 256;
  }

  // Make the target string equal the source string.
  string target = source;

  Try<svn::Diff> diff = svn::diff(source, target);

  ASSERT_SOME(diff);

  Try<string> result = svn::patch(source, diff.get());

  ASSERT_SOME_EQ(target, result);
  ASSERT_SOME_EQ(source, result);
}
