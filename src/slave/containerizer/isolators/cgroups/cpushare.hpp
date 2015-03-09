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

#ifndef __CPUSHARE_ISOLATOR_HPP__
#define __CPUSHARE_ISOLATOR_HPP__

#include <string>

#include <mesos/slave/isolator.hpp>

#include <stout/hashmap.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/isolators/cgroups/constants.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Use the Linux cpu cgroup controller for cpu isolation which uses the
// Completely Fair Scheduler (CFS).
// - cpushare implements proportionally weighted scheduling.
// - cfs implements hard quota based scheduling.
class CgroupsCpushareIsolatorProcess : public mesos::slave::IsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  virtual ~CgroupsCpushareIsolatorProcess();

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ExecutorRunState>& states);

  virtual process::Future<Option<CommandInfo> > prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user);

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid);

  virtual process::Future<mesos::slave::Limitation> watch(
      const ContainerID& containerId);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId);

private:
  CgroupsCpushareIsolatorProcess(
      const Flags& flags,
      const hashmap<std::string, std::string>& hierarchies,
      const std::vector<std::string>& subsystems);

  virtual process::Future<std::list<Nothing> > _cleanup(
      const ContainerID& containerId,
      const process::Future<std::list<Nothing> >& future);

  struct Info
  {
    Info(const ContainerID& _containerId, const std::string& _cgroup)
      : containerId(_containerId), cgroup(_cgroup) {}

    const ContainerID containerId;
    const std::string cgroup;
    Option<pid_t> pid;

    process::Promise<mesos::slave::Limitation> limitation;
  };

  const Flags flags;

  // Map from subsystem to hierarchy.
  hashmap<std::string, std::string> hierarchies;

  // Subsystems used for this isolator. Typically, there are two
  // elements in the vector: 'cpu' and 'cpuacct'. If cpu and cpuacct
  // systems are co-mounted (e.g., systems using systemd), then there
  // will be only one element in the vector which is 'cpu,cpuacct'.
  std::vector<std::string> subsystems;

  // TODO(bmahler): Use Owned<Info>.
  hashmap<ContainerID, Info*> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CPUSHARE_ISOLATOR_HPP__
