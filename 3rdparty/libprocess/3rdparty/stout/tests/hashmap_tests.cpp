#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <string>

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
