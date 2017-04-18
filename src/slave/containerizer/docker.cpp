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
#include <map>
#include <set>
#include <string>
#include <vector>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/containerizer.hpp>

#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/adaptor.hpp>
#include <stout/fs.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/jsonify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/uuid.hpp>

#include <stout/os/killtree.hpp>

#include "common/status_utils.hpp"

#include "hook/manager.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#include "linux/fs.hpp"
#include "linux/systemd.hpp"
#endif // __linux__

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/constants.hpp"

#include "usage/usage.hpp"

using namespace process;

using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

using mesos::slave::ContainerLogger;
using mesos::slave::ContainerIO;
using mesos::slave::ContainerTermination;

using mesos::internal::slave::state::SlaveState;
using mesos::internal::slave::state::FrameworkState;
using mesos::internal::slave::state::ExecutorState;
using mesos::internal::slave::state::RunState;

namespace mesos {
namespace internal {
namespace slave {

// Declared in header, see explanation there.
const string DOCKER_NAME_PREFIX = "mesos-";


// Declared in header, see explanation there.
const string DOCKER_NAME_SEPERATOR = ".";


// Declared in header, see explanation there.
const string DOCKER_SYMLINK_DIRECTORY = path::join("docker", "links");


#ifdef __WINDOWS__
const string MESOS_DOCKER_EXECUTOR = "mesos-docker-executor.exe";
#else
const string MESOS_DOCKER_EXECUTOR = "mesos-docker-executor";
#endif // __WINDOWS__



// Parse the ContainerID from a Docker container and return None if
// the container was not launched from Mesos.
Option<ContainerID> parse(const Docker::Container& container)
{
  Option<string> name = None();
  Option<ContainerID> containerId = None();

  if (strings::startsWith(container.name, DOCKER_NAME_PREFIX)) {
    name = strings::remove(
        container.name, DOCKER_NAME_PREFIX, strings::PREFIX);
  } else if (strings::startsWith(container.name, "/" + DOCKER_NAME_PREFIX)) {
    name = strings::remove(
        container.name, "/" + DOCKER_NAME_PREFIX, strings::PREFIX);
  }

  if (name.isSome()) {
    // For Mesos version < 0.23.0, the docker container name format
    // was DOCKER_NAME_PREFIX + containerId, and starting with 0.23.0
    // it is changed to DOCKER_NAME_PREFIX + slaveId +
    // DOCKER_NAME_SEPERATOR + containerId.
    // To be backward compatible during upgrade, we still have to
    // support the previous format.
    // TODO(tnachen): Remove this check after deprecation cycle.
    if (!strings::contains(name.get(), DOCKER_NAME_SEPERATOR)) {
      ContainerID id;
      id.set_value(name.get());
      containerId = id;
    } else {
      vector<string> parts = strings::split(name.get(), DOCKER_NAME_SEPERATOR);
      if (parts.size() == 2 || parts.size() == 3) {
        ContainerID id;
        id.set_value(parts[1]);
        containerId = id;
      }
    }

    // Check if id is a valid UUID.
    if (containerId.isSome()) {
      Try<UUID> uuid = UUID::fromString(containerId.get().value());
      if (uuid.isError()) {
        return None();
      }
    }
  }

  return containerId;
}


Try<DockerContainerizer*> DockerContainerizer::create(
    const Flags& flags,
    Fetcher* fetcher,
    const Option<NvidiaComponents>& nvidia)
{
  // Create and initialize the container logger module.
  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  if (logger.isError()) {
    return Error("Failed to create container logger: " + logger.error());
  }

  Try<Owned<Docker>> create = Docker::create(
      flags.docker,
      flags.docker_socket,
      true,
      flags.docker_config);

  if (create.isError()) {
    return Error("Failed to create docker: " + create.error());
  }

  Shared<Docker> docker = create->share();

  if (flags.docker_mesos_image.isSome()) {
    Try<Nothing> validateResult = docker->validateVersion(Version(1, 5, 0));
    if (validateResult.isError()) {
      string message = "Docker with mesos images requires docker 1.5+";
      message += validateResult.error();
      return Error(message);
    }
  }

  // TODO(tnachen): We should also mark the work directory as shared
  // mount here, more details please refer to MESOS-3483.

  return new DockerContainerizer(
      flags,
      fetcher,
      Owned<ContainerLogger>(logger.get()),
      docker,
      nvidia);
}


DockerContainerizer::DockerContainerizer(
    const Owned<DockerContainerizerProcess>& _process)
  : process(_process)
{
  spawn(process.get());
}


DockerContainerizer::DockerContainerizer(
    const Flags& flags,
    Fetcher* fetcher,
    const Owned<ContainerLogger>& logger,
    Shared<Docker> docker,
    const Option<NvidiaComponents>& nvidia)
  : process(new DockerContainerizerProcess(
      flags,
      fetcher,
      logger,
      docker,
      nvidia))
{
  spawn(process.get());
}


DockerContainerizer::~DockerContainerizer()
{
  terminate(process.get());
  process::wait(process.get());
}


// Constructs the flags for the `mesos-docker-executor`.
// Custom docker executors will also be invoked with these flags.
//
// NOTE: `taskEnvironment` is currently used to propagate environment variables
// from a hook: `slavePreLaunchDockerEnvironmentDecorator`.
::mesos::internal::docker::Flags dockerFlags(
  const Flags& flags,
  const string& name,
  const string& directory,
  const Option<map<string, string>>& taskEnvironment)
{
  ::mesos::internal::docker::Flags dockerFlags;
  dockerFlags.container = name;
  dockerFlags.docker = flags.docker;
  dockerFlags.sandbox_directory = directory;
  dockerFlags.mapped_directory = flags.sandbox_directory;
  dockerFlags.docker_socket = flags.docker_socket;
  dockerFlags.launcher_dir = flags.launcher_dir;

  if (taskEnvironment.isSome()) {
    dockerFlags.task_environment = string(jsonify(taskEnvironment.get()));
  }

#ifdef __linux__
  dockerFlags.cgroups_enable_cfs = flags.cgroups_enable_cfs,
#endif

  // TODO(alexr): Remove this after the deprecation cycle (started in 1.0).
  dockerFlags.stop_timeout = flags.docker_stop_timeout;

  return dockerFlags;
}


Try<DockerContainerizerProcess::Container*>
DockerContainerizerProcess::Container::create(
    const ContainerID& id,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const map<string, string>& environment,
    bool checkpoint,
    const Flags& flags)
{
  // Before we do anything else we first make sure the stdout/stderr
  // files exist and have the right file ownership.
  Try<Nothing> touch = os::touch(path::join(directory, "stdout"));

  if (touch.isError()) {
    return Error("Failed to touch 'stdout': " + touch.error());
  }

  touch = os::touch(path::join(directory, "stderr"));

  if (touch.isError()) {
    return Error("Failed to touch 'stderr': " + touch.error());
  }

  // NOTE: `os::chown` has no meaningful interpretation on Windows. This is
  // safe to `#ifdef` out because we don't compile the user flag on Windows, so
  // this should always be `None`.
#ifndef __WINDOWS__
  if (user.isSome()) {
    Try<Nothing> chown = os::chown(user.get(), directory);

    if (chown.isError()) {
      return Error("Failed to chown: " + chown.error());
    }
  }
#endif // __WINDOWS__

  string dockerSymlinkPath = path::join(
      paths::getSlavePath(flags.work_dir, slaveId),
      DOCKER_SYMLINK_DIRECTORY);

  Try<Nothing> mkdir = os::mkdir(dockerSymlinkPath);
  if (mkdir.isError()) {
    return Error("Unable to create symlink folder for docker " +
                 dockerSymlinkPath + ": " + mkdir.error());
  }

  bool symlinked = false;
  string containerWorkdir = directory;
  // We need to symlink the sandbox directory if the directory
  // path has a colon, as Docker CLI uses the colon as a separator.
  if (strings::contains(directory, ":")) {
    containerWorkdir = path::join(dockerSymlinkPath, id.value());

    Try<Nothing> symlink = ::fs::symlink(directory, containerWorkdir);

    if (symlink.isError()) {
      return Error("Failed to symlink directory '" + directory +
                   "' to '" + containerWorkdir + "': " + symlink.error());
    }

    symlinked = true;
  }

  Option<ContainerInfo> containerInfo = None();
  Option<CommandInfo> commandInfo = None();
  bool launchesExecutorContainer = false;
  if (taskInfo.isSome() && flags.docker_mesos_image.isSome()) {
    // Override the container and command to launch an executor
    // in a docker container.
    ContainerInfo newContainerInfo;

    // Mounting in the docker socket so the executor can communicate to
    // the host docker daemon. We are assuming the current instance is
    // launching docker containers to the host daemon as well.
    Volume* dockerSockVolume = newContainerInfo.add_volumes();
    dockerSockVolume->set_host_path(flags.docker_socket);
    dockerSockVolume->set_container_path(flags.docker_socket);
    dockerSockVolume->set_mode(Volume::RO);

    // Mounting in sandbox so the logs from the executor can be
    // persisted over container failures.
    Volume* sandboxVolume = newContainerInfo.add_volumes();
    sandboxVolume->set_host_path(containerWorkdir);
    sandboxVolume->set_container_path(containerWorkdir);
    sandboxVolume->set_mode(Volume::RW);

    ContainerInfo::DockerInfo dockerInfo;
    dockerInfo.set_image(flags.docker_mesos_image.get());

    // `--pid=host` is required for `mesos-docker-executor` to find
    // the pid of the task in `/proc` when running
    // `mesos-docker-executor` in a separate docker container.
    Parameter* pidParameter = dockerInfo.add_parameters();
    pidParameter->set_key("pid");
    pidParameter->set_value("host");

    // `--cap-add=SYS_ADMIN` and `--cap-add=SYS_PTRACE` are required
    // for `mesos-docker-executor` to enter the namespaces of the task
    // during health checking when running `mesos-docker-executor` in a
    // separate docker container.
    Parameter* capAddParameter = dockerInfo.add_parameters();
    capAddParameter->set_key("cap-add");
    capAddParameter->set_value("SYS_ADMIN");
    capAddParameter = dockerInfo.add_parameters();
    capAddParameter->set_key("cap-add");
    capAddParameter->set_value("SYS_PTRACE");

    newContainerInfo.mutable_docker()->CopyFrom(dockerInfo);

    // NOTE: We do not set the optional `taskEnvironment` here as
    // this field is currently used to propagate environment variables
    // from a hook. This hook is called after `Container::create`.
    ::mesos::internal::docker::Flags dockerExecutorFlags = dockerFlags(
      flags,
      Container::name(slaveId, stringify(id)),
      containerWorkdir,
      None());

    // Override the command with the docker command executor.
    CommandInfo newCommandInfo;
    newCommandInfo.set_shell(false);

    newCommandInfo.set_value(
        path::join(flags.launcher_dir, MESOS_DOCKER_EXECUTOR));

    // Stringify the flags as arguments.
    // This minimizes the need for escaping flag values.
    foreachvalue (const flags::Flag& flag, dockerExecutorFlags) {
      Option<string> value = flag.stringify(dockerExecutorFlags);
      if (value.isSome()) {
        newCommandInfo.add_arguments(
            "--" + flag.effective_name().value + "=" + value.get());
      }
    }

    if (taskInfo->has_command()) {
      newCommandInfo.mutable_uris()->CopyFrom(taskInfo->command().uris());
    }

    containerInfo = newContainerInfo;
    commandInfo = newCommandInfo;
    launchesExecutorContainer = true;
  }

  return new Container(
      id,
      taskInfo,
      executorInfo,
      containerWorkdir,
      user,
      slaveId,
      checkpoint,
      symlinked,
      flags,
      commandInfo,
      containerInfo,
      environment,
      launchesExecutorContainer);
}


Future<Nothing> DockerContainerizerProcess::fetch(
    const ContainerID& containerId,
    const SlaveID& slaveId)
{
  CHECK(containers_.contains(containerId));
  Container* container = containers_.at(containerId);

  return fetcher->fetch(
      containerId,
      container->command,
      container->directory,
      None(),
      slaveId,
      flags);
}


Future<Nothing> DockerContainerizerProcess::pull(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container is already destroyed");
  }

  Container* container = containers_.at(containerId);
  container->state = Container::PULLING;

  string image = container->image();

  Future<Docker::Image> future = docker->pull(
    container->directory,
    image,
    container->forcePullImage());

  containers_.at(containerId)->pull = future;

  return future.then(defer(self(), [=]() {
    VLOG(1) << "Docker pull " << image << " completed";
    return Nothing();
  }));
}


Try<Nothing> DockerContainerizerProcess::updatePersistentVolumes(
    const ContainerID& containerId,
    const string& directory,
    const Resources& current,
    const Resources& updated)
{
  // Docker Containerizer currently is only expected to run on Linux.
#ifdef __linux__
  // Unmount all persistent volumes that are no longer present.
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

    if (updated.contains(resource)) {
      continue;
    }

    const string target = path::join(
        directory, resource.disk().volume().container_path());

    Try<Nothing> unmount = fs::unmount(target);
    if (unmount.isError()) {
      return Error("Failed to unmount persistent volume at '" + target +
                   "': " + unmount.error());
    }

    // TODO(tnachen): Remove mount point after unmounting. This requires
    // making sure the work directory is marked as a shared mount. For
    // more details please refer to MESOS-3483.
  }

  // Get user and group info for this task based on the sandbox directory.
  struct stat s;
  if (::stat(directory.c_str(), &s) < 0) {
    return Error("Failed to get ownership for '" + directory + "': " +
                 os::strerror(errno));
  }

  const uid_t uid = s.st_uid;
  const gid_t gid = s.st_gid;

  // Mount all new persistent volumes added.
  foreach (const Resource& resource, updated.persistentVolumes()) {
    // This is enforced by the master.
    CHECK(resource.disk().has_volume());

    if (current.contains(resource)) {
      continue;
    }

    const string source =
      paths::getPersistentVolumePath(flags.work_dir, resource);

    // Ignore absolute and nested paths.
    const string& containerPath = resource.disk().volume().container_path();
    if (strings::contains(containerPath, "/")) {
      LOG(WARNING) << "Skipping updating mount for persistent volume "
                   << resource << " of container " << containerId
                   << " because the container path '" << containerPath
                   << "' contains slash";
      continue;
    }

    bool isVolumeInUse = false;

    foreachvalue (const Container* container, containers_) {
      if (container->resources.contains(resource)) {
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
        return Error(
            "Failed to change the ownership of the persistent volume at '" +
            source + "' with uid " + stringify(uid) +
            " and gid " + stringify(gid) + ": " + chown.error());
      }
    }

    // TODO(tnachen): We should check if the target already exists
    // when we support updating persistent mounts.
    const string target = path::join(directory, containerPath);

    Try<Nothing> mkdir = os::mkdir(target);
    if (mkdir.isError()) {
      return Error("Failed to create persistent mount point at '" + target
                     + "': " + mkdir.error());
    }

    LOG(INFO) << "Mounting '" << source << "' to '" << target
              << "' for persistent volume " << resource
              << " of container " << containerId;

    // Bind mount the persistent volume to the container.
    Try<Nothing> mount = fs::mount(source, target, None(), MS_BIND, nullptr);
    if (mount.isError()) {
      return Error(
          "Failed to mount persistent volume from '" +
          source + "' to '" + target + "': " + mount.error());
    }

    // If the mount needs to be read-only, do a remount.
    if (resource.disk().volume().mode() == Volume::RO) {
      mount = fs::mount(
          None(), target, None(), MS_BIND | MS_RDONLY | MS_REMOUNT, nullptr);

      if (mount.isError()) {
        return Error(
            "Failed to remount persistent volume as read-only from '" +
            source + "' to '" + target + "': " + mount.error());
      }
    }
  }
#else
  if (!current.persistentVolumes().empty() ||
      !updated.persistentVolumes().empty()) {
    return Error("Persistent volumes are only supported on linux");
  }
#endif // __linux__

  return Nothing();
}


Future<Nothing> DockerContainerizerProcess::mountPersistentVolumes(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container is already destroyed");
  }

  Container* container = containers_.at(containerId);
  container->state = Container::MOUNTING;

  if (container->task.isNone() &&
      !container->resources.persistentVolumes().empty()) {
    LOG(ERROR) << "Persistent volumes found with container '" << containerId
               << "' but are not supported with custom executors";
    return Nothing();
  }

  Try<Nothing> updateVolumes = updatePersistentVolumes(
      containerId,
      container->directory,
      Resources(),
      container->resources);

  if (updateVolumes.isError()) {
    return Failure(updateVolumes.error());
  }

  return Nothing();
}


/**
 *  Unmount persistent volumes that is mounted for a container.
 */
