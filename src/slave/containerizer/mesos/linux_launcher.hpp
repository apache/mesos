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

#ifndef __LINUX_LAUNCHER_HPP__
#define __LINUX_LAUNCHER_HPP__

#include <process/owned.hpp>

#include "slave/containerizer/mesos/launcher.hpp"

namespace mesos {
namespace internal {
namespace slave {

class LinuxLauncherProcess;

// Launcher for Linux systems with cgroups. Uses a freezer cgroup to
// track pids.
class LinuxLauncher : public Launcher
{
public:
  static Try<Launcher*> create(const Flags& flags);

  // Returns 'true' if prerequisites for using LinuxLauncher are available.
  static bool available();

  ~LinuxLauncher() override;

  process::Future<hashset<ContainerID>> recover(
      const std::vector<mesos::slave::ContainerState>& states) override;

  Try<pid_t> fork(
      const ContainerID& containerId,
      const std::string& path,
      const std::vector<std::string>& argv,
      const mesos::slave::ContainerIO& containerIO,
      const flags::FlagsBase* flags,
      const Option<std::map<std::string, std::string>>& environment,
      const Option<int>& enterNamespaces,
      const Option<int>& cloneNamespaces,
      const std::vector<int_fd>& whitelistFds) override;

  process::Future<Nothing> destroy(const ContainerID& containerId) override;

  process::Future<ContainerStatus> status(
      const ContainerID& containerId) override;

private:
  LinuxLauncher(
      const Flags& flags,
      const std::string& freezerHierarchy,
      const Option<std::string>& systemdHierarchy);

  process::Owned<LinuxLauncherProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __LINUX_LAUNCHER_HPP__
