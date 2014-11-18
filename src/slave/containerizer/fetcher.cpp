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

#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace fetcher {

map<string, string> environment(
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags)
{
  map<string, string> result;

  result["MESOS_COMMAND_INFO"] = stringify(JSON::Protobuf(commandInfo));

  result["MESOS_WORK_DIRECTORY"] = directory;

  if (user.isSome()) {
    result["MESOS_USER"] = user.get();
  }

  if (!flags.frameworks_home.empty()) {
    result["MESOS_FRAMEWORKS_HOME"] = flags.frameworks_home;
  }

  if (!flags.hadoop_home.empty()) {
    result["HADOOP_HOME"] = flags.hadoop_home;
  }

  return result;
}


Try<Subprocess> run(
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

  Try<Subprocess> fetcher = subprocess(
    command,
    Subprocess::PIPE(),
    stdout.isSome()
      ? Subprocess::FD(stdout.get())
      : Subprocess::PIPE(),
    stderr.isSome()
      ? Subprocess::FD(stderr.get())
      : Subprocess::PIPE(),
    environment(commandInfo, directory, user, flags));

  if (fetcher.isError()) {
    return Error("Failed to execute mesos-fetcher: " + fetcher.error());
  }

  return fetcher;
}


Try<Subprocess> run(
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags)
{
  // Before we fetch let's make sure we create 'stdout' and 'stderr'
  // files into which we can redirect the output of the mesos-fetcher
  // (and later redirect the child's stdout/stderr).

  // TODO(tillt): Consider adding O_CLOEXEC for atomic close-on-exec.
  // TODO(tillt): Considering updating fetcher::run to take paths
  // instead of file descriptors and then use Subprocess::PATH()
  // instead of Subprocess::FD(). The reason this can't easily be done
  // today is because we not only need to open the files but also
  // chown them.
  Try<int> out = os::open(
      path::join(directory, "stdout"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (out.isError()) {
    return Error("Failed to create 'stdout' file: " + out.error());
  }

  // Repeat for stderr.
  Try<int> err = os::open(
      path::join(directory, "stderr"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

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

  Try<Subprocess> fetcher = fetcher::run(
      commandInfo,
      directory,
      user,
      flags,
      out.get(),
      err.get());

  fetcher.get().status()
    .onAny(lambda::bind(&os::close, out.get()))
    .onAny(lambda::bind(&os::close, err.get()));

  return fetcher;
}


Future<Nothing> _run(
    const ContainerID& containerId,
    const Option<int>& status)
{
  if (status.isNone()) {
    return Failure("No status available from fetcher");
  } else if (status.get() != 0) {
    return Failure("Failed to fetch URIs for container '" +
                   stringify(containerId) + "'with exit status: " +
                   stringify(status.get()));
  }

  return Nothing();
}


} // namespace fetcher {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
