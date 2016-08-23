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

#include <mesos/mesos.hpp>

#include <mesos/module/container_logger.hpp>

#include <mesos/slave/container_logger.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/try.hpp>
#include <stout/nothing.hpp>

#include "slave/container_loggers/lib_lognoop.hpp"


using namespace mesos;
using namespace process;

using mesos::slave::ContainerLogger;

namespace mesos {
namespace internal {
namespace logger {

using SubprocessInfo = ContainerLogger::SubprocessInfo;


class LognoopContainerLoggerProcess :
  public Process<LognoopContainerLoggerProcess>
{
public:
  LognoopContainerLoggerProcess() {}

  Future<Nothing> recover(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory)
  {
    // No state to recover.
    return Nothing();
  }

  // just redirect stderr/stdout to /dev/null.
  Future<SubprocessInfo> prepare(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory)
  {
    ContainerLogger::SubprocessInfo info;

    info.out = SubprocessInfo::IO::PATH("/dev/null");
    info.err = SubprocessInfo::IO::PATH("/dev/null");

    return info;
  }
};


LognoopContainerLogger::LognoopContainerLogger()
  : process(new LognoopContainerLoggerProcess())
{
  spawn(process.get());
}


LognoopContainerLogger::~LognoopContainerLogger()
{
  terminate(process.get());
  wait(process.get());
}


Try<Nothing> LognoopContainerLogger::initialize()
{
  return Nothing();
}

Future<Nothing> LognoopContainerLogger::recover(
    const ExecutorInfo& executorInfo,
    const std::string& sandboxDirectory)
{
  return dispatch(
      process.get(),
      &LognoopContainerLoggerProcess::recover,
      executorInfo,
      sandboxDirectory);
}

Future<SubprocessInfo> LognoopContainerLogger::prepare(
    const ExecutorInfo& executorInfo,
    const std::string& sandboxDirectory)
{
  return dispatch(
      process.get(),
      &LognoopContainerLoggerProcess::prepare,
      executorInfo,
      sandboxDirectory);
}

} // namespace logger {
} // namespace internal {
} // namespace mesos {


mesos::modules::Module<ContainerLogger>
org_apache_mesos_LognoopContainerLogger(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Lognoop Container Logger module.",
    nullptr,
    [](const Parameters& parameters) -> ContainerLogger* {
      return new mesos::internal::logger::LognoopContainerLogger();
    });
