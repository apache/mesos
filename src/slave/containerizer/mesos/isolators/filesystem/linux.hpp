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

#ifndef __LINUX_FILESYSTEM_ISOLATOR_HPP__
#define __LINUX_FILESYSTEM_ISOLATOR_HPP__

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/owned.hpp>
#include <process/pid.hpp>

#include <process/metrics/gauge.hpp>

#include <stout/hashmap.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The filesystem isolator on Linux that is responsible for preparing
// the root filesystems and volumes (e.g., persistent volumes) for
// containers. It relies on Linux mount namespace to prevent mounts of
// a container from being propagated to the host mount table.
class LinuxFilesystemIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  virtual ~LinuxFilesystemIsolatorProcess();

  virtual bool supportsNesting();

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId);

private:
  LinuxFilesystemIsolatorProcess(const Flags& flags);

  Try<std::vector<CommandInfo>> getPreExecCommands(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  const Flags flags;

  struct Info
  {
    Info(const std::string& _directory) : directory(_directory) {}

    Info(const std::string& _directory,
         const Option<ExecutorInfo>& _executor)
      : directory(_directory),
        executor(_executor) {}

    const std::string directory;

    // Track resources so we can unmount unneeded persistent volumes.
    Resources resources;

    Option<ExecutorInfo> executor;
  };

  hashmap<ContainerID, process::Owned<Info>> infos;

  struct Metrics
  {
    explicit Metrics(
        const process::PID<LinuxFilesystemIsolatorProcess>& isolator);
    ~Metrics();

    process::metrics::Gauge containers_new_rootfs;
  } metrics;

  double _containers_new_rootfs();
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __LINUX_FILESYSTEM_ISOLATOR_HPP__
