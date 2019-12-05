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

#include <sys/mount.h>

#include <sstream>
#include <string>
#include <vector>
#include <utility>

#include <glog/logging.h>

#include <process/collect.hpp>
#include <process/id.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/adaptor.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/realpath.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/strerror.hpp>

#include "common/protobuf_utils.hpp"

#include "linux/fs.hpp"
#include "linux/ns.hpp"

#include "slave/paths.hpp"

#include "slave/containerizer/mesos/mount.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "slave/containerizer/mesos/isolators/filesystem/linux.hpp"

using namespace process;

using std::ostringstream;
using std::pair;
using std::string;
using std::vector;

using mesos::internal::protobuf::slave::createContainerMount;
using mesos::internal::protobuf::slave::containerSymlinkOperation;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerMountInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

// List of special filesystems useful for a chroot environment.
// NOTE: This list is ordered, e.g., mount /proc before bind
// mounting /proc/sys.
//
// TODO(jasonlai): These special filesystem mount points need to be
// bind-mounted prior to all other mount points specified in
// `ContainerLaunchInfo`.
//
// One example of the known issues caused by this behavior is:
// https://issues.apache.org/jira/browse/MESOS-6798
// There will be follow-up efforts on moving the logic below to
// proper isolators.
//
// TODO(jasonlai): Consider adding knobs to allow write access to
// those system files if configured by the operator.
static const ContainerMountInfo ROOTFS_CONTAINER_MOUNTS[] = {
  createContainerMount(
      "proc",
      "/proc",
      "proc",
      MS_NOSUID | MS_NOEXEC | MS_NODEV),
  createContainerMount(
      "/proc/bus",
      "/proc/bus",
      MS_BIND | MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV),
  createContainerMount(
      "/proc/fs",
      "/proc/fs",
      MS_BIND | MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV),
  createContainerMount(
      "/proc/irq",
      "/proc/irq",
      MS_BIND | MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV),
  createContainerMount(
      "/proc/sys",
      "/proc/sys",
      MS_BIND | MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV),
  createContainerMount(
      "/proc/sysrq-trigger",
      "/proc/sysrq-trigger",
      MS_BIND | MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV),
  createContainerMount(
      "sysfs",
      "/sys",
      "sysfs",
      MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV),
  createContainerMount(
      "tmpfs",
      "/sys/fs/cgroup",
      "tmpfs",
      "mode=755",
      MS_NOSUID | MS_NOEXEC | MS_NODEV),
  createContainerMount(
      "tmpfs",
      "/dev",
      "tmpfs",
      "mode=755",
      MS_NOSUID | MS_NOEXEC | MS_STRICTATIME),
  // We mount devpts with the gid=5 option because the `tty` group is
  // GID 5 on all standard Linux distributions. The glibc grantpt(3)
  // API ensures that the terminal GID is that of the `tty` group, and
  // invokes a privileged helper if necessary. Since the helper won't
  // work in all container configurations (since it may not be possible
  // to acquire the necessary privileges), mounting with the right `gid`
  // option avoids any possible failure.
  createContainerMount(
      "devpts",
      "/dev/pts",
      "devpts",
      "newinstance,ptmxmode=0666,mode=0620,gid=5",
      MS_NOSUID | MS_NOEXEC),
};


static const vector<string> ROOTFS_MASKED_PATHS = {
  "/proc/acpi",
  "/proc/asound",
  "/proc/kcore",
  "/proc/keys",
  "/proc/key-users",
  "/proc/latency_stats",
  "/proc/sched_debug",
  "/proc/scsi",
  "/proc/timer_list",
  "/proc/timer_stats",
  "/sys/firmware",
};


