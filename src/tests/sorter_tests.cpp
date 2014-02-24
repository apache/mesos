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
#include <stdint.h>

#include <gmock/gmock.h>

#include "master/drf_sorter.hpp"
#include "master/sorter.hpp"

using namespace mesos;

using mesos::internal::master::allocator::Sorter;
using mesos::internal::master::allocator::DRFSorter;

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

  Resources totalResources = Resources::parse("cpus:100;mem:100").get();
  sorter.add(totalResources);

  sorter.add("a");
  Resources aResources = Resources::parse("cpus:5;mem:5").get();
  sorter.allocated("a", aResources);

  Resources bResources = Resources::parse("cpus:6;mem:6").get();
  sorter.add("b");
  sorter.allocated("b", bResources);

  // shares: a = .05, b = .06
  checkSorter(sorter, 2, "a", "b");

  Resources cResources = Resources::parse("cpus:1;mem:1").get();
  sorter.add("c");
  sorter.allocated("c", cResources);

  Resources dResources = Resources::parse("cpus:3;mem:1").get();
  sorter.add("d");
  sorter.allocated("d", dResources);

  // shares: a = .05, b = .06, c = .01, d = .03
  checkSorter(sorter, 4, "c", "d", "a", "b");

  sorter.remove("a");
  Resources bUnallocated = Resources::parse("cpus:4;mem:4").get();
  sorter.unallocated("b", bUnallocated);

  // shares: b = .02, c = .01, d = .03
  checkSorter(sorter, 3, "c", "b", "d");

  Resources eResources = Resources::parse("cpus:1;mem:5").get();
  sorter.add("e");
  sorter.allocated("e", eResources);

  Resources removedResources = Resources::parse("cpus:50;mem:0").get();
  sorter.remove(removedResources);
  // total resources is now cpus = 50, mem = 100

  // shares: b = .04, c = .02, d = .06, e = .05
  checkSorter(sorter, 4, "c", "b", "e", "d");

  Resources addedResources = Resources::parse("cpus:0;mem:100").get();
  sorter.add(addedResources);
  // total resources is now cpus = 50, mem = 200

  Resources fResources = Resources::parse("cpus:5;mem:1").get();
  sorter.add("f");
  sorter.allocated("f", fResources);

  Resources cResources2 = Resources::parse("cpus:0;mem:15").get();
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


TEST(SorterTest, WDRFSorter)
{
  DRFSorter sorter;

  sorter.add(Resources::parse("cpus:100;mem:100").get());

  sorter.add("a");

  sorter.allocated("a", Resources::parse("cpus:5;mem:5").get());

  sorter.add("b", 2);
  sorter.allocated("b", Resources::parse("cpus:6;mem:6").get());

  // shares: a = .05, b = .03
  checkSorter(sorter, 2, "b", "a");

  sorter.add("c");
  sorter.allocated("c", Resources::parse("cpus:4;mem:4").get());

  // shares: a = .05, b = .03, c = .04
  checkSorter(sorter, 3, "b", "c", "a");

  sorter.add("d", 10);
  sorter.allocated("d", Resources::parse("cpus:10;mem:20").get());

  // shares: a = .05, b = .03, c = .04, d = .02
  checkSorter(sorter, 4, "d", "b", "c", "a");

  sorter.remove("b");

  checkSorter(sorter, 3, "d", "c", "a");

  sorter.allocated("d", Resources::parse("cpus:10;mem:25").get());

  // shares: a = .05, c = .04, d = .045
  checkSorter(sorter, 3, "c", "d", "a");

  sorter.add("e", .1);
  sorter.allocated("e", Resources::parse("cpus:1;mem:1").get());

  // shares: a = .05, c = .04, d = .045, e = .1
  checkSorter(sorter, 4, "c", "d", "a", "e");

  sorter.remove("a");

  checkSorter(sorter, 3, "c", "d", "e");
}
