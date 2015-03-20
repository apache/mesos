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

#ifndef __LAUNCHER_HPP__
#define __LAUNCHER_HPP__

#include <list>
#include <map>
#include <string>

#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/subprocess.hpp>

#include <stout/flags.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

class Launcher
{
public:
  virtual ~Launcher() {}

  // Recover the necessary state for each container listed in state.
  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ExecutorRunState>& states) = 0;

  // Fork a new process in the containerized context. The child will
  // exec the binary at the given path with the given argv, flags and
  // environment. The I/O of the child will be redirected according to
  // the specified I/O descriptors. The user can provide a 'setup'
  // function which will be invoked in the child process right before
  // the exec. The 'setup' function has to be async signal safe. The
  // parent will return the child's pid if the fork is successful.
  virtual Try<pid_t> fork(
      const ContainerID& containerId,
      const std::string& path,
      const std::vector<std::string>& argv,
      const process::Subprocess::IO& in,
      const process::Subprocess::IO& out,
      const process::Subprocess::IO& err,
      const Option<flags::FlagsBase>& flags,
      const Option<std::map<std::string, std::string>>& environment,
      const Option<lambda::function<int()>>& setup) = 0;

  // Kill all processes in the containerized context.
  virtual process::Future<Nothing> destroy(const ContainerID& containerId) = 0;
};


// Launcher suitable for any POSIX compliant system. Uses process
// groups and sessions to track processes in a container. POSIX states
// that process groups cannot migrate between sessions so all
// processes for a container will be contained in a session.
class PosixLauncher : public Launcher
{
public:
  static Try<Launcher*> create(const Flags& flags);

  virtual ~PosixLauncher() {}

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ExecutorRunState>& states);

  virtual Try<pid_t> fork(
      const ContainerID& containerId,
      const std::string& path,
      const std::vector<std::string>& argv,
      const process::Subprocess::IO& in,
      const process::Subprocess::IO& out,
      const process::Subprocess::IO& err,
      const Option<flags::FlagsBase>& flags,
      const Option<std::map<std::string, std::string>>& environment,
      const Option<lambda::function<int()>>& setup);

  virtual process::Future<Nothing> destroy(const ContainerID& containerId);

private:
  PosixLauncher() {}

  // The 'pid' is the process id of the first process and also the
  // process group id and session id.
  hashmap<ContainerID, pid_t> pids;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __LAUNCHER_HPP__