static Try<Nothing> makeStandardDevices(
    const string& devicesDir,
    const string& rootDir,
    ContainerLaunchInfo& launchInfo)
{
  // List of standard devices useful for a chroot environment.
  // TODO(idownes): Make this list configurable.
  const vector<string> devices = {
    "full",
    "null",
    "random",
    "tty",
    "urandom",
    "zero"
  };

  // Import each device into the chroot environment. Copy both the
  // mode and the device itself from the corresponding host device.
  foreach (const string& device, devices) {
    Try<Nothing> mknod = fs::chroot::copyDeviceNode(
        path::join("/",  "dev", device),
        path::join(devicesDir, device));

    if (mknod.isError()) {
      return Error(
          "Failed to import device '" + device + "': " + mknod.error());
    }

    // Bind mount from the devices directory into the rootfs.
    *launchInfo.add_mounts() = createContainerMount(
        path::join(devicesDir, device),
        path::join(rootDir, "dev", device),
        MS_BIND);
  }

  const vector<pair<string, string>> symlinks = {
    {"/proc/self/fd",   path::join(rootDir, "dev", "fd")},
    {"/proc/self/fd/0", path::join(rootDir, "dev", "stdin")},
    {"/proc/self/fd/1", path::join(rootDir, "dev", "stdout")},
    {"/proc/self/fd/2", path::join(rootDir, "dev", "stderr")},
    {"pts/ptmx",        path::join(rootDir, "dev", "ptmx")}
  };

  foreach (const auto& symlink, symlinks) {
    *launchInfo.add_file_operations() =
      containerSymlinkOperation(symlink.first, symlink.second);
  }

  // TODO(idownes): Set up console device.
  return Nothing();
}


static Try<Nothing> makeDevicesDir(
    const string& devicesDir,
    const Option<string>& username)
{
  Try<Nothing> mkdir = os::mkdir(devicesDir);
  if (mkdir.isError()) {
    return Error(
        "Failed to create container devices directory: " + mkdir.error());
  }

  Try<Nothing> chmod = os::chmod(devicesDir, 0700);
  if (chmod.isError()) {
    return Error(
        "Failed to set container devices directory permissions: " +
        chmod.error());
  }

  // We need to restrict access to the devices directory so that all
  // processes on the system don't get access to devices that we make
  // read-write. This means that we have to chown to ensure that the
  // container user still has access.
  if (username.isSome()) {
    Try<Nothing> chown = os::chown(username.get(), devicesDir);
    if (chown.isError()) {
      return Error(
          "Failed to set '" + username.get() + "' "
          "as the container devices directory owner: " + chown.error());
    }
  }

  return Nothing();
}


// Make sure that the specified target directory is in a shared mount
// so that when forking a child process (with a new mount namespace),
// the child process does not hold extra references to the mounts
// underneath the target directory. For instance, container's
// persistent volume mounts and provisioner mounts (e.g., when using
// the bind/overlayfs backend) under agent's `work_dir`. This ensures
// that cleanup operations (i.e., unmount) on the host mount namespace
// can be propagated to child's mount namespaces. See MESOS-3483 for
// more details.
// TODO(jieyu): Consider moving this helper to 'src/linux/fs.hpp|cpp'.
static Try<Nothing> ensureSharedMount(const string& _targetDir)
{
  // Mount table entries use realpaths. Therefore, we first get the
  // realpath of the target directory.
  Result<string> targetDir = os::realpath(_targetDir);
  if (!targetDir.isSome()) {
    return Error(
        "Failed to get the realpath of '" + _targetDir + "': " +
        (targetDir.isError() ? targetDir.error() : "Not found"));
  }

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Error("Failed to get mount table: " + table.error());
  }

  // Trying to find the mount entry that contains the target
  // directory. We achieve that by doing a reverse traverse of the
  // mount table to find the first entry whose target is a prefix of
  // the target directory.
  Try<fs::MountInfoTable::Entry> targetDirMount =
    table->findByTarget(_targetDir);

  if (targetDirMount.isError()) {
    return Error(
        "Failed to find the mount containing '" + _targetDir +
        "': " + targetDirMount.error());
  }

  // If 'targetDirMount' is a shared mount in its own peer group, then
  // we don't need to do anything. Otherwise, we need to do a self
  // bind mount of the target directory to make sure it's a shared
  // mount in its own peer group.
  bool bindMountNeeded = false;

  if (targetDirMount->shared().isNone()) {
    bindMountNeeded = true;
  } else {
    foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
      // Skip 'targetDirMount' and any mount underneath it. Also, we
      // skip those mounts whose targets are not the parent of the
      // target directory because even if they are in the same peer
      // group as the working directory mount, it won't affect it.
      if (entry.id != targetDirMount->id &&
          !strings::startsWith(entry.target, path::join(targetDir.get(), "")) &&
          entry.shared() == targetDirMount->shared() &&
          strings::startsWith(targetDir.get(), path::join(entry.target, ""))) {
        bindMountNeeded = true;
        break;
      }
    }
  }

  if (bindMountNeeded) {
    if (targetDirMount->target != targetDir.get()) {
      // This is the case where the target directory mount does not
      // exist in the mount table (e.g., a new host running Mesos
      // slave for the first time).
      LOG(INFO) << "Bind mounting '" << targetDir.get()
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
          targetDir.get(),
          targetDir.get(),
          targetDir.get(),
          targetDir.get());

      if (mount.isError()) {
        return Error(
            "Failed to bind mount '" + targetDir.get() +
            "' and make it a shared mount: " + mount.error());
      }
    } else {
      // This is the case where the target directory mount is in the
      // mount table, but it's not a shared mount in its own peer
      // group (possibly due to slave crash while preparing the
      // target directory mount). It's safe to re-do the following.
      LOG(INFO) << "Making '" << targetDir.get() << "' a shared mount";

      Try<string> mount = os::shell(
          "mount --make-private %s && "
          "mount --make-shared %s",
          targetDir.get(),
          targetDir.get());

      if (mount.isError()) {
        return Error(
            "Failed to make '" + targetDir.get() +
            "' a shared mount: " + mount.error());
      }
    }
  }

  return Nothing();
}


