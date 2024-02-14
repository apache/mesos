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

#include <string>

#include <gmock/gmock.h>

#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/tests/utils.hpp>

#include "linux/cgroups2.hpp"

#include "tests/mesos.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace tests {

const string TEST_MOUNT_POINT = "/tmp/cgroups2";

class Cgroups2Test : public TemporaryDirectoryTest 
{
public:
  static void SetUpTestCase()
  {
  }

  static void TearDownTestCase()
  {
  }
};

TEST_F(Cgroups2Test, ROOT_CGROUPS2_Enabled)
{
  EXPECT_TRUE(cgroups2::enabled());
}

TEST_F(Cgroups2Test, ROOT_CGROUPS2_AvailableSubsystems)
{
  EXPECT_SOME(cgroups2::mount_or_create(TEST_MOUNT_POINT));
  EXPECT_SOME(cgroups2::subsystems::available());
  EXPECT_TRUE(cgroups2::subsystems::available().get().count("cpu") == 1);
  EXPECT_SOME(cgroups2::cleanup());
}

TEST_F(Cgroups2Test, ROOT_CGROUPS2_Prepare)
{
  EXPECT_SOME(cgroups2::prepare(TEST_MOUNT_POINT, { "cpu" }));
  EXPECT_TRUE(cgroups2::subsystems::available().get().count("cpu") == 1);
  EXPECT_SOME_TRUE(cgroups2::subsystems::enabled(
    cgroups2::ROOT_CGROUP, 
    { "cpu"}
  ));
  EXPECT_SOME(cgroups2::cleanup());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {

