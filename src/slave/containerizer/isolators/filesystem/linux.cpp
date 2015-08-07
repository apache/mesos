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

#include <list>
#include <sstream>
#include <string>

#include <glog/logging.h>

#include <process/collect.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "linux/fs.hpp"
#include "linux/ns.hpp"

#include "slave/containerizer/isolators/filesystem/linux.hpp"

using namespace process;

using std::list;
using std::ostringstream;
using std::string;

using mesos::slave::ContainerState;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerPrepareInfo;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> LinuxFilesystemIsolatorProcess::create(
    const Flags& flags,
    const hashmap<Image::Type, Owned<Provisioner>>& provisioners)
{
  Result<string> user = os::user();
  if (!user.isSome()) {
    return Error("Failed to determine user: " +
                 (user.isError() ? user.error() : "username not found"));
  }

  if (user.get() != "root") {
    return Error("LinuxFilesystemIsolator requires root privileges");
  }

  Owned<MesosIsolatorProcess> process(
      new LinuxFilesystemIsolatorProcess(flags, provisioners));

  return new MesosIsolator(process);
}


LinuxFilesystemIsolatorProcess::LinuxFilesystemIsolatorProcess(
    const Flags& _flags,
    const hashmap<Image::Type, Owned<Provisioner>>& _provisioners)
  : flags(_flags),
    provisioners(_provisioners) {}


LinuxFilesystemIsolatorProcess::~LinuxFilesystemIsolatorProcess() {}


Future<Option<int>> LinuxFilesystemIsolatorProcess::namespaces()
{
  return CLONE_NEWNS;
}


Future<Nothing> LinuxFilesystemIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  list<Future<Nothing>> futures;
  foreachvalue (const Owned<Provisioner>& provisioner, provisioners) {
    futures.push_back(provisioner->recover(states, orphans));
  }

  return collect(futures)
    .then(defer(PID<LinuxFilesystemIsolatorProcess>(this),
                &LinuxFilesystemIsolatorProcess::_recover,
                states,
                orphans));
}


Future<Nothing> LinuxFilesystemIsolatorProcess::_recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    infos.put(state.container_id(), Owned<Info>(new Info(state.directory())));
  }

  // TODO(jieyu): Clean up unknown containers' work directory mounts
  // and the corresponding persistent volume mounts. This can be
  // achieved by iterating the mount table and find those unknown
  // mounts whose sources are under the slave 'work_dir'.

  return Nothing();
}


Future<Option<ContainerPrepareInfo>> LinuxFilesystemIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  infos.put(containerId, Owned<Info>(new Info(directory)));

  // Provision the root filesystem if needed.
  if (executorInfo.has_container()) {
    CHECK_EQ(executorInfo.container().type(), ContainerInfo::MESOS);

    if (executorInfo.container().mesos().has_image()) {
      const Image& image = executorInfo.container().mesos().image();

      if (!provisioners.contains(image.type())) {
        return Failure(
            "No suitable provisioner found for container image type '" +
            stringify(image.type()) + "'");
      }

      return provisioners[image.type()]->provision(containerId, image)
        .then(defer(PID<LinuxFilesystemIsolatorProcess>(this),
                    &LinuxFilesystemIsolatorProcess::_prepare,
                    containerId,
                    executorInfo,
                    directory,
                    user,
                    lambda::_1));
    }
  }

  // TODO(jieyu): Provision images in volumes as well.

  return _prepare(containerId, executorInfo, directory, user, None());
}


Future<Option<ContainerPrepareInfo>> LinuxFilesystemIsolatorProcess::_prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const Option<string>& rootfs)
{
  CHECK(infos.contains(containerId));

  ContainerPrepareInfo prepareInfo;

  // If the container changes its root filesystem, we need to mount
  // the container's work directory into its root filesystem (creating
  // it if needed) so that the executor and the task can access the
  // work directory.
  //
  // NOTE: The mount of the work directory must be a shared mount in
  // the host filesystem so that any mounts underneath it will
  // propagate into the container's mount namespace. This is how we
  // can update persistent volumes for the container.
  if (rootfs.isSome()) {
    // This is the mount point of the work directory in the root filesystem.
    const string sandbox = path::join(rootfs.get(), flags.sandbox_directory);

    if (!os::exists(sandbox)) {
      Try<Nothing> mkdir = os::mkdir(sandbox);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create sandbox mount point at '" +
            sandbox + "': " + mkdir.error());
      }
    }

    Try<Nothing> mount = fs::mount(
        directory,
        sandbox,
        None(),
        MS_BIND,
        NULL);

    if (mount.isError()) {
      return Failure(
          "Failed to mount work directory '" + directory +
          "' to '" + sandbox + "': " + mount.error());
    }

    mount = fs::mount(
        None(),
        sandbox,
        None(),
        MS_SHARED,
        NULL);

    if (mount.isError()) {
      return Failure(
          "Failed to mark work directory '" + directory +
          "' as a shared mount: " + mount.error());
    }

    prepareInfo.set_rootfs(rootfs.get());
  }

  // Prepare the commands that will be run in the container's mount
  // namespace right after forking the executor process. We use these
  // commands to mount those volumes specified in the container info
  // so that they don't pollute the host mount namespace.
  if (executorInfo.has_container() &&
      executorInfo.container().volumes_size() > 0) {
    Try<string> _script = script(executorInfo, directory, rootfs);
    if (_script.isError()) {
      return Failure("Failed to generate isolation script: " + _script.error());
    }

    CommandInfo* command = prepareInfo.add_commands();
    command->set_value(_script.get());
  }

  return update(containerId, executorInfo.resources())
    .then([prepareInfo]() -> Future<Option<ContainerPrepareInfo>> {
      return prepareInfo;
    });
}


