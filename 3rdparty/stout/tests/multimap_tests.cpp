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

#include <stdint.h>

#include <set>
#include <string>

#include <gtest/gtest.h>

#include <stout/foreach.hpp>
#include <stout/multihashmap.hpp>
#include <stout/multimap.hpp>

using std::set;
using std::string;

template <typename T>
class MultimapTest : public ::testing::Test {};

typedef ::testing::Types<
  Multimap<string, uint16_t>, multihashmap<string, uint16_t>> MultimapTypes;

// Causes all TYPED_TEST(MultimapTest, ...) to be run for each of the
// specified multimap types.
TYPED_TEST_CASE(MultimapTest, MultimapTypes);


// Tests construction of multimaps passing initializer lists as parameters.
TYPED_TEST(MultimapTest, InitializerList)
{
  typedef TypeParam Map;

  Map map1({{"hello", 1}, {"Hello", 2}});
  EXPECT_EQ(2u, map1.size());

  EXPECT_TRUE(Map{}.empty());

  Map map2(
    {{"foo", 102}, {"foo", 103}, {"bar", 102}, {"bar", 103}, {"baz", 1}});
  ASSERT_EQ(2u, map2.get("foo").size());
  ASSERT_EQ(2u, map2.get("bar").size());
  ASSERT_EQ(1u, map2.get("baz").size());
  ASSERT_EQ(5u, map2.size());
}


// Tests conversion from std::multimap to our multimap type.
TYPED_TEST(MultimapTest, FromMultimap)
{
  typedef TypeParam Map;

  Multimap<typename Map::key_type, typename Map::mapped_type> map1(
    {{"foo", 102}, {"foo", 103}, {"bar", 102}, {"bar", 103}, {"baz", 1}});

  Map map2(map1);

  ASSERT_EQ(2u, map2.get("foo").size());
  ASSERT_EQ(2u, map2.get("bar").size());
  ASSERT_EQ(1u, map2.get("baz").size());
  ASSERT_EQ(5u, map2.size());
}


// Tests move constructor from std::multimap.
TYPED_TEST(MultimapTest, FromRValueMultimap)
{
  typedef TypeParam Map;

  Multimap<typename Map::key_type, typename Map::mapped_type> map1(
    {{"foo", 102}, {"foo", 103}, {"bar", 102}, {"bar", 103}, {"baz", 1}});

  Map map2(std::move(map1));

  ASSERT_EQ(2u, map2.get("foo").size());
  ASSERT_EQ(2u, map2.get("bar").size());
  ASSERT_EQ(1u, map2.get("baz").size());
  ASSERT_EQ(5u, map2.size());
}


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
  ASSERT_TRUE(map.get("foo").empty());

  ASSERT_TRUE(map.empty());

  map.put("foo", 1024);
  map.put("foo", 1025);
  ASSERT_EQ(2u, map.get("foo").size());

  ASSERT_EQ(2u, map.size());

  map.remove("foo");
  ASSERT_TRUE(map.get("foo").empty());
  ASSERT_TRUE(map.empty());
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

  ASSERT_EQ(2u, keys.size());
  ASSERT_EQ(1u, keys.count("foo"));
  ASSERT_EQ(1u, keys.count("bar"));
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
