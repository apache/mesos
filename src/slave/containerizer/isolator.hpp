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

#ifndef __ISOLATOR_HPP__
#define __ISOLATOR_HPP__

#include <list>
#include <string>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/try.hpp>

#include "slave/flags.hpp"
#include "slave/state.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class IsolatorProcess;

// Information when an executor is impacted by a resource limitation and should
// be terminated. Intended to support resources like memory where the Linux
// kernel may invoke the OOM killer, killing some/all of a container's
// processes.
struct Limitation
{
  Limitation(
      const Resource& _resource,
      const std::string& _message)
    : resource(_resource),
      message(_message) {}

  // Resource (type and value) that triggered the limitation.
  const Resource resource;
  // Description of the limitation.
  const std::string message;
};


class Isolator
{
public:
  Isolator(process::Owned<IsolatorProcess> process);
  ~Isolator();

  // Recover containers from the run states.
  process::Future<Nothing> recover(
      const std::list<state::RunState>& states);

  // Prepare for isolation of the executor.
  process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo);

  // Isolate the executor. Any steps that require execution in the
  // containerized context (e.g. inside a network namespace) can be returned in
  // the optional CommandInfo and they will be run by the Launcher.  This could
  // be a simple command or a URI (including a local file) that will be fetched
  // and executed.
  process::Future<Option<CommandInfo> > isolate(
      const ContainerID& containerId,
      pid_t pid);

  // Watch the containerized executor and report if any resource constraint
  // impacts the container, e.g., the kernel killing some processes.
  process::Future<Limitation> watch(const ContainerID& containerId);

  // Update the resources allocated to the container.
  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  // Gather resource usage statistics for the container.
  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) const;

  // Clean up a terminated container. This is called after the executor and all
  // processes in the container have terminated.
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

  virtual process::Future<Nothing> recover(
      const std::list<state::RunState>& state) = 0;

  virtual process::Future<Nothing> prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo) = 0;

  virtual process::Future<Option<CommandInfo> > isolate(
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
} // namespace internal {
} // namespace mesos {

#endif // __ISOLATOR_HPP__
