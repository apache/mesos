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

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/path.hpp>

#include "linux/cgroups2.hpp"

#include "slave/containerizer/mesos/paths.hpp"

using std::string;

using mesos::internal::slave::containerizer::paths::buildPath;
using mesos::internal::slave::containerizer::paths::JOIN;
using mesos::internal::slave::containerizer::paths::PREFIX;
using mesos::internal::slave::containerizer::paths::SUFFIX;

namespace mesos {
namespace internal {
namespace tests {

TEST(MesosContainerizerPathsTest, BuildPathPrefixMode)
{
  ContainerID parent;
  parent.set_value("parent");

  ContainerID child;
  child.set_value("child");
  child.mutable_parent()->CopyFrom(parent);

  const string separator = "separator";

  EXPECT_EQ(
      buildPath(parent, separator, PREFIX),
      path::join(separator, parent.value()));

  EXPECT_EQ(
      buildPath(child, separator, PREFIX),
      path::join(separator, parent.value(), separator, child.value()));
}


TEST(MesosContainerizerPathsTest, BuildPathSuffixMode)
{
  ContainerID parent;
  parent.set_value("parent");

  ContainerID child;
  child.set_value("child");
  child.mutable_parent()->CopyFrom(parent);

  const string separator = "separator";

  EXPECT_EQ(
      buildPath(parent, separator, SUFFIX),
      path::join(parent.value(), separator));

  EXPECT_EQ(
      buildPath(child, separator, SUFFIX),
      path::join(parent.value(), separator, child.value(), separator));
}


TEST(MesosContainerizerPathsTest, BuildPathJoinMode)
{
  ContainerID parent;
  parent.set_value("parent");

  ContainerID child;
  child.set_value("child");
  child.mutable_parent()->CopyFrom(parent);

  const string separator = "separator";

  EXPECT_EQ(
      buildPath(parent, separator, JOIN),
      parent.value());

  EXPECT_EQ(
      buildPath(child, separator, JOIN),
      path::join(parent.value(), separator, child.value()));
}


TEST(MesosContainerizerPathsTest, CGROUPS2_Cgroups2Paths)
{
  namespace cgroups2 = mesos::internal::slave::containerizer::paths::cgroups2;

  ContainerID parent;
  parent.set_value("parent");

  ContainerID child1;
  child1.set_value("child1");
  child1.mutable_parent()->CopyFrom(parent);

  ContainerID child2;
  child2.set_value("child2");
  child2.mutable_parent()->CopyFrom(child1);

  EXPECT_EQ("mesos/agent",
            cgroups2::agent("mesos"));
  EXPECT_EQ("mesos/agent/leaf",
            cgroups2::agent("mesos", true));

  EXPECT_EQ("mesos/parent",
            cgroups2::container("mesos", parent));
  EXPECT_EQ("mesos/parent/leaf",
            cgroups2::container("mesos", parent, true));

  EXPECT_EQ("mesos/parent/mesos/child1",
            cgroups2::container("mesos", child1));
  EXPECT_EQ("mesos/parent/mesos/child1/leaf",
            cgroups2::container("mesos", child1, true));

  EXPECT_EQ("mesos/parent/mesos/child1/mesos/child2",
            cgroups2::container("mesos", child2));
  EXPECT_EQ("mesos/parent/mesos/child1/mesos/child2/leaf",
            cgroups2::container("mesos", child2, true));
}


TEST(MesosContainerizerPathsTest, CGROUPS2_Cgroups2ParsePaths)
{
  namespace cgroups2 = mesos::internal::slave::containerizer::paths::cgroups2;

  EXPECT_NONE(cgroups2::containerId("mesos", ""));
  EXPECT_NONE(cgroups2::containerId("mesos", "/"));
  EXPECT_NONE(cgroups2::containerId("mesos", cgroups2::agent("mesos")));
  EXPECT_NONE(cgroups2::containerId("mesos", cgroups2::agent("mesos", true)));

  // Setup the container chain: id1 -> id2 -> id3
  ContainerID id1;
  id1.set_value("id1");

  ContainerID id2;
  id2.set_value("id2");
  id2.mutable_parent()->CopyFrom(id1);

  ContainerID id3;
  id3.set_value("id3");
  id3.mutable_parent()->CopyFrom(id2);

  auto EXPECT_SOME_ID_EQ =
    [](ContainerID expected, Option<ContainerID> _actual) {
    EXPECT_SOME(_actual);

    ContainerID actual = *_actual;
    while (expected.has_parent() && actual.has_parent()) {
      EXPECT_EQ(expected.value(), actual.value());
      expected = expected.parent();
      actual = actual.parent();
    }

    EXPECT_EQ(expected.has_parent(), actual.has_parent());
    EXPECT_EQ(expected.value(), actual.value());
  };

  EXPECT_SOME_ID_EQ(
      id3,
      cgroups2::containerId("mesos", cgroups2::container("mesos", id3)));

  EXPECT_SOME_ID_EQ(
      id2,
      cgroups2::containerId("mesos", cgroups2::container("mesos", id2)));

  EXPECT_SOME_ID_EQ(
      id1,
      cgroups2::containerId("mesos", cgroups2::container("mesos", id1)));

  // Test leaf cgroups.
  EXPECT_SOME_ID_EQ(
      id3,
      cgroups2::containerId("mesos", cgroups2::container("mesos", id3, true)));

  EXPECT_SOME_ID_EQ(
      id2,
      cgroups2::containerId("mesos", cgroups2::container("mesos", id2, true)));

  EXPECT_SOME_ID_EQ(
      id1,
      cgroups2::containerId("mesos", cgroups2::container("mesos", id1, true)));

  EXPECT_SOME_ID_EQ(
      id1,
      cgroups2::containerId(
          "root", cgroups2::container("root", id1, true)));

  EXPECT_SOME_ID_EQ(
      id1,
      cgroups2::containerId(
          "test/root", cgroups2::container("test/root", id1, true)));

  EXPECT_SOME_ID_EQ(
      id3,
      cgroups2::containerId(
          "test/root", "test/root/id1/mesos/id2/mesos/id3"));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
