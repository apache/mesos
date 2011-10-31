#include <gtest/gtest.h>

#include <string>

#include "common/foreach.hpp"
#include "common/multihashmap.hpp"

using std::string;


TEST(Multihashmap, Put)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  ASSERT_EQ(1, map.get("foo").size());

  map.put("foo", 1025);
  ASSERT_EQ(2, map.get("foo").size());

  ASSERT_EQ(2, map.size());

  map.put("bar", 1024);
  ASSERT_EQ(1, map.get("bar").size());

  map.put("bar", 1025);
  ASSERT_EQ(2, map.get("bar").size());

  ASSERT_EQ(4, map.size());
}


TEST(Multihashmap, Remove)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.remove("foo", 1024);
  ASSERT_EQ(0, map.get("foo").size());

  ASSERT_EQ(0, map.size());

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2, map.get("foo").size());

  ASSERT_EQ(2, map.size());

  map.remove("foo");
  ASSERT_EQ(0, map.get("foo").size());
  ASSERT_EQ(0, map.size());
}


TEST(Multihashmap, Size)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2, map.get("foo").size());
  ASSERT_TRUE(map.contains("foo", 1024));
  ASSERT_TRUE(map.contains("foo", 1025));
  ASSERT_EQ(2, map.size());

  map.put("bar", 1024);
  map.put("bar", 1025);
  ASSERT_EQ(2, map.get("bar").size());
  ASSERT_TRUE(map.contains("bar", 1024));
  ASSERT_TRUE(map.contains("bar", 1025));
  ASSERT_EQ(4, map.size());
}


TEST(Multihashmap, Iterator)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2, map.get("foo").size());
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


TEST(Multihashmap, Foreach)
{
  multihashmap<string, uint16_t> map;

  map.put("foo", 1024);
  map.put("bar", 1025);
  ASSERT_EQ(1, map.get("foo").size());
  ASSERT_EQ(1, map.get("bar").size());
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
