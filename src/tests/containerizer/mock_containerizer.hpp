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

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/slave/containerizer.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>

#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "slave/containerizer/containerizer.hpp"

namespace mesos {
namespace internal {
namespace tests {

class MockContainerizer : public slave::Containerizer
{
public:
  MOCK_METHOD1(
      recover,
      process::Future<Nothing>(
          const Option<slave::state::SlaveState>&));

  MOCK_METHOD8(
      launch,
      process::Future<bool>(
          const ContainerID&,
          const Option<TaskInfo>&,
          const ExecutorInfo&,
          const std::string&,
          const Option<std::string>&,
          const SlaveID&,
          const std::map<std::string, std::string>&,
          bool));

  MOCK_METHOD6(
      launch,
      process::Future<bool>(
          const ContainerID&,
          const CommandInfo&,
          const Option<ContainerInfo>&,
          const Option<std::string>&,
          const SlaveID&,
          const Option<mesos::slave::ContainerClass>&));

  MOCK_METHOD1(
      attach,
      process::Future<process::http::Connection>(const ContainerID&));

  MOCK_METHOD2(
      update,
      process::Future<Nothing>(
          const ContainerID&,
          const Resources&));

  MOCK_METHOD1(
      usage,
      process::Future<ResourceStatistics>(
          const ContainerID&));

  MOCK_METHOD1(
      wait,
      process::Future<Option<mesos::slave::ContainerTermination>>(
          const ContainerID&));

  MOCK_METHOD1(
      destroy,
      process::Future<bool>(const ContainerID&));

  MOCK_METHOD0(
      containers,
      process::Future<hashset<ContainerID>>());
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {
