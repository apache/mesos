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

#ifndef __POSIX_DISK_ISOLATOR_HPP__
#define __POSIX_DISK_ISOLATOR_HPP__

#include <string>

#include <process/owned.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.
class DiskUsageCollectorProcess;


// Responsible for collecting disk usage for paths, while ensuring
// that an interval elapses between each collection.
class DiskUsageCollector
{
public:
  DiskUsageCollector(const Duration& interval);
  ~DiskUsageCollector();

  // Returns the disk usage rooted at 'path'. The user can discard the
  // returned future to cancel the check.
  process::Future<Bytes> usage(
      const std::string& path,
      const std::vector<std::string>& excludes);

private:
  DiskUsageCollectorProcess* process;
};


// This isolator monitors the disk usage for containers, and reports
// ContainerLimitation when a container exceeds its disk quota. This
// leverages the DiskUsageCollector to ensure that we don't induce too
// much CPU usage and disk caching effects from running 'du' too
// often.
//
// NOTE: Currently all containers are processed in the same queue,
// which means that when a container starts, it could take many disk
// collection intervals until any data is available in the resource
// usage statistics!
//
// TODO(jieyu): Consider handling each container independently, or
// triggering an initial collection when the container starts, to
// ensure that we have usage statistics without a large delay.
class PosixDiskIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  ~PosixDiskIsolatorProcess() override;

  bool supportsNesting() override;
  bool supportsStandalone() override;

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid) override;

  process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId) override;

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
  PosixDiskIsolatorProcess(const Flags& flags);

  process::Future<Bytes> collect(
      const ContainerID& containerId,
      const std::string& path);

  void _collect(
      const ContainerID& containerId,
      const std::string& path,
      const process::Future<Bytes>& future);

  const Flags flags;
  DiskUsageCollector collector;

  struct Info
  {
    explicit Info(const std::string& _directory)
      : directories({_directory}), sandbox(_directory) {}

    Bytes ephemeralUsage() const;

    // Save the executor ephemeral storage (sandbox and rootfs)
    // directories so that we know where to collect disk usage
    // for disk resources without DiskInfo.
    hashset<std::string> directories;

    std::string sandbox;

    process::Promise<mesos::slave::ContainerLimitation> limitation;

    // The keys of the hashmaps contain the executor working directory
    // above, and optionally paths of volumes used by the container.
    // For each path, we maintain its quota and its last usage.
    struct PathInfo
    {
      ~PathInfo();

      Resources quota;
      process::Future<Bytes> usage;
      Option<Bytes> lastUsage;
    };

    hashmap<std::string, PathInfo> paths;
  };

  hashmap<ContainerID, process::Owned<Info>> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __POSIX_DISK_ISOLATOR_HPP__
