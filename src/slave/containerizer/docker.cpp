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
#include <process/io.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

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

#include "slave/containerizer/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"

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
string DOCKER_NAME_PREFIX = "mesos-";


class DockerContainerizerProcess
  : public process::Process<DockerContainerizerProcess>
{
public:
  DockerContainerizerProcess(
      const Flags& _flags,
      const Docker& _docker)
    : flags(_flags),
      docker(_docker) {}

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
      bool killed = true); // process is either killed or reaped.

  virtual process::Future<hashset<ContainerID> > containers();

private:
  // Continuations and helpers.
  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& directory);

  process::Future<Nothing> _fetch(
      const ContainerID& containerId,
      const Option<int>& status);

  process::Future<Nothing> pull(
      const ContainerID& containerId,
      const std::string& directory,
      const ContainerInfo::DockerInfo& dockerInfo);

  process::Future<Nothing> _pull(
      const Subprocess& s);

  process::Future<Nothing> __pull(
      const Subprocess& s,
      const string& output);

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

  process::Future<bool> _launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  process::Future<bool> __launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  process::Future<bool> __launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  process::Future<bool> ___launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  process::Future<bool> ___launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint);

  process::Future<bool> ____launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint,
      const Docker::Container& container);

  void _destroy(
      const ContainerID& containerId,
      bool killed);

  void __destroy(
      const ContainerID& containerId,
      bool killed,
      const Future<Nothing>& future);

  void ___destroy(
      const ContainerID& containerId,
      bool killed,
      const Future<Option<int> >& status);

  process::Future<Nothing> _update(
      const ContainerID& containerId,
      const Resources& resources,
      const Docker::Container& container);

  Future<ResourceStatistics> _usage(
    const ContainerID& containerId,
    const Docker::Container& container);

  // Call back for when the executor exits. This will trigger
  // container destroy.
  void reaped(const ContainerID& containerId);

  static std::string containerName(const ContainerID& containerId);

  const Flags flags;

  Docker docker;

  struct Container
  {
    Container(const ContainerID& id)
      : state(FETCHING), id(id) {}

    // The DockerContainerier needs to be able to properly clean up
    // Docker containers, regardless of when they are destroyed. For
    // example, if a container gets destroyed while we are fetching,
    // we need to not keep running the fetch, nor should we try and
    // start the Docker container. For this reason, we've split out
    // the states into:
    //
    //     FETCHING
    //     PULLING
    //     RUNNING
    //     DESTROYING
    //
    // In particular, we made 'PULLING' be it's own state so that we
    // could easily destroy and cleanup when a user initiated pulling
    // a really big image but we timeout due to the executor
    // registration timeout. Since we curently have no way to discard
    // a Docker::run, we needed to explicitely do the pull (which is
    // the part that takes the longest) so that we can also explicitly
    // kill it when asked. Once the functions at Docker::* get support
    // for discarding, then we won't need to make pull be it's own
    // state anymore, although it doesn't hurt since it gives us
    // better error messages.
    enum State {
      FETCHING,
      PULLING,
      RUNNING,
      DESTROYING
    } state;

    ContainerID id;

    // Promise for future returned from wait().
    Promise<containerizer::Termination> termination;

    // Exit status of executor or container (depending on whether or
    // not we used the command executor). Represented as a promise so
    // that destroying can chain with it being set.
    Promise<Future<Option<int> > > status;

    // Future that tells us whether or not the run is still pending or
    // has failed so we know whether or not to wait for 'status'.
    Future<Nothing> run;

    // We keep track of the resources for each container so we can set
    // the ResourceStatistics limits in usage().
    Resources resources;

    // The mesos-fetcher subprocess, kept around so that we can do a
    // killtree on it if we're asked to destroy a container while we
    // are fetching.
    Option<Subprocess> fetcher;

    // The docker pull subprocess is stored so we can killtree the
    // pid when destroy is called while docker is pulling the image.
    Option<Subprocess> pull;
  };

  hashmap<ContainerID, Container*> containers_;
};


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


Try<DockerContainerizer*> DockerContainerizer::create(const Flags& flags)
{
  Try<Docker> docker = Docker::create(flags.docker);
  if (docker.isError()) {
    return Error(docker.error());
  }

  return new DockerContainerizer(flags, docker.get());
}


DockerContainerizer::DockerContainerizer(
    const Flags& flags,
    const Docker& docker)
{
  process = new DockerContainerizerProcess(flags, docker);
  spawn(process);
}


DockerContainerizer::~DockerContainerizer()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Nothing> DockerContainerizerProcess::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& directory)
{
  if (commandInfo.uris().size() == 0) {
    return Nothing();
  }

  Result<string> realpath = os::realpath(
      path::join(flags.launcher_dir, "mesos-fetcher"));

  if (!realpath.isSome()) {
    LOG(ERROR) << "Failed to determine the canonical path "
               << "for the mesos-fetcher '"
               << path::join(flags.launcher_dir, "mesos-fetcher")
               << "': "
               << (realpath.isError() ? realpath.error() :
                   "No such file or directory");
    return Failure("Could not fetch URIs: failed to find mesos-fetcher");
  }

  map<string, string> fetcherEnv = fetcherEnvironment(
      commandInfo,
      directory,
      None(),
      flags);

  VLOG(1) << "Starting to fetch URIs for container: " << containerId
          << ", directory: " << directory;

  // NOTE: It's important that we create a pipe for the mesos-fetcher
  // stdin so that when the slave exits it will terminate itself.
  Try<Subprocess> fetcher = subprocess(
      realpath.get(),
      Subprocess::PIPE(),
      Subprocess::PATH(path::join(directory, "stdout")),
      Subprocess::PATH(path::join(directory, "stderr")),
      fetcherEnv);

  if (fetcher.isError()) {
    return Failure("Failed to execute mesos-fetcher: " + fetcher.error());
  }

  containers_[containerId]->fetcher = fetcher.get();

  return fetcher.get().status()
    .then(defer(self(), &Self::_fetch, containerId, lambda::_1));
}


Future<Nothing> DockerContainerizerProcess::_fetch(
    const ContainerID& containerId,
    const Option<int>& status)
{
  if (!status.isSome()) {
    return Failure("No status available from fetch");
  } else if (status.get() != 0) {
    return Failure("Fetch exited with status " + WSTRINGIFY(status.get()));
  }

  return Nothing();
}


// TODO(benh): Move this into Docker::pull after we've correctly made
// the futures returned from Docker::* functions be discardable.
Future<Nothing> DockerContainerizerProcess::pull(
    const ContainerID& containerId,
    const string& directory,
    const ContainerInfo::DockerInfo& dockerInfo)
{
  vector<string> argv;
  argv.push_back(flags.docker);
  argv.push_back("pull");

  vector<string> parts = strings::split(dockerInfo.image(), ":");

  if (parts.size() > 2) {
    return Failure("Not expecting multiple ':' in image: " +
                   dockerInfo.image());
  }

  if (parts.size() == 2) {
    argv.push_back(dockerInfo.image());
  } else {
    argv.push_back(parts[0] + ":latest");
  }

  VLOG(1) << "Running " << strings::join(" ", argv);

  map<string, string> environment;
  environment["HOME"] = directory;

  Try<Subprocess> s = subprocess(
      flags.docker,
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      None(),
      environment);

  if (s.isError()) {
    return Failure("Failed to execute 'docker pull': " + s.error());
  }

  containers_[containerId]->pull = s.get();

  return s.get().status()
    .then(defer(self(), &Self::_pull, s.get()));
}


Future<Nothing> DockerContainerizerProcess::_pull(
    const Subprocess& s)
{
  CHECK_READY(s.status());

  Option<int> status = s.status().get();

  if (status.isSome() && status.get() == 0) {
    return Nothing();
  }

  CHECK_SOME(s.err());
  return io::read(s.err().get())
    .then(defer(self(), &Self::__pull, s, lambda::_1));
 }


Future<Nothing> DockerContainerizerProcess::__pull(
    const Subprocess& s,
    const string& output)
{
  CHECK_READY(s.status());

  Option<int> status = s.status().get();

  if (status.isNone()) {
    return Failure("No exit status available from 'docker pull': \n" + output);
  }

  CHECK_NE(0, status.get());

  return Failure("Failed to execute 'docker pull', exited with status (" +
                 WSTRINGIFY(status.get()) + "): \n" + output);
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


string DockerContainerizerProcess::containerName(const ContainerID& containerId)
{
  return DOCKER_NAME_PREFIX + stringify(containerId);
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
  return docker.ps(true, DOCKER_NAME_PREFIX)
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
      // TODO(benh): Retry 'docker rm -f' if it failed but the container
      // still exists (asynchronously).
      docker.kill(container.id, true);
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

  ContainerInfo containerInfo = executorInfo.container();

  if (containerInfo.type() != ContainerInfo::DOCKER) {
    LOG(INFO) << "Skipping non-docker container";
    return false;
  }

  LOG(INFO) << "Starting container '" << containerId
            << "' for task '" << taskInfo.task_id()
            << "' (and executor '" << executorInfo.executor_id()
            << "') of framework '" << executorInfo.framework_id() << "'";

  Container* container = new Container(containerId);
  containers_[containerId] = container;

  return fetch(containerId, taskInfo.command(), directory)
    .then(defer(self(),
                &Self::_launch,
                containerId,
                taskInfo,
                executorInfo,
                directory,
                slaveId,
                slavePid,
                checkpoint))
    .onFailed(defer(self(), &Self::destroy, containerId, true));
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
  // Doing the fetch might have succeded but we were actually asked to
  // destroy the container, which we did, so don't continue.
  if (!containers_.contains(containerId)) {
    return Failure("Container was destroyed while launching");
  }

  containers_[containerId]->state = Container::PULLING;

  return pull(containerId, directory, taskInfo.container().docker())
    .then(defer(self(),
                &Self::__launch,
                containerId,
                taskInfo,
                executorInfo,
                directory,
                slaveId,
                slavePid,
                checkpoint));
}

Future<bool> DockerContainerizerProcess::__launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container was destroyed while pulling image");
  }

  containers_[containerId]->state = Container::RUNNING;

  // Try and start the Docker container.
  containers_[containerId]->run = docker.run(
      taskInfo.container(),
      taskInfo.command(),
      containerName(containerId),
      directory,
      flags.docker_sandbox_directory,
      taskInfo.resources());

  return containers_[containerId]->run
    .then(defer(self(),
                &Self::___launch,
                containerId,
                taskInfo,
                executorInfo,
                directory,
                slaveId,
                slavePid,
                checkpoint));
}


Future<bool> DockerContainerizerProcess::___launch(
    const ContainerID& containerId,
    const TaskInfo& taskInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  // After we do Docker::run we shouldn't remove a container until
  // after we set 'status', which we do in this function.
  CHECK(containers_.contains(containerId));

  // Prepare environment variables for the executor.
  map<string, string> env = executorEnvironment(
      executorInfo,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      flags.recovery_timeout);

  // Include any enviroment variables from ExecutorInfo.
  foreach (const Environment::Variable& variable,
           executorInfo.command().environment().variables()) {
    env[variable.name()] = variable.value();
  }

  // Construct the mesos-executor "override" to do a 'docker wait'
  // using the "name" we gave the container (to distinguish it from
  // Docker containers not created by Mesos). Note, however, that we
  // don't want the exit status from 'docker wait' but rather the exit
  // status from the container, hence the use of /bin/bash.
  string override =
    "/bin/sh -c 'exit `" +
    flags.docker + " wait " + containerName(containerId) + "`'";

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
    string error = string(strerror(errno));
    os::close(s.get().in().get());
    return Failure("Failed to synchronize with child process: " + error);
  }

  // Store the resources for usage().
  containers_[containerId]->resources = taskInfo.resources();

  // And finally watch for when the executor gets reaped.
  containers_[containerId]->status.set(process::reap(s.get().pid()));

  containers_[containerId]->status.future().get()
    .onAny(defer(self(), &Self::reaped, containerId));

  // TODO(benh): Check failure of Docker::logs.
  docker.logs(containerName(containerId), directory);

  return true;
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

  LOG(INFO) << "Starting container '" << containerId
            << "' for executor '" << executorInfo.executor_id()
            << "' and framework '" << executorInfo.framework_id() << "'";

  Container* container = new Container(containerId);
  containers_[containerId] = container;

  return fetch(containerId, executorInfo.command(), directory)
    .then(defer(self(),
                &Self::_launch,
                containerId,
                executorInfo,
                directory,
                slaveId,
                slavePid,
                checkpoint))
    .onFailed(defer(self(), &Self::destroy, containerId, true));
}


Future<bool> DockerContainerizerProcess::_launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  // Doing the fetch might have succeded but we were actually asked to
  // destroy the container, which we did, so don't continue.
  if (!containers_.contains(containerId)) {
    return Failure("Container was destroyed while launching");
  }

  containers_[containerId]->state = Container::PULLING;

  return pull(containerId, directory, executorInfo.container().docker())
    .then(defer(self(),
                &Self::__launch,
                containerId,
                executorInfo,
                directory,
                slaveId,
                slavePid,
                checkpoint));
}


Future<bool> DockerContainerizerProcess::__launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  if (!containers_.contains(containerId)) {
    return Failure("Container was destroyed while pulling image");
  }

  containers_[containerId]->state = Container::RUNNING;

  map<string, string> env = executorEnvironment(
      executorInfo,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      flags.recovery_timeout);

  // Include any environment variables from CommandInfo.
  foreach (const Environment::Variable& variable,
           executorInfo.command().environment().variables()) {
    env[variable.name()] = variable.value();
  }

  // Try and start the Docker container.
  containers_[containerId]->run = docker.run(
      executorInfo.container(),
      executorInfo.command(),
      containerName(containerId),
      directory,
      flags.docker_sandbox_directory,
      None(),
      env);

  return containers_[containerId]->run
    .then(defer(self(),
                &Self::___launch,
                containerId,
                executorInfo,
                directory,
                slaveId,
                slavePid,
                checkpoint));
}


Future<bool> DockerContainerizerProcess::___launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  // We shouldn't remove container until we set 'status'.
  CHECK(containers_.contains(containerId));
  return docker.inspect(containerName(containerId))
     .then(defer(self(),
                &Self::____launch,
                containerId,
                executorInfo,
                directory,
                slaveId,
                slavePid,
                checkpoint,
                lambda::_1));
}


Future<bool> DockerContainerizerProcess::____launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint,
    const Docker::Container& container)
{
  // After we do Docker::run we shouldn't remove a container until
  // after we set 'status', which we do in this function.
  CHECK(containers_.contains(containerId));

  Option<int> pid = container.pid;

  if (!pid.isSome()) {
    return Failure("Unable to get executor pid after launch");
  }

  if (checkpoint) {
    // TODO(tnachen): We might not be able to checkpoint if the slave
    // dies before it can checkpoint while the executor is still
    // running. Optinally we can consider recording the slave id and
    // executor id as part of the docker container name so we can
    // recover from this.
    const string& path =
      slave::paths::getForkedPidPath(
          slave::paths::getMetaRootDir(flags.work_dir),
          slaveId,
          executorInfo.framework_id(),
          executorInfo.executor_id(),
          containerId);

    LOG(INFO) << "Checkpointing executor's forked pid "
              << pid.get() << " to '" << path <<  "'";

    Try<Nothing> checkpointed =
      slave::state::checkpoint(path, stringify(pid.get()));

    if (checkpointed.isError()) {
      return Failure("Failed to checkpoint executor's forked pid to '"
                     + path + "': " + checkpointed.error());
    }
  }

  // Store the resources for usage().
  containers_[containerId]->resources = executorInfo.resources();

  // And finally watch for when the container gets reaped.
  containers_[containerId]->status.set(process::reap(pid.get()));

  containers_[containerId]->status.future().get()
    .onAny(defer(self(), &Self::reaped, containerId));

  // TODO(benh): Check failure of Docker::logs.
  docker.logs(containerName(containerId), directory);

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

  // Store the resources for usage().
  containers_[containerId]->resources = _resources;

#ifdef __linux__
  if (!_resources.cpus().isSome() && !_resources.mem().isSome()) {
    LOG(WARNING) << "Ignoring update as no supported resources are present";
    return Nothing();
  }

  return docker.inspect(containerName(containerId))
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

  // First check that this container still appears to be running.
  Option<pid_t> pid = container.pid;
  if (pid.isNone()) {
    return Nothing();
  }

  // Determine the cgroup for the 'cpu' subsystem (based on the
  // container's pid).
  Result<string> cpuCgroup = cgroups::cpu::cgroup(pid.get());

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
  Result<string> memoryCgroup = cgroups::memory::cgroup(pid.get());

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

  return docker.inspect(containerName(containerId))
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

  // Note that here getting the root pid is enough because
  // the root process acts as an 'init' process in the docker
  // container, so no other child processes will escape it.
  Try<ResourceStatistics> statistics =
    mesos::internal::usage(pid.get(), true, true);
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

    if (container->fetcher.isSome()) {
      // Best effort kill the entire fetcher tree.
      os::killtree(container->fetcher.get().pid(), SIGKILL);
    }

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

    if (container->pull.isSome()) {
      os::killtree(container->pull.get().pid(), SIGKILL);
    }

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

  // TODO(benh): Retry 'docker rm -f' if it failed but the container
  // still exists (asynchronously).

  LOG(INFO) << "Running docker kill on container '" << containerId << "'";

  docker.kill(containerName(containerId), true)
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
    const Future<Option<int> >& status)
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
  delete container;
}


Future<hashset<ContainerID> > DockerContainerizerProcess::containers()
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


} // namespace slave {
} // namespace internal {
} // namespace mesos {
