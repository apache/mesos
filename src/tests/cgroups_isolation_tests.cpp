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

#include <map>
#include <utility>

#include <gtest/gtest.h>

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>

#include "linux/proc.hpp"

#include "slave/cgroups_isolation_module.hpp"

#include "tests/script.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using std::map;

// Run the balloon framework under the cgroups isolation module.
TEST_SCRIPT(CgroupsIsolationTest,
            ROOT_CGROUPS_BalloonFramework,
            "balloon_framework_test.sh")


#define GROW_USAGE(delta, cpuset, usage)                                   \
  ({                                                                       \
    const map<proc::CPU, double>& allocation = cpuset.grow(delta, usage);  \
    foreachpair (const proc::CPU& cpu, double allocated, allocation) {     \
      usage[cpu] += allocated;                                             \
      ASSERT_LT(usage[cpu], 1.001);                                        \
    }                                                                      \
  })


#define SHRINK_USAGE(delta, cpuset, usage)                                 \
  ({                                                                       \
    const map<proc::CPU, double>& deallocation = cpuset.shrink(delta);     \
    foreachpair (const proc::CPU& cpu, double deallocated, deallocation) { \
      usage[cpu] -= deallocated;                                           \
      ASSERT_GT(usage[cpu], -0.001);                                       \
    }                                                                      \
  })


TEST(CgroupsCpusetTest, OneCPUOneCpuset)
{
  Cpuset cpuset;

  map<proc::CPU, double> usage;
  // NOTE: Using the [] operator here led to a warning with gcc 4.4.3.
  usage.insert(std::make_pair(proc::CPU(0, 0, 0), 0.0));

  // Saturate the CPU.
  GROW_USAGE(0.2, cpuset, usage);
  GROW_USAGE(0.1, cpuset, usage);
  GROW_USAGE(0.2, cpuset, usage);
  GROW_USAGE(0.5, cpuset, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 1.0, 0.001);
  ASSERT_NEAR(cpuset.usage(), 1.0, 0.001);

  ASSERT_EQ(stringify(cpuset), "0");

  // Empty the CPU.
  SHRINK_USAGE(0.5, cpuset, usage);
  SHRINK_USAGE(0.2, cpuset, usage);
  SHRINK_USAGE(0.1, cpuset, usage);
  SHRINK_USAGE(0.2, cpuset, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 0.0, 0.001);
  ASSERT_NEAR(cpuset.usage(), 0.0, 0.001);

  ASSERT_EQ(stringify(cpuset), "");
}


TEST(CgroupsCpusetTest, OneCPUManyCpusets)
{
  Cpuset cpuset1, cpuset2, cpuset3;

  map<proc::CPU, double> usage;
  // NOTE: Using the [] operator here led to a warning with gcc 4.4.3.
  usage.insert(std::make_pair(proc::CPU(0, 0, 0), 0.0));

  // Saturate the CPU.
  GROW_USAGE(0.2, cpuset1, usage);
  GROW_USAGE(0.1, cpuset2, usage);
  GROW_USAGE(0.2, cpuset3, usage);
  GROW_USAGE(0.5, cpuset1, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 1.0, 0.001);
  ASSERT_NEAR(cpuset1.usage(), 0.7, 0.001);
  ASSERT_NEAR(cpuset2.usage(), 0.1, 0.001);
  ASSERT_NEAR(cpuset3.usage(), 0.2, 0.001);

  ASSERT_EQ(stringify(cpuset1), "0");
  ASSERT_EQ(stringify(cpuset2), "0");
  ASSERT_EQ(stringify(cpuset3), "0");

  // Empty the CPU.
  SHRINK_USAGE(0.5, cpuset1, usage);
  SHRINK_USAGE(0.2, cpuset3, usage);
  SHRINK_USAGE(0.1, cpuset2, usage);
  SHRINK_USAGE(0.2, cpuset1, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 0.0, 0.001);
  ASSERT_NEAR(cpuset1.usage(), 0.0, 0.001);
  ASSERT_NEAR(cpuset2.usage(), 0.0, 0.001);
  ASSERT_NEAR(cpuset3.usage(), 0.0, 0.001);

  ASSERT_EQ(stringify(cpuset1), "");
  ASSERT_EQ(stringify(cpuset2), "");
  ASSERT_EQ(stringify(cpuset3), "");
}


