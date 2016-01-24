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
#include <sstream>
#include <string>

#include <glog/logging.h>

#include <process/collect.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/adaptor.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/shell.hpp>
#include <stout/os/strerror.hpp>

#include "linux/fs.hpp"
#include "linux/ns.hpp"

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/mount.hpp"

#include "slave/containerizer/mesos/isolators/filesystem/linux.hpp"

using namespace process;

using std::list;
using std::ostringstream;
using std::string;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerState;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> LinuxFilesystemIsolatorProcess::create(const Flags& flags)
{
  Result<string> user = os::user();
  if (!user.isSome()) {
    return Error("Failed to determine user: " +
                 (user.isError() ? user.error() : "username not found"));
  }

  if (user.get() != "root") {
    return Error("LinuxFilesystemIsolator requires root privileges");
  }

  // Make slave's work_dir a shared mount so that when forking a child
  // process (with a new mount namespace), the child process does not
  // hold extra references to container's work directory mounts and
  // provisioner mounts (e.g., when using the bind backend) because
  // cleanup operations within work_dir can be propagted to all
  // container namespaces. See MESOS-3483 for more details.
  LOG(INFO) << "Making '" << flags.work_dir << "' a shared mount";

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Error("Failed to get mount table: " + table.error());
  }

  Option<fs::MountInfoTable::Entry> workDirMount;
  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    // TODO(jieyu): Make sure 'flags.work_dir' is a canonical path.
    if (entry.target == flags.work_dir) {
      workDirMount = entry;
      break;
    }
  }

  // Do a self bind mount if needed. If the mount already exists, make
  // sure it is a shared mount of its own peer group.
  if (workDirMount.isNone()) {
    // NOTE: Instead of using fs::mount to perform the bind mount, we
    // use the shell command here because the syscall 'mount' does not
    // update the mount table (i.e., /etc/mtab). In other words, the
    // mount will not be visible if the operator types command
    // 'mount'. Since this mount will still be presented after all
    // containers and the slave are stopped, it's better to make it
    // visible. It's OK to use the blocking os::shell here because
    // 'create' will only be invoked during initialization.
    Try<string> mount = os::shell(
        "mount --bind %s %s && "
        "mount --make-slave %s && "
        "mount --make-shared %s",
        flags.work_dir.c_str(),
        flags.work_dir.c_str(),
        flags.work_dir.c_str(),
        flags.work_dir.c_str());

    if (mount.isError()) {
      return Error(
          "Failed to self bind mount '" + flags.work_dir +
          "' and make it a shared mount: " + mount.error());
    }
  } else {
    if (workDirMount.get().shared().isNone()) {
      // This is the case where the work directory mount is not a
      // shared mount yet (possibly due to slave crash while preparing
      // the work directory mount). It's safe to re-do the following.
      Try<string> mount = os::shell(
          "mount --make-slave %s && "
          "mount --make-shared %s",
          flags.work_dir.c_str(),
          flags.work_dir.c_str());

      if (mount.isError()) {
        return Error(
            "Failed to self bind mount '" + flags.work_dir +
            "' and make it a shared mount: " + mount.error());
      }
    } else {
      // We need to make sure that the shared mount is in its own peer
      // group. To check that, we need to get the parent mount.
      foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
        if (entry.id == workDirMount.get().parent) {
          // If the work directory mount and its parent mount are in
          // the same peer group, we need to re-do the following
          // commands so that they are in different peer groups.
          if (entry.shared() == workDirMount.get().shared()) {
            Try<string> mount = os::shell(
                "mount --make-slave %s && "
                "mount --make-shared %s",
                flags.work_dir.c_str(),
                flags.work_dir.c_str());

            if (mount.isError()) {
              return Error(
                  "Failed to self bind mount '" + flags.work_dir +
                  "' and make it a shared mount: " + mount.error());
            }
          }

          break;
        }
      }
    }
  }

  Owned<MesosIsolatorProcess> process(
      new LinuxFilesystemIsolatorProcess(flags));

  return new MesosIsolator(process);
}


LinuxFilesystemIsolatorProcess::LinuxFilesystemIsolatorProcess(
    const Flags& _flags)
  : flags(_flags),
    metrics(PID<LinuxFilesystemIsolatorProcess>(this)) {}


LinuxFilesystemIsolatorProcess::~LinuxFilesystemIsolatorProcess() {}


Future<Nothing> LinuxFilesystemIsolatorProcess::recover(
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

  // Recover both known and unknown orphans by scanning the mount
  // table and finding those mounts whose roots are under slave's
  // sandbox root directory. Those mounts are container's work
  // directory mounts. Mounts from unknown orphans will be cleaned up
  // immediately. Mounts from known orphans will be cleaned up when
  // those known orphan containers are being destroyed by the slave.
  hashset<ContainerID> unknownOrphans;

  string sandboxRootDir = paths::getSandboxRootDir(flags.work_dir);

  foreach (const fs::MountInfoTable::Entry& entry, table.get().entries) {
    if (!strings::startsWith(entry.root, sandboxRootDir)) {
      continue;
    }

    // TODO(jieyu): Here, we retrieve the container ID by taking the
    // basename of 'entry.root'. This assumes that the slave's sandbox
    // root directory are organized according to the comments in the
    // beginning of slave/paths.hpp.
    ContainerID containerId;
    containerId.set_value(Path(entry.root).basename());

    if (infos.contains(containerId)) {
      continue;
    }

    Owned<Info> info(new Info(entry.root));

    if (entry.root != entry.target) {
      info->sandbox = entry.target;
    }

    infos.put(containerId, info);

    // Remember all the unknown orphan containers.
    if (!orphans.contains(containerId)) {
      unknownOrphans.insert(containerId);
    }
  }

  // Cleanup mounts from unknown orphans.
  list<Future<Nothing>> futures;
  foreach (const ContainerID& containerId, unknownOrphans) {
    futures.push_back(cleanup(containerId));
  }

  return collect(futures)
    .then([]() -> Future<Nothing> { return Nothing(); });
}


Future<Option<ContainerLaunchInfo>> LinuxFilesystemIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  const string& directory = containerConfig.directory();

  Option<string> user;
  if (containerConfig.has_user()) {
    user = containerConfig.user();
  }

  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  infos.put(containerId, Owned<Info>(new Info(directory)));

  const Owned<Info>& info = infos[containerId];

  ContainerLaunchInfo launchInfo;
  launchInfo.set_namespaces(CLONE_NEWNS);

  if (containerConfig.has_rootfs()) {
    // If the container changes its root filesystem, we need to mount
    // the container's work directory into its root filesystem
    // (creating it if needed) so that the executor and the task can
    // access the work directory.
    //
    // NOTE: The mount of the work directory must be a shared mount in
    // the host filesystem so that any mounts underneath it will
    // propagate into the container's mount namespace. This is how we
    // can update persistent volumes for the container.
    const string rootfs = containerConfig.rootfs();

    // This is the mount point of the work directory in the root filesystem.
    const string sandbox = path::join(rootfs, flags.sandbox_directory);

    // Save the path 'sandbox' which will be used in 'cleanup()'.
    info->sandbox = sandbox;

    Try<Nothing> mkdir = os::mkdir(sandbox);
    if (mkdir.isError()) {
      return Failure(
          "Failed to create sandbox mount point at '" +
          sandbox + "': " + mkdir.error());
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
        MS_SLAVE,
        NULL);

    if (mount.isError()) {
      return Failure(
          "Failed to mark sandbox '" + sandbox +
          "' as a slave mount: " + mount.error());
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

    launchInfo.set_rootfs(rootfs);
  }

  const ExecutorInfo& executorInfo = containerConfig.executorinfo();

  // Prepare the commands that will be run in the container's mount
  // namespace right after forking the executor process. We use these
  // commands to mount those volumes specified in the container info
  // so that they don't pollute the host mount namespace.
  Try<string> _script =
    script(containerId, executorInfo, containerConfig);
  if (_script.isError()) {
    return Failure("Failed to generate isolation script: " + _script.error());
  }

  CommandInfo* command = launchInfo.add_commands();
  command->set_value(_script.get());

  return update(containerId, executorInfo.resources())
    .then([launchInfo]() -> Future<Option<ContainerLaunchInfo>> {
      return launchInfo;
    });
}


Try<string> LinuxFilesystemIsolatorProcess::script(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const ContainerConfig& containerConfig)
{
  ostringstream out;
  out << "#!/bin/sh\n";
  out << "set -x -e\n";

  // Make sure mounts in the container mount namespace do not
  // propagate back to the host mount namespace.
  // NOTE: We cannot simply run `mount --make-rslave /`, for more info
  // please refer to comments in mount.hpp.
  MesosContainerizerMount::Flags mountFlags;
  mountFlags.operation = MesosContainerizerMount::MAKE_RSLAVE;
  mountFlags.path = "/";
  out << path::join(flags.launcher_dir, "mesos-containerizer") << " "
      << MesosContainerizerMount::NAME << " "
      << stringify(mountFlags) << "\n";

  // Try to unmount work directory mounts and persistent volume mounts
  // for other containers to release the extra references to them.
  // NOTE:
  // 1) This doesn't completely eliminate the race condition between
  //    this container copying mount table and other containers being
  //    cleaned up. This is instead a best-effort attempt.
  // 2) This script assumes that all the mounts the container needs
  //    under the slave work directory have its container ID in the
  //    path either for the mount source (e.g. sandbox self-bind mount)
  //    or the mount target (e.g. mounting sandbox into new rootfs).
  //
  // TODO(xujyan): This command may fail if --work_dir is not specified
  // with a real path as real paths are used in the mount table. It
  // doesn't work when the paths contain reserved characters such as
  // spaces either because such characters in mount info are encoded
  // in the escaped form (i.e. '\0xx').
  out << "grep -E '" << flags.work_dir << "/.+' /proc/self/mountinfo | "
      << "grep -v '" << containerId.value() << "' | "
      << "cut -d' ' -f5 | " // '-f5' is the mount target. See MountInfoTable.
      << "xargs --no-run-if-empty umount -l || "
      << "true \n"; // We mask errors in this command.

  if (!executorInfo.has_container()) {
    return out.str();
  }

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
      source = path::join(containerConfig.directory(), volume.host_path());

      // TODO(jieyu): We need to check that source resolves under the
      // work directory because a user can potentially use a container
      // path like '../../abc'.

      Try<Nothing> mkdir = os::mkdir(source);
      if (mkdir.isError()) {
        return Error(
            "Failed to create the source of the mount at '" +
            source + "': " + mkdir.error());
      }

      // TODO(idownes): Consider setting ownership and mode.
    }

    // Determine the target of the mount.
    string target;

    if (strings::startsWith(volume.container_path(), "/")) {
      if (containerConfig.has_rootfs()) {
        target = path::join(
            containerConfig.rootfs(),
            volume.container_path());
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
      if (containerConfig.has_rootfs()) {
        target = path::join(containerConfig.rootfs(),
                            flags.sandbox_directory,
                            volume.container_path());
      } else {
        target = path::join(containerConfig.directory(),
                            volume.container_path());
      }

      // TODO(jieyu): We need to check that target resolves under the
      // sandbox because a user can potentially use a container path
      // like '../../abc'.

      Try<Nothing> mkdir = os::mkdir(target);
      if (mkdir.isError()) {
        return Error(
            "Failed to create the target of the mount at '" +
            target + "': " + mkdir.error());
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
    string source = paths::getPersistentVolumePath(flags.work_dir, resource);

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
      return Failure("Failed to get ownership for '" + info->directory + "': " +
                     os::strerror(errno));
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

  bool sandboxMountExists = false;

  // Reverse unmount order to handle nested mount points.
  foreach (const fs::MountInfoTable::Entry& entry,
           adaptor::reverse(table.get().entries)) {
    // NOTE: All persistent volumes are mounted at targets under the
    // container's work directory. We unmount all the persistent
    // volumes before unmounting the sandbox/work directory mount.
    if (entry.target == sandbox) {
      sandboxMountExists = true;
    } else if (strings::startsWith(entry.target, sandbox)) {
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

  if (!sandboxMountExists) {
    // This could happen if the container was not launched by this
    // isolator (e.g., slaves prior to 0.25.0), or the container did
    // not specify a root filesystem.
    LOG(INFO) << "Ignoring unmounting sandbox/work directory"
              << " for container " << containerId;
  } else {
    LOG(INFO) << "Unmounting sandbox/work directory '" << sandbox
              << "' for container " << containerId;

    Try<Nothing> unmount = fs::unmount(sandbox);
    if (unmount.isError()) {
      return Failure(
          "Failed to unmount sandbox/work directory '" + sandbox +
          "': " + unmount.error());
    }
  }

  return Nothing();
}


LinuxFilesystemIsolatorProcess::Metrics::Metrics(
    const PID<LinuxFilesystemIsolatorProcess>& isolator)
  : containers_new_rootfs(
      "containerizer/mesos/filesystem/containers_new_rootfs",
      defer(isolator, &LinuxFilesystemIsolatorProcess::_containers_new_rootfs))
{
  process::metrics::add(containers_new_rootfs);
}


LinuxFilesystemIsolatorProcess::Metrics::~Metrics()
{
  process::metrics::remove(containers_new_rootfs);
}


double LinuxFilesystemIsolatorProcess::_containers_new_rootfs()
{
  double count = 0.0;

  foreachvalue (const Owned<Info>& info, infos) {
    if (info->sandbox.isSome()) {
      ++count;
    }
  }

  return count;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
