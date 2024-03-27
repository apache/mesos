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

#include <stout/path.hpp>

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

    const string& ROOT = "mesos";

    ContainerID parent;
    parent.set_value("parent");

    ContainerID child1;
    child1.set_value("child2");
    child1.mutable_parent()->CopyFrom(parent);

    ContainerID child2;
    child2.set_value("child2");
    child2.mutable_parent()->CopyFrom(child1);

    EXPECT_EQ("mesos/agent", cgroups2::agent(ROOT));
    EXPECT_EQ("mesos/agent/leaf", cgroups2::agent(ROOT, true));

    EXPECT_EQ(
        path::join(ROOT, parent.value()), cgroups2::container(ROOT, parent));
    EXPECT_EQ(path::join(
        ROOT, parent.value(), "leaf"), cgroups2::container(ROOT, parent, true));

    EXPECT_EQ(
        path::join(ROOT, parent.value(), "mesos", child1.value()),
        cgroups2::container(ROOT, child1));
    EXPECT_EQ(
        path::join(ROOT, parent.value(), "mesos", child1.value(), "leaf"),
        cgroups2::container(ROOT, child1, true));

    EXPECT_EQ(
        path::join(
            ROOT,
            parent.value(), "mesos", child1.value(), "mesos", child2.value()),
        cgroups2::container(ROOT, child2));
    EXPECT_EQ(
        path::join(
            ROOT,
            parent.value(),
            "mesos", child1.value(), "mesos", child2.value(), "leaf"),
        cgroups2::container(ROOT, child2, true));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
