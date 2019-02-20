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

#ifndef __NVIDIA_VOLUME_HPP__
#define __NVIDIA_VOLUME_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <mesos/docker/spec.hpp>

#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {

// Provides a consolidated volume of binaries and libraries
// from the user-space portion of the Nvidia GPU drivers.
// We follow the approach taken by Nvidia's docker plugin:
//   https://github.com/NVIDIA/nvidia-docker/
class NvidiaVolume
{
public:
  // Returns the Nvidia volume. Once we create the volume it is
  // never deleted until reboot. This means that if an agent
  // crashes and tries to recreate the volume it will only ever
  // *add* new libraries to it (nothing will be deleted from it).
  // This prevents already-running containers from having
  // whatever libraries they depend on ripped out from under them.
  static Try<NvidiaVolume> create();

  const std::string& HOST_PATH() const;
  const std::string& CONTAINER_PATH() const;
  Environment ENV(const ::docker::spec::v1::ImageManifest& manifest) const;

  // Returns whether the container based on a docker image should have the
  // volume injected.
  bool shouldInject(const ::docker::spec::v1::ImageManifest& manifest) const;

private:
  NvidiaVolume() = delete;

  NvidiaVolume(const std::string& _hostPath,
               const std::string& _containerPath)
    : hostPath(_hostPath),
      containerPath(_containerPath) {}

  std::string hostPath;
  std::string containerPath;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __NVIDIA_GPU_VOLUME_HPP__
