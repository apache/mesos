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

#include <mesos/secret/resolver.hpp>

#include <mesos/slave/containerizer.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "common/future_tracker.hpp"

#include "slave/csi_server.hpp"
#include "slave/gc.hpp"

#include "slave/volume_gid_manager/volume_gid_manager.hpp"

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
  enum class LaunchResult {
    SUCCESS,
    ALREADY_LAUNCHED,
    NOT_SUPPORTED,
  };

  // Attempts to create a containerizer as specified by 'isolation' in
  // flags.
  static Try<Containerizer*> create(
      const Flags& flags,
      bool local,
      Fetcher* fetcher,
      GarbageCollector* gc,
      SecretResolver* secretResolver = nullptr,
      VolumeGidManager* volumeGidManager = nullptr,
      PendingFutureTracker* futureTracker = nullptr,
      CSIServer* csiServer = nullptr);

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

  // Launch a container with the specified ContainerConfig.
  //
  // If the ContainerID has a parent, this will attempt to launch
  // a nested container.
  // NOTE: For nested containers, the required `directory` field of
  // the ContainerConfig will be determined by the containerizer.
  virtual process::Future<LaunchResult> launch(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig,
      const std::map<std::string, std::string>& environment,
      const Option<std::string>& pidCheckpointPath) = 0;

  // Create an HTTP connection that can be used to "attach" (i.e.,
  // stream input to or stream output from) a container.
  virtual process::Future<process::http::Connection> attach(
      const ContainerID& containerId)
  {
    return process::Failure("Unsupported");
  }

  // Update the resources for a container.
  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {}) = 0;

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
  // The future may be failed if an error occurs during termination of
  // the executor or destruction of the container.
  //
  // Returns `None` if the container cannot be found.
  // NOTE: For terminated nested containers, whose parent container is
  // still running, the checkpointed `ContainerTermination` must be returned.
  virtual process::Future<Option<mesos::slave::ContainerTermination>> wait(
      const ContainerID& containerId) = 0;

  // Destroy a starting or running container, killing all processes and
  // releasing all resources. Returns the same result as `wait()` method,
  // therefore calling `wait()` right before `destroy()` is not required.
  virtual process::Future<Option<mesos::slave::ContainerTermination>> destroy(
      const ContainerID& containerId) = 0;

  // Sends a signal to a running container. Returns false when the container
  // cannot be found. The future may be failed if an error occurs in sending
  // the signal to the running container.
  virtual process::Future<bool> kill(
      const ContainerID& containerId,
      int signal)
  {
    return process::Failure("Unsupported");
  };

  virtual process::Future<hashset<ContainerID>> containers() = 0;

  // Remove a nested container, including its sandbox and runtime directories.
  //
  // NOTE: You can only remove a a nested container that has been fully
  // destroyed and whose parent has not been destroyed yet. If the parent has
  // already been destroyed, then the sandbox and runtime directories will be
  // eventually garbage collected. The caller is responsible for ensuring that
  // `containerId` belongs to a nested container.
  virtual process::Future<Nothing> remove(const ContainerID& containerId)
  {
    return process::Failure("Unsupported");
  }

  // Prune unused images from supported image stores.
  virtual process::Future<Nothing> pruneImages(
      const std::vector<Image>& excludedImages) = 0;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CONTAINERIZER_HPP__
