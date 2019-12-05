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

#ifndef __XFS_DISK_ISOLATOR_HPP__
#define __XFS_DISK_ISOLATOR_HPP__

#include <string>
#include <utility>

#include <process/owned.hpp>

#include <process/metrics/push_gauge.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/xfs/utils.hpp"

namespace mesos {
namespace internal {
namespace slave {

class XfsDiskIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  ~XfsDiskIsolatorProcess() override;

  process::PID<XfsDiskIsolatorProcess> self() const
  {
    return process::PID<XfsDiskIsolatorProcess>(this);
  }

  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans) override;

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

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

protected:
  void initialize() override;

private:
  struct Info
  {
    struct PathInfo
    {
      Bytes quota;
      const prid_t projectId;
      const Option<Resource::DiskInfo> disk;
    };

    Info(const std::string& directory, prid_t projectId)
    {
      paths.put(directory, PathInfo{Bytes(0), projectId, None()});
    }

    hashmap<std::string, PathInfo> paths;
    process::Promise<mesos::slave::ContainerLimitation> limitation;
  };

  struct ProjectRoots {
    std::string deviceName;
    hashset<std::string> directories;
  };

  XfsDiskIsolatorProcess(
      Duration watchInterval,
      xfs::QuotaPolicy quotaPolicy,
      const std::string& workDir,
      const IntervalSet<prid_t>& projectIds,
      Duration projectWatchInterval);

  // Responsible for validating a container hasn't broken the soft limit.
  void check();

  // Take the next project ID from the unallocated pool.
  Option<prid_t> nextProjectId();

  // Return this project ID to the unallocated pool.
  void returnProjectId(prid_t projectId);

  // Check which project IDs are currently in use and deallocate the ones
  // that are not.
  void reclaimProjectIds();

  Try<Nothing> scheduleProjectRoot(
      prid_t projectId, const std::string& rootDir);

  const Duration watchInterval;
  const Duration projectWatchInterval;
  xfs::QuotaPolicy quotaPolicy;
  const std::string workDir;
  const IntervalSet<prid_t> totalProjectIds;
  IntervalSet<prid_t> freeProjectIds;
  hashmap<ContainerID, process::Owned<Info>> infos;

  // Track the device and filesystem path of unused project IDs we want
  // to reclaim.
  hashmap<prid_t, ProjectRoots> scheduledProjects;

  // Metrics used by the XFS disk isolator.
  struct Metrics
  {
    Metrics();
    ~Metrics();

    process::metrics::PushGauge project_ids_total;
    process::metrics::PushGauge project_ids_free;
  } metrics;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __XFS_DISK_ISOLATOR_HPP__
