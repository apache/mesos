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

#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>

#include "slave/containerizer/mesos/provisioner/paths.hpp"

#include "tests/mesos.hpp"

namespace paths = mesos::internal::slave::provisioner::paths;

using std::string;

namespace mesos {
namespace internal {
namespace tests {

class ProvisionerPathTest : public TemporaryDirectoryTest {};


TEST_F(ProvisionerPathTest, ListProvisionerContainers)
{
  ContainerID child1;  // parent1/child1
  ContainerID child2;  // parent1/child2
  ContainerID child3;  // parent2/child3
  ContainerID parent1; // parent1
  ContainerID parent2; // parent2

  child1.set_value("child1");
  child1.mutable_parent()->set_value("parent1");
  child2.set_value("child2");
  child2.mutable_parent()->set_value("parent1");
  child3.set_value("child3");
  child3.mutable_parent()->set_value("parent2");
  parent1.set_value("parent1");
  parent2.set_value("parent2");

  const string provisionerDir = os::getcwd();
  const string containerDir1 = paths::getContainerDir(provisionerDir, child1);
  const string containerDir2 = paths::getContainerDir(provisionerDir, child2);
  const string containerDir3 = paths::getContainerDir(provisionerDir, child3);

  ASSERT_SOME(os::mkdir(containerDir1));
  ASSERT_SOME(os::mkdir(containerDir2));
  ASSERT_SOME(os::mkdir(containerDir3));

  Try<hashset<ContainerID>> containerIds =
    paths::listContainers(provisionerDir);

  ASSERT_SOME(containerIds);

  EXPECT_TRUE(containerIds->contains(parent1));
  EXPECT_TRUE(containerIds->contains(parent2));
  EXPECT_TRUE(containerIds->contains(child1));
  EXPECT_TRUE(containerIds->contains(child2));
  EXPECT_TRUE(containerIds->contains(child3));
  EXPECT_EQ(5u, containerIds->size());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
