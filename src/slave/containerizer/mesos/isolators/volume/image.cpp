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

#include <list>
#include <string>
#include <vector>

#include <glog/logging.h>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/id.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/strings.hpp>

#include "slave/containerizer/mesos/isolators/volume/image.hpp"

#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

using std::list;
using std::string;
using std::vector;

using process::defer;
using process::Failure;
using process::Future;
using process::Owned;
using process::PID;
using process::Shared;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

VolumeImageIsolatorProcess::VolumeImageIsolatorProcess(
    const Flags& _flags,
    const Shared<Provisioner>& _provisioner)
  : ProcessBase(process::ID::generate("volume-image-isolator")),
    flags(_flags),
    provisioner(_provisioner) {}


VolumeImageIsolatorProcess::~VolumeImageIsolatorProcess() {}


bool VolumeImageIsolatorProcess::supportsNesting()
{
  return true;
}


Try<Isolator*> VolumeImageIsolatorProcess::create(
    const Flags& flags,
    const Shared<Provisioner>& provisioner)
{
  // Make sure 'filesystem/linux' isolator is used.
  // NOTE: 'filesystem/linux' isolator will make sure mounts in the
  // child mount namespace will not be propagated back to the host
  // mount namespace.
  if (!strings::contains(flags.isolation, "filesystem/linux")) {
    return Error("'filesystem/linux' must be enabled"
                 " to create the volume image isolator");
  }

  process::Owned<MesosIsolatorProcess> process(
      new VolumeImageIsolatorProcess(flags, provisioner));

  return new MesosIsolator(process);
}


Future<Option<ContainerLaunchInfo>> VolumeImageIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (!containerConfig.has_container_info()) {
    return None();
  }

  if (containerConfig.container_info().type() != ContainerInfo::MESOS) {
    return Failure("Can only prepare image volumes for a MESOS container");
  }

  vector<string> targets;
  list<Future<ProvisionInfo>> futures;

  for (int i = 0; i < containerConfig.container_info().volumes_size(); i++) {
    const Volume& volume = containerConfig.container_info().volumes(i);

    if (!volume.has_image()) {
      continue;
    }

    if (containerConfig.has_container_class() &&
        containerConfig.container_class() == ContainerClass::DEBUG) {
      return Failure("Image volume is not supported for DEBUG containers");
    }

    // Determine the target of the mount. The mount target
    // is determined by 'container_path'.
    string target;

    // The logic to determine a volume mount target is identical to
    // linux filesystem isolator, because volume image isolator has
    // a dependency on that isolator, and it assumes that if the
    // container specifies a rootfs the sandbox is already bind
    // mounted into the container.
    if (path::absolute(volume.container_path())) {
      // To specify an image volume for a container, operators should
      // be allowed to define the 'container_path' either as an absolute
      // path or a relative path. Please see linux filesystem isolator
      // for detail.
      if (containerConfig.has_rootfs()) {
        target = path::join(
            containerConfig.rootfs(),
            volume.container_path());

        Try<Nothing> mkdir = os::mkdir(target);
        if (mkdir.isError()) {
          return Failure(
              "Failed to create the target of the mount at '" +
              target + "': " + mkdir.error());
        }
      } else {
        target = volume.container_path();

        if (!os::exists(target)) {
          return Failure("Absolute container path '" + target + "' "
                         "does not exist");
        }
      }
    } else {
      if (containerConfig.has_rootfs()) {
        target = path::join(containerConfig.rootfs(),
                            flags.sandbox_directory,
                            volume.container_path());
      } else {
        target = path::join(containerConfig.directory(),
                            volume.container_path());
      }

      // NOTE: We cannot create the mount point at 'target' if
      // container has rootfs defined. The bind mount of the sandbox
      // will hide what's inside 'target'. So we should always create
      // the mount point in 'directory'.
      const string mountPoint = path::join(
          containerConfig.directory(),
          volume.container_path());

      Try<Nothing> mkdir = os::mkdir(mountPoint);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create the target of the mount at '" +
            mountPoint + "': " + mkdir.error());
      }
    }

    targets.push_back(target);
    futures.push_back(provisioner->provision(containerId, volume.image()));
  }

  return await(futures)
    .then(defer(
        PID<VolumeImageIsolatorProcess>(this),
        &VolumeImageIsolatorProcess::_prepare,
        containerId,
        targets,
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> VolumeImageIsolatorProcess::_prepare(
    const ContainerID& containerId,
    const vector<string>& targets,
    const list<Future<ProvisionInfo>>& futures)
{
  ContainerLaunchInfo launchInfo;

  vector<string> messages;
  vector<string> sources;

  foreach (const Future<ProvisionInfo>& future, futures) {
    if (!future.isReady()) {
      messages.push_back(future.isFailed() ? future.failure() : "discarded");
      continue;
    }

    sources.push_back(future.get().rootfs);
  }

  if (!messages.empty()) {
    return Failure(strings::join("\n", messages));
  }

  CHECK_EQ(sources.size(), targets.size());

  for (size_t i = 0; i < sources.size(); i++) {
    const string& source = sources[i];
    const string& target = targets[i];

    LOG(INFO) << "Mounting image volume rootfs '" << source
              << "' to '" << target << "' for container " << containerId;

    if (!os::exists(source)) {
      return Failure(
          "Provisioned rootfs '" + source + "' does not exist");
    }

    CommandInfo* command = launchInfo.add_pre_exec_commands();
    command->set_shell(false);
    command->set_value("mount");
    command->add_arguments("mount");
    command->add_arguments("-n");
    command->add_arguments("--rbind");
    command->add_arguments(source);
    command->add_arguments(target);
  }

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