Try<Nothing> DockerContainerizerProcess::unmountPersistentVolumes(
    const ContainerID& containerId)
{
  // We assume volumes are only supported on Linux, and also
  // the target path contains the containerId.
#ifdef __linux__
  Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
  if (table.isError()) {
    return Error("Failed to get mount table: " + table.error());
  }

  vector<string> unmountErrors;

  foreach (const fs::MountInfoTable::Entry& entry,
           adaptor::reverse(table.get().entries)) {
    // TODO(tnachen): We assume there is only one docker container
    // running per container Id and no other mounts will have the
    // container Id name. We might need to revisit if this is no
    // longer true.
    //
    // TODO(jieyu): Currently, we don't enforce that slave's work_dir
    // is a slave+shared mount (similar to what we did in the Linux
    // filesystem isolator). Therefore, it's likely that the
    // persistent volume mounts are propagate to other mount points in
    // the system (MESOS-4832). That's the reason we need the
    // 'startsWith' check below. Consider making sure slave's work_dir
    // is a slave_shared mount. In that way, we can lift that check.
    //
    // TODO(jieyu): Consider checking if 'entry.root' is under volume
    // root directory or not as well.
    if (strings::startsWith(entry.target, flags.work_dir) &&
        strings::contains(entry.target, containerId.value())) {
      LOG(INFO) << "Unmounting volume for container '" << containerId << "'";

      // TODO(jieyu): Use MNT_DETACH here to workaround an issue of
      // incorrect handling of container destroy failures. Currently,
      // if unmount fails there, the containerizer will still treat
      // the container as terminated, and the agent will schedule the
      // cleanup of the container's sandbox. Since the mount hasn't
      // been removed in the sandbox, that'll result in data in the
      // persistent volume being incorrectly deleted. Use MNT_DETACH
      // here so that the mount point in the sandbox will be removed
      // immediately.  See MESOS-7366 for more details.
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
    return Error(strings::join(", ", unmountErrors));
  }
#endif // __linux__
  return Nothing();
}


#ifdef __linux__
Future<Nothing> DockerContainerizerProcess::allocateNvidiaGpus(
    const ContainerID& containerId,
    const size_t count)
{
  if (!nvidia.isSome()) {
    return Failure("Attempted to allocate GPUs"
                   " without Nvidia libraries available");
  }

  if (!containers_.contains(containerId)) {
    return Failure("Container is already destroyed");
  }

  return nvidia->allocator.allocate(count)
    .then(defer(
        self(),
        &Self::_allocateNvidiaGpus,
        containerId,
        lambda::_1));
}


Future<Nothing> DockerContainerizerProcess::_allocateNvidiaGpus(
    const ContainerID& containerId,
    const set<Gpu>& allocated)
{
  if (!containers_.contains(containerId)) {
    return nvidia->allocator.deallocate(allocated);
  }

  foreach (const Gpu& gpu, allocated) {
    containers_.at(containerId)->gpus.insert(gpu);
  }

  return Nothing();
}


Future<Nothing> DockerContainerizerProcess::deallocateNvidiaGpus(
    const ContainerID& containerId)
{
  if (!nvidia.isSome()) {
    return Failure("Attempted to deallocate GPUs"
                   " without Nvidia libraries available");
  }

  return nvidia->allocator.deallocate(containers_.at(containerId)->gpus)
    .then(defer(
        self(),
        &Self::_deallocateNvidiaGpus,
        containerId,
        containers_.at(containerId)->gpus));
}


Future<Nothing> DockerContainerizerProcess::_deallocateNvidiaGpus(
    const ContainerID& containerId,
    const set<Gpu>& deallocated)
{
  if (containers_.contains(containerId)) {
    foreach (const Gpu& gpu, deallocated) {
      containers_.at(containerId)->gpus.erase(gpu);
    }
  }

  return Nothing();
}
#endif // __linux__


Try<Nothing> DockerContainerizerProcess::checkpoint(
    const ContainerID& containerId,
    pid_t pid)
{
  CHECK(containers_.contains(containerId));

  Container* container = containers_.at(containerId);

  container->executorPid = pid;

  if (container->checkpoint) {
    const string& path =
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          container->slaveId,
          container->executor.framework_id(),
          container->executor.executor_id(),
          containerId);

    LOG(INFO) << "Checkpointing pid " << pid << " to '" << path << "'";

    return slave::state::checkpoint(path, stringify(pid));
  }

  return Nothing();
}


Future<Nothing> DockerContainerizer::recover(
    const Option<SlaveState>& state)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::recover,
      state);
}


Future<bool> DockerContainerizer::launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const map<string, string>& environment,
    bool checkpoint)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::launch,
      containerId,
      taskInfo,
      executorInfo,
      directory,
      user,
      slaveId,
      environment,
      checkpoint);
}


Future<Nothing> DockerContainerizer::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::update,
      containerId,
      resources,
      false);
}


Future<ResourceStatistics> DockerContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::usage,
      containerId);
}


Future<ContainerStatus> DockerContainerizer::status(
    const ContainerID& containerId)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::status,
      containerId);
}


Future<Option<ContainerTermination>> DockerContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::wait,
      containerId);
}


Future<bool> DockerContainerizer::destroy(const ContainerID& containerId)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::destroy,
      containerId, true);
}


Future<hashset<ContainerID>> DockerContainerizer::containers()
{
  return dispatch(process.get(), &DockerContainerizerProcess::containers);
}


Future<Nothing> DockerContainerizerProcess::recover(
    const Option<SlaveState>& state)
{
  LOG(INFO) << "Recovering Docker containers";

  // Get the list of all Docker containers (running and exited) in
  // order to remove any orphans and reconcile checkpointed executors.
  return docker->ps(true, DOCKER_NAME_PREFIX)
    .then(defer(self(), &Self::_recover, state, lambda::_1));
}


Future<Nothing> DockerContainerizerProcess::_recover(
    const Option<SlaveState>& state,
    const list<Docker::Container>& _containers)
{
  if (state.isSome()) {
    // Although the slave checkpoints executor pids, before 0.23
    // docker containers without custom executors didn't record the
    // container type in the executor info, therefore the Docker
    // containerizer doesn't know if it should recover that container
    // as it could be launched from another containerizer. The
    // workaround is to reconcile running Docker containers and see
    // if we can find an known container that matches the
    // checkpointed container id.
    // TODO(tnachen): Remove this explicit reconciliation 0.24.
    hashset<ContainerID> existingContainers;

    // Tracks all the task containers that launched an executor in
    // a docker container.
    hashset<ContainerID> executorContainers;

    foreach (const Docker::Container& container, _containers) {
      Option<ContainerID> id = parse(container);
      if (id.isSome()) {
        existingContainers.insert(id.get());
        if (strings::contains(container.name, ".executor")) {
          executorContainers.insert(id.get());
        }
      }
    }

    // Collection of pids that we've started reaping in order to
    // detect very unlikely duplicate scenario (see below).
    hashmap<ContainerID, pid_t> pids;

    foreachvalue (const FrameworkState& framework, state->frameworks) {
      foreachvalue (const ExecutorState& executor, framework.executors) {
        if (executor.info.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its info could not be recovered";
          continue;
        }

        if (executor.latest.isNone()) {
          LOG(WARNING) << "Skipping recovery of executor '" << executor.id
                       << "' of framework " << framework.id
                       << " because its latest run could not be recovered";
          continue;
        }

        // We are only interested in the latest run of the executor!
        const ContainerID& containerId = executor.latest.get();
        Option<RunState> run = executor.runs.get(containerId);
        CHECK_SOME(run);
        CHECK_SOME(run.get().id);
        CHECK_EQ(containerId, run.get().id.get());

        // We need the pid so the reaper can monitor the executor so
        // skip this executor if it's not present. This is not an
        // error because the slave will try to wait on the container
        // which will return a failed 'ContainerTermination' and
        // everything will get cleaned up.
        if (!run.get().forkedPid.isSome()) {
          continue;
        }

        if (run.get().completed) {
          VLOG(1) << "Skipping recovery of executor '" << executor.id
                  << "' of framework " << framework.id
                  << " because its latest run "
                  << containerId << " is completed";
          continue;
        }

        const ExecutorInfo executorInfo = executor.info.get();
        if (executorInfo.has_container() &&
            executorInfo.container().type() != ContainerInfo::DOCKER) {
          LOG(INFO) << "Skipping recovery of executor '" << executor.id
                    << "' of framework " << framework.id
                    << " because it was not launched from docker "
                    << "containerizer";
          continue;
        }

        if (!executorInfo.has_container() &&
            !existingContainers.contains(containerId)) {
          LOG(INFO) << "Skipping recovery of executor '" << executor.id
                    << "' of framework " << framework.id
                    << " because its executor is not marked as docker "
                    << "and the docker container doesn't exist";
          continue;
        }

        LOG(INFO) << "Recovering container '" << containerId
                  << "' for executor '" << executor.id
                  << "' of framework " << framework.id;

        // Create and store a container.
        Container* container = new Container(containerId);
        containers_[containerId] = container;
        container->slaveId = state->id;
        container->state = Container::RUNNING;
        container->launchesExecutorContainer =
          executorContainers.contains(containerId);

        pid_t pid = run.get().forkedPid.get();

        container->status.set(process::reap(pid));

        container->status.future().get()
          .onAny(defer(self(), &Self::reaped, containerId));

        if (pids.containsValue(pid)) {
          // This should (almost) never occur. There is the
          // possibility that a new executor is launched with the same
          // pid as one that just exited (highly unlikely) and the
          // slave dies after the new executor is launched but before
          // it hears about the termination of the earlier executor
          // (also unlikely).
          return Failure(
              "Detected duplicate pid " + stringify(pid) +
              " for container " + stringify(containerId));
        }

        pids.put(containerId, pid);

        const string sandboxDirectory = paths::getExecutorRunPath(
            flags.work_dir,
            state->id,
            framework.id,
            executor.id,
            containerId);

        container->directory = sandboxDirectory;
      }
    }
  }

  if (flags.docker_kill_orphans) {
    return __recover(_containers);
  }

  return Nothing();
}


Future<Nothing> DockerContainerizerProcess::__recover(
    const list<Docker::Container>& _containers)
{
  list<ContainerID> containerIds;
  list<Future<Nothing>> futures;
  foreach (const Docker::Container& container, _containers) {
    VLOG(1) << "Checking if Docker container named '"
            << container.name << "' was started by Mesos";

    Option<ContainerID> id = parse(container);

    // Ignore containers that Mesos didn't start.
    if (id.isNone()) {
      continue;
    }

    VLOG(1) << "Checking if Mesos container with ID '"
            << stringify(id.get()) << "' has been orphaned";

    // Check if we're watching an executor for this container ID and
    // if not, rm -f the Docker container.
    if (!containers_.contains(id.get())) {
      // TODO(alexr): After the deprecation cycle (started in 1.0), update
      // this to omit the timeout. Graceful shutdown of the container is not
      // a containerizer responsibility; it is the responsibility of the agent
      // in co-operation with the executor. Once `destroy()` is called, the
      // container should be destroyed forcefully.
      futures.push_back(
          docker->stop(
              container.id,
              flags.docker_stop_timeout,
              true));
      containerIds.push_back(id.get());
    }
  }

  return collect(futures)
    .then(defer(self(), [=]() -> Future<Nothing> {
      foreach (const ContainerID& containerId, containerIds) {
        Try<Nothing> unmount = unmountPersistentVolumes(containerId);
        if (unmount.isError()) {
          return Failure("Unable to unmount volumes for Docker container '" +
                         containerId.value() + "': " + unmount.error());
        }
      }

      return Nothing();
    }));
}


Future<bool> DockerContainerizerProcess::launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const map<string, string>& environment,
    bool checkpoint)
{
  CHECK(!containerId.has_parent());

  if (containers_.contains(containerId)) {
    return Failure("Container already started");
  }

  Option<ContainerInfo> containerInfo;

  if (taskInfo.isSome() && taskInfo.get().has_container()) {
    containerInfo = taskInfo.get().container();
  } else if (executorInfo.has_container()) {
    containerInfo = executorInfo.container();
  }

  if (containerInfo.isNone()) {
    LOG(INFO) << "No container info found, skipping launch";
    return false;
  }

  if (containerInfo.get().type() != ContainerInfo::DOCKER) {
    LOG(INFO) << "Skipping non-docker container";
    return false;
  }

  Try<Container*> container = Container::create(
      containerId,
      taskInfo,
      executorInfo,
      directory,
      user,
      slaveId,
      environment,
      checkpoint,
      flags);

  if (container.isError()) {
    return Failure("Failed to create container: " + container.error());
  }

  containers_[containerId] = container.get();

  if (taskInfo.isSome()) {
    LOG(INFO) << "Starting container '" << containerId
              << "' for task '" << taskInfo.get().task_id()
              << "' (and executor '" << executorInfo.executor_id()
              << "') of framework " << executorInfo.framework_id();
  } else {
    LOG(INFO) << "Starting container '" << containerId
              << "' for executor '" << executorInfo.executor_id()
              << "' and framework " << executorInfo.framework_id();
  }

  Future<Nothing> f = Nothing();

  if (HookManager::hooksAvailable()) {
    f = HookManager::slavePreLaunchDockerTaskExecutorDecorator(
        taskInfo,
        executorInfo,
        container.get()->name(),
        container.get()->directory,
        flags.sandbox_directory,
        container.get()->environment)
      .then(defer(self(), [this, taskInfo, containerId](
          const DockerTaskExecutorPrepareInfo& decoratorInfo)
          -> Future<Nothing> {
        if (!containers_.contains(containerId)) {
          return Failure("Container is already destroyed");
        }

        Container* container = containers_.at(containerId);

        if (decoratorInfo.has_executorenvironment()) {
          foreach (
              const Environment::Variable& variable,
              decoratorInfo.executorenvironment().variables()) {
            // TODO(tillt): Tell the user about overrides possibly
            // happening here while making sure we state the source
            // hook causing this conflict.
            container->environment[variable.name()] =
              variable.value();
          }
        }

        if (!decoratorInfo.has_taskenvironment()) {
          return Nothing();
        }

        map<string, string> taskEnvironment;

        foreach (
            const Environment::Variable& variable,
            decoratorInfo.taskenvironment().variables()) {
          taskEnvironment[variable.name()] = variable.value();
        }

        if (taskInfo.isSome()) {
          container->taskEnvironment = taskEnvironment;

          // For dockerized command executors, the flags have already
          // been serialized into the command, albeit without these
          // environment variables. Append the last flag to the
          // overridden command.
          if (container->launchesExecutorContainer) {
            container->command.add_arguments(
                "--task_environment=" +
                string(jsonify(taskEnvironment)));
          }
        } else {
          // For custom executors, the environment variables from a
          // hook are passed directly into the executor.  It is up to
          // the custom executor whether individual tasks should
          // inherit these variables.
          foreachpair (
              const string& key,
              const string& value,
              taskEnvironment) {
            container->environment[key] = value;
          }
        }

        return Nothing();
      }));
  }

  return f.then(defer(
      self(),
      &Self::_launch,
      containerId,
      taskInfo,
      executorInfo,
      directory,
      slaveId));
}


Future<bool> DockerContainerizerProcess::_launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container is already destroyed");
  }

  Container* container = containers_.at(containerId);

  if (taskInfo.isSome() && flags.docker_mesos_image.isNone()) {
    // Launching task by forking a subprocess to run docker executor.
    // TODO(steveniemitz): We should call 'update' to set CPU/CFS/mem
    // quotas after 'launchExecutorProcess'. However, there is a race
    // where 'update' can be called before mesos-docker-executor
    // creates the Docker container for the task. See more details in
    // the comments of r33174.
    return container->launch = fetch(containerId, slaveId)
      .then(defer(self(), [=]() {
        return pull(containerId);
      }))
      .then(defer(self(), [=]() {
        if (HookManager::hooksAvailable()) {
          HookManager::slavePostFetchHook(containerId, directory);
        }

        return mountPersistentVolumes(containerId);
      }))
      .then(defer(self(), [=]() {
        return launchExecutorProcess(containerId);
      }))
      .then(defer(self(), [=](pid_t pid) {
        return reapExecutor(containerId, pid);
      }));
  }

  string containerName = container->name();

  if (container->executorName().isSome()) {
    // Launch the container with the executor name as we expect the
    // executor will launch the docker container.
    containerName = container->executorName().get();
  }

  // Launching task or executor by launching a separate docker
  // container to run the executor.
  // We need to do so for launching a task because as the slave is
  // running in a container (via docker_mesos_image flag) we want the
  // executor to keep running when the slave container dies.
  return container->launch = fetch(containerId, slaveId)
    .then(defer(self(), [=]() {
      return pull(containerId);
    }))
    .then(defer(self(), [=]() {
      if (HookManager::hooksAvailable()) {
        HookManager::slavePostFetchHook(containerId, directory);
      }

      return mountPersistentVolumes(containerId);
    }))
    .then(defer(self(), [=]() {
      return launchExecutorContainer(containerId, containerName);
    }))
    .then(defer(self(), [=](const Docker::Container& dockerContainer) {
      // Call update to set CPU/CFS/mem quotas at launch.
      // TODO(steveniemitz): Once the minimum docker version supported
      // is >= 1.7 this can be changed to pass --cpu-period and
      // --cpu-quota to the 'docker run' call in
      // launchExecutorContainer.
      return update(containerId, executorInfo.resources(), true)
        .then([=]() {
          return Future<Docker::Container>(dockerContainer);
        });
    }))
    .then(defer(self(), [=](const Docker::Container& dockerContainer) {
      return checkpointExecutor(containerId, dockerContainer);
    }))
    .then(defer(self(), [=](pid_t pid) {
      return reapExecutor(containerId, pid);
    }));
}


Future<Docker::Container> DockerContainerizerProcess::launchExecutorContainer(
    const ContainerID& containerId,
    const string& containerName)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container is already destroyed");
  }

  Container* container = containers_.at(containerId);
  container->state = Container::RUNNING;

  return logger->prepare(
      container->executor,
      container->directory,
      container->user)
    .then(defer(
        self(),
        [=](const ContainerIO& containerIO)
          -> Future<Docker::Container> {
    Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
        container->container,
        container->command,
        containerName,
        container->directory,
        flags.sandbox_directory,
        container->resources,
#ifdef __linux__
        flags.cgroups_enable_cfs,
#else
        false,
#endif
        container->environment,
        None() // No extra devices.
    );

    if (runOptions.isError()) {
      return Failure(runOptions.error());
    }

    // Start the executor in a Docker container.
    // This executor could either be a custom executor specified by an
    // ExecutorInfo, or the docker executor.
    Future<Option<int>> run = docker->run(
        runOptions.get(),
        containerIO.out,
        containerIO.err);

    // It's possible that 'run' terminates before we're able to
    // obtain an 'inspect' result. It's also possible that 'run'
    // fails in such a manner that we will never see the container
    // via 'inspect'. In these cases we discard the 'inspect' and
    // propagate a failure back.

    auto promise = std::make_shared<Promise<Docker::Container>>();

    Future<Docker::Container> inspect =
      docker->inspect(containerName, slave::DOCKER_INSPECT_DELAY);

    inspect
      .onAny([=](Future<Docker::Container> container) {
        promise->associate(container);
      });

    run.onAny([=]() mutable {
      if (!run.isReady()) {
        inspect.discard();
        promise->fail(run.isFailed() ? run.failure() : "discarded");
      } else if (run->isNone()) {
        inspect.discard();
        promise->fail("Failed to obtain exit status of container");
      } else {
        if (!WSUCCEEDED(run->get())) {
          inspect.discard();
          promise->fail("Container " + WSTRINGIFY(run->get()));
        }

        // TODO(bmahler): Handle the case where the 'run' exits
        // cleanly but no 'inspect' result is available.
      }
    });

    return promise->future();
  }));
}


Future<pid_t> DockerContainerizerProcess::launchExecutorProcess(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container is already destroyed");
  }

  Container* container = containers_.at(containerId);
  container->state = Container::RUNNING;

  // Prepare environment variables for the executor.
  map<string, string> environment = container->environment;

  // Include any environment variables from ExecutorInfo.
  foreach (const Environment::Variable& variable,
           container->executor.command().environment().variables()) {
    environment[variable.name()] = variable.value();
  }

  // Pass GLOG flag to the executor.
  const Option<string> glog = os::getenv("GLOG_v");
  if (glog.isSome()) {
    environment["GLOG_v"] = glog.get();
  }

  if (environment.count("PATH") == 0) {
    environment["PATH"] = os::host_default_path();
  }

#ifdef __WINDOWS__
  // TODO(dpravat): (MESOS-6816) We should allow system environment variables to
  // be overwritten if they are specified by the framework.  This might cause
  // applications to not work, but upon overriding system defaults, it becomes
  // the overidder's problem.
  Option<map<std::wstring, std::wstring>> systemEnvironment =
    process::internal::getSystemEnvironment();
  foreachpair(const std::wstring& key,
              const std::wstring& value,
              systemEnvironment.get()) {
    environment[stringify(key)] = stringify(value);
  }
#endif // __WINDOWS__

  vector<string> argv;
  argv.push_back(MESOS_DOCKER_EXECUTOR);

  Future<Nothing> allocateGpus = Nothing();

#ifdef __linux__
  Option<double> gpus = Resources(container->resources).gpus();

  if (gpus.isSome() && gpus.get() > 0) {
    // Make sure that the `gpus` resource is not fractional.
    // We rely on scalar resources only have 3 digits of precision.
    if (static_cast<long long>(gpus.get() * 1000.0) % 1000 != 0) {
      return Failure("The 'gpus' resource must be an unsigned integer");
    }

    allocateGpus = allocateNvidiaGpus(containerId, gpus.get());
  }
#endif // __linux__

  return allocateGpus
    .then(defer(self(), [=]() {
      return logger->prepare(
          container->executor,
          container->directory,
          container->user);
    }))
    .then(defer(
        self(),
        [=](const ContainerIO& containerIO)
          -> Future<pid_t> {
    // NOTE: The child process will be blocked until all hooks have been
    // executed.
    vector<Subprocess::ParentHook> parentHooks;

    // NOTE: Currently we don't care about the order of the hooks, as
    // both hooks are independent.

    // A hook that is executed in the parent process. It attempts to checkpoint
    // the process pid.
    //
    // NOTE:
    // - The child process is blocked by the hook infrastructure while
    //   these hooks are executed.
    // - It is safe to bind `this`, as hooks are executed immediately
    //   in a `subprocess` call.
    // - If `checkpoiont` returns an Error, the child process will be killed.
    parentHooks.emplace_back(Subprocess::ParentHook(lambda::bind(
        &DockerContainerizerProcess::checkpoint,
        this,
        containerId,
        lambda::_1)));

#ifdef __linux__
    // If we are on systemd, then extend the life of the executor. Any
    // grandchildren's lives will also be extended.
    if (systemd::enabled()) {
      parentHooks.emplace_back(Subprocess::ParentHook(
          &systemd::mesos::extendLifetime));
    }
#endif // __linux__

    // Prepare the flags to pass to the mesos docker executor process.
    ::mesos::internal::docker::Flags launchFlags = dockerFlags(
        flags,
        container->name(),
        container->directory,
        container->taskEnvironment);

    VLOG(1) << "Launching 'mesos-docker-executor' with flags '"
            << launchFlags << "'";

    // Construct the mesos-docker-executor using the "name" we gave the
    // container (to distinguish it from Docker containers not created
    // by Mesos).
    Try<Subprocess> s = subprocess(
        path::join(flags.launcher_dir, MESOS_DOCKER_EXECUTOR),
        argv,
        Subprocess::PIPE(),
        containerIO.out,
        containerIO.err,
        &launchFlags,
        environment,
        None(),
        parentHooks,
        {Subprocess::ChildHook::SETSID(),
         Subprocess::ChildHook::CHDIR(container->directory)});

    if (s.isError()) {
      return Failure("Failed to fork executor: " + s.error());
    }

    return s.get().pid();
  }));
}


Future<pid_t> DockerContainerizerProcess::checkpointExecutor(
    const ContainerID& containerId,
    const Docker::Container& dockerContainer)
{
  // After we do Docker::run we shouldn't remove a container until
  // after we set Container::status.
  CHECK(containers_.contains(containerId));

  Option<pid_t> pid = dockerContainer.pid;

  if (!pid.isSome()) {
    return Failure("Unable to get executor pid after launch");
  }

  Try<Nothing> checkpointed = checkpoint(containerId, pid.get());

  if (checkpointed.isError()) {
    return Failure(
        "Failed to checkpoint executor's pid: " + checkpointed.error());
  }

  return pid.get();
}


Future<bool> DockerContainerizerProcess::reapExecutor(
    const ContainerID& containerId,
    pid_t pid)
{
  // After we do Docker::run we shouldn't remove a container until
  // after we set 'status', which we do in this function.
  CHECK(containers_.contains(containerId));

  Container* container = containers_.at(containerId);

  // And finally watch for when the container gets reaped.
  container->status.set(process::reap(pid));

  container->status.future().get()
    .onAny(defer(self(), &Self::reaped, containerId));

  return true;
}


