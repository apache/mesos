// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <mesos/values.hpp>

#include <stout/bytes.hpp>
#include <stout/stringify.hpp>

#include "slave/containerizer/containerizer.hpp"

#include "tests/mesos.hpp"

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::DEFAULT_PORTS;
using mesos::internal::slave::Slave;

namespace mesos {
namespace internal {
namespace tests {

class ContainerizerResourcesTest : public MesosTest {};

// This test verifies that standard resources (memory, CPU, ports etc) are
// automatically added if and only if they are not explicitly specified on
// the command line.
TEST_F(ContainerizerResourcesTest, AutoResources)
{
  slave::Flags flags = CreateSlaveFlags();
  // Use a resource string that contains the standard resources as
  // substrings, to make sure they don't accidentally suppress the
  // automatic values (regression test for MESOS-6821).
  flags.resources =
    "my.cpus:1000;cpus-suffix:3;memory(mem):1024"
    ";things:{mem,disk,ports};diskports:[30000-35000]";

  Try<Resources> resources = Containerizer::resources(flags);
  ASSERT_SOME(resources);

  EXPECT_LT(0, resources->cpus().getOrElse(0));
  EXPECT_LT(Bytes(0), resources->mem().getOrElse(0));
  EXPECT_LT(Bytes(0), resources->disk().getOrElse(0));

  EXPECT_EQ(
      DEFAULT_PORTS, stringify(resources->ports().getOrElse(Value::Ranges())));
}


// This test verifies that auto-detection of resources is suppressed by
// explicitly requiring zero amount of the resource.
TEST_F(ContainerizerResourcesTest, AutoResourcesZero)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:0;mem(*):0;disk:0;ports:[]";

  Try<Resources> resources = Containerizer::resources(flags);
  ASSERT_SOME(resources);

  EXPECT_EQ(0, resources->cpus().getOrElse(0));
  EXPECT_EQ(Bytes(0), resources->mem().getOrElse(0));
  EXPECT_EQ(Bytes(0), resources->disk().getOrElse(0));
  EXPECT_EQ("[]", stringify(resources->ports().getOrElse(Value::Ranges())));
}


// This test verifies that auto-detection of resources is suppressed when
// explicitly specifying resource values.
TEST_F(ContainerizerResourcesTest, AutoResourcesNonZero)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.resources = "cpus:2;mem(*):1024;disk:4096;ports:[30000-35000]";

  Try<Resources> resources = Containerizer::resources(flags);
  ASSERT_SOME(resources);

  EXPECT_EQ(2, resources->cpus().getOrElse(0));
  EXPECT_EQ(Megabytes(1024), resources->mem().getOrElse(0));
  EXPECT_EQ(Megabytes(4096), resources->disk().getOrElse(0));
  EXPECT_EQ(
      "[30000-35000]",
      stringify(resources->ports().getOrElse(Value::Ranges())));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