// Make sure the target directory allow device files (i.e., there no
// `nodev` on the mounted filesystem that contains the target path).
static Try<Nothing> ensureAllowDevices(const string& _targetDir)
{
  // Mount table entries use realpaths. Therefore, we first get the
  // realpath of the target directory.
  Result<string> targetDir = os::realpath(_targetDir);
  if (!targetDir.isSome()) {
    return Error(
        "Failed to get the realpath of '" + _targetDir + "': " +
        (targetDir.isError() ? targetDir.error() : "Not found"));
  }

  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Error("Failed to get mount table: " + table.error());
  }

  // Trying to find the mount entry that contains the target
  // directory. We achieve that by doing a reverse traverse of the
  // mount table to find the first entry whose target is a prefix of
  // the target directory.
  Try<fs::MountInfoTable::Entry> targetDirMount =
    table->findByTarget(_targetDir);

  if (targetDirMount.isError()) {
    return Error(
        "Failed to find the mount containing '" + _targetDir +
        "': " + targetDirMount.error());
  }

  // No need to do anything if the mount has no `nodev`.
  if (!strings::contains(targetDirMount->vfsOptions, "nodev")) {
    return Nothing();
  }

  if (targetDirMount->target != targetDir.get()) {
    // This is the case where the target directory mount does not
    // exist in the mount table (e.g., a new host running Mesos
    // slave for the first time).
    LOG(INFO) << "Self bind mounting '" << targetDir.get()
              << "' and remounting with '-o remount,dev'";

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
        "mount -o remount,dev %s",
        targetDir.get(),
        targetDir.get(),
        targetDir.get());

    if (mount.isError()) {
      return Error(
          "Failed to self bind mount '" + targetDir.get() +
          "' and remount with '-o remount,dev': " + mount.error());
    }
  } else {
    // This is the case where the target directory mount is in the
    // mount table, but it's not remounted yet to remove 'nodev'
    // (possibly due to slave crash while preparing the target
    // directory mount). It's safe to re-do the following.
    LOG(INFO) << "Remounting '" << targetDir.get() << "' with '-o remount,dev'";

    Try<string> mount = os::shell(
        "mount -o remount,dev %s",
        targetDir.get());

    if (mount.isError()) {
      return Error(
          "Failed to remount '" + targetDir.get() +
          "' with '-o remount,dev': " + mount.error());
    }
  }

  return Nothing();
}


