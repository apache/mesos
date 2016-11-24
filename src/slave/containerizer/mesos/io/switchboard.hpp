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

#ifndef __MESOS_CONTAINERIZER_IO_SWITCHBOARD_HPP__
#define __MESOS_CONTAINERIZER_IO_SWITCHBOARD_HPP__

#include <string>

#include <mesos/slave/container_logger.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/flags.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

class IOSwitchboardProcess;

// The `IOSwitchboard` is designed to feed stdin to a container from
// an external source, as well as redirect the stdin/stdout of a
// container to multiple targets.
//
// The primary motivation of this component is to enable support in
// mesos similar to `docker attach` and `docker exec` whereby an
// external client can attach to the stdin/stdout/stderr of a running
// container as well as launch arbitrary subcommands inside a
// container and attach to its stdin/stdout/stderr.
class IOSwitchboard
{
public:
  struct SubprocessInfo
  {
    SubprocessInfo();
    process::Subprocess::IO in;
    process::Subprocess::IO out;
    process::Subprocess::IO err;
  };

  static Try<process::Owned<IOSwitchboard>> create(
      const Flags& flags,
      bool local);

  ~IOSwitchboard();

  process::Future<SubprocessInfo> prepare(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user);

private:
  explicit IOSwitchboard(
      const Flags& flags,
      bool local,
      const process::Owned<mesos::slave::ContainerLogger>& logger);

  process::Owned<IOSwitchboardProcess> process;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_IO_SWITCHBOARD_HPP__
