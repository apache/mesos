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

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include <mesos/appc/spec.hpp>

#include "slave/containerizer/mesos/provisioner/appc/paths.hpp"

using std::string;

namespace appc {
namespace spec {

Try<ImageManifest> parse(const string& value)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(value);
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  Try<ImageManifest> manifest = protobuf::parse<ImageManifest>(json.get());

  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  Option<Error> error = validateManifest(manifest.get());
  if (error.isSome()) {
    return Error("Schema validation failed: " + error->message);
  }

  return manifest.get();
}


string getImageRootfsPath(const string& imagePath)
{
  return path::join(imagePath, "rootfs");
}


string getImageManifestPath(const string& imagePath)
{
  return path::join(imagePath, "manifest");
}


Try<ImageManifest> getManifest(const string& imagePath)
{
  const string path = getImageManifestPath(imagePath);

  Try<string> read = os::read(path);
  if (read.isError()) {
    return Error(
        "Failed to read manifest from '" + path + "': " +
        read.error());
  }

  Try<ImageManifest> parseManifest = parse(read.get());
  if (parseManifest.isError()) {
    return Error(
        "Failed to parse manifest from '" + path + "': " +
        parseManifest.error());
  }

  return parseManifest.get();
}


Option<Error> validateManifest(const ImageManifest& manifest)
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
  if (!os::stat::isdir(getImageRootfsPath(imagePath))) {
    return Error("No rootfs directory found in image layout");
  }

  if (!os::stat::isfile(getImageManifestPath(imagePath))) {
    return Error("No manifest found in image layout");
  }

  return None();
}


Option<Error> validate(const string& imagePath)
{
  Option<Error> validate = validateLayout(imagePath);
  if (validate.isSome()) {
    return Error(
        "Image validation failed for image at '" + imagePath + "': " +
        validate->message);
  }

  Try<ImageManifest> manifest = getManifest(imagePath);
  if (manifest.isError()) {
    return Error(
        "Image validation failed for image at '" + imagePath + "': " +
        manifest.error());
  }

  validate = validateManifest(manifest.get());
  if (validate.isSome()) {
    return Error(
        "Image validation failed for image at '" + imagePath + "': " +
        validate->message);
  }

  validate = validateImageID(Path(imagePath).basename());
  if (validate.isSome()) {
    return Error(
        "Image validation failed for image at '" + imagePath + "': " +
         validate->message);
  }

  return None();
}

} // namespace spec {
} // namespace appc {
