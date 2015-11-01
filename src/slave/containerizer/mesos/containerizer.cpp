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

#include <mesos/module/isolator.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/io.hpp>
#include <process/metrics/metrics.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/fs.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "common/protobuf_utils.hpp"

#include "module/manager.hpp"

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/launcher.hpp"
#ifdef __linux__
#include "slave/containerizer/linux_launcher.hpp"
#endif

#include "slave/containerizer/mesos/isolators/posix.hpp"

#include "slave/containerizer/mesos/isolators/posix/disk.hpp"

#ifdef __linux__
#include "slave/containerizer/mesos/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/mem.hpp"
#include "slave/containerizer/mesos/isolators/cgroups/perf_event.hpp"
#endif

#ifdef __linux__
#include "slave/containerizer/mesos/isolators/filesystem/linux.hpp"
#endif
#include "slave/containerizer/mesos/isolators/filesystem/posix.hpp"
#ifdef __linux__
#include "slave/containerizer/mesos/isolators/filesystem/shared.hpp"
#endif

#ifdef __linux__
#include "slave/containerizer/mesos/isolators/namespaces/pid.hpp"
#endif

#ifdef WITH_NETWORK_ISOLATOR
#include "slave/containerizer/mesos/isolators/network/port_mapping.hpp"
#endif

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

using std::list;
using std::map;
using std::string;
using std::vector;

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

using mesos::modules::ModuleManager;

using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerPrepareInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using state::SlaveState;
using state::FrameworkState;
using state::ExecutorState;
using state::RunState;

const char MESOS_CONTAINERIZER[] = "mesos-containerizer";

