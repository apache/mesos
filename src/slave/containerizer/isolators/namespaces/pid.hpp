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

#ifndef __NAMESPACES_PID_ISOLATOR_HPP__
#define __NAMESPACES_PID_ISOLATOR_HPP__

#include <mesos/slave/isolator.hpp>

#include "slave/flags.hpp"

#include <sys/types.h>

#include <string>

#include <stout/result.hpp>

namespace mesos {
namespace slave {

// This isolator itself does not specify the necessary clone() flags
// (see the LinuxLauncher for that) but it is used to keep track of a
// container's pid namespace through a bind mount and exposed by
// getNamespace().
class NamespacesPidIsolatorProcess : public IsolatorProcess
{
public:
  static Try<Isolator*> create(const Flags& flags);

  // Return the pid namespace of the container. Returns None if the
  // container was not created in a separate pid namespace, i.e.,
  // processes are in the same namespace as the slave. This is used by
  // the LinuxLauncher to determine if it can kill the leading process
  // in the container and let the kernel kill the remaining processes.
  // A container may not have a pid namespace if it was created
  // without the namespaces/pid isolator and the slave was
  // subsequently restarted with namespaces/pid enabled.
  static Result<ino_t> getNamespace(const ContainerID& container);

  NamespacesPidIsolatorProcess() {}

  virtual ~NamespacesPidIsolatorProcess() {}

  virtual process::Future<Nothing> recover(
      const std::list<ExecutorRunState>& states);

  virtual process::Future<Option<CommandInfo> > prepare(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user);

  virtual process::Future<Nothing> isolate(
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
};

} // namespace slave {
} // namespace mesos {

#endif // __NAMESPACES_PID_ISOLATOR_HPP__
