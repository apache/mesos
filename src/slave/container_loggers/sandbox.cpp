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

#include <map>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/containerizer.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>

#include "slave/container_loggers/sandbox.hpp"

using namespace process;

using mesos::slave::ContainerLogger;
using mesos::slave::ContainerIO;

namespace mesos {
namespace internal {
namespace slave {

class SandboxContainerLoggerProcess :
  public Process<SandboxContainerLoggerProcess>
{
public:
  SandboxContainerLoggerProcess()
    : ProcessBase(process::ID::generate("sandbox-logger")) {}

  process::Future<ContainerIO> prepare(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user)
  {
    ContainerIO io;

    io.out = ContainerIO::IO::PATH(path::join(sandboxDirectory, "stdout"));
    io.err = ContainerIO::IO::PATH(path::join(sandboxDirectory, "stderr"));

    return io;
  }
};


SandboxContainerLogger::SandboxContainerLogger()
  : process(new SandboxContainerLoggerProcess())
{
  spawn(process.get());
}


SandboxContainerLogger::~SandboxContainerLogger()
{
  terminate(process.get());
  wait(process.get());
}


Try<Nothing> SandboxContainerLogger::initialize()
{
  return Nothing();
}


Future<ContainerIO> SandboxContainerLogger::prepare(
    const ExecutorInfo& executorInfo,
    const std::string& sandboxDirectory,
    const Option<std::string>& user)
{
  return dispatch(
      process.get(),
      &SandboxContainerLoggerProcess::prepare,
      executorInfo,
      sandboxDirectory,
      user);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
