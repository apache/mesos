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

#include <sstream>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/io.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/fatal.hpp>
#include <stout/os.hpp>
#include <stout/unreachable.hpp>

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/mesos_containerizer.hpp"

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

// Local function declaration/definitions.
Future<Nothing> _nothing() { return Nothing(); }


MesosContainerizer::MesosContainerizer(
    const Flags& flags,
    bool local,
    const Owned<Launcher>& launcher,
    const vector<Owned<Isolator> >& isolators)
{
  process = new MesosContainerizerProcess(
      flags, local, launcher, isolators);
  spawn(process);
}


MesosContainerizer::~MesosContainerizer()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Nothing> MesosContainerizer::recover(const Option<state::SlaveState>& state)
{
  return dispatch(process, &MesosContainerizerProcess::recover, state);
}


Future<Nothing> MesosContainerizer::launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return dispatch(process,
                  &MesosContainerizerProcess::launch,
                  containerId,
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
  return dispatch(process,
                  &MesosContainerizerProcess::update,
                  containerId,
                  resources);
}


Future<ResourceStatistics> MesosContainerizer::usage(
    const ContainerID& containerId)
{
  return dispatch(process, &MesosContainerizerProcess::usage, containerId);
}


Future<Containerizer::Termination> MesosContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process, &MesosContainerizerProcess::wait, containerId);
}


void MesosContainerizer::destroy(const ContainerID& containerId)
{
  dispatch(process, &MesosContainerizerProcess::destroy, containerId);
}


Future<Nothing> MesosContainerizerProcess::recover(
    const Option<state::SlaveState>& state)
{
  LOG(INFO) << "Recovering containerizer";

  // Gather the executor run states that we will attempt to recover.
  list<RunState> recoverable;
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
        CHECK(executor.runs.contains(containerId));
        const RunState& run = executor.runs.get(containerId).get();

        // We need the pid so the reaper can monitor the executor so skip this
        // executor if it's not present. This is not an error because the slave
        // will try to wait on the container which will return a failed
        // Termination and everything will get cleaned up.
        if (!run.forkedPid.isSome()) {
          continue;
        }

        if (run.completed) {
          VLOG(1) << "Skipping recovery of executor '" << executor.id
                  << "' of framework " << framework.id
                  << " because its latest run "
                  << containerId << " is completed";
          continue;
        }

        LOG(INFO) << "Recovering container '" << containerId
                  << "' for executor '" << executor.id
                  << "' of framework " << framework.id;

        recoverable.push_back(run);
      }
    }
  }

  // Try to recover the launcher first.
  Try<Nothing> recover = launcher->recover(recoverable);
  if (recover.isError()) {
    return Failure(recover.error());
  }

  // Then recover the isolators.
  list<Future<Nothing> > futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->recover(recoverable));
  }

  // If all isolators recover then continue.
  return collect(futures)
    .then(defer(self(), &Self::_recover, recoverable));
}


Future<Nothing> MesosContainerizerProcess::_recover(
    const list<RunState>& recovered)
{
  foreach (const RunState& run, recovered) {
    CHECK_SOME(run.id);
    const ContainerID& containerId = run.id.get();

    Owned<Promise<Containerizer::Termination> > promise(
        new Promise<Containerizer::Termination>());
    promises.put(containerId, promise);

    CHECK_SOME(run.forkedPid);
    Future<Option<int > > status = process::reap(run.forkedPid.get());
    statuses[containerId] = status;
    status.onAny(defer(self(), &Self::reaped, containerId));

    foreach (const Owned<Isolator>& isolator, isolators) {
      isolator->watch(containerId)
        .onAny(defer(self(), &Self::limited, containerId, lambda::_1));
    }
  }

  return Nothing();
}


// Log the message and then exit(1) in an async-signal-safe manner.
// TODO(idownes): Move this into stout, possibly replacing its fatal(), and
// support multiple messages to write out.
void asyncSafeFatal(const char* message)
{
  // Ignore the return value from write() to silence compiler warning.
  while (write(STDERR_FILENO, message, strlen(message)) == -1 &&
      errno == EINTR);
  _exit(1);
}


// This function is executed by the forked child and should be
// async-signal-safe.
// TODO(idownes): Several functions used here are not actually
// async-signal-safe:
// 1) os::close, os::chown and os::chdir concatenate strings on error
// 2) os::setenv uses ::setenv that is not listed as safe
// 3) freopen is not listed as safe
// These can all be corrected and also we could write better error messages
// with multiple writes in an improved asyncSafeFatal.
int execute(
    const CommandInfo& command,
    const string& directory,
    const Option<string>& user,
    const map<string, string>& env,
    bool redirectIO,
    int pipeRead,
    int pipeWrite)
{
  // Do a blocking read on the pipe until the parent signals us to continue.
  os::close(pipeWrite);
  int buf;
  ssize_t len;
  while ((len = read(pipeRead, &buf, sizeof(buf))) == -1 && errno == EINTR);

  if (len != sizeof(buf)) {
    os::close(pipeRead);
    asyncSafeFatal("Failed to synchronize with parent");
  }
  os::close(pipeRead);

  // Chown the work directory if a user is provided.
  if (user.isSome()) {
    Try<Nothing> chown = os::chown(user.get(), directory);
    if (chown.isError()) {
      asyncSafeFatal("Failed to chown work directory");
    }
  }

  // Change user if provided.
  if (user.isSome() && !os::su(user.get())) {
    asyncSafeFatal("Failed to change user");
  }

  // Enter working directory.
  if (os::chdir(directory) < 0) {
    asyncSafeFatal("Failed to chdir into work directory");
  }

  // First set up any additional environment variables.
  // TODO(idownes): setenv is not async-signal-safe. Environment variables
  // could instead be set using execle.
  foreachpair (const string& key, const string& value, env) {
    os::setenv(key, value);
  }

  // Then set up environment variables from CommandInfo.
  foreach(const Environment::Variable& variable,
      command.environment().variables()) {
    os::setenv(variable.name(), variable.value());
  }

  // Redirect output to files in working dir if required. We append because
  // others (e.g., mesos-fetcher) may have already logged to the files.
  // TODO(bmahler): It would be best if instead of closing stderr /
  // stdout and redirecting, we instead always output to stderr /
  // stdout. Also tee'ing their output into the work directory files
  // when redirection is desired.
  // TODO(idownes): freopen is not async-signal-safe. Could use dup2 and open
  // directly.
  if (redirectIO) {
    if (freopen("stdout", "a", stdout) == NULL) {
      asyncSafeFatal("freopen failed");
    }
    if (freopen("stderr", "a", stderr) == NULL) {
      asyncSafeFatal("freopen failed");
    }
  }

  // Execute the command (via '/bin/sh -c command').
  execl("/bin/sh", "sh", "-c", command.value().c_str(), (char*) NULL);

  // If we get here, the execv call failed.
  asyncSafeFatal("Failed to execute command");

  // This should not be reached.
  return -1;
}


// Launching an executor involves the following steps:
// 1. Prepare the container. First call prepare on each isolator and then
//    fetch the executor into the container sandbox.
// 2. Fork the executor. The forked child is blocked from exec'ing until it has
//    been isolated.
// 3. Isolate the executor. Call isolate with the pid for each isolator.
// 4. Exec the executor. The forked child is signalled to continue and exec the
//    executor.
Future<Nothing> MesosContainerizerProcess::launch(
    const ContainerID& containerId,
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

  Owned<Promise<Containerizer::Termination> > promise(
      new Promise<Containerizer::Termination>());
  promises.put(containerId, promise);

  // Store the resources for usage().
  resources.put(containerId, executorInfo.resources());

  LOG(INFO) << "Starting container '" << containerId
            << "' for executor '" << executorInfo.executor_id()
            << "' of framework '" << executorInfo.framework_id() << "'";

  // Prepare additional environment variables for the executor.
  const map<string, string>& env = executorEnvironment(
      executorInfo,
      directory,
      slaveId,
      slavePid,
      checkpoint,
      flags.recovery_timeout);

  // Use a pipe to block the child until it's been isolated.
  // The parent will close its read end after the child is forked, and the
  // write end afer the child is signalled to exec.
  // TODO(idownes): Ensure the pipe's file descriptors are closed even if some
  // stage of the executor launch fails.
  int pipes[2];
  // We assume this should not fail under reasonable conditions so we use CHECK.
  CHECK(pipe(pipes) == 0);

  // Prepare a function for the forked child to exec() the executor.
  lambda::function<int()> inChild = lambda::bind(
      &execute,
      executorInfo.command(),
      directory,
      user,
      env,
      !local,
      pipes[0],
      pipes[1]);

  return prepare(containerId, executorInfo, directory, user)
    .then(defer(self(),
                &Self::fork,
                containerId,
                executorInfo,
                inChild,
                slaveId,
                checkpoint,
                pipes[0]))
    .then(defer(self(),
                &Self::isolate,
                containerId,
                lambda::_1))
    .then(defer(self(),
                &Self::exec,
                containerId,
                pipes[1]))
    .onAny(lambda::bind(&os::close, pipes[0]))
    .onAny(lambda::bind(&os::close, pipes[1]))
    .onFailed(defer(self(),
                    &Self::destroy,
                    containerId));
}


Future<Nothing> MesosContainerizerProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user)
{
  // Start preparing all isolators (in parallel).
  list<Future<Nothing> > futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->prepare(containerId, executorInfo));
  }

  // Wait for all isolators to complete preparations then fetch the executor.
  return collect(futures)
    .then(defer(
          self(),
          &Self::fetch,
          containerId,
          executorInfo.command(),
          directory,
          user));
}


Future<Nothing> _fetch(
    const ContainerID& containerId,
    const Option<int>& status)
{
  if (status.isNone() || (status.get() != 0)) {
    return Failure("Failed to fetch URIs for container '" +
                   stringify(containerId) + "': exit status " +
                   (status.isNone() ? "none" : stringify(status.get())));
  }

  return Nothing();
}


Future<Nothing> MesosContainerizerProcess::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user)
{
  // Determine path for mesos-fetcher.
  Result<string> realpath = os::realpath(
      path::join(flags.launcher_dir, "mesos-fetcher"));

  if (!realpath.isSome()) {
    LOG(ERROR) << "Failed to determine the canonical path "
                << "for the mesos-fetcher '"
                << path::join(flags.launcher_dir, "mesos-fetcher")
                << "': "
                << (realpath.isError() ? realpath.error()
                                       : "No such file or directory");
    return Failure("Could not fetch URIs: failed to find mesos-fetcher");
  }

  // Prepare the environment variables to pass to mesos-fetcher.
  string uris = "";
  foreach (const CommandInfo::URI& uri, commandInfo.uris()) {
    uris += uri.value() + "+" +
            (uri.has_executable() && uri.executable() ? "1" : "0");
    uris += " ";
  }
  // Remove extra space at the end.
  uris = strings::trim(uris);

  // Use /usr/bin/env to set the environment variables for the fetcher
  // subprocess because we cannot pollute the slave's environment.
  // TODO(idownes): Remove this once Subprocess accepts environment variables.
  string command = "/usr/bin/env";
  command += " MESOS_EXECUTOR_URIS=" + uris;
  command += " MESOS_WORK_DIRECTORY=" + directory;
  if (user.isSome()) {
    command += " MESOS_USER=" + user.get();
  }
  command += " MESOS_FRAMEWORKS_HOME=" + flags.frameworks_home;
  command += " HADOOP_HOME=" + flags.hadoop_home;

  // Now the actual mesos-fetcher command.
  command += " " + realpath.get();

  LOG(INFO) << "Fetching URIs for container '" << containerId
            << "' using command '" << command << "'";

  Try<Subprocess> fetcher = subprocess(command);
  if (fetcher.isError()) {
    return Failure("Failed to execute mesos-fetcher: " + fetcher.error());
  }

  // Redirect output (stdout and stderr) from the fetcher to log files in the
  // executor work directory, chown'ing them if a user is specified.
  Try<int> out = os::open(
      path::join(directory, "stdout"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (out.isError()) {
    return Failure("Failed to redirect stdout: " + out.error());
  }

  if (user.isSome()) {
    Try<Nothing> chown = os::chown(user.get(), path::join(directory, "stdout"));
    if (chown.isError()) {
      os::close(out.get());
      return Failure("Failed to redirect stdout:" + chown.error());
    }
  }

  Try<Nothing> nonblock = os::nonblock(fetcher.get().out());
  if (nonblock.isError()) {
    os::close(out.get());
    return Failure("Failed to redirect stdout:" + nonblock.error());
  }

  io::splice(fetcher.get().out(), out.get())
    .onAny(lambda::bind(&os::close, out.get()));

  // Repeat for stderr.
  Try<int> err = os::open(
      path::join(directory, "stderr"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (err.isError()) {
    os::close(out.get());
    return Failure("Failed to redirect stderr:" + err.error());
  }

  if (user.isSome()) {
    Try<Nothing> chown = os::chown(user.get(), path::join(directory, "stderr"));
    if (chown.isError()) {
      os::close(out.get());
      os::close(err.get());
      return Failure("Failed to redirect stderr:" + chown.error());
    }
  }

  nonblock = os::nonblock(fetcher.get().err());
  if (nonblock.isError()) {
    os::close(out.get());
    os::close(err.get());
    return Failure("Failed to redirect stderr:" + nonblock.error());
  }

  io::splice(fetcher.get().err(), err.get())
    .onAny(lambda::bind(&os::close, err.get()));

  return fetcher.get().status()
    .then(lambda::bind(&_fetch, containerId, lambda::_1));
}


Future<pid_t> MesosContainerizerProcess::fork(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const lambda::function<int()>& inChild,
    const SlaveID& slaveId,
    bool checkpoint,
    int pipeRead)
{
  Try<pid_t> forked = launcher->fork(containerId, inChild);

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

  // Monitor the executor's pid. We keep the future because we'll refer to it
  // again during container destroy.
  Future<Option<int> > status = process::reap(pid);
  statuses.put(containerId, status);
  status.onAny(defer(self(), &Self::reaped, containerId));

  return pid;
}


Future<Nothing> MesosContainerizerProcess::isolate(
    const ContainerID& containerId,
    pid_t _pid)
{
  // Set up callbacks for isolator limitations.
  foreach (const Owned<Isolator>& isolator, isolators) {
    isolator->watch(containerId)
      .onAny(defer(self(), &Self::limited, containerId, lambda::_1));
  }

  // Isolate the executor with each isolator and get optional additional
  // commands to be run in the containerized context.
  list<Future<Option<CommandInfo> > > futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->isolate(containerId, _pid));
  }

  // Wait for all isolators to complete then run additional commands.
  return collect(futures)
    .then(defer(self(), &Self::_isolate, containerId, lambda::_1));
}


Future<Nothing> MesosContainerizerProcess::_isolate(
    const ContainerID& containerId,
    const list<Option<CommandInfo> >& commands)
{
  // TODO(idownes): Implement execution of additional isolation commands.
  foreach (const Option<CommandInfo>& command, commands) {
    if (command.isSome()) {
      LOG(WARNING) << "Additional isolation commands not implemented";
    }
  }

  return Nothing();
}


Future<Nothing> MesosContainerizerProcess::exec(
    const ContainerID& containerId,
    int pipeWrite)
{
  CHECK(promises.contains(containerId));

  // Now that we've contained the child we can signal it to continue by
  // writing to the pipe.
  int buf;
  ssize_t len;
  while ((len = write(pipeWrite, &buf, sizeof(buf))) == -1 && errno == EINTR);

  if (len != sizeof(buf)) {
    return Failure("Failed to synchronize child process: " +
                   string(strerror(errno)));
  }

  return Nothing();
}


Future<Containerizer::Termination> MesosContainerizerProcess::wait(
    const ContainerID& containerId)
{
  if (!promises.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  return promises[containerId]->future();
}


Future<Nothing> MesosContainerizerProcess::update(
    const ContainerID& containerId,
    const Resources& _resources)
{
  if (!resources.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  // Store the resources for usage().
  resources.put(containerId, _resources);

  // Update each isolator.
  list<Future<Nothing> > futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->update(containerId, _resources));
  }

  // Wait for all isolators to complete.
  return collect(futures)
    .then(lambda::bind(_nothing));
}


// Resources are used to set the limit fields in the statistics but are
// optional because they aren't known after recovery until/unless update() is
// called.
Future<ResourceStatistics> _usage(
    const ContainerID& containerId,
    const Option<Resources>& resources,
    const list<Future<ResourceStatistics> >& statistics)
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
  if (!promises.contains(containerId)) {
    return Failure("Unknown container: " + stringify(containerId));
  }

  list<Future<ResourceStatistics> > futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->usage(containerId));
  }

  // Use await() here so we can return partial usage statistics.
  // TODO(idownes): After recovery resources won't be known until after an
  // update() because they aren't part of the SlaveState.
  return await(futures)
    .then(lambda::bind(
          _usage, containerId, resources.get(containerId), lambda::_1));
}


void MesosContainerizerProcess::destroy(const ContainerID& containerId)
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

  if (statuses.contains(containerId)) {
    // Kill all processes then continue destruction.
    launcher->destroy(containerId)
      .onAny(defer(self(), &Self::_destroy, containerId, lambda::_1));
  } else {
    // The executor never forked so no processes to kill, go straight to
    // __destroy() with status = None().
    __destroy(containerId, None());
  }
}


void MesosContainerizerProcess::_destroy(
    const ContainerID& containerId,
    const Future<Nothing>& future)
{
  // Something has gone wrong and the launcher wasn't able to kill all the
  // processes in the container. We cannot clean up the isolators because they
  // may require that all processes have exited so just return the failure to
  // the slave.
  // TODO(idownes): This is a pretty bad state to be in but we should consider
  // cleaning up here.
  if (!future.isReady()) {
    promises[containerId]->fail(
        "Failed to destroy container: " +
        (future.isFailed() ? future.failure() : "discarded future"));
    return;
  }

  // We've successfully killed all processes in the container so get the exit
  // status of the executor when it's ready (it may already be) and continue
  // the destroy.
  statuses.get(containerId).get()
    .onAny(defer(self(), &Self::__destroy, containerId, lambda::_1));
}


void MesosContainerizerProcess::__destroy(
    const ContainerID& containerId,
    const Future<Option<int > >& status)
{
  // A container is 'killed' if any isolator limited it.
  // Note: We may not see a limitation in time for it to be registered. This
  // could occur if the limitation (e.g., an OOM) killed the executor and we
  // triggered destroy() off the executor exit.
  bool killed = false;
  string message;
  if (limitations.contains(containerId)) {
    killed = true;
    foreach (const Limitation& limitation, limitations.get(containerId)) {
      message += limitation.message;
    }
    message = strings::trim(message);
  } else {
    message = "Executor terminated";
  }

  // We can now clean up all isolators.
  foreach (const Owned<Isolator>& isolator, isolators) {
    isolator->cleanup(containerId);
  }

  promises[containerId]->set(Containerizer::Termination(
        status.isReady() ? status.get() : None(),
        killed,
        message));

  promises.erase(containerId);
  statuses.erase(containerId);
  limitations.erase(containerId);
  resources.erase(containerId);
  destroying.erase(containerId);
}


void MesosContainerizerProcess::reaped(const ContainerID& containerId)
{
  if (!promises.contains(containerId)) {
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
  if (!promises.contains(containerId)) {
    return;
  }

  if (future.isReady()) {
    LOG(INFO) << "Container " << containerId << " has reached its limit for"
              << " resource " << future.get().resource
              << " and will be terminated";
    limitations.put(containerId, future.get());
  } else {
    // TODO(idownes): A discarded future will not be an error when isolators
    // discard their promises after cleanup.
    LOG(ERROR) << "Error in a resource limitation for container "
               << containerId << ": " << (future.isFailed() ? future.failure()
                                                            : "discarded");
  }

  // The container has been affected by the limitation so destroy it.
  destroy(containerId);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