TEST(CgroupsCpusetTest, ManyCPUOneCpuset)
{
  Cpuset cpuset;

  map<proc::CPU, double> usage;
  // NOTE: Using the [] operator here led to a warning with gcc 4.4.3.
  usage.insert(std::make_pair(proc::CPU(0, 0, 0), 0.0));
  usage.insert(std::make_pair(proc::CPU(1, 0, 0), 0.0));
  usage.insert(std::make_pair(proc::CPU(2, 0, 0), 0.0));

  // Saturate the first CPU.
  GROW_USAGE(0.2, cpuset, usage);
  GROW_USAGE(0.1, cpuset, usage);
  GROW_USAGE(0.2, cpuset, usage);
  GROW_USAGE(0.5, cpuset, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 1.0, 0.001);
  ASSERT_NEAR(cpuset.usage(), 1.0, 0.001);

  ASSERT_EQ(stringify(cpuset), "0");

  // Saturate the second CPU.
  GROW_USAGE(0.6, cpuset, usage);
  GROW_USAGE(0.4, cpuset, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 1.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(1,0,0)], 1.0, 0.001);
  ASSERT_NEAR(cpuset.usage(), 2.0, 0.001);

  ASSERT_EQ(stringify(cpuset), "0,1");

  // Partial third CPU.
  GROW_USAGE(0.1, cpuset, usage);

  ASSERT_NEAR(usage[proc::CPU(2,0,0)], 0.1, 0.001);
  ASSERT_NEAR(cpuset.usage(), 2.1, 0.001);

  ASSERT_EQ(stringify(cpuset), "0,1,2");

  // Empty the CPU.
  SHRINK_USAGE(0.5, cpuset, usage);
  SHRINK_USAGE(0.2, cpuset, usage);
  SHRINK_USAGE(0.1, cpuset, usage);
  SHRINK_USAGE(0.1, cpuset, usage);
  SHRINK_USAGE(0.2, cpuset, usage);
  SHRINK_USAGE(0.4, cpuset, usage);
  SHRINK_USAGE(0.6, cpuset, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 0.0, 0.001);
  ASSERT_NEAR(cpuset.usage(), 0.0, 0.001);

  ASSERT_EQ(stringify(cpuset), "");
}