// We define a container is privileged if it is sharing the PID
// namespace with the host. For nested containers, we walk up
// the tree and verify it is shared all the way up to the root.
static Try<bool> isPrivilegedContainer(
    const string runtimeDir,
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (!containerConfig.container_info().linux_info().share_pid_namespace()) {
    return false;
  }

  CHECK(containerConfig.container_info().linux_info().share_pid_namespace());

  // If we are a root container, we are privileged because we share
  // the host's PID namespace.
  if (!containerId.has_parent()) {
    return true;
  }

  // If we are a nested container, we have to walk up the container tree.
  ContainerID parentId = containerId.parent();
  Result<ContainerConfig> parentConfig =
    containerizer::paths::getContainerConfig(runtimeDir, parentId);

  if (parentConfig.isNone()) {
    return Error(
        "Failed to find config for parent container " + stringify(parentId));
  }

  if (parentConfig.isError()) {
    return Error(parentConfig.error());
  }

  return isPrivilegedContainer(runtimeDir, parentId, parentConfig.get());
}


Try<Isolator*> LinuxFilesystemIsolatorProcess::create(
    const Flags& flags,
    VolumeGidManager* volumeGidManager)
{
  if (geteuid() != 0) {
    return Error("'filesystem/linux' isolator requires root privileges");
  }

  if (flags.launcher != "linux") {
    return Error("'filesystem/linux' isolator requires 'linux' launcher");
  }


  Try<bool> supported = ns::supported(CLONE_NEWNS);
  if (supported.isError() || !supported.get()) {
    return Error(
        "The 'filesystem/linux' isolator requires mount namespace support");
  }

  // Make sure that slave's working directory is in a shared mount so
  // that when forking a child process (with a new mount namespace),
  // the child process does not hold extra references to container's
  // persistent volume mounts and provisioner mounts (e.g., when using
  // the bind/overlayfs backend). This ensures that cleanup operations
  // within slave's working directory can be propagated to all
  // containers. See MESOS-3483 for more details.
  Try<Nothing> workDirSharedMount = ensureSharedMount(flags.work_dir);
  if (workDirSharedMount.isError()) {
    return Error(workDirSharedMount.error());
  }

  // Make sure that container's runtime dir has device file access.
  // Some Linux distributions will mount `/run` with `nodev`,
  // restricting accessing to device files under `/run`. However,
  // Mesos prepares device files for containers under container's
  // runtime dir (which is typically under `/run`) and bind mount into
  // container root filesystems. Therefore, we need to make sure those
  // device files can be accessed by the container. We need to do a
  // self bind mount and remount with proper options if necessary. See
  // MESOS-9462 for more details.
  const string containersRuntimeDir = path::join(
      flags.runtime_dir,
      containerizer::paths::CONTAINER_DIRECTORY);

  Try<Nothing> mkdir = os::mkdir(containersRuntimeDir);
  if (mkdir.isError()) {
    return Error(
        "Failed to create container's runtime dir at '" +
        containersRuntimeDir + "': " + mkdir.error());
  }

  Try<Nothing> containersDirMount = ensureAllowDevices(containersRuntimeDir);
  if (containersDirMount.isError()) {
    return Error(containersDirMount.error());
  }

  Owned<MesosIsolatorProcess> process(
      new LinuxFilesystemIsolatorProcess(flags, volumeGidManager));

  return new MesosIsolator(process);
}


LinuxFilesystemIsolatorProcess::LinuxFilesystemIsolatorProcess(
    const Flags& _flags,
    VolumeGidManager* _volumeGidManager)
  : ProcessBase(process::ID::generate("linux-filesystem-isolator")),
    flags(_flags),
    volumeGidManager(_volumeGidManager),
    metrics(PID<LinuxFilesystemIsolatorProcess>(this)) {}


LinuxFilesystemIsolatorProcess::~LinuxFilesystemIsolatorProcess() {}


bool LinuxFilesystemIsolatorProcess::supportsNesting()
{
  return true;
}


