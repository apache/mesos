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

// Validate if the specified image manifest conforms to the Docker spec.
Option<Error> validateManifest(const docker::DockerImageManifest& manifest);

// TODO(Gilbert): add validations here, e.g., Manifest, Blob, Layout, ImageID.

// Parse the DockerImageManifest from the specified JSON object.
Try<docker::DockerImageManifest> parse(const JSON::Object& json);

} // namespace spec {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_DOCKER_SPEC_HPP__