TEST(CgroupsCpusetTest, ManyCPUManyCpusets)
{
  Cpuset cpuset1, cpuset2, cpuset3;

  map<proc::CPU, double> usage;
  // NOTE: Using the [] operator here led to a warning with gcc 4.4.3.
  usage.insert(std::make_pair(proc::CPU(0, 0, 0), 0.0));
  usage.insert(std::make_pair(proc::CPU(1, 0, 0), 0.0));
  usage.insert(std::make_pair(proc::CPU(2, 0, 0), 0.0));

  // Saturate the first CPU.
  GROW_USAGE(0.2, cpuset1, usage);
  GROW_USAGE(0.1, cpuset2, usage);
  GROW_USAGE(0.2, cpuset3, usage);
  GROW_USAGE(0.5, cpuset1, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 1.0, 0.001);
  ASSERT_NEAR(cpuset1.usage(), 0.7, 0.001);
  ASSERT_NEAR(cpuset2.usage(), 0.1, 0.001);
  ASSERT_NEAR(cpuset3.usage(), 0.2, 0.001);

  ASSERT_EQ(stringify(cpuset1), "0");
  ASSERT_EQ(stringify(cpuset2), "0");
  ASSERT_EQ(stringify(cpuset3), "0");

  // Saturate the second CPU.
  GROW_USAGE(0.6, cpuset3, usage);
  GROW_USAGE(0.4, cpuset2, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 1.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(1,0,0)], 1.0, 0.001);
  ASSERT_NEAR(cpuset2.usage(), 0.5, 0.001);
  ASSERT_NEAR(cpuset3.usage(), 0.8, 0.001);

  ASSERT_EQ(stringify(cpuset2), "0,1");
  ASSERT_EQ(stringify(cpuset3), "0,1");

  // Partial third CPU.
  GROW_USAGE(0.1, cpuset2, usage);

  ASSERT_NEAR(usage[proc::CPU(2,0,0)], 0.1, 0.001);
  ASSERT_NEAR(cpuset2.usage(), 0.6, 0.001);

  ASSERT_EQ(stringify(cpuset2), "0,1,2");

  // Empty the CPU.
  SHRINK_USAGE(0.5, cpuset1, usage);
  SHRINK_USAGE(0.2, cpuset1, usage);
  SHRINK_USAGE(0.1, cpuset2, usage);
  SHRINK_USAGE(0.1, cpuset2, usage);
  SHRINK_USAGE(0.2, cpuset3, usage);
  SHRINK_USAGE(0.4, cpuset2, usage);
  SHRINK_USAGE(0.6, cpuset3, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 0.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(1,0,0)], 0.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(2,0,0)], 0.0, 0.001);

  ASSERT_NEAR(cpuset1.usage(), 0.0, 0.001);
  ASSERT_NEAR(cpuset2.usage(), 0.0, 0.001);
  ASSERT_NEAR(cpuset3.usage(), 0.0, 0.001);

  ASSERT_EQ(stringify(cpuset1), "");
  ASSERT_EQ(stringify(cpuset2), "");
  ASSERT_EQ(stringify(cpuset3), "");
}


TEST(CgroupsCpusetTest, IntegerAllocations)
{
  // Ensure no fragmentation occurs.
  Cpuset cpuset1, cpuset2, cpuset3;

  map<proc::CPU, double> usage;
  // NOTE: Using the [] operator here led to a warning with gcc 4.4.3.
  usage.insert(std::make_pair(proc::CPU(0, 0, 0), 0.0));
  usage.insert(std::make_pair(proc::CPU(1, 0, 0), 0.0));
  usage.insert(std::make_pair(proc::CPU(2, 0, 0), 0.0));
  usage.insert(std::make_pair(proc::CPU(3, 0, 0), 0.0));

  // Saturate the CPUs.
  GROW_USAGE(1.0, cpuset1, usage);
  GROW_USAGE(2.0, cpuset2, usage);
  GROW_USAGE(1.0, cpuset3, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 1.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(1,0,0)], 1.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(2,0,0)], 1.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(3,0,0)], 1.0, 0.001);

  ASSERT_NEAR(cpuset1.usage(), 1.0, 0.001);
  ASSERT_NEAR(cpuset2.usage(), 2.0, 0.001);
  ASSERT_NEAR(cpuset3.usage(), 1.0, 0.001);

  ASSERT_EQ(stringify(cpuset1), "0");
  ASSERT_EQ(stringify(cpuset2), "1,2");
  ASSERT_EQ(stringify(cpuset3), "3");

  // Empty the CPU.
  SHRINK_USAGE(1.0, cpuset1, usage);
  SHRINK_USAGE(2.0, cpuset2, usage);
  SHRINK_USAGE(1.0, cpuset3, usage);

  ASSERT_NEAR(usage[proc::CPU(0,0,0)], 0.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(1,0,0)], 0.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(2,0,0)], 0.0, 0.001);
  ASSERT_NEAR(usage[proc::CPU(3,0,0)], 0.0, 0.001);

  ASSERT_NEAR(cpuset1.usage(), 0.0, 0.001);
  ASSERT_NEAR(cpuset2.usage(), 0.0, 0.001);
  ASSERT_NEAR(cpuset3.usage(), 0.0, 0.001);

  ASSERT_EQ(stringify(cpuset1), "");
  ASSERT_EQ(stringify(cpuset2), "");
  ASSERT_EQ(stringify(cpuset3), "");
}
