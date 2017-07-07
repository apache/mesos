// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __SLAVE_CONTAINERIZER_FETCHER_HPP__
#define __SLAVE_CONTAINERIZER_FETCHER_HPP__

#include <list>
#include <string>

#include <mesos/mesos.hpp>

#include <mesos/fetcher/fetcher.hpp>

#include <process/future.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class FetcherProcess;

// Argument passing to and invocation of the external fetcher program.
// Bookkeeping of executor files that are cached after downloading from
// a URI by the fetcher program. Cache eviction. There has to be exactly
// one fetcher with a distinct cache directory per active slave. This
// means that the cache directory can only be fixed after the slave ID
// has been determined by registration or recovery. Downloads to cache
// files are separated on a per-user basis. The cache must only be used
// for URIs for which the expected download size can be determined and
// trusted before downloading. If there is any problem using the cache
// for any given URI, the fetch procedure automatically reverts to
// fetching directly into the sandbox directory.
class Fetcher
{
public:
  // Extracts the basename from a URI. For example, "d.txt" from
  // "htpp://1.2.3.4:5050/a/b/c/d.txt". The current implementation
  // only works for fairly regular-shaped URIs with a "/" and a proper
  // file name at the end.
  static Try<std::string> basename(const std::string& uri);

  // Some checks to make sure using the URI value in shell commands
  // is safe.
  // TODO(benh): These should be pushed into the scheduler driver and
  // reported to the user.
  static Try<Nothing> validateUri(const std::string& uri);

  // Checks to make sure the URI 'output_file' is valid.
  static Try<Nothing> validateOutputFile(const std::string& path);

  // Determines if the given URI refers to a local file system path
  // and prepends frameworksHome if it is a relative path. Fails if
  // frameworksHome is empty and a local path is indicated.
  static Result<std::string> uriToLocalPath(
      const std::string& uri,
      const Option<std::string>& frameworksHome);

  static bool isNetUri(const std::string& uri);

  Fetcher(const Flags& flags);

  // This is only public for tests.
  Fetcher(const process::Owned<FetcherProcess>& process);

  virtual ~Fetcher();

  // Download the URIs specified in the command info and place the
  // resulting files into the given sandbox directory. Chmod said files
  // to the user if given. Send stdout and stderr output to files
  // "stdout" and "stderr" in the given directory. Extract archives and/or
  // use the cache if so instructed by the given CommandInfo::URI items.
  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user);

  // Best effort to kill the fetcher subprocess associated with the
  // indicated container. Do nothing if no such subprocess exists.
  void kill(const ContainerID& containerId);

private:
  process::Owned<FetcherProcess> process;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONTAINERIZER_FETCHER_HPP__
