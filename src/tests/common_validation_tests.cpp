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

#include <stout/gtest.hpp>

#include "common/validation.hpp"

namespace mesos {
namespace internal {
namespace tests {

TEST(CommonValidationTest, Volume)
{
  // One of the 'host_path', 'image' or 'source' must be set.
  {
    Volume volume;
    volume.set_container_path("container_path");
    volume.set_mode(Volume::RW);

    EXPECT_SOME_EQ(
        Error("Only one of them should be set: "
              "'host_path', 'image' and 'source'"),
        common::validation::validateVolume(volume));

    volume.set_host_path("host_path");
    volume.mutable_image();

    EXPECT_SOME_EQ(
        Error("Only one of them should be set: "
              "'host_path', 'image' and 'source'"),
        common::validation::validateVolume(volume));
  }

  // Source 'type' is compatible with the corresponding fields.
  {
    Volume volume;
    volume.set_container_path("container_path");
    volume.set_mode(Volume::RW);

    Volume::Source* source = volume.mutable_source();
    source->set_type(Volume::Source::DOCKER_VOLUME);

    EXPECT_SOME_EQ(
        Error("'source.docker_volume' is not set for "
              "DOCKER_VOLUME volume"),
        common::validation::validateVolume(volume));

    source->set_type(Volume::Source::HOST_PATH);

    EXPECT_SOME_EQ(
        Error("'source.host_path' is not set for "
              "HOST_PATH volume"),
        common::validation::validateVolume(volume));

    source->set_type(Volume::Source::SANDBOX_PATH);

    EXPECT_SOME_EQ(
        Error("'source.sandbox_path' is not set for "
              "SANDBOX_PATH volume"),
        common::validation::validateVolume(volume));

    source->set_type(Volume::Source::SECRET);

    EXPECT_SOME_EQ(
        Error("'source.secret' is not set for "
              "SECRET volume"),
        common::validation::validateVolume(volume));
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
