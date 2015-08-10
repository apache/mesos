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

#include <string>

#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include "slave/containerizer/provisioners/appc/spec.hpp"

#include "tests/utils.hpp"

using std::string;
using std::vector;

using namespace process;

using namespace mesos::internal::slave::appc;

namespace mesos {
namespace internal {
namespace tests {

class AppcProvisionerTest : public TemporaryDirectoryTest {};


TEST_F(AppcProvisionerTest, ValidateImageManifest)
{
  JSON::Value manifest = JSON::parse(
      "{"
      "  \"acKind\": \"ImageManifest\","
      "  \"acVersion\": \"0.6.1\","
      "  \"name\": \"foo.com/bar\","
      "  \"labels\": ["
      "    {"
      "      \"name\": \"version\","
      "      \"value\": \"1.0.0\""
      "    },"
      "    {"
      "      \"name\": \"arch\","
      "      \"value\": \"amd64\""
      "    },"
      "    {"
      "      \"name\": \"os\","
      "      \"value\": \"linux\""
      "    }"
      "  ],"
      "  \"annotations\": ["
      "    {"
      "      \"name\": \"created\","
      "      \"value\": \"1438983392\""
      "    }"
      "  ]"
      "}").get();

  EXPECT_SOME(spec::parse(stringify(manifest)));

  // Incorrect acKind for image manifest.
  manifest = JSON::parse(
      "{"
      "  \"acKind\": \"PodManifest\","
      "  \"acVersion\": \"0.6.1\","
      "  \"name\": \"foo.com/bar\""
      "}").get();

  EXPECT_ERROR(spec::parse(stringify(manifest)));
}


TEST_F(AppcProvisionerTest, ValidateLayout)
{
  string image = os::getcwd();

  JSON::Value manifest = JSON::parse(
      "{"
      "  \"acKind\": \"ImageManifest\","
      "  \"acVersion\": \"0.6.1\","
      "  \"name\": \"foo.com/bar\""
      "}").get();

  ASSERT_SOME(os::write(path::join(image, "manifest"), stringify(manifest)));

  // Missing rootfs.
  EXPECT_SOME(spec::validateLayout(image));

  ASSERT_SOME(os::mkdir(path::join(image, "rootfs", "tmp")));
  ASSERT_SOME(os::write(path::join(image, "rootfs", "tmp", "test"), "test"));

  EXPECT_NONE(spec::validateLayout(image));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
