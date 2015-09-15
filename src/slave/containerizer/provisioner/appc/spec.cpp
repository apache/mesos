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

#include <stout/os/stat.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include "slave/containerizer/provisioner/appc/paths.hpp"
#include "slave/containerizer/provisioner/appc/spec.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace appc {
namespace spec {

Option<Error> validateManifest(const AppcImageManifest& manifest)
{
  // TODO(idownes): Validate that required fields are present when
  // this cannot be expressed in the protobuf specification, e.g.,
  // repeated fields with >= 1.
  // TODO(xujyan): More thorough type validation:
  // https://github.com/appc/spec/blob/master/spec/types.md
  if (manifest.ackind() != "ImageManifest") {
    return Error("Incorrect acKind field: " + manifest.ackind());
  }

  return None();
}


Option<Error> validateImageID(const string& imageId)
{
  if (!strings::startsWith(imageId, "sha512-")) {
    return Error("Image ID needs to start with sha512-");
  }

  string hash = strings::remove(imageId, "sha512-", strings::PREFIX);
  if (hash.length() != 128) {
    return Error("Invalid hash length for: " + hash);
  }

  return None();
}


Option<Error> validateLayout(const string& imagePath)
{
  if (!os::stat::isdir(paths::getImageRootfsPath(imagePath))) {
    return Error("No rootfs directory found in image layout");
  }

  if (!os::stat::isfile(paths::getImageManifestPath(imagePath))) {
    return Error("No manifest found in image layout");
  }

  return None();
}


Try<AppcImageManifest> parse(const string& value)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(value);
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  Try<AppcImageManifest> manifest =
    protobuf::parse<AppcImageManifest>(json.get());

  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  Option<Error> error = validateManifest(manifest.get());
  if (error.isSome()) {
    return Error("Schema validation failed: " + error.get().message);
  }

  return manifest.get();
}

} // namespace spec {
} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
