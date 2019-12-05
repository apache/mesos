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

#ifndef __MESOS_SLAVE_ISOLATOR_HPP__
#define __MESOS_SLAVE_ISOLATOR_HPP__

#include <string>
#include <vector>

#include <mesos/resources.hpp>

#include <mesos/slave/containerizer.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace slave {

class Isolator
{
public:
  virtual ~Isolator() {}

  // Returns true if this isolator supports nested containers. This
  // method is designed to allow isolators to opt-in to support nested
  // containers.
  virtual bool supportsNesting()
  {
    return false;
  }

  virtual bool supportsStandalone()
  {
    return false;
  }

  // Recover containers from the run states and the orphan containers
  // (known to the launcher but not known to the slave) detected by
  // the launcher.
  virtual process::Future<Nothing> recover(
      const std::vector<ContainerState>& states,
      const hashset<ContainerID>& orphans)
  {
    return Nothing();
  }

  // Prepare for isolation of the executor. Any steps that require
  // execution in the containerized context (e.g. inside a network
  // namespace) can be returned in the optional CommandInfo and they
  // will be run by the Launcher.
  // TODO(idownes): Any URIs or Environment in the CommandInfo will be
  // ignored; only the command value is used.
  virtual process::Future<Option<ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const ContainerConfig& containerConfig)
  {
    return None();
  }

  // Isolate the executor.
  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid)
  {
    return Nothing();
  }

  // Watch the containerized executor and report if any resource
  // constraint impacts the container, e.g., the kernel killing some
  // processes.
  virtual process::Future<ContainerLimitation> watch(
      const ContainerID& containerId)
  {
    return process::Future<ContainerLimitation>();
  }

  // Update the resources allocated to the container.
  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {})
  {
    return Nothing();
  }

  // Gather resource usage statistics for the container.
  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId)
  {
    return ResourceStatistics();
  }

  // Get the run-time status of isolator specific properties
  // associated with the container.
  virtual process::Future<ContainerStatus> status(
      const ContainerID& containerId)
  {
    return ContainerStatus();
  }

  // Clean up a terminated container. This is called after the
  // executor and all processes in the container have terminated.
  // It's likely that isolator `cleanup` is called for an unknown
  // container (see MESOS-6059). Therefore, the isolator should ignore
  // the cleanup is the container is unknown to it. In any case, the
  // `cleanup` won't be called multiple times for a container. Also,
  // if `prepare` is called, the cleanup is guaranteed to be called
  // after `prepare` finishes (or fails).
  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId)
  {
    return Nothing();
  }
};

} // namespace slave {
} // namespace mesos {

#endif // __MESOS_SLAVE_ISOLATOR_HPP__
