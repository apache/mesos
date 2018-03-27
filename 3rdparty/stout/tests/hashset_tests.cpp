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

#include <string>
#include <utility>

#include <boost/functional/hash.hpp>

#include <stout/hashset.hpp>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

using std::string;


TEST(HashsetTest, InitializerList)
{
  hashset<string> set{"hello"};
  EXPECT_EQ(1u, set.size());

  EXPECT_TRUE((hashset<int>{}.empty()));

  hashset<int> set1{1, 3, 5, 7, 11};
  EXPECT_EQ(5u, set1.size());
  EXPECT_TRUE(set1.contains(1));
  EXPECT_TRUE(set1.contains(3));
  EXPECT_TRUE(set1.contains(5));
  EXPECT_TRUE(set1.contains(7));
  EXPECT_TRUE(set1.contains(11));

  EXPECT_FALSE(set1.contains(2));
}


TEST(HashsetTest, FromStdSet)
{
  std::set<int> set1{1, 3, 5, 7};

  hashset<int> set2(set1);

  EXPECT_EQ(set1.size(), set2.size());
  EXPECT_EQ(4u, set2.size());

  foreach (const auto set1_entry, set1) {
    EXPECT_TRUE(set2.contains(set1_entry));
  }
}


TEST(HashsetTest, FromRValueStdSet)
{
  std::set<int> set1{1, 3};

  hashset<int> set2(std::move(set1));

  EXPECT_EQ(2u, set2.size());

  EXPECT_TRUE(set2.contains(1));
  EXPECT_TRUE(set2.contains(3));

  EXPECT_FALSE(set2.contains(2));
}


TEST(HashsetTest, CustomHashAndEqual)
{
  struct CaseInsensitiveHash
  {
    size_t operator()(const string& key) const
    {
      size_t seed = 0;
      foreach (const char c, key) {
        boost::hash_combine(seed, ::tolower(c));
      }
      return seed;
    }
  };

  struct CaseInsensitiveEqual
  {
    bool operator()(const string& left, const string& right) const
    {
      if (left.size() != right.size()) {
        return false;
      }
      for (size_t i = 0; i < left.size(); ++i) {
        if (::tolower(left[i]) != ::tolower(right[i])) {
          return false;
        }
      }
      return true;
    }
  };

  hashset<string, CaseInsensitiveHash, CaseInsensitiveEqual> set;

  set.insert("abc");
  set.insert("def");
  EXPECT_TRUE(set.contains("Abc"));
  EXPECT_TRUE(set.contains("dEf"));

  EXPECT_EQ(2u, set.size());
  set.insert("Abc");
  set.insert("DEF");
  EXPECT_EQ(2u, set.size());
  EXPECT_TRUE(set.contains("abc"));
  EXPECT_TRUE(set.contains("def"));
}


TEST(HashsetTest, Insert)
{
  hashset<string> hs1;
  hs1.insert(string("HS1"));
  hs1.insert(string("HS3"));

  hashset<string> hs2;
  hs2.insert(string("HS2"));

  hs1 = hs2;
  ASSERT_EQ(1u, hs1.size());
  ASSERT_TRUE(hs1.contains("HS2"));
  ASSERT_TRUE(hs1 == hs2);
}


TEST(HashsetTest, Union)
{
  hashset<int> hs1;
  hs1.insert(1);
  hs1.insert(2);
  hs1.insert(3);

  hashset<int> hs2;
  hs2.insert(3);
  hs2.insert(4);
  hs2.insert(5);

  hashset<int> hs3 = hs1 | hs2;

  ASSERT_EQ(5u, hs3.size());
  ASSERT_TRUE(hs3.contains(1));
  ASSERT_TRUE(hs3.contains(2));
  ASSERT_TRUE(hs3.contains(3));
  ASSERT_TRUE(hs3.contains(4));
  ASSERT_TRUE(hs3.contains(5));

  hashset<int> hs4 = hs1;
  hs4 |= hs2;

  ASSERT_EQ(hs3, hs4);
}


TEST(HashsetTest, Difference)
{
  hashset<int> hs1;
  hs1.insert(1);
  hs1.insert(2);
  hs1.insert(3);

  hashset<int> hs2;
  hs2.insert(3);
  hs2.insert(4);
  hs2.insert(5);

  hashset<int> hs3 = hs1 - hs2;

  ASSERT_EQ(2u, hs3.size());
  ASSERT_TRUE(hs3.contains(1));
  ASSERT_TRUE(hs3.contains(2));
  ASSERT_FALSE(hs3.contains(3));
  ASSERT_FALSE(hs3.contains(4));
  ASSERT_FALSE(hs3.contains(5));

  hashset<int> hs4 = hs1;
  hs4 -= hs2;

  ASSERT_EQ(hs3, hs4);
}
