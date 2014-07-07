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

#ifndef __LINUX_LAUNCHER_HPP__
#define __LINUX_LAUNCHER_HPP__

#include "slave/containerizer/launcher.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Launcher for Linux systems with cgroups. Uses a freezer cgroup to
// track pids.
class LinuxLauncher : public Launcher
{
public:
  static Try<Launcher*> create(const Flags& flags);

  virtual ~LinuxLauncher() {}

  virtual process::Future<Nothing> recover(
      const std::list<state::RunState>& states);

  virtual Try<pid_t> fork(
      const ContainerID& containerId,
      const std::string& path,
      const std::vector<std::string>& argv,
      const process::Subprocess::IO& in,
      const process::Subprocess::IO& out,
      const process::Subprocess::IO& err,
      const Option<flags::FlagsBase>& flags,
      const Option<std::map<std::string, std::string> >& environment,
      const Option<lambda::function<int()> >& setup);

  virtual process::Future<Nothing> destroy(const ContainerID& containerId);

private:
  LinuxLauncher(
      const Flags& flags,
      int namespaces,
      const std::string& hierarchy);

  static const std::string subsystem;
  const Flags flags;
  const int namespaces;
  const std::string hierarchy;

  std::string cgroup(const ContainerID& containerId);

  // The 'pid' is the process id of the child process and also the
  // process group id and session id.
  hashmap<ContainerID, pid_t> pids;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __LINUX_LAUNCHER_HPP__
