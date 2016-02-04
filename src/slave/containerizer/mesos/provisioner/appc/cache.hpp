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

#ifndef __APPC_PROVISIONER_CACHE_HPP__
#define __APPC_PROVISIONER_CACHE_HPP__

#include <string>

#include <mesos/appc/spec.hpp>
#include <mesos/mesos.pb.h>

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

// Defines a locally cached image (which has passed validation).
struct CachedImage
{
  static Try<CachedImage> create(const std::string& imagePath);

  CachedImage(
      const ::appc::spec::ImageManifest& _manifest,
      const std::string& _id,
      const std::string& _path)
    : manifest(_manifest), id(_id), path(_path) {}

  std::string rootfs() const;

  const ::appc::spec::ImageManifest manifest;

  // Image ID of the format "sha512-value" where "value" is the hex
  // encoded string of the sha512 digest of the uncompressed tar file
  // of the image.
  const std::string id;

  // Absolute path to the extracted image.
  const std::string path;
};

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __APPC_PROVISIONER_CACHE_HPP__
