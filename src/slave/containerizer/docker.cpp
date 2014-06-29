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

#include <process/defer.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>

#include "docker/docker.hpp"

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/docker.hpp"

#include "usage/usage.hpp"


using std::list;
using std::map;
using std::string;

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
string DOCKER_NAME_PREFIX = "mesos-";


class DockerContainerizerProcess
  : public process::Process<DockerContainerizerProcess>
{
public:
  DockerContainerizerProcess(
      const Flags& flags,
      bool local,
      const Docker& docker)
    : flags(flags),
      docker(docker) {}

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual void destroy(
      const ContainerID& containerId,
      const bool& killed = true);

  virtual process::Future<hashset<ContainerID> > containers();

private:
  // Continuations and helpers.
  process::Future<Nothing> _recover(
      const std::list<Docker::Container>& containers);

  process::Future<bool> _launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  void _destroy(
      const ContainerID& containerId,
      const bool& killed,
      const Future<Option<int> >& future);

  void __destroy(
      const ContainerID& containerId,
      const bool& killed,
      const Future<Option<int > >& status);

  Future<ResourceStatistics> _usage(
    const ContainerID& containerId,
    const Future<Docker::Container> container);

  // Call back for when the executor exits. This will trigger
  // container destroy.
  void reaped(const ContainerID& containerId);

  // Parse the ContainerID from a Docker container and return None if
  // the container was not launched from Mesos.
  Option<ContainerID> parse(const Docker::Container& container);

  const Flags flags;

  Docker docker;

  // TODO(idownes): Consider putting these per-container variables into a
  // struct.
  // Promises for futures returned from wait().
  hashmap<ContainerID,
    process::Owned<process::Promise<containerizer::Termination> > > promises;

  // We need to keep track of the future exit status for each executor because
  // we'll only get a single notification when the executor exits.
  hashmap<ContainerID, process::Future<Option<int> > > statuses;

  // We keep track of the resources for each container so we can set the
  // ResourceStatistics limits in usage().
  hashmap<ContainerID, Resources> resources;

  // Set of containers that are in process of being destroyed.
  hashset<ContainerID> destroying;
};


Try<DockerContainerizer*> DockerContainerizer::create(
    const Flags& flags,
    bool local)
{
  Docker docker(flags.docker);
  Try<Nothing> validation = Docker::validate(docker);
  if (validation.isError()) {
    return Error(validation.error());
  }

  return new DockerContainerizer(flags, local, docker);
}


DockerContainerizer::DockerContainerizer(
    const Flags& flags,
    bool local,
    const Docker& docker)
{
  process = new DockerContainerizerProcess(flags, local, docker);
  spawn(process);
}


DockerContainerizer::~DockerContainerizer()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Nothing> DockerContainerizer::recover(
    const Option<SlaveState>& state)
{
  return dispatch(process, &DockerContainerizerProcess::recover, state);
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
  return dispatch(process,
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
  return dispatch(process,
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
  return dispatch(process,
                  &DockerContainerizerProcess::update,
                  containerId,
                  resources);
}


Future<ResourceStatistics> DockerContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(process, &DockerContainerizerProcess::usage, containerId);
}


Future<containerizer::Termination> DockerContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process, &DockerContainerizerProcess::wait, containerId);
}


void DockerContainerizer::destroy(const ContainerID& containerId)
{
  dispatch(process, &DockerContainerizerProcess::destroy, containerId, true);
}


Future<hashset<ContainerID> > DockerContainerizer::containers()
{
  return dispatch(process, &DockerContainerizerProcess::containers);
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

        // Save a termination promise.
        Owned<Promise<containerizer::Termination> > promise(
            new Promise<containerizer::Termination>());

        promises.put(containerId, promise);

        CHECK_SOME(run.get().forkedPid);
        pid_t pid = run.get().forkedPid.get();

        Future<Option<int > > status = process::reap(pid);

        statuses[containerId] = status;
        status.onAny(defer(self(), &Self::reaped, containerId));

        if (pids.containsValue(pid)) {
          // This should (almost) never occur. There is the
          // possibility that a new executor is launched with the same
          // pid as one that just exited (highly unlikely) and the
          // slave dies after the new executor is launched but before
          // it hears about the termination of the earlier executor
          // (also unlikely). Regardless, the launcher can't do
          // anything sensible so this is considered an error.
          return Failure(
              "Detected duplicate pid " + stringify(pid) +
              " for container " + stringify(containerId));
        }

        pids.put(containerId, pid);
      }
    }
  }

  // Get the list of Docker containers in order to Remove any orphans.
  return docker.ps()
    .then(defer(self(), &Self::_recover, lambda::_1));
}


