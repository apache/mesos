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
#include <process/id.hpp>

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
#include "slave/containerizer/mesos/paths.hpp"

#include "slave/containerizer/mesos/isolators/filesystem/linux.hpp"

using namespace process;

using std::list;
using std::ostringstream;
using std::string;
using std::vector;

using mesos::slave::ContainerClass;
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
  if (geteuid() != 0) {
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
  : ProcessBase(process::ID::generate("linux-filesystem-isolator")),
    flags(_flags),
    metrics(PID<LinuxFilesystemIsolatorProcess>(this)) {}


LinuxFilesystemIsolatorProcess::~LinuxFilesystemIsolatorProcess() {}


bool LinuxFilesystemIsolatorProcess::supportsNesting()
{
    return true;
}


Future<Nothing> LinuxFilesystemIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    Option<ExecutorInfo> executorInfo;
    if (state.has_executor_info()) {
      executorInfo = state.executor_info();
    }

    Owned<Info> info(new Info(
        state.directory(),
        executorInfo));

    infos.put(state.container_id(), info);
  }

  // Remove orphaned persistent volume mounts.
  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Failure("Failed to get mount table: " + table.error());
  }

  list<Future<Nothing>> cleanups;

  foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
    // Check for mounts inside an executor's run path. These are
    // persistent volumes mounts.
    Try<paths::ExecutorRunPath> runPath =
      slave::paths::parseExecutorRunPath(flags.work_dir, entry.target);

    if (runPath.isError()) {
      continue;
    }

    const string rootSandboxPath = paths::getExecutorRunPath(
        flags.work_dir,
        runPath->slaveId,
        runPath->frameworkId,
        runPath->executorId,
        runPath->containerId);

    Try<ContainerID> containerId =
      containerizer::paths::parseSandboxPath(
          runPath->containerId,
          rootSandboxPath,
          entry.target);

    // Since we pass the same 'entry.target' to 'parseSandboxPath' and
    // 'parseSandboxPath', we should not see an error here.
    if (containerId.isError()) {
      return Failure("Parsing sandbox path failed: " + containerId.error());
    }

    if (infos.contains(containerId.get())) {
      continue;
    }

    // TODO(josephw): We only track persistent volumes for containers
    // launched by MesosContainerizer. Nested containers or containers
    // that are listed in 'orphans' were presumably created by an
    // earlier `MesosContainerizer`. Other persistent volumes may have
    // been created by other actors, such as the
    // `DockerContainerizer`.
    if (orphans.contains(containerId.get())) {
      infos.put(containerId.get(), Owned<Info>(new Info(
          containerizer::paths::getSandboxPath(
              rootSandboxPath,
              containerId.get()))));
    } else if (containerId->has_parent()) {
      infos.put(containerId.get(), Owned<Info>(new Info(
          containerizer::paths::getSandboxPath(
              rootSandboxPath,
              containerId.get()))));

      LOG(INFO) << "Cleaning up unknown orphaned nested container "
                << containerId.get();

      cleanups.push_back(cleanup(containerId.get()));
    }
  }

  return collect(cleanups)
    .then([]() { return Nothing(); });
}


