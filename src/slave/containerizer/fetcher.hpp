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

#ifndef __SLAVE_FETCHER_HPP__
#define __SLAVE_FETCHER_HPP__

#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/hashmap.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace slave {

// Forward declaration.
class FetcherProcess;

// Argument passing to and invocation of the external fetcher program.
// TODO(bernd-mesos) : Orchestration and synchronization of fetching
// phases. Bookkeeping of executor files that are cached after
// downloading from a URI by the fetcher program. Cache eviction.
// There has to be exactly one fetcher with a distinct cache dir per
// active slave. This means that the cache dir can only be fixed
// after the slave ID has been determined by registration or recovery.
class Fetcher
{
public:
  // Builds the environment used to run mesos-fetcher. This
  // environment contains one variable with the name
  // "MESOS_FETCHER_INFO", and its value is a protobuf of type
  // mesos::fetcher::FetcherInfo.
  static std::map<std::string, std::string> environment(
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const Flags& flags);

  Fetcher();

  virtual ~Fetcher();

  // Download the URIs specified in the command info and place the
  // resulting files into the given work directory. Chmod said files
  // to the user if given.
  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const Flags& flags,
      const Option<int>& stdout,
      const Option<int>& stderr);

  // Same as above, but send stdout and stderr to the files 'stdout'
  // and 'stderr' in the specified directory.
  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const Flags& flags);

  // Best effort to kill the fetcher subprocess associated with the
  // indicated container. Do nothing if no such subprocess exists.
  void kill(const ContainerID& containerId);

private:
  process::Owned<FetcherProcess> process;
};


class FetcherProcess : public process::Process<FetcherProcess>
{
public:
  FetcherProcess() : ProcessBase("__fetcher__") {}

  virtual ~FetcherProcess();

  // Fetcher implementation.
  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const Flags& flags,
      const Option<int>& stdout,
      const Option<int>& stderr);

  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const Flags& flags);

  void kill(const ContainerID& containerId);

private:
  // Check status and return an error if any.
  process::Future<Nothing> _fetch(
      const ContainerID& containerId,
      const Option<int>& status);

  // Run the mesos-fetcher with custom output redirection. If
  // 'stdout' and 'stderr' file descriptors are provided then respective
  // output from the mesos-fetcher will be redirected to the file
  // descriptors. The file descriptors are duplicated (via dup) because
  // redirecting might still be occuring even after the mesos-fetcher has
  // terminated since there still might be data to be read.
  // This method is only "public" for test purposes.
  Try<process::Subprocess> run(
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const Flags& flags,
      const Option<int>& stdout,
      const Option<int>& stderr);

  // Run the mesos-fetcher, creating a "stdout" and "stderr" file
  // in the given directory and using these for output.
  Try<process::Subprocess> run(
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const Flags& flags);

  hashmap<ContainerID, pid_t> subprocessPids;
};

} // namespace slave {
} // namespace mesos {

#endif // __SLAVE_FETCHER_HPP__
