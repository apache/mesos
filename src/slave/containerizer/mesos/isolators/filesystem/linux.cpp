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

  // Make sure that slave's working directory is in a shared mount so
  // that when forking a child process (with a new mount namespace),
  // the child process does not hold extra references to container's
  // persistent volume mounts and provisioner mounts (e.g., when using
  // the bind/overlayfs backend). This ensures that cleanup operations
  // within slave's working directory can be propagated to all
  // containers. See MESOS-3483 for more details.

  // Mount table entries use realpaths. Therefore, we first get the
  // realpath of the slave's working directory.
  Result<string> workDir = os::realpath(flags.work_dir);
  if (!workDir.isSome()) {
    return Error(
        "Failed to get the realpath of slave's working directory: " +
        (workDir.isError() ? workDir.error() : "Not found"));
  }

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Error("Failed to get mount table: " + table.error());
  }

  // Trying to find the mount entry that contains the slave's working
  // directory. We achieve that by doing a reverse traverse of the
  // mount table to find the first entry whose target is a prefix of
  // slave's working directory.
  Option<fs::MountInfoTable::Entry> workDirMount;
  foreach (const fs::MountInfoTable::Entry& entry,
           adaptor::reverse(table->entries)) {
    if (strings::startsWith(workDir.get(), entry.target)) {
      workDirMount = entry;
      break;
    }
  }

  // It's unlikely that we cannot find 'workDirMount' because '/' is
  // always mounted and will be the 'workDirMount' if no other mounts
  // found in between.
  if (workDirMount.isNone()) {
    return Error("Cannot find the mount containing slave's working directory");
  }

  // If 'workDirMount' is a shared mount in its own peer group, then
  // we don't need to do anything. Otherwise, we need to do a self
  // bind mount of slave's working directory to make sure it's a
  // shared mount in its own peer group.
  bool bindMountNeeded = false;

  if (workDirMount->shared().isNone()) {
    bindMountNeeded = true;
  } else {
    foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
      // Skip 'workDirMount' and any mount underneath it. Also, we
      // skip those mounts whose targets are not the parent of the
      // working directory because even if they are in the same peer
      // group as the working directory mount, it won't affect it.
      if (entry.id != workDirMount->id &&
          !strings::startsWith(entry.target, workDir.get()) &&
          entry.shared() == workDirMount->shared() &&
          strings::startsWith(workDir.get(), entry.target)) {
        bindMountNeeded = true;
        break;
      }
    }
  }

  if (bindMountNeeded) {
    if (workDirMount->target != workDir.get()) {
      // This is the case where the working directory mount does not
      // exist in the mount table (e.g., a new host running Mesos
      // slave for the first time).
      LOG(INFO) << "Bind mounting '" << workDir.get()
                << "' and making it a shared mount";

      // NOTE: Instead of using fs::mount to perform the bind mount,
      // we use the shell command here because the syscall 'mount'
      // does not update the mount table (i.e., /etc/mtab). In other
      // words, the mount will not be visible if the operator types
      // command 'mount'. Since this mount will still be presented
      // after all containers and the slave are stopped, it's better
      // to make it visible. It's OK to use the blocking os::shell
      // here because 'create' will only be invoked during
      // initialization.
      Try<string> mount = os::shell(
          "mount --bind %s %s && "
          "mount --make-private %s && "
          "mount --make-shared %s",
          workDir->c_str(),
          workDir->c_str(),
          workDir->c_str(),
          workDir->c_str());

      if (mount.isError()) {
        return Error(
            "Failed to bind mount '" + workDir.get() +
            "' and make it a shared mount: " + mount.error());
      }
    } else {
      // This is the case where the working directory mount is in the
      // mount table, but it's not a shared mount in its own peer
      // group (possibly due to slave crash while preparing the
      // working directory mount). It's safe to re-do the following.
      LOG(INFO) << "Making '" << workDir.get() << "' a shared mount";

      Try<string> mount = os::shell(
          "mount --make-private %s && "
          "mount --make-shared %s",
          workDir->c_str(),
          workDir->c_str());

      if (mount.isError()) {
        return Error(
            "Failed to make '" + workDir.get() +
            "' a shared mount: " + mount.error());
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
  foreach (const ContainerState& state, states) {
    Owned<Info> info(new Info(
        state.directory(),
        state.executor_info()));

    infos.put(state.container_id(), info);
  }

  // Remove orphaned persistent volume mounts.
  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Failure("Failed to get mount table: " + table.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    // Check for mounts inside an executor's run path. These are
    // persistent volumes mounts.
    Try<paths::ExecutorRunPath> runPath =
      paths::parseExecutorRunPath(flags.work_dir, entry.target);

    if (runPath.isError()) {
      continue;
    }

    if (infos.contains(runPath->containerId)) {
      continue;
    }

    // TODO(josephw): We only track persistent volumes for known
    // orphans as these orphans were presumably created by an earlier
    // `MesosContainerizer`. Other persistent volumes may have been
    // created by other actors, such as the `DockerContainerizer`.
    if (orphans.contains(runPath->containerId)) {
      Owned<Info> info(new Info(paths::getExecutorRunPath(
          flags.work_dir,
          runPath->slaveId,
          runPath->frameworkId,
          runPath->executorId,
          runPath->containerId)));

      infos.put(runPath->containerId, info);
    }
  }

  return Nothing();
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

  Owned<Info> info(new Info(
      directory,
      containerConfig.executor_info()));

  infos.put(containerId, info);

  ContainerLaunchInfo launchInfo;
  launchInfo.set_namespaces(CLONE_NEWNS);

  // Prepare the commands that will be run in the container's mount
  // namespace right after forking the executor process. We use these
  // commands to mount those volumes specified in the container info
  // so that they don't pollute the host mount namespace.
  Try<string> _script = script(containerId, containerConfig);
  if (_script.isError()) {
    return Failure("Failed to generate isolation script: " + _script.error());
  }

  CommandInfo* command = launchInfo.add_commands();
  command->set_value(_script.get());

  return update(containerId, containerConfig.executor_info().resources())
    .then([launchInfo]() -> Future<Option<ContainerLaunchInfo>> {
      return launchInfo;
    });
}


Try<string> LinuxFilesystemIsolatorProcess::script(
    const ContainerID& containerId,
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

  if (!containerConfig.executor_info().has_container()) {
    return out.str();
  }

  // Bind mount the sandbox if the container specifies a rootfs.
  if (containerConfig.has_rootfs()) {
    string sandbox = path::join(
        containerConfig.rootfs(),
        flags.sandbox_directory);

    Try<Nothing> mkdir = os::mkdir(sandbox);
    if (mkdir.isError()) {
      return Error(
          "Failed to create sandbox mount point at '" +
          sandbox + "': " + mkdir.error());
    }

    out << "mount -n --rbind '" << containerConfig.directory()
        << "' '" << sandbox << "'\n";
  }

  foreach (const Volume& volume,
           containerConfig.executor_info().container().volumes()) {
    // NOTE: Volumes with source will be handled by the corresponding
    // isolators (e.g., docker/volume).
    if (volume.has_source()) {
      VLOG(1) << "Ignored a volume with source for container '"
              << containerId << "'";
      continue;
    }

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
        return Error("Absolute host path '" + source + "' does not exist");
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

        Try<Nothing> mkdir = os::mkdir(target);
        if (mkdir.isError()) {
          return Error(
              "Failed to create the target of the mount at '" +
              target + "': " + mkdir.error());
        }
      } else {
        target = volume.container_path();

        // An absolute path must already exist. This is because we
        // want to avoid creating mount points outside the work
        // directory in the host filesystem.
        if (!os::exists(target)) {
          return Error("Absolute container path '" + target + "' "
                       "does not exist");
        }
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

      // NOTE: We cannot create the mount point at 'target' if
      // container has rootfs defined. The bind mount of the sandbox
      // will hide what's inside 'target'. So we should always create
      // the mount point in 'directory'.
      string mountPoint = path::join(
          containerConfig.directory(),
          volume.container_path());

      Try<Nothing> mkdir = os::mkdir(mountPoint);
      if (mkdir.isError()) {
        return Error(
            "Failed to create the target of the mount at '" +
            mountPoint + "': " + mkdir.error());
      }
    }

    // TODO(jieyu): Consider the mode in the volume.
    out << "mount -n --rbind '" << source << "' '" << target << "'\n";
  }

  return out.str();
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
    string target = path::join(info->directory, containerPath);

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
    string target = path::join(info->directory, containerPath);

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

      Try<Nothing> mount = fs::mount(source, target, None(), MS_BIND, nullptr);
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
  string sandbox = info->directory;

  infos.erase(containerId);

  // Cleanup the mounts for this container in the host mount
  // namespace, including container's work directory and all the
  // persistent volume mounts.
  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Failure("Failed to get mount table: " + table.error());
  }

  // Reverse unmount order to handle nested mount points.
  foreach (const fs::MountInfoTable::Entry& entry,
           adaptor::reverse(table->entries)) {
    // NOTE: All persistent volumes are mounted at targets under the
    // container's work directory. We unmount all the persistent
    // volumes before unmounting the sandbox/work directory mount.
    if (strings::startsWith(entry.target, sandbox)) {
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
    if (info->executor.isSome() &&
        info->executor->has_container() &&
        info->executor->container().type() == ContainerInfo::MESOS &&
        info->executor->container().mesos().has_image()) {
      ++count;
    }
  }

  return count;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