Future<Nothing> DockerContainerizerProcess::_recover(
    const list<Docker::Container>& containers)
{
  foreach (const Docker::Container& container, containers) {
    VLOG(1) << "Checking if Docker container named '"
            << container.name() << "' was started by Mesos";

    Option<ContainerID> id = parse(container);

    // Ignore containers that Mesos didn't start.
    if (id.isNone()) {
      continue;
    }

    VLOG(1) << "Checking if Mesos container with ID '"
            << stringify(id.get()) << "' has been orphaned";

    // Check if we're watching an executor for this container ID and
    // if not, rm -f the Docker container.
    if (!statuses.keys().contains(id.get())) {
      // TODO(benh): Retry 'docker rm -f' if it failed but the container
      // still exists (asynchronously).
      docker.rm(container.id());
    }
  }

  return Nothing();
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
  // TODO(benh): Implement support for launching an ExecutorInfo.
  return false;
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
  if (promises.contains(containerId)) {
    LOG(ERROR) << "Cannot start already running container '"
               << containerId << "'";
    return Failure("Container already started");
  }

  if (!taskInfo.has_command()) {
    return false;
  }

  const CommandInfo& command = taskInfo.command();

  // Check if we should try and launch this command.
  if (!command.has_container() ||
      !strings::startsWith(command.container().image(), "docker")) {
    return false;
  }

  Owned<Promise<containerizer::Termination> > promise(
      new Promise<containerizer::Termination>());

  promises.put(containerId, promise);

  // Store the resources for usage().
  resources.put(containerId, taskInfo.resources());

  LOG(INFO) << "Starting container '" << containerId
            << "' for task '" << taskInfo.task_id()
            << "' (and executor '" << executorInfo.executor_id()
            << "') of framework '" << executorInfo.framework_id() << "'";

  // Extract the Docker image.
  string image = command.container().image();
  image = strings::remove(image, "docker://", strings::PREFIX);

  // Construct the Docker container name.
  string name = DOCKER_NAME_PREFIX + stringify(containerId);

  // Start a docker container then launch the executor (but destroy
  // the Docker container if launching the executor failed).
  return docker.run(image, command.value(), name)
    .then(defer(self(),
                &Self::_launch,
                containerId,
                taskInfo,
                executorInfo,
                directory,
                slaveId,
                slavePid,
                checkpoint))
    .onFailed(defer(self(), &Self::destroy, containerId, false));
}


Future<bool> DockerContainerizerProcess::_launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
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

  // Construct the mesos-executor "override" to do a 'docker wait'
  // using the "name" we gave the container (to distinguish it from
  // Docker containers not created by Mesos).
  // TODO(benh): Get full path to 'docker'.
  string override =
    "docker wait " + DOCKER_NAME_PREFIX + stringify(containerId);

  Try<Subprocess> s = subprocess(
      executorInfo.command().value() + " --override " + override,
      Subprocess::PIPE(),
      Subprocess::PATH(path::join(directory, "stdout")),
      Subprocess::PATH(path::join(directory, "stderr")),
      env,
      lambda::bind(&setup, directory));

  if (s.isError()) {
    return Failure("Failed to fork executor: " + s.error());
  }

  // Checkpoint the executor's pid if requested.
  if (checkpoint) {
    const string& path = slave::paths::getForkedPidPath(
        slave::paths::getMetaRootDir(flags.work_dir),
        slaveId,
        executorInfo.framework_id(),
        executorInfo.executor_id(),
        containerId);

    LOG(INFO) << "Checkpointing executor's forked pid "
              << s.get().pid() << " to '" << path <<  "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(path, stringify(s.get().pid()));

    if (checkpointed.isError()) {
      LOG(ERROR) << "Failed to checkpoint executor's forked pid to '"
                 << path << "': " << checkpointed.error();

      // Close the subprocess's stdin so that it aborts.
      CHECK_SOME(s.get().in());
      os::close(s.get().in().get());

      return Failure("Could not checkpoint executor's pid");
    }
  }

  // Checkpoing complete, now synchronize with the process so that it
  // can continue to execute.
  CHECK_SOME(s.get().in());
  char c;
  ssize_t length;
  while ((length = write(s.get().in().get(), &c, sizeof(c))) == -1 &&
         errno == EINTR);

  if (length != sizeof(c)) {
    os::close(s.get().in().get());
    return Failure("Failed to synchronize with child process: " +
                   string(strerror(errno)));
  }

  // And finally watch for when the executor gets reaped.
  Future<Option<int> > status = process::reap(s.get().pid());

  statuses.put(containerId, status);
  status.onAny(defer(self(), &Self::reaped, containerId));

  return true;
}


Future<Nothing> DockerContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  // TODO(benh): Right now we're only launching tasks so we don't
  // expect the containers to be resized. This will need to get
  // implemented to support executors.
  return Nothing();
}


Future<ResourceStatistics> DockerContainerizerProcess::usage(
    const ContainerID& containerId)
{
#ifndef __linux__
  return Failure("Does not support usage() on non-linux platform");
#endif // __linux__

  if (!promises.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  // Construct the Docker container name.
  string name = DOCKER_NAME_PREFIX + stringify(containerId);
  return docker.inspect(name)
    .then(defer(self(), &Self::_usage, containerId, lambda::_1));
}


Future<ResourceStatistics> DockerContainerizerProcess::_usage(
    const ContainerID& containerId,
    const Future<Docker::Container> container)
{
  pid_t pid = container.get().pid();
  if (pid == 0) {
    return Failure("Container is not running");
  }
  Try<ResourceStatistics> usage =
    mesos::internal::usage(pid, true, true);
  if (usage.isError()) {
    return Failure(usage.error());
  }
  return usage.get();
}


Future<containerizer::Termination> DockerContainerizerProcess::wait(
    const ContainerID& containerId)
{
  if (!promises.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  return promises[containerId]->future();
}


void DockerContainerizerProcess::destroy(
    const ContainerID& containerId,
    const bool& killed)
{
  if (!promises.contains(containerId)) {
    LOG(WARNING) << "Ignoring destroy of unknown container: " << containerId;
    return;
  }

  if (destroying.contains(containerId)) {
    // Destroy has already been initiated.
    return;
  }

  destroying.insert(containerId);

  LOG(INFO) << "Destroying container '" << containerId << "'";

  // Do a 'docker rm -f' which we'll then find out about in '_wait'
  // after the mesos-executor exits because it's doing a 'docker wait'
  // (via --override).
  //
  // NOTE: We might not actually have a mesos-executor running (which
  // we could check by looking if 'containerId' is a key in
  // 'statuses') but if that is the case then we're doing a destroy
  // because we failed to launch the mesos-executor (see defer at
  // bottom of 'launch') so no need to do anything after a successful
  // 'docker rm -f'.

  // TODO(benh): Retry 'docker rm -f' if it failed but the container
  // still exists (asynchronously).
  docker.rm(DOCKER_NAME_PREFIX + stringify(containerId))
    .onAny(defer(self(), &Self::_destroy, containerId, killed, lambda::_1));
}


void DockerContainerizerProcess::_destroy(
    const ContainerID& containerId,
    const bool& killed,
    const Future<Option<int> >& future)
{
  if (!future.isReady()) {
    promises[containerId]->fail(
        "Failed to destroy container: " +
        (future.isFailed() ? future.failure() : "discarded future"));

    destroying.erase(containerId);

    return;
  }

  statuses.get(containerId).get()
    .onAny(defer(self(), &Self::__destroy, containerId, killed, lambda::_1));
}


void DockerContainerizerProcess::__destroy(
    const ContainerID& containerId,
    const bool& killed,
    const Future<Option<int> >& status)
{
  containerizer::Termination termination;
  termination.set_killed(killed);
  if (status.isReady() && status.get().isSome()) {
    termination.set_status(status.get().get());
  }

  promises[containerId]->set(termination);

  destroying.erase(containerId);
  promises.erase(containerId);
  statuses.erase(containerId);
}


Future<hashset<ContainerID> > DockerContainerizerProcess::containers()
{
  return promises.keys();
}


void DockerContainerizerProcess::reaped(const ContainerID& containerId)
{
  if (!promises.contains(containerId)) {
    return;
  }

  LOG(INFO) << "Executor for container '" << containerId << "' has exited";

  // The executor has exited so destroy the container.
  destroy(containerId, false);
}


Option<ContainerID> DockerContainerizerProcess::parse(
    const Docker::Container& container)
{
  Option<string> name = None();

  if (strings::startsWith(container.name(), DOCKER_NAME_PREFIX)) {
    name = strings::remove(
        container.name(), DOCKER_NAME_PREFIX, strings::PREFIX);
  } else if (strings::startsWith(container.name(), "/" + DOCKER_NAME_PREFIX)) {
    name = strings::remove(
        container.name(), "/" + DOCKER_NAME_PREFIX, strings::PREFIX);
  }

  if (name.isSome()) {
    ContainerID id;
    id.set_value(name.get());
    return id;
  }

  return None();
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