bool LinuxFilesystemIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Nothing> LinuxFilesystemIsolatorProcess::recover(
    const vector<ContainerState>& states,
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

  vector<Future<Nothing>> cleanups;

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

  // Currently, we do not support persistent volumes for standalone
  // containers. Therefore, we perform the check here to reject the
  // standalone container launch if persistent volumes are specified.
  const bool isStandaloneContainer =
    containerizer::paths::isStandaloneContainer(flags.runtime_dir, containerId);

  if (isStandaloneContainer &&
      !Resources(containerConfig.resources()).persistentVolumes().empty()) {
    return Failure(
        "Persistent volumes are not supported for standalone containers");
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

  if (containerConfig.has_rootfs()) {
    // Set up the container devices directory.
    const string devicesDir = containerizer::paths::getContainerDevicesPath(
        flags.runtime_dir, containerId);

    CHECK(!os::exists(devicesDir));

    Try<Nothing> mkdir = makeDevicesDir(
        devicesDir,
        containerConfig.has_user() ? containerConfig.user()
                                   : Option<string>::none());
    if (mkdir.isError()) {
      return Failure(
          "Failed to create container devices directory: " + mkdir.error());
    }

    // Bind mount 'root' itself. This is because pivot_root requires
    // 'root' to be not on the same filesystem as process' current root.
    *launchInfo.add_mounts() = createContainerMount(
        containerConfig.rootfs(),
        containerConfig.rootfs(),
        MS_REC | MS_BIND);

    foreach (const ContainerMountInfo& mnt, ROOTFS_CONTAINER_MOUNTS) {
      // The target for special mounts must always be an absolute path.
      CHECK(path::is_absolute(mnt.target()));

      ContainerMountInfo* info = launchInfo.add_mounts();

      *info = mnt;
      info->set_target(path::join(containerConfig.rootfs(), mnt.target()));

      // Absolute path mounts are always relative to the container root.
      if (mnt.has_source() && path::is_absolute(mnt.source())) {
        info->set_source(path::join(containerConfig.rootfs(), info->source()));
      }
    }

    // If `namespaces/ipc` isolator is not enabled, for backward compatibility
    // we will keep the previous behavior: if the container has its own rootfs,
    // it will have its own /dev/shm, otherwise it will share agent's /dev/shm.
    // If `namespaces/ipc` isolator is enabled, /dev/shm will be handled there.
    if (!strings::contains(flags.isolation, "namespaces/ipc")) {
      *launchInfo.add_mounts() = createContainerMount(
          "tmpfs",
          path::join(containerConfig.rootfs(), "/dev/shm"),
          "tmpfs",
          "mode=1777",
          MS_NOSUID | MS_NODEV | MS_STRICTATIME);
    }

    Try<Nothing> makedev =
      makeStandardDevices(devicesDir, containerConfig.rootfs(), launchInfo);
    if (makedev.isError()) {
      return Failure(
          "Failed to prepare standard devices: " + makedev.error());
    }

    // Bind mount the sandbox if the container specifies a rootfs.
    const string sandbox = path::join(
        containerConfig.rootfs(),
        flags.sandbox_directory);

    // If the rootfs is a read-only filesystem (e.g., using the bind
    // backend), the sandbox must be already exist. Please see the
    // comments in 'provisioner/backend.hpp' for details.
    mkdir = os::mkdir(sandbox);
    if (mkdir.isError()) {
      return Failure(
          "Failed to create sandbox mount point at '" +
          sandbox + "': " + mkdir.error());
    }

    *launchInfo.add_mounts() = createContainerMount(
        containerConfig.directory(), sandbox, MS_BIND | MS_REC);

    Try<bool> privileged =
      isPrivilegedContainer(flags.runtime_dir, containerId, containerConfig);
    if (privileged.isError()) {
      return Failure(privileged.error());
    }

    // Apply container path masking for non-privileged containers.
    if (!privileged.get()) {
      foreach (const string& path, ROOTFS_MASKED_PATHS) {
        launchInfo.add_masked_paths(
            path::join(containerConfig.rootfs(), path));
      }
    }
  }

  // Currently, we only need to update resources for top level containers.
  if (containerId.has_parent()) {
    return launchInfo;
  }

  return update(containerId, containerConfig.resources())
    .then(defer(
        self(),
        [this, containerId, containerConfig, launchInfo]() mutable
            -> Future<Option<ContainerLaunchInfo>> {
          if (!infos.contains(containerId)) {
            return Failure("Unknown container");
          }

          foreach (gid_t gid, infos[containerId]->gids) {
            // For command task with its own rootfs, the command executor will
            // run as root and the task itself will run as the specified normal
            // user, so here we add the supplementary group for the task and the
            // command executor will set it accordingly when launching the task.
            if (containerConfig.has_task_info() &&
                containerConfig.has_rootfs()) {
              launchInfo.add_task_supplementary_groups(gid);
            } else {
              launchInfo.add_supplementary_groups(gid);
            }
          }

          return launchInfo;
    }));
}


Future<Nothing> LinuxFilesystemIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
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

    if (resourceRequests.contains(resource)) {
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

  vector<Future<gid_t>> futures;

  // We then mount new persistent volumes.
  foreach (const Resource& resource, resourceRequests.persistentVolumes()) {
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

    // If the container's user is root (uid == 0), we do not need to do any
    // changes about the volume's ownership since it has the full permissions
    // to access the volume.
    if (uid != 0) {
      // For persistent volumes not from resource providers, if volume gid
      // manager is enabled, call volume gid manager to allocate a gid to
      // make sure the container has the permission to access the volume.
      //
      // TODO(qianzhang): Support gid allocation for persistent volumes from
      // resource providers.
      if (!Resources::hasResourceProvider(resource) &&
          volumeGidManager) {
        LOG(INFO) << "Invoking volume gid manager to allocate gid to the "
                  << "volume path '" << source << "' for container "
                  << containerId;

        futures.push_back(
            volumeGidManager->allocate(source, VolumeGidInfo::PERSISTENT));
      } else {
        bool isVolumeInUse = false;

        // Check if the shared persistent volume is currently used by another
        // container. We do not need to do this check for local persistent
        // volume since it can only be used by one container at a time.
        if (resource.has_shared()) {
          foreachpair (const ContainerID& _containerId,
                       const Owned<Info>& info,
                       infos) {
            // Skip self.
            if (_containerId == containerId) {
              continue;
            }

            if (info->resources.contains(resource)) {
              isVolumeInUse = true;
              break;
            }
          }
        }

        // Set the ownership of the persistent volume to match that of the
        // sandbox directory if the volume is not already in use. If the
        // volume is currently in use by other containers, tasks in this
        // container may fail to read from or write to the persistent volume
        // due to incompatible ownership and file system permissions.
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
        } else {
          LOG(INFO) << "Leaving the ownership of the persistent volume at '"
                    << source << "' unchanged because it is in use";
        }
      }
    }

    // Determine the target of the mount.
    string target = path::join(info->directory, containerPath);

    if (os::exists(target)) {
      // NOTE: There are two scenarios that we may have the mount
      // target existed:
      // 1. This is possible because 'info->resources' will be reset
      //    when slave restarts and recovers. When the slave calls
      //    'containerizer->update' after the executor reregisters,
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

    const unsigned mountFlags =
      MS_BIND | (resource.disk().volume().mode() == Volume::RO ? MS_RDONLY : 0);

    Try<Nothing> mount = fs::mount(source, target, None(), mountFlags, nullptr);
    if (mount.isError()) {
      return Failure(
          "Failed to mount persistent volume from '" +
          source + "' to '" + target + "': " + mount.error());
    }
  }

  // Store the new resources;
  info->resources = resourceRequests;

  return collect(futures)
    .then(defer(self(), [this, containerId](const vector<gid_t>& gids)
      -> Future<Nothing> {
      if (!infos.contains(containerId)) {
        return Failure("Unknown container");
      }

      infos[containerId]->gids = gids;

      return Nothing();
    }));
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
