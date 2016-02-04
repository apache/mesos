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

#include <mesos/appc/spec.hpp>

#include "slave/containerizer/mesos/provisioner/appc/cache.hpp"

using std::string;

namespace spec = appc::spec;

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

Try<CachedImage> CachedImage::create(const string& imagePath)
{
  Option<Error> error = spec::validateLayout(imagePath);
  if (error.isSome()) {
    return Error("Invalid image layout: " + error.get().message);
  }

  string imageId = Path(imagePath).basename();

  error = spec::validateImageID(imageId);
  if (error.isSome()) {
    return Error("Invalid image ID: " + error.get().message);
  }

  Try<string> read = os::read(spec::getImageManifestPath(imagePath));
  if (read.isError()) {
    return Error("Failed to read manifest: " + read.error());
  }

  Try<spec::ImageManifest> manifest = spec::parse(read.get());
  if (manifest.isError()) {
    return Error("Failed to parse manifest: " + manifest.error());
  }

  return CachedImage(manifest.get(), imageId, imagePath);
}


string CachedImage::rootfs() const
{
  return path::join(path, "rootfs");
}

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
