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

#ifndef __TESTS_DISK_PROFILE_SERVER_HPP__
#define __TESTS_DISK_PROFILE_SERVER_HPP__

#include <memory>

#include <gmock/gmock.h>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/none.hpp>

namespace mesos {
namespace internal {
namespace tests {

// A simple helper to provide a mock HTTP endpoint for `UriDiskProfileAdaptor`
// to fetch disk profile mappings from.
class TestDiskProfileServerProcess
  : public process::Process<TestDiskProfileServerProcess>
{
public:
  TestDiskProfileServerProcess()
    : process::ProcessBase("test-disk-profile-server") {}

  inline process::http::URL url()
  {
    return process::http::URL(
        "http",
        process::address().ip,
        process::address().port,
        self().id + "/profiles");
  }

  MOCK_METHOD1(
      profiles,
      process::Future<process::http::Response>(const process::http::Request&));

private:
  void initialize() override
  {
    route("/profiles", None(), &Self::profiles);
  }
};


class TestDiskProfileServer
{
public:
  static inline process::Future<std::shared_ptr<TestDiskProfileServer>> create()
  {
    // TODO(chhsiao): Make `server` a `unique_ptr` and move it into the
    // following lambda once we get C++14.
    std::shared_ptr<TestDiskProfileServer>  server(new TestDiskProfileServer());

    // Wait for the process to finish initializing so that the routes are ready.
    return process::dispatch(
        server->process->self(),
        [=]() -> std::shared_ptr<TestDiskProfileServer> { return server; });
  }

  ~TestDiskProfileServer()
  {
    process::terminate(process.get());
    process::wait(process.get());
  }

  std::unique_ptr<TestDiskProfileServerProcess> process;

private:
  TestDiskProfileServer() : process(new TestDiskProfileServerProcess())
  {
    process::spawn(process.get());
  }
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_DISK_PROFILE_SERVER_HPP__
