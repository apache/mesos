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

#include <set>

#include <mesos/module/isolator.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/adaptor.hpp>
#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/wait.hpp>

#include "common/protobuf_utils.hpp"

#include "hook/manager.hpp"

#include "module/manager.hpp"

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/constants.hpp"
#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/launcher.hpp"
#include "slave/containerizer/mesos/paths.hpp"
#include "slave/containerizer/mesos/utils.hpp"

#include "slave/containerizer/mesos/io/switchboard.hpp"

#include "slave/containerizer/mesos/isolators/filesystem/posix.hpp"
#include "slave/containerizer/mesos/isolators/posix.hpp"
#include "slave/containerizer/mesos/isolators/posix/disk.hpp"
#include "slave/containerizer/mesos/isolators/posix/rlimits.hpp"
#include "slave/containerizer/mesos/isolators/volume/sandbox_path.hpp"

#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

#ifdef __WINDOWS__
#include "slave/containerizer/mesos/isolators/windows.hpp"
#include "slave/containerizer/mesos/isolators/filesystem/windows.hpp"
#endif // __WINDOWS__

#ifdef __linux__
#include "slave/containerizer/mesos/linux_launcher.hpp"

#include "slave/containerizer/mesos/isolators/appc/runtime.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/cgroups.hpp"
#include "slave/containerizer/mesos/isolators/docker/runtime.hpp"
#include "slave/containerizer/mesos/isolators/docker/volume/isolator.hpp"
#include "slave/containerizer/mesos/isolators/filesystem/linux.hpp"
#include "slave/containerizer/mesos/isolators/filesystem/shared.hpp"
#include "slave/containerizer/mesos/isolators/gpu/nvidia.hpp"
#include "slave/containerizer/mesos/isolators/linux/capabilities.hpp"
#include "slave/containerizer/mesos/isolators/namespaces/ipc.hpp"
#include "slave/containerizer/mesos/isolators/namespaces/pid.hpp"
#include "slave/containerizer/mesos/isolators/network/cni/cni.hpp"
#include "slave/containerizer/mesos/isolators/volume/image.hpp"
#endif // __linux__

#ifdef WITH_NETWORK_ISOLATOR
#include "slave/containerizer/mesos/isolators/network/port_mapping.hpp"
#endif

#if ENABLE_XFS_DISK_ISOLATOR
#include "slave/containerizer/mesos/isolators/xfs/disk.hpp"
#endif

using process::collect;
using process::dispatch;
using process::defer;

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::Shared;
using process::Subprocess;

using process::http::Connection;

using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

using mesos::internal::slave::state::SlaveState;
using mesos::internal::slave::state::FrameworkState;
using mesos::internal::slave::state::ExecutorState;
using mesos::internal::slave::state::RunState;

using mesos::modules::ModuleManager;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerLogger;
using mesos::slave::ContainerIO;
using mesos::slave::ContainerState;
using mesos::slave::ContainerTermination;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<MesosContainerizer*> MesosContainerizer::create(
    const Flags& flags,
    bool local,
    Fetcher* fetcher,
    const Option<NvidiaComponents>& nvidia)
{
  // Modify `flags` based on the deprecated `isolation` flag (and then
  // use `flags_` in the rest of this function).
  Flags flags_ = flags;

  if (flags.isolation == "process") {
    LOG(WARNING) << "The 'process' isolation flag is deprecated, "
                 << "please update your flags to "
                 << "'--isolation=posix/cpu,posix/mem' (or "
                 << "'--isolation=windows/cpu' if you are on Windows).";

#ifndef __WINDOWS__
    flags_.isolation = "posix/cpu,posix/mem";
#else
    flags_.isolation = "windows/cpu";
#endif // __WINDOWS__
  } else if (flags.isolation == "cgroups") {
    LOG(WARNING) << "The 'cgroups' isolation flag is deprecated, "
                 << "please update your flags to"
                 << " '--isolation=cgroups/cpu,cgroups/mem'.";

    flags_.isolation = "cgroups/cpu,cgroups/mem";
  }

  // One and only one filesystem isolator is required. The filesystem
  // isolator is responsible for preparing the filesystems for
  // containers (e.g., prepare filesystem roots, volumes, etc.). If
  // the user does not specify one, 'filesystem/posix' (or
  // 'filesystem/windows' on Windows) will be used
  //
  // TODO(jieyu): Check that only one filesystem isolator is used.
  if (!strings::contains(flags_.isolation, "filesystem/")) {
#ifndef __WINDOWS__
    flags_.isolation += ",filesystem/posix";
#else
    flags_.isolation += ",filesystem/windows";
#endif // __WINDOWS__
  }

  if (strings::contains(flags_.isolation, "posix/disk")) {
    LOG(WARNING) << "'posix/disk' has been renamed as 'disk/du', "
                 << "please update your --isolation flag to use 'disk/du'";

    if (strings::contains(flags_.isolation, "disk/du")) {
      return Error(
          "Using 'posix/disk' and 'disk/du' simultaneously is disallowed");
    }
  }

#ifdef __linux__
  // One and only one `network` isolator is required. The network
  // isolator is responsible for preparing the network namespace for
  // containers. If the user does not specify one, 'network/cni'
  // isolator will be used.

  // TODO(jieyu): Check that only one network isolator is used.
  if (!strings::contains(flags_.isolation, "network/")) {
    flags_.isolation += ",network/cni";
  }

  // Always enable 'volume/image' on linux if 'filesystem/linux' is
  // enabled, to ensure backwards compatibility.
  //
  // TODO(gilbert): Make sure the 'gpu/nvidia' isolator to be created
  // after all volume isolators, so that the nvidia gpu libraries
  // '/usr/local/nvidia' will be overwritten.
  if (strings::contains(flags_.isolation, "filesystem/linux") &&
      !strings::contains(flags_.isolation, "volume/image")) {
    flags_.isolation += ",volume/image";
  }
#endif // __linux__

  LOG(INFO) << "Using isolation: " << flags_.isolation;

  // Create the launcher for the MesosContainerizer.
  Try<Launcher*> launcher = [&flags_]() -> Try<Launcher*> {
#ifdef __linux__
    if (flags_.launcher == "linux") {
      return LinuxLauncher::create(flags_);
    } else if (flags_.launcher == "posix") {
      return SubprocessLauncher::create(flags_);
    } else {
      return Error("Unknown or unsupported launcher: " + flags_.launcher);
    }
#elif defined(__WINDOWS__)
    if (flags_.launcher != "windows") {
      return Error("Unsupported launcher: " + flags_.launcher);
    }

    return SubprocessLauncher::create(flags_);
#else
    if (flags_.launcher != "posix") {
      return Error("Unsupported launcher: " + flags_.launcher);
    }

    return SubprocessLauncher::create(flags_);
#endif // __linux__
  }();

  if (launcher.isError()) {
    return Error("Failed to create launcher: " + launcher.error());
  }

  Try<Owned<Provisioner>> _provisioner = Provisioner::create(flags_);
  if (_provisioner.isError()) {
    return Error("Failed to create provisioner: " + _provisioner.error());
  }

  Shared<Provisioner> provisioner = _provisioner.get().share();

  // Create the isolators.
  //
  // Currently, the order of the entries in the --isolation flag
  // specifies the ordering of the isolators. Specifically, the
  // `create` and `prepare` calls for each isolator are run serially
  // in the order in which they appear in the --isolation flag, while
  // the `cleanup` call is serialized in reverse order.
  //
  // It is the responsibility of each isolator to check its
  // dependency requirements (if any) during its `create`
  // execution. This means that if the operator specifies the
  // flags in the wrong order, it will produce an error during
  // isolator creation.
  //
  // NOTE: We ignore the placement of the filesystem isolator in
  // the --isolation flag and place it at the front of the isolator
  // list. This is a temporary hack until isolators are able to
  // express and validate their ordering requirements.

  const hashmap<string, lambda::function<Try<Isolator*>(const Flags&)>>
    creators = {
    // Filesystem isolators.
#ifndef __WINDOWS__
    {"filesystem/posix", &PosixFilesystemIsolatorProcess::create},
#else
    {"filesystem/windows", &WindowsFilesystemIsolatorProcess::create},
#endif // __WINDOWS__
#ifdef __linux__
    {"filesystem/linux", &LinuxFilesystemIsolatorProcess::create},

    // TODO(jieyu): Deprecate this in favor of using filesystem/linux.
    {"filesystem/shared", &SharedFilesystemIsolatorProcess::create},
#endif // __linux__

    // Runtime isolators.
#ifndef __WINDOWS__
    {"posix/cpu", &PosixCpuIsolatorProcess::create},
    {"posix/mem", &PosixMemIsolatorProcess::create},
    {"posix/rlimits", &PosixRLimitsIsolatorProcess::create},

    // "posix/disk" is deprecated in favor of the name "disk/du".
    {"posix/disk", &PosixDiskIsolatorProcess::create},
    {"disk/du", &PosixDiskIsolatorProcess::create},
    {"volume/sandbox_path", &VolumeSandboxPathIsolatorProcess::create},

#if ENABLE_XFS_DISK_ISOLATOR
    {"disk/xfs", &XfsDiskIsolatorProcess::create},
#endif // ENABLE_XFS_DISK_ISOLATOR
#else
    {"windows/cpu", &WindowsCpuIsolatorProcess::create},
#endif // __WINDOWS__

#ifdef __linux__
    {"cgroups/blkio", &CgroupsIsolatorProcess::create},
    {"cgroups/cpu", &CgroupsIsolatorProcess::create},
    {"cgroups/cpuset", &CgroupsIsolatorProcess::create},
    {"cgroups/devices", &CgroupsIsolatorProcess::create},
    {"cgroups/hugetlb", &CgroupsIsolatorProcess::create},
    {"cgroups/mem", &CgroupsIsolatorProcess::create},
    {"cgroups/net_cls", &CgroupsIsolatorProcess::create},
    {"cgroups/net_prio", &CgroupsIsolatorProcess::create},
    {"cgroups/perf_event", &CgroupsIsolatorProcess::create},
    {"cgroups/pids", &CgroupsIsolatorProcess::create},
    {"appc/runtime", &AppcRuntimeIsolatorProcess::create},
    {"docker/runtime", &DockerRuntimeIsolatorProcess::create},
    {"docker/volume", &DockerVolumeIsolatorProcess::create},
    {"linux/capabilities", &LinuxCapabilitiesIsolatorProcess::create},

    {"volume/image",
      [&provisioner] (const Flags& flags) -> Try<Isolator*> {
        return VolumeImageIsolatorProcess::create(flags, provisioner);
      }},

    {"gpu/nvidia",
      [&nvidia] (const Flags& flags) -> Try<Isolator*> {
        if (!nvml::isAvailable()) {
          return Error("Cannot create the Nvidia GPU isolator:"
                       " NVML is not available");
        }

        CHECK_SOME(nvidia)
          << "Nvidia components should be set when NVML is available";

        return NvidiaGpuIsolatorProcess::create(flags, nvidia.get());
      }},

    {"namespaces/ipc", &NamespacesIPCIsolatorProcess::create},
    {"namespaces/pid", &NamespacesPidIsolatorProcess::create},
    {"network/cni", &NetworkCniIsolatorProcess::create},
#endif // __linux__
    // NOTE: Network isolation is currently not supported on Windows builds.
#if !defined(__WINDOWS__) && defined(WITH_NETWORK_ISOLATOR)
    {"network/port_mapping", &PortMappingIsolatorProcess::create},
#endif
  };

  vector<string> tokens = strings::tokenize(flags_.isolation, ",");
  set<string> isolations = set<string>(tokens.begin(), tokens.end());

  if (tokens.size() != isolations.size()) {
    return Error("Duplicate entries found in --isolation flag '" +
                 stringify(tokens) + "'");
  }

  vector<Owned<Isolator>> isolators;

  // Note: For cgroups, we only create `CgroupsIsolatorProcess` once.
  // We use this flag to identify whether `CgroupsIsolatorProcess` has
  // been created or not.
  bool cgroupsIsolatorCreated = false;

  foreach (const string& isolation, isolations) {
    if (strings::startsWith(isolation, "cgroups/")) {
      if (cgroupsIsolatorCreated) {
        // Skip when `CgroupsIsolatorProcess` have been created.
        continue;
      } else {
        cgroupsIsolatorCreated = true;
      }
    }

    Try<Isolator*> isolator = [&]() -> Try<Isolator*> {
      if (creators.contains(isolation)) {
        return creators.at(isolation)(flags_);
      } else if (ModuleManager::contains<Isolator>(isolation)) {
        return ModuleManager::create<Isolator>(isolation);
      }
      return Error("Unknown or unsupported isolator");
    }();

    if (isolator.isError()) {
      return Error("Failed to create isolator '" + isolation + "': " +
                   isolator.error());
    }

    // NOTE: The filesystem isolator must be the first isolator used
    // so that the runtime isolators can have a consistent view on the
    // prepared filesystem (e.g., any volume mounts are performed).
    if (strings::contains(isolation, "filesystem/")) {
      isolators.insert(isolators.begin(), Owned<Isolator>(isolator.get()));
    } else {
      isolators.push_back(Owned<Isolator>(isolator.get()));
    }
  }

  return MesosContainerizer::create(
      flags_,
      local,
      fetcher,
      Owned<Launcher>(launcher.get()),
      provisioner,
      isolators);
}


Try<MesosContainerizer*> MesosContainerizer::create(
    const Flags& flags,
    bool local,
    Fetcher* fetcher,
    const Owned<Launcher>& launcher,
    const Shared<Provisioner>& provisioner,
    const vector<Owned<Isolator>>& isolators)
{
  // Add I/O switchboard to the isolator list.
  //
  // TODO(jieyu): This is a workaround currently because we don't have
  // good support for dynamic pointer cast for 'Owned' yet. The I/O
  // switchboard object will be released automatically during the
  // destruction of the containerizer.
  Try<IOSwitchboard*> ioSwitchboard = IOSwitchboard::create(flags, local);
  if (ioSwitchboard.isError()) {
    return Error("Failed to create I/O switchboard: " + ioSwitchboard.error());
  }

  vector<Owned<Isolator>> _isolators(isolators);

  _isolators.push_back(Owned<Isolator>(new MesosIsolator(
      Owned<MesosIsolatorProcess>(ioSwitchboard.get()))));

  return new MesosContainerizer(Owned<MesosContainerizerProcess>(
      new MesosContainerizerProcess(
          flags,
          fetcher,
          ioSwitchboard.get(),
          launcher,
          provisioner,
          _isolators)));
}


MesosContainerizer::MesosContainerizer(
    const Owned<MesosContainerizerProcess>& _process)
  : process(_process)
{
  spawn(process.get());
}


MesosContainerizer::~MesosContainerizer()
{
  terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> MesosContainerizer::recover(
    const Option<state::SlaveState>& state)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::recover,
                  state);
}


Future<bool> MesosContainerizer::launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const map<string, string>& environment,
    bool checkpoint)
{
  // Need to disambiguate for the compiler.
  Future<bool> (MesosContainerizerProcess::*launch)(
      const ContainerID&,
      const Option<TaskInfo>&,
      const ExecutorInfo&,
      const string&,
      const Option<string>&,
      const SlaveID&,
      const map<string, string>&,
      bool) = &MesosContainerizerProcess::launch;

  return dispatch(process.get(),
                  launch,
                  containerId,
                  taskInfo,
                  executorInfo,
                  directory,
                  user,
                  slaveId,
                  environment,
                  checkpoint);
}


Future<bool> MesosContainerizer::launch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const Option<ContainerInfo>& containerInfo,
    const Option<string>& user,
    const SlaveID& slaveId,
    const Option<ContainerClass>& containerClass)
{
  // Need to disambiguate for the compiler.
  Future<bool> (MesosContainerizerProcess::*launch)(
      const ContainerID&,
      const CommandInfo&,
      const Option<ContainerInfo>&,
      const Option<string>&,
      const SlaveID&,
      const Option<ContainerClass>&) = &MesosContainerizerProcess::launch;

  return dispatch(process.get(),
                  launch,
                  containerId,
                  commandInfo,
                  containerInfo,
                  user,
                  slaveId,
                  containerClass);
}


Future<Connection> MesosContainerizer::attach(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::attach,
                  containerId);
}


Future<Nothing> MesosContainerizer::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::update,
                  containerId,
                  resources);
}


Future<ResourceStatistics> MesosContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::usage,
                  containerId);
}


Future<ContainerStatus> MesosContainerizer::status(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::status,
                  containerId);
}


Future<Option<ContainerTermination>> MesosContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::wait,
                  containerId);
}


Future<bool> MesosContainerizer::destroy(const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::destroy,
                  containerId);
}


Future<hashset<ContainerID>> MesosContainerizer::containers()
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::containers);
}


Future<Nothing> MesosContainerizer::remove(const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::remove,
                  containerId);
}


Future<Nothing> MesosContainerizerProcess::recover(
    const Option<state::SlaveState>& state)
{
  LOG(INFO) << "Recovering containerizer";

  // Gather the executor run states that we will attempt to recover.
  list<ContainerState> recoverable;
  if (state.isSome()) {
    foreachvalue (const FrameworkState& framework, state.get().frameworks) {
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

        // We need the pid so the reaper can monitor the executor so
        // skip this executor if it's not present. This is not an
        // error because the slave will try to wait on the container
        // which will return a failed ContainerTermination and
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

        // Note that MesosContainerizer will also recover executors
        // launched by the DockerContainerizer as before 0.23 the
        // slave doesn't checkpoint container information.
        const ExecutorInfo& executorInfo = executor.info.get();
        if (executorInfo.has_container() &&
            executorInfo.container().type() != ContainerInfo::MESOS) {
          LOG(INFO) << "Skipping recovery of executor '" << executor.id
                    << "' of framework " << framework.id
                    << " because it was not launched from mesos containerizer";
          continue;
        }

        LOG(INFO) << "Recovering container " << containerId
                  << " for executor '" << executor.id
                  << "' of framework " << framework.id;

        // NOTE: We create the executor directory before checkpointing
        // the executor. Therefore, it's not possible for this
        // directory to be non-existent.
        const string& directory = paths::getExecutorRunPath(
            flags.work_dir,
            state.get().id,
            framework.id,
            executor.id,
            containerId);

        CHECK(os::exists(directory));

        ContainerState executorRunState =
          protobuf::slave::createContainerState(
              executorInfo,
              run.get().id.get(),
              run.get().forkedPid.get(),
              directory);

        recoverable.push_back(executorRunState);
      }
    }
  }

  // Recover the executor containers from 'SlaveState'.
  hashset<ContainerID> alive;
  foreach (const ContainerState& state, recoverable) {
    const ContainerID& containerId = state.container_id();
    alive.insert(containerId);

    // Contruct the structure for containers from the 'SlaveState'
    // first, to maintain the children list in the container.
    Owned<Container> container(new Container());
    container->status = reap(containerId, state.pid());

    // We only checkpoint the containerizer pid after the container
    // successfully launched, therefore we can assume checkpointed
    // containers should be running after recover.
    container->state = RUNNING;
    container->pid = state.pid();
    container->directory = state.directory();
    containers_[containerId] = container;
  }

  // TODO(gilbert): Draw the logic VENN Diagram here in comment.
  hashset<ContainerID> orphans;

  // Recover the containers from the runtime directory.
  Try<vector<ContainerID>> containerIds =
    containerizer::paths::getContainerIds(flags.runtime_dir);

  if (containerIds.isError()) {
    return Failure(
        "Failed to get container ids from the runtime directory: " +
        containerIds.error());
  }

  // Reconcile the runtime containers with the containers from
  // `recoverable`. Treat discovered orphans as "known orphans"
  // that we aggregate with any orphans that get returned from
  // calling `launcher->recover`.
  foreach (const ContainerID& containerId, containerIds.get()) {
    if (alive.contains(containerId)) {
      continue;
    }

    // Nested containers may have already been destroyed, but we leave
    // their runtime directories around for the lifetime of their
    // top-level container. If they have already been destroyed, we
    // checkpoint their termination state, so the existence of this
    // checkpointed information means we can safely ignore them here.
    const string terminationPath = path::join(
        containerizer::paths::getRuntimePath(flags.runtime_dir, containerId),
        containerizer::paths::TERMINATION_FILE);

    if (os::exists(terminationPath)) {
      continue;
    }

    // Attempt to read the pid from the container runtime directory.
    Result<pid_t> pid =
      containerizer::paths::getContainerPid(flags.runtime_dir, containerId);

    if (pid.isError()) {
      return Failure("Failed to get container pid: " + pid.error());
    }

    // Determine the sandbox if this is a nested container.
    Option<string> directory;
    if (containerId.has_parent()) {
      const ContainerID& rootContainerId =
        protobuf::getRootContainerId(containerId);
      CHECK(containers_.contains(rootContainerId));

      if (containers_[rootContainerId]->directory.isSome()) {
        directory = containerizer::paths::getSandboxPath(
            containers_[rootContainerId]->directory.get(),
            containerId);
      }
    }

    Owned<Container> container(new Container());
    container->state = RUNNING;
    container->pid = pid.isSome() ? pid.get() : Option<pid_t>();
    container->directory = directory;

    // Invoke 'reap' on each 'Container'. However, It's possible
    // that 'pid' for a container is unknown (e.g., agent crashes
    // after fork before checkpoint the pid). In that case, simply
    // assume the child process will exit because of the pipe,
    // and do not call 'reap' on it.
    if (pid.isSome()) {
      container->status = reap(containerId, pid.get());
    } else {
      container->status = Future<Option<int>>(None());
    }

    containers_[containerId] = container;

    // Add recoverable nested containers to the list of 'ContainerState'.
    //
    // TODO(klueska): The final check in the if statement makes sure
    // that this container was not marked for forcible destruction on
    // recover. We currently only support 'destroy-on-recovery'
    // semantics for nested `DEBUG` containers. If we ever support it
    // on other types of containers, we may need duplicate this logic
    // elsewhere.
    if (containerId.has_parent() &&
        alive.contains(protobuf::getRootContainerId(containerId)) &&
        pid.isSome() &&
        !containerizer::paths::getContainerForceDestroyOnRecovery(
            flags.runtime_dir, containerId)) {
      CHECK_SOME(directory);
      ContainerState state =
        protobuf::slave::createContainerState(
            None(),
            containerId,
            container->pid.get(),
            container->directory.get());

      recoverable.push_back(state);
      continue;
    }

    orphans.insert(containerId);
  }

  // Try to recover the launcher first.
  return launcher->recover(recoverable)
    .then(defer(self(), [=](
        const hashset<ContainerID>& launchedOrphans) -> Future<Nothing> {
      // For the extra part of launcher orphans, which are not included
      // in the constructed orphan list. The parent-child relationship
      // will be maintained at the end of 'recover' before orphans are
      // cleaned up.
      hashset<ContainerID> _orphans = orphans;
      foreach (const ContainerID& containerId, launchedOrphans) {
        if (orphans.contains(containerId)) {
          continue;
        }

        Owned<Container> container(new Container());
        container->state = RUNNING;
        container->status = Future<Option<int>>(None());
        containers_[containerId] = container;

        _orphans.insert(containerId);
      }

      return _recover(recoverable, _orphans);
    }));
}


Future<Nothing> MesosContainerizerProcess::_recover(
    const list<ContainerState>& recoverable,
    const hashset<ContainerID>& orphans)
{
  // Recover isolators first then recover the provisioner, because of
  // possible cleanups on unknown containers.
  return recoverIsolators(recoverable, orphans)
    .then(defer(self(), &Self::recoverProvisioner, recoverable, orphans))
    .then(defer(self(), &Self::__recover, recoverable, orphans));
}


Future<list<Nothing>> MesosContainerizerProcess::recoverIsolators(
    const list<ContainerState>& recoverable,
    const hashset<ContainerID>& orphans)
{
  list<Future<Nothing>> futures;

  // Then recover the isolators.
  foreach (const Owned<Isolator>& isolator, isolators) {
    // NOTE: We should not send nested containers to the isolator if
    // the isolator does not support nesting.
    if (isolator->supportsNesting()) {
      futures.push_back(isolator->recover(recoverable, orphans));
    } else {
      // Strip nested containers from 'recoverable' and 'orphans'.
      list<ContainerState> _recoverable;
      hashset<ContainerID> _orphans;

      foreach (const ContainerState& state, recoverable) {
        if (!state.container_id().has_parent()) {
          _recoverable.push_back(state);
        }
      }

      foreach (const ContainerID& orphan, orphans) {
        if (!orphan.has_parent()) {
          _orphans.insert(orphan);
        }
      }

      futures.push_back(isolator->recover(_recoverable, _orphans));
    }
  }

  // If all isolators recover then continue.
  return collect(futures);
}


Future<Nothing> MesosContainerizerProcess::recoverProvisioner(
    const list<ContainerState>& recoverable,
    const hashset<ContainerID>& orphans)
{
  // TODO(gilbert): Consolidate 'recoverProvisioner()' interface
  // once the launcher returns a full set of known containers.
  hashset<ContainerID> knownContainerIds = orphans;

  foreach (const ContainerState& state, recoverable) {
    knownContainerIds.insert(state.container_id());
  }

  return provisioner->recover(knownContainerIds);
}


Future<Nothing> MesosContainerizerProcess::__recover(
    const list<ContainerState>& recovered,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& run, recovered) {
    const ContainerID& containerId = run.container_id();

    foreach (const Owned<Isolator>& isolator, isolators) {
      // If this is a nested container, we need to skip isolators that
      // do not support nesting.
      if (containerId.has_parent() && !isolator->supportsNesting()) {
        continue;
      }

      isolator->watch(containerId)
        .onAny(defer(self(), &Self::limited, containerId, lambda::_1));
    }
  }

  // Maintain the children list in the `Container` struct.
  foreachpair (const ContainerID& containerId,
               const Owned<Container>& container,
               containers_) {
    if (containerId.has_parent()) {
      CHECK(containers_.contains(containerId.parent()));
      containers_[containerId.parent()]->children.insert(containerId);
    }

    // NOTE: We do not register the callback until we correctly setup
    // the parent/child relationship. 'destroy' uses that information
    // to make sure all child containers are cleaned up before it
    // starts to cleanup the parent container.
    container->status->onAny(defer(self(), &Self::reaped, containerId));
  }

  // Destroy all the orphan containers.
  foreach (const ContainerID& containerId, orphans) {
    LOG(INFO) << "Cleaning up orphan container " << containerId;
    destroy(containerId);
  }

  return Nothing();
}


// Launching an executor involves the following steps:
// 1. Call prepare on each isolator.
// 2. Fork the executor. The forked child is blocked from exec'ing until it has
//    been isolated.
// 3. Isolate the executor. Call isolate with the pid for each isolator.
// 4. Fetch the executor.
// 5. Exec the executor. The forked child is signalled to continue. It will
//    first execute any preparation commands from isolators and then exec the
//    executor.
Future<bool> MesosContainerizerProcess::launch(
    const ContainerID& containerId,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& _executorInfo,
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

  if (taskInfo.isSome() &&
      taskInfo.get().has_container() &&
      taskInfo.get().container().type() != ContainerInfo::MESOS) {
    return false;
  }

  // NOTE: We make a copy of the executor info because we may mutate
  // it with default container info.
  ExecutorInfo executorInfo = _executorInfo;

  if (executorInfo.has_container() &&
      executorInfo.container().type() != ContainerInfo::MESOS) {
    return false;
  }

  // Add the default container info to the executor info.
  // TODO(jieyu): Rename the flag to be default_mesos_container_info.
  if (!executorInfo.has_container() &&
      flags.default_container_info.isSome()) {
    executorInfo.mutable_container()->CopyFrom(
        flags.default_container_info.get());
  }

  LOG(INFO) << "Starting container " << containerId
            << " for executor '" << executorInfo.executor_id()
            << "' of framework " << executorInfo.framework_id();

  ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.mutable_command_info()->CopyFrom(executorInfo.command());
  containerConfig.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig.set_directory(directory);

  if (user.isSome()) {
    containerConfig.set_user(user.get());
  }

  if (taskInfo.isSome()) {
    // Command task case.
    containerConfig.mutable_task_info()->CopyFrom(taskInfo.get());

    if (taskInfo->has_container()) {
      ContainerInfo* containerInfo = containerConfig.mutable_container_info();
      containerInfo->CopyFrom(taskInfo->container());
    }
  } else {
    // Other cases.
    if (executorInfo.has_container()) {
      ContainerInfo* containerInfo = containerConfig.mutable_container_info();
      containerInfo->CopyFrom(executorInfo.container());
    }
  }

  return launch(containerId,
                containerConfig,
                environment,
                slaveId,
                checkpoint);
}


Future<bool> MesosContainerizerProcess::launch(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig,
    const map<string, string>& environment,
    const SlaveID& slaveId,
    bool checkpoint)
{
  // Before we launch the container, we first create the container
  // runtime directory to hold internal checkpoint information about
  // the container.
  //
  // NOTE: This is different than the checkpoint information requested
  // by the agent via the `checkpoint` parameter. The containerizer
  // itself uses the runtime directory created here to checkpoint
  // state for internal use.
  const string runtimePath =
    containerizer::paths::getRuntimePath(flags.runtime_dir, containerId);

  Try<Nothing> mkdir = os::mkdir(runtimePath);
  if (mkdir.isError()) {
    return Failure(
        "Failed to make the containerizer runtime directory"
        " '" + runtimePath + "': " + mkdir.error());
  }

  // If we are launching a `DEBUG` container,
  // checkpoint a file to mark it as destroy-on-recovery.
  if (containerConfig.has_container_class() &&
      containerConfig.container_class() == ContainerClass::DEBUG) {
    const string path =
      containerizer::paths::getContainerForceDestroyOnRecoveryPath(
          flags.runtime_dir, containerId);

    Try<Nothing> checkpointed = slave::state::checkpoint(path, "");
    if (checkpointed.isError()) {
      return Failure("Failed to checkpoint file to mark DEBUG container"
                     " as 'destroy-on-recovery'");
    }
  }

  Owned<Container> container(new Container());
  container->state = PROVISIONING;
  container->config = containerConfig;
  container->resources = containerConfig.resources();
  container->directory = containerConfig.directory();

  // Maintain the 'children' list in the parent's 'Container' struct,
  // which will be used for recursive destroy.
  if (containerId.has_parent()) {
    CHECK(containers_.contains(containerId.parent()));
    containers_[containerId.parent()]->children.insert(containerId);
  }

  containers_.put(containerId, container);

  // We'll first provision the image for the container, and
  // then provision the images specified in `volumes` using
  // the 'volume/image' isolator.
  if (!containerConfig.has_container_info() ||
      !containerConfig.container_info().mesos().has_image()) {
    return prepare(containerId, None())
      .then(defer(self(), [this, containerId] () {
        return ioSwitchboard->extractContainerIO(containerId);
      }))
      .then(defer(self(),
                  &Self::_launch,
                  containerId,
                  lambda::_1,
                  environment,
                  slaveId,
                  checkpoint));
  }

  container->provisioning = provisioner->provision(
      containerId,
      containerConfig.container_info().mesos().image());

  return container->provisioning
    .then(defer(self(),
                [=](const ProvisionInfo& provisionInfo) -> Future<bool> {
      return prepare(containerId, provisionInfo)
        .then(defer(self(), [this, containerId] () {
          return ioSwitchboard->extractContainerIO(containerId);
        }))
        .then(defer(self(),
                    &Self::_launch,
                    containerId,
                    lambda::_1,
                    environment,
                    slaveId,
                    checkpoint));
    }));
}


Future<Nothing> MesosContainerizerProcess::prepare(
    const ContainerID& containerId,
    const Option<ProvisionInfo>& provisionInfo)
{
  // This is because if a 'destroy' happens during the provisoiner is
  // provisioning in '_launch', even if the '____destroy' will wait
  // for the 'provision' in '_launch' to finish, there is still a
  // chance that '____destroy' and its dependencies finish before
  // 'prepare' starts since onAny is not guaranteed to be executed
  // in order.
  if (!containers_.contains(containerId)) {
    return Failure("Container destroyed during provisioning");
  }

  const Owned<Container>& container = containers_.at(containerId);

  // Make sure containerizer is not in DESTROYING state, to avoid
  // a possible race that containerizer is destroying the container
  // while it is preparing isolators for the container.
  if (container->state == DESTROYING) {
    return Failure("Container is being destroyed during provisioning");
  }

  CHECK_EQ(container->state, PROVISIONING);

  container->state = PREPARING;

  if (provisionInfo.isSome()) {
    container->config.set_rootfs(provisionInfo->rootfs);

    if (provisionInfo->dockerManifest.isSome() &&
        provisionInfo->appcManifest.isSome()) {
      return Failure("Container cannot have both Docker and Appc manifests");
    }

    if (provisionInfo->dockerManifest.isSome()) {
      ContainerConfig::Docker* docker = container->config.mutable_docker();
      docker->mutable_manifest()->CopyFrom(provisionInfo->dockerManifest.get());
    }

    if (provisionInfo->appcManifest.isSome()) {
      ContainerConfig::Appc* appc = container->config.mutable_appc();
      appc->mutable_manifest()->CopyFrom(provisionInfo->appcManifest.get());
    }
  }

  // Captured for lambdas below.
  ContainerConfig containerConfig = container->config;

  // We prepare the isolators sequentially according to their ordering
  // to permit basic dependency specification, e.g., preparing a
  // filesystem isolator before other isolators.
  Future<list<Option<ContainerLaunchInfo>>> f =
    list<Option<ContainerLaunchInfo>>();

  foreach (const Owned<Isolator>& isolator, isolators) {
    // If this is a nested container, we need to skip isolators that
    // do not support nesting.
    if (containerId.has_parent() && !isolator->supportsNesting()) {
      continue;
    }

    // Chain together preparing each isolator.
    f = f.then([=](list<Option<ContainerLaunchInfo>> launchInfos) {
      return isolator->prepare(containerId, containerConfig)
        .then([=](const Option<ContainerLaunchInfo>& launchInfo) mutable {
          launchInfos.push_back(launchInfo);
          return launchInfos;
        });
      });
  }

  container->launchInfos = f;

  return f.then([]() { return Nothing(); });
}


Future<Nothing> MesosContainerizerProcess::fetch(
    const ContainerID& containerId,
    const SlaveID& slaveId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container destroyed during isolating");
  }

  const Owned<Container>& container = containers_.at(containerId);

  if (container->state == DESTROYING) {
    return Failure("Container is being destroyed during isolating");
  }

  CHECK_EQ(container->state, ISOLATING);

  container->state = FETCHING;

  const string directory = container->config.directory();

  Option<string> user;
  if (container->config.has_user()) {
    user = container->config.user();
  }

  return fetcher->fetch(
      containerId,
      container->config.command_info(),
      directory,
      user,
      slaveId,
      flags)
    .then([=]() -> Future<Nothing> {
      if (HookManager::hooksAvailable()) {
        HookManager::slavePostFetchHook(containerId, directory);
      }
      return Nothing();
    });
}


Future<bool> MesosContainerizerProcess::_launch(
    const ContainerID& containerId,
    const Option<ContainerIO>& containerIO,
    const map<string, string>& environment,
    const SlaveID& slaveId,
    bool checkpoint)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container destroyed during preparing");
  }

  const Owned<Container>& container = containers_.at(containerId);

  if (container->state == DESTROYING) {
    return Failure("Container is being destroyed during preparing");
  }

  CHECK(containerIO.isSome());
  CHECK_EQ(container->state, PREPARING);
  CHECK_READY(container->launchInfos);

  ContainerLaunchInfo launchInfo;

  foreach (const Option<ContainerLaunchInfo>& isolatorLaunchInfo,
           container->launchInfos.get()) {
    if (isolatorLaunchInfo.isNone()) {
      continue;
    }

    // Merge isolator launch infos. Perform necessary sanity checks to
    // make sure launch infos returned by isolators do not conflict.
    if (isolatorLaunchInfo->has_rootfs()) {
      return Failure("Isolator should not specify rootfs");
    }

    if (isolatorLaunchInfo->has_user()) {
      return Failure("Isolator should not specify user");
    }

    // NOTE: 'command' from 'isolatorLaunchInfo' will be merged. It
    // is isolators' responsibility to make sure that the merged
    // command is a valid command.
    if (isolatorLaunchInfo->has_command() &&
        launchInfo.has_command()) {
      VLOG(1) << "Merging launch commands '" << launchInfo.command()
              << "' and '" << isolatorLaunchInfo->command()
              << "' from two different isolators";
    }

    if (isolatorLaunchInfo->has_working_directory() &&
        launchInfo.has_working_directory()) {
      return Failure("Multiple isolators specify working directory");
    }

    if (isolatorLaunchInfo->has_capabilities() &&
        launchInfo.has_capabilities()) {
      return Failure("Multiple isolators specify capabilities");
    }

    if (isolatorLaunchInfo->has_rlimits() &&
        launchInfo.has_rlimits()) {
      return Failure("Multiple isolators specify rlimits");
    }

    if (isolatorLaunchInfo->has_tty_slave_path() &&
        launchInfo.has_tty_slave_path()) {
      return Failure("Multiple isolators specify tty");
    }

    launchInfo.MergeFrom(isolatorLaunchInfo.get());
  }

  // Remove duplicated entries in enter and clone namespaces.
  set<int> enterNamespaces(
      launchInfo.enter_namespaces().begin(),
      launchInfo.enter_namespaces().end());

  set<int> cloneNamespaces(
      launchInfo.clone_namespaces().begin(),
      launchInfo.clone_namespaces().end());

  launchInfo.clear_enter_namespaces();
  launchInfo.clear_clone_namespaces();

  foreach (int ns, enterNamespaces) {
    launchInfo.add_enter_namespaces(ns);
  }

  foreach (int ns, cloneNamespaces) {
    launchInfo.add_clone_namespaces(ns);
  }

  // Determine the launch command for the container.
  if (!launchInfo.has_command()) {
    launchInfo.mutable_command()->CopyFrom(container->config.command_info());
  }

  // For command tasks specifically, we should add the task_environment
  // flag to the launch command of the command executor.
  // TODO(tillt): Remove this once we no longer support the old style
  // command task (i.e., that uses mesos-execute).
  if (container->config.has_task_info() && launchInfo.has_task_environment()) {
    hashmap<string, string> commandTaskEnvironment;

    foreach (const Environment::Variable& variable,
             launchInfo.task_environment().variables()) {
      const string& name = variable.name();
      const string& value = variable.value();
      if (commandTaskEnvironment.contains(name) &&
          commandTaskEnvironment[name] != value) {
        LOG(WARNING) << "Overwriting environment variable '" << name << "' "
                     << "for container " << containerId;
      }
      // TODO(tillt): Consider making this 'secret' aware.
      commandTaskEnvironment[name] = value;
    }

    if (!commandTaskEnvironment.empty()) {
      Environment taskEnvironment;
      foreachpair (
          const string& name, const string& value, commandTaskEnvironment) {
        Environment::Variable* variable = taskEnvironment.add_variables();
        variable->set_name(name);
        variable->set_value(value);
      }
      launchInfo.mutable_command()->add_arguments(
          "--task_environment=" + stringify(JSON::protobuf(taskEnvironment)));
    }
  }

  // For the command executor case, we should add the rootfs flag to
  // the launch command of the command executor.
  // TODO(jieyu): Remove this once we no longer support the old style
  // command task (i.e., that uses mesos-execute).
  // TODO(jieyu): Consider move this to filesystem isolator.
  if (container->config.has_task_info() &&
      container->config.has_rootfs()) {
    launchInfo.mutable_command()->add_arguments(
        "--rootfs=" + container->config.rootfs());
  }

  // TODO(jieyu): 'uris', 'environment' and 'user' in the launch
  // command will be ignored. 'environment' and 'user' are set
  // explicitly in 'ContainerLaunchInfo'. In fact, the above fields
  // should be moved to TaskInfo or ExecutorInfo, instead of putting
  // them in CommandInfo.
  launchInfo.mutable_command()->clear_uris();
  launchInfo.mutable_command()->clear_environment();
  launchInfo.mutable_command()->clear_user();

  // Determine the environment for the command to be launched. The
  // priority of the environment should be:
  //  1) User specified environment in CommandInfo.
  //  2) Environment returned by isolators (i.e., in 'launchInfo').
  //  3) Environment passed from agent (e.g., executor environment).

  // Save a copy of the environment returned by isolators because
  // earlier entries in 'launchInfo.environment' will be overwritten
  // by later entries. 'launchInfo.environment' will later be used as
  // the environment for the container, and the container will not
  // inherit the agent environment.
  Environment isolatorEnvironment = launchInfo.environment();

  Environment* containerEnvironment = launchInfo.mutable_environment();
  containerEnvironment->Clear();

  foreachpair (const string& key, const string& value, environment) {
    Environment::Variable* variable = containerEnvironment->add_variables();
    variable->set_name(key);
    variable->set_value(value);
  }

  // TODO(klueska): Remove the check below once we have a good way of
  // setting the sandbox directory for DEBUG containers.
  if (!container->config.has_container_class() ||
      container->config.container_class() != ContainerClass::DEBUG) {
    // TODO(jieyu): Consider moving this to filesystem isolator.
    //
    // NOTE: For the command executor case, although it uses the host
    // filesystem for itself, we still set 'MESOS_SANDBOX' according to
    // the root filesystem of the task (if specified). Command executor
    // itself does not use this environment variable.
    Environment::Variable* variable = containerEnvironment->add_variables();
    variable->set_name("MESOS_SANDBOX");
    variable->set_value(container->config.has_rootfs()
      ? flags.sandbox_directory
      : container->config.directory());
  }

  containerEnvironment->MergeFrom(isolatorEnvironment);

  // Include any user specified environment variables.
  if (container->config.command_info().has_environment()) {
    containerEnvironment->MergeFrom(
        container->config.command_info().environment());
  }

  // Determine the rootfs for the container to be launched.
  //
  // NOTE: Command task is a special case. Even if the container
  // config has a root filesystem, the executor container still uses
  // the host filesystem.
  if (!container->config.has_task_info() && container->config.has_rootfs()) {
    launchInfo.set_rootfs(container->config.rootfs());
  }

  // Determine the working directory for the container.
  if (launchInfo.has_rootfs()) {
    if (!launchInfo.has_working_directory()) {
      launchInfo.set_working_directory(flags.sandbox_directory);
    }
  } else {
    // NOTE: If the container shares the host filesystem, we should
    // not allow them to 'cd' into an arbitrary directory because
    // that'll create security issues.
    if (launchInfo.has_working_directory()) {
      LOG(WARNING) << "Ignore the working directory '"
                   << launchInfo.working_directory() << "' specified in "
                   << "the container launch info for container "
                   << containerId << " since the container is using the "
                   << "host filesystem";
    }

    launchInfo.set_working_directory(container->config.directory());
  }

  // TODO(klueska): Debug containers should set their working
  // directory to their sandbox directory (once we know how to set
  // that properly).
  if (container->config.has_container_class() &&
      container->config.container_class() == ContainerClass::DEBUG) {
    launchInfo.clear_working_directory();
  }

  // Determine the user to launch the container as.
  if (container->config.has_user()) {
    launchInfo.set_user(container->config.user());
  }

  // TODO(gilbert): Remove this once we no longer support command
  // task in favor of default executor.
  if (container->config.has_task_info() &&
      container->config.has_rootfs()) {
    // We need to set the executor user as root as it needs to
    // perform chroot (even when switch_user is set to false).
    launchInfo.set_user("root");
  }

  // Use a pipe to block the child until it's been isolated.
  // The `pipes` array is captured later in a lambda.
  Try<std::array<int_fd, 2>> pipes_ = os::pipe();

  // TODO(jmlvanre): consider returning failure if `pipe` gives an
  // error. Currently we preserve the previous logic.
  CHECK_SOME(pipes_);

  const std::array<int_fd, 2>& pipes = pipes_.get();

  // Prepare the flags to pass to the launch process.
  MesosContainerizerLaunch::Flags launchFlags;

  launchFlags.launch_info = JSON::protobuf(launchInfo);

  launchFlags.pipe_read = pipes[0];
  launchFlags.pipe_write = pipes[1];

#ifndef __WINDOWS__
  // Set the `runtime_directory` launcher flag so that the launch
  // helper knows where to checkpoint the status of the container
  // once it exits.
  const string runtimePath =
    containerizer::paths::getRuntimePath(flags.runtime_dir, containerId);

  CHECK(os::exists(runtimePath));

  launchFlags.runtime_directory = runtimePath;
#endif // __WINDOWS__

  VLOG(1) << "Launching '" << MESOS_CONTAINERIZER << "' with flags '"
          << launchFlags << "'";

  Option<int> _enterNamespaces;
  Option<int> _cloneNamespaces;

  foreach (int ns, enterNamespaces) {
    _enterNamespaces = _enterNamespaces.isSome()
      ? _enterNamespaces.get() | ns
      : ns;
  }

  foreach (int ns, cloneNamespaces) {
    _cloneNamespaces = _cloneNamespaces.isSome()
      ? _cloneNamespaces.get() | ns
      : ns;
  }

#ifdef __linux__
  // For now we need to special case entering a parent container's
  // mount namespace. We do this to ensure that we have access to
  // the binary we launch with `launcher->fork()`.
  //
  // TODO(klueska): Remove this special case once we pull
  // the container's `init` process out of its container.
  if (_enterNamespaces.isSome() && (_enterNamespaces.get() & CLONE_NEWNS)) {
    CHECK(containerId.has_parent());

    if (!containers_.contains(containerId.parent())) {
      return Failure("Unknown parent container");
    }

    if (containers_.at(containerId.parent())->pid.isNone()) {
      return Failure("Unknown parent container pid");
    }

    pid_t parentPid = containers_.at(containerId.parent())->pid.get();

    Try<pid_t> mountNamespaceTarget = getMountNamespaceTarget(parentPid);
    if (mountNamespaceTarget.isError()) {
      return Failure(
          "Cannot get target mount namespace from "
          "process " + stringify(parentPid));
    }

    launchFlags.namespace_mnt_target = mountNamespaceTarget.get();
    _enterNamespaces = _enterNamespaces.get() & ~CLONE_NEWNS;
  }
#endif // __linux__

  // Passing the launch flags via environment variables to the launch
  // helper due to the sensitivity of those flags. Otherwise the
  // launch flags would have been visible through commands like `ps`
  // which are not protected from unprivileged users on the host.
  map<string, string> launchFlagsEnvironment =
    launchFlags.buildEnvironment("MESOS_CONTAINERIZER_");

  // The launch helper should inherit the agent's environment.
  map<string, string> launchEnvironment = os::environment();

  launchEnvironment.insert(
      launchFlagsEnvironment.begin(),
      launchFlagsEnvironment.end());

  // Fork the child using launcher.
  vector<string> argv(2);
  argv[0] = path::join(flags.launcher_dir, MESOS_CONTAINERIZER);
  argv[1] = MesosContainerizerLaunch::NAME;

  Try<pid_t> forked = launcher->fork(
      containerId,
      argv[0],
      argv,
      containerIO->in,
      containerIO->out,
      containerIO->err,
      nullptr,
      launchEnvironment,
      // 'enterNamespaces' will be ignored by SubprocessLauncher.
      _enterNamespaces,
      // 'cloneNamespaces' will be ignored by SubprocessLauncher.
      _cloneNamespaces);

  if (forked.isError()) {
    return Failure("Failed to fork: " + forked.error());
  }

  pid_t pid = forked.get();
  container->pid = pid;

  // Checkpoint the forked pid if requested by the agent.
  if (checkpoint) {
    const string& path = slave::paths::getForkedPidPath(
        slave::paths::getMetaRootDir(flags.work_dir),
        slaveId,
        container->config.executor_info().framework_id(),
        container->config.executor_info().executor_id(),
        containerId);

    LOG(INFO) << "Checkpointing container's forked pid " << pid
              << " to '" << path << "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(path, stringify(pid));

    if (checkpointed.isError()) {
      LOG(ERROR) << "Failed to checkpoint container's forked pid to '"
                 << path << "': " << checkpointed.error();

      return Failure("Could not checkpoint container's pid");
    }
  }

  // Checkpoint the forked pid to the container runtime directory.
  //
  // NOTE: This checkpoint MUST happen after checkpointing the `pid`
  // to the meta directory above. This ensures that there will never
  // be a pid checkpointed to the container runtime directory until
  // after it has been checkpointed in the agent's meta directory.
  // By maintaining this invariant we know that the only way a `pid`
  // could ever exist in the runtime directory and NOT in the agent
  // meta directory is if the meta directory was wiped clean for
  // some reason. As such, we know if we run into this situation
  // that it is safe to treat the relevant containers as orphans and
  // destroy them.
  const string pidPath = path::join(
      containerizer::paths::getRuntimePath(flags.runtime_dir, containerId),
      containerizer::paths::PID_FILE);

  Try<Nothing> checkpointed =
    slave::state::checkpoint(pidPath, stringify(pid));

  if (checkpointed.isError()) {
    return Failure("Failed to checkpoint the container pid to"
                   " '" + pidPath + "': " + checkpointed.error());
  }

  // Monitor the forked process's pid. We keep the future because
  // we'll refer to it again during container destroy.
  container->status = reap(containerId, pid);
  container->status->onAny(defer(self(), &Self::reaped, containerId));

  return isolate(containerId, pid)
    .then(defer(self(),
                &Self::fetch,
                containerId,
                slaveId))
    .then(defer(self(), &Self::exec, containerId, pipes[1]))
    .onAny([pipes]() { os::close(pipes[0]); })
    .onAny([pipes]() { os::close(pipes[1]); });
}


Future<bool> MesosContainerizerProcess::isolate(
    const ContainerID& containerId,
    pid_t _pid)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container destroyed during preparing");
  }

  if (containers_.at(containerId)->state == DESTROYING) {
    return Failure("Container is being destroyed during preparing");
  }

  CHECK_EQ(containers_.at(containerId)->state, PREPARING);

  containers_.at(containerId)->state = ISOLATING;

  // Set up callbacks for isolator limitations.
  foreach (const Owned<Isolator>& isolator, isolators) {
    // If this is a nested container, we need to skip isolators that
    // do not support nesting.
    if (containerId.has_parent() && !isolator->supportsNesting()) {
      continue;
    }

    isolator->watch(containerId)
      .onAny(defer(self(), &Self::limited, containerId, lambda::_1));
  }

  // Isolate the executor with each isolator.
  // NOTE: This is done is parallel and is not sequenced like prepare
  // or destroy because we assume there are no dependencies in
  // isolation.
  list<Future<Nothing>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    // If this is a nested container, we need to skip isolators that
    // do not support nesting.
    if (containerId.has_parent() && !isolator->supportsNesting()) {
      continue;
    }

    futures.push_back(isolator->isolate(containerId, _pid));
  }

  // Wait for all isolators to complete.
  Future<list<Nothing>> future = collect(futures);

  containers_.at(containerId)->isolation = future;

  return future.then([]() { return true; });
}


Future<bool> MesosContainerizerProcess::exec(
    const ContainerID& containerId,
    int_fd pipeWrite)
{
  // The container may be destroyed before we exec the executor so
  // return failure here.
  if (!containers_.contains(containerId)) {
    return Failure("Container destroyed during fetching");
  }

  if (containers_.at(containerId)->state == DESTROYING) {
    return Failure("Container is being destroyed during fetching");
  }

  CHECK_EQ(containers_.at(containerId)->state, FETCHING);

  // Now that we've contained the child we can signal it to continue
  // by writing to the pipe.
  char dummy;
  ssize_t length;
  while ((length = os::write(pipeWrite, &dummy, sizeof(dummy))) == -1 &&
         errno == EINTR);

  if (length != sizeof(dummy)) {
    return Failure("Failed to synchronize child process: " +
                   os::strerror(errno));
  }

  containers_.at(containerId)->state = RUNNING;

  return true;
}


Future<bool> MesosContainerizerProcess::launch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const Option<ContainerInfo>& containerInfo,
    const Option<string>& user,
    const SlaveID& slaveId,
    const Option<ContainerClass>& containerClass)
{
  CHECK(containerId.has_parent());

  if (containers_.contains(containerId)) {
    return Failure(
        "Nested container " + stringify(containerId) + " already started");
  }

  const ContainerID& parentContainerId = containerId.parent();
  if (!containers_.contains(parentContainerId)) {
    return Failure(
        "Parent container " + stringify(parentContainerId) +
        " does not exist");
  }

  if (containers_[parentContainerId]->state == DESTROYING) {
    return Failure(
        "Parent container " + stringify(parentContainerId) +
        " is in 'DESTROYING' state");
  }

  LOG(INFO) << "Starting nested container " << containerId;

  const ContainerID rootContainerId = protobuf::getRootContainerId(containerId);

  CHECK(containers_.contains(rootContainerId));
  if (containers_[rootContainerId]->directory.isNone()) {
    return Failure(
        "Unexpected empty sandbox directory for root container " +
        stringify(rootContainerId));
  }

  const string directory = containerizer::paths::getSandboxPath(
      containers_[rootContainerId]->directory.get(),
      containerId);

  Try<Nothing> mkdir = os::mkdir(directory);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create nested sandbox directory '" +
        directory + "': " + mkdir.error());
  }

#ifndef __WINDOWS__
  if (user.isSome()) {
    LOG(INFO) << "Trying to chown '" << directory << "' to user '"
              << user.get() << "'";

    Try<Nothing> chown = os::chown(user.get(), directory);
    if (chown.isError()) {
      LOG(WARNING) << "Failed to chown sandbox directory '" << directory
                   << "'. This may be due to attempting to run the container "
                   << "as a nonexistent user on the agent; see the description"
                   << " for the `--switch_user` flag for more information: "
                   << chown.error();
    }
  }
#endif // __WINDOWS__

  ContainerConfig containerConfig;
  containerConfig.mutable_command_info()->CopyFrom(commandInfo);
  containerConfig.set_directory(directory);

  if (user.isSome()) {
    containerConfig.set_user(user.get());
  }

  if (containerInfo.isSome()) {
    containerConfig.mutable_container_info()->CopyFrom(containerInfo.get());
  }

  if (containerClass.isSome()) {
    containerConfig.set_container_class(containerClass.get());
  }

  // TODO(jieyu): This is currently best effort. After the agent fails
  // over, 'executor_info' won't be set in root parent container's
  // 'config'. Consider populating 'executor_info' in recover path.
  if (containers_[rootContainerId]->config.has_executor_info()) {
    containerConfig.mutable_executor_info()->CopyFrom(
        containers_[rootContainerId]->config.executor_info());
  }

  return launch(containerId,
                containerConfig,
                map<string, string>(),
                slaveId,
                false);
}


Future<Connection> MesosContainerizerProcess::attach(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Unknown container " + stringify(containerId));
  }

  return ioSwitchboard->connect(containerId);
}


Future<Option<ContainerTermination>> MesosContainerizerProcess::wait(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    // If a container does not exist in our `container_` hashmap, it
    // may be a nested container with checkpointed termination
    // state. Attempt to return as such.
    if (containerId.has_parent()) {
      Result<ContainerTermination> termination =
        containerizer::paths::getContainerTermination(
            flags.runtime_dir,
            containerId);

      if (termination.isError()) {
        return Failure("Failed to get container termination state:"
                       " " + termination.error());
      }

      if (termination.isSome()) {
        return termination.get();
      }
    }

    // For all other cases return `None()`. See the comments in
    // `destroy()` for race conditions which lead to "unknown
    // containers".
    return None();
  }

  return containers_.at(containerId)->termination.future()
    .then(Option<ContainerTermination>::some);
}


Future<Nothing> MesosContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  CHECK(!containerId.has_parent());

  if (!containers_.contains(containerId)) {
    // It is not considered a failure if the container is not known
    // because the slave will attempt to update the container's
    // resources on a task's terminal state change but the executor
    // may have already exited and the container cleaned up.
    LOG(WARNING) << "Ignoring update for unknown container " << containerId;
    return Nothing();
  }

  const Owned<Container>& container = containers_.at(containerId);

  if (container->state == DESTROYING) {
    LOG(WARNING) << "Ignoring update for currently being destroyed "
                 << "container " << containerId;
    return Nothing();
  }

  // NOTE: We update container's resources before isolators are updated
  // so that subsequent containerizer->update can be handled properly.
  container->resources = resources;

  // Update each isolator.
  list<Future<Nothing>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    // NOTE: No need to skip non-nesting aware isolator here because
    // 'update' currently will not be called for nested container.
    futures.push_back(isolator->update(containerId, resources));
  }

  // Wait for all isolators to complete.
  return collect(futures)
    .then([]() { return Nothing(); });
}


// Resources are used to set the limit fields in the statistics but
// are optional because they aren't known after recovery until/unless
// update() is called.
Future<ResourceStatistics> _usage(
    const ContainerID& containerId,
    const Option<Resources>& resources,
    const list<Future<ResourceStatistics>>& statistics)
{
  CHECK(!containerId.has_parent());

  ResourceStatistics result;

  // Set the timestamp now we have all statistics.
  result.set_timestamp(Clock::now().secs());

  foreach (const Future<ResourceStatistics>& statistic, statistics) {
    if (statistic.isReady()) {
      result.MergeFrom(statistic.get());
    } else {
      LOG(WARNING) << "Skipping resource statistic for container "
                   << containerId << " because: "
                   << (statistic.isFailed() ? statistic.failure()
                                            : "discarded");
    }
  }

  if (resources.isSome()) {
    // Set the resource allocations.
    Option<Bytes> mem = resources.get().mem();
    if (mem.isSome()) {
      result.set_mem_limit_bytes(mem.get().bytes());
    }

    Option<double> cpus = resources.get().cpus();
    if (cpus.isSome()) {
      result.set_cpus_limit(cpus.get());
    }
  }

  return result;
}


Future<ResourceStatistics> MesosContainerizerProcess::usage(
    const ContainerID& containerId)
{
  CHECK(!containerId.has_parent());

  if (!containers_.contains(containerId)) {
    return Failure("Unknown container " + stringify(containerId));
  }

  list<Future<ResourceStatistics>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    // NOTE: No need to skip non-nesting aware isolator here because
    // 'update' currently will not be called for nested container.
    futures.push_back(isolator->usage(containerId));
  }

  // Use await() here so we can return partial usage statistics.
  // TODO(idownes): After recovery resources won't be known until
  // after an update() because they aren't part of the SlaveState.
  return await(futures)
    .then(lambda::bind(
          _usage,
          containerId,
          containers_.at(containerId)->resources,
          lambda::_1));
}


Future<ContainerStatus> MesosContainerizerProcess::status(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  list<Future<ContainerStatus>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    // If this is a nested container, we need to skip isolators that
    // do not support nesting.
    if (containerId.has_parent() && !isolator->supportsNesting()) {
      continue;
    }

    futures.push_back(isolator->status(containerId));
  }
  futures.push_back(launcher->status(containerId));

  // We are using `await` here since we are interested in partial
  // results from calls to `isolator->status`. We also need to
  // serialize the invocation to `await` in order to maintain the
  // order of requests for `ContainerStatus` by the agent.  See
  // MESOS-4671 for more details.
  VLOG(2) << "Serializing status request for container " << containerId;

  return containers_.at(containerId)->sequence.add<ContainerStatus>(
      [=]() -> Future<ContainerStatus> {
        return await(futures)
          .then([containerId](const list<Future<ContainerStatus>>& statuses) {
            ContainerStatus result;
            result.mutable_container_id()->CopyFrom(containerId);

            foreach (const Future<ContainerStatus>& status, statuses) {
              if (status.isReady()) {
                result.MergeFrom(status.get());
              } else {
                LOG(WARNING) << "Skipping status for container "
                             << containerId << " because: "
                             << (status.isFailed() ? status.failure()
                                                   : "discarded");
              }
            }

            VLOG(2) << "Aggregating status for container " << containerId;

            return result;
          });
      });
}


Future<bool> MesosContainerizerProcess::destroy(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    // This can happen due to the race between destroys initiated by
    // the launch failure, the terminated executor and the agent so
    // the same container is destroyed multiple times in reaction to
    // one failure. e.g., a stuck fetcher results in:
    // - The agent invoking destroy(), which kills the fetcher and
    //   the executor.
    // - The agent invoking destroy() again for the failed launch
    //   (due to the fetcher getting killed).
    // - The containerizer invoking destroy() for the reaped executor.
    //
    // The guard here and `if (container->state == DESTROYING)` below
    // make sure redundant destroys short-circuit.

    // TODO(bmahler): Currently the agent does not log destroy
    // failures or unknown containers, so we log it here for now.
    // Move this logging into the callers.
    LOG(WARNING) << "Attempted to destroy unknown container " << containerId;

    return false;
  }

  const Owned<Container>& container = containers_.at(containerId);

  if (container->state == DESTROYING) {
    return container->termination.future()
      .then([]() { return true; });
  }

  LOG(INFO) << "Destroying container " << containerId << " in "
            << container->state << " state";

  // NOTE: We save the previous state so that '_destroy' can properly
  // cleanup based on the previous state of the container.
  State previousState = container->state;

  container->state = DESTROYING;

  list<Future<bool>> destroys;
  foreach (const ContainerID& child, container->children) {
    destroys.push_back(destroy(child));
  }

  await(destroys)
    .then(defer(self(), [=](const list<Future<bool>>& futures) {
      _destroy(containerId, previousState, futures);
      return Nothing();
    }));

  return container->termination.future()
    .then([]() { return true; });
}


void MesosContainerizerProcess::_destroy(
    const ContainerID& containerId,
    const State& previousState,
    const list<Future<bool>>& destroys)
{
  CHECK(containers_.contains(containerId));

  const Owned<Container>& container = containers_[containerId];

  CHECK_EQ(container->state, DESTROYING);

  vector<string> errors;
  foreach (const Future<bool>& future, destroys) {
    if (!future.isReady()) {
      errors.push_back(future.isFailed()
        ? future.failure()
        : "discarded");
    }
  }

  if (!errors.empty()) {
    container->termination.fail(
        "Failed to destroy nested containers: " +
        strings::join("; ", errors));

    ++metrics.container_destroy_errors;
    return;
  }

  if (previousState == PROVISIONING) {
    VLOG(1) << "Waiting for the provisioner to complete provisioning "
            << "before destroying container " << containerId;

    // Wait for the provisioner to finish provisioning before we
    // start destroying the container.
    container->provisioning
      .onAny(defer(
          self(),
          &Self::_____destroy,
          containerId,
          list<Future<Nothing>>()));

    return;
  }

  if (previousState == PREPARING) {
    VLOG(1) << "Waiting for the isolators to complete preparing "
            << "before destroying container " << containerId;

    // We need to wait for the isolators to finish preparing to
    // prevent a race that the destroy method calls the 'cleanup'
    // method of an isolator before the 'prepare' method is called.
    //
    // NOTE: It's likely that the launcher already forked the
    // container. However, since we change the state to 'DESTROYING',
    // the 'isolate()' will fail, causing the control pipes being
    // closed. The container will terminate itself. Therefore, we
    // should wait for the container to terminate before we start to
    // cleanup isolators.
    await(container->launchInfos,
          container->status.isSome()
            ? container->status.get()
            : None())
      .onAny(defer(self(), &Self::____destroy, containerId));

    return;
  }

  if (previousState == ISOLATING) {
    VLOG(1) << "Waiting for the isolators to complete isolation "
            << "before destroying container " << containerId;

    // Wait for the isolators to finish isolating before we start
    // to destroy the container.
    container->isolation
      .onAny(defer(self(), &Self::__destroy, containerId));

    return;
  }

  // Either RUNNING or FETCHING at this point.
  if (previousState == FETCHING) {
    fetcher->kill(containerId);
  }

  __destroy(containerId);
}


void MesosContainerizerProcess::__destroy(
    const ContainerID& containerId)
{
  CHECK(containers_.contains(containerId));

  // Kill all processes then continue destruction.
  launcher->destroy(containerId)
    .onAny(defer(self(), &Self::___destroy, containerId, lambda::_1));
}


void MesosContainerizerProcess::___destroy(
    const ContainerID& containerId,
    const Future<Nothing>& future)
{
  CHECK(containers_.contains(containerId));

  const Owned<Container>& container = containers_.at(containerId);

  // Something has gone wrong and the launcher wasn't able to kill all
  // the processes in the container. We cannot clean up the isolators
  // because they may require that all processes have exited so just
  // return the failure to the slave.
  // TODO(idownes): This is a pretty bad state to be in but we should
  // consider cleaning up here.
  if (!future.isReady()) {
    container->termination.fail(
        "Failed to kill all processes in the container: " +
        (future.isFailed() ? future.failure() : "discarded future"));

    ++metrics.container_destroy_errors;
    return;
  }

  // We've successfully killed all processes in the container so get
  // the exit status of the executor when it's ready (it may already
  // be) and continue the destroy.
  CHECK_SOME(container->status);

  container->status.get()
    .onAny(defer(self(), &Self::____destroy, containerId));
}


void MesosContainerizerProcess::____destroy(
    const ContainerID& containerId)
{
  CHECK(containers_.contains(containerId));

  cleanupIsolators(containerId)
    .onAny(defer(self(), &Self::_____destroy, containerId, lambda::_1));
}


void MesosContainerizerProcess::_____destroy(
    const ContainerID& containerId,
    const Future<list<Future<Nothing>>>& cleanups)
{
  // This should not occur because we only use the Future<list> to
  // facilitate chaining.
  CHECK_READY(cleanups);
  CHECK(containers_.contains(containerId));

  const Owned<Container>& container = containers_.at(containerId);

  // Check cleanup succeeded for all isolators. If not, we'll fail the
  // container termination.
  vector<string> errors;
  foreach (const Future<Nothing>& cleanup, cleanups.get()) {
    if (!cleanup.isReady()) {
      errors.push_back(cleanup.isFailed()
        ? cleanup.failure()
        : "discarded");
    }
  }

  if (!errors.empty()) {
    container->termination.fail(
        "Failed to clean up an isolator when destroying container: " +
        strings::join("; ", errors));

    ++metrics.container_destroy_errors;
    return;
  }

  provisioner->destroy(containerId)
    .onAny(defer(self(), &Self::______destroy, containerId, lambda::_1));
}


