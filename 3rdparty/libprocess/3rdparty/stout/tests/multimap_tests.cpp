#include <stdint.h>

#include <gtest/gtest.h>

#include <set>
#include <string>

#include <stout/foreach.hpp>
#include <stout/multimap.hpp>
#include <stout/multihashmap.hpp>

using std::set;
using std::string;

template <typename T>
class MultimapTest : public ::testing::Test {};

typedef ::testing::Types<
  Multimap<string, uint16_t>, multihashmap<string, uint16_t> > MultimapTypes;

// Causes all TYPED_TEST(MultimapTest, ...) to be run for each of the
// specified multimap types.
TYPED_TEST_CASE(MultimapTest, MultimapTypes);


TYPED_TEST(MultimapTest, Put)
{
  typedef TypeParam Map;

  Map map;

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


TYPED_TEST(MultimapTest, Remove)
{
  typedef TypeParam Map;

  Map map;

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


TYPED_TEST(MultimapTest, Size)
{
  typedef TypeParam Map;

  Map map;

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


TYPED_TEST(MultimapTest, Keys)
{
  typedef TypeParam Map;

  Map map;

  map.put("foo", 1024);
  map.put("foo", 1024);
  map.put("foo", 1024);
  map.put("foo", 1025);
  map.put("bar", 1);

  set<string> keys = map.keys();

  ASSERT_EQ(2, keys.size());
  ASSERT_EQ(1, keys.count("foo"));
  ASSERT_EQ(1, keys.count("bar"));
}


TYPED_TEST(MultimapTest, Iterator)
{
  typedef TypeParam Map;

  Map map;

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());
  ASSERT_TRUE(map.contains("foo", 1024));
  ASSERT_TRUE(map.contains("foo", 1025));

  typename Map::iterator i = map.begin();

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


TYPED_TEST(MultimapTest, Foreach)
{
  typedef TypeParam Map;

  Map map;

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
      FAIL() << "Unexpected key/value in multimap";
    }
  }
}