Try<MesosContainerizer*> MesosContainerizer::create(
    const Flags& flags,
    bool local,
    Fetcher* fetcher)
{
  string isolation;

  if (flags.isolation == "process") {
    LOG(WARNING) << "The 'process' isolation flag is deprecated, "
                 << "please update your flags to"
                 << " '--isolation=posix/cpu,posix/mem'.";

    isolation = "posix/cpu,posix/mem";
  } else if (flags.isolation == "cgroups") {
    LOG(WARNING) << "The 'cgroups' isolation flag is deprecated, "
                 << "please update your flags to"
                 << " '--isolation=cgroups/cpu,cgroups/mem'.";

    isolation = "cgroups/cpu,cgroups/mem";
  } else {
    isolation = flags.isolation;
  }

  // One and only one filesystem isolator is required. The filesystem
  // isolator is responsible for preparing the filesystems for
  // containers (e.g., prepare filesystem roots, volumes, etc.). If
  // the user does not specify one, 'filesystem/posix' will be used.
  //
  // TODO(jieyu): Check that only one filesystem isolator is used.
  if (!strings::contains(isolation, "filesystem/")) {
    isolation += ",filesystem/posix";
  }

  // Modify the flags to include any changes to isolation.
  Flags flags_ = flags;
  flags_.isolation = isolation;

  LOG(INFO) << "Using isolation: " << isolation;

#ifdef __linux__
  // The provisioner will be used by the 'filesystem/linux' isolator.
  Try<Owned<Provisioner>> provisioner = Provisioner::create(flags, fetcher);
  if (provisioner.isError()) {
    return Error("Failed to create provisioner: " + provisioner.error());
  }
#endif

  // Create a MesosContainerizerProcess using isolators and a launcher.
  const hashmap<string, lambda::function<Try<Isolator*>(const Flags&)>>
    creators = {
    // Filesystem isolators.
    {"filesystem/posix", &PosixFilesystemIsolatorProcess::create},
#ifdef __linux__
    {"filesystem/linux", lambda::bind(&LinuxFilesystemIsolatorProcess::create,
                                      lambda::_1,
                                      provisioner.get())},

    // TODO(jieyu): Deprecate this in favor of using filesystem/linux.
    {"filesystem/shared", &SharedFilesystemIsolatorProcess::create},
#endif

    // Runtime isolators.
    {"posix/cpu", &PosixCpuIsolatorProcess::create},
    {"posix/mem", &PosixMemIsolatorProcess::create},
    {"posix/disk", &PosixDiskIsolatorProcess::create},
#ifdef __linux__
    {"cgroups/cpu", &CgroupsCpushareIsolatorProcess::create},
    {"cgroups/mem", &CgroupsMemIsolatorProcess::create},
    {"cgroups/perf_event", &CgroupsPerfEventIsolatorProcess::create},
    {"namespaces/pid", &NamespacesPidIsolatorProcess::create},
#endif
#ifdef WITH_NETWORK_ISOLATOR
    {"network/port_mapping", &PortMappingIsolatorProcess::create},
#endif
  };

  vector<Owned<Isolator>> isolators;

  foreach (const string& type, strings::tokenize(isolation, ",")) {
    Owned<Isolator> isolator;

    if (creators.contains(type)) {
      Try<Isolator*> _isolator = creators.at(type)(flags_);
      if (_isolator.isError()) {
        return Error(
            "Could not create isolator " + type + ": " + _isolator.error());
      }

      isolator.reset(_isolator.get());
    } else if (ModuleManager::contains<Isolator>(type)) {
      Try<Isolator*> _isolator = ModuleManager::create<Isolator>(type);
      if (_isolator.isError()) {
        return Error(
            "Could not create isolator " + type + ": " + _isolator.error());
      }

      isolator.reset(_isolator.get());
    } else {
      return Error("Unknown or unsupported isolator: " + type);
    }

    // NOTE: The filesystem isolator must be the first isolator used
    // so that the runtime isolators can have a consistent view on the
    // prepared filesystem (e.g., any volume mounts are performed).
    if (strings::contains(type, "filesystem/")) {
      isolators.insert(isolators.begin(), isolator);
    } else {
      isolators.push_back(isolator);
    }
  }

#ifdef __linux__
  Try<Launcher*> launcher = (Launcher*) NULL;
  if (flags_.launcher.isSome()) {
    // If the user has specified the launcher, use it.
    if (flags_.launcher.get() == "linux") {
      launcher = LinuxLauncher::create(flags_);
    } else if (flags_.launcher.get() == "posix") {
      launcher = PosixLauncher::create(flags_);
    } else {
      return Error("Unknown or unsupported launcher: " + flags_.launcher.get());
    }
  } else {
    // Use Linux launcher if it is available, POSIX otherwise.
    launcher = LinuxLauncher::available()
      ? LinuxLauncher::create(flags_)
      : PosixLauncher::create(flags_);
  }
#else
  if (flags_.launcher.isSome() && flags_.launcher.get() != "posix") {
    return Error("Unsupported launcher: " + flags_.launcher.get());
  }

  Try<Launcher*> launcher = PosixLauncher::create(flags_);
#endif // __linux__

  if (launcher.isError()) {
    return Error("Failed to create launcher: " + launcher.error());
  }

  return new MesosContainerizer(
      flags_,
      local,
      fetcher,
      Owned<Launcher>(launcher.get()),
      isolators);
}


MesosContainerizer::MesosContainerizer(
    const Flags& flags,
    bool local,
    Fetcher* fetcher,
    const Owned<Launcher>& launcher,
    const vector<Owned<Isolator>>& isolators)
  : process(new MesosContainerizerProcess(
      flags,
      local,
      fetcher,
      launcher,
      isolators))
{
  spawn(process.get());
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
  return dispatch(process.get(), &MesosContainerizerProcess::recover, state);
}


Future<bool> MesosContainerizer::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::launch,
                  containerId,
                  executorInfo,
                  directory,
                  user,
                  slaveId,
                  slavePid,
                  checkpoint);
}


Future<bool> MesosContainerizer::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return dispatch(process.get(),
                  &MesosContainerizerProcess::launch,
                  containerId,
                  taskInfo,
                  executorInfo,
                  directory,
                  user,
                  slaveId,
                  slavePid,
                  checkpoint);
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
  return dispatch(
      process.get(),
      &MesosContainerizerProcess::usage,
      containerId);
}


Future<containerizer::Termination> MesosContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process.get(), &MesosContainerizerProcess::wait, containerId);
}


void MesosContainerizer::destroy(const ContainerID& containerId)
{
  dispatch(
      process.get(),
      &MesosContainerizerProcess::destroy,
      containerId);
}


Future<hashset<ContainerID>> MesosContainerizer::containers()
{
  return dispatch(process.get(), &MesosContainerizerProcess::containers);
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

        // We need the pid so the reaper can monitor the executor so skip this
        // executor if it's not present. This is not an error because the slave
        // will try to wait on the container which will return a failed
        // Termination and everything will get cleaned up.
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

        LOG(INFO) << "Recovering container '" << containerId
                  << "' for executor '" << executor.id
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

  // Try to recover the launcher first.
  return launcher->recover(recoverable)
    .then(defer(self(), &Self::_recover, recoverable, lambda::_1));
}


Future<Nothing> MesosContainerizerProcess::_recover(
    const list<ContainerState>& recoverable,
    const hashset<ContainerID>& orphans)
{
  list<Future<Nothing>> futures;

  // Then recover the isolators.
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->recover(recoverable, orphans));
  }

  // If all isolators recover then continue.
  return collect(futures)
    .then(defer(self(), &Self::__recover, recoverable, orphans));
}


Future<Nothing> MesosContainerizerProcess::__recover(
    const list<ContainerState>& recovered,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& run, recovered) {
    const ContainerID& containerId = run.container_id();

    Container* container = new Container();

    Future<Option<int>> status = process::reap(run.pid());
    status.onAny(defer(self(), &Self::reaped, containerId));
    container->status = status;

    container->directory = run.directory();

    // We only checkpoint the containerizer pid after the container
    // successfully launched, therefore we can assume checkpointed
    // containers should be running after recover.
    container->state = RUNNING;

    containers_[containerId] = Owned<Container>(container);

    foreach (const Owned<Isolator>& isolator, isolators) {
      isolator->watch(containerId)
        .onAny(defer(self(), &Self::limited, containerId, lambda::_1));
    }
  }

  // Destroy all the orphan containers.
  // NOTE: We do not fail the recovery if the destroy of orphan
  // containers failed. See MESOS-2367 for details.
  foreach (const ContainerID& containerId, orphans) {
    LOG(INFO) << "Removing orphan container " << containerId;

    launcher->destroy(containerId)
      .then(defer(self(), &Self::cleanupIsolators, containerId))
      .onAny(defer(self(), &Self::___recover, containerId, lambda::_1));
  }

  return Nothing();
}


void MesosContainerizerProcess::___recover(
    const ContainerID& containerId,
    const Future<list<Future<Nothing>>>& future)
{
  // NOTE: If 'future' is not ready, that indicates launcher destroy
  // has failed because 'cleanupIsolators' should always return a
  // ready future.
  if (!future.isReady()) {
    LOG(ERROR) << "Failed to destroy orphan container " << containerId << ": "
               << (future.isFailed() ? future.failure() : "discarded");

    ++metrics.container_destroy_errors;
    return;
  }

  // Indicates if the isolator cleanups have any failure or not.
  bool cleanupFailed = false;

  foreach (const Future<Nothing>& cleanup, future.get()) {
    if (!cleanup.isReady()) {
      LOG(ERROR) << "Failed to clean up an isolator when destroying "
                 << "orphan container " << containerId << ": "
                 << (cleanup.isFailed() ? cleanup.failure() : "discarded");

      cleanupFailed = true;
    }
  }

  if (cleanupFailed) {
    ++metrics.container_destroy_errors;
  }
}


Future<bool> MesosContainerizerProcess::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (taskInfo.has_container()) {
    // We return false as this containerizer does not support
    // handling TaskInfo::ContainerInfo.
    return false;
  }

  return launch(
      containerId,
      executorInfo,
      directory,
      user,
      slaveId,
      slavePid,
      checkpoint);
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
    const ExecutorInfo& _executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (containers_.contains(containerId)) {
    return Failure("Container already started");
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

  // MesosContainerizer does not support ContainerInfo in CommandInfo.
  if (executorInfo.command().has_container()) {
    return false;
  }

  LOG(INFO) << "Starting container '" << containerId
            << "' for executor '" << executorInfo.executor_id()
            << "' of framework '" << executorInfo.framework_id() << "'";

  Container* container = new Container();
  container->directory = directory;
  container->state = PREPARING;
  container->resources = executorInfo.resources();

  containers_.put(containerId, Owned<Container>(container));

  return prepare(containerId, executorInfo, directory, user)
    .then(defer(self(),
                &Self::_launch,
                containerId,
                executorInfo,
                directory,
                user,
                slaveId,
                slavePid,
                checkpoint,
                lambda::_1));
}


static list<Option<ContainerPrepareInfo>> accumulate(
    list<Option<ContainerPrepareInfo>> l,
    const Option<ContainerPrepareInfo>& e)
{
  l.push_back(e);
  return l;
}


static Future<list<Option<ContainerPrepareInfo>>> _prepare(
    const Owned<Isolator>& isolator,
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const list<Option<ContainerPrepareInfo>> prepareInfos)
{
  // Propagate any failure.
  return isolator->prepare(containerId, executorInfo, directory, user)
    .then(lambda::bind(&accumulate, prepareInfos, lambda::_1));
}


Future<list<Option<ContainerPrepareInfo>>> MesosContainerizerProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user)
{
  CHECK(containers_.contains(containerId));

  // We prepare the isolators sequentially according to their ordering
  // to permit basic dependency specification, e.g., preparing a
  // filesystem isolator before other isolators.
  Future<list<Option<ContainerPrepareInfo>>> f =
    list<Option<ContainerPrepareInfo>>();

  foreach (const Owned<Isolator>& isolator, isolators) {
    // Chain together preparing each isolator.
    f = f.then(lambda::bind(&_prepare,
                            isolator,
                            containerId,
                            executorInfo,
                            directory,
                            user,
                            lambda::_1));
  }

  containers_[containerId]->prepareInfos = f;

  return f;
}


Future<Nothing> MesosContainerizerProcess::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container is already destroyed");
  }

  return fetcher->fetch(
      containerId,
      commandInfo,
      directory,
      user,
      slaveId,
      flags);
}


Future<bool> MesosContainerizerProcess::_launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint,
    const list<Option<ContainerPrepareInfo>>& prepareInfos)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container has been destroyed");
  }

  if (containers_[containerId]->state == DESTROYING) {
    return Failure("Container is currently being destroyed");
  }

  // Determine the root filesystem for the container. Only one
  // isolator should return the container root filesystem.
  Option<string> rootfs;
  foreach (const Option<ContainerPrepareInfo>& prepareInfo, prepareInfos) {
    if (prepareInfo.isSome() && prepareInfo.get().has_rootfs()) {
      if (rootfs.isSome()) {
        return Failure("Only one isolator should return the container rootfs");
      } else {
        rootfs = prepareInfo.get().rootfs();
      }
    }
  }

  // Prepare environment variables for the executor.
  map<string, string> environment = executorEnvironment(
      executorInfo,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      flags);

  // TODO(jieyu): Consider moving this to 'executorEnvironment' and
  // consolidating with docker containerizer.
  environment["MESOS_SANDBOX"] =
    rootfs.isSome() ? flags.sandbox_directory : directory;

  // Include any enviroment variables from CommandInfo.
  foreach (const Environment::Variable& variable,
           executorInfo.command().environment().variables()) {
    environment[variable.name()] = variable.value();
  }

  JSON::Array commandArray;
  int namespaces = 0;
  foreach (const Option<ContainerPrepareInfo>& prepareInfo, prepareInfos) {
    if (!prepareInfo.isSome()) {
      continue;
    }

    // Populate the list of additional commands to be run inside the container
    // context.
    foreach (const CommandInfo& command, prepareInfo.get().commands()) {
      commandArray.values.push_back(JSON::Protobuf(command));
    }

    // Process additional environment variables returned by isolators.
    if (prepareInfo.get().has_environment()) {
      foreach (const Environment::Variable& variable,
          prepareInfo.get().environment().variables()) {
        environment[variable.name()] = variable.value();
      }
    }

    if (prepareInfo.get().has_namespaces()) {
      namespaces |= prepareInfo.get().namespaces();
    }
  }

  // TODO(jieyu): Use JSON::Array once we have generic parse support.
  JSON::Object commands;
  commands.values["commands"] = commandArray;

  // Use a pipe to block the child until it's been isolated.
  int pipes[2];

  // We assume this should not fail under reasonable conditions so we
  // use CHECK.
  CHECK(pipe(pipes) == 0);

  // Prepare the flags to pass to the launch process.
  MesosContainerizerLaunch::Flags launchFlags;

  launchFlags.command = JSON::Protobuf(executorInfo.command());

  launchFlags.directory = rootfs.isSome() ? flags.sandbox_directory : directory;
  launchFlags.rootfs = rootfs;
  launchFlags.user = user;
  launchFlags.pipe_read = pipes[0];
  launchFlags.pipe_write = pipes[1];
  launchFlags.commands = commands;

  // Fork the child using launcher.
  vector<string> argv(2);
  argv[0] = MESOS_CONTAINERIZER;
  argv[1] = MesosContainerizerLaunch::NAME;

  Try<pid_t> forked = launcher->fork(
      containerId,
      path::join(flags.launcher_dir, MESOS_CONTAINERIZER),
      argv,
      Subprocess::FD(STDIN_FILENO),
      (local ? Subprocess::FD(STDOUT_FILENO)
             : Subprocess::PATH(path::join(directory, "stdout"))),
      (local ? Subprocess::FD(STDERR_FILENO)
             : Subprocess::PATH(path::join(directory, "stderr"))),
      launchFlags,
      environment,
      None(),
      namespaces); // 'namespaces' will be ignored by PosixLauncher.

  if (forked.isError()) {
    return Failure("Failed to fork executor: " + forked.error());
  }
  pid_t pid = forked.get();

  // Checkpoint the executor's pid if requested.
  if (checkpoint) {
    const string& path = slave::paths::getForkedPidPath(
        slave::paths::getMetaRootDir(flags.work_dir),
        slaveId,
        executorInfo.framework_id(),
        executorInfo.executor_id(),
        containerId);

    LOG(INFO) << "Checkpointing executor's forked pid " << pid
              << " to '" << path <<  "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(path, stringify(pid));

    if (checkpointed.isError()) {
      LOG(ERROR) << "Failed to checkpoint executor's forked pid to '"
                 << path << "': " << checkpointed.error();

      return Failure("Could not checkpoint executor's pid");
    }
  }

  // Monitor the executor's pid. We keep the future because we'll
  // refer to it again during container destroy.
  Future<Option<int>> status = process::reap(pid);
  status.onAny(defer(self(), &Self::reaped, containerId));
  containers_[containerId]->status = status;

  return isolate(containerId, pid)
    .then(defer(self(),
                &Self::fetch,
                containerId,
                executorInfo.command(),
                directory,
                user,
                slaveId))
    .then(defer(self(), &Self::exec, containerId, pipes[1]))
    .onAny(lambda::bind(&os::close, pipes[0]))
    .onAny(lambda::bind(&os::close, pipes[1]));
}


Future<bool> MesosContainerizerProcess::isolate(
    const ContainerID& containerId,
    pid_t _pid)
{
  CHECK(containers_.contains(containerId));

  containers_[containerId]->state = ISOLATING;

  // Set up callbacks for isolator limitations.
  foreach (const Owned<Isolator>& isolator, isolators) {
    isolator->watch(containerId)
      .onAny(defer(self(), &Self::limited, containerId, lambda::_1));
  }

  // Isolate the executor with each isolator.
  // NOTE: This is done is parallel and is not sequenced like prepare
  // or destroy because we assume there are no dependencies in
  // isolation.
  list<Future<Nothing>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->isolate(containerId, _pid));
  }

  // Wait for all isolators to complete.
  Future<list<Nothing>> future = collect(futures);

  containers_[containerId]->isolation = future;

  return future.then([]() { return true; });
}


Future<bool> MesosContainerizerProcess::exec(
    const ContainerID& containerId,
    int pipeWrite)
{
  // The container may be destroyed before we exec the executor so
  // return failure here.
  if (!containers_.contains(containerId) ||
      containers_[containerId]->state == DESTROYING) {
    return Failure("Container destroyed during launch");
  }

  // Now that we've contained the child we can signal it to continue
  // by writing to the pipe.
  char dummy;
  ssize_t length;
  while ((length = write(pipeWrite, &dummy, sizeof(dummy))) == -1 &&
         errno == EINTR);

  if (length != sizeof(dummy)) {
    return Failure("Failed to synchronize child process: " +
                   string(strerror(errno)));
  }

  containers_[containerId]->state = RUNNING;

  return true;
}


Future<containerizer::Termination> MesosContainerizerProcess::wait(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  return containers_[containerId]->promise.future();
}


Future<Nothing> MesosContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (!containers_.contains(containerId)) {
    // It is not considered a failure if the container is not known
    // because the slave will attempt to update the container's
    // resources on a task's terminal state change but the executor
    // may have already exited and the container cleaned up.
    LOG(WARNING) << "Ignoring update for unknown container: " << containerId;
    return Nothing();
  }

  const Owned<Container>& container = containers_[containerId];

  if (container->state == DESTROYING) {
    LOG(WARNING) << "Ignoring update for currently being destroyed container: "
                 << containerId;
    return Nothing();
  }

  // NOTE: We update container's resources before isolators are updated
  // so that subsequent containerizer->update can be handled properly.
  container->resources = resources;

  // Update each isolator.
  list<Future<Nothing>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
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
  if (!containers_.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  list<Future<ResourceStatistics>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->usage(containerId));
  }

  // Use await() here so we can return partial usage statistics.
  // TODO(idownes): After recovery resources won't be known until
  // after an update() because they aren't part of the SlaveState.
  return await(futures)
    .then(lambda::bind(
          _usage,
          containerId,
          containers_[containerId]->resources,
          lambda::_1));
}


void MesosContainerizerProcess::destroy(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    LOG(WARNING) << "Ignoring destroy of unknown container: " << containerId;
    return;
  }

  Container* container = containers_[containerId].get();

  if (container->state == DESTROYING) {
    // Destroy has already been initiated.
    return;
  }

  LOG(INFO) << "Destroying container '" << containerId << "'";

  if (container->state == PREPARING) {
    VLOG(1) << "Waiting for the isolators to complete preparing before "
            << "destroying the container";

    container->state = DESTROYING;

    Future<Option<int>> status = None();
    // We need to wait for the isolators to finish preparing to prevent
    // a race that the destroy method calls isolators' cleanup before
    // it starts preparing.
    container->prepareInfos
      .onAny(defer(
          self(),
          &Self::___destroy,
          containerId,
          status,
          "Container destroyed while preparing isolators"));

    return;
  }

  if (container->state == FETCHING) {
    fetcher->kill(containerId);
  }

  if (container->state == ISOLATING) {
    VLOG(1) << "Waiting for the isolators to complete for container '"
            << containerId << "'";

    container->state = DESTROYING;

    // Wait for the isolators to finish isolating before we start
    // to destroy the container.
    container->isolation
      .onAny(defer(self(), &Self::_destroy, containerId));

    return;
  }

  container->state = DESTROYING;
  _destroy(containerId);
}


void MesosContainerizerProcess::_destroy(
    const ContainerID& containerId)
{
  // Kill all processes then continue destruction.
  launcher->destroy(containerId)
    .onAny(defer(self(), &Self::__destroy, containerId, lambda::_1));
}


void MesosContainerizerProcess::__destroy(
    const ContainerID& containerId,
    const Future<Nothing>& future)
{
  CHECK(containers_.contains(containerId));

  // Something has gone wrong and the launcher wasn't able to kill all
  // the processes in the container. We cannot clean up the isolators
  // because they may require that all processes have exited so just
  // return the failure to the slave.
  // TODO(idownes): This is a pretty bad state to be in but we should
  // consider cleaning up here.
  if (!future.isReady()) {
    containers_[containerId]->promise.fail(
        "Failed to destroy container " + stringify(containerId) + ": " +
        (future.isFailed() ? future.failure() : "discarded future"));

    containers_.erase(containerId);

    ++metrics.container_destroy_errors;

    return;
  }

  // We've successfully killed all processes in the container so get
  // the exit status of the executor when it's ready (it may already
  // be) and continue the destroy.
  containers_[containerId]->status
    .onAny(defer(
        self(),
        &Self::___destroy,
        containerId,
        lambda::_1,
        None()));
}


void MesosContainerizerProcess::___destroy(
    const ContainerID& containerId,
    const Future<Option<int>>& status,
    const Option<string>& message)
{
  cleanupIsolators(containerId)
    .onAny(defer(self(),
                 &Self::____destroy,
                 containerId,
                 status,
                 lambda::_1,
                 message));
}


void MesosContainerizerProcess::____destroy(
    const ContainerID& containerId,
    const Future<Option<int>>& status,
    const Future<list<Future<Nothing>>>& cleanups,
    Option<string> message)
{
  // This should not occur because we only use the Future<list> to
  // facilitate chaining.
  CHECK_READY(cleanups);
  CHECK(containers_.contains(containerId));

  Container* container = containers_[containerId].get();

  // Check cleanup succeeded for all isolators. If not, we'll fail the
  // container termination and remove the 'destroying' flag but leave
  // all other state. The container is now in an inconsistent state.
  foreach (const Future<Nothing>& cleanup, cleanups.get()) {
    if (!cleanup.isReady()) {
      container->promise.fail(
          "Failed to clean up an isolator when destroying container '" +
          stringify(containerId) + "': " +
          (cleanup.isFailed() ? cleanup.failure() : "discarded future"));

      containers_.erase(containerId);

      ++metrics.container_destroy_errors;

      return;
    }
  }

  containerizer::Termination termination;

  if (status.isReady() && status->isSome()) {
    termination.set_status(status->get());
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

    message = strings::join("; ", messages);
  }

  if (message.isNone()) {
    message = "Executor terminated";
  }

  termination.set_message(message.get());

  container->promise.set(termination);

  containers_.erase(containerId);
}


void MesosContainerizerProcess::reaped(const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return;
  }

  LOG(INFO) << "Executor for container '" << containerId << "' has exited";

  // The executor has exited so destroy the container.
  destroy(containerId);
}


void MesosContainerizerProcess::limited(
    const ContainerID& containerId,
    const Future<ContainerLimitation>& future)
{
  if (!containers_.contains(containerId) ||
      containers_[containerId]->state == DESTROYING) {
    return;
  }

  if (future.isReady()) {
    LOG(INFO) << "Container " << containerId << " has reached its limit for"
              << " resource " << future.get().resources()
              << " and will be terminated";

    containers_[containerId]->limitations.push_back(future.get());
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


static Future<list<Future<Nothing>>> _cleanupIsolators(
    const Owned<Isolator>& isolator,
    const ContainerID& containerId,
    list<Future<Nothing>> cleanups)
{
  // Accumulate but do not propagate any failure.
  Future<Nothing> cleanup = isolator->cleanup(containerId);
  cleanups.push_back(cleanup);

  // Wait for the cleanup to complete/fail before returning the list.
  // We use await here to asynchronously wait for the isolator to
  // complete then return cleanups.
  list<Future<Nothing>> cleanup_;
  cleanup_.push_back(cleanup);

  return await(cleanup_)
    .then([cleanups]() -> list<Future<Nothing>> { return cleanups; });
}


Future<list<Future<Nothing>>> MesosContainerizerProcess::cleanupIsolators(
    const ContainerID& containerId)
{
  Future<list<Future<Nothing>>> f = list<Future<Nothing>>();

  // NOTE: We clean up each isolator in the reverse order they were
  // prepared (see comment in prepare()).
  for (auto it = isolators.crbegin(); it != isolators.crend(); ++it) {
    const Owned<Isolator>& isolator = *it;

    // We'll try to clean up all isolators, waiting for each to
    // complete and continuing if one fails.
    // TODO(jieyu): Technically, we cannot bind 'isolator' here
    // because the ownership will be transferred after the bind.
    f = f.then(lambda::bind(&_cleanupIsolators,
                            isolator,
                            containerId,
                            lambda::_1));
  }

  return f;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
