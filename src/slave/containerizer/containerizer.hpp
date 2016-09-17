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

#ifndef __CONTAINERIZER_HPP__
#define __CONTAINERIZER_HPP__

#include <map>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/slave/containerizer.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/fetcher.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class Slave;
class Flags;

namespace state {
// Forward declaration.
struct SlaveState;
} // namespace state {


// An abstraction of a Containerizer that will contain an executor and
// its tasks.
class Containerizer
{
public:
  // Attempts to create a containerizer as specified by 'isolation' in
  // flags.
  static Try<Containerizer*> create(
      const Flags& flags,
      bool local,
      Fetcher* fetcher);

  // Determine slave resources from flags, probing the system or
  // querying a delegate.
  // TODO(idownes): Consider making this non-static and moving to
  // containerizer implementations to enable a containerizer to best
  // determine the resources, particularly if containerizeration is
  // delegated.
  static Try<Resources> resources(const Flags& flags);

  virtual ~Containerizer() {}

  // Recover all containerized executors specified in state. Any
  // containerized executors present on the system but not included in
  // state (or state is None) will be terminated and cleaned up.
  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state) = 0;

  // Launch a containerized task/executor. Returns true if launching
  // this TaskInfo/ExecutorInfo is supported and it has been launched,
  // otherwise false or a failure if something went wrong.
  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const std::map<std::string, std::string>& environment,
      bool checkpoint) = 0;

  // Launch a nested container.
  // TODO(jieyu): Consider combining with the 'launch' above.
  virtual process::Future<Nothing> launch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const Option<ContainerInfo>& containerInfo,
      const Resources& resources)
  {
    return process::Failure("Unsupported");
  }

  // Update the resources for a container.
  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources) = 0;

  // Get resource usage statistics on the container.
  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) = 0;

  // Retrieve the run-time state of various isolator properties
  // associated with the container. Unlike other methods in this class
  // we are not making this pure virtual, since a `Containerizer`
  // doesn't necessarily need to return the status of a container.
  virtual process::Future<ContainerStatus> status(
      const ContainerID &containerId)
  {
    return ContainerStatus();
  }

  // Wait on the 'ContainerTermination'. If the executor terminates,
  // the containerizer should also destroy the containerized context.
  // Returns None if the container cannot be found.
  // The future may be failed if an error occurs during termination of
  // the executor or destruction of the container.
  virtual process::Future<Option<mesos::slave::ContainerTermination>> wait(
      const ContainerID& containerId) = 0;

  // Destroy a running container, killing all processes and releasing
  // all resources. Returns false when the container cannot be found,
  // or a failure if something went wrong.
  //
  // NOTE: You cannot wait() on containers that have been destroyed,
  // so you should always call wait() before destroy().
  virtual process::Future<bool> destroy(const ContainerID& containerId) = 0;

  virtual process::Future<hashset<ContainerID>> containers() = 0;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CONTAINERIZER_HPP__
