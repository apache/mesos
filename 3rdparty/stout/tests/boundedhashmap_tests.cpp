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

#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <stout/boundedhashmap.hpp>
#include <stout/gtest.hpp>

using std::list;
using std::string;
using std::vector;

TEST(BoundedHashMapTest, Set)
{
  BoundedHashMap<string, int> map(2);

  map.set("foo", 1);
  EXPECT_SOME_EQ(1, map.get("foo"));
  EXPECT_EQ(1u, map.size());

  map.set("bar", 2);
  EXPECT_SOME_EQ(2, map.get("bar"));
  EXPECT_EQ(2u, map.size());

  // This should evict key "foo".
  map.set("baz", 3);
  EXPECT_SOME_EQ(3, map.get("baz"));
  EXPECT_SOME_EQ(2, map.get("bar"));
  EXPECT_NONE(map.get("foo"));
  EXPECT_EQ(2u, map.size());

  // Update key "bar"; this should not change the insertion order.
  map.set("bar", 4);
  EXPECT_SOME_EQ(4, map.get("bar"));
  EXPECT_EQ(2u, map.size());

  // This should evict key "bar".
  map.set("quux", 5);
  EXPECT_SOME_EQ(5, map.get("quux"));
  EXPECT_SOME_EQ(3, map.get("baz"));
  EXPECT_NONE(map.get("bar"));
  EXPECT_EQ(2u, map.size());
}


TEST(BoundedHashMapTest, EmptyMap)
{
  BoundedHashMap<string, int> map(0);

  map.set("foo", 1);
  EXPECT_NONE(map.get("foo"));
  EXPECT_TRUE(map.empty());

  map.set("bar", 1);
  EXPECT_NONE(map.get("bar"));
  EXPECT_TRUE(map.empty());
}


TEST(BoundedHashmapTest, Contains)
{
  BoundedHashMap<string, int> map(2);

  map.set("foo", 1);
  map.set("bar", 2);

  ASSERT_TRUE(map.contains("foo"));
  ASSERT_TRUE(map.contains("bar"));
  ASSERT_FALSE(map.contains("caz"));
}


TEST(BoundedHashmapTest, Erase)
{
  BoundedHashMap<string, int> map(2);

  map.set("foo", 1);
  map.set("bar", 2);
  ASSERT_EQ(2u, map.size());

  ASSERT_EQ(1u, map.erase("foo"));
  ASSERT_EQ(0u, map.erase("caz")); // Non-existent key.
  ASSERT_NONE(map.get("foo"));
  ASSERT_EQ(1u, map.size());
  ASSERT_SOME_EQ(2, map.get("bar"));
}


TEST(BoundedHashmapTest, Keys)
{
  BoundedHashMap<string, int> map(5);

  list<string> keys = {"foo", "bar", "food", "rad", "cat"};

  // Insert keys into the map.
  foreach (const string& key, keys) {
    map.set(key, 1);
  }
  map.set("foo", 1); // Re-insert a key.

  // Ensure the keys returned are the same as insertion order.
  ASSERT_EQ(keys, map.keys());
}


TEST(BoundedHashmapTest, Values)
{
  BoundedHashMap<string, int> map(3);

  map.set("foo", 1);
  map.set("bar", 2);
  map.set("caz", 3);

  int val = 0;
  foreach (int value, map.values()) {
    ++val;
    ASSERT_EQ(val, value);
  }
}


TEST(BoundedHashMapTest, Foreach)
{
  BoundedHashMap<string, int> map(3);

  map.set("foo", 1);
  map.set("bar", 2);
  map.set("caz", 3);

  map.set("foo", 4); // Re-insert a key.

  list<string> keyList = map.keys();
  list<int> valueList = map.values();

  vector<string> keys{keyList.begin(), keyList.end()};
  vector<int> values{valueList.begin(), valueList.end()};

  {
    int i = 0;
    foreachpair (const string& key, int value, map) {
      EXPECT_EQ(keys[i], key);
      EXPECT_EQ(values[i], value);
      i++;
    }
  }

  {
    int i = 0;
    foreachkey (const string& key, map) {
      EXPECT_EQ(keys[i], key);
      i++;
    }
  }

  {
    int i = 0;
    foreachvalue (int value, map) {
      EXPECT_EQ(values[i], value);
      i++;
    }
  }
}


// Check that `foreach`-style loops can be used with a const ref to
// BoundedHashMap.
TEST(BoundedHashMapTest, ForeachConst)
{
  BoundedHashMap<string, int> map(3);

  map.set("foo", 1);
  map.set("bar", 2);
  map.set("caz", 3);

  const BoundedHashMap<string, int>& constMap = map;

  foreachkey (const string& key, constMap) {
    EXPECT_NE("qux", key);
  }
  foreachvalue (int value, constMap) {
    EXPECT_NE(0, value);
  }
}


TEST(BoundedHashMapTest, ForeachMutate)
{
  BoundedHashMap<int, string> map(3);

  map.set(1, "foo");
  map.set(2, "bar");
  map.set(3, "caz");

  foreachpair (int key, string& value, map) {
    if (key == 2) {
      value = "qux";
    }
  }

  list<string> values = {"foo", "qux", "caz"};
  EXPECT_EQ(values, map.values());
}
