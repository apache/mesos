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

#ifndef __RESOURCE_PROVIDER_DAEMON_HPP__
#define __RESOURCE_PROVIDER_DAEMON_HPP__

#include <string>

#include <process/owned.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {

// Forward declarations.
class LocalResourceProviderDaemonProcess;


// A daemon in the agent monitoring in-process local resource providers.
// It monitors the config files in the config dir. Based on that, it
// starts or stops local resource providers.
class LocalResourceProviderDaemon
{
public:
  static Try<process::Owned<LocalResourceProviderDaemon>> create(
      const slave::Flags& flags);

  ~LocalResourceProviderDaemon();

  LocalResourceProviderDaemon(
      const LocalResourceProviderDaemon& other) = delete;

  LocalResourceProviderDaemon& operator=(
      const LocalResourceProviderDaemon& other) = delete;

private:
  LocalResourceProviderDaemon(
      const std::string& workDir,
      const Option<std::string>& configDir);

  process::Owned<LocalResourceProviderDaemonProcess> process;
};

} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_DAEMON_HPP__