void MesosContainerizerProcess::______destroy(
    const ContainerID& containerId,
    const Future<bool>& destroy)
{
  CHECK(containers_.contains(containerId));

  const Owned<Container>& container = containers_.at(containerId);

  if (!destroy.isReady()) {
    container->termination.fail(
        "Failed to destroy the provisioned rootfs when destroying container: " +
        (destroy.isFailed() ? destroy.failure() : "discarded future"));

    ++metrics.container_destroy_errors;
    return;
  }

  ContainerTermination termination;

  if (container->status.isSome() &&
      container->status->isReady() &&
      container->status->get().isSome()) {
    termination.set_status(container->status->get().get());
  }

  // NOTE: We may not see a limitation in time for it to be
  // registered. This could occur if the limitation (e.g., an OOM)
  // killed the executor and we triggered destroy() off the executor
  // exit.
  if (!container->limitations.empty()) {
    termination.set_state(TaskState::TASK_FAILED);

    // We concatenate the messages if there are multiple limitations.
    vector<string> messages;

    foreach (const ContainerLimitation& limitation, container->limitations) {
      messages.push_back(limitation.message());

      if (limitation.has_reason()) {
        termination.add_reasons(limitation.reason());
      }
    }

    termination.set_message(strings::join("; ", messages));
  }

  // Now that we are done destroying the container we need to cleanup
  // its runtime directory. There are two cases to consider:
  //
  // (1) We are a nested container:
  //     In this case we should defer deletion of the runtime directory
  //     until the top-level container is destroyed. Instead, we
  //     checkpoint a file with the termination state indicating that
  //     the container has already been destroyed. This allows
  //     subsequent calls to `wait()` to succeed with the proper
  //     termination state until the top-level container is destroyed.
  //     It also prevents subsequent `destroy()` calls from attempting
  //     to cleanup the container a second time.
  //
  // (2) We are a top-level container:
  //     We should simply remove the runtime directory. Since we build
  //     the runtime directories of nested containers hierarchically,
  //     removing the top-level runtime directory will automatically
  //     cleanup all nested container runtime directories as well.
  //
  // NOTE: The runtime directory will not exist for legacy containers,
  // so we need to make sure it actually exists before attempting to
  // remove it.
  const string runtimePath =
    containerizer::paths::getRuntimePath(flags.runtime_dir, containerId);

  if (containerId.has_parent()) {
    const string terminationPath =
      path::join(runtimePath, containerizer::paths::TERMINATION_FILE);

    LOG(INFO) << "Checkpointing termination state to nested container's"
              << " runtime directory '" << terminationPath << "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(terminationPath, termination);

    if (checkpointed.isError()) {
      LOG(ERROR) << "Failed to checkpoint nested container's termination state"
                 << " to '" << terminationPath << "': " << checkpointed.error();
    }
  } else if (os::exists(runtimePath)) {
    Try<Nothing> rmdir = os::rmdir(runtimePath);
    if (rmdir.isError()) {
      LOG(WARNING) << "Failed to remove the runtime directory"
                   << " for container " << containerId
                   << ": " << rmdir.error();
    }
  }

  container->termination.set(termination);

  if (containerId.has_parent()) {
    CHECK(containers_.contains(containerId.parent()));
    CHECK(containers_[containerId.parent()]->children.contains(containerId));
    containers_[containerId.parent()]->children.erase(containerId);
  }

  containers_.erase(containerId);
}


Future<Nothing> MesosContainerizerProcess::remove(
    const ContainerID& containerId)
{
  // TODO(gkleiman): Check that recovery has completed before continuing.

  CHECK(containerId.has_parent());

  if (containers_.contains(containerId)) {
    return Failure("Nested container has not terminated yet");
  }

  const ContainerID rootContainerId = protobuf::getRootContainerId(containerId);

  if (!containers_.contains(rootContainerId)) {
    return Failure("Unknown root container");
  }

  const string runtimePath =
    containerizer::paths::getRuntimePath(flags.runtime_dir, containerId);

  if (os::exists(runtimePath)) {
    Try<Nothing> rmdir = os::rmdir(runtimePath);
    if (rmdir.isError()) {
      return Failure(
          "Failed to remove the runtime directory: " + rmdir.error());
    }
  }

  const string sandboxPath = containerizer::paths::getSandboxPath(
      containers_[rootContainerId]->directory.get(), containerId);

  if (os::exists(sandboxPath)) {
    Try<Nothing> rmdir = os::rmdir(sandboxPath);
    if (rmdir.isError()) {
      return Failure(
          "Failed to remove the sandbox directory: " + rmdir.error());
    }
  }

  return Nothing();
}


Future<Option<int>> MesosContainerizerProcess::reap(
    const ContainerID& containerId,
    pid_t pid)
{
#ifdef __WINDOWS__
  // We currently don't checkpoint the wait status on windows so
  // just return the reaped status directly.
  return process::reap(pid);
#else
  return process::reap(pid)
    .then(defer(self(), [=](const Option<int>& status) -> Future<Option<int>> {
      // Determine if we just reaped a legacy container or a
      // non-legacy container. We do this by checking for the
      // existence of the container runtime directory (which only
      // exists for new (i.e. non-legacy) containers). If it is a
      // legacy container, we simply forward the reaped exit status
      // back to the caller.
      const string runtimePath =
        containerizer::paths::getRuntimePath(flags.runtime_dir, containerId);

      if (!os::exists(runtimePath)) {
        return status;
      }

      // If we are a non-legacy container, attempt to reap the
      // container status from the checkpointed status file.
      Result<int> containerStatus =
        containerizer::paths::getContainerStatus(
            flags.runtime_dir,
            containerId);

      if (containerStatus.isError()) {
        return Failure("Failed to get container status: " +
                       containerStatus.error());
      } else if (containerStatus.isSome()) {
        return containerStatus.get();
      }

      // If there isn't a container status file or it is empty, then the
      // init process must have been interrupted by a SIGKILL before
      // it had a chance to write the file. Return as such.
      return W_EXITCODE(0, SIGKILL);
    }));
#endif // __WINDOWS__
}


void MesosContainerizerProcess::reaped(const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return;
  }

  LOG(INFO) << "Container " << containerId << " has exited";

  // The executor has exited so destroy the container.
  destroy(containerId);
}


void MesosContainerizerProcess::limited(
    const ContainerID& containerId,
    const Future<ContainerLimitation>& future)
{
  if (!containers_.contains(containerId) ||
      containers_.at(containerId)->state == DESTROYING) {
    return;
  }

  if (future.isReady()) {
    LOG(INFO) << "Container " << containerId << " has reached its limit for"
              << " resource " << future.get().resources()
              << " and will be terminated";

    containers_.at(containerId)->limitations.push_back(future.get());
  } else {
    // TODO(idownes): A discarded future will not be an error when
    // isolators discard their promises after cleanup.
    LOG(ERROR) << "Error in a resource limitation for container "
               << containerId << ": " << (future.isFailed() ? future.failure()
                                                            : "discarded");
  }

  // The container has been affected by the limitation so destroy it.
  destroy(containerId);
}


Future<hashset<ContainerID>> MesosContainerizerProcess::containers()
{
  return containers_.keys();
}


MesosContainerizerProcess::Metrics::Metrics()
  : container_destroy_errors(
        "containerizer/mesos/container_destroy_errors")
{
  process::metrics::add(container_destroy_errors);
}


MesosContainerizerProcess::Metrics::~Metrics()
{
  process::metrics::remove(container_destroy_errors);
}


Future<list<Future<Nothing>>> MesosContainerizerProcess::cleanupIsolators(
    const ContainerID& containerId)
{
  Future<list<Future<Nothing>>> f = list<Future<Nothing>>();

  // NOTE: We clean up each isolator in the reverse order they were
  // prepared (see comment in prepare()).
  foreach (const Owned<Isolator>& isolator, adaptor::reverse(isolators)) {
    // If this is a nested container, we need to skip isolators that
    // do not support nesting.
    if (containerId.has_parent() && !isolator->supportsNesting()) {
      continue;
    }

    // We'll try to clean up all isolators, waiting for each to
    // complete and continuing if one fails.
    // TODO(jieyu): Technically, we cannot bind 'isolator' here
    // because the ownership will be transferred after the bind.
    f = f.then([=](list<Future<Nothing>> cleanups) {
      // Accumulate but do not propagate any failure.
      Future<Nothing> cleanup = isolator->cleanup(containerId);
      cleanups.push_back(cleanup);

      // Wait for the cleanup to complete/fail before returning the
      // list. We use await here to asynchronously wait for the
      // isolator to complete then return cleanups.
      return await(list<Future<Nothing>>({cleanup}))
        .then([cleanups]() -> Future<list<Future<Nothing>>> {
          return cleanups;
        });
    });
  }

  return f;
}


std::ostream& operator<<(
    std::ostream& stream,
    const MesosContainerizerProcess::State& state)
{
  switch (state) {
    case MesosContainerizerProcess::PROVISIONING:
      return stream << "PROVISIONING";
    case MesosContainerizerProcess::PREPARING:
      return stream << "PREPARING";
    case MesosContainerizerProcess::ISOLATING:
      return stream << "ISOLATING";
    case MesosContainerizerProcess::FETCHING:
      return stream << "FETCHING";
    case MesosContainerizerProcess::RUNNING:
      return stream << "RUNNING";
    case MesosContainerizerProcess::DESTROYING:
      return stream << "DESTROYING";
    default:
      UNREACHABLE();
  }
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {
