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

#include "tests/mock_fetcher.hpp"

using mesos::fetcher::FetcherInfo;

using process::Future;

using std::shared_ptr;
using std::string;

using testing::_;
using testing::Invoke;

namespace mesos {
namespace internal {
namespace tests {

MockFetcherProcess::MockFetcherProcess(const slave::Flags& flags)
  : slave::FetcherProcess(flags)
{
  // Set up default behaviors, calling the original methods.
  EXPECT_CALL(*this, _fetch(_, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockFetcherProcess::unmocked__fetch));
  EXPECT_CALL(*this, run(_, _, _, _))
    .WillRepeatedly(Invoke(this, &MockFetcherProcess::unmocked_run));
}


MockFetcherProcess::~MockFetcherProcess() {}


Future<Nothing> MockFetcherProcess::unmocked__fetch(
    const hashmap<CommandInfo::URI, Option<Future<shared_ptr<Cache::Entry>>>>&
      entries,
    const ContainerID& containerId,
    const string& sandboxDirectory,
    const string& cacheDirectory,
    const Option<string>& user)
{
  return slave::FetcherProcess::_fetch(
      entries,
      containerId,
      sandboxDirectory,
      cacheDirectory,
      user);
}


Future<Nothing> MockFetcherProcess::unmocked_run(
    const ContainerID& containerId,
    const string& sandboxDirectory,
    const Option<string>& user,
    const FetcherInfo& info)
{
  return slave::FetcherProcess::run(
      containerId,
      sandboxDirectory,
      user,
      info);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