Try<string> LinuxFilesystemIsolatorProcess::script(
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& rootfs)
{
  CHECK(executorInfo.has_container());

  ostringstream out;
  out << "#!/bin/sh\n";
  out << "set -x -e\n";

  foreach (const Volume& volume, executorInfo.container().volumes()) {
    if (!volume.has_host_path()) {
      return Error("A volume misses 'host_path'");
    }

    // If both 'host_path' and 'container_path' are relative paths,
    // return an error because the user can just directly access the
    // volume in the work directory.
    if (!strings::startsWith(volume.host_path(), "/") &&
        !strings::startsWith(volume.container_path(), "/")) {
      return Error(
          "Both 'host_path' and 'container_path' of a volume are relative");
    }

    // Determine the source of the mount.
    string source;

    if (strings::startsWith(volume.host_path(), "/")) {
      source = volume.host_path();

      // An absolute path must already exist.
      if (!os::exists(source)) {
        return Error("Absolute host path does not exist");
      }
    } else {
      // Path is interpreted as relative to the work directory.
      source = path::join(directory, volume.host_path());

      // TODO(jieyu): We need to check that source resolves under the
      // work directory because a user can potentially use a container
      // path like '../../abc'.

      if (!os::exists(source)) {
        Try<Nothing> mkdir = os::mkdir(source);
        if (mkdir.isError()) {
          return Error(
              "Failed to create the source of the mount at '" +
              source + "': " + mkdir.error());
        }

        // TODO(idownes): Consider setting ownership and mode.
      }
    }

    // Determine the target of the mount.
    string target;

    if (strings::startsWith(volume.container_path(), "/")) {
      if (rootfs.isSome()) {
        target = path::join(rootfs.get(), volume.container_path());
      } else {
        target = volume.container_path();
      }

      // An absolute path must already exist. This is because we want
      // to avoid creating mount points outside the work directory in
      // the host filesystem or in the container filesystem root.
      if (!os::exists(target)) {
        return Error("Absolute container path does not exist");
      }

      // TODO(jieyu): We need to check that target resolves under
      // 'rootfs' because a user can potentially use a container path
      // like '/../../abc'.
    } else {
      if (rootfs.isSome()) {
        target = path::join(rootfs.get(),
                            flags.sandbox_directory,
                            volume.container_path());
      } else {
        target = path::join(directory, volume.container_path());
      }

      // TODO(jieyu): We need to check that target resolves under the
      // sandbox because a user can potentially use a container path
      // like '../../abc'.

      if (!os::exists(target)) {
        Try<Nothing> mkdir = os::mkdir(target);
        if (mkdir.isError()) {
          return Error(
              "Failed to create the target of the mount at '" +
              target + "': " + mkdir.error());
        }
      }
    }

    // TODO(jieyu): Consider the mode in the volume.
    out << "mount -n --bind '" << source << "' '" << target << "'\n";
  }

  return out.str();
}


Future<Nothing> LinuxFilesystemIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  // No-op, isolation happens when unsharing the mount namespace.
  return Nothing();
}


Future<ContainerLimitation> LinuxFilesystemIsolatorProcess::watch(
    const ContainerID& containerId)
{
  // No-op.
  return Future<ContainerLimitation>();
}


Future<Nothing> LinuxFilesystemIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  // TODO(jieyu): Update persistent volumes in this function.
  return Nothing();
}


Future<ResourceStatistics> LinuxFilesystemIsolatorProcess::usage(
    const ContainerID& containerId)
{
  // No-op, no usage gathered.
  return ResourceStatistics();
}


Future<Nothing> LinuxFilesystemIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container: "
            << containerId;

    return Nothing();
  }

  const Owned<Info>& info = infos[containerId];

  // Cleanup the mounts for this container in the host mount
  // namespace, including container's work directory (if container
  // root filesystem is used), and all the persistent volume mounts.
  //
  // NOTE: We don't need to cleanup mounts in the container's mount
  // namespace because it's done automatically by the kernel when the
  // mount namespace is destroyed after the last process terminates.
  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Failure("Failed to get mount table: " + table.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    // NOTE: Currently, all persistent volumes are mounted at targets
    // under the container's work directory.
    if (entry.root == info->directory ||
        strings::startsWith(entry.target, info->directory)) {
      LOG(INFO) << "Unmounting '" << entry.target
                << "' for container " << containerId;

      Try<Nothing> unmount = fs::unmount(entry.target, MNT_DETACH);
      if (unmount.isError()) {
        return Failure(
            "Failed to unmount '" + entry.target + "': " + unmount.error());
      }
    }
  }

  infos.erase(containerId);

  // Destroy the provisioned root filesystem.
  list<Future<bool>> futures;
  foreachvalue (const Owned<Provisioner>& provisioner, provisioners) {
    futures.push_back(provisioner->destroy(containerId));
  }

  return collect(futures)
    .then([]() -> Future<Nothing> { return Nothing(); });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
