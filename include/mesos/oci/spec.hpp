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

#ifndef __MESOS_OCI_SPEC_HPP__
#define __MESOS_OCI_SPEC_HPP__

#include <mesos/oci/spec.pb.h>

namespace oci {
namespace spec {
namespace image {
namespace v1 {

// Constant strings for OCI image media types:
// https://github.com/opencontainers/image-spec/blob/master/media-types.md
constexpr char MEDIA_TYPE_INDEX[] =
    "application/vnd.oci.image.index.v1+json";

constexpr char MEDIA_TYPE_MANIFEST[] =
    "application/vnd.oci.image.manifest.v1+json";

constexpr char MEDIA_TYPE_CONFIG[] =
    "application/vnd.oci.image.config.v1+json";

constexpr char MEDIA_TYPE_LAYER[] =
    "application/vnd.oci.image.layer.v1.tar";

constexpr char MEDIA_TYPE_LAYER_GZIP[] =
    "application/vnd.oci.image.layer.v1.tar+gzip";

constexpr char MEDIA_TYPE_NONDIST_LAYER[] =
    "application/vnd.oci.image.layer.nondistributable.v1.tar";

constexpr char MEDIA_TYPE_NONDIST_LAYER_GZIP[] =
    "application/vnd.oci.image.layer.nondistributable.v1.tar+gzip";

// Rootfs type of OCI image configuration.
constexpr char ROOTFS_TYPE[] = "layers";

/**
 * Returns the OCI v1 descriptor, image index, image manifest
 * and image configuration from the given string.
 */
template <typename T>
Try<T> parse(const std::string& s);

} // namespace v1 {
} // namespace image {
} // namespace spec {
} // namespace oci {

#endif // __MESOS_OCI_SPEC_HPP__
