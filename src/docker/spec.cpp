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
#include <stout/none.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include "docker/spec.hpp"

using std::string;

namespace docker {
namespace spec {
namespace v1 {

Option<Error> validate(const ImageManifest& manifest)
{
  // TODO(gilbert): Add validations.
  return None();
}


Try<ImageManifest> parse(const JSON::Object& json)
{
  Try<ImageManifest> manifest = protobuf::parse<ImageManifest>(json);
  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  Option<Error> error = validate(manifest.get());
  if (error.isSome()) {
    return Error("Docker v1 image manifest validation failed: " +
                 error.get().message);
  }

  return manifest.get();
}

} // namespace v1 {


namespace v2 {

Option<Error> validate(const ImageManifest& manifest)
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
    return Error("The size of 'fsLayers' should be equal "
                 "to the size of 'history'");
  }

  // Verify 'fsLayers' field.
  foreach (const ImageManifest::FsLayer& fslayer, manifest.fslayers()) {
    const string& blobSum = fslayer.blobsum();
    if (!strings::contains(blobSum, ":")) {
      return Error("Incorrect 'blobSum' format: " + blobSum);
    }
  }

  return None();
}


Try<ImageManifest> parse(const JSON::Object& json)
{
  Try<ImageManifest> manifest = protobuf::parse<ImageManifest>(json);
  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  Option<Error> error = validate(manifest.get());
  if (error.isSome()) {
    return Error("Docker v2 image manifest validation failed: " +
                 error.get().message);
  }

  return manifest.get();
}

} // namespace v2 {
} // namespace spec {
} // namespace docker {
