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

#include <mesos/fetcher/fetcher.hpp>

#include <process/dispatch.hpp>
#include <process/process.hpp>

#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

using std::map;
using std::string;
using std::vector;

using process::Future;

using mesos::fetcher::FetcherInfo;

namespace mesos {
namespace slave {


Fetcher::Fetcher() : process(new FetcherProcess())
{
  spawn(process.get());
}


Fetcher::~Fetcher()
{
  terminate(process.get());
  process::wait(process.get());
}


map<string, string> Fetcher::environment(
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags)
{
  FetcherInfo fetcherInfo;

  fetcherInfo.mutable_command_info()->CopyFrom(commandInfo);

  fetcherInfo.set_work_directory(directory);

  if (user.isSome()) {
    fetcherInfo.set_user(user.get());
  }

  if (!flags.frameworks_home.empty()) {
    fetcherInfo.set_frameworks_home(flags.frameworks_home);
  }

  if (!flags.hadoop_home.empty()) {
    fetcherInfo.set_hadoop_home(flags.hadoop_home);
  }

  map<string, string> result;
  result["MESOS_FETCHER_INFO"] = stringify(JSON::Protobuf(fetcherInfo));

  return result;
}


Future<Nothing> Fetcher::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags,
    const Option<int>& stdout,
    const Option<int>& stderr)
{
  if (commandInfo.uris().size() == 0) {
    return Nothing();
  }

  return dispatch(process.get(),
                  &FetcherProcess::fetch,
                  containerId,
                  commandInfo,
                  directory,
                  user,
                  flags,
                  stdout,
                  stderr);
}


Future<Nothing> Fetcher::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags)
{
  if (commandInfo.uris().size() == 0) {
    return Nothing();
  }

  return dispatch(process.get(),
                  &FetcherProcess::fetch,
                  containerId,
                  commandInfo,
                  directory,
                  user,
                  flags);
}


void Fetcher::kill(const ContainerID& containerId)
{
  dispatch(process.get(), &FetcherProcess::kill, containerId);
}


FetcherProcess::~FetcherProcess()
{
  foreach (const ContainerID& containerId, subprocessPids.keys()) {
    kill(containerId);
  }
}


Future<Nothing> FetcherProcess::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags,
    const Option<int>& stdout,
    const Option<int>& stderr)
{
  VLOG(1) << "Starting to fetch URIs for container: " << containerId
        << ", directory: " << directory;

  Try<Subprocess> subprocess =
    run(commandInfo, directory, user, flags, stdout, stderr);

  if (subprocess.isError()) {
    return Failure("Failed to execute mesos-fetcher: " + subprocess.error());
  }

  subprocessPids[containerId] = subprocess.get().pid();

  return subprocess.get().status()
    .then(defer(self(), &Self::_fetch, containerId, lambda::_1));
}


Future<Nothing> FetcherProcess::fetch(
    const ContainerID& containerId,
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags)
{
  VLOG(1) << "Starting to fetch URIs for container: " << containerId
        << ", directory: " << directory;

  Try<Subprocess> subprocess = run(commandInfo, directory, user, flags);

  if (subprocess.isError()) {
    return Failure("Failed to execute mesos-fetcher: " + subprocess.error());
  }

  subprocessPids[containerId] = subprocess.get().pid();

  return subprocess.get().status()
    .then(defer(self(), &Self::_fetch, containerId, lambda::_1));
}


Future<Nothing> FetcherProcess::_fetch(
    const ContainerID& containerId,
    const Option<int>& status)
{
  subprocessPids.erase(containerId);

  if (status.isNone()) {
    return Failure("No status available from fetcher");
  } else if (status.get() != 0) {
    return Failure("Failed to fetch URIs for container '" +
                   stringify(containerId) + "'with exit status: " +
                   stringify(status.get()));
  }

  return Nothing();
}


Try<Subprocess> FetcherProcess::run(
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags,
    const Option<int>& stdout,
    const Option<int>& stderr)
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
    return Error("Could not fetch URIs: failed to find mesos-fetcher");
  }

  // Now the actual mesos-fetcher command.
  string command = realpath.get();

  LOG(INFO) << "Fetching URIs using command '" << command << "'";

  Try<Subprocess> fetcherSubprocess = subprocess(
    command,
    Subprocess::PIPE(),
    stdout.isSome()
      ? Subprocess::FD(stdout.get())
      : Subprocess::PIPE(),
    stderr.isSome()
      ? Subprocess::FD(stderr.get())
      : Subprocess::PIPE(),
    Fetcher::environment(commandInfo, directory, user, flags));

  if (fetcherSubprocess.isError()) {
    return Error(
        "Failed to execute mesos-fetcher: " +  fetcherSubprocess.error());
  }

  return fetcherSubprocess;
}


Try<Subprocess> FetcherProcess::run(
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags)
{
  // Before we fetch let's make sure we create 'stdout' and 'stderr'
  // files into which we can redirect the output of the mesos-fetcher
  // (and later redirect the child's stdout/stderr).

  // TODO(tillt): Considering updating fetcher::run to take paths
  // instead of file descriptors and then use Subprocess::PATH()
  // instead of Subprocess::FD(). The reason this can't easily be done
  // today is because we not only need to open the files but also
  // chown them.
  Try<int> out = os::open(
      path::join(directory, "stdout"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (out.isError()) {
    return Error("Failed to create 'stdout' file: " + out.error());
  }

  // Repeat for stderr.
  Try<int> err = os::open(
      path::join(directory, "stderr"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (err.isError()) {
    os::close(out.get());
    return Error("Failed to create 'stderr' file: " + err.error());
  }

  if (user.isSome()) {
    Try<Nothing> chown = os::chown(user.get(), directory);
    if (chown.isError()) {
      os::close(out.get());
      os::close(err.get());
      return Error("Failed to chown work directory");
    }
  }

  Try<Subprocess> subprocess = run(
      commandInfo,
      directory,
      user,
      flags,
      out.get(),
      err.get());

  subprocess.get().status()
    .onAny(lambda::bind(&os::close, out.get()))
    .onAny(lambda::bind(&os::close, err.get()));

  return subprocess;
}


void FetcherProcess::kill(const ContainerID& containerId)
{
  if (subprocessPids.contains(containerId)) {
    VLOG(1) << "Killing the fetcher for container '" << containerId << "'";
    // Best effort kill the entire fetcher tree.
    os::killtree(subprocessPids.get(containerId).get(), SIGKILL);

    subprocessPids.erase(containerId);
  }
}

} // namespace slave {
} // namespace mesos {
