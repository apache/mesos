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

#include <set>
#include <string>

#include <stout/tests/utils.hpp>
#include <stout/try.hpp>

#include "linux/cgroups2.hpp"

using std::set;
using std::string;

namespace mesos {
namespace internal {
namespace tests {

class Cgroups2Test : public TemporaryDirectoryTest {};


TEST_F(Cgroups2Test, ROOT_CGROUPS2_Enabled)
{
  EXPECT_TRUE(cgroups2::enabled());
}


TEST_F(Cgroups2Test, ROOT_CGROUPS2_AvailableSubsystems)
{

  Try<bool> mounted = cgroups2::mounted();
  ASSERT_SOME(mounted);

  if (!*mounted) {
    ASSERT_SOME(cgroups2::mount());
  }

  Try<set<string>> available = cgroups2::controllers::available(
    cgroups2::ROOT_CGROUP);

  ASSERT_SOME(available);
  EXPECT_TRUE(available->count("cpu") == 1);

  if (!*mounted) {
    EXPECT_SOME(cgroups2::unmount());
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {

