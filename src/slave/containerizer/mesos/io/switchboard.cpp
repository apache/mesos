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

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>

#include "slave/containerizer/mesos/io/switchboard.hpp"

using std::string;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerIO;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLogger;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<IOSwitchboard*> IOSwitchboard::create(
    const Flags& flags,
    bool local)
{
  Try<ContainerLogger*> logger =
    ContainerLogger::create(flags.container_logger);

  if (logger.isError()) {
    return Error("Cannot create container logger: " + logger.error());
  }

  return new IOSwitchboard(
      flags,
      local,
      Owned<ContainerLogger>(logger.get()));
}


IOSwitchboard::IOSwitchboard(
    const Flags& _flags,
    bool _local,
    Owned<ContainerLogger> _logger)
  : flags(_flags),
    local(_local),
    logger(_logger) {}


IOSwitchboard::~IOSwitchboard() {}


bool IOSwitchboard::supportsNesting()
{
  return true;
}


Future<Option<ContainerLaunchInfo>> IOSwitchboard::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  // In local mode, the container will inherit agent's stdio.
  if (local) {
    return None();
  }

  // TODO(jieyu): Currently, if the agent fails over after the
  // executor is launched, but before its nested containers are
  // launched, the nested containers launched later might not have
  // access to the root parent container's ExecutorInfo (i.e.,
  // 'containerConfig.executor_info()' will be empty).
  return logger->prepare(
      containerConfig.executor_info(),
      containerConfig.directory(),
      containerConfig.has_user()
        ? Option<string>(containerConfig.user())
        : None())
    .then(defer(
        PID<IOSwitchboard>(this),
        &IOSwitchboard::_prepare,
        lambda::_1));
}


Future<Option<ContainerLaunchInfo>> IOSwitchboard::_prepare(
    const ContainerLogger::SubprocessInfo& loggerInfo)
{
  ContainerLaunchInfo launchInfo;

  ContainerIO* out = launchInfo.mutable_out();
  ContainerIO* err = launchInfo.mutable_err();

  switch (loggerInfo.out.type()) {
    case ContainerLogger::SubprocessInfo::IO::Type::FD:
      out->set_type(ContainerIO::FD);
      out->set_fd(loggerInfo.out.fd().get());
      break;
    case ContainerLogger::SubprocessInfo::IO::Type::PATH:
      out->set_type(ContainerIO::PATH);
      out->set_path(loggerInfo.out.path().get());
      break;
    default:
      UNREACHABLE();
  }

  switch (loggerInfo.err.type()) {
    case ContainerLogger::SubprocessInfo::IO::Type::FD:
      err->set_type(ContainerIO::FD);
      err->set_fd(loggerInfo.err.fd().get());
      break;
    case ContainerLogger::SubprocessInfo::IO::Type::PATH:
      err->set_type(ContainerIO::PATH);
      err->set_path(loggerInfo.err.path().get());
      break;
    default:
      UNREACHABLE();
  }

  return launchInfo;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
