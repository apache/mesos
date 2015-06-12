#include <ctype.h>

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/functional/hash.hpp>

#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>

using std::string;


TEST(HashMapTest, InitializerList)
{
  hashmap<string, int> map{{"hello", 1}};
  EXPECT_EQ(1, map.size());

  EXPECT_TRUE((hashmap<int, int>{}.empty()));

  hashmap<int, int> map2{{1, 2}, {2, 3}, {3, 4}};
  EXPECT_EQ(3, map2.size());
  EXPECT_SOME_EQ(2, map2.get(1));
  EXPECT_SOME_EQ(3, map2.get(2));
  EXPECT_SOME_EQ(4, map2.get(3));
  EXPECT_NONE(map2.get(4));
}


TEST(HashMapTest, Insert)
{
  hashmap<string, int> map;
  map["abc"] = 1;
  map.put("def", 2);

  ASSERT_SOME_EQ(1, map.get("abc"));
  ASSERT_SOME_EQ(2, map.get("def"));

  map.put("def", 4);
  ASSERT_SOME_EQ(4, map.get("def"));
  ASSERT_EQ(2, map.size());
}


TEST(HashMapTest, Contains)
{
  hashmap<string, int> map;
  map["abc"] = 1;

  ASSERT_TRUE(map.contains("abc"));
  ASSERT_TRUE(map.containsValue(1));

  ASSERT_FALSE(map.contains("def"));
  ASSERT_FALSE(map.containsValue(2));
}


TEST(HashMapTest, CustomHashAndEqual)
{
  struct CaseInsensitiveHash
  {
    size_t operator () (const string& key) const
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
    bool operator () (const string& left, const string& right) const
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

  hashmap<string, int, CaseInsensitiveHash, CaseInsensitiveEqual> map;

  map["abc"] = 1;
  map.put("def", 2);
  EXPECT_SOME_EQ(1, map.get("Abc"));
  EXPECT_SOME_EQ(2, map.get("dEf"));

  map.put("Abc", 3);
  map["DEF"] = 4;
  EXPECT_SOME_EQ(3, map.get("abc"));
  EXPECT_SOME_EQ(4, map.get("def"));

  EXPECT_EQ(2, map.size());
  EXPECT_TRUE(map.contains("abc"));
  EXPECT_TRUE(map.contains("aBc"));
}
