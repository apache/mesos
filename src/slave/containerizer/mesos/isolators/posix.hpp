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

#ifndef __POSIX_ISOLATOR_HPP__
#define __POSIX_ISOLATOR_HPP__

#include <process/future.hpp>
#include <process/id.hpp>

#include <stout/hashmap.hpp>
#include <stout/os.hpp>

#include <stout/os/pstree.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

#include "usage/usage.hpp"

namespace mesos {
namespace internal {
namespace slave {

// A basic MesosIsolatorProcess that keeps track of the pid but
// doesn't do any resource isolation. Subclasses must implement
// usage() for their appropriate resource(s).
class PosixIsolatorProcess : public MesosIsolatorProcess
{
public:
  process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& state,
      const hashset<ContainerID>& orphans) override
  {
    foreach (const mesos::slave::ContainerState& run, state) {
      // This should (almost) never occur: see comment in
      // SubprocessLauncher::recover().
      if (pids.contains(run.container_id())) {
        return process::Failure("Container already recovered");
      }

      pids.put(run.container_id(), static_cast<pid_t>(run.pid()));

      process::Owned<process::Promise<mesos::slave::ContainerLimitation>>
        promise(new process::Promise<mesos::slave::ContainerLimitation>());
      promises.put(run.container_id(), promise);
    }

    return Nothing();
  }

  process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override
  {
    if (promises.contains(containerId)) {
      return process::Failure("Container " + stringify(containerId) +
                              " has already been prepared");
    }

    process::Owned<process::Promise<mesos::slave::ContainerLimitation>> promise(
        new process::Promise<mesos::slave::ContainerLimitation>());
    promises.put(containerId, promise);

    return None();
  }

  process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid) override
  {
    if (!promises.contains(containerId)) {
      return process::Failure("Unknown container: " + stringify(containerId));
    }

    pids.put(containerId, pid);

    return Nothing();
  }

  process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId) override
  {
    if (!promises.contains(containerId)) {
      return process::Failure("Unknown container: " + stringify(containerId));
    }

    return promises[containerId]->future();
  }

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {}) override
  {
    if (!promises.contains(containerId)) {
      return process::Failure("Unknown container: " + stringify(containerId));
    }

    // No resources are actually isolated so nothing to do.
    return Nothing();
  }

  process::Future<Nothing> cleanup(const ContainerID& containerId) override
  {
    if (!promises.contains(containerId)) {
      VLOG(1) << "Ignoring cleanup request for unknown container "
              << containerId;

      return Nothing();
    }

    // TODO(idownes): We should discard the container's promise here to signal
    // to anyone that holds the future from watch().
    promises.erase(containerId);

    pids.erase(containerId);

    return Nothing();
  }

protected:
  hashmap<ContainerID, pid_t> pids;
  hashmap<ContainerID,
          process::Owned<process::Promise<mesos::slave::ContainerLimitation>>>
    promises;
};


class PosixCpuIsolatorProcess : public PosixIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags)
  {
    process::Owned<MesosIsolatorProcess> process(
        new PosixCpuIsolatorProcess());

    return new MesosIsolator(process);
  }

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) override
  {
    if (!pids.contains(containerId)) {
      LOG(WARNING) << "No resource usage for unknown container '"
                   << containerId << "'";
      return ResourceStatistics();
    }

    // Use 'mesos-usage' but only request 'cpus_' values.
    Try<ResourceStatistics> usage =
      mesos::internal::usage(pids.at(containerId), false, true);
    if (usage.isError()) {
      return process::Failure(usage.error());
    }
    return usage.get();
  }

protected:
  PosixCpuIsolatorProcess()
    : ProcessBase(process::ID::generate("posix-cpu-isolator")) {}
};

class PosixMemIsolatorProcess : public PosixIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags)
  {
    process::Owned<MesosIsolatorProcess> process(
        new PosixMemIsolatorProcess());

    return new MesosIsolator(process);
  }

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) override
  {
    if (!pids.contains(containerId)) {
      LOG(WARNING) << "No resource usage for unknown container '"
                   << containerId << "'";
      return ResourceStatistics();
    }

    // Use 'mesos-usage' but only request 'mem_' values.
    Try<ResourceStatistics> usage =
      mesos::internal::usage(pids.at(containerId), true, false);
    if (usage.isError()) {
      return process::Failure(usage.error());
    }
    return usage.get();
  }

protected:
  PosixMemIsolatorProcess()
    : ProcessBase(process::ID::generate("posix-mem-isolator")) {}
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __POSIX_ISOLATOR_HPP__
