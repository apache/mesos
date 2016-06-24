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

#ifndef __NVIDIA_GPU_ALLOCATOR_HPP__
#define __NVIDIA_GPU_ALLOCATOR_HPP__

#include <iosfwd>
#include <memory>
#include <set>

#include <mesos/resources.hpp>

#include <process/future.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Simple abstraction of a GPU.
//
// TODO(klueska): Once we have a generic "Device" type it will look
// very similar to this. At that point we should build redefine this
// abstraction in terms of it.
struct Gpu
{
  unsigned int major;
  unsigned int minor;
};


// Manages the allocation of GPU devices. This can be shared across
// components (e.g. containerizers) to ensure that components do not
// perform conflicting allocation of GPU devices.
class NvidiaGpuAllocator
{
public:
  NvidiaGpuAllocator() = delete;

  static Try<Resources> resources(const Flags& flags);

  static Try<NvidiaGpuAllocator> create(
      const Flags& flags,
      const Resources& resources);

  const std::set<Gpu>& total() const;

  process::Future<std::set<Gpu>> allocate(size_t count);
  process::Future<Nothing> allocate(const std::set<Gpu>& gpus);
  process::Future<Nothing> deallocate(const std::set<Gpu>& gpus);

private:
  NvidiaGpuAllocator(const std::set<Gpu>& gpus);

  // Forward declaration.
  struct Data;

  std::shared_ptr<Data> data;
};


bool operator<(const Gpu& left, const Gpu& right);
bool operator>(const Gpu& left, const Gpu& right);
bool operator<=(const Gpu& left, const Gpu& right);
bool operator>=(const Gpu& left, const Gpu& right);
bool operator==(const Gpu& left, const Gpu& right);
bool operator!=(const Gpu& left, const Gpu& right);

std::ostream& operator<<(std::ostream& stream, const Gpu& gpu);

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __NVIDIA_GPU_ALLOCATOR_HPP__
