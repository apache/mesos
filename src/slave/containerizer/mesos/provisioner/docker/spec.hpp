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

#ifndef __PROVISIONER_DOCKER_SPEC_HPP__
#define __PROVISIONER_DOCKER_SPEC_HPP__

#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>

#include <mesos/mesos.hpp>

#include "slave/containerizer/mesos/provisioner/docker/message.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace spec {

namespace v1 {

// Validate if the specified v1 image manifest conforms to
// the Docker v1 spec.
Option<Error> validate(const docker::v1::ImageManifest& manifest);

// Parse the v1::ImageManifest from the specified JSON object.
Try<docker::v1::ImageManifest> parse(const JSON::Object& json);

} // namespace v1 {


namespace v2 {

// Validate if the specified v2 image manifest conforms to
// the Docker v2 spec.
Option<Error> validate(const docker::v2::ImageManifest& manifest);

// TODO(gilbert): Add validations here, e.g., Manifest, Blob, Layout, ImageID.

// Parse the v2::ImageManifest from the specified JSON object.
Try<docker::v2::ImageManifest> parse(const JSON::Object& json);

} // namespace v2 {

} // namespace spec {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_DOCKER_SPEC_HPP__
