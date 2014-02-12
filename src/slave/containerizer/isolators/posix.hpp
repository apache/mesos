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

#ifndef __POSIX_ISOLATOR_HPP__
#define __POSIX_ISOLATOR_HPP__

#include <stout/hashmap.hpp>
#include <stout/os/pstree.hpp>

#include <process/future.hpp>

#include "slave/containerizer/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// A basic IsolatorProcess that keeps track of the pid but doesn't do any
// resource isolation. Subclasses must implement usage() for their appropriate
// resource(s).
class PosixIsolatorProcess : public IsolatorProcess
{
public:
  virtual process::Future<Nothing> recover(
      const std::list<state::RunState>& state)
  {
    foreach (const state::RunState& run, state) {
      if (!run.id.isSome()) {
        return process::Failure("ContainerID is required to recover");
      }

      if (!run.forkedPid.isSome()) {
        return process::Failure("Executor pid is required to recover");
      }

      // This should (almost) never occur: see comment in
      // PosixLauncher::recover().
      if (pids.contains(run.id.get())) {
        return process::Failure("Container already recovered");
      }

      pids.put(run.id.get(), run.forkedPid.get());

      process::Owned<process::Promise<Limitation> > promise(
          new process::Promise<Limitation>());
      promises.put(run.id.get(), promise);
    }

    return Nothing();
  }

  virtual process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo)
  {
    if (promises.contains(containerId)) {
      return process::Failure("Container " + stringify(containerId) +
                              " has already been prepared");
    }

    process::Owned<process::Promise<Limitation> > promise(
        new process::Promise<Limitation>());
    promises.put(containerId, promise);

    return Nothing();
  }

  virtual process::Future<Option<CommandInfo> > isolate(
      const ContainerID& containerId,
      pid_t pid)
  {
    if (!promises.contains(containerId)) {
      return process::Failure("Unknown container: " + stringify(containerId));
    }

    pids.put(containerId, pid);

    return None();
  }

  virtual process::Future<Limitation> watch(
      const ContainerID& containerId)
  {
    if (!promises.contains(containerId)) {
      return process::Failure("Unknown container: " + stringify(containerId));
    }

    return promises[containerId]->future();
  }

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources)
  {
    if (!promises.contains(containerId)) {
      return process::Failure("Unknown container: " + stringify(containerId));
    }

    // No resources are actually isolated so nothing to do.
    return Nothing();
  }

  virtual process::Future<Nothing> cleanup(const ContainerID& containerId)
  {
    if (!promises.contains(containerId)) {
      return process::Failure("Unknown container: " + stringify(containerId));
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
          process::Owned<process::Promise<Limitation> > > promises;
};


class PosixCpuIsolatorProcess : public PosixIsolatorProcess
{
public:
  static Try<Isolator*> create(const Flags& flags)
  {
    process::Owned<IsolatorProcess> process(new PosixCpuIsolatorProcess());

    return new Isolator(process);
  }

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId)
  {
    if (!pids.contains(containerId)) {
      LOG(WARNING) << "No resource usage for unknown container '"
                   << containerId << "'";
      return ResourceStatistics();
    }

    Try<os::ProcessTree> tree = os::pstree(pids.get(containerId).get());

    if (!tree.isSome()) {
      return ResourceStatistics();
    }

    ResourceStatistics result;

    std::deque<os::ProcessTree> trees;
    trees.push_back(tree.get());

    while (!trees.empty()) {
      os::ProcessTree root = trees.front();

      // We only show utime and stime when both are available, otherwise
      // we're exposing a partial view of the CPU times.
      if (root.process.utime.isSome() && root.process.stime.isSome()) {
        result.set_cpus_user_time_secs(
            result.cpus_user_time_secs() + root.process.utime.get().secs());
        result.set_cpus_system_time_secs(
            result.cpus_system_time_secs() + root.process.stime.get().secs());
      }

      trees.pop_front();
      foreach (const os::ProcessTree& child, root.children) {
        trees.push_back(child);
      }
    }

    return result;
  }

private:
  PosixCpuIsolatorProcess() {}
};


class PosixMemIsolatorProcess : public PosixIsolatorProcess
{
public:
  static Try<Isolator*> create(const Flags& flags)
  {
    process::Owned<IsolatorProcess> process(new PosixMemIsolatorProcess());

    return new Isolator(process);
  }

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId)
  {
    if (!pids.contains(containerId)) {
      LOG(WARNING) << "No resource usage for unknown container '"
                   << containerId << "'";
      return ResourceStatistics();
    }

    Try<os::ProcessTree> tree = os::pstree(pids.get(containerId).get());

    if (!tree.isSome()) {
      return ResourceStatistics();
    }

    ResourceStatistics result;

    std::deque<os::ProcessTree> trees;
    trees.push_back(tree.get());

    while (!trees.empty()) {
      os::ProcessTree root = trees.front();

      if (root.process.rss.isSome()) {
        result.set_mem_rss_bytes(
            result.mem_rss_bytes() + root.process.rss.get().bytes());
      }

      trees.pop_front();
      foreach (const os::ProcessTree& child, root.children) {
        trees.push_back(child);
      }
    }

    return result;
  }

private:
  PosixMemIsolatorProcess() {}
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __POSIX_ISOLATOR_HPP__