Future<Nothing> DockerContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& _resources,
    bool force)
{
  CHECK(!containerId.has_parent());

  if (!containers_.contains(containerId)) {
    LOG(WARNING) << "Ignoring updating unknown container " << containerId;
    return Nothing();
  }

  Container* container = containers_.at(containerId);

  if (container->state == Container::DESTROYING)  {
    LOG(INFO) << "Ignoring updating container " << containerId
              << " that is being destroyed";
    return Nothing();
  }

  if (container->resources == _resources && !force) {
    LOG(INFO) << "Ignoring updating container " << containerId
              << " because resources passed to update are identical to"
              << " existing resources";
    return Nothing();
  }

  // TODO(tnachen): Support updating persistent volumes, which requires
  // Docker mount propagation support.

  // TODO(gyliu): Support updating GPU resources.

  // Store the resources for usage().
  container->resources = _resources;

#ifdef __linux__
  if (!_resources.cpus().isSome() && !_resources.mem().isSome()) {
    LOG(WARNING) << "Ignoring update as no supported resources are present";
    return Nothing();
  }

  // Skip inspecting the docker container if we already have the pid.
  if (container->pid.isSome()) {
    return __update(containerId, _resources, container->pid.get());
  }

  return docker->inspect(containers_.at(containerId)->name())
    .then(defer(self(), &Self::_update, containerId, _resources, lambda::_1));
#else
  return Nothing();
#endif // __linux__
}


Future<Nothing> DockerContainerizerProcess::_update(
    const ContainerID& containerId,
    const Resources& _resources,
    const Docker::Container& container)
{
  if (container.pid.isNone()) {
    return Nothing();
  }

  if (!containers_.contains(containerId)) {
    LOG(INFO) << "Container has been removed after docker inspect, "
              << "skipping update";
    return Nothing();
  }

  containers_.at(containerId)->pid = container.pid.get();

  return __update(containerId, _resources, container.pid.get());
}


Future<Nothing> DockerContainerizerProcess::__update(
    const ContainerID& containerId,
    const Resources& _resources,
    pid_t pid)
{
#ifdef __linux__
  // Determine the cgroups hierarchies where the 'cpu' and
  // 'memory' subsystems are mounted (they may be the same). Note that
  // we make these static so we can reuse the result for subsequent
  // calls.
  static Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  static Result<string> memoryHierarchy = cgroups::hierarchy("memory");

  if (cpuHierarchy.isError()) {
    return Failure("Failed to determine the cgroup hierarchy "
                   "where the 'cpu' subsystem is mounted: " +
                   cpuHierarchy.error());
  }

  if (memoryHierarchy.isError()) {
    return Failure("Failed to determine the cgroup hierarchy "
                   "where the 'memory' subsystem is mounted: " +
                   memoryHierarchy.error());
  }

  // We need to find the cgroup(s) this container is currently running
  // in for both the hierarchy with the 'cpu' subsystem attached and
  // the hierarchy with the 'memory' subsystem attached so we can
  // update the proper cgroup control files.

  // Determine the cgroup for the 'cpu' subsystem (based on the
  // container's pid).
  Result<string> cpuCgroup = cgroups::cpu::cgroup(pid);

  if (cpuCgroup.isError()) {
    return Failure("Failed to determine cgroup for the 'cpu' subsystem: " +
                   cpuCgroup.error());
  } else if (cpuCgroup.isNone()) {
    LOG(WARNING) << "Container " << containerId
                 << " does not appear to be a member of a cgroup"
                 << " where the 'cpu' subsystem is mounted";
  }

  // And update the CPU shares (if applicable).
  if (cpuHierarchy.isSome() &&
      cpuCgroup.isSome() &&
      _resources.cpus().isSome()) {
    double cpuShares = _resources.cpus().get();

    uint64_t shares =
      std::max((uint64_t) (CPU_SHARES_PER_CPU * cpuShares), MIN_CPU_SHARES);

    Try<Nothing> write =
      cgroups::cpu::shares(cpuHierarchy.get(), cpuCgroup.get(), shares);

    if (write.isError()) {
      return Failure("Failed to update 'cpu.shares': " + write.error());
    }

    LOG(INFO) << "Updated 'cpu.shares' to " << shares
              << " at " << path::join(cpuHierarchy.get(), cpuCgroup.get())
              << " for container " << containerId;

    // Set cfs quota if enabled.
    if (flags.cgroups_enable_cfs) {
      write = cgroups::cpu::cfs_period_us(
          cpuHierarchy.get(),
          cpuCgroup.get(),
          CPU_CFS_PERIOD);

      if (write.isError()) {
        return Failure("Failed to update 'cpu.cfs_period_us': " +
                       write.error());
      }

      Duration quota = std::max(CPU_CFS_PERIOD * cpuShares, MIN_CPU_CFS_QUOTA);

      write = cgroups::cpu::cfs_quota_us(
          cpuHierarchy.get(),
          cpuCgroup.get(),
          quota);

      if (write.isError()) {
        return Failure("Failed to update 'cpu.cfs_quota_us': " + write.error());
      }

      LOG(INFO) << "Updated 'cpu.cfs_period_us' to " << CPU_CFS_PERIOD
                << " and 'cpu.cfs_quota_us' to " << quota
                << " (cpus " << cpuShares << ")"
                << " for container " << containerId;
    }
  }

  // Now determine the cgroup for the 'memory' subsystem.
  Result<string> memoryCgroup = cgroups::memory::cgroup(pid);

  if (memoryCgroup.isError()) {
    return Failure("Failed to determine cgroup for the 'memory' subsystem: " +
                   memoryCgroup.error());
  } else if (memoryCgroup.isNone()) {
    LOG(WARNING) << "Container " << containerId
                 << " does not appear to be a member of a cgroup"
                 << " where the 'memory' subsystem is mounted";
  }

  // And update the memory limits (if applicable).
  if (memoryHierarchy.isSome() &&
      memoryCgroup.isSome() &&
      _resources.mem().isSome()) {
    // TODO(tnachen): investigate and handle OOM with docker.
    Bytes mem = _resources.mem().get();
    Bytes limit = std::max(mem, MIN_MEMORY);

    // Always set the soft limit.
    Try<Nothing> write =
      cgroups::memory::soft_limit_in_bytes(
          memoryHierarchy.get(), memoryCgroup.get(), limit);

    if (write.isError()) {
      return Failure("Failed to set 'memory.soft_limit_in_bytes': " +
                     write.error());
    }

    LOG(INFO) << "Updated 'memory.soft_limit_in_bytes' to " << limit
              << " for container " << containerId;

    // Read the existing limit.
    Try<Bytes> currentLimit =
      cgroups::memory::limit_in_bytes(
          memoryHierarchy.get(), memoryCgroup.get());

    if (currentLimit.isError()) {
      return Failure("Failed to read 'memory.limit_in_bytes': " +
                     currentLimit.error());
    }

    // Only update if new limit is higher.
    // TODO(benh): Introduce a MemoryWatcherProcess which monitors the
    // discrepancy between usage and soft limit and introduces a
    // "manual oom" if necessary.
    if (limit > currentLimit.get()) {
      write = cgroups::memory::limit_in_bytes(
          memoryHierarchy.get(), memoryCgroup.get(), limit);

      if (write.isError()) {
        return Failure("Failed to set 'memory.limit_in_bytes': " +
                       write.error());
      }

      LOG(INFO) << "Updated 'memory.limit_in_bytes' to " << limit << " at "
                << path::join(memoryHierarchy.get(), memoryCgroup.get())
                << " for container " << containerId;
    }
  }
#endif // __linux__

  return Nothing();
}