Future<Option<ContainerLaunchInfo>> LinuxFilesystemIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // If we are a nested container in the `DEBUG` class, then we only
  // use this isolator to indicate that we should enter our parent's
  // MOUNT namespace. We don't want to clone a new MOUNT namespace or
  // run any new pre-exec commands in it. For now, we also don't
  // support provisioning a new filesystem or setting a `rootfs` for
  // the container. We also don't support mounting any volumes.
  if (containerId.has_parent() &&
      containerConfig.has_container_class() &&
      containerConfig.container_class() == ContainerClass::DEBUG) {
    if (containerConfig.has_rootfs()) {
      return Failure("A 'rootfs' cannot be set for DEBUG containers");
    }

    if (containerConfig.has_container_info() &&
        containerConfig.container_info().volumes().size() > 0) {
      return Failure("Volumes not supported for DEBUG containers");
    }

    ContainerLaunchInfo launchInfo;
    launchInfo.add_enter_namespaces(CLONE_NEWNS);
    return launchInfo;
  }

  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  const string& directory = containerConfig.directory();

  Option<ExecutorInfo> executorInfo;
  if (containerConfig.has_executor_info()) {
    executorInfo = containerConfig.executor_info();
  }

  infos.put(containerId, Owned<Info>(new Info(
      directory,
      executorInfo)));

  ContainerLaunchInfo launchInfo;
  launchInfo.add_clone_namespaces(CLONE_NEWNS);

  // Prepare the commands that will be run in the container's mount
  // namespace right after forking the executor process. We use these
  // commands to mount those volumes specified in the container info
  // so that they don't pollute the host mount namespace.
  Try<vector<CommandInfo>> commands =
    getPreExecCommands(containerId, containerConfig);

  if (commands.isError()) {
    return Failure("Failed to get pre-exec commands: " + commands.error());
  }

  foreach (const CommandInfo& command, commands.get()) {
    launchInfo.add_pre_exec_commands()->CopyFrom(command);
  }

  // Currently, we only need to update resources for top level containers.
  if (containerId.has_parent()) {
    return launchInfo;
  }

  return update(containerId, containerConfig.executor_info().resources())
    .then([launchInfo]() -> Future<Option<ContainerLaunchInfo>> {
      return launchInfo;
    });
}


