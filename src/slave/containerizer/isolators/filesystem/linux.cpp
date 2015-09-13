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

#include "slave/paths.hpp"

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
  // Read the mount table in the host mount namespace to recover paths
  // to containers' work directories if their root filesystems are
  // changed. Method 'cleanup()' relies on this information to clean
  // up mounts in the host mount namespace for each container.
  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Failure("Failed to get mount table: " + table.error());
  }

  foreach (const ContainerState& state, states) {
    Owned<Info> info(new Info(state.directory()));

    foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
      if (entry.root == info->directory) {
        info->sandbox = entry.target;
        break;
      }
    }

    infos.put(state.container_id(), info);
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

  if (!executorInfo.has_container()) {
    return __prepare(containerId, executorInfo, directory, user, None());
  }

  // Provision the root filesystem if needed.
  CHECK_EQ(executorInfo.container().type(), ContainerInfo::MESOS);

  if (!executorInfo.container().mesos().has_image()) {
    return _prepare(containerId, executorInfo, directory, user, None());
  }

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


Future<Option<ContainerPrepareInfo>> LinuxFilesystemIsolatorProcess::_prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const Option<string>& rootfs)
{
  CHECK(executorInfo.has_container());
  CHECK_EQ(executorInfo.container().type(), ContainerInfo::MESOS);

  // We will provision the images specified in ContainerInfo::volumes
  // as well. We will mutate ContainerInfo::volumes to include the
  // paths to the provisioned root filesystems (by setting the
  // 'host_path') if the volume specifies an image as the source.
  Owned<ExecutorInfo> _executorInfo(new ExecutorInfo(executorInfo));
  list<Future<Nothing>> futures;

  for (int i = 0; i < _executorInfo->container().volumes_size(); i++) {
    Volume* volume = _executorInfo->mutable_container()->mutable_volumes(i);

    if (!volume->has_image()) {
      continue;
    }

    const Image& image = volume->image();

    if (!provisioners.contains(image.type())) {
      return Failure(
          "No suitable provisioner found for image type '" +
          stringify(image.type()) + "' in a volume");
    }

    futures.push_back(
        provisioners[image.type()]->provision(containerId, image)
          .then([volume](const string& path) -> Future<Nothing> {
            volume->set_host_path(path);
            return Nothing();
          }));
  }

  return collect(futures)
    .then([=]() -> Future<Option<ContainerPrepareInfo>> {
      return __prepare(containerId, *_executorInfo, directory, user, rootfs);
    });
}


