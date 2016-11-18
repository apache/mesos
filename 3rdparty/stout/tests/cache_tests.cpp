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

#include <gtest/gtest.h>

#include <stout/cache.hpp>
#include <stout/gtest.hpp>


TEST(CacheTest, Insert)
{
  Cache<int, std::string> cache(1);
  EXPECT_EQ(0u, cache.size());
  cache.put(1, "a");
  EXPECT_SOME_EQ("a", cache.get(1));
  EXPECT_EQ(1u, cache.size());
}


TEST(CacheTest, Update)
{
  Cache<int, std::string> cache(1);
  cache.put(1, "a");
  cache.put(1, "b");
  EXPECT_SOME_EQ("b", cache.get(1));
  EXPECT_EQ(1u, cache.size());
}


TEST(CacheTest, Erase)
{
  Cache<int, std::string> cache(2);
  cache.put(1, "a");
  cache.put(2, "b");

  EXPECT_NONE(cache.erase(44));

  EXPECT_SOME_EQ("b", cache.erase(2));
  EXPECT_EQ(1u, cache.size());

  EXPECT_NONE(cache.erase(2));
  EXPECT_EQ(1u, cache.size());

  EXPECT_SOME_EQ("a", cache.erase(1));
  EXPECT_EQ(0u, cache.size());
}


TEST(CacheTest, LRUEviction)
{
  Cache<int, std::string> cache(2);
  cache.put(1, "a");
  cache.put(2, "b");
  cache.put(3, "c");

  EXPECT_NONE(cache.get(1));

  // 'Get' makes '2' the most-recently used (MRU) item.
  cache.get(2);
  cache.put(4, "d");
  EXPECT_NONE(cache.get(3));
  EXPECT_SOME_EQ("b", cache.get(2));
  EXPECT_SOME_EQ("d", cache.get(4));

  // 'Put' also makes '2' MRU.
  cache.put(2, "x");
  cache.put(5, "e");
  EXPECT_NONE(cache.get(4));
  EXPECT_SOME_EQ("x", cache.get(2));
  EXPECT_SOME_EQ("e", cache.get(5));

  // 'Erase' the LRU.
  cache.erase(2);
  cache.put(6, "f");
  cache.put(7, "g");
  EXPECT_NONE(cache.get(5));
}
