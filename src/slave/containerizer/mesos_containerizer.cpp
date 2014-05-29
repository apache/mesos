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

#include <stout/os/execenv.hpp>

#include "slave/paths.hpp"
#include "slave/slave.hpp"

#ifdef __linux__
#include "slave/containerizer/linux_launcher.hpp"
#endif // __linux__
#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/isolator.hpp"
#include "slave/containerizer/launcher.hpp"
#include "slave/containerizer/mesos_containerizer.hpp"

#include "slave/containerizer/isolators/posix.hpp"
#ifdef __linux__
#include "slave/containerizer/isolators/cgroups/cpushare.hpp"
#include "slave/containerizer/isolators/cgroups/mem.hpp"
#endif // __linux__

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


// Helper method to build the environment map used to launch fetcher.
map<string, string> fetcherEnvironment(
    const CommandInfo& commandInfo,
    const std::string& directory,
    const Option<std::string>& user,
    const Flags& flags)
{
  // Prepare the environment variables to pass to mesos-fetcher.
  string uris = "";
  foreach (const CommandInfo::URI& uri, commandInfo.uris()) {
    uris += uri.value() + "+" +
            (uri.has_executable() && uri.executable() ? "1" : "0") +
            (uri.extract() ? "X" : "N");
    uris += " ";
  }
  // Remove extra space at the end.
  uris = strings::trim(uris);

  map<string, string> environment;
  environment["MESOS_EXECUTOR_URIS"] = uris;
  environment["MESOS_WORK_DIRECTORY"] = directory;
  if (user.isSome()) {
    environment["MESOS_USER"] = user.get();
  }
  if (!flags.frameworks_home.empty()) {
    environment["MESOS_FRAMEWORKS_HOME"] = flags.frameworks_home;
  }
  if (!flags.hadoop_home.empty()) {
    environment["HADOOP_HOME"] = flags.hadoop_home;
  }

  return environment;
}


Try<MesosContainerizer*> MesosContainerizer::create(
    const Flags& flags,
    bool local)
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

  LOG(INFO) << "Using isolation: " << isolation;

  // Create a MesosContainerizerProcess using isolators and a launcher.
  hashmap<std::string, Try<Isolator*> (*)(const Flags&)> creators;

  creators["posix/cpu"]   = &PosixCpuIsolatorProcess::create;
  creators["posix/mem"]   = &PosixMemIsolatorProcess::create;
#ifdef __linux__
  creators["cgroups/cpu"] = &CgroupsCpushareIsolatorProcess::create;
  creators["cgroups/mem"] = &CgroupsMemIsolatorProcess::create;
#endif // __linux__

  vector<Owned<Isolator> > isolators;

  foreach (const string& type, strings::split(isolation, ",")) {
    if (creators.contains(type)) {
      Try<Isolator*> isolator = creators[type](flags);
      if (isolator.isError()) {
        return Error(
            "Could not create isolator " + type + ": " + isolator.error());
      } else {
        isolators.push_back(Owned<Isolator>(isolator.get()));
      }
    } else {
      return Error("Unknown or unsupported isolator: " + type);
    }
  }

#ifdef __linux__
  // Use cgroups on Linux if any cgroups isolators are used.
  Try<Launcher*> launcher = strings::contains(isolation, "cgroups")
    ? LinuxLauncher::create(flags)
    : PosixLauncher::create(flags);
#else
  Try<Launcher*> launcher = PosixLauncher::create(flags);
#endif // __linux__

  if (launcher.isError()) {
    return Error("Failed to create launcher: " + launcher.error());
  }

  return new MesosContainerizer(
      flags, local, Owned<Launcher>(launcher.get()), isolators);
}


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


Future<Nothing> MesosContainerizer::recover(
    const Option<state::SlaveState>& state)
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


Future<Nothing> MesosContainerizer::launch(
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


Future<containerizer::Termination> MesosContainerizer::wait(
    const ContainerID& containerId)
{
  return dispatch(process, &MesosContainerizerProcess::wait, containerId);
}


void MesosContainerizer::destroy(const ContainerID& containerId)
{
  dispatch(process, &MesosContainerizerProcess::destroy, containerId);
}


Future<hashset<ContainerID> > MesosContainerizer::containers()
{
  return dispatch(process, &MesosContainerizerProcess::containers);
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
        Option<RunState> run = executor.runs.get(containerId);
        CHECK_SOME(run);

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

        recoverable.push_back(run.get());
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

    Owned<Promise<containerizer::Termination> > promise(
        new Promise<containerizer::Termination>());
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


// This function is executed by the forked child and must remain
// async-signal-safe.
int execute(
    const CommandInfo& command,
    const string& directory,
    const os::ExecEnv& envp,
    uid_t uid,
    gid_t gid,
    bool redirectIO,
    int pipeRead,
    int pipeWrite,
    const list<Option<CommandInfo> >& commands)
{
  if (close(pipeWrite) != 0) {
    ABORT("Failed to close pipe[1]");
  }

  // Do a blocking read on the pipe until the parent signals us to continue.
  char dummy;
  ssize_t length;
  while ((length = read(pipeRead, &dummy, sizeof(dummy))) == -1 &&
         errno == EINTR);

  if (length != sizeof(dummy)) {
    close(pipeRead);
    ABORT("Failed to synchronize with parent");
  }

  if (close(pipeRead) != 0) {
    ABORT("Failed to close pipe[0]");
  }

  // Change gid and uid.
  if (setgid(gid) != 0) {
    ABORT("Failed to set gid");
  }

  if (setuid(uid) != 0) {
    ABORT("Failed to set uid");
  }

  // Enter working directory.
  if (chdir(directory.c_str()) != 0) {
    ABORT("Failed to chdir into work directory");
  }

  // Redirect output to files in working dir if required. We append because
  // others (e.g., mesos-fetcher) may have already logged to the files.
  // TODO(bmahler): It would be best if instead of closing stderr /
  // stdout and redirecting, we instead always output to stderr /
  // stdout. Also tee'ing their output into the work directory files
  // when redirection is desired.
  if (redirectIO) {
    int fd;
    while ((fd = open(
        "stdout",
        O_CREAT | O_WRONLY | O_APPEND,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)) == -1 &&
            errno == EINTR);
    if (fd == -1) {
      ABORT("Failed to open stdout");
    }

    int status;
    while ((status = dup2(fd, STDOUT_FILENO)) == -1 && errno == EINTR);
    if (status == -1) {
      ABORT("Failed to dup2 for stdout");
    }

    if (close(fd) == -1) {
      ABORT("Failed to close stdout fd");
    }

    while ((fd = open(
        "stderr",
        O_CREAT | O_WRONLY | O_APPEND,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)) == -1 &&
            errno == EINTR);
    if (fd == -1) {
      ABORT("Failed to open stderr");
    }

    while ((status = dup2(fd, STDERR_FILENO)) == -1 && errno == EINTR);
    if (status == -1) {
      ABORT("Failed to dup2 for stderr");
    }

    if (close(fd) == -1) {
      ABORT("Failed to close stderr fd");
    }
  }

  // Run additional preparation commands. These are run as the same user and
  // with the environment as the slave.
  // NOTE: os::system() is async-signal-safe.
  foreach (const Option<CommandInfo>& command, commands) {
    if (command.isSome()) {
      // Block until the command completes.
      int status = os::system(command.get().value());

      if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
        ABORT("Command '",
              command.get().value().c_str(),
              "' failed to execute successfully");
      }
    }
  }

  // Execute the command (via '/bin/sh -c command') with its environment.
  execle("/bin/sh", "sh", "-c", command.value().c_str(), (char*) NULL, envp());

  // If we get here, the execv call failed.
  ABORT("Failed to execute command");

  // This should not be reached.
  return -1;
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

  // TODO(tillt): The slave should expose which containerization
  // mechanisms it supports to avoid scheduling tasks that it cannot
  // run.
  const CommandInfo& command = executorInfo.command();
  if (command.has_container()) {
    // We return a Failure as this containerizer does not support
    // handling ContainerInfo. Users have to be made aware of this
    // lack of support to prevent confusion in the task configuration.
    return Failure("ContainerInfo is not supported");
  }

  Owned<Promise<containerizer::Termination> > promise(
      new Promise<containerizer::Termination>());
  promises.put(containerId, promise);

  // Store the resources for usage().
  resources.put(containerId, executorInfo.resources());

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
                    &Self::destroy,
                    containerId));
}


Future<Nothing> MesosContainerizerProcess::launch(
    const ContainerID& containerId,
    const TaskInfo&,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint)
{
  return launch(
      containerId,
      executorInfo,
      directory,
      user,
      slaveId,
      slavePid,
      checkpoint);
}

Future<list<Option<CommandInfo> > > MesosContainerizerProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user)
{
  // Start preparing all isolators (in parallel) and gather any additional
  // preparation comands that must be run in the forked child before exec'ing
  // the executor.
  list<Future<Option<CommandInfo> > > futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->prepare(containerId, executorInfo));
  }

  // Wait for all isolators to complete preparations.
  return collect(futures);
}


Future<Nothing> _fetch(
    const ContainerID& containerId,
    const string& directory,
    const Option<string>& user,
    const Option<int>& status)
{
  if (status.isNone() || (status.get() != 0)) {
    return Failure("Failed to fetch URIs for container '" +
                   stringify(containerId) + "': exit status " +
                   (status.isNone() ? "none" : stringify(status.get())));
  }

  // Chown the work directory if a user is provided.
  if (user.isSome()) {
    Try<Nothing> chown = os::chown(user.get(), directory);
    if (chown.isError()) {
      return Failure("Failed to chown work directory");
    }
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

  map<string, string> environment =
    fetcherEnvironment(commandInfo, directory, user, flags);

  // Now the actual mesos-fetcher command.
  string command = realpath.get();

  LOG(INFO) << "Fetching URIs for container '" << containerId
            << "' using command '" << command << "'";

  Try<Subprocess> fetcher = subprocess(command, environment);
  if (fetcher.isError()) {
    return Failure("Failed to execute mesos-fetcher: " + fetcher.error());
  }

  // Redirect output (stdout and stderr) from the fetcher to log files
  // in the executor work directory, chown'ing them if a user is
  // specified.
  // TODO(tillt): Consider adding O_CLOEXEC for atomic close-on-exec.
  // TODO(tillt): Consider adding an overload to io::redirect
  // that accepts a file path as 'to' for further reducing code. We
  // would however also need an owner user parameter for such overload
  // to perfectly replace the below.
  Try<int> out = os::open(
      path::join(directory, "stdout"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (out.isError()) {
    return Failure("Failed to redirect stdout: " + out.error());
  }

  if (user.isSome()) {
    Try<Nothing> chown = os::chown(
        user.get(), path::join(directory, "stdout"));
    if (chown.isError()) {
      os::close(out.get());
      return Failure(
          "Failed to redirect stdout: Failed to chown: " +
          chown.error());
    }
  }

  // Redirect takes care of nonblocking and close-on-exec for the
  // supplied file descriptors.
  io::redirect(fetcher.get().out(), out.get());

  // Redirect does 'dup' the file descriptor, hence we can close the
  // original now.
  os::close(out.get());

  // Repeat for stderr.
  Try<int> err = os::open(
      path::join(directory, "stderr"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (err.isError()) {
    return Failure(
        "Failed to redirect stderr: Failed to open: " +
        err.error());
  }

  if (user.isSome()) {
    Try<Nothing> chown = os::chown(
        user.get(), path::join(directory, "stderr"));
    if (chown.isError()) {
      os::close(err.get());
      return Failure(
          "Failed to redirect stderr: Failed to chown: " +
          chown.error());
    }
  }

  io::redirect(fetcher.get().err(), err.get());

  os::close(err.get());

  return fetcher.get().status()
    .then(lambda::bind(&_fetch, containerId, directory, user, lambda::_1));
}


Future<Nothing> MesosContainerizerProcess::_launch(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user,
    const SlaveID& slaveId,
    const PID<Slave>& slavePid,
    bool checkpoint,
    const list<Option<CommandInfo> >& commands)
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

  // Construct a representation of the environment suitable for
  // passing to execle in the child. We construct it here because it
  // is not async-signal-safe.
  os::ExecEnv envp(env);

  // Use a pipe to block the child until it's been isolated.
  int pipes[2];
  // We assume this should not fail under reasonable conditions so we
  // use CHECK.
  CHECK(pipe(pipes) == 0);

  // Determine the uid and gid for the child now because getpwnam is
  // not async signal safe.
  Result<uid_t> uid = os::getuid(user);
  if (uid.isError() || uid.isNone()) {
    return Failure("Invalid user: " + (uid.isError() ? uid.error()
                                                     : "nonexistent"));
  }

  Result<gid_t> gid = os::getgid(user);
  if (gid.isError() || gid.isNone()) {
    return Failure("Invalid user: " + (gid.isError() ? gid.error()
                                                     : "nonexistent"));
  }


  // Prepare a function for the forked child to exec() the executor.
  lambda::function<int()> childFunction = lambda::bind(
      &execute,
      executorInfo.command(),
      directory,
      envp,
      uid.get(),
      gid.get(),
      !local,
      pipes[0],
      pipes[1],
      commands);

  Try<pid_t> forked = launcher->fork(containerId, childFunction);

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


Future<Nothing> MesosContainerizerProcess::isolate(
    const ContainerID& containerId,
    pid_t _pid)
{
  // Set up callbacks for isolator limitations.
  foreach (const Owned<Isolator>& isolator, isolators) {
    isolator->watch(containerId)
      .onAny(defer(self(), &Self::limited, containerId, lambda::_1));
  }

  // Isolate the executor with each isolator.
  list<Future<Nothing> > futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->isolate(containerId, _pid));
  }

  // Wait for all isolators to complete.
  return collect(futures)
    .then(lambda::bind(&_nothing));
}


Future<Nothing> MesosContainerizerProcess::exec(
    const ContainerID& containerId,
    int pipeWrite)
{
  CHECK(promises.contains(containerId));

  // Now that we've contained the child we can signal it to continue by
  // writing to the pipe.
  char dummy;
  ssize_t length;
  while ((length = write(pipeWrite, &dummy, sizeof(dummy))) == -1 &&
         errno == EINTR);

  if (length != sizeof(dummy)) {
    return Failure("Failed to synchronize child process: " +
                   string(strerror(errno)));
  }

  return Nothing();
}


Future<containerizer::Termination> MesosContainerizerProcess::wait(
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
  // The resources hashmap won't initially contain the container's resources
  // after recovery so we must check the promises hashmap (which is set during
  // recovery).
  if (!promises.contains(containerId)) {
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

    destroying.erase(containerId);

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
  // Now that all processes have exited we can now clean up all isolators.
  list<Future<Nothing> > futures;
  foreach (const Owned<Isolator>& isolator, isolators) {
    futures.push_back(isolator->cleanup(containerId));
  }

  // Wait for all isolators to complete cleanup before continuing.
  collect(futures)
    .onAny(defer(self(), &Self::___destroy, containerId, status, lambda::_1));
}


void MesosContainerizerProcess::___destroy(
    const ContainerID& containerId,
    const Future<Option<int > >& status,
    const Future<list<Nothing> >& futures)
{
  // Something has gone wrong with one of the Isolators and cleanup failed.
  // We'll fail the container termination and remove the 'destroying' flag but
  // leave all other state. The containerizer is now in a bad state because
  // at least one isolator has failed to clean up.
  if (!futures.isReady()) {
    promises[containerId]->fail(
        "Failed to clean up isolators when destroying container: " +
        (futures.isFailed() ? futures.failure() : "discarded future"));

    destroying.erase(containerId);

    return;
  }

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

  containerizer::Termination termination;
  termination.set_killed(killed);
  termination.set_message(message);
  if (status.isReady() && status.get().isSome()) {
    termination.set_status(status.get().get());
  }

  promises[containerId]->set(termination);

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


Future<hashset<ContainerID> > MesosContainerizerProcess::containers()
{
  return promises.keys();
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
