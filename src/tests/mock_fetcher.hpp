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

#ifndef __TESTS_MOCK_FETCHER_HPP__
#define __TESTS_MOCK_FETCHER_HPP__

#include <memory>
#include <string>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include <mesos/fetcher/fetcher.hpp>

#include <process/future.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>

#include "slave/slave.hpp"

#include "slave/containerizer/fetcher_process.hpp"

namespace mesos {
namespace internal {
namespace tests {

// Definition of a mock FetcherProcess to be used in tests with gmock.
class MockFetcherProcess : public slave::FetcherProcess
{
public:
  MockFetcherProcess(const slave::Flags& flags);
  ~MockFetcherProcess() override;

  MOCK_METHOD5(_fetch, process::Future<Nothing>(
      const hashmap<
          CommandInfo::URI,
          Option<process::Future<std::shared_ptr<Cache::Entry>>>>&
        entries,
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const std::string& cacheDirectory,
      const Option<std::string>& user));

  process::Future<Nothing> unmocked__fetch(
      const hashmap<
          CommandInfo::URI,
          Option<process::Future<std::shared_ptr<Cache::Entry>>>>&
        entries,
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const std::string& cacheDirectory,
      const Option<std::string>& user);

  MOCK_METHOD4(run, process::Future<Nothing>(
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const mesos::fetcher::FetcherInfo& info));

  process::Future<Nothing> unmocked_run(
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const mesos::fetcher::FetcherInfo& info);
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MOCK_FETCHER_HPP__
