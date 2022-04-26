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
#include <vector>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/linkedhashmap.hpp>

using std::string;
using std::vector;

TEST(LinkedHashmapTest, Put)
{
  LinkedHashMap<string, int> map;

  map["foo"] = 1;
  ASSERT_SOME_EQ(1, map.get("foo"));
  ASSERT_EQ(1u, map.size());

  map["bar"] = 2;
  ASSERT_SOME_EQ(2, map.get("bar"));
  ASSERT_EQ(2u, map.size());

  map["foo"] = 3;
  ASSERT_SOME_EQ(3, map.get("foo"));
  ASSERT_EQ(2u, map.size());
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
  ASSERT_EQ(2u, map.size());

  ASSERT_EQ(1u, map.erase("foo"));
  ASSERT_EQ(0u, map.erase("caz")); // Non-existent key.
  ASSERT_NONE(map.get("foo"));
  ASSERT_EQ(1u, map.size());
  ASSERT_SOME_EQ(2, map.get("bar"));
}


TEST(LinkedHashmapTest, Keys)
{
  LinkedHashMap<string, int> map;

  vector<string> keys = {"foo", "bar", "food", "rad", "cat"};

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
    ++val;
    ASSERT_EQ(val, value);
  }
}


TEST(LinkedHashMapTest, Foreach)
{
  LinkedHashMap<string, int> map;

  map["foo"] = 1;
  map["bar"] = 2;
  map["caz"] = 3;

  map["foo"] = 4; // Re-insert a key.

  vector<string> keyList = map.keys();
  vector<int> valueList = map.values();

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
// LinkedHashMap.
TEST(LinkedHashMapTest, ForeachConst)
{
  LinkedHashMap<string, int> map;

  map["foo"] = 1;
  map["bar"] = 2;
  map["caz"] = 3;

  const LinkedHashMap<string, int>& constMap = map;

  foreachkey (const string& key, constMap) {
    EXPECT_NE("qux", key);
  }
  foreachvalue (int value, constMap) {
    EXPECT_NE(0, value);
  }
}


TEST(LinkedHashMapTest, ForeachMutate)
{
  LinkedHashMap<int, string> map;

  map[1] = "foo";
  map[2] = "bar";
  map[3] = "caz";

  foreachpair (int key, string& value, map) {
    if (key == 2) {
      value = "qux";
    }
  }

  vector<string> values = {"foo", "qux", "caz"};
  EXPECT_EQ(values, map.values());
}


// TODO(bmahler): Simplify this test once LinkedHashMap
// has equality operators.
TEST(LinkedHashMapTest, CopyConstruction)
{
  LinkedHashMap<int, string> map;

  map[1] = "1";
  map[2] = "2";
  map[3] = "3";

  LinkedHashMap<int, string> copy(map);

  EXPECT_EQ(map.keys(), copy.keys());
  EXPECT_EQ(map.values(), copy.values());

  EXPECT_EQ(1u, map.erase(1));
  EXPECT_EQ(1u, copy.erase(1));

  EXPECT_EQ(map.keys(), copy.keys());
  EXPECT_EQ(map.values(), copy.values());

  copy[4] = "4";

  EXPECT_NE(map.keys(), copy.keys());
  EXPECT_NE(map.values(), copy.values());
}


// TODO(bmahler): Simplify this test once LinkedHashMap
// has equality operators.
TEST(LinkedHashMapTest, Assignment)
{
  LinkedHashMap<int, string> map;

  map[1] = "1";
  map[2] = "2";
  map[3] = "3";

  LinkedHashMap<int, string> copy;
  copy = map;

  EXPECT_EQ(map.keys(), copy.keys());
  EXPECT_EQ(map.values(), copy.values());

  EXPECT_EQ(1u, map.erase(1));
  EXPECT_EQ(1u, copy.erase(1));

  EXPECT_EQ(map.keys(), copy.keys());
  EXPECT_EQ(map.values(), copy.values());

  copy[4] = "4";

  EXPECT_NE(map.keys(), copy.keys());
  EXPECT_NE(map.values(), copy.values());

  // Test self-assignment.
  copy = map;
  map = map;  // NOLINT(clang-diagnostic-self-assign-overloaded)

  EXPECT_EQ(copy.keys(), map.keys());
  EXPECT_EQ(copy.values(), map.values());
}
