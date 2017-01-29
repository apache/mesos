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

#ifndef __TEST_STORE_HPP__
#define __TEST_STORE_HPP__

#include <gmock/gmock.h>

#include <stout/option.hpp>

#include <process/future.hpp>
#include <process/shared.hpp>

#include "slave/containerizer/mesos/provisioner/store.hpp"

#include "tests/containerizer/rootfs.hpp"

namespace mesos {
namespace internal {
namespace tests {

class TestStore : public slave::Store
{
public:
  TestStore(const hashmap<std::string, process::Shared<Rootfs>>& _rootfses)
      : rootfses(_rootfses)
  {
    using testing::_;
    using testing::DoDefault;
    using testing::Invoke;

    ON_CALL(*this, recover())
      .WillByDefault(Invoke(this, &TestStore::unmocked_recover));
    EXPECT_CALL(*this, recover())
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, get(_, _))
      .WillByDefault(Invoke(this, &TestStore::unmocked_get));
    EXPECT_CALL(*this, get(_, _))
      .WillRepeatedly(DoDefault());
  }

  MOCK_METHOD0(
      recover,
      process::Future<Nothing>());

  MOCK_METHOD1(
      get,
      process::Future<slave::ImageInfo>(
          const Image& image,
          const std::string& backend));

  process::Future<Nothing> unmocked_recover()
  {
    return Nothing();
  }

  process::Future<slave::ImageInfo> unmocked_get(
      const Image& image,
      const std::string& backend)
  {
    if (!image.has_appc()) {
      return process::Failure("Expecting APPC image");
    }

    Option<process::Shared<Rootfs>> rootfs = rootfses.at(image.appc().name());
    if (rootfs.isSome()) {
      return slave::ImageInfo{
          std::vector<std::string>({rootfs.get()->root}), None()};
    }

    return process::Failure("Cannot find image '" + image.appc().name());
  }

private:
  hashmap<std::string, process::Shared<Rootfs>> rootfses;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_STORE_HPP__
