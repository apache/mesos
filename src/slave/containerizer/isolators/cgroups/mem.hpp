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

#ifndef __MEM_ISOLATOR_HPP__
#define __MEM_ISOLATOR_HPP__

#include <mesos/resources.hpp>

#include <process/future.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "mesos/resources.hpp"

#include "slave/containerizer/isolator.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {


class CgroupsMemIsolatorProcess : public IsolatorProcess
{
public:
  static Try<Isolator*> create(const Flags& flags);

  virtual ~CgroupsMemIsolatorProcess();

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
  CgroupsMemIsolatorProcess(const Flags& flags, const std::string& hierarchy);

  virtual process::Future<Nothing> _cleanup(const ContainerID& containerId);

  struct Info
  {
    Info(const ContainerID& _containerId, const std::string& _cgroup)
      : containerId(_containerId), cgroup(_cgroup) {}

    const ContainerID containerId;
    const std::string cgroup;
    Option<pid_t> pid;

    process::Promise<Limitation> limitation;

    // Used to cancel the OOM listening.
    process::Future<uint64_t> oomNotifier;
  };

  // Start listening on OOM events. This function will create an
  // eventfd and start polling on it.
  void oomListen(const ContainerID& containerId);

  // This function is invoked when the polling on eventfd has a
  // result.
  void oomWaited(
      const ContainerID& containerId,
      const process::Future<uint64_t>& future);

  // This function is invoked when the OOM event happens.
  void oom(const ContainerID& containerId);

  const Flags flags;

  // The path to the cgroups subsystem hierarchy root.
  const std::string hierarchy;

  hashmap<ContainerID, Info*> infos;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MEM_ISOLATOR_HPP__
