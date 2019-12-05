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

#ifndef __NVIDIA_GPU_ISOLATOR_HPP__
#define __NVIDIA_GPU_ISOLATOR_HPP__

#include <map>
#include <set>
#include <vector>

#include <process/future.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/gpu/allocator.hpp"
#include "slave/containerizer/mesos/isolators/gpu/components.hpp"
#include "slave/containerizer/mesos/isolators/gpu/volume.hpp"

namespace mesos {
namespace internal {
namespace slave {

// This isolator uses the cgroups devices subsystem to control
// access to Nvidia GPUs. Since this is the very first device
// isolator, it currently contains generic device isolation
// logic that needs to be pulled up into a generic device
// isolator.
//
// GPUs are allocated to containers in an arbitrary fashion.
// For example, if a container requires 2 GPUs, we will
// arbitrarily choose 2 from the GPUs that are available.
// This may not behave well if tasks within an executor use
// GPUs since we cannot identify which task are using which
// GPUs (i.e. when a task terminates, we may remove a GPU
// that is still being used by a different task!).
//
// Note that this isolator is not responsible for ensuring
// that the necessary Nvidia libraries are visible in the
// container. If filesystem isolation is not enabled, this
// means that the container can simply use the libraries
// available on the host. When filesystem isolation is
// enabled, it is the responsibility of the operator /
// application developer to ensure that the necessary
// libraries are visible to the container (note that they
// must be version compatible with the kernel driver on
// the host).
//
// TODO(klueska): To better support containers with a
// provisioned filesystem, we will need to add a mechanism
// for operators to inject the libraries as a volume into
// containers that require GPU access.
//
// TODO(klueska): If multiple containerizers are enabled,
// they need to co-ordinate their allocation of GPUs.
//
// TODO(klueska): Move generic device isolation logic
// out into its own component.
class NvidiaGpuIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(
      const Flags& flags,
      const NvidiaComponents& components);

  bool supportsNesting() override;
  bool supportsStandalone() override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {}) override;

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) override;

  process::Future<Nothing> cleanup(
      const ContainerID& containerId) override;

private:
  NvidiaGpuIsolatorProcess(
      const Flags& _flags,
      const std::string& hierarchy,
      const NvidiaGpuAllocator& _allocator,
      const NvidiaVolume& _volume,
      const std::map<Path, cgroups::devices::Entry>& _controlDeviceEntries);

  virtual process::Future<Option<mesos::slave::ContainerLaunchInfo>> _prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  process::Future<Nothing> _update(
      const ContainerID& containerId,
      const std::set<Gpu>& allocation);

  struct Info
  {
    Info(const ContainerID& _containerId, const std::string& _cgroup)
      : containerId(_containerId), cgroup(_cgroup) {}

    const ContainerID containerId;
    const std::string cgroup;
    std::set<Gpu> allocated;
  };

  const Flags flags;

  // The path to the cgroups subsystem hierarchy root.
  const std::string hierarchy;

  // TODO(bmahler): Use Owned<Info>.
  hashmap<ContainerID, Info*> infos;

  NvidiaGpuAllocator allocator;
  NvidiaVolume volume;

  const std::map<Path, cgroups::devices::Entry> controlDeviceEntries;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __NVIDIA_GPU_ISOLATOR_HPP__
