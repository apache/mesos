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

#include <mesos/module/container_logger.hpp>

#include <mesos/slave/container_logger.hpp>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "module/manager.hpp"

#include "slave/container_loggers/sandbox.hpp"

using std::string;

namespace mesos {
namespace slave {

Try<ContainerLogger*> ContainerLogger::create(const Option<string>& type)
{
  ContainerLogger* logger = nullptr;

  if (type.isNone()) {
    logger = new internal::slave::SandboxContainerLogger();
  } else {
    // Try to load container logger from module.
    Try<ContainerLogger*> module =
      modules::ModuleManager::create<ContainerLogger>(type.get());

    if (module.isError()) {
      return Error(
          "Failed to create container logger module '" + type.get() +
          "': " + module.error());
    }

    logger = module.get();
  }

  // Initialize the module.
  Try<Nothing> initialize = logger->initialize();
  if (initialize.isError()) {
    delete logger;

    return Error(
        "Failed to initialize container logger module: " + initialize.error());
  }

  return logger;
}

} // namespace slave {
} // namespace mesos {