Future<Option<ContainerPrepareInfo>> LinuxFilesystemIsolatorProcess::__prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const Option<string>& rootfs)
{
  CHECK(infos.contains(containerId));

  const Owned<Info>& info = infos[containerId];

  ContainerPrepareInfo prepareInfo;

  if (rootfs.isNone()) {
    // It the container does not change its root filesystem, we need
    // to do a self bind mount of the container's work directory and
    // mark it as a shared mount. This is necessary for any mounts
    // underneath it to be propagated into the container's mount
    // namespace. This is how we can update persistent volumes.
    LOG(INFO) << "Bind mounting work directory '" << directory
              << "' for container " << containerId;

    Try<Nothing> mount = fs::mount(
        directory,
        directory,
        None(),
        MS_BIND,
        NULL);

    if (mount.isError()) {
      return Failure(
          "Failed to self bind mount work directory '" +
          directory + "': " + mount.error());
    }

    mount = fs::mount(
        None(),
        directory,
        None(),
        MS_SHARED,
        NULL);

    if (mount.isError()) {
      return Failure(
          "Failed to mark work directory '" + directory +
          "' as a shared mount: " + mount.error());
    }
  } else {
    // If the container changes its root filesystem, we need to mount
    // the container's work directory into its root filesystem
    // (creating it if needed) so that the executor and the task can
    // access the work directory.
    //
    // NOTE: The mount of the work directory must be a shared mount in
    // the host filesystem so that any mounts underneath it will
    // propagate into the container's mount namespace. This is how we
    // can update persistent volumes for the container.

    // This is the mount point of the work directory in the root filesystem.
    const string sandbox = path::join(rootfs.get(), flags.sandbox_directory);

    // Save the path 'sandbox' which will be used in 'cleanup()'.
    info->sandbox = sandbox;

    if (!os::exists(sandbox)) {
      Try<Nothing> mkdir = os::mkdir(sandbox);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create sandbox mount point at '" +
            sandbox + "': " + mkdir.error());
      }
    }

    LOG(INFO) << "Bind mounting work directory from '" << directory
              << "' to '" << sandbox << "' for container " << containerId;

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
          "Failed to mark sandbox '" + sandbox +
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

  // Make sure mounts in the container mount namespace do not
  // propagate back to the host mount namespace.
  out << "mount --make-rslave /\n";

  // TODO(jieyu): Try to unmount work directory mounts and persistent
  // volume mounts for other containers to release the extra
  // references to those mounts.

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
    out << "mount -n --rbind '" << source << "' '" << target << "'\n";
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
  // Mount persistent volumes. We do this in the host namespace and
  // rely on mount propagation for them to be visible inside the
  // container.
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  const Owned<Info>& info = infos[containerId];

  Resources current = info->resources;

  // We first remove unneeded persistent volumes.
  foreach (const Resource& resource, current.persistentVolumes()) {
    // This is enforced by the master.
    CHECK(resource.disk().has_volume());

    // Ignore absolute and nested paths.
    const string& containerPath = resource.disk().volume().container_path();
    if (strings::contains(containerPath, "/")) {
      LOG(WARNING) << "Skipping updating mount for persistent volume "
                   << resource << " of container " << containerId
                   << " because the container path '" << containerPath
                   << "' contains slash";
      continue;
    }

    if (resources.contains(resource)) {
      continue;
    }

    // Determine the target of the mount.
    string target;

    if (info->sandbox.isSome()) {
      target = path::join(info->sandbox.get(), containerPath);
    } else {
      target = path::join(info->directory, containerPath);
    }

    LOG(INFO) << "Removing mount '" << target << "' for persistent volume "
              << resource << " of container " << containerId;

    // The unmount will fail if the task/executor is still using files
    // or directories under 'target'.
    Try<Nothing> unmount = fs::unmount(target);
    if (unmount.isError()) {
      return Failure(
          "Failed to unmount unneeded persistent volume at '" +
          target + "': " + unmount.error());
    }

    // NOTE: This is a non-recursive rmdir.
    Try<Nothing> rmdir = os::rmdir(target, false);
    if (rmdir.isError()) {
      return Failure(
          "Failed to remove persistent volume mount point at '" +
          target + "': " + rmdir.error());
    }
  }

  // We then mount new persistent volumes.
  foreach (const Resource& resource, resources.persistentVolumes()) {
    // This is enforced by the master.
    CHECK(resource.disk().has_volume());

    // Ignore absolute and nested paths.
    const string& containerPath = resource.disk().volume().container_path();
    if (strings::contains(containerPath, "/")) {
      LOG(WARNING) << "Skipping updating mount for persistent volume "
                   << resource << " of container " << containerId
                   << " because the container path '" << containerPath
                   << "' contains slash";
      continue;
    }

    if (current.contains(resource)) {
      continue;
    }

    // Determine the source of the mount.
    string source = paths::getPersistentVolumePath(
        flags.work_dir,
        resource.role(),
        resource.disk().persistence().id());

    // Set the ownership of the persistent volume to match that of the
    // sandbox directory.
    //
    // NOTE: Currently, persistent volumes in Mesos are exclusive,
    // meaning that if a persistent volume is used by one task or
    // executor, it cannot be concurrently used by other task or
    // executor. But if we allow multiple executors to use same
    // persistent volume at the same time in the future, the ownership
    // of the persistent volume may conflict here.
    //
    // TODO(haosdent): Consider letting the frameworks specify the
    // user/group of the persistent volumes.
    struct stat s;
    if (::stat(info->directory.c_str(), &s) < 0) {
      return Failure(
          "Failed to get ownership for '" + info->directory +
          "': " + strerror(errno));
    }

    LOG(INFO) << "Changing the ownership of the persistent volume at '"
              << source << "' with uid " << s.st_uid
              << " and gid " << s.st_gid;

    Try<Nothing> chown = os::chown(s.st_uid, s.st_gid, source, true);
    if (chown.isError()) {
      return Failure(
          "Failed to change the ownership of the persistent volume at '" +
          source + "' with uid " + stringify(s.st_uid) +
          " and gid " + stringify(s.st_gid) + ": " + chown.error());
    }

    // Determine the target of the mount.
    string target;

    if (info->sandbox.isSome()) {
      target = path::join(info->sandbox.get(), containerPath);
    } else {
      target = path::join(info->directory, containerPath);
    }

    if (os::exists(target)) {
      // NOTE: This is possible because 'info->resources' will be
      // reset when slave restarts and recovers. When the slave calls
      // 'containerizer->update' after the executor re-registers,
      // we'll try to re-mount all the already mounted volumes.

      // TODO(jieyu): Check the source of the mount matches the entry
      // with the same target in the mount table if one can be found.
      // If not, mount the persistent volume as we did below. This is
      // possible because the slave could crash after it unmounts the
      // volume but before it is able to delete the mount point.
    } else {
      Try<Nothing> mkdir = os::mkdir(target);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create persistent volume mount point at '" +
            target + "': " + mkdir.error());
      }

      LOG(INFO) << "Mounting '" << source << "' to '" << target
                << "' for persistent volume " << resource
                << " of container " << containerId;

      Try<Nothing> mount = fs::mount(source, target, None(), MS_BIND, NULL);
      if (mount.isError()) {
        return Failure(
            "Failed to mount persistent volume from '" +
            source + "' to '" + target + "': " + mount.error());
      }
    }
  }

  // Store the new resources;
  info->resources = resources;

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

  // NOTE: We don't need to cleanup mounts in the container's mount
  // namespace because it's done automatically by the kernel when the
  // mount namespace is destroyed after the last process terminates.

  // The path to the container' work directory which is the parent of
  // all the persistent volume mounts.
  string sandbox;

  if (info->sandbox.isSome()) {
    sandbox = info->sandbox.get();
  } else {
    sandbox = info->directory;
  }

  infos.erase(containerId);

  // Cleanup the mounts for this container in the host mount
  // namespace, including container's work directory and all the
  // persistent volume mounts.
  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Failure("Failed to get mount table: " + table.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    // NOTE: All persistent volumes are mounted at targets under the
    // container's work directory.
    if (entry.target != sandbox &&
        strings::startsWith(entry.target, sandbox)) {
      LOG(INFO) << "Unmounting volume '" << entry.target
                << "' for container " << containerId;

      Try<Nothing> unmount = fs::unmount(entry.target);
      if (unmount.isError()) {
        return Failure(
            "Failed to unmount volume '" + entry.target +
            "': " + unmount.error());
      }
    }
  }

  // Cleanup the container's work directory mount.
  LOG(INFO) << "Unmounting sandbox/work directory '" << sandbox
            << "' for container " << containerId;

  Try<Nothing> unmount = fs::unmount(sandbox);
  if (unmount.isError()) {
    return Failure(
        "Failed to unmount sandbox/work directory '" + sandbox +
        "': " + unmount.error());
  }

  // Destroy the provisioned root filesystems.
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
