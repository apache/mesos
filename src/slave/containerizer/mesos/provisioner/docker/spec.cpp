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
Option<Error> validateManifest(const docker::v2::ImageManifest& manifest)
{
  // Validate required fields are present,
  // e.g., repeated fields that has to be >= 1.
  if (manifest.fslayers_size() <= 0) {
    return Error("'fsLayers' field size must be at least one");
  }

  if (manifest.history_size() <= 0) {
    return Error("'history' field size must be at least one");
  }

  if (manifest.signatures_size() <= 0) {
    return Error("'signatures' field size must be at least one");
  }

  // Verify that blobSum and v1Compatibility numbers are equal.
  if (manifest.fslayers_size() != manifest.history_size()) {
    return Error("There should be equal size of 'fsLayers' "
                 "with corresponding 'history'");
  }

  // FsLayers field validation.
  foreach (const docker::v2::ImageManifest::FsLayers& fslayer,
           manifest.fslayers()) {
    const string& blobSum = fslayer.blobsum();
    if (!strings::contains(blobSum, ":")) {
      return Error("Incorrect 'blobSum' format: " + blobSum);
    }
  }

  return None();
}


Try<docker::v2::ImageManifest> parse(const JSON::Object& json)
{
  Try<docker::v2::ImageManifest> manifest =
    protobuf::parse<docker::v2::ImageManifest>(json);

  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  Option<Error> error = validateManifest(manifest.get());
  if (error.isSome()) {
    return Error("Docker v2 Image Manifest Validation failed: " +
                 error.get().message);
  }

  return manifest.get();
}

} // namespace spec {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
