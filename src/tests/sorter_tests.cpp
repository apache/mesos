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

#include <stdarg.h>

#include <gmock/gmock.h>

#include "master/drf_sorter.hpp"
#include "master/sorter.hpp"

using namespace mesos::internal;

using mesos::internal::master::Sorter;
using mesos::internal::master::DRFSorter;

using std::list;
using std::string;

void checkSorter(Sorter& sorter, uint32_t count, ...)
{
  va_list args;
  va_start(args, count);
  list<string> ordering = sorter.sort();
  EXPECT_EQ(ordering.size(), count);

  foreach (const string& actual, ordering) {
    const char* expected = va_arg(args, char*);
    EXPECT_EQ(actual, expected);
  }
  va_end(args);
}


TEST(SorterTest, DRFSorter)
{
  DRFSorter sorter;

  Resources totalResources = Resources::parse("cpus:100;mem:100");
  sorter.add(totalResources);

  sorter.add("a");
  Resources aResources = Resources::parse("cpus:5;mem:5");
  sorter.allocated("a", aResources);

  Resources bResources = Resources::parse("cpus:6;mem:6");
  sorter.add("b");
  sorter.allocated("b", bResources);

  // shares: a = .05, b = .06
  checkSorter(sorter, 2, "a", "b");

  Resources cResources = Resources::parse("cpus:1;mem:1");
  sorter.add("c");
  sorter.allocated("c", cResources);

  Resources dResources = Resources::parse("cpus:3;mem:1");
  sorter.add("d");
  sorter.allocated("d", dResources);

  // shares: a = .05, b = .06, c = .01, d = .03
  checkSorter(sorter, 4, "c", "d", "a", "b");

  sorter.remove("a");
  Resources bUnallocated = Resources::parse("cpus:4;mem:4");
  sorter.unallocated("b", bUnallocated);

  // shares: b = .02, c = .01, d = .03
  checkSorter(sorter, 3, "c", "b", "d");

  Resources eResources = Resources::parse("cpus:1;mem:5");
  sorter.add("e");
  sorter.allocated("e", eResources);

  Resources removedResources = Resources::parse("cpus:50;mem:0");
  sorter.remove(removedResources);
  // total resources is now cpus = 50, mem = 100

  // shares: b = .04, c = .02, d = .06, e = .05
  checkSorter(sorter, 4, "c", "b", "e", "d");

  Resources addedResources = Resources::parse("cpus:0;mem:100");
  sorter.add(addedResources);
  // total resources is now cpus = 50, mem = 200

  Resources fResources = Resources::parse("cpus:5;mem:1");
  sorter.add("f");
  sorter.allocated("f", fResources);

  Resources cResources2 = Resources::parse("cpus:0;mem:15");
  sorter.allocated("c", cResources2);

  // shares: b = .04, c = .08, d = .06, e = .025, f = .1
  checkSorter(sorter, 5, "e", "b", "d", "c", "f");

  EXPECT_TRUE(sorter.contains("b"));

  EXPECT_FALSE(sorter.contains("a"));

  EXPECT_EQ(sorter.count(), 5);

  sorter.deactivate("d");

  EXPECT_TRUE(sorter.contains("d"));

  checkSorter(sorter, 4, "e", "b", "c", "f");

  EXPECT_EQ(sorter.count(), 5);

  sorter.activate("d");

  checkSorter(sorter, 5, "e", "b", "d", "c", "f");
}
