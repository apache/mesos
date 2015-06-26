#include <stdint.h>

#include <list>
#include <string>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/linkedhashmap.hpp>

using std::list;
using std::string;

TEST(LinkedHashmapTest, Put)
{
  LinkedHashMap<string, int> map;

  map["foo"] = 1;
  ASSERT_SOME_EQ(1, map.get("foo"));
  ASSERT_EQ(1, map.size());

  map["bar"] = 2;
  ASSERT_SOME_EQ(2, map.get("bar"));
  ASSERT_EQ(2, map.size());

  map["foo"] = 3;
  ASSERT_SOME_EQ(3, map.get("foo"));
  ASSERT_EQ(2, map.size());
}


TEST(LinkedHashmapTest, Contains)
{
  LinkedHashMap<string, int> map;
  map["foo"] = 1;
  map["bar"] = 2;
  ASSERT_TRUE(map.contains("foo"));
  ASSERT_TRUE(map.contains("bar"));
  ASSERT_FALSE(map.contains("caz"));
}


TEST(LinkedHashmapTest, Erase)
{
  LinkedHashMap<string, int> map;

  map["foo"] = 1;
  map["bar"] = 2;
  ASSERT_EQ(2, map.size());

  ASSERT_EQ(1, map.erase("foo"));
  ASSERT_EQ(0, map.erase("caz")); // Non-existent key.
  ASSERT_NONE(map.get("foo"));
  ASSERT_EQ(1, map.size());
  ASSERT_SOME_EQ(2, map.get("bar"));
}


TEST(LinkedHashmapTest, Keys)
{
  LinkedHashMap<string, int> map;

  std::list<string> keys;
  keys.push_back("foo");
  keys.push_back("bar");
  keys.push_back("food");
  keys.push_back("rad");
  keys.push_back("cat");

  // Insert keys into the map.
  foreach (const string& key, keys) {
    map[key] = 1;
  }
  map["foo"] = 1; // Re-insert a key.

  // Ensure the keys returned are the same as insertion order.
  ASSERT_EQ(keys, map.keys());
}


TEST(LinkedHashmapTest, Values)
{
  LinkedHashMap<string, int> map;

  map["foo"] = 1;
  map["bar"] = 2;
  map["caz"] = 3;

  int val = 0;
  foreach (int value, map.values()) {
    ASSERT_EQ(++val, value);
  }
}
