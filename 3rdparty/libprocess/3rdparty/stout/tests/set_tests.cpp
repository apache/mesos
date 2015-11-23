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

#include <set>

#include <gtest/gtest.h>

#include <stout/set.hpp>

TEST(SetTest, Set)
{
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1) | Set<int>(2));
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1, 2) | Set<int>(1));
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1) + 2);
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1, 2) + 2);
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1, 2, 3) & Set<int>(1, 2));
  EXPECT_EQ(Set<int>(1, 2), Set<int>(1, 2) & Set<int>(1, 2));

  Set<int> left;
  left.insert(2);
  left.insert(4);

  Set<int> right;
  right.insert(1);
  right.insert(3);

  EXPECT_EQ(Set<int>(1, 2, 3, 4), left | right);
  EXPECT_EQ(Set<int>(), left & right);

  std::set<int> s = left;

  EXPECT_EQ(Set<int>(2, 4, 6), s + 6);
}