Try<vector<CommandInfo>> LinuxFilesystemIsolatorProcess::getPreExecCommands(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  vector<CommandInfo> commands;

  // Make sure mounts in the container mount namespace do not
  // propagate back to the host mount namespace.
  // NOTE: We cannot simply run `mount --make-rslave /`, for more info
  // please refer to comments in mount.hpp.
  CommandInfo command;
  command.set_shell(false);
  command.set_value(path::join(flags.launcher_dir, "mesos-containerizer"));
  command.add_arguments("mesos-containerizer");
  command.add_arguments(MesosContainerizerMount::NAME);

  MesosContainerizerMount::Flags mountFlags;
  mountFlags.operation = MesosContainerizerMount::MAKE_RSLAVE;
  mountFlags.path = "/";

  foreachvalue (const flags::Flag& flag, mountFlags) {
    const Option<string> value = flag.stringify(mountFlags);
    if (value.isSome()) {
      command.add_arguments(
          "--" + flag.effective_name().value + "=" + value.get());
    }
  }

  commands.push_back(command);

  if (!containerConfig.has_container_info()) {
    return commands;
  }

  // Bind mount the sandbox if the container specifies a rootfs.
  if (containerConfig.has_rootfs()) {
    string sandbox = path::join(
        containerConfig.rootfs(),
        flags.sandbox_directory);

    // If the rootfs is a read-only filesystem (e.g., using the bind
    // backend), the sandbox must be already exist. Please see the
    // comments in 'provisioner/backend.hpp' for details.
    Try<Nothing> mkdir = os::mkdir(sandbox);
    if (mkdir.isError()) {
      return Error(
          "Failed to create sandbox mount point at '" +
          sandbox + "': " + mkdir.error());
    }

    CommandInfo command;
    command.set_shell(false);
    command.set_value("mount");
    command.add_arguments("mount");
    command.add_arguments("-n");
    command.add_arguments("--rbind");
    command.add_arguments(containerConfig.directory());
    command.add_arguments(sandbox);

    commands.push_back(command);
  }

  // Get the parent sandbox user and group info for the source path.
  struct stat s;
  if (::stat(containerConfig.directory().c_str(), &s) < 0) {
    return ErrnoError("Failed to stat '" + containerConfig.directory() + "'");
  }

  const uid_t uid = s.st_uid;
  const gid_t gid = s.st_gid;

  foreach (const Volume& volume, containerConfig.container_info().volumes()) {
    // NOTE: Volumes with source will be handled by the corresponding
    // isolators (e.g., docker/volume).
    if (volume.has_source()) {
      VLOG(1) << "Ignored a volume with source for container "
              << containerId;
      continue;
    }

    if (volume.has_image()) {
      VLOG(1) << "Ignored an image volume for container " << containerId;
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

      // NOTE: Chown should be avoided if the source directory already
      // exists because it may be owned by some other user and should
      // not be mutated.
      if (!os::exists(source)) {
        Try<Nothing> mkdir = os::mkdir(source);
        if (mkdir.isError()) {
          return Error(
              "Failed to create the source of the mount at '" +
              source + "': " + mkdir.error());
        }

        LOG(INFO) << "Changing the ownership of the sandbox volume at '"
                  << source << "' with UID " << uid << " and GID " << gid;

        Try<Nothing> chown = os::chown(uid, gid, source, false);
        if (chown.isError()) {
          return Error(
              "Failed to change the ownership of the sandbox volume at '" +
              source + "' with UID " + stringify(uid) + " and GID " +
              stringify(gid) + ": " + chown.error());
        }
      }
    }

    // Determine the target of the mount. The mount target
    // is determined by 'container_path'. It can be either
    // a directory, or the path of a file.
    string target;

    if (strings::startsWith(volume.container_path(), "/")) {
      if (containerConfig.has_rootfs()) {
        target = path::join(
            containerConfig.rootfs(),
            volume.container_path());

        if (os::stat::isfile(source)) {
          // The file volume case.
          Try<Nothing> mkdir = os::mkdir(Path(target).dirname());
          if (mkdir.isError()) {
            return Error(
                "Failed to create directory '" +
                Path(target).dirname() + "' "
                "for the target mount file: " + mkdir.error());
          }

          Try<Nothing> touch = os::touch(target);
          if (touch.isError()) {
            return Error(
                "Failed to create the target mount file at '" +
                target + "': " + touch.error());
          }
        } else {
          Try<Nothing> mkdir = os::mkdir(target);
          if (mkdir.isError()) {
            return Error(
                "Failed to create the target of the mount at '" +
                target + "': " + mkdir.error());
          }
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

      if (os::stat::isfile(source)) {
        // The file volume case.
        Try<Nothing> mkdir = os::mkdir(Path(mountPoint).dirname());
        if (mkdir.isError()) {
          return Error(
              "Failed to create the target mount file directory at '" +
              Path(mountPoint).dirname() + "': " + mkdir.error());
        }

        Try<Nothing> touch = os::touch(mountPoint);
        if (touch.isError()) {
          return Error(
              "Failed to create the target mount file at '" +
              target + "': " + touch.error());
        }
      } else {
        Try<Nothing> mkdir = os::mkdir(mountPoint);
        if (mkdir.isError()) {
          return Error(
              "Failed to create the target of the mount at '" +
              mountPoint + "': " + mkdir.error());
        }
      }
    }

    // TODO(jieyu): Consider the mode in the volume.
    CommandInfo command;
    command.set_shell(false);
    command.set_value("mount");
    command.add_arguments("mount");
    command.add_arguments("-n");
    command.add_arguments("--rbind");
    command.add_arguments(source);
    command.add_arguments(target);

    commands.push_back(command);
  }

  return commands;
}


Future<Nothing> LinuxFilesystemIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (containerId.has_parent()) {
    return Failure("Not supported for nested containers");
  }

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

  // Get user and group info for this task based on the task's sandbox.
  struct stat s;
  if (::stat(info->directory.c_str(), &s) < 0) {
    return Failure("Failed to get ownership for '" + info->directory +
                   "': " + os::strerror(errno));
  }

  const uid_t uid = s.st_uid;
  const gid_t gid = s.st_gid;

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

    bool isVolumeInUse = false;

    foreachvalue (const Owned<Info>& info, infos) {
      if (info->resources.contains(resource)) {
        isVolumeInUse = true;
        break;
      }
    }

    // Set the ownership of the persistent volume to match that of the sandbox
    // directory if the volume is not already in use. If the volume is
    // currently in use by other containers, tasks in this container may fail
    // to read from or write to the persistent volume due to incompatible
    // ownership and file system permissions.
    if (!isVolumeInUse) {
      LOG(INFO) << "Changing the ownership of the persistent volume at '"
                << source << "' with uid " << uid << " and gid " << gid;

      Try<Nothing> chown = os::chown(uid, gid, source, false);

      if (chown.isError()) {
        return Failure(
            "Failed to change the ownership of the persistent volume at '" +
            source + "' with uid " + stringify(uid) +
            " and gid " + stringify(gid) + ": " + chown.error());
      }
    }

    // Determine the target of the mount.
    string target = path::join(info->directory, containerPath);

    if (os::exists(target)) {
      // NOTE: There are two scenarios that we may have the mount
      // target existed:
      // 1. This is possible because 'info->resources' will be reset
      //    when slave restarts and recovers. When the slave calls
      //    'containerizer->update' after the executor re-registers,
      //    we'll try to re-mount all the already mounted volumes.
      // 2. There may be multiple references to the persistent
      //    volume's mount target. E.g., a host volume and a
      //    persistent volume are both specified, and the source
      //    of the host volume is the same as the container path
      //    of the persistent volume.

      // Check the source of the mount matches the entry with the
      // same target in the mount table if one can be found. If
      // not, mount the persistent volume as we did below. This is
      // possible because the slave could crash after it unmounts the
      // volume but before it is able to delete the mount point.
      Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
      if (table.isError()) {
        return Failure("Failed to get mount table: " + table.error());
      }

      // Check a particular persistent volume is mounted or not.
      bool volumeMounted = false;

      foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
        // TODO(gilbert): Check source of the mount matches the entry's
        // root. Note that the root is relative to the root of its parent
        // mount. See:
        // http://man7.org/linux/man-pages/man5/proc.5.html
        if (target == entry.target) {
          volumeMounted = true;
          break;
        }
      }

      if (volumeMounted) {
        continue;
      }
    }

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

    // If the mount needs to be read-only, do a remount.
    if (resource.disk().volume().mode() == Volume::RO) {
      mount = fs::mount(
          None(), target, None(), MS_BIND | MS_RDONLY | MS_REMOUNT, nullptr);

      if (mount.isError()) {
        return Failure(
            "Failed to remount persistent volume as read-only from '" +
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

  // Make sure the container we are cleaning up doesn't have any
  // children (they should have already been cleaned up by a previous
  // call if it had any).
  foreachkey (const ContainerID& _containerId, infos) {
    if (_containerId.has_parent() && _containerId.parent() == containerId) {
      return Failure(
          "Container " + stringify(containerId) + " has non terminated "
          "child container " + stringify(_containerId));
    }
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

  vector<string> unmountErrors;

  // Reverse unmount order to handle nested mount points.
  foreach (const fs::MountInfoTable::Entry& entry,
           adaptor::reverse(table->entries)) {
    // NOTE: All persistent volumes are mounted at targets under the
    // container's work directory. We unmount all the persistent
    // volumes before unmounting the sandbox/work directory mount.
    if (strings::startsWith(entry.target, sandbox)) {
      LOG(INFO) << "Unmounting volume '" << entry.target
                << "' for container " << containerId;

      // TODO(jieyu): Use MNT_DETACH here to workaround an issue of
      // incorrect handling of container destroy failures. Currently,
      // if isolator cleanup returns a failure, the slave will treat
      // the container as terminated, and will schedule the cleanup of
      // the container's sandbox. Since the mount hasn't been removed
      // in the sandbox, that'll result in data in the persistent
      // volume being incorrectly deleted. Use MNT_DETACH here so that
      // the mount point in the sandbox will be removed immediately.
      // See MESOS-7366 for more details.
      Try<Nothing> unmount = fs::unmount(entry.target, MNT_DETACH);
      if (unmount.isError()) {
        // NOTE: Instead of short circuit, we try to perform as many
        // unmount as possible. We'll accumulate the errors together
        // in the end.
        unmountErrors.push_back(
            "Failed to unmount volume '" + entry.target +
            "': " + unmount.error());
      }
    }
  }

  if (!unmountErrors.empty()) {
    return Failure(strings::join(", ", unmountErrors));
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


// TODO(gilbert): Currently, this only supports counting rootfses for
// top level containers. We should figure out another way to collect
// this information if necessary.
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
