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

#ifndef __TEST_LAUNCHER_HPP__
#define __TEST_LAUNCHER_HPP__

#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/flags.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "slave/containerizer/mesos/launcher.hpp"

namespace mesos {
namespace internal {
namespace tests {


class TestLauncher : public slave::Launcher
{
public:
  TestLauncher(const process::Owned<slave::Launcher>& _real);

  ~TestLauncher() override;

  MOCK_METHOD1(
      recover,
      process::Future<hashset<ContainerID>>(
          const std::vector<mesos::slave::ContainerState>& states));

  MOCK_METHOD9(
      fork,
      Try<pid_t>(
          const ContainerID& containerId,
          const std::string& path,
          const std::vector<std::string>& argv,
          const mesos::slave::ContainerIO& containerIO,
          const flags::FlagsBase* flags,
          const Option<std::map<std::string, std::string>>& env,
          const Option<int>& enterNamespaces,
          const Option<int>& cloneNamespaces,
          const std::vector<int_fd>& whitelistFds));

  MOCK_METHOD1(
      destroy,
      process::Future<Nothing>(const ContainerID& containerId));

  MOCK_METHOD1(
      status,
      process::Future<ContainerStatus>(const ContainerID& containerId));

  process::Owned<slave::Launcher> real;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_LAUNCHER_HPP__
