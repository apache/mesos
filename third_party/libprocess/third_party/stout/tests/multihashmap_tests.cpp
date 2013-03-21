#include <stdint.h>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <string>

#include <stout/multihashmap.hpp>

using std::string;


TEST(MultihashmapTest, Put)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  ASSERT_EQ(1u, map.get("foo").size());

  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());

  ASSERT_EQ(2u, map.size());

  map.put("bar", 1024);
  ASSERT_EQ(1u, map.get("bar").size());

  map.put("bar", 1025);
  ASSERT_EQ(2u, map.get("bar").size());

  ASSERT_EQ(4u, map.size());
}


TEST(MultihashmapTest, Remove)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.remove("foo", 1024);
  ASSERT_EQ(0u, map.get("foo").size());

  ASSERT_EQ(0u, map.size());

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());

  ASSERT_EQ(2u, map.size());

  map.remove("foo");
  ASSERT_EQ(0u, map.get("foo").size());
  ASSERT_EQ(0u, map.size());
}


TEST(MultihashmapTest, Size)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());
  ASSERT_TRUE(map.contains("foo", 1024));
  ASSERT_TRUE(map.contains("foo", 1025));
  ASSERT_EQ(2u, map.size());

  map.put("bar", 1024);
  map.put("bar", 1025);
  ASSERT_EQ(2u, map.get("bar").size());
  ASSERT_TRUE(map.contains("bar", 1024));
  ASSERT_TRUE(map.contains("bar", 1025));
  ASSERT_EQ(4u, map.size());
}


TEST(MultihashmapTest, Iterator)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());
  ASSERT_TRUE(map.contains("foo", 1024));
  ASSERT_TRUE(map.contains("foo", 1025));

  multihashmap<string, uint16_t>::iterator i = map.begin();

  ASSERT_TRUE(i != map.end());

  ASSERT_EQ("foo", i->first);
  ASSERT_EQ(1024, i->second);

  ++i;
  ASSERT_TRUE(i != map.end());

  ASSERT_EQ("foo", i->first);
  ASSERT_EQ(1025, i->second);

  ++i;
  ASSERT_TRUE(i == map.end());
}


TEST(MultihashmapTest, Foreach)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("bar", 1025);
  ASSERT_EQ(1u, map.get("foo").size());
  ASSERT_EQ(1u, map.get("bar").size());
  ASSERT_TRUE(map.contains("foo", 1024));
  ASSERT_TRUE(map.contains("bar", 1025));

  foreachpair (const string& key, uint16_t value, map) {
    if (key == "foo") {
      ASSERT_EQ(1024, value);
    } else if (key == "bar") {
      ASSERT_EQ(1025, value);
    } else {
      FAIL() << "Unexpected key/value in multihashmap";
    }
  }
}
