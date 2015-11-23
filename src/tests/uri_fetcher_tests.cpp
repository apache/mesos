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

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>
#include <stout/path.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/getcwd.hpp>

#include <stout/tests/utils.hpp>

#include <mesos/uri/fetcher.hpp>
#include <mesos/uri/uri.hpp>

#include "uri/schemes/http.hpp"

using namespace process;

using testing::_;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

class TestHttpServer : public Process<TestHttpServer>
{
public:
  TestHttpServer() : ProcessBase("TestHttpServer")
  {
    route("/test", None(), &TestHttpServer::test);
  }

  MOCK_METHOD1(test, Future<http::Response>(const http::Request&));
};


class CurlFetcherPluginTest : public TemporaryDirectoryTest
{
protected:
  virtual void SetUp()
  {
    TemporaryDirectoryTest::SetUp();

    spawn(server);
  }

  virtual void TearDown()
  {
    terminate(server);
    wait(server);

    TemporaryDirectoryTest::TearDown();
  }

  TestHttpServer server;
};


TEST_F(CurlFetcherPluginTest, CURL_ValidUri)
{
  URI uri = uri::http(
      stringify(server.self().address.ip),
      "/TestHttpServer/test",
      server.self().address.port);

  EXPECT_CALL(server, test(_))
    .WillOnce(Return(http::OK("test")));

  Try<Owned<uri::Fetcher>> fetcher = uri::Fetcher::create();
  ASSERT_SOME(fetcher);

  AWAIT_READY(fetcher.get()->fetch(uri, os::getcwd()));

  EXPECT_TRUE(os::exists(path::join(os::getcwd(), "test")));
}


TEST_F(CurlFetcherPluginTest, CURL_InvalidUri)
{
  URI uri = uri::http(
      stringify(server.self().address.ip),
      "/TestHttpServer/test",
      server.self().address.port);

  EXPECT_CALL(server, test(_))
    .WillOnce(Return(http::NotFound()));

  Try<Owned<uri::Fetcher>> fetcher = uri::Fetcher::create();
  ASSERT_SOME(fetcher);

  AWAIT_FAILED(fetcher.get()->fetch(uri, os::getcwd()));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
