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
#include <map>
#include <string>
#include <vector>

#include <process/check.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/io.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/fs.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>

#include "common/status_utils.hpp"

#include "docker/docker.hpp"

#ifdef __linux__
#include "linux/cgroups.hpp"
#endif // __linux__

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"


#include "slave/containerizer/isolators/cgroups/constants.hpp"

#include "usage/usage.hpp"


using std::list;
using std::map;
using std::string;
using std::vector;

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

using state::SlaveState;
using state::FrameworkState;
using state::ExecutorState;
using state::RunState;


// Declared in header, see explanation there.
// TODO(benh): At some point to run multiple slaves we'll need to make
// the Docker container name creation include the slave ID.
const string DOCKER_NAME_PREFIX = "mesos-";

// Declared in header, see explanation there.
const string DOCKER_SYMLINK_DIRECTORY = "docker/links";

// Parse the ContainerID from a Docker container and return None if
// the container was not launched from Mesos.
Option<ContainerID> parse(const Docker::Container& container)
{
  Option<string> name = None();

  if (strings::startsWith(container.name, DOCKER_NAME_PREFIX)) {
    name = strings::remove(
        container.name, DOCKER_NAME_PREFIX, strings::PREFIX);
  } else if (strings::startsWith(container.name, "/" + DOCKER_NAME_PREFIX)) {
    name = strings::remove(
        container.name, "/" + DOCKER_NAME_PREFIX, strings::PREFIX);
  }

  if (name.isSome()) {
    ContainerID id;
    id.set_value(name.get());
    return id;
  }

  return None();
}


Try<DockerContainerizer*> DockerContainerizer::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  Try<Docker*> docker = Docker::create(flags.docker);
  if (docker.isError()) {
    return Error(docker.error());
  }

  return new DockerContainerizer(
      flags,
      fetcher,
      Shared<Docker>(docker.get()));
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
    Shared<Docker> docker)
  : process(new DockerContainerizerProcess(flags, fetcher, docker))
{
  spawn(process.get());
}


DockerContainerizer::~DockerContainerizer()
{
  terminate(process.get());
  process::wait(process.get());
}


Try<DockerContainerizerProcess::Container*>
DockerContainerizerProcess::Container::create(
    const ContainerID& id,
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
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

  if (user.isSome()) {
    Try<Nothing> chown = os::chown(user.get(), directory);

    if (chown.isError()) {
      return Error("Failed to chown: " + chown.error());
    }
  }

  string dockerSymlinkPath = path::join(
      paths::getSlavePath(flags.work_dir, slaveId),
      DOCKER_SYMLINK_DIRECTORY);

  if (!os::exists(dockerSymlinkPath)) {
    Try<Nothing> mkdir = os::mkdir(dockerSymlinkPath);
    if (mkdir.isError()) {
      return Error("Unable to create symlink folder for docker " +
                   dockerSymlinkPath + ": " + mkdir.error());
    }
  }

  bool symlinked = false;
  string containerWorkdir = directory;
  // We need to symlink the sandbox directory if the directory
  // path has a colon, as Docker CLI uses the colon as a seperator.
  if (strings::contains(directory, ":")) {
    containerWorkdir = path::join(dockerSymlinkPath, id.value());

    Try<Nothing> symlink = ::fs::symlink(directory, containerWorkdir);

    if (symlink.isError()) {
      return Error("Failed to symlink directory '" + directory +
                   "' to '" + containerWorkdir + "': " + symlink.error());
    }

    symlinked = true;
  }

  return new Container(
      id,
      taskInfo,
      executorInfo,
      containerWorkdir,
      user,
      slaveId,
      slavePid,
      checkpoint,
      symlinked,
      flags);
}


Future<Nothing> DockerContainerizerProcess::fetch(
    const ContainerID& containerId)
{
  CHECK(containers_.contains(containerId));
  Container* container = containers_[containerId];

  return fetcher->fetch(
      containerId,
      container->command(),
      container->directory,
      None(),
      flags);
}


Future<Nothing> DockerContainerizerProcess::pull(
    const ContainerID& containerId,
    const string& directory,
    const string& image,
    bool forcePullImage)
{
  Future<Docker::Image> future = docker->pull(directory, image, forcePullImage);
  containers_[containerId]->pull = future;
  return future.then(defer(self(), &Self::_pull, image));
}


Future<Nothing> DockerContainerizerProcess::_pull(const string& image)
{
  VLOG(1) << "Docker pull " << image << " completed";
  return Nothing();
}


Try<Nothing> DockerContainerizerProcess::checkpoint(
    const ContainerID& containerId,
    pid_t pid)
{
  CHECK(containers_.contains(containerId));

  Container* container = containers_[containerId];

  if (container->checkpoint) {
    const string& path =
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          container->slaveId,
          container->executor.framework_id(),
          container->executor.executor_id(),
          containerId);

    LOG(INFO) << "Checkpointing pid " << pid << " to '" << path <<  "'";

    return slave::state::checkpoint(path, stringify(pid));
  }

  return Nothing();
}


Future<Nothing> DockerContainerizer::recover(
    const Option<SlaveState>& state)
{
  return dispatch(process.get(), &DockerContainerizerProcess::recover, state);
}


Future<bool> DockerContainerizer::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::launch,
      containerId,
      executorInfo,
      directory,
      user,
      slaveId,
      slavePid,
      checkpoint);
}


Future<bool> DockerContainerizer::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
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
      slavePid,
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
      resources);
}


Future<ResourceStatistics> DockerContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::usage,
      containerId);
}


Future<containerizer::Termination> DockerContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(
      process.get(),
      &DockerContainerizerProcess::wait,
      containerId);
}


void DockerContainerizer::destroy(const ContainerID& containerId)
{
  dispatch(
      process.get(),
      &DockerContainerizerProcess::destroy,
      containerId, true);
}


Future<hashset<ContainerID>> DockerContainerizer::containers()
{
  return dispatch(process.get(), &DockerContainerizerProcess::containers);
}


// A Subprocess async-safe "setup" helper used by
// DockerContainerizerProcess when launching the mesos-executor that
// does a 'setsid' and then synchronizes with the parent.
static int setup(const string& directory)
{
  // Put child into its own process session to prevent slave suicide
  // on child process SIGKILL/SIGTERM.
  if (::setsid() == -1) {
    return errno;
  }

  // Run the process in the specified directory.
  if (!directory.empty()) {
    if (::chdir(directory.c_str()) == -1) {
      return errno;
    }
  }

  // Synchronize with parent process by reading a byte from stdin.
  char c;
  ssize_t length;
  while ((length = read(STDIN_FILENO, &c, sizeof(c))) == -1 && errno == EINTR);

  if (length != sizeof(c)) {
    // This will occur if the slave terminates during executor launch.
    // There's a reasonable probability this will occur during slave
    // restarts across a large/busy cluster.
    ABORT("Failed to synchronize with slave (it has probably exited)");
  }

  return 0;
}


Future<Nothing> DockerContainerizerProcess::recover(
    const Option<SlaveState>& state)
{
  LOG(INFO) << "Recovering Docker containers";

  if (state.isSome()) {
    // Collection of pids that we've started reaping in order to
    // detect very unlikely duplicate scenario (see below).
    hashmap<ContainerID, pid_t> pids;

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
        CHECK_EQ(containerId, run.get().id.get());

        // We need the pid so the reaper can monitor the executor so
        // skip this executor if it's not present. This is not an
        // error because the slave will try to wait on the container
        // which will return a failed Termination and everything will
        // get cleaned up.
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

        // Create and store a container.
        Container* container = new Container(containerId);
        containers_[containerId] = container;

        container->state = Container::RUNNING;

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
      }
    }
  }

  // Get the list of all Docker containers (running and exited) in
  // order to remove any orphans.
  return docker->ps(true, DOCKER_NAME_PREFIX)
    .then(defer(self(), &Self::_recover, lambda::_1));
}


Future<Nothing> DockerContainerizerProcess::_recover(
    const list<Docker::Container>& _containers)
{
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
      // TODO(tnachen): Consider using executor_shutdown_grace_period.
      docker->stop(container.id, flags.docker_stop_timeout, true);
    }
  }

  return Nothing();
}


Future<bool> DockerContainerizerProcess::launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (containers_.contains(containerId)) {
    return Failure("Container already started");
  }

  if (!taskInfo.has_container()) {
    LOG(INFO) << "No container info found, skipping launch";
    return false;
  }

  ContainerInfo containerInfo = taskInfo.container();

  if (containerInfo.type() != ContainerInfo::DOCKER) {
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
      slavePid,
      checkpoint,
      flags);

  if (container.isError()) {
    return Failure("Failed to create container: " + container.error());
  }

  containers_[containerId] = container.get();

  LOG(INFO) << "Starting container '" << containerId
            << "' for task '" << taskInfo.task_id()
            << "' (and executor '" << executorInfo.executor_id()
            << "') of framework '" << executorInfo.framework_id() << "'";

  return fetch(containerId)
    .then(defer(self(), &Self::_launch, containerId))
    .then(defer(self(), &Self::__launch, containerId))
    .then(defer(self(), &Self::___launch, containerId))
    .then(defer(self(), &Self::______launch, containerId, lambda::_1))
    .onFailed(defer(self(), &Self::destroy, containerId, true));
}


Future<Nothing> DockerContainerizerProcess::_launch(
    const ContainerID& containerId)
{
  // Doing the fetch might have succeded but we were actually asked to
  // destroy the container, which we did, so don't continue.
  if (!containers_.contains(containerId)) {
    return Failure("Container was destroyed while launching");
  }

  Container* container = containers_[containerId];

  container->state = Container::PULLING;

  return pull(
      containerId,
      container->directory,
      container->image(),
      container->forcePullImage());
}


Future<Nothing> DockerContainerizerProcess::__launch(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container was destroyed while pulling image");
  }

  Container* container = containers_[containerId];

  container->state = Container::RUNNING;

  // Try and start the Docker container.
  return container->run = docker->run(
      container->container(),
      container->command(),
      container->name(),
      container->directory,
      flags.docker_sandbox_directory,
      container->resources,
      container->environment());
}


Future<pid_t> DockerContainerizerProcess::___launch(
    const ContainerID& containerId)
{
  // After we do Docker::run we shouldn't remove a container until
  // after we set Container::status.
  CHECK(containers_.contains(containerId));

  Container* container = containers_[containerId];

  // Prepare environment variables for the executor.
  map<string, string> environment = executorEnvironment(
      container->executor,
      container->directory,
      container->slaveId,
      container->slavePid,
      container->checkpoint,
      flags.recovery_timeout);

  // Include any enviroment variables from ExecutorInfo.
  foreach (const Environment::Variable& variable,
           container->executor.command().environment().variables()) {
    environment[variable.name()] = variable.value();
  }

  // Construct the mesos-executor "override" to do a 'docker wait'
  // using the "name" we gave the container (to distinguish it from
  // Docker containers not created by Mesos). Note, however, that we
  // don't want the exit status from 'docker wait' but rather the exit
  // status from the container, hence the use of /bin/bash.
  string override =
    "/bin/sh -c 'exit `" + flags.docker + " wait " + container->name() + "`'";

  Try<Subprocess> s = subprocess(
      container->executor.command().value() + " --override " + override,
      Subprocess::PIPE(),
      Subprocess::PATH(path::join(container->directory, "stdout")),
      Subprocess::PATH(path::join(container->directory, "stderr")),
      environment,
      lambda::bind(&setup, container->directory));

  if (s.isError()) {
    return Failure("Failed to fork executor: " + s.error());
  }

  // Checkpoint the executor's pid (if necessary).
  Try<Nothing> checkpointed = checkpoint(containerId, s.get().pid());

  if (checkpointed.isError()) {
    // Close the subprocess's stdin so that it aborts.
    CHECK_SOME(s.get().in());
    os::close(s.get().in().get());

    return Failure(
        "Failed to checkpoint executor's pid: " + checkpointed.error());
  }

  // Checkpoing complete, now synchronize with the process so that it
  // can continue to execute.
  CHECK_SOME(s.get().in());
  char c;
  ssize_t length;
  while ((length = write(s.get().in().get(), &c, sizeof(c))) == -1 &&
         errno == EINTR);

  if (length != sizeof(c)) {
    string error = string(strerror(errno));
    os::close(s.get().in().get());
    return Failure("Failed to synchronize with child process: " + error);
  }

  return s.get().pid();
}


Future<bool> DockerContainerizerProcess::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (containers_.contains(containerId)) {
    return Failure("Container already started");
  }

  if (!executorInfo.has_container()) {
    LOG(INFO) << "No container info found, skipping launch";
    return false;
  }

  ContainerInfo containerInfo = executorInfo.container();

  if (containerInfo.type() != ContainerInfo::DOCKER) {
    LOG(INFO) << "Skipping non-docker container";
    return false;
  }

  Try<Container*> container = Container::create(
      containerId,
      None(),
      executorInfo,
      directory,
      user,
      slaveId,
      slavePid,
      checkpoint,
      flags);

  if (container.isError()) {
    return Failure("Failed to create container: " + container.error());
  }

  containers_[containerId] = container.get();

  LOG(INFO) << "Starting container '" << containerId
            << "' for executor '" << executorInfo.executor_id()
            << "' and framework '" << executorInfo.framework_id() << "'";

  return fetch(containerId)
    .then(defer(self(), &Self::_launch, containerId))
    .then(defer(self(), &Self::__launch, containerId))
    .then(defer(self(), &Self::____launch, containerId))
    .then(defer(self(), &Self::_____launch, containerId, lambda::_1))
    .then(defer(self(), &Self::______launch, containerId, lambda::_1))
    .onFailed(defer(self(), &Self::destroy, containerId, true));
}


Future<Docker::Container> DockerContainerizerProcess::____launch(
    const ContainerID& containerId)
{
  // After we do Docker::run we shouldn't remove a container until
  // after we set Container::status.
  CHECK(containers_.contains(containerId));

  Container* container = containers_[containerId];

  return docker->inspect(container->name());
}


Future<pid_t> DockerContainerizerProcess::_____launch(
    const ContainerID& containerId,
    const Docker::Container& container)
{
  // After we do Docker::run we shouldn't remove a container until
  // after we set Container::status.
  CHECK(containers_.contains(containerId));

  Option<int> pid = container.pid;

  if (!pid.isSome()) {
    return Failure("Unable to get executor pid after launch");
  }

  // TODO(tnachen): We might not be able to checkpoint if the slave
  // dies before it can checkpoint while the executor is still
  // running. Optinally we can consider recording the slave id and
  // executor id as part of the docker container name so we can
  // recover from this.

  Try<Nothing> checkpointed = checkpoint(containerId, pid.get());

  if (checkpointed.isError()) {
    return Failure(
        "Failed to checkpoint executor's pid: " + checkpointed.error());
  }

  return pid.get();
}


Future<bool> DockerContainerizerProcess::______launch(
    const ContainerID& containerId,
    pid_t pid)
{
  // After we do Docker::run we shouldn't remove a container until
  // after we set 'status', which we do in this function.
  CHECK(containers_.contains(containerId));

  Container* container = containers_[containerId];

  // And finally watch for when the container gets reaped.
  container->status.set(process::reap(pid));

  container->status.future().get()
    .onAny(defer(self(), &Self::reaped, containerId));

  // TODO(benh): Check failure of Docker::logs.
  docker->logs(container->name(), container->directory);

  return true;
}


Future<Nothing> DockerContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& _resources)
{
  if (!containers_.contains(containerId)) {
    LOG(WARNING) << "Ignoring updating unknown container: "
                 << containerId;
    return Nothing();
  }

  Container* container = containers_[containerId];

  if (container->state == Container::DESTROYING)  {
    LOG(INFO) << "Ignoring updating container '" << containerId
              << "' that is being destroyed";
    return Nothing();
  }

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

  return docker->inspect(containers_[containerId]->name())
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

  containers_[containerId]->pid = container.pid.get();

  return __update(containerId, _resources, container.pid.get());
}


Future<Nothing> DockerContainerizerProcess::__update(
    const ContainerID& containerId,
    const Resources& _resources,
    pid_t pid)
{
#ifdef __linux__
  // Determine the the cgroups hierarchies where the 'cpu' and
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
                 << " does not appear to be a member of a cgroup "
                 << "where the 'cpu' subsystem is mounted";
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
  }

  // Now determine the cgroup for the 'memory' subsystem.
  Result<string> memoryCgroup = cgroups::memory::cgroup(pid);

  if (memoryCgroup.isError()) {
    return Failure("Failed to determine cgroup for the 'memory' subsystem: " +
                   memoryCgroup.error());
  } else if (memoryCgroup.isNone()) {
    LOG(WARNING) << "Container " << containerId
                 << " does not appear to be a member of a cgroup "
                 << "where the 'memory' subsystem is mounted";
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
#ifndef __linux__
  return Failure("Does not support usage() on non-linux platform");
#else
  if (!containers_.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  Container* container = containers_[containerId];

  if (container->state == Container::DESTROYING) {
    return Failure("Container is being removed: " + stringify(containerId));
  }

  // Skip inspecting the docker container if we already have the pid.
  if (container->pid.isSome()) {
    return __usage(containerId, container->pid.get());
  }

  return docker->inspect(container->name())
    .then(defer(self(), &Self::_usage, containerId, lambda::_1));
#endif // __linux__
}


Future<ResourceStatistics> DockerContainerizerProcess::_usage(
    const ContainerID& containerId,
    const Docker::Container& _container)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container has been destroyed:" + stringify(containerId));
  }

  Container* container = containers_[containerId];

  if (container->state == Container::DESTROYING) {
    return Failure("Container is being removed: " + stringify(containerId));
  }

  Option<pid_t> pid = _container.pid;
  if (pid.isNone()) {
    return Failure("Container is not running");
  }

  container->pid = pid;

  return __usage(containerId, pid.get());
}


Future<ResourceStatistics> DockerContainerizerProcess::__usage(
    const ContainerID& containerId,
    pid_t pid)
{
  Container* container = containers_[containerId];

  // Note that here getting the root pid is enough because
  // the root process acts as an 'init' process in the docker
  // container, so no other child processes will escape it.
  Try<ResourceStatistics> statistics = mesos::internal::usage(pid, true, true);
  if (statistics.isError()) {
    return Failure(statistics.error());
  }

  ResourceStatistics result = statistics.get();

  // Set the resource allocations.
  const Resources& resource = container->resources;
  Option<Bytes> mem = resource.mem();
  if (mem.isSome()) {
    result.set_mem_limit_bytes(mem.get().bytes());
  }

  Option<double> cpus = resource.cpus();
  if (cpus.isSome()) {
    result.set_cpus_limit(cpus.get());
  }

  return result;
}


Future<containerizer::Termination> DockerContainerizerProcess::wait(
    const ContainerID& containerId)
{
  if (!containers_.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  return containers_[containerId]->termination.future();
}


void DockerContainerizerProcess::destroy(
    const ContainerID& containerId,
    bool killed)
{
  if (!containers_.contains(containerId)) {
    LOG(WARNING) << "Ignoring destroy of unknown container: " << containerId;
    return;
  }

  Container* container = containers_[containerId];

  if (container->run.isFailed()) {
    VLOG(1) << "Container '" << containerId << "' run failed";

    // This means we failed to do Docker::run and we're trying to
    // cleanup (or someone happens to have asked to destroy this
    // container before the destroy that we enqueued has had a chance
    // to get executed, which when it does, will just be skipped).
    CHECK_PENDING(container->status.future());
    containerizer::Termination termination;
    termination.set_killed(killed);
    termination.set_message(
        "Failed to run container: " + container->run.failure());
    container->termination.set(termination);

    containers_.erase(containerId);
    delete container;

    return;
  }

  if (container->state == Container::DESTROYING) {
    // Destroy has already been initiated.
    return;
  }

  LOG(INFO) << "Destroying container '" << containerId << "'";

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
  // If we're RUNNING, we want to wait for the status to get set, then
  // do a Docker::kill, then wait for the status to complete, then
  // cleanup.

  if (container->state == Container::FETCHING) {
    LOG(INFO) << "Destroying Container '"
              << containerId << "' in FETCHING state";

    fetcher->kill(containerId);

    containerizer::Termination termination;
    termination.set_killed(killed);
    termination.set_message("Container destroyed while fetching");
    container->termination.set(termination);

    // Even if the fetch succeeded just before we did the killtree,
    // removing the container here means that we won't proceed with
    // the Docker::run.
    containers_.erase(containerId);
    delete container;

    return;
  }

  if (container->state == Container::PULLING) {
    LOG(INFO) << "Destroying Container '"
              << containerId << "' in PULLING state";

    container->pull.discard();

    containerizer::Termination termination;
    termination.set_killed(killed);
    termination.set_message("Container destroyed while pulling image");
    container->termination.set(termination);

    containers_.erase(containerId);
    delete container;

    return;
  }

  CHECK(container->state == Container::RUNNING);

  container->state = Container::DESTROYING;

  // Otherwise, wait for Docker::run to succeed, in which case we'll
  // continue in _destroy (calling Docker::kill) or for Docker::run to
  // fail, in which case we'll re-execute this function and cleanup
  // above.

  container->status.future()
    .onAny(defer(self(), &Self::_destroy, containerId, killed));
}


void DockerContainerizerProcess::_destroy(
    const ContainerID& containerId,
    bool killed)
{
  CHECK(containers_.contains(containerId));

  Container* container = containers_[containerId];

  CHECK(container->state == Container::DESTROYING);

  // Do a 'docker rm -f' which we'll then find out about in '_destroy'
  // after we've reaped either the container's root process (in the
  // event that we had just launched a container for an executor) or
  // the mesos-executor (in the case we launched a container for a
  // task). As a reminder, the mesos-executor exits because it's doing
  // a 'docker wait' on the container using the --override flag of
  // mesos-executor.

  LOG(INFO) << "Running docker stop on container '" << containerId << "'";

  docker->stop(container->name(),
              flags.docker_stop_timeout)
    .onAny(defer(self(), &Self::__destroy, containerId, killed, lambda::_1));
}


void DockerContainerizerProcess::__destroy(
    const ContainerID& containerId,
    bool killed,
    const Future<Nothing>& kill)
{
  CHECK(containers_.contains(containerId));

  Container* container = containers_[containerId];

  if (!kill.isReady()) {
    // TODO(benh): This means we've failed to do a Docker::kill, which
    // means it's possible that the container is still going to be
    // running after we return! We either need to have a periodic
    // "garbage collector", or we need to retry the Docker::kill
    // indefinitely until it has been sucessful.
    container->termination.fail(
        "Failed to kill the Docker container: " +
        (kill.isFailed() ? kill.failure() : "discarded future"));

    containers_.erase(containerId);

    delay(flags.docker_remove_delay, self(), &Self::remove, container->name());

    delete container;

    return;
  }

  // Status must be ready since we did a Docker::kill.
  CHECK_READY(containers_[containerId]->status.future());

  container->status.future().get()
    .onAny(defer(self(), &Self::___destroy, containerId, killed, lambda::_1));
}


void DockerContainerizerProcess::___destroy(
    const ContainerID& containerId,
    bool killed,
    const Future<Option<int>>& status)
{
  CHECK(containers_.contains(containerId));

  Container* container = containers_[containerId];

  containerizer::Termination termination;
  termination.set_killed(killed);

  if (status.isReady() && status.get().isSome()) {
    termination.set_status(status.get().get());
  }

  termination.set_message(
      killed ? "Container killed" : "Container terminated");

  container->termination.set(termination);

  containers_.erase(containerId);

  delay(flags.docker_remove_delay, self(), &Self::remove, container->name());

  delete container;
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

  LOG(INFO) << "Executor for container '" << containerId << "' has exited";

  // The executor has exited so destroy the container.
  destroy(containerId, false);
}


void DockerContainerizerProcess::remove(const string& container)
{
  docker->rm(container, true);
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
