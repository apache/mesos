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

#include <algorithm>
#include <set>
#include <utility>

#include <mesos/module/isolator.hpp>

#include <mesos/secret/resolver.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>
#include <process/time.hpp>

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

#ifdef __WINDOWS__
#include <stout/internal/windows/inherit.hpp>
#endif // __WINDOWS__

#include <stout/os/wait.hpp>

#include "common/protobuf_utils.hpp"

#include "hook/manager.hpp"

#ifdef ENABLE_LAUNCHER_SEALING
#include "linux/memfd.hpp"
#endif // ENABLE_LAUNCHER_SEALING

#include "module/manager.hpp"

#include "slave/csi_server.hpp"
#include "slave/gc.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/constants.hpp"
#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/isolator_tracker.hpp"
#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/launcher.hpp"
#include "slave/containerizer/mesos/launcher_tracker.hpp"
#include "slave/containerizer/mesos/paths.hpp"
#include "slave/containerizer/mesos/utils.hpp"

#include "slave/containerizer/mesos/io/switchboard.hpp"

#include "slave/containerizer/mesos/isolators/environment_secret.hpp"
#include "slave/containerizer/mesos/isolators/filesystem/posix.hpp"
#include "slave/containerizer/mesos/isolators/posix.hpp"
#include "slave/containerizer/mesos/isolators/posix/disk.hpp"
#include "slave/containerizer/mesos/isolators/posix/rlimits.hpp"
#include "slave/containerizer/mesos/isolators/volume/host_path.hpp"
#include "slave/containerizer/mesos/isolators/volume/sandbox_path.hpp"

#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

#ifdef __WINDOWS__
#include "slave/containerizer/mesos/isolators/docker/runtime.hpp"
#include "slave/containerizer/mesos/isolators/windows/cpu.hpp"
#include "slave/containerizer/mesos/isolators/windows/mem.hpp"
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
#include "slave/containerizer/mesos/isolators/linux/capabilities.hpp"
#include "slave/containerizer/mesos/isolators/linux/devices.hpp"
#include "slave/containerizer/mesos/isolators/linux/nnp.hpp"
#include "slave/containerizer/mesos/isolators/namespaces/ipc.hpp"
#include "slave/containerizer/mesos/isolators/namespaces/pid.hpp"
#include "slave/containerizer/mesos/isolators/network/cni/cni.hpp"
#include "slave/containerizer/mesos/isolators/volume/host_path.hpp"
#include "slave/containerizer/mesos/isolators/volume/image.hpp"
#include "slave/containerizer/mesos/isolators/volume/secret.hpp"
#include "slave/containerizer/mesos/isolators/volume/csi/isolator.hpp"
#endif // __linux__

#if ENABLE_SECCOMP_ISOLATOR
#include "slave/containerizer/mesos/isolators/linux/seccomp.hpp"
#endif

#ifdef ENABLE_PORT_MAPPING_ISOLATOR
#include "slave/containerizer/mesos/isolators/network/port_mapping.hpp"
#endif

#ifdef ENABLE_NETWORK_PORTS_ISOLATOR
#include "slave/containerizer/mesos/isolators/network/ports.hpp"
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
using process::Time;

using process::http::Connection;

using std::map;
using std::pair;
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
    GarbageCollector* gc,
    SecretResolver* secretResolver,
    const Option<NvidiaComponents>& nvidia,
    VolumeGidManager* volumeGidManager,
    PendingFutureTracker* futureTracker,
    CSIServer* csiServer)
{
  Try<hashset<string>> isolations = [&flags]() -> Try<hashset<string>> {
    const vector<string> tokens(strings::tokenize(flags.isolation, ","));
    hashset<string> isolations(set<string>(tokens.begin(), tokens.end()));

    if (tokens.size() != isolations.size()) {
      return Error("Duplicate entries found in --isolation flag '" +
                  stringify(tokens) + "'");
    }

    return isolations;
  }();

  if (isolations.isError()) {
    return Error(isolations.error());
  }

  const hashmap<string, vector<string>> deprecations = {
#ifdef __WINDOWS__
    {"process", {"windows/cpu", "windows/mem"}},
#else
    {"process", {"posix/cpu", "posix/mem"}},
#endif // __WINDOWS__

#ifdef __linux__
    {"cgroups", {"cgroups/cpu", "cgroups/mem"}},
#endif // __linux__

    {"posix/disk", {"disk/du"}}
  };

  // Replace any deprecated isolator names with their current equivalents.
  foreachpair (const string& name,
               const vector<string>& replacements,
               deprecations) {
    if (isolations->contains(name)) {
      LOG(WARNING)
        << "The '" << name << "' isolation flag is deprecated, "
        << "please update your flags to"
        << " '--isolation=" << strings::join(",", replacements) << "'.";

      isolations->erase(name);

      foreach (const string& isolator, replacements) {
        isolations->insert(isolator);
      }
    }
  }

  // One and only one filesystem isolator is required. The filesystem
  // isolator is responsible for preparing the filesystems for
  // containers (e.g., prepare filesystem roots, volumes, etc.). If
  // the user does not specify one, 'filesystem/posix' (or
  // 'filesystem/windows' on Windows) will be used.
  switch (std::count_if(
      isolations->begin(),
      isolations->end(),
      [](const string& s) {
        return strings::startsWith(s, "filesystem/");
      })) {
    case 0:
#ifdef __WINDOWS__
      isolations->insert("filesystem/windows");
#else
      isolations->insert("filesystem/posix");
#endif // __WINDOWS__
      break;

    case 1:
      break;

    default:
      return Error(
          "Using multiple filesystem isolators simultaneously is disallowed");
  }

#ifdef __linux__

  // The network isolator is responsible for preparing the network
  // namespace for containers. In general, one and only one `network`
  // isolator is required (e.g. `network/cni` and `network/port_mapping`
  // cannot co-exist). However, since the `network/ports` isolator
  // only deals with ports resources and does not configure any network
  // namespaces, it doesn't count for the purposes of this check.
  switch (std::count_if(
      isolations->begin(),
      isolations->end(),
      [](const string& s) {
        return strings::startsWith(s, "network/") && s != "network/ports";
      })) {
    case 0:
      // If the user does not specify anything, the 'network/cni'
      // isolator will be used.
      isolations->insert("network/cni");
      break;

    case 1:
      break;

    default:
      return Error(
          "Using multiple network isolators simultaneously is disallowed");
  }

  if (isolations->contains("filesystem/linux")) {
    // Always enable 'volume/image', 'volume/host_path',
    // 'volume/sandbox_path' on linux if 'filesystem/linux' is enabled
    // for backwards compatibility.
    isolations->insert("volume/image");
    isolations->insert("volume/host_path");
    isolations->insert("volume/sandbox_path");
  }
#endif // __linux__

  // Always add environment secret isolator.
  isolations->insert("environment_secret");

#ifdef __linux__
  if (flags.image_providers.isSome()) {
    // The 'filesystem/linux' isolator and 'linux' launcher are required
    // for the mesos containerizer to support container images.
    if (!isolations->contains("filesystem/linux")) {
      return Error("The 'filesystem/linux' isolator must be enabled for"
                   " container image support");
    }

    if (flags.launcher != "linux") {
      return Error("The 'linux' launcher must be used for container"
                   " image support");
    }
  }
#endif // __linux__

  LOG(INFO) << "Using isolation " << stringify(isolations.get());

  // Create the launcher for the MesosContainerizer.
  Try<Launcher*> _launcher = [&flags]() -> Try<Launcher*> {
#ifdef __linux__
    if (flags.launcher == "linux") {
      return LinuxLauncher::create(flags);
    } else if (flags.launcher == "posix") {
      return SubprocessLauncher::create(flags);
    } else {
      return Error("Unknown or unsupported launcher: " + flags.launcher);
    }
#elif defined(__WINDOWS__)
    if (flags.launcher != "windows") {
      return Error("Unsupported launcher: " + flags.launcher);
    }

    return SubprocessLauncher::create(flags);
#else
    if (flags.launcher != "posix") {
      return Error("Unsupported launcher: " + flags.launcher);
    }

    return SubprocessLauncher::create(flags);
#endif // __linux__
  }();

  if (_launcher.isError()) {
    return Error("Failed to create launcher: " + _launcher.error());
  }

  Owned<Launcher> launcher = Owned<Launcher>(_launcher.get());

  if (futureTracker != nullptr) {
    launcher = Owned<Launcher>(new LauncherTracker(launcher, futureTracker));
  }

  Try<Owned<Provisioner>> _provisioner =
    Provisioner::create(flags, secretResolver);

  if (_provisioner.isError()) {
    return Error("Failed to create provisioner: " + _provisioner.error());
  }

  Shared<Provisioner> provisioner = _provisioner->share();

  // Built-in isolator definitions.
  //
  // The order of the entries in this table specifies the ordering of the
  // isolators. Specifically, the `create` and `prepare` calls for each
  // isolator are run serially in the order in which they appear in the
  // table, while the `cleanup` call is serialized in reverse order.
  //
  // The ordering of isolators below is:
  //  - filesystem
  //  - runtime (ie. resources, etc)
  //  - volumes
  //  - disk
  //  - gpu
  //  - network
  //  - miscellaneous
  const vector<pair<string, lambda::function<Try<Isolator*>(const Flags&)>>>
    creators = {
    // Filesystem isolators.

#ifdef __WINDOWS__
    {"filesystem/windows",
      [volumeGidManager] (const Flags& flags) -> Try<Isolator*> {
        return WindowsFilesystemIsolatorProcess::create(
            flags,
            volumeGidManager);
      }},
#else
    {"filesystem/posix",
      [volumeGidManager] (const Flags& flags) -> Try<Isolator*> {
        return PosixFilesystemIsolatorProcess::create(
            flags,
            volumeGidManager);
      }},
#endif // __WINDOWS__

#ifdef __linux__
    {"filesystem/linux",
      [volumeGidManager] (const Flags& flags) -> Try<Isolator*> {
        return LinuxFilesystemIsolatorProcess::create(
            flags,
            volumeGidManager);
      }},

    // TODO(jieyu): Deprecate this in favor of using filesystem/linux.
    {"filesystem/shared", &SharedFilesystemIsolatorProcess::create},
#endif // __linux__

    // Runtime isolators.

#ifndef __WINDOWS__
    {"posix/cpu", &PosixCpuIsolatorProcess::create},
    {"posix/mem", &PosixMemIsolatorProcess::create},
    {"posix/rlimits", &PosixRLimitsIsolatorProcess::create},
#endif // __WINDOWS__

#ifdef __WINDOWS__
    {"windows/cpu", &WindowsCpuIsolatorProcess::create},
    {"windows/mem", &WindowsMemIsolatorProcess::create},
#endif // __WINDOWS__

#ifdef __linux__
    {"cgroups/all", &CgroupsIsolatorProcess::create},
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

    {"linux/devices", &LinuxDevicesIsolatorProcess::create},
    {"linux/capabilities", &LinuxCapabilitiesIsolatorProcess::create},
    {"linux/nnp", &LinuxNNPIsolatorProcess::create},

    {"namespaces/ipc", &NamespacesIPCIsolatorProcess::create},
    {"namespaces/pid", &NamespacesPidIsolatorProcess::create},
#endif // __linux__

    // Volume isolators.

#ifndef __WINDOWS__
    {"volume/sandbox_path",
      [volumeGidManager] (const Flags& flags) -> Try<Isolator*> {
        return VolumeSandboxPathIsolatorProcess::create(
            flags,
            volumeGidManager);
      }},
#endif // __WINDOWS__

#ifdef __linux__
    {"docker/volume", &DockerVolumeIsolatorProcess::create},
    {"volume/host_path", &VolumeHostPathIsolatorProcess::create},
    {"volume/image",
      [&provisioner] (const Flags& flags) -> Try<Isolator*> {
        return VolumeImageIsolatorProcess::create(flags, provisioner);
      }},

    {"volume/secret",
      [secretResolver] (const Flags& flags) -> Try<Isolator*> {
        return VolumeSecretIsolatorProcess::create(flags, secretResolver);
      }},

    {"volume/csi",
      [csiServer] (const Flags& flags) -> Try<Isolator*> {
        return VolumeCSIIsolatorProcess::create(flags, csiServer);
      }},
#endif // __linux__

    // Disk isolators.

#ifndef __WINDOWS__
    {"disk/du", &PosixDiskIsolatorProcess::create},
#endif // !__WINDOWS__

#if ENABLE_XFS_DISK_ISOLATOR
    {"disk/xfs", &XfsDiskIsolatorProcess::create},
#endif // ENABLE_XFS_DISK_ISOLATOR

#if ENABLE_SECCOMP_ISOLATOR
    {"linux/seccomp", &LinuxSeccompIsolatorProcess::create},
#endif // ENABLE_SECCOMP_ISOLATOR

    // GPU isolators.

#ifdef __linux__
    // The 'gpu/nvidia' isolator must be created after all volume
    // isolators, so that the nvidia gpu libraries '/usr/local/nvidia'
    // will not be overwritten.
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
#endif // __linux__

    // Network isolators.

#ifdef __linux__
    {"network/cni", &NetworkCniIsolatorProcess::create},
#endif // __linux__

#ifdef ENABLE_PORT_MAPPING_ISOLATOR
    {"network/port_mapping", &PortMappingIsolatorProcess::create},
#endif

#ifdef ENABLE_NETWORK_PORTS_ISOLATOR
    {"network/ports", &NetworkPortsIsolatorProcess::create},
#endif

    // Secrets isolators.

    {"environment_secret",
      [secretResolver] (const Flags& flags) -> Try<Isolator*> {
        return EnvironmentSecretIsolatorProcess::create(flags, secretResolver);
      }},
  };

  vector<Owned<Isolator>> isolators;

  // Note: For cgroups, we only create `CgroupsIsolatorProcess` once.
  // We use this flag to identify whether `CgroupsIsolatorProcess` has
  // been created or not.
  bool cgroupsIsolatorCreated = false;

  // First, apply the built-in isolators, in dependency order.
  foreach (const auto& creator, creators) {
    if (!isolations->contains(creator.first)) {
      continue;
    }

    if (strings::startsWith(creator.first, "cgroups/")) {
      if (cgroupsIsolatorCreated) {
        // Skip when `CgroupsIsolatorProcess` have already been created.
        continue;
      }

      cgroupsIsolatorCreated = true;
    }

    Try<Isolator*> _isolator = creator.second(flags);
    if (_isolator.isError()) {
      return Error("Failed to create isolator '" + creator.first + "': " +
                   _isolator.error());
    }

    Owned<Isolator> isolator(_isolator.get());

    if (futureTracker != nullptr) {
      isolator = Owned<Isolator>(
          new IsolatorTracker(isolator, creator.first, futureTracker));
    }

    isolators.push_back(isolator);
  }

  // Next, apply any custom isolators in the order given by the flags.
  foreach (const string& name, strings::tokenize(flags.isolation, ",")) {
    if (ModuleManager::contains<Isolator>(name)) {
      Try<Isolator*> _isolator = ModuleManager::create<Isolator>(name);

      if (_isolator.isError()) {
        return Error("Failed to create isolator '" + name + "': " +
                    _isolator.error());
      }

      Owned<Isolator> isolator(_isolator.get());

      if (futureTracker != nullptr) {
        isolator = Owned<Isolator>(
            new IsolatorTracker(isolator, name, futureTracker));
      }

      isolators.push_back(isolator);
      continue;
    }

    if (deprecations.contains(name)) {
      continue;
    }

    if (std::find_if(
        creators.begin(),
        creators.end(),
        [&name] (const decltype(creators)::value_type& creator) {
          return creator.first == name;
        }) != creators.end()) {
      continue;
    }

    return Error("Unknown or unsupported isolator '" + name + "'");
  }

  return MesosContainerizer::create(
      flags,
      local,
      fetcher,
      gc,
      launcher,
      provisioner,
      isolators,
      volumeGidManager);
}


Try<MesosContainerizer*> MesosContainerizer::create(
    const Flags& flags,
    bool local,
    Fetcher* fetcher,
    GarbageCollector* gc,
    const Owned<Launcher>& launcher,
    const Shared<Provisioner>& provisioner,
    const vector<Owned<Isolator>>& isolators,
    VolumeGidManager* volumeGidManager)
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

  Option<int_fd> initMemFd;
  Option<int_fd> commandExecutorMemFd;

#ifdef ENABLE_LAUNCHER_SEALING
  // Clone the launcher binary in memory for security concerns.
  Try<int_fd> memFd = memfd::cloneSealedFile(
      path::join(flags.launcher_dir, MESOS_CONTAINERIZER));

  if (memFd.isError()) {
    return Error(
        "Failed to clone a sealed file '" +
        path::join(flags.launcher_dir, MESOS_CONTAINERIZER) + "' in memory: " +
        memFd.error());
  }

  initMemFd = memFd.get();

  // Clone the command executor binary in memory for security.
  memFd = memfd::cloneSealedFile(
      path::join(flags.launcher_dir, MESOS_EXECUTOR));

  if (memFd.isError()) {
    return Error(
        "Failed to clone a sealed file '" +
        path::join(flags.launcher_dir, MESOS_EXECUTOR) + "' in memory: " +
        memFd.error());
  }

  commandExecutorMemFd = memFd.get();
#endif // ENABLE_LAUNCHER_SEALING

  return new MesosContainerizer(Owned<MesosContainerizerProcess>(
      new MesosContainerizerProcess(
          flags,
          fetcher,
          gc,
          ioSwitchboard.get(),
          launcher,
          provisioner,
          _isolators,
          volumeGidManager,
          initMemFd,
          commandExecutorMemFd)));
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


Future<Containerizer::LaunchResult> MesosContainerizer::launch(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig,
    const map<string, string>& environment,
    const Option<std::string>& pidCheckpointPath)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::launch,
                  containerId,
                  containerConfig,
                  environment,
                  pidCheckpointPath);
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
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::update,
                  containerId,
                  resourceRequests,
                  resourceLimits);
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


Future<Option<ContainerTermination>> MesosContainerizer::destroy(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::destroy,
                  containerId,
                  None());
}


Future<bool> MesosContainerizer::kill(
    const ContainerID& containerId,
    int signal)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::kill,
                  containerId,
                  signal);
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


Future<Nothing> MesosContainerizer::pruneImages(
    const vector<Image>& excludedImages)
{
  return dispatch(
      process.get(), &MesosContainerizerProcess::pruneImages, excludedImages);
}


Future<Nothing> MesosContainerizerProcess::recover(
    const Option<state::SlaveState>& state)
{
  LOG(INFO) << "Recovering Mesos containers";

  // Gather the container states that we will attempt to recover.
  vector<ContainerState> recoverable;
  if (state.isSome()) {
    // Gather the latest run of checkpointed executors.
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
        CHECK_SOME(run->id);

        // We need the pid so the reaper can monitor the executor so
        // skip this executor if it's not present. This is not an
        // error because the slave will try to wait on the container
        // which will return a failed ContainerTermination and
        // everything will get cleaned up.
        if (!run->forkedPid.isSome()) {
          continue;
        }

        if (run->completed) {
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
            state->id,
            framework.id,
            executor.id,
            containerId);

        CHECK(os::exists(directory));

        ContainerState executorRunState =
          protobuf::slave::createContainerState(
              executorInfo,
              None(),
              run->id.get(),
              run->forkedPid.get(),
              directory);

        recoverable.push_back(executorRunState);
      }
    }
  }

  // Recover the containers from 'SlaveState'.
  foreach (ContainerState& state, recoverable) {
    const ContainerID& containerId = state.container_id();

    // Contruct the structure for containers from the 'SlaveState'
    // first, to maintain the children list in the container.
    Owned<Container> container(new Container());
    container->status = reap(containerId, static_cast<pid_t>(state.pid()));

    // We only checkpoint the containerizer pid after the container
    // successfully launched, therefore we can assume checkpointed
    // containers should be running after recover.
    container->state = RUNNING;
    container->pid = static_cast<pid_t>(state.pid());
    container->directory = state.directory();

    // Attempt to read the launch config of the container.
    Result<ContainerConfig> config =
      containerizer::paths::getContainerConfig(flags.runtime_dir, containerId);

    if (config.isError()) {
      return Failure(
        "Failed to get config for container " + stringify(containerId) +
        ": " + config.error());
    }

    if (config.isSome()) {
      container->config = config.get();

      // Copy the ephemeral volume paths to the ContainerState, since this
      // information is otherwise only available to the prepare() callback.
      *state.mutable_ephemeral_volumes() = config->ephemeral_volumes();
    } else {
      VLOG(1) << "No config is recovered for container " << containerId
              << ", this means image pruning will be disabled.";
    }

    containers_[containerId] = container;
  }

  // TODO(gilbert): Draw the logic VENN Diagram here in comment.
  hashset<ContainerID> orphans;

  // Recover the containers from the runtime directory.
  //
  // NOTE: The returned vector guarantees that parent containers
  // will always appear before their child containers (if any).
  // This is particularly important for containers nested underneath
  // standalone containers, because standalone containers are only
  // added to the list of recoverable containers in the following loop,
  // whereas normal parent containers are added in the prior loop.
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
    if (containers_.contains(containerId)) {
      continue;
    }

    // Determine the sandbox if this is a nested or standalone container.
    const bool isStandaloneContainer =
      containerizer::paths::isStandaloneContainer(
          flags.runtime_dir, containerId);

    const ContainerID& rootContainerId =
      protobuf::getRootContainerId(containerId);

    Option<string> directory;
    if (containerId.has_parent()) {
      CHECK(containers_.contains(rootContainerId));

      if (containers_[rootContainerId]->directory.isSome()) {
        directory = containerizer::paths::getSandboxPath(
            containers_[rootContainerId]->directory.get(),
            containerId);
      }
    } else if (isStandaloneContainer) {
      directory = slave::paths::getContainerPath(flags.work_dir, containerId);
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
      CHECK(containerId.has_parent());

      // Schedule the sandbox of the terminated nested container for garbage
      // collection. Containers that exited while the agent was offline
      // (i.e. before the termination file was checkpointed) will be GC'd
      // after recovery.
      if (flags.gc_non_executor_container_sandboxes &&
          directory.isSome() &&
          os::exists(directory.get())) {
        // TODO(josephw): Should we also GC the runtime directory?
        // This has the downside of potentially wiping out the exit status
        // of the container when disk space is low.
        garbageCollect(directory.get());
      }

      continue;
    }

    // TODO(josephw): Schedule GC for standalone containers.
    // We currently delete the runtime directory of standalone containers
    // upon exit, which means there is no record of the sandbox directory to GC.

    // Attempt to read the pid from the container runtime directory.
    Result<pid_t> pid =
      containerizer::paths::getContainerPid(flags.runtime_dir, containerId);

    if (pid.isError()) {
      return Failure("Failed to get container pid: " + pid.error());
    }

    // Attempt to read the launch config of the container.
    Result<ContainerConfig> config =
      containerizer::paths::getContainerConfig(flags.runtime_dir, containerId);

    if (config.isError()) {
      return Failure("Failed to get container config: " + config.error());
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

    if (config.isSome()) {
      container->config = ContainerConfig();
      container->config->CopyFrom(config.get());
    } else {
      VLOG(1) << "No checkpointed config recovered for container "
              << containerId << ", this means image pruning will "
              << "be disabled.";
    }

    containers_[containerId] = container;

    // TODO(klueska): The final check in the if statement makes sure
    // that this container was not marked for forcible destruction on
    // recover. We currently only support 'destroy-on-recovery'
    // semantics for nested `DEBUG` containers. If we ever support it
    // on other types of containers, we may need duplicate this logic
    // elsewhere.
    const bool isRecoverableNestedContainer =
      containerId.has_parent() &&
      containers_.contains(rootContainerId) &&
      !orphans.contains(rootContainerId) &&
      pid.isSome() &&
      !containerizer::paths::getContainerForceDestroyOnRecovery(
          flags.runtime_dir, containerId);

    const bool isRecoverableStandaloneContainer =
      isStandaloneContainer && pid.isSome();

    // Add recoverable nested containers or standalone containers
    // to the list of 'ContainerState'.
    if (isRecoverableNestedContainer || isRecoverableStandaloneContainer) {
      CHECK_SOME(container->directory);
      CHECK_SOME(container->pid);

      ContainerState state =
        protobuf::slave::createContainerState(
            None(),
            config.isSome() && config->has_container_info() ?
                Option<ContainerInfo>(config->container_info()) :
                Option<ContainerInfo>::none(),
            containerId,
            container->pid.get(),
            container->directory.get());

      if (config.isSome()) {
        // Copy the ephemeral volume paths to the ContainerState, since this
        // information is otherwise only available to the prepare() callback.
        *state.mutable_ephemeral_volumes() = config->ephemeral_volumes();
      }

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
    const vector<ContainerState>& recoverable,
    const hashset<ContainerID>& orphans)
{
  // Recover isolators first then recover the provisioner, because of
  // possible cleanups on unknown containers.
  return recoverIsolators(recoverable, orphans)
    .then(defer(self(), &Self::recoverProvisioner, recoverable, orphans))
    .then(defer(self(), &Self::__recover, recoverable, orphans));
}


Future<vector<Nothing>> MesosContainerizerProcess::recoverIsolators(
    const vector<ContainerState>& recoverable,
    const hashset<ContainerID>& orphans)
{
  LOG(INFO) << "Recovering isolators";

  vector<Future<Nothing>> futures;

  // Then recover the isolators.
  foreach (const Owned<Isolator>& isolator, isolators) {
    vector<ContainerState> _recoverable;
    hashset<ContainerID> _orphans;

    foreach (const ContainerState& state, recoverable) {
      if (isSupportedByIsolator(
              state.container_id(),
              isolator->supportsNesting(),
              isolator->supportsStandalone())) {
        _recoverable.push_back(state);
      }
    }

    foreach (const ContainerID& orphan, orphans) {
      if (isSupportedByIsolator(
              orphan,
              isolator->supportsNesting(),
              isolator->supportsStandalone())) {
        _orphans.insert(orphan);
      }
    }

    futures.push_back(isolator->recover(_recoverable, _orphans));
  }

  // If all isolators recover then continue.
  return collect(futures);
}


Future<Nothing> MesosContainerizerProcess::recoverProvisioner(
    const vector<ContainerState>& recoverable,
    const hashset<ContainerID>& orphans)
{
  LOG(INFO) << "Recovering provisioner";

  // TODO(gilbert): Consolidate 'recoverProvisioner()' interface
  // once the launcher returns a full set of known containers.
  hashset<ContainerID> knownContainerIds = orphans;

  foreach (const ContainerState& state, recoverable) {
    knownContainerIds.insert(state.container_id());
  }

  return provisioner->recover(knownContainerIds);
}


Future<Nothing> MesosContainerizerProcess::__recover(
    const vector<ContainerState>& recovered,
    const hashset<ContainerID>& orphans)
{
  // Recover containers' launch information.
  foreach (const ContainerState& run, recovered) {
    const ContainerID& containerId = run.container_id();

    // Attempt to read container's launch information.
    Result<ContainerLaunchInfo> containerLaunchInfo =
      containerizer::paths::getContainerLaunchInfo(
          flags.runtime_dir, containerId);

    if (containerLaunchInfo.isError()) {
      return Failure(
          "Failed to recover launch information of container " +
          stringify(containerId) + ": " + containerLaunchInfo.error());
    }

    if (containerLaunchInfo.isSome()) {
      containers_[containerId]->launchInfo = containerLaunchInfo.get();
    }
  }

  foreach (const ContainerState& run, recovered) {
    const ContainerID& containerId = run.container_id();

    foreach (const Owned<Isolator>& isolator, isolators) {
      if (!isSupportedByIsolator(
              containerId,
              isolator->supportsNesting(),
              isolator->supportsStandalone())) {
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
    destroy(containerId, None());
  }

  return Nothing();
}


// Launching an container involves the following steps:
// 1. Call prepare on each isolator.
// 2. Fork a helper process. The forked helper is blocked from exec'ing
//    until it has been isolated.
// 3. Isolate the helper's pid; e.g. call `isolate` for each isolator.
// 4. Fetch any URIs.
// 5. Signal the helper process to continue. It will first execute any
//    preparation commands from isolators and then exec the starting command.
Future<Containerizer::LaunchResult> MesosContainerizerProcess::launch(
    const ContainerID& containerId,
    const ContainerConfig& _containerConfig,
    const map<string, string>& environment,
    const Option<std::string>& pidCheckpointPath)
{
  if (containers_.contains(containerId)) {
    return Containerizer::LaunchResult::ALREADY_LAUNCHED;
  }

  if (_containerConfig.has_container_info() &&
      _containerConfig.container_info().type() != ContainerInfo::MESOS) {
    return Containerizer::LaunchResult::NOT_SUPPORTED;
  }

  // NOTE: We make a copy of the ContainerConfig because we may need
  // to modify it based on the parent container (for nested containers).
  ContainerConfig containerConfig = _containerConfig;

  // For nested containers, we must perform some extra validation (i.e. does
  // the parent exist?), inherit user from parent if needed and create the
  // sandbox directory based on the parent's sandbox.
  if (containerId.has_parent()) {
    if (containerConfig.has_task_info() ||
        containerConfig.has_executor_info()) {
      return Failure(
          "Nested containers may not supply a TaskInfo/ExecutorInfo");
    }

    if (pidCheckpointPath.isSome()) {
      return Failure("Nested containers may not be checkpointed");
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

    // Inherit user from the parent container iff there is no
    // user specified in the nested container's `commandInfo`.
    if (!containerConfig.has_user() &&
        containers_[parentContainerId]->config.isSome() &&
        containers_[parentContainerId]->config->has_user()) {
      containerConfig.set_user(containers_[parentContainerId]->config->user());
    }

    const ContainerID rootContainerId =
      protobuf::getRootContainerId(containerId);

    CHECK(containers_.contains(rootContainerId));
    if (containers_[rootContainerId]->directory.isNone()) {
      return Failure(
          "Unexpected empty sandbox directory for root container " +
          stringify(rootContainerId));
    }

    const string directory = containerizer::paths::getSandboxPath(
        containers_[rootContainerId]->directory.get(),
        containerId);

    if (containerConfig.has_user()) {
      LOG_BASED_ON_CLASS(containerConfig.container_class())
        << "Creating sandbox '" << directory << "'"
        << " for user '" << containerConfig.user() << "'";
    } else {
      LOG_BASED_ON_CLASS(containerConfig.container_class())
        << "Creating sandbox '" << directory << "'";
    }

    Try<Nothing> mkdir = slave::paths::createSandboxDirectory(
        directory,
        containerConfig.has_user() ? Option<string>(containerConfig.user())
                                   : Option<string>::none());

    if (mkdir.isError()) {
      return Failure(
          "Failed to create nested sandbox '" +
          directory + "': " + mkdir.error());
    }

    // Modify the sandbox directory in the ContainerConfig.
    // TODO(josephw): Should we validate that this value
    // is not set for nested containers?
    containerConfig.set_directory(directory);

    // TODO(jieyu): This is currently best effort. After the agent fails
    // over, 'executor_info' won't be set in root parent container's
    // 'config'. Consider populating 'executor_info' in recover path.
    if (containers_[rootContainerId]->config.isSome()) {
      if (containers_[rootContainerId]->config->has_executor_info()) {
        containerConfig.mutable_executor_info()->CopyFrom(
          containers_[rootContainerId]->config->executor_info());
      }
    } else {
      LOG(WARNING) << "Cannot determine executor_info for root container '"
                   << rootContainerId << "' which has no config recovered.";
    }
  }

  LOG_BASED_ON_CLASS(containerConfig.container_class())
    << "Starting container " << containerId;

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

  // If we are launching a standalone container, checkpoint a file to
  // mark it as a standalone container. Nested containers launched
  // under a standalone container are treated as nested containers
  // (_not_ as both standalone and nested containers).
  if (!containerId.has_parent() &&
      !containerConfig.has_task_info() &&
      !containerConfig.has_executor_info()) {
    const string path =
      containerizer::paths::getStandaloneContainerMarkerPath(
          flags.runtime_dir, containerId);

    Try<Nothing> checkpointed = slave::state::checkpoint(path, "");
    if (checkpointed.isError()) {
      return Failure(
          "Failed to checkpoint file to mark container as standalone");
    }
  }

  Owned<Container> container(new Container());
  container->config = containerConfig;
  container->resourceRequests = containerConfig.resources();
  container->resourceLimits = containerConfig.limits();
  container->directory = containerConfig.directory();

  // Maintain the 'children' list in the parent's 'Container' struct,
  // which will be used for recursive destroy.
  if (containerId.has_parent()) {
    CHECK(containers_.contains(containerId.parent()));
    containers_[containerId.parent()]->children.insert(containerId);
  }

  containers_.put(containerId, container);
  transition(containerId, PROVISIONING);

  Future<Nothing> _prepare;

  // We'll first provision the image for the container, and
  // then provision the images specified in `volumes` using
  // the 'volume/image' isolator.
  if (!containerConfig.has_container_info() ||
      !containerConfig.container_info().mesos().has_image()) {
    _prepare = prepare(containerId, None());
  } else {
    container->provisioning = provisioner->provision(
      containerId,
      containerConfig.container_info().mesos().image());

    _prepare = container->provisioning
      .then(defer(self(), [=](const ProvisionInfo& provisionInfo) {
        return prepare(containerId, provisionInfo);
      }));
  }

  return _prepare
    .then(defer(self(), [this, containerId] () {
      return ioSwitchboard->extractContainerIO(containerId);
    }))
    .then(defer(self(), [=](const Option<ContainerIO>& containerIO) {
      return _launch(containerId, containerIO, environment, pidCheckpointPath);
    }))
    .onAny(defer(self(), [this, containerId](
        const Future<Containerizer::LaunchResult>& future) {
      // We need to clean up the container IO in the case when IOSwitchboard
      // process has started, but we have not taken ownership of the container
      // IO by calling `extractContainerIO()`. This may happen if `launch`
      // future is discarded by the caller of this method. The container IO
      // stores FDs in the `FDWrapper` struct, which closes these FDs on its
      // destruction. Otherwise, IOSwitchboard might get stuck trying to read
      // leaked FDs.
      ioSwitchboard->extractContainerIO(containerId);
    }));
}


Future<Nothing> MesosContainerizerProcess::prepare(
    const ContainerID& containerId,
    const Option<ProvisionInfo>& provisionInfo)
{
  // This is because if a 'destroy' happens during the provisioner is
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
  CHECK_SOME(container->config);

  if (provisionInfo.isSome()) {
    container->config->set_rootfs(provisionInfo->rootfs);

    if (provisionInfo->ephemeralVolumes.isSome()) {
      foreach (const Path& path, provisionInfo->ephemeralVolumes.get()) {
        container->config->add_ephemeral_volumes(path);
      }
    }

    if (provisionInfo->dockerManifest.isSome() &&
        provisionInfo->appcManifest.isSome()) {
      return Failure("Container cannot have both Docker and Appc manifests");
    }

    if (provisionInfo->dockerManifest.isSome()) {
      ContainerConfig::Docker* docker = container->config->mutable_docker();
      docker->mutable_manifest()->CopyFrom(provisionInfo->dockerManifest.get());
    }

    if (provisionInfo->appcManifest.isSome()) {
      ContainerConfig::Appc* appc = container->config->mutable_appc();
      appc->mutable_manifest()->CopyFrom(provisionInfo->appcManifest.get());
    }
  }

  // Captured for lambdas below.
  ContainerConfig containerConfig = container->config.get();

  // Checkpoint the `ContainerConfig` which includes all information to launch a
  // container. Critical information (e.g., `ContainerInfo`) can be used for
  // tracking container image usage.
  const string configPath = path::join(
      containerizer::paths::getRuntimePath(flags.runtime_dir, containerId),
      containerizer::paths::CONTAINER_CONFIG_FILE);

  Try<Nothing> configCheckpointed =
    slave::state::checkpoint(configPath, containerConfig);

  if (configCheckpointed.isError()) {
    return Failure("Failed to checkpoint the container config to '" +
                   configPath + "': " + configCheckpointed.error());
  }

  VLOG(1) << "Checkpointed ContainerConfig at '" << configPath << "'";

  transition(containerId, PREPARING);

  // We prepare the isolators sequentially according to their ordering
  // to permit basic dependency specification, e.g., preparing a
  // filesystem isolator before other isolators.
  Future<vector<Option<ContainerLaunchInfo>>> f =
    vector<Option<ContainerLaunchInfo>>();

  foreach (const Owned<Isolator>& isolator, isolators) {
    if (!isSupportedByIsolator(
            containerId,
            isolator->supportsNesting(),
            isolator->supportsStandalone())) {
      continue;
    }

    // Chain together preparing each isolator.
    f = f.then([=](vector<Option<ContainerLaunchInfo>> launchInfos) {
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
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container destroyed during isolating");
  }

  const Owned<Container>& container = containers_.at(containerId);

  if (container->state == DESTROYING) {
    return Failure("Container is being destroyed during isolating");
  }

  CHECK_EQ(container->state, ISOLATING);

  transition(containerId, FETCHING);

  CHECK_SOME(container->config);

  const string directory = container->config->directory();

  return fetcher->fetch(
      containerId,
      container->config->command_info(),
      directory,
      container->config->has_user()
        ? container->config->user()
        : Option<string>::none())
    .then([=]() -> Future<Nothing> {
      if (HookManager::hooksAvailable()) {
        HookManager::slavePostFetchHook(containerId, directory);
      }
      return Nothing();
    });
}


Future<Containerizer::LaunchResult> MesosContainerizerProcess::_launch(
    const ContainerID& containerId,
    const Option<ContainerIO>& containerIO,
    const map<string, string>& environment,
    const Option<std::string>& pidCheckpointPath)
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
  CHECK_SOME(container->config);

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

    if (isolatorLaunchInfo->has_effective_capabilities() &&
        launchInfo.has_effective_capabilities()) {
      return Failure("Multiple isolators specify effective capabilities");
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

  // Remove duplicated entries in supplementary groups.
  set<uint32_t> supplementaryGroups(
      launchInfo.supplementary_groups().begin(),
      launchInfo.supplementary_groups().end());

  launchInfo.clear_supplementary_groups();

  foreach (uint32_t gid, supplementaryGroups) {
    launchInfo.add_supplementary_groups(gid);
  }

  // Determine the launch command for the container.
  if (!launchInfo.has_command()) {
    launchInfo.mutable_command()->CopyFrom(container->config->command_info());
  } else {
    // For command tasks, merge the launch commands with the executor
    // launch command.
    if (container->config->has_task_info()) {
      // Isolators are not supposed to set any other fields in the
      // command except the arguments for the command executor.
      CHECK(launchInfo.command().uris().empty())
        << "Isolators mutate 'uris' in container launch command";
      CHECK(!launchInfo.command().has_environment())
        << "Isolators mutate 'environment' in container launch command";
      CHECK(!launchInfo.command().has_shell())
        << "Isolators mutate 'shell' in container launch command";
      CHECK(!launchInfo.command().has_value())
        << "Isolators mutate 'value' in container launch command";
      CHECK(!launchInfo.command().has_user())
        << "Isolators mutate 'user' in container launch command";

      // NOTE: The ordering here is important because we want the
      // command executor arguments to be in front of the arguments
      // set by isolators. See details in MESOS-7909.
      CommandInfo launchCommand = container->config->command_info();
      launchCommand.MergeFrom(launchInfo.command());
      launchInfo.mutable_command()->CopyFrom(launchCommand);
    }
  }

  // For command tasks specifically, we should add the task_environment
  // flag and the task_supplementary_groups flag to the launch command
  // of the command executor.
  // TODO(tillt): Remove this once we no longer support the old style
  // command task (i.e., that uses mesos-execute).
  if (container->config->has_task_info()) {
    if (launchInfo.has_task_environment()) {
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

    if (launchInfo.task_supplementary_groups_size() > 0) {
      // Remove duplicated entries in supplementary groups.
      set<uint32_t> taskSupplementaryGroups(
          launchInfo.task_supplementary_groups().begin(),
          launchInfo.task_supplementary_groups().end());

      launchInfo.mutable_command()->add_arguments(
          "--task_supplementary_groups=" +
          strings::join(",", taskSupplementaryGroups));
    }
  }

  // For the command executor case, we should add the rootfs flag to
  // the launch command of the command executor.
  // TODO(jieyu): Remove this once we no longer support the old style
  // command task (i.e., that uses mesos-execute).
  // TODO(jieyu): Consider move this to filesystem isolator.
  if (container->config->has_task_info() &&
      container->config->has_rootfs()) {
    launchInfo.mutable_command()->add_arguments(
        "--rootfs=" + container->config->rootfs());
  }

  // TODO(jieyu): 'uris', 'environment' and 'user' in the launch
  // command will be ignored. 'environment' and 'user' are set
  // explicitly in 'ContainerLaunchInfo'. In fact, the above fields
  // should be moved to TaskInfo or ExecutorInfo, instead of putting
  // them in CommandInfo.
  launchInfo.mutable_command()->clear_uris();
  launchInfo.mutable_command()->clear_environment();
  launchInfo.mutable_command()->clear_user();

  // Determine the environment for the command to be launched.
  //
  // For non-DEBUG containers, the priority of the environment is:
  //  1) User specified environment in CommandInfo.
  //  2) Environment returned by isolators (i.e., in 'launchInfo').
  //  3) Environment passed from agent (e.g., executor environment).
  //
  // DEBUG containers inherit parent's environment,
  // hence the priority of the environment is:
  //
  // 1) User specified environment in CommandInfo.
  // 2) Environment returned by isolators (i.e., in 'launchInfo').
  // 3) Environment passed from agent (e.g., executor environment).
  // 4) Environment inherited from the parent container.
  //
  // TODO(alexr): Consider using `hashmap` for merging environments to
  // avoid duplicates, because `MergeFrom()` appends to the list.
  Environment containerEnvironment;

  // Inherit environment from the parent container for DEBUG containers.
  if (container->containerClass() == ContainerClass::DEBUG) {
    // DEBUG containers must have a parent.
    CHECK(containerId.has_parent());
    if (containers_[containerId.parent()]->launchInfo.isSome()) {
      containerEnvironment.CopyFrom(
          containers_[containerId.parent()]->launchInfo->environment());
    }
  }

  // Include environment passed from agent.
  foreachpair (const string& key, const string& value, environment) {
    Environment::Variable* variable = containerEnvironment.add_variables();
    variable->set_name(key);
    variable->set_value(value);
  }

  // DEBUG containers inherit MESOS_SANDBOX from their parent.
  if (container->containerClass() == ContainerClass::DEFAULT) {
    // TODO(jieyu): Consider moving this to filesystem isolator.
    //
    // NOTE: For the command executor case, although it uses the host
    // filesystem for itself, we still set 'MESOS_SANDBOX' according to
    // the root filesystem of the task (if specified). Command executor
    // itself does not use this environment variable.
    Environment::Variable* variable = containerEnvironment.add_variables();
    variable->set_name("MESOS_SANDBOX");
    variable->set_value(container->config->has_rootfs()
      ? flags.sandbox_directory
      : container->config->directory());
  }

  // `launchInfo.environment` contains the environment returned by
  // isolators. `launchInfo.environment` will later be overwritten
  // by `containerEnvironment`, hence isolator environment will
  // contribute to the resulting container environment.
  containerEnvironment.MergeFrom(launchInfo.environment());

  // Include user specified environment.
  // Skip over any secrets as they should have been resolved by the
  // environment_secret isolator.
  if (container->config->command_info().has_environment()) {
    foreach (const Environment::Variable& variable,
             container->config->command_info().environment().variables()) {
      if (variable.type() != Environment::Variable::SECRET) {
        containerEnvironment.add_variables()->CopyFrom(variable);
      }
    }
  }

  // Set the aggregated environment of the launch command.
  launchInfo.mutable_environment()->CopyFrom(containerEnvironment);

  // Determine the rootfs for the container to be launched.
  //
  // NOTE: Command task is a special case. Even if the container
  // config has a root filesystem, the executor container still uses
  // the host filesystem.
  if (!container->config->has_task_info() &&
      container->config->has_rootfs()) {
    launchInfo.set_rootfs(container->config->rootfs());
  }

  // For a non-DEBUG container, working directory is set to container sandbox,
  // i.e., MESOS_SANDBOX, unless one of the isolators overrides it. DEBUG
  // containers set their working directory to their parent working directory,
  // with the command executor being a special case (see below).
  //
  // TODO(alexr): Determining working directory is a convoluted process. We
  // should either simplify the logic or extract it into a helper routine.
  if (container->containerClass() == ContainerClass::DEBUG) {
    // DEBUG containers must have a parent.
    CHECK(containerId.has_parent());

    if (containers_[containerId.parent()]->launchInfo.isSome()) {
      // TODO(alexr): Remove this once we no longer support executorless
      // command tasks in favor of default executor.
      //
      // The `ContainerConfig` may not exist for the parent container.
      // The check is necessary because before MESOS-6894, containers
      // do not have `ContainerConfig` checkpointed. For the upgrade
      // scenario, if any nested container is launched under an existing
      // legacy container, the agent would fail due to an unguarded access
      // to the parent legacy container's ContainerConfig. We need to add
      // this check. Please see MESOS-8325 for details.
      if (containers_[containerId.parent()]->config.isSome() &&
          containers_[containerId.parent()]->config->has_task_info()) {
        // For the command executor case, even if the task itself has a root
        // filesystem, the executor container still uses the host filesystem,
        // hence `ContainerLaunchInfo.working_directory`, which points to the
        // executor working directory in the host filesystem, may be different
        // from the task working directory when task defines an image. Fall back
        // to the sandbox directory if task working directory is not present.
        if (containers_[containerId.parent()]->config->has_rootfs()) {
          // We can extract the task working directory from the flag being
          // passed to the command executor.
          foreach (
              const string& flag,
              containers_[containerId.parent()]
                ->launchInfo->command().arguments()) {
            if (strings::startsWith(flag, "--working_directory=")) {
              launchInfo.set_working_directory(strings::remove(
                  flag, "--working_directory=", strings::PREFIX));
              break;
            }
          }

          // If "--working_directory" argument is not found, default to the
          // sandbox directory.
          if (!launchInfo.has_working_directory()) {
            launchInfo.set_working_directory(flags.sandbox_directory);
          }
        } else {
          // Parent is command executor, but the task does not define an image.
          launchInfo.set_working_directory(containers_[containerId.parent()]
            ->launchInfo->working_directory());
        }
      } else {
        // Parent is a non-command task.
        launchInfo.set_working_directory(
            containers_[containerId.parent()]->launchInfo->working_directory());
      }
    } else {
      // Working directory cannot be determined, because
      // parent working directory is unknown.
      launchInfo.clear_working_directory();
    }
  } else if (launchInfo.has_rootfs()) {
    // Non-DEBUG container which defines an image.
    if (!launchInfo.has_working_directory()) {
      launchInfo.set_working_directory(flags.sandbox_directory);
    }
  } else {
    // Non-DEBUG container which does not define an image.
    //
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

    launchInfo.set_working_directory(container->config->directory());
  }

  // Determine the user to launch the container as.
  if (container->config->has_user()) {
    launchInfo.set_user(container->config->user());
  }

  // TODO(gilbert): Remove this once we no longer support command
  // task in favor of default executor.
  if (container->config->has_task_info() &&
      container->config->has_rootfs()) {
    // We need to set the executor user as root as it needs to
    // perform chroot (even when switch_user is set to false).
    launchInfo.set_user("root");
  }

  // Store container's launch information for future access.
  container->launchInfo = launchInfo;

  // Checkpoint container's launch information.
  const string launchInfoPath =
    containerizer::paths::getContainerLaunchInfoPath(
        flags.runtime_dir, containerId);

  Try<Nothing> checkpointed = slave::state::checkpoint(
      launchInfoPath, launchInfo);

  if (checkpointed.isError()) {
    LOG(ERROR) << "Failed to checkpoint container's launch information to '"
               << launchInfoPath << "': " << checkpointed.error();

    return Failure("Could not checkpoint container's launch information: " +
                   checkpointed.error());
  }

  // Use a pipe to block the child until it's been isolated.
  // The `pipes` array is captured later in a lambda.
  //
  // TODO(jmlvanre): consider returning failure if `pipe` gives an
  // error. Currently we preserve the previous logic.
#ifdef __WINDOWS__
  // On Windows, we have to make the pipes inheritable and not overlapped, since
  // that's what the mesos container launcher assumes.
  Try<std::array<int_fd, 2>> pipes_ = os::pipe(false, false);
  CHECK_SOME(pipes_);

  foreach (const int_fd& fd, pipes_.get()) {
    Try<Nothing> result = ::internal::windows::set_inherit(fd, true);
    if (result.isError()) {
      return Failure(
          "Could not set pipe inheritance for launcher: " + result.error());
    }
  }
#else
  Try<std::array<int_fd, 2>> pipes_ = os::pipe();
  CHECK_SOME(pipes_);
#endif // __WINDOWS__

  const std::array<int_fd, 2>& pipes = pipes_.get();

  vector<int_fd> whitelistFds{pipes[0], pipes[1]};

  // Seal the command executor binary if needed.
  if (container->config->has_task_info() && commandExecutorMemFd.isSome()) {
    launchInfo.mutable_command()->set_value(
        "/proc/self/fd/" + stringify(commandExecutorMemFd.get()));

    whitelistFds.push_back(commandExecutorMemFd.get());
  }

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

    const Owned<Container>& parentContainer =
      containers_.at(containerId.parent());

    if (parentContainer->pid.isNone()) {
      return Failure("Unknown parent container pid");
    }

    const pid_t parentPid = parentContainer->pid.get();

    // For the command executor case, we need to find a PID of its task,
    // which will be used to enter the task's mount namespace.
    if (parentContainer->config.isSome() &&
        parentContainer->config->has_task_info()) {
      Try<pid_t> mountNamespaceTarget = getMountNamespaceTarget(parentPid);
      if (mountNamespaceTarget.isError()) {
        return Failure(
            "Cannot get target mount namespace from process " +
            stringify(parentPid) + ": " + mountNamespaceTarget.error());
      }

      launchFlags.namespace_mnt_target = mountNamespaceTarget.get();
    } else {
      launchFlags.namespace_mnt_target = parentPid;
    }

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
  string initPath = initMemFd.isSome()
    ? ("/proc/self/fd/" + stringify(initMemFd.get()))
    : path::join(flags.launcher_dir, MESOS_CONTAINERIZER);

  vector<string> argv(2);
  argv[0] = path::join(flags.launcher_dir, MESOS_CONTAINERIZER);
  argv[1] = MesosContainerizerLaunch::NAME;

  Try<pid_t> forked = launcher->fork(
      containerId,
      initPath,
      argv,
      containerIO.get(),
      nullptr,
      launchEnvironment,
      // 'enterNamespaces' will be ignored by SubprocessLauncher.
      _enterNamespaces,
      // 'cloneNamespaces' will be ignored by SubprocessLauncher.
      _cloneNamespaces,
      whitelistFds);

  if (forked.isError()) {
    os::close(pipes[0]);
    os::close(pipes[1]);

    return Failure("Failed to fork: " + forked.error());
  }

  pid_t pid = forked.get();
  container->pid = pid;

  // Checkpoint the forked pid if requested by the agent.
  if (pidCheckpointPath.isSome()) {
    LOG_BASED_ON_CLASS(container->containerClass())
      << "Checkpointing container's forked pid " << pid
      << " to '" << pidCheckpointPath.get() << "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(pidCheckpointPath.get(), stringify(pid));

    if (checkpointed.isError()) {
      LOG(ERROR) << "Failed to checkpoint container's forked pid to '"
                 << pidCheckpointPath.get() << "': " << checkpointed.error();

      os::close(pipes[0]);
      os::close(pipes[1]);

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

  checkpointed = slave::state::checkpoint(pidPath, stringify(pid));

  if (checkpointed.isError()) {
    os::close(pipes[0]);
    os::close(pipes[1]);

    return Failure("Failed to checkpoint the container pid to"
                   " '" + pidPath + "': " + checkpointed.error());
  }

  // Monitor the forked process's pid. We keep the future because
  // we'll refer to it again during container destroy.
  container->status = reap(containerId, pid);
  container->status->onAny(defer(self(), &Self::reaped, containerId));

  return isolate(containerId, pid)
    .then(defer(self(), &Self::fetch, containerId))
    .then(defer(self(), &Self::exec, containerId, pipes[1]))
    .onAny([pipes]() { os::close(pipes[0]); })
    .onAny([pipes]() { os::close(pipes[1]); });
}


Future<Nothing> MesosContainerizerProcess::isolate(
    const ContainerID& containerId,
    pid_t _pid)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container destroyed during preparing");
  }

  const Owned<Container>& container = containers_.at(containerId);

  if (container->state == DESTROYING) {
    return Failure("Container is being destroyed during preparing");
  }

  CHECK_EQ(container->state, PREPARING);

  transition(containerId, ISOLATING);

  // Set up callbacks for isolator limitations.
  foreach (const Owned<Isolator>& isolator, isolators) {
    if (!isSupportedByIsolator(
            containerId,
            isolator->supportsNesting(),
            isolator->supportsStandalone())) {
      continue;
    }

    isolator->watch(containerId)
      .onAny(defer(self(), &Self::limited, containerId, lambda::_1));
  }

  // Isolate the executor with each isolator.
  // NOTE: This is done is parallel and is not sequenced like prepare
  // or destroy because we assume there are no dependencies in
  // isolation.
  vector<Future<Nothing>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    if (!isSupportedByIsolator(
            containerId,
            isolator->supportsNesting(),
            isolator->supportsStandalone())) {
      continue;
    }

    futures.push_back(isolator->isolate(containerId, _pid));
  }

  // Wait for all isolators to complete.
  Future<vector<Nothing>> future = collect(futures);

  container->isolation = future;

  return future.then([]() { return Nothing(); });
}


Future<Containerizer::LaunchResult> MesosContainerizerProcess::exec(
    const ContainerID& containerId,
    int_fd pipeWrite)
{
  // The container may be destroyed before we exec the executor so
  // return failure here.
  if (!containers_.contains(containerId)) {
    return Failure("Container destroyed during fetching");
  }

  const Owned<Container>& container = containers_.at(containerId);

  if (container->state == DESTROYING) {
    return Failure("Container is being destroyed during fetching");
  }

  CHECK_EQ(container->state, FETCHING);

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

  transition(containerId, RUNNING);

  return Containerizer::LaunchResult::SUCCESS;
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

  // NOTE: Use 'undiscardable' here to make sure discard from the
  // caller does not propagate into 'termination.future()' which will
  // be used in 'destroy()'. We don't want a discard on 'wait()' call
  // to affect future calls to 'destroy()'. See more details in
  // MESOS-7926.
  return undiscardable(containers_.at(containerId)->termination.future())
    .then(Option<ContainerTermination>::some);
}


Future<Nothing> MesosContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
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
  container->resourceRequests = resourceRequests;
  container->resourceLimits = resourceLimits;

  // Update each isolator.
  vector<Future<Nothing>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    if (!isSupportedByIsolator(
            containerId,
            isolator->supportsNesting(),
            isolator->supportsStandalone())) {
      continue;
    }

    futures.push_back(
        isolator->update(containerId, resourceRequests, resourceLimits));
  }

  // Wait for all isolators to complete.
  return collect(futures)
    .then([]() { return Nothing(); });
}


Future<ResourceStatistics> _usage(
    const ContainerID& containerId,
    const Option<Resources>& resourceRequests,
    const Option<google::protobuf::Map<string, Value::Scalar>>& resourceLimits,
    bool enableCfsQuota,
    const vector<Future<ResourceStatistics>>& statistics)
{
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

  Option<double> cpuRequest, cpuLimit, memLimit;
  Option<Bytes> memRequest;

  if (resourceRequests.isSome()) {
    cpuRequest = resourceRequests->cpus();
    memRequest = resourceRequests->mem();
  }

  if (resourceLimits.isSome()) {
    foreach (auto&& limit, resourceLimits.get()) {
      if (limit.first == "cpus") {
        cpuLimit = limit.second.value();
      } else if (limit.first == "mem") {
        memLimit = limit.second.value();
      }
    }
  }

  if (cpuRequest.isSome()) {
    result.set_cpus_soft_limit(cpuRequest.get());
  }

  if (cpuLimit.isSome()) {
    // Get the total CPU numbers of this node, we will use it to set container's
    // hard CPU limit if the CPU limit specified by framework is infinity.
    static Option<long> totalCPUs;
    if (totalCPUs.isNone()) {
      Try<long> cpus = os::cpus();
      if (cpus.isError()) {
        return Failure(
            "Failed to auto-detect the number of cpus: " + cpus.error());
      }

      totalCPUs = cpus.get();
    }

    CHECK_SOME(totalCPUs);

    result.set_cpus_limit(
        std::isinf(cpuLimit.get()) ? totalCPUs.get() : cpuLimit.get());
  } else if (enableCfsQuota && cpuRequest.isSome()) {
    result.set_cpus_limit(cpuRequest.get());
  }

  if (memRequest.isSome()) {
    result.set_mem_soft_limit_bytes(memRequest->bytes());
  }

  if (memLimit.isSome()) {
    // Get the total memory of this node, we will use it to set container's hard
    // memory limit if the memory limit specified by framework is infinity.
    static Option<Bytes> totalMem;
    if (totalMem.isNone()) {
      Try<os::Memory> mem = os::memory();
      if (mem.isError()) {
        return Failure(
            "Failed to auto-detect the size of main memory: " + mem.error());
      }

      totalMem = mem->total;
    }

    CHECK_SOME(totalMem);

    result.set_mem_limit_bytes(
        std::isinf(memLimit.get())
          ? totalMem->bytes()
          : Megabytes(static_cast<uint64_t>(memLimit.get())).bytes());
  } else if (memRequest.isSome()) {
    result.set_mem_limit_bytes(memRequest->bytes());
  }

  return result;
}


Future<ResourceStatistics> MesosContainerizerProcess::usage(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Unknown container " + stringify(containerId));
  }

  vector<Future<ResourceStatistics>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    if (!isSupportedByIsolator(
            containerId,
            isolator->supportsNesting(),
            isolator->supportsStandalone())) {
      continue;
    }

    futures.push_back(isolator->usage(containerId));
  }

  Option<Resources> resourceRequests;
  Option<google::protobuf::Map<string, Value::Scalar>> resourceLimits;

  // TODO(idownes): After recovery top-level container's resource requests and
  // limits won't be known until after an update() because they aren't part of
  // the SlaveState.
  //
  // For nested containers, we will get their resource requests and limits from
  // their `ContainerConfig` since the `resourceRequests` and `resourceLimits`
  // fields in the `Container` struct won't be recovered for nested containers
  // after agent restart and update() won't be called for nested containers.
  if (containerId.has_parent()) {
    if (containers_.at(containerId)->config.isSome()) {
      resourceRequests = containers_.at(containerId)->config->resources();
      resourceLimits = containers_.at(containerId)->config->limits();
    }
  } else {
    resourceRequests = containers_.at(containerId)->resourceRequests;
    resourceLimits = containers_.at(containerId)->resourceLimits;
  }

  // Use await() here so we can return partial usage statistics.
  return await(futures)
    .then(lambda::bind(
          _usage,
          containerId,
          resourceRequests,
          resourceLimits,
#ifdef __linux__
          flags.cgroups_enable_cfs,
#else
          false,
#endif
          lambda::_1));
}


Future<ContainerStatus> MesosContainerizerProcess::status(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  vector<Future<ContainerStatus>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    if (!isSupportedByIsolator(
            containerId,
            isolator->supportsNesting(),
            isolator->supportsStandalone())) {
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
          .then([containerId](const vector<Future<ContainerStatus>>& statuses) {
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


Future<Option<ContainerTermination>> MesosContainerizerProcess::destroy(
    const ContainerID& containerId,
    const Option<ContainerTermination>& termination)
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

    // A nested container might have already been terminated, therefore
    // `containers_` might not contain it, but its exit status might have
    // been checkpointed.
    return wait(containerId);
  }

  const Owned<Container>& container = containers_.at(containerId);

  if (container->state == DESTROYING) {
    // NOTE: Use 'undiscardable' here to make sure discard from the
    // caller does not propagate into 'termination.future()' which
    // will be used in 'wait()'. We don't want a discard on
    // 'destroy()' call to affect future calls to 'wait()'. See more
    // details in MESOS-7926.
    return undiscardable(container->termination.future())
      .then(Option<ContainerTermination>::some);
  }

  LOG_BASED_ON_CLASS(container->containerClass())
    << "Destroying container " << containerId << " in "
    << container->state << " state";

  // NOTE: We save the previous state so that '_destroy' can properly
  // cleanup based on the previous state of the container.
  State previousState = container->state;

  transition(containerId, DESTROYING);

  vector<Future<Option<ContainerTermination>>> destroys;
  foreach (const ContainerID& child, container->children) {
    destroys.push_back(destroy(child, termination));
  }

  await(destroys).then(defer(
    self(), [=](const vector<Future<Option<ContainerTermination>>>& futures) {
      _destroy(containerId, termination, previousState, futures);
      return Nothing();
    }));

  // NOTE: Use 'undiscardable' here to make sure discard from the
  // caller does not propagate into 'termination.future()' which will
  // be used in 'wait()'. We don't want a discard on 'destroy()' call
  // to affect future calls to 'wait()'. See more details in
  // MESOS-7926.
  return undiscardable(container->termination.future())
    .then(Option<ContainerTermination>::some);
}


void MesosContainerizerProcess::_destroy(
    const ContainerID& containerId,
    const Option<ContainerTermination>& termination,
    const State& previousState,
    const vector<Future<Option<ContainerTermination>>>& destroys)
{
  CHECK(containers_.contains(containerId));

  const Owned<Container>& container = containers_[containerId];

  CHECK_EQ(container->state, DESTROYING);

  vector<string> errors;
  foreach (const Future<Option<ContainerTermination>>& future, destroys) {
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
    VLOG(1) << "Discarding the provisioning for container " << containerId;

    container->provisioning.discard();

    return _____destroy(containerId, termination, vector<Future<Nothing>>());
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
      .onAny(defer(self(), &Self::____destroy, containerId, termination));

    return;
  }

  if (previousState == ISOLATING) {
    VLOG(1) << "Waiting for the isolators to complete isolation "
            << "before destroying container " << containerId;

    // Wait for the isolators to finish isolating before we start
    // to destroy the container.
    container->isolation
      .onAny(defer(self(), &Self::__destroy, containerId, termination));

    return;
  }

  // Either RUNNING or FETCHING at this point.
  if (previousState == FETCHING) {
    fetcher->kill(containerId);
  }

  __destroy(containerId, termination);
}


void MesosContainerizerProcess::__destroy(
    const ContainerID& containerId,
    const Option<ContainerTermination>& termination)
{
  CHECK(containers_.contains(containerId));

  // Kill all processes then continue destruction.
  launcher->destroy(containerId)
    .onAny(defer(
        self(),
        &Self::___destroy,
        containerId,
        termination,
        lambda::_1));
}


void MesosContainerizerProcess::___destroy(
    const ContainerID& containerId,
    const Option<ContainerTermination>& termination,
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

  container->status
    ->onAny(defer(self(), &Self::____destroy, containerId, termination));
}


void MesosContainerizerProcess::____destroy(
    const ContainerID& containerId,
    const Option<ContainerTermination>& termination)
{
  CHECK(containers_.contains(containerId));

#ifndef __WINDOWS__
  if (volumeGidManager) {
    const Owned<Container>& container = containers_.at(containerId);

    if (container->config.isSome()) {
      VLOG(1) << "Invoking volume gid manager to deallocate gid for container "
              << containerId;

      volumeGidManager->deallocate(container->config->directory())
        .onAny(defer(self(), [=](const Future<Nothing>& future) {
          CHECK(containers_.contains(containerId));

          if (!future.isReady()) {
            container->termination.fail(
                "Failed to deallocate gid when destroying container: " +
                (future.isFailed() ? future.failure() : "discarded future"));

            ++metrics.container_destroy_errors;
            return;
          }

          cleanupIsolators(containerId)
            .onAny(defer(
                self(),
                &Self::_____destroy,
                containerId,
                termination,
                lambda::_1));
      }));

      return;
    }
  }
#endif // __WINDOWS__

  cleanupIsolators(containerId)
    .onAny(defer(
        self(),
        &Self::_____destroy,
        containerId,
        termination,
        lambda::_1));
}


void MesosContainerizerProcess::_____destroy(
    const ContainerID& containerId,
    const Option<ContainerTermination>& termination,
    const Future<vector<Future<Nothing>>>& cleanups)
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
    .onAny(defer(
        self(),
        &Self::______destroy,
        containerId,
        termination,
        lambda::_1));
}


void MesosContainerizerProcess::______destroy(
    const ContainerID& containerId,
    const Option<ContainerTermination>& _termination,
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

  if (_termination.isSome()) {
    termination = _termination.get();
  }

  if (container->status.isSome() &&
      container->status->isReady() &&
      container->status->get().isSome()) {
    termination.set_status(container->status->get().get());
  }

  // Now that we are done destroying the container we need to cleanup
  // its runtime directory. There are two cases to consider:
  //
  // (1) We are a nested container:
  //     * In this case we should defer deletion of the runtime directory
  //     until the top-level container is destroyed. Instead, we
  //     checkpoint a file with the termination state indicating that
  //     the container has already been destroyed. This allows
  //     subsequent calls to `wait()` to succeed with the proper
  //     termination state until the top-level container is destroyed.
  //     It also prevents subsequent `destroy()` calls from attempting
  //     to cleanup the container a second time.
  //     * We also schedule the nested container's sandbox directory for
  //     garbage collection, if this behavior is enabled.
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

    LOG_BASED_ON_CLASS(container->containerClass())
      << "Checkpointing termination state to nested container's runtime"
      << " directory '" << terminationPath << "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(terminationPath, termination);

    if (checkpointed.isError()) {
      LOG(ERROR) << "Failed to checkpoint nested container's termination state"
                 << " to '" << terminationPath << "': " << checkpointed.error();
    }

    // Schedule the sandbox of the nested container for garbage collection.
    if (flags.gc_non_executor_container_sandboxes) {
      const ContainerID rootContainerId =
        protobuf::getRootContainerId(containerId);

      CHECK(containers_.contains(rootContainerId));

      if (containers_[rootContainerId]->directory.isSome()) {
        const string sandboxPath = containerizer::paths::getSandboxPath(
          containers_[rootContainerId]->directory.get(), containerId);

        garbageCollect(sandboxPath);
      }
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


Future<Nothing> MesosContainerizerProcess::garbageCollect(const string& path)
{
  // Some tests do not pass the GC actor into the containerizer for
  // convenience of test construction. Those tests should not exercise
  // this code path.
  CHECK_NOTNULL(gc);

  Try<long> mtime = os::stat::mtime(path);
  if (mtime.isError()) {
    LOG(ERROR) << "Failed to find the mtime of '" << path
               << "': " << mtime.error();
    return Failure(mtime.error());
  }

  // It is unsafe for testing to use unix time directly, we must use
  // Time::create to convert into a Time object that reflects the
  // possibly advanced state of the libprocess Clock.
  Try<Time> time = Time::create(mtime.get());
  CHECK_SOME(time);

  // GC based on the modification time.
  Duration delay = flags.gc_delay - (Clock::now() - time.get());

  return gc->schedule(delay, path);
}


Future<bool> MesosContainerizerProcess::kill(
    const ContainerID& containerId,
    int signal)
{
  if (!containers_.contains(containerId)) {
    LOG(WARNING) << "Attempted to kill unknown container " << containerId;

    return false;
  }

  const Owned<Container>& container = containers_.at(containerId);

  LOG_BASED_ON_CLASS(container->containerClass())
    << "Sending " << strsignal(signal) << " to container "
    << containerId << " in " << container->state << " state";

  // This can happen when we try to signal the container before it
  // is launched. We destroy the container forcefully in this case.
  //
  // TODO(anand): Consider chaining this to the launch completion
  // future instead.
  if (container->pid.isNone()) {
    LOG(WARNING) << "Unable to find the pid for container " << containerId
                 << ", destroying it";

    destroy(containerId, None());
    return true;
  }

  int status = os::kill(container->pid.get(), signal);
  if (status != 0) {
    return Failure("Unable to send signal to container: "  +
                   os::strerror(errno));
  }

  return true;
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

  if (containers_[rootContainerId]->directory.isSome()) {
    const string sandboxPath = containerizer::paths::getSandboxPath(
        containers_[rootContainerId]->directory.get(), containerId);

    if (os::exists(sandboxPath)) {
      // Unschedule the nested container sandbox from garbage collection
      // to prevent potential double-deletion in future.
      if (flags.gc_non_executor_container_sandboxes) {
        CHECK_NOTNULL(gc);
        gc->unschedule(sandboxPath);
      }

      Try<Nothing> rmdir = os::rmdir(sandboxPath);
      if (rmdir.isError()) {
        return Failure(
            "Failed to remove the sandbox directory: " + rmdir.error());
      }
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

  LOG_BASED_ON_CLASS(containers_.at(containerId)->containerClass())
    << "Container " << containerId << " has exited";

  // The executor has exited so destroy the container.
  destroy(containerId, None());
}


void MesosContainerizerProcess::limited(
    const ContainerID& containerId,
    const Future<ContainerLimitation>& future)
{
  if (!containers_.contains(containerId) ||
      containers_.at(containerId)->state == DESTROYING) {
    return;
  }

  Option<ContainerTermination> termination = None();

  if (future.isReady()) {
    LOG_BASED_ON_CLASS(containers_.at(containerId)->containerClass())
      << "Container " << containerId << " has reached its limit for resource "
      << future->resources() << " and will be terminated";

    termination = ContainerTermination();
    termination->set_state(TaskState::TASK_FAILED);
    termination->set_message(future->message());

    if (future->has_reason()) {
      termination->set_reason(future->reason());
    }

    if (!future->resources().empty()) {
        termination->mutable_limited_resources()->CopyFrom(
            future->resources());
    }
  } else {
    // TODO(idownes): A discarded future will not be an error when
    // isolators discard their promises after cleanup.
    LOG(ERROR) << "Error in a resource limitation for container "
               << containerId << ": " << (future.isFailed() ? future.failure()
                                                            : "discarded");
  }

  // The container has been affected by the limitation so destroy it.
  destroy(containerId, termination);
}


Future<hashset<ContainerID>> MesosContainerizerProcess::containers()
{
  return containers_.keys();
}


Future<Nothing> MesosContainerizerProcess::pruneImages(
    const vector<Image>& excludedImages)
{
  vector<Image> _excludedImages;
  _excludedImages.reserve(containers_.size() + excludedImages.size());

  foreachpair (
      const ContainerID& containerId,
      const Owned<Container>& container,
      containers_) {
    // Checkpointing ContainerConfig is introduced recently. Legacy containers
    // do not have the information of which image is used. Image pruning is
    // disabled.
    if (container->config.isNone()) {
      return Failure(
          "Container " + stringify(containerId) +
          " does not have ContainerConfig "
          "checkpointed. Image pruning is disabled");
    }

    const ContainerConfig& containerConfig = container->config.get();
    if (containerConfig.has_container_info() &&
        containerConfig.container_info().mesos().has_image()) {
      _excludedImages.push_back(
          containerConfig.container_info().mesos().image());
    }
  }

  foreach (const Image& image, excludedImages) {
    _excludedImages.push_back(image);
  }

  // TODO(zhitao): use std::unique to deduplicate `_excludedImages`.

  return provisioner->pruneImages(_excludedImages);
}


ContainerClass MesosContainerizerProcess::Container::containerClass()
{
  return (config.isSome() && config->has_container_class())
      ? config->container_class()
      : ContainerClass::DEFAULT;
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


Future<vector<Future<Nothing>>> MesosContainerizerProcess::cleanupIsolators(
    const ContainerID& containerId)
{
  Future<vector<Future<Nothing>>> f = vector<Future<Nothing>>();

  // NOTE: We clean up each isolator in the reverse order they were
  // prepared (see comment in prepare()).
  foreach (const Owned<Isolator>& isolator, adaptor::reverse(isolators)) {
    if (!isSupportedByIsolator(
            containerId,
            isolator->supportsNesting(),
            isolator->supportsStandalone())) {
      continue;
    }

    // We'll try to clean up all isolators, waiting for each to
    // complete and continuing if one fails.
    // TODO(jieyu): Technically, we cannot bind 'isolator' here
    // because the ownership will be transferred after the bind.
    f = f.then([=](vector<Future<Nothing>> cleanups) {
      // Accumulate but do not propagate any failure.
      Future<Nothing> cleanup = isolator->cleanup(containerId);
      cleanups.push_back(cleanup);

      // Wait for the cleanup to complete/fail before returning the
      // list. We use await here to asynchronously wait for the
      // isolator to complete then return cleanups.
      return await(vector<Future<Nothing>>({cleanup}))
        .then([cleanups]() -> Future<vector<Future<Nothing>>> {
          return cleanups;
        });
    });
  }

  return f;
}


void MesosContainerizerProcess::transition(
    const ContainerID& containerId,
    const State& state)
{
  CHECK(containers_.contains(containerId));

  Time now = Clock::now();
  const Owned<Container>& container = containers_.at(containerId);

  LOG_BASED_ON_CLASS(container->containerClass())
    << "Transitioning the state of container " << containerId << " from "
    << container->state << " to " << state
    << " after " << (now - container->lastStateTransition);

  container->state = state;
  container->lastStateTransition = now;
}


bool MesosContainerizerProcess::isSupportedByIsolator(
    const ContainerID& containerId,
    bool isolatorSupportsNesting,
    bool isolatorSupportsStandalone)
{
  if (!isolatorSupportsNesting) {
    if (containerId.has_parent()) {
      return false;
    }
  }

  if (!isolatorSupportsStandalone) {
    // NOTE: If standalone container is not supported, the nested
    // containers of the standalone container won't be supported
    // neither.
    ContainerID rootContainerId = protobuf::getRootContainerId(containerId);

    bool isStandaloneContainer =
      containerizer::paths::isStandaloneContainer(
          flags.runtime_dir,
          rootContainerId);

    if (isStandaloneContainer) {
      return false;
    }
  }

  return true;
}


std::ostream& operator<<(
    std::ostream& stream,
    const MesosContainerizerProcess::State& state)
{
  switch (state) {
    case MesosContainerizerProcess::STARTING:
      return stream << "STARTING";
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
  }

  UNREACHABLE();
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {
