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

#include <stout/os.hpp>

#include "module/manager.hpp"

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/launcher.hpp"
#ifdef __linux__
#include "slave/containerizer/linux_launcher.hpp"
#endif // __linux__

#include "slave/containerizer/isolators/posix.hpp"
#include "slave/containerizer/isolators/posix/disk.hpp"
#ifdef __linux__
#include "slave/containerizer/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"
#include "slave/containerizer/isolators/cgroups/perf_event.hpp"
#include "slave/containerizer/isolators/filesystem/shared.hpp"
#include "slave/containerizer/isolators/namespaces/pid.hpp"
#endif // __linux__
#ifdef WITH_NETWORK_ISOLATOR
#include "slave/containerizer/isolators/network/port_mapping.hpp"
#endif

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"

using std::list;
using std::map;
using std::string;
using std::vector;

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

using mesos::modules::ModuleManager;

using mesos::slave::ExecutorRunState;
using mesos::slave::Isolator;
using mesos::slave::IsolatorProcess;
using mesos::slave::Limitation;

using state::SlaveState;
using state::FrameworkState;
using state::ExecutorState;
using state::RunState;

// Local function declaration/definitions.
Future<Nothing> _nothing() { return Nothing(); }


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

  // Modify the flags to include any changes to isolation.
  Flags flags_ = flags;
  flags_.isolation = isolation;

  LOG(INFO) << "Using isolation: " << isolation;

  // Create a MesosContainerizerProcess using isolators and a launcher.
  hashmap<string, Try<Isolator*> (*)(const Flags&)> creators;

  creators["posix/cpu"]   = &PosixCpuIsolatorProcess::create;
  creators["posix/mem"]   = &PosixMemIsolatorProcess::create;
  creators["posix/disk"]  = &PosixDiskIsolatorProcess::create;
#ifdef __linux__
  creators["cgroups/cpu"] = &CgroupsCpushareIsolatorProcess::create;
  creators["cgroups/mem"] = &CgroupsMemIsolatorProcess::create;
  creators["cgroups/perf_event"] = &CgroupsPerfEventIsolatorProcess::create;
  creators["filesystem/shared"] = &SharedFilesystemIsolatorProcess::create;
  creators["namespaces/pid"] = &NamespacesPidIsolatorProcess::create;
#endif // __linux__
#ifdef WITH_NETWORK_ISOLATOR
  creators["network/port_mapping"] = &PortMappingIsolatorProcess::create;
#endif

  vector<Owned<Isolator>> isolators;

  foreach (const string& type, strings::split(isolation, ",")) {
    if (creators.contains(type)) {
      Try<Isolator*> isolator = creators[type](flags_);
      if (isolator.isError()) {
        return Error(
            "Could not create isolator " + type + ": " + isolator.error());
      } else {
        isolators.push_back(Owned<Isolator>(isolator.get()));
      }
    } else if (ModuleManager::contains<Isolator>(type)) {
      Try<Isolator*> isolator = ModuleManager::create<Isolator>(type);
      if (isolator.isError()) {
        return Error(
            "Could not create isolator " + type + ": " + isolator.error());
      } else {
        if (type == "filesystem/shared") {
          // Filesystem isolator must be the first isolator used for prepare()
          // so any volume mounts are performed before anything else runs.
          isolators.insert(isolators.begin(), Owned<Isolator>(isolator.get()));
        } else {
          isolators.push_back(Owned<Isolator>(isolator.get()));
        }
      }
    } else {
      return Error("Unknown or unsupported isolator: " + type);
    }
  }

#ifdef __linux__
  // Determine which launcher to use based on the isolation flag.
  Try<Launcher*> launcher =
    (strings::contains(isolation, "cgroups") ||
     strings::contains(isolation, "network/port_mapping") ||
     strings::contains(isolation, "filesystem/shared") ||
     strings::contains(isolation, "namespaces"))
    ? LinuxLauncher::create(flags_)
    : PosixLauncher::create(flags_);
#else
  Try<Launcher*> launcher = PosixLauncher::create(flags_);
#endif // __linux__

  if (launcher.isError()) {
    return Error("Failed to create launcher: " + launcher.error());
  }

  return new MesosContainerizer(
      flags_, local, fetcher, Owned<Launcher>(launcher.get()), isolators);
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
  dispatch(process.get(), &MesosContainerizerProcess::destroy, containerId);
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
  list<ExecutorRunState> recoverable;
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

        LOG(INFO) << "Recovering container '" << containerId
                  << "' for executor '" << executor.id
                  << "' of framework " << framework.id;

        ExecutorRunState executorRunState(
            run.get().id.get(),
            run.get().forkedPid.get(),
            run.get().directory);

        recoverable.push_back(executorRunState);
      }
    }
  }

  // Try to recover the launcher first.
  return launcher->recover(recoverable)
    .then(defer(self(), &Self::_recover, recoverable));
}


Future<Nothing> MesosContainerizerProcess::_recover(
    const list<ExecutorRunState>& recoverable)
{
  // Then recover the isolators.
  list<Future<Nothing>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->recover(recoverable));
  }

  // If all isolators recover then continue.
  return collect(futures)
    .then(defer(self(), &Self::__recover, recoverable));
}


Future<Nothing> MesosContainerizerProcess::__recover(
    const list<ExecutorRunState>& recovered)
{
  foreach (const ExecutorRunState& run, recovered) {
    const ContainerID& containerId = run.id;

    Container* container = new Container();

    Future<Option<int>> status = process::reap(run.pid);
    status.onAny(defer(self(), &Self::reaped, containerId));
    container->status = status;

    container->directory = run.directory;

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

  return Nothing();
}


// Launching an executor involves the following steps:
// 1. Call prepare on each isolator.
// 2. Fork the executor. The forked child is blocked from exec'ing until it has
//    been isolated.
// 3. Isolate the executor. Call isolate with the pid for each isolator.
// 4. Fetch the executor.
// 4. Exec the executor. The forked child is signalled to continue. It will
//    first execute any preparation commands from isolators and then exec the
//    executor.
Future<bool> MesosContainerizerProcess::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (containers_.contains(containerId)) {
    LOG(ERROR) << "Cannot start already running container '"
               << containerId << "'";
    return Failure("Container already started");
  }

  // We support MESOS containers or ExecutorInfos with no
  // ContainerInfo given.
  if (executorInfo.has_container() &&
      executorInfo.container().type() != ContainerInfo::MESOS) {
    return false;
  }

  const CommandInfo& command = executorInfo.command();
  if (command.has_container()) {
    // We return false as this containerizer does not support
    // handling ContainerInfo.
    return false;
  }

  Container* container = new Container();
  container->resources = executorInfo.resources();
  container->directory = directory;
  container->state = PREPARING;
  containers_[containerId] = Owned<Container>(container);

  LOG(INFO) << "Starting container '" << containerId
            << "' for executor '" << executorInfo.executor_id()
            << "' of framework '" << executorInfo.framework_id() << "'";

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
                lambda::_1))
    .onFailed(defer(self(),
                    &Self::__launch,
                    containerId,
                    executorInfo,
                    lambda::_1));
}


void MesosContainerizerProcess::__launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& failure)
{
  LOG(ERROR) << "Failed to launch container '" << containerId
             << "' for executor '" << executorInfo.executor_id()
             << "' of framework '" << executorInfo.framework_id()
             << "': " << failure;

  destroy(containerId);
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


static list<Option<CommandInfo>> accumulate(
    list<Option<CommandInfo>> l,
    const Option<CommandInfo>& e)
{
  l.push_back(e);
  return l;
}


static Future<list<Option<CommandInfo>>> _prepare(
    const Owned<Isolator>& isolator,
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const list<Option<CommandInfo>> commands)
{
  // Propagate any failure.
  return isolator->prepare(containerId, executorInfo, directory, user)
    .then(lambda::bind(&accumulate, commands, lambda::_1));
}


Future<list<Option<CommandInfo>>> MesosContainerizerProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user)
{
  // We prepare the isolators sequentially according to their ordering
  // to permit basic dependency specification, e.g., preparing a
  // filesystem isolator before other isolators.
  Future<list<Option<CommandInfo>>> f = list<Option<CommandInfo>>();

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

  return f;
}


Future<Nothing> MesosContainerizerProcess::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container is already destroyed");
  }

  return fetcher->fetch(
      containerId,
      commandInfo,
      directory,
      user,
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
    const list<Option<CommandInfo>>& commands)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container has been destroyed");
  }

  if (containers_[containerId]->state == DESTROYING) {
    return Failure("Container is currently being destroyed");
  }

  // Prepare environment variables for the executor.
  map<string, string> env = executorEnvironment(
      executorInfo,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      flags.recovery_timeout);

  // Include any enviroment variables from CommandInfo.
  foreach (const Environment::Variable& variable,
           executorInfo.command().environment().variables()) {
    env[variable.name()] = variable.value();
  }

  // Use a pipe to block the child until it's been isolated.
  int pipes[2];

  // We assume this should not fail under reasonable conditions so we
  // use CHECK.
  CHECK(pipe(pipes) == 0);

  // Prepare the flags to pass to the launch process.
  MesosContainerizerLaunch::Flags launchFlags;

  launchFlags.command = JSON::Protobuf(executorInfo.command());
  launchFlags.directory = directory;
  launchFlags.user = user;
  launchFlags.pipe_read = pipes[0];
  launchFlags.pipe_write = pipes[1];

  // Prepare the additional preparation commands.
  // TODO(jieyu): Use JSON::Array once we have generic parse support.
  JSON::Object object;
  JSON::Array array;
  foreach (const Option<CommandInfo>& command, commands) {
    if (command.isSome()) {
      array.values.push_back(JSON::Protobuf(command.get()));
    }
  }
  object.values["commands"] = array;

  launchFlags.commands = object;

  // Fork the child using launcher.
  vector<string> argv(2);
  argv[0] = "mesos-containerizer";
  argv[1] = MesosContainerizerLaunch::NAME;

  Try<pid_t> forked = launcher->fork(
      containerId,
      path::join(flags.launcher_dir, "mesos-containerizer"),
      argv,
      Subprocess::FD(STDIN_FILENO),
      (local ? Subprocess::FD(STDOUT_FILENO)
             : Subprocess::PATH(path::join(directory, "stdout"))),
      (local ? Subprocess::FD(STDERR_FILENO)
             : Subprocess::PATH(path::join(directory, "stderr"))),
      launchFlags,
      env,
      None());

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
                user))
    .then(defer(self(), &Self::exec, containerId, pipes[1]))
    .onAny(lambda::bind(&os::close, pipes[0]))
    .onAny(lambda::bind(&os::close, pipes[1]));
}


static Future<bool> _isolate()
{
  return true;
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

  return future.then(lambda::bind(&_isolate));
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
    const Resources& _resources)
{
  if (!containers_.contains(containerId)) {
    // It is not considered a failure if the container is not known
    // because the slave will attempt to update the container's
    // resources on a task's terminal state change but the executor
    // may have already exited and the container cleaned up.
    LOG(WARNING) << "Ignoring update for unknown container: " << containerId;
    return Nothing();
  }

  if (containers_[containerId]->state == DESTROYING) {
    LOG(WARNING) << "Ignoring update for currently being destroyed container: "
                 << containerId;
    return Nothing();
  }

  // Store the resources for usage().
  containers_[containerId]->resources = _resources;

  // Update each isolator.
  list<Future<Nothing>> futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->update(containerId, _resources));
  }

  // Wait for all isolators to complete.
  return collect(futures)
    .then(lambda::bind(&_nothing));
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


void MesosContainerizerProcess::destroy(const ContainerID& containerId)
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
    // We cannot simply terminate the container if it's preparing
    // since isolator's prepare doesn't need any cleanup.
    containerizer::Termination termination;
    termination.set_killed(true);
    termination.set_message("Container destroyed while preparing isolators");
    container->promise.set(termination);

    containers_.erase(containerId);

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


void MesosContainerizerProcess::_destroy(const ContainerID& containerId)
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
    .onAny(defer(self(), &Self::___destroy, containerId, lambda::_1));
}


static list<Future<Nothing>> _cleanup(const list<Future<Nothing>>& cleanups)
{
  return cleanups;
}


static Future<list<Future<Nothing>>> cleanup(
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
    .then(lambda::bind(&_cleanup, cleanups));
}


// TODO(idownes): Use a reversed view of the container rather than
// reversing a copy.
template <typename T>
static T reversed(const T& t)
{
  T r = t;
  std::reverse(r.begin(), r.end());
  return r;
}


void MesosContainerizerProcess::___destroy(
    const ContainerID& containerId,
    const Future<Option<int>>& status)
{
  // We clean up each isolator in the reverse order they were
  // prepared (see comment in prepare()).
  Future<list<Future<Nothing>>> f = list<Future<Nothing>>();

  foreach (const Owned<Isolator>& isolator, reversed(isolators)) {
    // We'll try to clean up all isolators, waiting for each to
    // complete and continuing if one fails.
    f = f.then(lambda::bind(&cleanup,
                            isolator,
                            containerId,
                            lambda::_1));
  }

  // Continue destroy when we're done trying to clean up.
  f.onAny(defer(self(),
                &Self::____destroy,
                containerId,
                status,
                lambda::_1));

  return;
}


void MesosContainerizerProcess::____destroy(
    const ContainerID& containerId,
    const Future<Option<int>>& status,
    const Future<list<Future<Nothing>>>& cleanups)
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
        stringify(containerId) + "' :" +
        (cleanup.isFailed() ? cleanup.failure() : "discarded future"));

      containers_.erase(containerId);

      ++metrics.container_destroy_errors;

      return;
    }
  }

  // A container is 'killed' if any isolator limited it.
  // Note: We may not see a limitation in time for it to be
  // registered. This could occur if the limitation (e.g., an OOM)
  // killed the executor and we triggered destroy() off the executor
  // exit.
  bool killed = false;
  string message;
  if (container->limitations.size() > 0) {
    killed = true;
    foreach (const Limitation& limitation, container->limitations) {
      message += limitation.message;
    }
    message = strings::trim(message);
  } else {
    message = "Executor terminated";
  }

  containerizer::Termination termination;
  termination.set_killed(killed);
  termination.set_message(message);
  if (status.isReady() && status.get().isSome()) {
    termination.set_status(status.get().get());
  }

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
    const Future<Limitation>& future)
{
  if (!containers_.contains(containerId) ||
      containers_[containerId]->state == DESTROYING) {
    return;
  }

  if (future.isReady()) {
    LOG(INFO) << "Container " << containerId << " has reached its limit for"
              << " resource " << future.get().resources
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


} // namespace slave {
} // namespace internal {
} // namespace mesos {
