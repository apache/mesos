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

#include <mesos/resources.hpp>

#include <process/future.hpp>

#include <stout/hashmap.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/isolator.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {


// Use the Linux cpu cgroup controller for cpu isolation which uses the
// Completely Fair Scheduler (CFS).
// - cpushare implements proportionally weighted scheduling.
// - cfs implements hard quota based scheduling.
class CgroupsCpushareIsolatorProcess : public IsolatorProcess
{
public:
  static Try<Isolator*> create(const Flags& flags);

  virtual ~CgroupsCpushareIsolatorProcess();

  virtual process::Future<Nothing> recover(
      const std::list<state::RunState>& states);

  virtual process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo);

  virtual process::Future<Option<CommandInfo> > isolate(
      const ContainerID& containerId,
      pid_t pid);

  virtual process::Future<Limitation> watch(
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
      const hashmap<std::string, std::string>& hierarchies);

  virtual process::Future<Nothing> _cleanup(const ContainerID& containerId);

  struct Info
  {
    Info(const ContainerID& _containerId, const std::string& _cgroup)
      : containerId(_containerId), cgroup(_cgroup) {}

    const ContainerID containerId;
    const std::string cgroup;
    Option<pid_t> pid;

    process::Promise<Limitation> limitation;
  };

  const Flags flags;

  // Map from subsystem to hierarchy.
  hashmap<std::string, std::string> hierarchies;

  hashmap<ContainerID, Info*> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CPUSHARE_ISOLATOR_HPP__
