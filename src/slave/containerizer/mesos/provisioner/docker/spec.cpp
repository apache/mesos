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

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include "slave/containerizer/mesos/provisioner/docker/spec.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace spec {

// Validate if the specified image manifest conforms to the Docker spec.
Option<Error> validateManifest(const DockerImageManifest& manifest)
{
  // Validate required fields are present,
  // e.g., repeated fields that has to be >= 1.
  if (manifest.fslayers_size() <= 0) {
    return Error("FsLayers field must have at least one blobSum");
  }

  if (manifest.history_size() <= 0) {
    return Error("History field must have at least one v1Compatibility");
  }

  if (manifest.signatures_size() <= 0) {
    return Error("Signatures field must have at least one signature");
  }

  // Verify that blobSum and v1Compatibility numbers are equal.
  if (manifest.fslayers_size() != manifest.history_size()) {
    return Error("Size of blobSum and v1Compatibility must be equal");
  }

  // FsLayers field validation.
  foreach (const docker::DockerImageManifest::FsLayers& fslayer,
           manifest.fslayers()) {
    const string& blobSum = fslayer.blobsum();
    if (!strings::contains(blobSum, ":")) {
      return Error("Incorrect blobSum format");
    }
  }

  return None();
}


Try<docker::DockerImageManifest> parse(const JSON::Object& json)
{
  Try<docker::DockerImageManifest> manifest =
    protobuf::parse<docker::DockerImageManifest>(json);

  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  Option<Error> error = validateManifest(manifest.get());
  if (error.isSome()) {
    return Error("Docker Image Manifest Validation failed: " +
                 error.get().message);
  }

  return manifest.get();
}

} // namespace spec {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
