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

#include <process/owned.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>

#include "slave/flags.hpp"
#include "slave/state.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "slave/containerizer/mesos/isolators/xfs/utils.hpp"

namespace mesos {
namespace internal {
namespace slave {

class XfsDiskIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  virtual ~XfsDiskIsolatorProcess();

  process::PID<XfsDiskIsolatorProcess> self() const
  {
    return process::PID<XfsDiskIsolatorProcess>(this);
  }

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId);

private:
  XfsDiskIsolatorProcess(
      const Flags& flags,
      const IntervalSet<prid_t>& projectIds);

  // Take the next project ID from the unallocated pool.
  Option<prid_t> nextProjectId();

  // Return this project ID to the unallocated pool.
  void returnProjectId(prid_t projectId);

  struct Info
  {
    explicit Info(const std::string& _directory, prid_t _projectId)
      : directory(_directory), quota(0),  projectId(_projectId) {}

    const std::string directory;
    Bytes quota;
    const prid_t projectId;
  };

  const Flags flags;
  const IntervalSet<prid_t> totalProjectIds;
  IntervalSet<prid_t> freeProjectIds;
  hashmap<ContainerID, process::Owned<Info>> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __XFS_DISK_ISOLATOR_HPP__
