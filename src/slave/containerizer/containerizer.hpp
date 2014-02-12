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

#ifndef __CONTAINERIZER_HPP__
#define __CONTAINERIZER_HPP__

#include <map>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

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

// An abstraction of a Containerizer that will contain an executor and its
// tasks.
class Containerizer
{
public:
  // Information about a container termination.
  struct Termination
  {
    Termination(
        const Option<int>& _status,
        bool _killed,
        const std::string& _message)
      : status(_status),
        killed(_killed),
        message(_message) {}

    // Exit status of the executor.
    const Option<int> status;

    // A container may be killed if it exceeds its resources; this will be
    // indicated by killed=true and described by the message string.
    const bool killed;
    const std::string message;
  };

  // Attempts to create a containerizer as specified by 'isolation' in flags.
  static Try<Containerizer*> create(const Flags& flags, bool local);

  // Determine slave resources from flags, probing the system or querying a
  // delegate.
  // TODO(idownes): Consider making this non-static and moving to containerizer
  // implementations to enable a containerizer to best determine the resources,
  // particularly if containerizeration is delegated.
  static Try<Resources> resources(const Flags& flags);

  virtual ~Containerizer() {}

  // Recover all containerized executors specified in state. Any containerized
  // executors present on the system but not included in state (or state is
  // None) will be terminated and cleaned up.
  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state) = 0;

  // Launch a containerized executor.
  virtual process::Future<Nothing> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint) = 0;

  // Update the resources for a container.
  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources) = 0;

  // Get resource usage statistics on the container.
  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) = 0;

  // Wait on the container's 'Termination'. If the executor terminates, the
  // containerizer should also destroy the containerized context. The future
  // may be failed if an error occurs during termination of the executor or
  // destruction of the container.
  virtual process::Future<Termination> wait(const ContainerID& containerId) = 0;

  // Destroy a running container, killing all processes and releasing all
  // resources.
  // NOTE: Containerizers will automatically destroy containers on executor
  // termination and manual destruction is not necessary. See wait().
  virtual void destroy(const ContainerID& containerId) = 0;
};


// Executor environment variables returned as (name, value) map.
std::map<std::string, std::string> executorEnvironment(
    const ExecutorInfo& executorInfo,
    const std::string& directory,
    const SlaveID& slaveId,
    const process::PID<Slave>& slavePid,
    bool checkpoint,
    const Duration& recoveryTimeout);

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CONTAINERIZER_HPP__
