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

#include <string>

#include <mesos/slave/container_logger.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/io/switchboard.hpp"

using mesos::slave::ContainerLogger;

using process::Future;
using process::Owned;
using process::Process;
using process::Subprocess;

using std::cerr;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

class IOSwitchboardProcess: public Process<IOSwitchboardProcess>
{
public:
  IOSwitchboardProcess(
    const Flags& flags,
    const Owned<ContainerLogger>& logger);

  Future<IOSwitchboard::SubprocessInfo> prepare(
      const ExecutorInfo& executorInfo,
      const string& sandboxDirectory,
      const Option<std::string>& user);

private:
  const Flags flags;
  Owned<mesos::slave::ContainerLogger> logger;
};


IOSwitchboard::SubprocessInfo::SubprocessInfo()
  : in(Subprocess::FD(STDIN_FILENO, Subprocess::IO::OWNED)),
    out(Subprocess::FD(STDOUT_FILENO, Subprocess::IO::OWNED)),
    err(Subprocess::FD(STDERR_FILENO, Subprocess::IO::OWNED)) {}


Try<Owned<IOSwitchboard>> IOSwitchboard::create(const Flags& flags)
{
  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  if (logger.isError()) {
    return Error("Cannot create container logger: " + logger.error());
  }

  return new IOSwitchboard(flags, Owned<ContainerLogger>(logger.get()));
}


IOSwitchboard::IOSwitchboard(
    const Flags& flags,
    const Owned<ContainerLogger>& logger)
  : process(new IOSwitchboardProcess(flags, logger))
{
  spawn(process.get());
}


IOSwitchboard::~IOSwitchboard()
{
  terminate(process.get());
  process::wait(process.get());
}


Future<IOSwitchboard::SubprocessInfo> IOSwitchboard::prepare(
    const ExecutorInfo& executorInfo,
    const std::string& sandboxDirectory,
    const Option<std::string>& user)
{
  return dispatch(
      process.get(),
      &IOSwitchboardProcess::prepare,
      executorInfo,
      sandboxDirectory,
      user);
}


IOSwitchboardProcess::IOSwitchboardProcess(
    const Flags& _flags,
    const Owned<ContainerLogger>& _logger)
  : flags(_flags),
    logger(_logger) {}


Future<IOSwitchboard::SubprocessInfo> IOSwitchboardProcess::prepare(
    const ExecutorInfo& executorInfo,
    const std::string& sandboxDirectory,
    const Option<std::string>& user)
{
  return logger->prepare(executorInfo, sandboxDirectory, user)
    .then(defer(self(), [](const ContainerLogger::SubprocessInfo& loggerInfo)
      -> Future<IOSwitchboard::SubprocessInfo> {
      IOSwitchboard::SubprocessInfo ioSwitchboardInfo;
      ioSwitchboardInfo.in = Subprocess::FD(STDIN_FILENO);
      ioSwitchboardInfo.out = loggerInfo.out;
      ioSwitchboardInfo.err = loggerInfo.err;

      return ioSwitchboardInfo;
    }));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
