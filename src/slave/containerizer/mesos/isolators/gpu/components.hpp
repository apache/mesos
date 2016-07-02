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

#ifndef __NVIDIA_GPU_COMPONENTS_HPP__
#define __NVIDIA_GPU_COMPONENTS_HPP__

#ifdef __linux__
#include "slave/containerizer/mesos/isolators/gpu/allocator.hpp"
#include "slave/containerizer/mesos/isolators/gpu/volume.hpp"
#endif

namespace mesos {
namespace internal {
namespace slave {

// Defines the set of Nvidia components needed by containerizers.
//
// We provide this level of indirection in order to have single
// optional argument passed to containerizers and to avoid using
// OS #ifdefs in the containerizer signatures.
struct NvidiaComponents
{
#ifdef __linux__
  NvidiaComponents(
    const NvidiaGpuAllocator& _allocator,
    const NvidiaVolume& _volume)
    : allocator(_allocator),
      volume(_volume) {}

  NvidiaGpuAllocator allocator;
  NvidiaVolume volume;
#endif
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __NVIDIA_GPU_COMPONENTS_HPP__