Future<ResourceStatistics> DockerContainerizerProcess::usage(
    const ContainerID& containerId)
{
  CHECK(!containerId.has_parent());

#ifndef __linux__
  return Failure("Does not support usage() on non-linux platform");
#else
  if (!containers_.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  Container* container = containers_.at(containerId);

  if (container->state == Container::DESTROYING) {
    return Failure("Container is being removed: " + stringify(containerId));
  }

  auto collectUsage = [this, containerId](
      pid_t pid) -> Future<ResourceStatistics> {
    // First make sure container is still there.
    if (!containers_.contains(containerId)) {
      return Failure("Container has been destroyed: " + stringify(containerId));
    }

    Container* container = containers_.at(containerId);

    if (container->state == Container::DESTROYING) {
      return Failure("Container is being removed: " + stringify(containerId));
    }

    const Try<ResourceStatistics> cgroupStats = cgroupsStatistics(pid);
    if (cgroupStats.isError()) {
      return Failure("Failed to collect cgroup stats: " + cgroupStats.error());
    }

    ResourceStatistics result = cgroupStats.get();

    // Set the resource allocations.
    const Resources& resource = container->resources;
    const Option<Bytes> mem = resource.mem();
    if (mem.isSome()) {
      result.set_mem_limit_bytes(mem.get().bytes());
    }

    const Option<double> cpus = resource.cpus();
    if (cpus.isSome()) {
      result.set_cpus_limit(cpus.get());
    }

    return result;
  };

  // Skip inspecting the docker container if we already have the pid.
  if (container->pid.isSome()) {
    return collectUsage(container->pid.get());
  }

  return docker->inspect(container->name())
    .then(defer(
      self(),
      [this, containerId, collectUsage]
        (const Docker::Container& _container) -> Future<ResourceStatistics> {
        const Option<pid_t> pid = _container.pid;
        if (pid.isNone()) {
          return Failure("Container is not running");
        }

        if (!containers_.contains(containerId)) {
          return Failure(
            "Container has been destroyed:" + stringify(containerId));
        }

        Container* container = containers_.at(containerId);

        // Update the container's pid now. We ran inspect because we didn't have
        // a pid for the container.
        container->pid = pid;

        return collectUsage(pid.get());
      }));
#endif // __linux__
}


Try<ResourceStatistics> DockerContainerizerProcess::cgroupsStatistics(
    pid_t pid) const
{
#ifndef __linux__
  return Error("Does not support cgroups on non-linux platform");
#else
  const Result<string> cpuacctHierarchy = cgroups::hierarchy("cpuacct");
  const Result<string> memHierarchy = cgroups::hierarchy("memory");

  if (cpuacctHierarchy.isError()) {
    return Error(
        "Failed to determine the cgroup 'cpuacct' subsystem hierarchy: " +
        cpuacctHierarchy.error());
  }

  if (memHierarchy.isError()) {
    return Error(
        "Failed to determine the cgroup 'memory' subsystem hierarchy: " +
        memHierarchy.error());
  }

  const Result<string> cpuacctCgroup = cgroups::cpuacct::cgroup(pid);
  if (cpuacctCgroup.isError()) {
    return Error(
        "Failed to determine cgroup for the 'cpuacct' subsystem: " +
        cpuacctCgroup.error());
  } else if (cpuacctCgroup.isNone()) {
    return Error("Unable to find 'cpuacct' cgroup subsystem");
  }

  const Result<string> memCgroup = cgroups::memory::cgroup(pid);
  if (memCgroup.isError()) {
    return Error(
        "Failed to determine cgroup for the 'memory' subsystem: " +
        memCgroup.error());
  } else if (memCgroup.isNone()) {
    return Error("Unable to find 'memory' cgroup subsystem");
  }

  const Try<cgroups::cpuacct::Stats> cpuAcctStat =
    cgroups::cpuacct::stat(cpuacctHierarchy.get(), cpuacctCgroup.get());

  if (cpuAcctStat.isError()) {
    return Error("Failed to get cpu.stat: " + cpuAcctStat.error());
  }

  const Try<hashmap<string, uint64_t>> memStats =
    cgroups::stat(memHierarchy.get(), memCgroup.get(), "memory.stat");

  if (memStats.isError()) {
    return Error(
        "Error getting memory statistics from cgroups memory subsystem: " +
        memStats.error());
  }

  if (!memStats.get().contains("rss")) {
    return Error("cgroups memory stats does not contain 'rss' data");
  }

  ResourceStatistics result;
  result.set_timestamp(Clock::now().secs());
  result.set_cpus_system_time_secs(cpuAcctStat.get().system.secs());
  result.set_cpus_user_time_secs(cpuAcctStat.get().user.secs());
  result.set_mem_rss_bytes(memStats.get().at("rss"));

  // Add the cpu.stat information only if CFS is enabled.
  if (flags.cgroups_enable_cfs) {
    const Result<string> cpuHierarchy = cgroups::hierarchy("cpu");

    if (cpuHierarchy.isError()) {
      return Error(
          "Failed to determine the cgroup 'cpu' subsystem hierarchy: " +
          cpuHierarchy.error());
    }

    const Result<string> cpuCgroup = cgroups::cpu::cgroup(pid);
    if (cpuCgroup.isError()) {
      return Error(
          "Failed to determine cgroup for the 'cpu' subsystem: " +
          cpuCgroup.error());
    } else if (cpuCgroup.isNone()) {
      return Error("Unable to find 'cpu' cgroup subsystem");
    }

    const Try<hashmap<string, uint64_t>> stat =
      cgroups::stat(cpuHierarchy.get(), cpuCgroup.get(), "cpu.stat");

    if (stat.isError()) {
      return Error("Failed to read cpu.stat: " + stat.error());
    }

    Option<uint64_t> nr_periods = stat.get().get("nr_periods");
    if (nr_periods.isSome()) {
      result.set_cpus_nr_periods(nr_periods.get());
    }

    Option<uint64_t> nr_throttled = stat.get().get("nr_throttled");
    if (nr_throttled.isSome()) {
      result.set_cpus_nr_throttled(nr_throttled.get());
    }

    Option<uint64_t> throttled_time = stat.get().get("throttled_time");
    if (throttled_time.isSome()) {
      result.set_cpus_throttled_time_secs(
          Nanoseconds(throttled_time.get()).secs());
    }
  }

  return result;
#endif // __linux__
}


Future<ContainerStatus> DockerContainerizerProcess::status(
    const ContainerID& containerId)
{
  ContainerStatus result;
  result.mutable_container_id()->CopyFrom(containerId);
  return result;
}


Future<Option<ContainerTermination>> DockerContainerizerProcess::wait(
    const ContainerID& containerId)
{
  CHECK(!containerId.has_parent());

  if (!containers_.contains(containerId)) {
    return None();
  }

  return containers_.at(containerId)->termination.future()
    .then(Option<ContainerTermination>::some);
}


Future<bool> DockerContainerizerProcess::destroy(
    const ContainerID& containerId,
    bool killed)
{
  if (!containers_.contains(containerId)) {
    // TODO(bmahler): Currently the agent does not log destroy
    // failures or unknown containers, so we log it here for now.
    // Move this logging into the callers.
    LOG(WARNING) << "Attempted to destroy unknown container " << containerId;

    return false;
  }

  // TODO(klueska): Ideally, we would do this check as the first thing
  // we do after entering this function. However, the containerizer
  // API currently requires callers of `launch()` to also call
  // `destroy()` if the launch fails (MESOS-6214). As such, putting
  // the check at the top of this function would cause the
  // containerizer to crash if the launch failure was due to the
  // container having its `parent` field set. Once we remove the
  // requirement for `destroy()` to be called explicitly after launch
  // failures, we should move this check to the top of this function.
  CHECK(!containerId.has_parent());

  Container* container = containers_.at(containerId);

  if (container->launch.isFailed()) {
    VLOG(1) << "Container " << containerId << " launch failed";

    // This means we failed to launch the container and we're trying to
    // cleanup.
    CHECK_PENDING(container->status.future());

    // NOTE: The launch error message will be retrieved by the slave
    // and properly set in the corresponding status update.
    container->termination.set(ContainerTermination());

    containers_.erase(containerId);
    delete container;

    return true;
  }

  if (container->state == Container::DESTROYING) {
    return container->termination.future()
      .then([]() { return true; });
  }

  LOG(INFO) << "Destroying container " << containerId;

  // It's possible that destroy is getting called before
  // DockerContainerizer::launch has completed (i.e., after we've
  // returned a future but before we've completed the fetching of the
  // URIs, or the Docker::run, or the wait, etc.).
  //
  // If we're FETCHING, we want to stop the fetching and then
  // cleanup. Note, we need to make sure that we deal with the race
  // with trying to terminate the fetcher so that even if the fetcher
  // returns successfully we won't try to do a Docker::run.
  //
  // If we're PULLING, we want to terminate the 'docker pull' and then
  // cleanup. Just as above, we'll need to deal with the race with
  // 'docker pull' returning successfully.
  //
  // If we're MOUNTING, we want to unmount all the persistent volumes
  // that has been mounted.
  //
  // If we're RUNNING, we want to wait for the status to get set, then
  // do a Docker::kill, then wait for the status to complete, then
  // cleanup.

  if (container->state == Container::FETCHING) {
    LOG(INFO) << "Destroying container " << containerId << " in FETCHING state";

    fetcher->kill(containerId);

    ContainerTermination termination;
    termination.set_message("Container destroyed while fetching");
    container->termination.set(termination);

    // Even if the fetch succeeded just before we did the killtree,
    // removing the container here means that we won't proceed with
    // the Docker::run.
    containers_.erase(containerId);
    delete container;

    return true;
  }

  if (container->state == Container::PULLING) {
    LOG(INFO) << "Destroying container " << containerId << " in PULLING state";

    container->pull.discard();

    ContainerTermination termination;
    termination.set_message("Container destroyed while pulling image");
    container->termination.set(termination);

    containers_.erase(containerId);
    delete container;

    return true;
  }

  if (container->state == Container::MOUNTING) {
    LOG(INFO) << "Destroying container " << containerId << " in MOUNTING state";

    // Persistent volumes might already been mounted, remove them
    // if necessary.
    Try<Nothing> unmount = unmountPersistentVolumes(containerId);
    if (unmount.isError()) {
      LOG(WARNING) << "Failed to remove persistent volumes on destroy for"
                   << " container " << containerId << ": " << unmount.error();
    }

    ContainerTermination termination;
    termination.set_message("Container destroyed while mounting volumes");
    container->termination.set(termination);

    containers_.erase(containerId);
    delete container;

    return true;
  }

  CHECK(container->state == Container::RUNNING);

  container->state = Container::DESTROYING;

  if (killed && container->executorPid.isSome()) {
    LOG(INFO) << "Sending SIGTERM to executor with pid: "
              << container->executorPid.get();
    // We need to clean up the executor as the executor might not have
    // received run task due to a failed containerizer update.
    // We also kill the executor first since container->status below
    // is waiting for the executor to finish.
    Try<list<os::ProcessTree>> kill =
      os::killtree(container->executorPid.get(), SIGTERM);

    if (kill.isError()) {
      // Ignoring the error from killing executor as it can already
      // have exited.
      VLOG(1) << "Ignoring error when killing executor pid "
              << container->executorPid.get() << " in destroy, error: "
              << kill.error();
    }
  }

  // Otherwise, wait for Docker::run to succeed, in which case we'll
  // continue in _destroy (calling Docker::kill) or for Docker::run to
  // fail, in which case we'll re-execute this function and cleanup
  // above.
  container->status.future()
    .onAny(defer(self(), &Self::_destroy, containerId, killed));

  return container->termination.future()
    .then([]() { return true; });
}


void DockerContainerizerProcess::_destroy(
    const ContainerID& containerId,
    bool killed)
{
  CHECK(containers_.contains(containerId));

  Container* container = containers_.at(containerId);

  CHECK(container->state == Container::DESTROYING);

  // Do a 'docker stop' which we'll then find out about in '_destroy'
  // after we've reaped either the container's root process (in the
  // event that we had just launched a container for an executor) or
  // the mesos-docker-executor (in the case we launched a container
  // for a task).
  LOG(INFO) << "Running docker stop on container " << containerId;

  if (killed) {
    // TODO(alexr): After the deprecation cycle (started in 1.0), update
    // this to omit the timeout. Graceful shutdown of the container is not
    // a containerizer responsibility; it is the responsibility of the agent
    // in co-operation with the executor. Once `destroy()` is called, the
    // container should be destroyed forcefully.
    // The `after` fallback should remain as a precaution against the docker
    // stop command hanging.
    docker->stop(container->name(), flags.docker_stop_timeout)
      .after(
          flags.docker_stop_timeout + DOCKER_FORCE_KILL_TIMEOUT,
          defer(self(), &Self::destroyTimeout, containerId, lambda::_1))
      .onAny(defer(self(), &Self::__destroy, containerId, killed, lambda::_1));
  } else {
    __destroy(containerId, killed, Nothing());
  }
}


void DockerContainerizerProcess::__destroy(
    const ContainerID& containerId,
    bool killed,
    const Future<Nothing>& kill)
{
  CHECK(containers_.contains(containerId));

  Container* container = containers_.at(containerId);

  if (!kill.isReady() && !container->status.future().isReady()) {
    // TODO(benh): This means we've failed to do a Docker::kill, which
    // means it's possible that the container is still going to be
    // running after we return! We either need to have a periodic
    // "garbage collector", or we need to retry the Docker::kill
    // indefinitely until it has been successful.

    string failure = "Failed to kill the Docker container: " +
                     (kill.isFailed() ? kill.failure() : "discarded future");

#ifdef __linux__
    // TODO(gyliu): We will never de-allocate these GPUs,
    // unless the agent is restarted!
    if (!container->gpus.empty()) {
      failure += ": " + stringify(container->gpus.size()) + " GPUs leaked";
    }
#endif // __linux__

    container->termination.fail(failure);

    containers_.erase(containerId);

    delay(
      flags.docker_remove_delay,
      self(),
      &Self::remove,
      container->name(),
      container->executorName());

    delete container;

    return;
  }

  // Status must be ready since we did a Docker::kill.
  CHECK_READY(container->status.future());

  container->status.future().get()
    .onAny(defer(self(), &Self::___destroy, containerId, killed, lambda::_1));
}


void DockerContainerizerProcess::___destroy(
    const ContainerID& containerId,
    bool killed,
    const Future<Option<int>>& status)
{
  CHECK(containers_.contains(containerId));

  Try<Nothing> unmount = unmountPersistentVolumes(containerId);
  if (unmount.isError()) {
    // TODO(tnachen): Failing to unmount a persistent volume now
    // leads to leaving the volume on the host, and we won't retry
    // again since the Docker container is removed. We should consider
    // not removing the container so we can retry.
    LOG(WARNING) << "Failed to remove persistent volumes on destroy for"
                 << " container " << containerId << ": " << unmount.error();
  }

  Future<Nothing> deallocateGpus = Nothing();

#ifdef __linux__
  // Deallocate GPU resources before we destroy container.
  if (!containers_.at(containerId)->gpus.empty()) {
    deallocateGpus = deallocateNvidiaGpus(containerId);
  }
#endif // __linux__

  deallocateGpus
    .onAny(defer(self(), &Self::____destroy, containerId, killed, status));
}


void DockerContainerizerProcess::____destroy(
    const ContainerID& containerId,
    bool killed,
    const Future<Option<int>>& status)
{
  Container* container = containers_.at(containerId);

  ContainerTermination termination;

  if (status.isReady() && status.get().isSome()) {
    termination.set_status(status.get().get());
  }

  termination.set_message(
      killed ? "Container killed" : "Container terminated");

  container->termination.set(termination);

  containers_.erase(containerId);

  delay(
    flags.docker_remove_delay,
    self(),
    &Self::remove,
    container->name(),
    container->executorName());

  delete container;
}


Future<Nothing> DockerContainerizerProcess::destroyTimeout(
    const ContainerID& containerId,
    Future<Nothing> future)
{
  CHECK(containers_.contains(containerId));

  LOG(WARNING) << "Docker stop timed out for container " << containerId;

  Container* container = containers_.at(containerId);

  // A hanging `docker stop` could be a problem with docker or even a kernel
  // bug. Assuming that this is a docker problem, circumventing docker and
  // killing the process run by it ourselves might help here.
  if (container->pid.isSome()) {
    LOG(WARNING) << "Sending SIGKILL to process with pid "
                 << container->pid.get();

    Try<list<os::ProcessTree>> kill =
      os::killtree(container->pid.get(), SIGKILL);

    if (kill.isError()) {
      // Ignoring the error from killing process as it can already
      // have exited.
      VLOG(1) << "Ignoring error when killing process pid "
              << container->executorPid.get() << " in destroy, error: "
              << kill.error();
    }
  }

  return future;
}


Future<hashset<ContainerID>> DockerContainerizerProcess::containers()
{
  return containers_.keys();
}


void DockerContainerizerProcess::reaped(const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return;
  }

  LOG(INFO) << "Executor for container " << containerId << " has exited";

  // The executor has exited so destroy the container.
  destroy(containerId, false);
}


void DockerContainerizerProcess::remove(
    const string& containerName,
    const Option<string>& executor)
{
  docker->rm(containerName, true);
  if (executor.isSome()) {
    docker->rm(executor.get(), true);
  }
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
