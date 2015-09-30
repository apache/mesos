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

#ifndef __TEST_PROVISIONER_HPP__
#define __TEST_PROVISIONER_HPP__

#include <gmock/gmock.h>

#include <process/shared.hpp>

#include <stout/hashmap.hpp>
#include <stout/stringify.hpp>

#include "slave/containerizer/provisioner/provisioner.hpp"

#include "tests/containerizer/rootfs.hpp"

namespace mesos {
namespace internal {
namespace tests {

class TestProvisioner : public slave::Provisioner
{
public:
  TestProvisioner(
      const hashmap<std::string, process::Shared<Rootfs>>& _rootfses)
    : rootfses(_rootfses)
  {
    using testing::_;
    using testing::DoDefault;
    using testing::Invoke;

    ON_CALL(*this, recover(_, _))
      .WillByDefault(Invoke(this, &TestProvisioner::unmocked_recover));
    EXPECT_CALL(*this, recover(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, provision(_, _))
      .WillByDefault(Invoke(this, &TestProvisioner::unmocked_provision));
    EXPECT_CALL(*this, provision(_, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, destroy(_))
      .WillByDefault(Invoke(this, &TestProvisioner::unmocked_destroy));
    EXPECT_CALL(*this, destroy(_))
      .WillRepeatedly(DoDefault());
  }

  MOCK_METHOD2(
      recover,
      process::Future<Nothing>(
          const std::list<mesos::slave::ContainerState>& states,
          const hashset<ContainerID>& orphans));

  MOCK_METHOD2(
      provision,
      process::Future<std::string>(
          const ContainerID& containerId,
          const Image& image));

  MOCK_METHOD1(
      destroy,
      process::Future<bool>(
          const ContainerID& containerId));

  process::Future<Nothing> unmocked_recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans)
  {
    return Nothing();
  }

  process::Future<std::string> unmocked_provision(
      const ContainerID& containerId,
      const Image& image)
  {
    if (image.type() != Image::APPC) {
      return process::Failure(
          "Unsupported image type '" + stringify(image.type()) + "'");
    }

    if (!rootfses.contains(image.appc().name())) {
      return process::Failure(
          "Image '" + image.appc().name() + "' is not found");
    }

    return rootfses[image.appc().name()]->root;
  }

  process::Future<bool> unmocked_destroy(
      const ContainerID& containerId)
  {
    return true;
  }

private:
  hashmap<std::string, process::Shared<Rootfs>> rootfses;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_PROVISIONER_HPP__
