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


process::Future<Option<int>> run(
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
    return Failure("Could not fetch URIs: failed to find mesos-fetcher");
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
    return Failure("Failed to execute mesos-fetcher: " + fetcher.error());
  }

  return fetcher.get().status();
}

} // namespace fetcher {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
