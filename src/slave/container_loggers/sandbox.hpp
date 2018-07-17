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

#ifndef __SLAVE_CONTAINER_LOGGERS_SANDBOX_HPP__
#define __SLAVE_CONTAINER_LOGGERS_SANDBOX_HPP__

#include <map>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/containerizer.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/try.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class SandboxContainerLoggerProcess;


// The default container logger.
//
// Containers launched through this container logger will have their
// stdout and stderr piped to the files "stdout" and "stderr",
// respectively, in the sandbox. These logs are accessible via the
// agent's `/files` endpoint.
class SandboxContainerLogger : public mesos::slave::ContainerLogger
{
public:
  SandboxContainerLogger();
  ~SandboxContainerLogger() override;

  // This is a noop. The sandbox container logger has nothing to initialize.
  Try<Nothing> initialize() override;

  // Tells the subprocess to redirect the container's stdout and
  // stderr to separate "stdout" and "stderr" files in the sandbox.
  // The `path`, `argv`, and `environment` are not changed.
  process::Future<mesos::slave::ContainerIO> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

protected:
  process::Owned<SandboxContainerLoggerProcess> process;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONTAINER_LOGGERS_SANDBOX_HPP__
