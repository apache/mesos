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

#ifndef __SLAVE_CONTAINER_LOGGER_LIB_LOGNOOP_HPP__
#define __SLAVE_CONTAINER_LOGGER_LIB_LOGNOOP_HPP__

#include <mesos/slave/container_logger.hpp>

namespace mesos {
namespace internal {
namespace logger {

// Forward declaration.
class LognoopContainerLoggerProcess;

// The `LognoopContainerLogger` is a container logger
// that does nothing more than
// redirecting stderr/stdout to /dev/null.
class LognoopContainerLogger : public mesos::slave::ContainerLogger
{
public:
  LognoopContainerLogger();

  virtual ~LognoopContainerLogger();

  // This is a noop.  The lognoop container logger has nothing to initialize.
  virtual Try<Nothing> initialize();

  virtual process::Future<Nothing> recover(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory);

  virtual process::Future<mesos::slave::ContainerLogger::SubprocessInfo>
  prepare(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory);

protected:
  process::Owned<LognoopContainerLoggerProcess> process;
};

} // namespace logger {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONTAINER_LOGGER_LIB_LOGNOOP_HPP__
