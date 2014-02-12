#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <string>

#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>

using std::string;


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
