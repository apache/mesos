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

#ifndef __MESOS_SLAVE_ISOLATOR_HPP__
#define __MESOS_SLAVE_ISOLATOR_HPP__

#include <list>
#include <string>

#include <mesos/resources.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace slave {

// Forward declaration.
class IsolatorProcess;

// Information when an executor is impacted by a resource limitation
// and should be terminated. Intended to support resources like memory
// where the Linux kernel may invoke the OOM killer, killing some/all
// of a container's processes.
struct Limitation
{
  Limitation(
      const Resources& _resources,
      const std::string& _message)
    : resources(_resources),
      message(_message) {}

  // Resources that triggered the limitation.
  // NOTE: 'Resources' is used here because the resource may span
  // multiple roles (e.g. `"mem(*):1;mem(role):2"`).
  Resources resources;

  // Description of the limitation.
  std::string message;
};


// This struct is derived from slave::state::RunState. It contains
// only those fields that are needed by Isolators for recovering the
// containers. The reason for not using RunState instead is to avoid
// any dependency on RunState and in turn on internal protobufs.
struct ExecutorRunState
{
  ExecutorRunState(
      const ExecutorInfo& executorInfo_,
      const ContainerID& id_,
      pid_t pid_,
      const std::string& directory_,
      const Option<std::string>& rootfs_)
    : executorInfo(executorInfo_),
      id(id_),
      pid(pid_),
      directory(directory_),
      rootfs(rootfs_) {}

  ExecutorInfo executorInfo;
  ContainerID id;        // Container id of the last executor run.
  pid_t pid;             // Executor pid.
  std::string directory; // Executor work directory.
  Option<std::string> rootfs; // Optional container rootfs.
};


class Isolator
{
public:
  explicit Isolator(process::Owned<IsolatorProcess> process);
  ~Isolator();

  // Returns the namespaces required by the isolator. The namespaces
  // are created while launching the executor. Isolators may return
  // a None() to indicate that they don't require any namespaces
  // (e.g., Isolators for OS X).
  // TODO(karya): Since namespaces are Linux-only, create a separate
  // LinuxIsolator (and corresponding LinuxIsolatorProcess) class
  // for Linux-specific isolators.
  process::Future<Option<int>> namespaces();

  // Recover containers from the run states and the orphan containers
  // (known to the launcher but not known to the slave) detected by
  // the launcher.
  process::Future<Nothing> recover(
      const std::list<ExecutorRunState>& states,
      const hashset<ContainerID>& orphans);

  // Prepare for isolation of the executor. Any steps that require
  // execution in the containerized context (e.g. inside a network
  // namespace) can be returned in the optional CommandInfo and they
  // will be run by the Launcher.
  // TODO(idownes): Any URIs or Environment in the CommandInfo will be
  // ignored; only the command value is used.
  process::Future<Option<CommandInfo>> prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& rootfs,
      const Option<std::string>& user);

  // Isolate the executor.
  process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid);

  // Watch the containerized executor and report if any resource
  // constraint impacts the container, e.g., the kernel killing some
  // processes.
  process::Future<Limitation> watch(const ContainerID& containerId);

  // Update the resources allocated to the container.
  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  // Gather resource usage statistics for the container.
  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) const;

  // Clean up a terminated container. This is called after the
  // executor and all processes in the container have terminated.
  process::Future<Nothing> cleanup(const ContainerID& containerId);

private:
  Isolator(const Isolator&); // Not copyable.
  Isolator& operator=(const Isolator&); // Not assignable.

  process::Owned<IsolatorProcess> process;
};


class IsolatorProcess : public process::Process<IsolatorProcess>
{
public:
  virtual ~IsolatorProcess() {}

  virtual process::Future<Option<int>> namespaces() { return None(); }

  virtual process::Future<Nothing> recover(
      const std::list<ExecutorRunState>& state,
      const hashset<ContainerID>& orphans) = 0;

  virtual process::Future<Option<CommandInfo>> prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& rootfs,
      const Option<std::string>& user) = 0;

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid) = 0;

  virtual process::Future<Limitation> watch(
      const ContainerID& containerId) = 0;

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources) = 0;

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) = 0;

  virtual process::Future<Nothing> cleanup(const ContainerID& containerId) = 0;
};

} // namespace slave {
} // namespace mesos {

#endif // __MESOS_SLAVE_ISOLATOR_HPP__
