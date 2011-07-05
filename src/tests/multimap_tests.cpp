/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <stdint.h>

#include <string>

#include "common/foreach.hpp"
#include "common/multimap.hpp"

using namespace mesos;
using namespace mesos::internal;

using std::string;


TEST(Multimap, Insert)
{
  multimap<string, uint16_t> map;

  map.insert("foo", 1024);
  ASSERT_EQ(1, map.count("foo"));

  map.insert("foo", 1025);
  ASSERT_EQ(2, map.count("foo"));

  ASSERT_EQ(1, map.size());

  map.insert("bar", 1024);
  ASSERT_EQ(1, map.count("bar"));

  map.insert("bar", 1025);
  ASSERT_EQ(2, map.count("bar"));

  ASSERT_EQ(2, map.size());
}


TEST(Multimap, Erase)
{
  multimap<string, uint16_t> map;

  map.insert("foo", 1024);
  map.erase("foo", 1024);
  ASSERT_EQ(0, map.count("foo"));

  ASSERT_EQ(0, map.size());

  map.insert("foo", 1024);
  map.insert("foo", 1025);
  ASSERT_EQ(2, map.count("foo"));

  ASSERT_EQ(1, map.size());

  map.erase("foo");
  ASSERT_EQ(0, map.count("foo"));
  ASSERT_EQ(0, map.size());
}


TEST(Multimap, Count)
{
  multimap<string, uint16_t> map;

  map.insert("foo", 1024);
  map.insert("foo", 1025);
  ASSERT_EQ(2, map.count("foo"));
  ASSERT_EQ(1, map.count("foo", 1024));
  ASSERT_EQ(1, map.count("foo", 1025));

  map.insert("bar", 1024);
  map.insert("bar", 1025);
  ASSERT_EQ(2, map.count("bar"));
  ASSERT_EQ(1, map.count("bar", 1024));
  ASSERT_EQ(1, map.count("bar", 1025));
}


TEST(Multimap, Iterator)
{
  multimap<string, uint16_t> map;

  map.insert("foo", 1024);
  map.insert("foo", 1025);
  ASSERT_EQ(2, map.count("foo"));
  ASSERT_EQ(1, map.count("foo", 1024));
  ASSERT_EQ(1, map.count("foo", 1025));

  multimap<string, uint16_t>::iterator i = map.begin();

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


TEST(Multimap, Foreach)
{
  multimap<string, uint16_t> map;

  map.insert("foo", 1024);
  map.insert("bar", 1025);
  ASSERT_EQ(1, map.count("foo"));
  ASSERT_EQ(1, map.count("foo"));
  ASSERT_EQ(1, map.count("foo", 1024));
  ASSERT_EQ(1, map.count("bar", 1025));

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
